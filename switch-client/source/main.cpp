#include "app.h"
#include "ui.h"
#include "menu.h"
#include "net.h"
#include "decoder.h"
#include "presenter.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>

SDL_Renderer* g_ren = nullptr;
Settings g_cfg;

static SDL_Window* g_win = nullptr;
static SDL_Surface* g_uiSurface = nullptr;
static bool g_directRenderer = false;
static bool g_presentUi = true;
static SDL_Rect g_uiRegion{0, 0, 1280, 720};
static bool g_presentFailed = false;
static SDL_GameController* g_pad = nullptr;

static const char* CFG_PATH = "/switch/lumiri.cfg";

void Settings::load() {
    FILE* f = fopen(CFG_PATH, "r");
    if (!f) f = fopen("/switch/remoteplay.cfg", "r");
    if (!f) return;
    char k[32]; int v;
    while (fscanf(f, "%31[^=]=%d\n", k, &v) == 2) {
        if (!strcmp(k, "res")) resIdx = v < 0 ? 0 : v > 3 ? 3 : v;
        else if (!strcmp(k, "fps")) fps = (v == 0 || v == 30 || v == 60 || v == 120) ? v : 60;
        else if (!strcmp(k, "mbps")) bitrateMbps = v < 2 ? 2 : v > 35 ? 35 : v;
        else if (!strcmp(k, "audio")) audio = v != 0;
        else if (!strcmp(k, "perf")) maxPerformance = v != 0;
        else if (!strcmp(k, "hwdec")) hwDecode = v != 0;
        else if (!strcmp(k, "directmode")) directVideo = v != 0;
        else if (!strcmp(k, "sens")) mouseSens = v < 1 ? 1 : v > 10 ? 10 : v;
        else if (!strcmp(k, "clk")) clocks = v != 0;
        else if (!strcmp(k, "cpumax")) cpuMax = v < 0 ? -1 : v > 3 ? 3 : v;
        else if (!strcmp(k, "gpumax")) gpuMax = v < 0 ? -1 : v > 3 ? 3 : v;
        else if (!strcmp(k, "rammhz")) ramMHz = v == 1600 ? 1600 : 2133;
        else if (!strcmp(k, "darkmode")) darkMode = v != 0;
        else if (!strcmp(k, "language")) english = v != 1;
    }
    fclose(f);
}

void Settings::save() const {
    FILE* f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "res=%d\nfps=%d\nmbps=%d\naudio=%d\nperf=%d\nhwdec=%d\nsens=%d\nclk=%d\ncpumax=%d\ngpumax=%d\ndirectmode=%d\nrammhz=%d\n",
            resIdx, fps, bitrateMbps, audio ? 1 : 0,
            maxPerformance ? 1 : 0, hwDecode ? 1 : 0, mouseSens, clocks ? 1 : 0,
            cpuMax, gpuMax, directVideo ? 1 : 0, ramMHz);
    fprintf(f, "language=%d\ndarkmode=%d\n", english ? 0 : 1, darkMode ? 1 : 0);
    fclose(f);
}

static void ensurePad() {
    if (g_pad && SDL_GameControllerGetAttached(g_pad)) return;
    if (g_pad) { SDL_GameControllerClose(g_pad); g_pad = nullptr; }
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
}

static bool swkbdText(const char* guide, const char* initial, char* out, size_t outLen) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide);
    if (initial && initial[0]) swkbdConfigSetInitialText(&kbd, initial);
    Result rc = swkbdShow(&kbd, out, outLen);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0];
}

static uint32_t readButtons(bool xboxLayout = false) {
    if (!g_pad) return 0;
    uint32_t b = 0;
    auto btn = [&](SDL_GameControllerButton s, uint32_t r) {
        if (SDL_GameControllerGetButton(g_pad, s)) b |= r; };
    btn(SDL_CONTROLLER_BUTTON_B, xboxLayout ? RP_BTN_B : RP_BTN_A);
    btn(SDL_CONTROLLER_BUTTON_A, xboxLayout ? RP_BTN_A : RP_BTN_B);
    btn(SDL_CONTROLLER_BUTTON_Y, xboxLayout ? RP_BTN_Y : RP_BTN_X);
    btn(SDL_CONTROLLER_BUTTON_X, xboxLayout ? RP_BTN_X : RP_BTN_Y);
    btn(SDL_CONTROLLER_BUTTON_LEFTSTICK, RP_BTN_LSTICK);
    btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK, RP_BTN_RSTICK);
    btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, RP_BTN_L);
    btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, RP_BTN_R);
    btn(SDL_CONTROLLER_BUTTON_START, RP_BTN_PLUS);
    btn(SDL_CONTROLLER_BUTTON_BACK, RP_BTN_MINUS);
    btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, RP_BTN_DLEFT);
    btn(SDL_CONTROLLER_BUTTON_DPAD_UP, RP_BTN_DUP);
    btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RP_BTN_DRIGHT);
    btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, RP_BTN_DDOWN);
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= RP_BTN_ZL;
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= RP_BTN_ZR;
    return b;
}

static int16_t axisVal(SDL_GameControllerAxis a, bool invert) {
    if (!g_pad) return 0;
    int v = SDL_GameControllerGetAxis(g_pad, a);
    if (invert) v = -v;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

struct EdgeDetect {
    uint32_t prev = 0;
    uint32_t pressed(uint32_t cur) { uint32_t p = cur & ~prev; prev = cur; return p; }
};

enum Screen { SCR_MAIN, SCR_SETTINGS, SCR_STREAM, SCR_ERROR, SCR_BROWSE };
static Screen g_screen = SCR_MAIN;
static char g_errMsg[256] = "";
static void presentFrame() {
    SDL_RenderPresent(g_ren);
    if (g_directRenderer && !presenterPresent(g_uiSurface,
        g_screen != SCR_STREAM || g_presentUi, g_screen == SCR_STREAM,
        g_screen == SCR_STREAM ? &g_uiRegion : nullptr)) g_presentFailed = true;
}

static std::vector<HostInfo> g_hosts;
static int g_selHost = 0;

static std::vector<GameItem> g_pendingGames;
static bool g_pendingReady = false;
static std::mutex g_gameMtx;
static std::vector<GameItem> g_gamesUI;
static std::vector<SDL_Texture*> g_gameTex;
static std::atomic<bool> g_gamesLoading{false};
static char g_gamesIp[16] = "";
static int g_gridSel = 0, g_gridTop = 0;

static std::thread g_fetchThread;

static void startGamesFetch(const char* ip) {
    if (g_gamesLoading.exchange(true)) return;

    if (g_fetchThread.joinable()) g_fetchThread.join();
    std::string ips(ip);
    g_fetchThread = std::thread([ips] {
        std::vector<GameItem> out;
        bool ok = fetchGames(ips.c_str(), out);
        if (ok) {
            std::lock_guard<std::mutex> lk(g_gameMtx);
            g_pendingGames = std::move(out);
            g_pendingReady = true;
        }
        g_gamesLoading = false;
    });
}

static void fetchThreadReap() {
    if (!g_gamesLoading && g_fetchThread.joinable()) g_fetchThread.join();
    std::lock_guard<std::mutex> lk(g_gameMtx);
    if (g_pendingReady) {
        for (auto t : g_gameTex) if (t) SDL_DestroyTexture(t);
        g_gamesUI = std::move(g_pendingGames);
        g_gameTex.assign(g_gamesUI.size(), nullptr);
        g_pendingGames.clear();
        g_pendingReady = false;
    }
}

static void clearGamesUI() {
    for (auto t : g_gameTex) if (t) SDL_DestroyTexture(t);
    g_gameTex.clear();
    g_gamesUI.clear();
    std::lock_guard<std::mutex> lk(g_gameMtx);
    g_pendingGames.clear();
    g_pendingReady = false;
}

static SDL_Texture* g_videoTex = nullptr;
static int g_texW = 0, g_texH = 0;
static DecodedPixelFormat g_texFormat = DECODED_YUV420P;
static std::atomic<bool> g_overlay{false};
static int g_overlaySel = 0;
static bool g_showStats = false;
static bool g_streamSettings = false;
static Settings g_settingsDraft;
static char g_connectedIp[16] = "";
static std::atomic<uint32_t> g_inputSeq{1};
static char g_audioErr[128] = "";

static std::atomic<int> g_accDxFx{0}, g_accDyFx{0}, g_wheelFx{0};
static std::atomic<uint64_t> g_clickUntil{0}, g_rclickUntil{0};
static int g_fingers = 0;
static uint64_t g_touchDownAt = 0;
static bool g_touchMoved = false;

static std::atomic<uint8_t> g_pendingKey{0};
static std::mutex g_textMtx;
static std::string g_pendingText;

static uint32_t g_lastFrames = 0;
static uint64_t g_lastBytes = 0, g_lastStatAt = 0, g_lastPingAt = 0;
static float g_dispFps = 0, g_dispMbps = 0;

static const uint32_t CPU_HZ[] = { 1020000000, 1224000000, 1581000000, 1785000000 };
static const uint32_t GPU_HZ[] = { 384000000, 460800000, 768000000, 921600000 };
static ClkrstSession g_cpuClk, g_gpuClk, g_emcClk;
static bool g_clkOpen = false;
static bool g_emcOpen = false;
static int g_cpuLv = 0, g_gpuLv = 0;
static uint32_t g_prevCpuHz = 0, g_prevGpuHz = 0, g_prevEmcHz = 0;
static uint32_t g_cpuHz = 0, g_gpuHz = 0;
static uint32_t g_emcHz = 0;

static void clocksRead() {
    g_cpuHz = g_gpuHz = g_emcHz = 0;
    if (!g_clkOpen) return;
    uint32_t hz = 0;
    if (R_SUCCEEDED(clkrstGetClockRate(&g_cpuClk, &hz))) g_cpuHz = hz;
    if (R_SUCCEEDED(clkrstGetClockRate(&g_gpuClk, &hz))) g_gpuHz = hz;
    if (g_emcOpen && R_SUCCEEDED(clkrstGetClockRate(&g_emcClk, &hz))) g_emcHz = hz;
}

static const char* clockText(uint32_t hz, char (&text)[16]) {
    if (hz) snprintf(text, sizeof(text), "%u", hz / 1000000);
    else snprintf(text, sizeof(text), "--");
    return text;
}

static int cpuCap() {
    if (g_cfg.cpuMax >= 0) return g_cfg.cpuMax > 3 ? 3 : g_cfg.cpuMax;
    return 3;
}
static int gpuCap() {
    if (g_cfg.gpuMax >= 0) return g_cfg.gpuMax > 3 ? 3 : g_cfg.gpuMax;
    return 3;
}

static void setCpuLv(int lv) {
    if (!g_clkOpen || lv < 0 || lv > 3) return;
    if (lv > cpuCap()) lv = cpuCap();
    if (R_SUCCEEDED(clkrstSetClockRate(&g_cpuClk, CPU_HZ[lv]))) g_cpuLv = lv;
}
static void setGpuLv(int lv) {
    if (!g_clkOpen || lv < 0 || lv > 3) return;
    if (lv > gpuCap()) lv = gpuCap();
    if (R_SUCCEEDED(clkrstSetClockRate(&g_gpuClk, GPU_HZ[lv]))) g_gpuLv = lv;
}

static AppletOperationMode g_clkMode = (AppletOperationMode)-1;
static void setRamClock() {
    if (!g_emcOpen) return;
    uint32_t rates[64]{};
    int32_t count = 0;
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    const Result query = clkrstGetPossibleClockRates(&g_emcClk, rates, 64, &type, &count);
    const uint32_t ceiling = uint32_t(g_cfg.ramMHz) * 1000000u;
    uint32_t selected = 0;
    if (R_SUCCEEDED(query) && type == PcvClockRatesListType_Discrete && count > 0 && count <= 64) {
        for (int i = 0; i < count; ++i)
            if (rates[i] <= ceiling && rates[i] > selected) selected = rates[i];
    }

    Result applied = 0;
    if (selected) applied = clkrstSetClockRate(&g_emcClk, selected);
    FILE* f = fopen("/switch/lumiri-clocks.log", "a");
    if (f) {
        fprintf(f, "RAM target=%u selected=%u query=0x%08X type=%d count=%d set=0x%08X\n",
                ceiling, selected, query, int(type), int(count), applied);
        fclose(f);
    }
}
static void clocksApply() {
    if (!g_clkOpen) return;
    AppletOperationMode m = appletGetOperationMode();
    if (m == g_clkMode) return;
    g_clkMode = m;
    setCpuLv(g_cfg.maxPerformance ? 3 : 2);
    setGpuLv(g_cfg.maxPerformance ? 3 : 1);
    setRamClock();
}

static void clocksOpen() {

    if (!g_cfg.clocks) return;
    if (g_clkOpen || R_FAILED(clkrstInitialize())) return;
    if (R_FAILED(clkrstOpenSession(&g_cpuClk, PcvModuleId_CpuBus, 3))) { clkrstExit(); return; }
    if (R_FAILED(clkrstOpenSession(&g_gpuClk, PcvModuleId_GPU, 3))) {
        clkrstCloseSession(&g_cpuClk);
        clkrstExit();
        return;
    }
    g_clkOpen = true;
    g_prevCpuHz = g_prevGpuHz = g_prevEmcHz = 0;
    clkrstGetClockRate(&g_cpuClk, &g_prevCpuHz);
    clkrstGetClockRate(&g_gpuClk, &g_prevGpuHz);
    if (R_SUCCEEDED(clkrstOpenSession(&g_emcClk, PcvModuleId_EMC, 3))) {
        g_emcOpen = true;
        clkrstGetClockRate(&g_emcClk, &g_prevEmcHz);
    }
    clocksApply();
    clocksRead();
}

static void clocksClose() {
    if (!g_clkOpen) return;
    if (g_prevCpuHz) clkrstSetClockRate(&g_cpuClk, g_prevCpuHz);
    if (g_prevGpuHz) clkrstSetClockRate(&g_gpuClk, g_prevGpuHz);
    if (g_emcOpen) {
        if (g_prevEmcHz) clkrstSetClockRate(&g_emcClk, g_prevEmcHz);
        clkrstCloseSession(&g_emcClk);
        g_emcOpen = false;
    }
    clkrstCloseSession(&g_cpuClk);
    clkrstCloseSession(&g_gpuClk);
    clkrstExit();
    g_clkOpen = false;
    g_clkMode = (AppletOperationMode)-1;
    g_cpuHz = g_gpuHz = g_emcHz = 0;
}

static std::thread g_inputThread;
static std::atomic<bool> g_inputRun{false};

static void inputLoop() {

    int carryX = 0, carryY = 0, carryW = 0;
    while (g_inputRun) {
        RpInputPkt pkt{};
        pkt.magic = RP_MAGIC;
        pkt.seq = g_inputSeq++;
        if (!g_overlay) {
            pkt.buttons = readButtons(true);
            pkt.lx = axisVal(SDL_CONTROLLER_AXIS_LEFTX, false);
            pkt.ly = axisVal(SDL_CONTROLLER_AXIS_LEFTY, true);
            pkt.rx = axisVal(SDL_CONTROLLER_AXIS_RIGHTX, false);
            pkt.ry = axisVal(SDL_CONTROLLER_AXIS_RIGHTY, true);
        }
        carryX += g_accDxFx.exchange(0);
        carryY += g_accDyFx.exchange(0);
        carryW += g_wheelFx.exchange(0);
        pkt.mouseDx = (int16_t)(carryX / 16); carryX -= pkt.mouseDx * 16;
        pkt.mouseDy = (int16_t)(carryY / 16); carryY -= pkt.mouseDy * 16;
        int w = carryW / 16;
        if (w) {
            pkt.wheel = (int8_t)(w > 127 ? 127 : w < -128 ? -128 : w);
            carryW -= pkt.wheel * 16;
        }
        uint64_t now = nowMs();
        if (now < g_clickUntil) pkt.mouseButtons |= 1;
        if (now < g_rclickUntil) pkt.mouseButtons |= 2;
        pkt.specialKey = g_pendingKey.exchange(0);
        char text[400];
        {
            std::lock_guard<std::mutex> lk(g_textMtx);
            if (!g_pendingText.empty()) {
                size_t l = g_pendingText.size() < sizeof(text) ? g_pendingText.size() : sizeof(text);
                memcpy(text, g_pendingText.data(), l);
                pkt.textLen = (uint16_t)l;
                g_pendingText.clear();
            }
        }
        sendInput(pkt, pkt.textLen ? text : nullptr);
        svcSleepThread(2000000);
    }
}

static void streamEnter() {
    clocksOpen();
    g_overlay = false; g_overlaySel = 0;
    g_accDxFx = 0; g_accDyFx = 0; g_wheelFx = 0;
    g_clickUntil = 0; g_rclickUntil = 0;
    g_fingers = 0;
    g_pendingKey = 0;
    { std::lock_guard<std::mutex> lk(g_textMtx); g_pendingText.clear(); }
    g_lastFrames = 0; g_lastBytes = 0; g_lastStatAt = nowMs();
    g_dispFps = g_dispMbps = 0;
    g_inputRun = true;
    g_inputThread = std::thread(inputLoop);
}

static void streamLeave() {
    g_streamSettings = false;
    g_english = g_cfg.english; theme::apply(g_cfg.darkMode);
    g_inputRun = false;
    if (g_inputThread.joinable()) g_inputThread.join();
    clocksClose();
    sessionDisconnect();
    if (g_directRenderer) presenterClearVideo();
    decoderExit();
    if (g_audioDev) { SDL_CloseAudioDevice(g_audioDev); g_audioDev = 0; }
    if (g_videoTex) { SDL_DestroyTexture(g_videoTex); g_videoTex = nullptr; g_texW = g_texH = 0; }
    discoveryStart();
    g_screen = SCR_MAIN;
}

static void connectTo(const char* ip, uint16_t launchGame = RP_GAME_NONE) {

    drawConnectionState(tr("Bağlanıyor…", "Connecting…"), ip, false);
    presentFrame();

    RpHandshakeAck ack{};
    if (!sessionConnect(ip, ack, g_errMsg, sizeof(g_errMsg), launchGame)) {
        g_screen = SCR_ERROR;
        return;
    }
    if (!decoderInit(g_cfg.width(), g_cfg.height(), g_cfg.hwDecode, g_directRenderer)) {
        snprintf(g_errMsg, sizeof(g_errMsg), tr("H264 çözücü başlatılamadı", "Could not initialize H264 decoder"));
        sessionDisconnect();
        g_screen = SCR_ERROR;
        return;
    }
    g_audioErr[0] = 0;
    if (g_cfg.audio && ack.audioRate) {
        SDL_AudioSpec want{}, have{};
        want.freq = (int)ack.audioRate;
        want.format = AUDIO_S16LSB;
        want.channels = 2;
        want.samples = 1024;

        g_audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);

        if (!g_audioDev)
            snprintf(g_audioErr, sizeof(g_audioErr), tr("Ses aygıtı açılamadı: %s", "Could not open audio device: %s"), SDL_GetError());
    }
    snprintf(g_connectedIp, sizeof(g_connectedIp), "%s", ip);
    sessionStart();
    discoveryStop();
    streamEnter();
    g_screen = SCR_STREAM;
}

static char g_brPath[500] = "";
static std::vector<RpBrowseEntry> g_brEntries;
static int g_brSel = 0, g_brTop = 0;
static bool g_brFailed = false;

static void browseLoad() {
    drawConnectionState(tr("Klasör açılıyor…", "Opening folder…"), g_brPath, false);
    presentFrame();
    g_brFailed = !browsePath(g_gamesIp, g_brPath, g_brEntries);
    g_brSel = 0; g_brTop = 0;
}

static void refreshGameList() {
    clearGamesUI();
    startGamesFetch(g_gamesIp);
}

static void drawBrowse(uint32_t cur, uint32_t pressed) {

    static uint64_t holdAt = 0, lastRep = 0;
    uint64_t now = nowMs();
    uint32_t held = cur & (RP_BTN_DUP | RP_BTN_DDOWN);
    if (held) {
        if (!holdAt) holdAt = now;
        else if (now - holdAt > 400 && now - lastRep > 70) { pressed |= held; lastRep = now; }
    } else holdAt = 0;

    int n = (int)g_brEntries.size();
    if (pressed & RP_BTN_DDOWN && g_brSel + 1 < n) g_brSel++;
    if (pressed & RP_BTN_DUP && g_brSel > 0) g_brSel--;
    if (pressed & RP_BTN_B) {
        char* sl = strrchr(g_brPath, '\\');
        if (sl) *sl = 0;
        else if (g_brPath[0]) g_brPath[0] = 0;
        else { g_screen = SCR_MAIN; return; }
        browseLoad();
        return;
    }
    if ((pressed & RP_BTN_A) && g_brSel < n) {
        RpBrowseEntry& e = g_brEntries[g_brSel];
        char full[500];
        snprintf(full, sizeof(full), "%s%s%s", g_brPath, g_brPath[0] ? "\\" : "", e.name);
        if (e.type == RP_FS_DIR) {
            snprintf(g_brPath, sizeof(g_brPath), "%s", full);
            browseLoad();
        } else {
            char def[64];
            snprintf(def, sizeof(def), "%.*s",
                     (int)(strlen(e.name) > 4 ? strlen(e.name) - 4 : strlen(e.name)), e.name);
            char name[64] = "";
            if (swkbdText(tr("Oyunun menüde görünecek adı", "Game name shown in the menu"), def, name, sizeof(name))) {
                if (addCustomGame(g_gamesIp, name, full)) {
                    refreshGameList();
                    g_screen = SCR_MAIN;
                    return;
                }
                g_brFailed = true;
            }
        }
        return;
    }

    if (g_brSel < g_brTop) g_brTop = g_brSel;
    if (g_brSel >= g_brTop + 6) g_brTop = g_brSel - 5;
    drawFolder(g_brEntries, g_brSel, g_brTop, g_brPath, g_brFailed);
}

static void drawMain(uint32_t pressed) {
    fetchThreadReap();
    discoveryTick(g_hosts);
    if (g_selHost >= (int)g_hosts.size()) g_selHost = g_hosts.empty() ? 0 : (int)g_hosts.size() - 1;

    if (!g_hosts.empty() && strcmp(g_gamesIp, g_hosts[g_selHost].ip) != 0 && !g_gamesLoading) {
        strncpy(g_gamesIp, g_hosts[g_selHost].ip, 15);
        clearGamesUI();
        startGamesFetch(g_gamesIp);
    }

    std::vector<GameItem>& games = g_gamesUI;
    const int tiles = 2 + (int)games.size();
    const int COLS = 5;
    if (g_gridSel >= tiles) g_gridSel = tiles - 1;

    if (pressed & RP_BTN_X) {
        char ip[64] = "";
        if (swkbdText(tr("PC'nin IP adresi (örn. 192.168.1.20)", "PC IP address (e.g. 192.168.1.20)"), nullptr, ip, sizeof(ip))) connectTo(ip);
        return;
    }
    if (pressed & RP_BTN_Y) { g_screen = SCR_SETTINGS; return; }
    if ((pressed & RP_BTN_MINUS) && !g_hosts.empty() && !g_gamesLoading)
        refreshGameList();
    if ((pressed & (RP_BTN_L | RP_BTN_R)) && g_hosts.size() > 1) {
        g_selHost = (g_selHost + ((pressed & RP_BTN_R) ? 1 : (int)g_hosts.size() - 1)) % (int)g_hosts.size();
        g_gridSel = 0; g_gridTop = 0;
    }
    if (pressed & RP_BTN_DRIGHT && g_gridSel + 1 < tiles) g_gridSel++;
    if (pressed & RP_BTN_DLEFT && g_gridSel > 0) g_gridSel--;
    if ((pressed & RP_BTN_DDOWN) && (g_gridSel / COLS + 1) * COLS < tiles)
        g_gridSel = std::min(g_gridSel + COLS, tiles - 1);
    if (pressed & RP_BTN_DUP && g_gridSel - COLS >= 0) g_gridSel -= COLS;
    if (pressed & RP_BTN_A && !g_hosts.empty()) {
        if (g_gridSel == 1) {
            g_brPath[0] = 0;
            g_screen = SCR_BROWSE;
            browseLoad();
            return;
        }
        uint16_t game = g_gridSel == 0 ? RP_GAME_NONE : (uint16_t)(g_gridSel - 2);
        connectTo(g_hosts[g_selHost].ip, game);
        return;
    }

    int selectedRow = g_gridSel / COLS;
    if (selectedRow < g_gridTop) g_gridTop = selectedRow;
    if (selectedRow >= g_gridTop + 2) g_gridTop = selectedRow - 1;
    std::vector<MenuTile> view;
    view.reserve(tiles);
    view.push_back({tr("Masaüstü", "Desktop"), tr("PC ekranını aç", "Open your PC screen"), nullptr, 0});
    view.push_back({tr("Oyun ekle", "Add a game"), tr("Kütüphanene ekle", "Grow your library"), nullptr, 1});
    for (int gi = 0; gi < int(games.size()); ++gi) {
        GameItem& g = games[gi];
        bool visible = gi + 2 >= g_gridTop * COLS && gi + 2 < (g_gridTop + 2) * COLS;
        if (visible && !g.icon.empty() && !g_gameTex[gi]) {
            SDL_Texture* t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STATIC, RP_ICON_SIZE, RP_ICON_SIZE);
            if (t) {
                SDL_UpdateTexture(t, nullptr, g.icon.data(), RP_ICON_SIZE * 4);
                SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                g_gameTex[gi] = t;
                g.icon.clear(); g.icon.shrink_to_fit();
            }
        }
        const char* badge = g.type == RP_GAME_STEAM ? "Steam" : g.type == RP_GAME_EPIC ? "Epic" : tr("Özel oyun", "Custom game");
        view.push_back({g.name, badge, g_gameTex[gi], 2});
    }
    drawLibrary(view, g_gridSel, g_gridTop, g_hosts.empty() ? nullptr : g_hosts[g_selHost].name,
                g_hosts.empty() ? "" : g_hosts[g_selHost].ip, g_gamesLoading, int(g_hosts.size()));
}

static void drawSettings(uint32_t pressed) {
    Settings& cfg = g_streamSettings ? g_settingsDraft : g_cfg;
    static int sel = 0;
    const int N = 13;
    if (pressed & (RP_BTN_L | RP_BTN_R)) {
        int group = sel < 4 ? 0 : sel < 7 ? 1 : sel < 11 ? 2 : 3;
        const int starts[] = {0, 4, 7, 11};
        group = (group + ((pressed & RP_BTN_R) ? 1 : 3)) % 4;
        sel = starts[group];
    }
    if (pressed & RP_BTN_DDOWN) sel = (sel + 1) % N;
    if (pressed & RP_BTN_DUP) sel = (sel + N - 1) % N;
    int d = (pressed & RP_BTN_DRIGHT) ? 1 : (pressed & RP_BTN_DLEFT) ? -1 : 0;
    if (d) {
        switch (sel) {
        case 0: cfg.resIdx += d;
            if (cfg.resIdx < 0) cfg.resIdx = 0;
            if (cfg.resIdx > 3) cfg.resIdx = 3;
            break;
        case 1: {
            const int modes[] = {30, 60, 120, 0};
            int index = 0;
            while (index < 3 && modes[index] != cfg.fps) ++index;
            cfg.fps = modes[(index + d + 4) % 4];
            break;
        }
        case 2: cfg.bitrateMbps += d * (cfg.bitrateMbps >= 10 ? 5 : 1);
            if (cfg.bitrateMbps < 2) cfg.bitrateMbps = 2;
            if (cfg.bitrateMbps > 35) cfg.bitrateMbps = 35;
            break;
        case 3: cfg.audio = !cfg.audio; break;
        case 4: cfg.maxPerformance = !cfg.maxPerformance; break;
        case 5: cfg.hwDecode = !cfg.hwDecode; break;
        case 6: cfg.mouseSens += d;
            if (cfg.mouseSens < 1) cfg.mouseSens = 1;
            if (cfg.mouseSens > 10) cfg.mouseSens = 10;
            break;
        case 7: cfg.clocks = !cfg.clocks; break;
        case 8: cfg.cpuMax = (cfg.cpuMax + 1 + d + 5) % 5 - 1; break;
        case 9: cfg.gpuMax = (cfg.gpuMax + 1 + d + 5) % 5 - 1; break;
        case 10: cfg.ramMHz = cfg.ramMHz == 1600 ? 2133 : 1600; break;
        case 11: cfg.english = !cfg.english; break;
        case 12: cfg.darkMode = !cfg.darkMode; break;
        }
    }
    g_english = cfg.english;
    theme::apply(cfg.darkMode);
    if (g_streamSettings && (pressed & RP_BTN_X)) {
        g_streamSettings = false; g_english = g_cfg.english; theme::apply(g_cfg.darkMode); return;
    }
    if (pressed & RP_BTN_B) {
        if (g_streamSettings) {
            const bool changed = cfg.resIdx != g_cfg.resIdx || cfg.fps != g_cfg.fps ||
                cfg.bitrateMbps != g_cfg.bitrateMbps || cfg.audio != g_cfg.audio ||
                cfg.maxPerformance != g_cfg.maxPerformance || cfg.hwDecode != g_cfg.hwDecode ||
                cfg.mouseSens != g_cfg.mouseSens || cfg.clocks != g_cfg.clocks ||
                cfg.cpuMax != g_cfg.cpuMax || cfg.gpuMax != g_cfg.gpuMax || cfg.ramMHz != g_cfg.ramMHz;
            if (changed) {
                const Settings selected = cfg;
                char ip[16]; snprintf(ip, sizeof(ip), "%s", g_connectedIp);
                streamLeave();
                g_cfg = selected; g_cfg.save();
                g_english = g_cfg.english; theme::apply(g_cfg.darkMode);
                connectTo(ip, RP_GAME_NONE);
            } else {
                g_cfg.english = cfg.english; g_cfg.darkMode = cfg.darkMode; g_cfg.save();
                g_streamSettings = false;
            }
        } else { cfg.save(); g_screen = SCR_MAIN; }
        return;
    }

    const char* names[N] = {
        tr("Çözünürlük", "Resolution"), tr("Kare Hızı", "Frame Rate"), tr("Bit Hızı", "Bitrate"), tr("Ses", "Audio"), tr("Performans", "Performance"),
        tr("H.264 Çözücü", "H.264 Decoder"), tr("Fare Hassasiyeti", "Mouse Sensitivity"), tr("OC Kontrolü", "OC Control"),
        tr("CPU Tavanı", "CPU Limit"), tr("GPU Tavanı", "GPU Limit"), tr("RAM Tavanı", "RAM Limit"), "Language / Dil", tr("Tema", "Theme")
    };
    char vals[N][32];
    static const char* RES_NAMES[] = { "480p", "540p", "720p", "1080p" };
    snprintf(vals[0], 32, "%s", RES_NAMES[cfg.resIdx & 3]);
    if (cfg.fps == 0) snprintf(vals[1], 32, "%s", tr("Sınırsız", "Unlimited"));
    else snprintf(vals[1], 32, "%d fps", cfg.fps);
    snprintf(vals[2], 32, "%d Mbps", cfg.bitrateMbps);
    snprintf(vals[3], 32, "%s", cfg.audio ? tr("Açık", "On") : tr("Kapalı", "Off"));
    snprintf(vals[4], 32, "%s", cfg.maxPerformance ? tr("Maksimum", "Maximum") : tr("Dengeli", "Balanced"));
    snprintf(vals[5], 32, "%s", cfg.hwDecode ? "NVDEC" : tr("Yazılım (CPU)", "Software (CPU)"));
    snprintf(vals[6], 32, "%d", cfg.mouseSens);
    snprintf(vals[7], 32, "%s", cfg.clocks ? tr("Uygulama", "Application") : tr("Sistem / harici", "System / external"));
    if (cfg.cpuMax < 0) snprintf(vals[8], 32, tr("Otomatik", "Automatic"));
    else snprintf(vals[8], 32, "%u MHz", CPU_HZ[cfg.cpuMax] / 1000000);
    if (cfg.gpuMax < 0) snprintf(vals[9], 32, tr("Otomatik", "Automatic"));
    else snprintf(vals[9], 32, "%.1f MHz", GPU_HZ[cfg.gpuMax] / 1000000.0);
    snprintf(vals[10], 32, "%d MHz", cfg.ramMHz);
    snprintf(vals[11], 32, "%s", cfg.english ? "English" : "Türkçe");
    snprintf(vals[12], 32, "%s", cfg.darkMode ? tr("Koyu", "Dark") : tr("Açık", "Light"));
    drawPreferences(names, vals, sel, g_streamSettings);
}

static void drawError(uint32_t pressed) {
    if (pressed & (RP_BTN_B | RP_BTN_A)) { g_screen = SCR_MAIN; return; }
    drawConnectionState(tr("Bağlanamadık.", "Could not connect."), g_errMsg, true);
}

struct TexUpdateCtx { bool created; };
static void frameSink(int w, int h, DecodedPixelFormat format,
                      const uint8_t* yp, int ypitch,
                      const uint8_t* up, int upitch,
                      const uint8_t* vp, int vpitch, void*) {
    if (!g_videoTex || g_texW != w || g_texH != h || g_texFormat != format) {
        if (g_videoTex) SDL_DestroyTexture(g_videoTex);
        Uint32 sdlFormat = format == DECODED_NV12
            ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
        g_videoTex = SDL_CreateTexture(g_ren, sdlFormat,
                                       SDL_TEXTUREACCESS_STREAMING, w, h);
        g_texW = w; g_texH = h; g_texFormat = format;
    }
    if (format == DECODED_NV12)
        SDL_UpdateNVTexture(g_videoTex, nullptr, yp, ypitch, up, upitch);
    else
        SDL_UpdateYUVTexture(g_videoTex, nullptr, yp, ypitch, up, upitch, vp, vpitch);
}

static const char* overlayLabel(int index) {
    const char* items[] = {
    tr("Devam Et", "Resume"), tr("Klavye Aç", "Open Keyboard"), "Enter", "Esc", "Alt+Tab", tr("Win Tuşu", "Windows Key"),
    tr("İstatistikler", "Statistics"), tr("Monitör Değiştir", "Switch Monitor"), tr("Yayın Ayarları", "Stream Settings"), tr("Bağlantıyı Kes", "Disconnect")
};
    return items[index];
}
static const int OVERLAY_N = 10;

static void overlayAction(int idx) {
    switch (idx) {
    case 0: g_overlay = false; break;
    case 1: {
        char text[400] = "";
        if (swkbdText(tr("PC'ye gönderilecek metin", "Text to send to PC"), nullptr, text, sizeof(text))) {
            std::lock_guard<std::mutex> lk(g_textMtx);
            g_pendingText = text;
        }
        g_overlay = false;
        break;
    }
    case 2: g_pendingKey = RP_KEY_ENTER; g_overlay = false; break;
    case 3: g_pendingKey = RP_KEY_ESCAPE; g_overlay = false; break;
    case 4: g_pendingKey = RP_KEY_ALTTAB; g_overlay = false; break;
    case 5: g_pendingKey = RP_KEY_LWIN; g_overlay = false; break;
    case 6: g_showStats = !g_showStats; break;
    case 7: sendCtrl(RP_CTRL_NEXT_MONITOR); g_overlay = false; break;
    case 8: g_settingsDraft = g_cfg; g_streamSettings = true; break;
    case 9: streamLeave(); break;
    }
}

static void drawStream(uint32_t cur, uint32_t pressed) {
    if (!sessionAlive() || !decoderAlive()) {
        snprintf(g_errMsg, sizeof(g_errMsg), "%s",
                 decoderAlive() ? tr("Host bağlantısı koptu", "Host connection lost") : tr("Görüntü çözücü durdu; yeniden bağlanın", "Video decoder stopped; reconnect"));
        streamLeave();
        g_screen = SCR_ERROR;
        return;
    }

    static bool comboPrev = false;
    bool combo = (cur & RP_BTN_ZL) && (cur & RP_BTN_ZR) && (cur & RP_BTN_PLUS);
    if (!g_streamSettings && combo && !comboPrev) { g_overlay = !g_overlay; g_overlaySel = 0; }
    comboPrev = combo;

    static uint32_t s_uploadUs = 0;
    uint64_t upT0 = armGetSystemTick();
    if (g_directRenderer ? decoderTakeNativeFrame(presenterSubmit, nullptr) : decoderTakeFrame(frameSink, nullptr))
        s_uploadUs = (uint32_t)armTicksToNs(armGetSystemTick() - upT0) / 1000;
    if (g_directRenderer) { g_texW = g_cfg.width(); g_texH = g_cfg.height(); }
    g_presentUi = g_overlay || g_streamSettings || g_showStats || g_audioErr[0] ||
                  (g_directRenderer ? !presenterHasFrame() : !g_videoTex);
    g_uiRegion = g_showStats && !g_overlay && !g_streamSettings && !g_audioErr[0] && presenterHasFrame()
        ? SDL_Rect{16, 16, 500, 200} : SDL_Rect{0, 0, 1280, 720};
    if (!g_directRenderer || g_presentUi) {
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, g_directRenderer ? 0 : 255);
        if (g_directRenderer) {
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
            SDL_RenderFillRect(g_ren, &g_uiRegion);
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        } else SDL_RenderClear(g_ren);
    }
    if (!g_directRenderer && g_videoTex) {

        float sc = std::fmin(1280.0f / g_texW, 720.0f / g_texH);
        int w = (int)(g_texW * sc), h = (int)(g_texH * sc);
        SDL_Rect dst{ (1280 - w) / 2, (720 - h) / 2, w, h };
        SDL_RenderCopy(g_ren, g_videoTex, nullptr, &dst);
    } else if (g_directRenderer ? !presenterHasFrame() : !g_videoTex) {
        drawText(520, 340, FS_BODY, theme::dim, tr("Görüntü bekleniyor...", "Waiting for video..."));
    }

    uint64_t now = nowMs();
    if (now - g_lastStatAt >= 1000) {
        uint32_t f = g_directRenderer ? presenterFrameCount() : decoderFrameCount();
        uint64_t b = g_netStats.videoBytes;
        g_dispFps = (float)(f - g_lastFrames) * 1000.f / (now - g_lastStatAt);
        g_dispMbps = (float)(b - g_lastBytes) * 8.f / 1e6f * 1000.f / (now - g_lastStatAt);
        g_lastFrames = f; g_lastBytes = b; g_lastStatAt = now;
        clocksApply();
        clocksRead();
    }
    if (now - g_lastPingAt >= 1000) { sendPing(); g_lastPingAt = now; }

    if (g_showStats) {
        fillRoundedRect(16, 16, 500, 200, 12, theme::card);
        int rtt = getRttMs();
        drawText(32, 26, FS_SMALL, theme::accent, "%.0f fps   %.1f Mbps", g_dispFps, g_dispMbps);
        drawText(32, 52, FS_SMALL, theme::text, tr("Gecikme: %s", "Latency: %s"),
                 rtt < 0 ? "—" : (std::to_string(rtt) + " ms").c_str());
        drawText(32, 78, FS_SMALL, theme::text, tr("Kayıp kare: %u", "Lost frames: %u"), g_netStats.droppedFrames.load());
        if (g_audioErr[0])
            drawText(32, 104, FS_SMALL, theme::danger, tr("Ses: AYGIT HATASI", "Audio: DEVICE ERROR"));
        else if (g_audioDev)
            drawText(32, 104, FS_SMALL, theme::text, tr("Ses kuyruğu: %u ms", "Audio queue: %u ms"),
                     (unsigned)audioQueueMs());
        else
            drawText(32, 104, FS_SMALL, theme::dim, tr("Ses: kapalı", "Audio: off"));
        if (g_clkOpen) {
            char cpu[16], gpu[16], emc[16];
            drawText(32, 130, FS_SMALL, theme::text, "CPU %s / GPU %s / RAM %s MHz",
                      clockText(g_cpuHz, cpu), clockText(g_gpuHz, gpu), clockText(g_emcHz, emc));
        } else
            drawText(32, 130, FS_SMALL, theme::dim, tr("OC kontrolü yok", "OC control unavailable"));
        drawText(32, 156, FS_SMALL, theme::dim, "%s dec %.1f  up %.1fms  q%u  %dx%d",
                 decoderUsingHardware() ? "NVDEC" : "CPU",
                 decoderLastDecodeUs() / 1000.0f, s_uploadUs / 1000.0f,
                 decoderQueueDepth(), g_texW, g_texH);
        if (g_directRenderer)
            drawText(32, 182, FS_SMALL, theme::accent, "%s | %.2f ms | copy %u KB",
                     presenterPath(), presenterUploadUs() / 1000.f, presenterVideoCopyBytes() / 1024);
        else if (g_clkOpen)
            drawText(32, 182, FS_SMALL, theme::dim, tr("Hedef CPU %u / RAM <=%d MHz", "Target CPU %u / RAM <=%d MHz"),
                     CPU_HZ[std::min(g_cfg.maxPerformance ? 3 : 2, cpuCap())] / 1000000, g_cfg.ramMHz);
    }
    if (g_audioErr[0] && !g_overlay)
        drawText(360, 24, FS_SMALL, theme::danger, "%s", g_audioErr);

    if (g_streamSettings) { drawSettings(pressed); return; }

    if (g_overlay) {

        const char* labels[OVERLAY_N];
        for (int i = 0; i < OVERLAY_N; ++i) labels[i] = overlayLabel(i);
        char statistics[64];
        snprintf(statistics, sizeof(statistics), tr("İstatistikler: %s", "Statistics: %s"),
                 g_showStats ? tr("Açık", "On") : tr("Kapalı", "Off"));
        labels[6] = statistics;
        if (pressed & RP_BTN_DDOWN) g_overlaySel = (g_overlaySel + 1) % OVERLAY_N;
        if (pressed & RP_BTN_DUP) g_overlaySel = (g_overlaySel + OVERLAY_N - 1) % OVERLAY_N;
        drawSessionMenu(labels, g_overlaySel, OVERLAY_N);
        if (pressed & RP_BTN_B) g_overlay = false;
        if (pressed & RP_BTN_A) {
            overlayAction(g_overlaySel);
            if (g_screen != SCR_STREAM) return;
        }
    }
}

static void handleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_FINGERDOWN:
        g_fingers++;
        if (g_fingers == 1) { g_touchDownAt = nowMs(); g_touchMoved = false; }
        else if (g_fingers == 2 && g_screen == SCR_STREAM && !g_overlay && !g_streamSettings)
            g_rclickUntil = nowMs() + 60;
        break;
    case SDL_FINGERUP:
        if (g_fingers > 0) g_fingers--;
        if (g_fingers == 0 && g_screen == SCR_STREAM && !g_overlay && !g_streamSettings &&
            !g_touchMoved && nowMs() - g_touchDownAt < 250 && nowMs() >= g_rclickUntil)
            g_clickUntil = nowMs() + 60;
        break;
    case SDL_FINGERMOTION:
        if (g_screen != SCR_STREAM || g_overlay || g_streamSettings) break;
        if (std::fabs(e.tfinger.dx) > 0.002f || std::fabs(e.tfinger.dy) > 0.002f)
            g_touchMoved = true;
        if (g_fingers >= 2) {
            g_wheelFx += (int)(-e.tfinger.dy * 20.f * 16);
        } else {
            float k = 1280.f * (0.4f + 0.12f * g_cfg.mouseSens);
            g_accDxFx += (int)(e.tfinger.dx * k * 16);
            g_accDyFx += (int)(e.tfinger.dy * k * 16);
        }
        break;
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_CONTROLLERDEVICEREMOVED:
        ensurePad();
        break;
    }
}

static void bootNote(const char* stage, const char* detail = "", bool reset = false) {
    FILE* f = fopen("/switch/lumiri-boot.log", reset ? "w" : "a");
    if (!f) return;
    fprintf(f, "%s %s\n", stage, detail);
    fclose(f);
}

int main(int, char**) {
    bootNote("Lumiri 1.4.2 startup", "", true);
    char environment[96];
    const auto version = hosversionGet();
    snprintf(environment, sizeof(environment), "HOS=%u.%u.%u appletType=%u directBuild=%u",
             unsigned((version >> 16) & 255), unsigned((version >> 8) & 255),
             unsigned(version & 255), unsigned(appletGetAppletType()), unsigned(RP_EXPERIMENTAL_DIRECT));
    bootNote("environment", environment);
    bootNote("network init");
    if (!netInit()) { bootNote("network init failed"); return 1; }
    g_cfg.load();
    g_english = g_cfg.english; theme::apply(g_cfg.darkMode);
#if !RP_EXPERIMENTAL_DIRECT

    g_cfg.directVideo = false;
#endif

    bootNote("SDL init");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        bootNote("SDL init failed", SDL_GetError()); SDL_Quit(); netExit(); return 1;
    }
    bootNote("window creation");
    g_win = SDL_CreateWindow("Lumiri", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
    if (!g_win) { bootNote("window creation failed", SDL_GetError()); SDL_Quit(); netExit(); return 1; }
    bootNote("renderer creation");
    if (g_cfg.directVideo && presenterInit()) {
        g_uiSurface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_ABGR8888);
        if (g_uiSurface) g_ren = SDL_CreateSoftwareRenderer(g_uiSurface);
        g_directRenderer = g_ren != nullptr;
        if (!g_directRenderer) {
            if (g_uiSurface) SDL_FreeSurface(g_uiSurface);
            g_uiSurface = nullptr; presenterExit();
        }
    }
    if (!g_directRenderer)
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) { bootNote("renderer creation failed", SDL_GetError()); SDL_DestroyWindow(g_win); SDL_Quit(); netExit(); return 1; }
    SDL_RenderSetLogicalSize(g_ren, 1280, 720);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    bootNote("font and UI init");
    if (!uiInit()) {
        bootNote("font and UI init failed", SDL_GetError());
        SDL_DestroyRenderer(g_ren);
        if (g_uiSurface) SDL_FreeSurface(g_uiSurface);
        if (g_directRenderer) presenterExit();
        SDL_Quit();
        netExit();
        return 1;
    }
    ensurePad();
    discoveryStart();
    bootNote("main loop ready");

    EdgeDetect nav;
    uint32_t heldDirection = 0;
    uint64_t repeatAt = 0;
    bool quit = false;
    uint64_t lastTick = 0;
    while (!quit && !g_presentFailed && appletMainLoop()) {

        uint64_t t = nowMs();
        if (lastTick && t - lastTick > 500 && g_screen == SCR_STREAM && sessionAlive())
            sessionResync();
        lastTick = t;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            handleEvent(e);
        }
        ensurePad();
        uint32_t cur = readButtons();
        uint32_t pressed = nav.pressed(cur);
        uint32_t directions = cur & (RP_BTN_DUP | RP_BTN_DDOWN | RP_BTN_DLEFT | RP_BTN_DRIGHT);
        bool menuActive = g_screen == SCR_MAIN || g_screen == SCR_SETTINGS ||
                          (g_screen == SCR_STREAM && (g_overlay || g_streamSettings));
        if (!menuActive || !directions) { heldDirection = 0; repeatAt = 0; }
        else if (directions != heldDirection) { heldDirection = directions; repeatAt = t + 330; }
        else if (t >= repeatAt) { pressed |= directions; repeatAt = t + 110; }

        if (g_directRenderer && !presenterBegin()) { g_presentFailed = true; break; }

        switch (g_screen) {
        case SCR_MAIN:
            if (pressed & RP_BTN_PLUS) { quit = true; break; }
            drawMain(pressed);
            break;
        case SCR_SETTINGS: drawSettings(pressed); break;
        case SCR_ERROR:    drawError(pressed); break;
        case SCR_BROWSE:   drawBrowse(cur, pressed); break;
        case SCR_STREAM:   drawStream(cur, pressed); break;
        }
        presentFrame();
    }

    if (g_screen == SCR_STREAM) streamLeave();
    if (g_fetchThread.joinable()) g_fetchThread.join();
    clearGamesUI();
    discoveryStop();
    uiExit();
    if (g_pad) SDL_GameControllerClose(g_pad);
    SDL_DestroyRenderer(g_ren);
    if (g_uiSurface) SDL_FreeSurface(g_uiSurface);
    if (g_directRenderer) presenterExit();
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    netExit();
    bootNote("exit", g_presentFailed ? "presentation failed" : "normal");
    return 0;
}
