#include "common.h"
#include <qos2.h>
#include <thread>
#include <cstdarg>
#include <ctime>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "qwave.lib")

static HANDLE g_qosHandle = nullptr;
static DEVMODEW g_prevDisplayMode{};
static bool g_displayModeChanged = false;

Session g_session;
SOCKET g_mediaSock = INVALID_SOCKET;
static CRITICAL_SECTION g_logCs;
static FILE* g_diagnostic = nullptr;

extern bool queryAudioFormat();
extern bool audioRouteToCable();
extern void audioRestoreRoute();
extern uint32_t g_audioRate;
extern uint16_t g_audioChannels;

static void optimizeDisplayRefresh(uint16_t streamFps) {
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) return;
    if (current.dmDisplayFrequency && current.dmDisplayFrequency % streamFps == 0) {
        logmsg("Ekran yenileme: %u Hz (yayınla senkron)", current.dmDisplayFrequency);
        return;
    }

    DEVMODEW best{};
    DWORD bestHz = 0;
    for (DWORD i = 0;; ++i) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(nullptr, i, &mode)) break;
        if (mode.dmPelsWidth != current.dmPelsWidth ||
            mode.dmPelsHeight != current.dmPelsHeight ||
            mode.dmBitsPerPel != current.dmBitsPerPel ||
            !mode.dmDisplayFrequency || mode.dmDisplayFrequency > 120 ||
            mode.dmDisplayFrequency % streamFps != 0) continue;
        if (mode.dmDisplayFrequency > bestHz) {
            best = mode;
            bestHz = mode.dmDisplayFrequency;
        }
    }
    if (!bestHz) {
        logmsg("[--] %u Hz, %u fps'in tam katı değil; mikro-takılma olabilir",
               current.dmDisplayFrequency, streamFps);
        return;
    }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    if (ChangeDisplaySettingsExW(nullptr, &best, nullptr, CDS_FULLSCREEN, nullptr) ==
        DISP_CHANGE_SUCCESSFUL) {
        g_prevDisplayMode = current;
        g_displayModeChanged = true;
        logmsg("[OK] Ekran yenileme yayın için geçici %u -> %u Hz",
               current.dmDisplayFrequency, bestHz);
        Sleep(500);
    }
}

static void restoreDisplayRefresh() {
    if (!g_displayModeChanged) return;
    g_prevDisplayMode.dmFields =
        DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    ChangeDisplaySettingsExW(nullptr, &g_prevDisplayMode, nullptr, 0, nullptr);
    g_displayModeChanged = false;
    logmsg("Ekran yenileme eski haline döndürüldü");
}

void logmsg(const char* fmt, ...) {
    EnterCriticalSection(&g_logCs);
    time_t t = time(nullptr);
    tm tmv; localtime_s(&tmv, &t);
    printf("[%02d:%02d:%02d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap; va_start(ap, fmt);
    if (g_diagnostic) {
        va_list copy; va_copy(copy, ap);
        fprintf(g_diagnostic, "[%02d:%02d:%02d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        vfprintf(g_diagnostic, fmt, copy);
        va_end(copy);
        fputc('\n', g_diagnostic); fflush(g_diagnostic);
    }
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    LeaveCriticalSection(&g_logCs);
}

void sendMedia(uint8_t channel, uint8_t flags, uint32_t frameId,
               const uint8_t* data, size_t len) {
    if (!g_session.active) return;
    uint16_t partCount = (uint16_t)((len + RP_MAX_PAYLOAD - 1) / RP_MAX_PAYLOAD);
    if (!partCount) partCount = 1;

    bool fec = channel == RP_CH_VIDEO && partCount > 1;
    alignas(32) uint8_t parity[RP_MAX_PAYLOAD];
    if (fec) memset(parity, 0, sizeof(parity));

    auto sendPacket = [&](const RpMediaHdr& hdr, const uint8_t* payload, uint16_t payloadLen) {
        WSABUF bufs[2] = {
            { (ULONG)sizeof(hdr), (CHAR*)&hdr },
            { payloadLen, (CHAR*)payload },
        };
        DWORD sent = 0;
        if (WSASend(g_mediaSock, bufs, 2, &sent, 0, nullptr, nullptr) == SOCKET_ERROR)
            g_session.sendErrors++;

        if (channel == RP_CH_VIDEO) {
            static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
            uint64_t bps = (uint64_t)(g_session.bitrateKbps ? g_session.bitrateKbps : 8000) * 1000;
            int64_t us = (int64_t)((sizeof(hdr) + payloadLen) * 8ull * 1000000ull /
                                   (bps * 3ull));
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            do { YieldProcessor(); QueryPerformanceCounter(&t1); }
            while ((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart < us);
        }
    };

    for (uint16_t i = 0; i < partCount; i++) {
        size_t off = (size_t)i * RP_MAX_PAYLOAD;
        uint16_t plen = (uint16_t)((len - off) > RP_MAX_PAYLOAD ? RP_MAX_PAYLOAD : (len - off));
        RpMediaHdr h{};
        h.channel = channel; h.flags = flags;
        h.partIdx = i; h.partCount = partCount;
        h.payloadLen = plen; h.frameId = frameId;
        h.frameBytes = (uint32_t)len;
        sendPacket(h, data + off, plen);
        if (fec) {
            for (uint16_t k = 0; k < plen; k++) parity[k] ^= data[off + k];
            if ((i % RP_FEC_GROUP) == RP_FEC_GROUP - 1 || i + 1 == partCount) {
                RpMediaHdr ph{};
                ph.channel = channel; ph.flags = flags | RP_FLAG_PARITY;
                ph.partIdx = i / RP_FEC_GROUP; ph.partCount = partCount;
                ph.payloadLen = RP_MAX_PAYLOAD; ph.frameId = frameId;
                ph.frameBytes = (uint32_t)len;
                sendPacket(ph, parity, RP_MAX_PAYLOAD);
                memset(parity, 0, sizeof(parity));
            }
        }
    }
}

static void discoveryThread() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(RP_PORT_DISCOVERY);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        logmsg("Keşif portu bind edilemedi"); return;
    }
    char host[32] = "PC";
    DWORD hlen = sizeof(host);
    GetComputerNameA(host, &hlen);
    char buf[64];
    while (true) {
        sockaddr_in from; int flen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
        if (n < (int)sizeof(RpDiscoverReq)) continue;
        RpDiscoverReq* req = (RpDiscoverReq*)buf;
        if (req->magic != RP_MAGIC || req->version != RP_PROTO_VERSION) continue;
        RpDiscoverResp resp{ RP_MAGIC, RP_PROTO_VERSION, {} };
        strncpy_s(resp.hostname, host, _TRUNCATE);
        sendto(s, (char*)&resp, sizeof(resp), 0, (sockaddr*)&from, flen);
    }
}

static void statsLoop() {
    uint64_t lastBytes = 0, lastFrames = 0;
    while (g_session.active && !g_session.stop) {
        for (int i = 0; i < 20 && g_session.active && !g_session.stop; ++i)
            Sleep(100);
        if (!g_session.active || g_session.stop) break;
        uint64_t b = g_session.videoBytes, f = g_session.videoFrames;
        logmsg("  stat: %.1f fps | %.2f Mbps | capture %.2fms | encode %.2fms | send %.2fms | qdrop %u | backpressure %u |"
               " girdi: %llu | udp hata: %llu",
            (f - lastFrames) / 2.0, (b - lastBytes) * 8.0 / 2e6,
            g_session.captureUs.load() / 1000.0,
            g_session.encodeSendUs.load() / 1000.0,
            g_session.sendUs.load() / 1000.0,
            g_session.videoQueueDrops.load(), g_session.captureSkips.load(),
            (unsigned long long)g_session.inputPkts.load(),
            (unsigned long long)g_session.sendErrors.load());
        lastBytes = b; lastFrames = f;
    }
}

static bool recvAll(SOCKET s, char* buf, int len) {
    while (len > 0) {
        int n = recv(s, buf, len, 0);
        if (n <= 0) return false;
        buf += n; len -= n;
    }
    return true;
}

static bool sendAll(SOCKET s, const char* buf, int len) {
    while (len > 0) {
        const int n = send(s, buf, len, 0);
        if (n <= 0) return false;
        buf += n; len -= n;
    }
    return true;
}

extern void scanGames();
extern void sendGamesList(SOCKET c);
extern void launchGameIdx(uint16_t idx);
extern void handleBrowse(SOCKET c, const RpBrowseReq* req);
extern void handleAddGame(SOCKET c, const RpAddGameReq* req);

static void serveClient(SOCKET c, sockaddr_in peer) {
    DWORD ioTimeout = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char*)&ioTimeout, sizeof(ioTimeout));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char*)&ioTimeout, sizeof(ioTimeout));
    char ip[64];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    logmsg("TCP kabul: %s; istek bekleniyor", ip);

    uint32_t magic = 0;
    if (!recvAll(c, (char*)&magic, 4)) { closesocket(c); return; }
    if (magic == RP_MAGIC_GAMES) {
        uint32_t ver = 0;
        recvAll(c, (char*)&ver, 4);
        logmsg("Oyun listesi taraniyor");
        scanGames();
        sendGamesList(c);
        logmsg("Oyun listesi tamamlandi");
        closesocket(c);
        return;
    }
    if (magic == RP_MAGIC_BROWSE) {
        RpBrowseReq req{};
        req.magic = magic;
        if (recvAll(c, (char*)&req + 4, sizeof(req) - 4)) handleBrowse(c, &req);
        closesocket(c);
        return;
    }
    if (magic == RP_MAGIC_ADDGAME) {
        RpAddGameReq req{};
        req.magic = magic;
        if (recvAll(c, (char*)&req + 4, sizeof(req) - 4)) handleAddGame(c, &req);
        closesocket(c);
        return;
    }

    RpHandshake hs;
    hs.magic = magic;
    if (magic != RP_MAGIC || !recvAll(c, (char*)&hs + 4, sizeof(hs) - 4)) {
        logmsg("%s: geçersiz handshake", ip);
        closesocket(c); return;
    }
    if (hs.version != RP_PROTO_VERSION) {
        RpHandshakeAck nak{ RP_MAGIC, 0, 0, 0 };
        send(c, (char*)&nak, sizeof(nak), 0);
        closesocket(c); return;
    }
    bool validRes = (hs.width == 854 && hs.height == 480) ||
                    (hs.width == 960 && hs.height == 540) ||
                    (hs.width == 1280 && hs.height == 720) ||
                    (hs.width == 1920 && hs.height == 1080);
    if (!validRes || (hs.fps != 0 && hs.fps != 30 && hs.fps != 60 && hs.fps != 120) ||
        hs.bitrateKbps < 2000 || hs.bitrateKbps > 35000) {
        RpHandshakeAck nak{ RP_MAGIC, 0, 0, 0 };
        send(c, (char*)&nak, sizeof(nak), 0);
        closesocket(c);
        return;
    }
    g_session.width = hs.width; g_session.height = hs.height;
    g_session.unlimitedFps = hs.fps == 0;
    g_session.fps = hs.fps ? hs.fps : 120; g_session.bitrateKbps = hs.bitrateKbps;
    g_session.audioEnabled = hs.audioEnabled != 0;
    g_session.mediaAddr = peer;
    g_session.mediaAddr.sin_port = htons(RP_PORT_MEDIA);
    g_session.videoBytes = 0; g_session.videoFrames = 0;
    g_session.inputPkts = 0; g_session.sendErrors = 0;
    g_session.idrReqs = 0;
    g_session.stop = false;
    g_session.nextMonitor = false;
    g_session.wantIdr = true;
    const ULONGLONG handshakeStart = GetTickCount64();
    logmsg("Handshake alindi: ekran hazirlaniyor");
    if (!g_session.unlimitedFps) optimizeDisplayRefresh(g_session.fps);
    else logmsg("Stream FPS: Unlimited");

    if (g_session.audioEnabled) {
        logmsg("Handshake: ses yonlendirme");
        audioRouteToCable();
        logmsg("Handshake: ses formati");
        queryAudioFormat();
    }

    connect(g_mediaSock, (sockaddr*)&g_session.mediaAddr, sizeof(g_session.mediaAddr));

    QOS_FLOWID qosFlow = 0;
    logmsg("Handshake: medya/QoS");
    if (g_qosHandle)
        QOSAddSocketToFlow(g_qosHandle, g_mediaSock, (sockaddr*)&g_session.mediaAddr,
                           QOSTrafficTypeAudioVideo, QOS_NON_ADAPTIVE_FLOW, &qosFlow);

    RpHandshakeAck ack{ RP_MAGIC, 1, g_audioRate, g_audioChannels };
    if (!sendAll(c, (char*)&ack, sizeof(ack))) {
        logmsg("Handshake yaniti gonderilemedi: Winsock %d", WSAGetLastError());
        if (qosFlow) QOSRemoveSocketFromFlow(g_qosHandle, g_mediaSock, qosFlow, 0);
        g_session.stop = true;
        audioRestoreRoute(); restoreDisplayRefresh(); closesocket(c);
        return;
    }
    logmsg("Handshake yaniti gonderildi (%llu ms)", GetTickCount64() - handshakeStart);
    g_session.active = true;
    logmsg("İstemci bağlandı: %s (%ux%u@%u, %u kbps, ses %s)",
        ip, hs.width, hs.height, hs.fps, hs.bitrateKbps, hs.audioEnabled ? "açık" : "kapalı");

    if (hs.launchGame != RP_GAME_NONE) launchGameIdx(hs.launchGame);

    std::thread tv(videoThread);
    std::thread ta;
    if (g_session.audioEnabled) ta = std::thread(audioThread);
    std::thread ts(statsLoop);

    char type;
    ULONGLONG lastControl = GetTickCount64();
    while (!g_session.stop) {
        fd_set readable;
        FD_ZERO(&readable); FD_SET(c, &readable);
        timeval timeout{0, 200000};
        int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready < 0) break;
        if (!ready) {
            if (GetTickCount64() - lastControl > 10000) break;
            continue;
        }
        if (recv(c, &type, 1, 0) != 1) break;
        lastControl = GetTickCount64();
        if (type == RP_CTRL_IDR) { g_session.wantIdr = true; g_session.idrReqs++; }
        else if (type == RP_CTRL_NEXT_MONITOR) g_session.nextMonitor = true;
        else if (type == RP_CTRL_PING) { char pong = RP_CTRL_PONG; send(c, &pong, 1, 0); }
        else if (type == RP_CTRL_BYE) break;
    }

    logmsg("İstemci ayrıldı: %s", ip);
    if (qosFlow) QOSRemoveSocketFromFlow(g_qosHandle, g_mediaSock, qosFlow, 0);
    g_session.active = false;
    g_session.stop = true;
    tv.join();
    if (ta.joinable()) ta.join();
    ts.join();
    audioRestoreRoute();
    restoreDisplayRefresh();
    closesocket(c);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    DWORD consoleMode = 0;
    const HANDLE consoleInput = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(consoleInput, &consoleMode))
        SetConsoleMode(consoleInput, (consoleMode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
    wchar_t logPath[MAX_PATH];
    const DWORD pathLen = GetModuleFileNameW(nullptr, logPath, MAX_PATH);
    if (pathLen && pathLen < MAX_PATH) {
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash && (slash - logPath) + 21 < MAX_PATH) {
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - logPath), L"lumiri-host.log");
            _wfopen_s(&g_diagnostic, logPath, L"w");
        }
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    InitializeCriticalSection(&g_logCs);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    if (argc >= 3 && !strcmp(argv[1], "--video-selftest")) {
        extern int videoSelfTest(const char*, bool, bool, bool, unsigned);
        const int result = videoSelfTest(argv[2], argc > 3 && !strcmp(argv[3], "sync"),
                                        argc > 3 && !strcmp(argv[3], "slow"),
                                        argc > 3 && !strcmp(argv[3], "monitor"),
                                        argc > 3 && !strcmp(argv[3], "unlimited") ? 0 :
                                        argc > 3 && !strcmp(argv[3], "120") ? 120 : 60);
        WSACleanup(); return result;
    }

    SOCKET lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(RP_PORT_CONTROL);
    if (lst == INVALID_SOCKET ||
        setsockopt(lst, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&exclusive, sizeof(exclusive)) != 0 ||
        bind(lst, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(lst, SOMAXCONN) != 0) {
        const int socketError = WSAGetLastError();
        char startupError[512];
        snprintf(startupError, sizeof(startupError),
                 "TCP %d acilamadi (Winsock %d).\n\n"
                 "Diger Lumiri host pencerelerini kapatin ve tekrar deneyin.\n"
                 "Eski host dist veya baska bir klasorden acik kalmis olabilir.",
                 RP_PORT_CONTROL, socketError);
        logmsg("%s", startupError);
        if (lst != INVALID_SOCKET) closesocket(lst);
        WSACleanup();
        MessageBoxA(nullptr, startupError, "Lumiri - Host baslatilamadi", MB_OK | MB_ICONERROR);
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        audioRestoreRoute();
        restoreDisplayRefresh();
        ExitProcess(0);
    }, TRUE);

    printf("==============================================\n");
    printf("  Lumiri PC Host  v1.3.1\n");
    printf("  Switch istemcisinin baglanmasi bekleniyor...\n");
    printf("==============================================\n");

    {
        BOOL elevated = FALSE;
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION te{}; DWORD rl = 0;
            if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &rl))
                elevated = te.TokenIsElevated;
            CloseHandle(tok);
        }
        if (elevated) logmsg("[OK] Yönetici — GPU önceliği tam etkin olacak");
        else logmsg("[!!] Yönetici DEĞİL — oyun içinde fps düşüyorsa host'u sağ tık > Yönetici olarak çalıştırın");
    }
    logmsg("Video backend: NVIDIA NVENC / AMD AMF H.264 (selected from the capture GPU on connection)");
    { extern void ensureVigemBus(); ensureVigemBus(); }
    queryAudioFormat();
    extern bool hasCableDevice();
    if (hasCableDevice())
        logmsg("[OK] VB-Cable kurulu — stream sırasında PC hoparlörü otomatik susturulur");
    else
        logmsg("[--] VB-Cable yok — ses PC'den de çalar. Sessiz PC için: vb-audio.com/Cable");
    logmsg("Baglanti engellenirse bu EXE'nin yanindaki firewall-onar.bat dosyasini yonetici olarak calistirin.");
    QOS_VERSION qv{ 1, 0 };
    if (!QOSCreateHandle(&qv, &g_qosHandle)) g_qosHandle = nullptr;
    g_mediaSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD mediaSendTimeout = 20;
    setsockopt(g_mediaSock, SOL_SOCKET, SO_SNDTIMEO, (char*)&mediaSendTimeout, sizeof(mediaSendTimeout));

    int sndbuf = 512 * 1024;
    setsockopt(g_mediaSock, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));

    std::thread(discoveryThread).detach();
    std::thread(inputThread).detach();

    logmsg("Hazır: keşif UDP %d, kontrol TCP %d", RP_PORT_DISCOVERY, RP_PORT_CONTROL);

    while (true) {
        sockaddr_in peer; int plen = sizeof(peer);
        SOCKET c = accept(lst, (sockaddr*)&peer, &plen);
        if (c == INVALID_SOCKET) break;
        BOOL nd = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (char*)&nd, sizeof(nd));
        serveClient(c, peer);
    }
    return 0;
}
