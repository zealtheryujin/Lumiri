#include "common.h"
#include "../common/stream_utils.h"
#include <ViGEm/Client.h>
#include <urlmon.h>
#include <shellapi.h>
#include <vector>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")

static PVIGEM_CLIENT g_vigem = nullptr;
static PVIGEM_TARGET g_pad = nullptr;

static const char* VIGEM_URL =
    "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/"
    "ViGEmBus_1.22.0_x64_x86_arm64.exe";

void ensureVigemBus() {
    PVIGEM_CLIENT probe = vigem_alloc();
    if (!probe) return;
    if (VIGEM_SUCCESS(vigem_connect(probe))) {
        vigem_disconnect(probe);
        vigem_free(probe);
        logmsg("[OK] ViGEmBus kurulu");
        return;
    }
    vigem_free(probe);

    logmsg("[--] ViGEmBus yok — gamepad çalışmaz. Otomatik kurulum başlatılıyor...");
    char dst[MAX_PATH];
    if (!GetTempPathA(sizeof(dst), dst)) return;
    strncat_s(dst, "ViGEmBusSetup.exe", _TRUNCATE);

    logmsg("     İndiriliyor: %s", VIGEM_URL);
    if (FAILED(URLDownloadToFileA(nullptr, VIGEM_URL, dst, 0, nullptr))) {
        logmsg("[!!] İndirilemedi (internet yok?). Elle kurun: %s", VIGEM_URL);
        return;
    }

    logmsg("     Kurulum açılıyor — UAC penceresine 'Evet' deyip kurulumu tamamlayın.");
    SHELLEXECUTEINFOA se{};
    se.cbSize = sizeof(se);
    se.fMask = SEE_MASK_NOCLOSEPROCESS;
    se.lpVerb = "runas";
    se.lpFile = dst;
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&se)) {
        logmsg("[!!] Kurulum başlatılamadı. Elle çalıştırın: %s", dst);
        return;
    }
    WaitForSingleObject(se.hProcess, INFINITE);
    CloseHandle(se.hProcess);
    DeleteFileA(dst);

    probe = vigem_alloc();
    if (!probe) return;
    if (VIGEM_SUCCESS(vigem_connect(probe))) {
        vigem_disconnect(probe);
        logmsg("[OK] ViGEmBus kuruldu — gamepad hazır");
    } else {
        logmsg("[!!] ViGEmBus hâlâ görünmüyor — PC'yi yeniden başlatmak gerekebilir.");
    }
    vigem_free(probe);
}

static bool vigemInit() {
    g_vigem = vigem_alloc();
    if (!VIGEM_SUCCESS(vigem_connect(g_vigem))) {
        logmsg("[--] ViGEmBus sürücüsü yok — gamepad çalışmaz. Kur: github.com/nefarius/ViGEmBus/releases");
        vigem_free(g_vigem); g_vigem = nullptr;
        return false;
    }
    g_pad = vigem_target_x360_alloc();
    if (!VIGEM_SUCCESS(vigem_target_add(g_vigem, g_pad))) {
        logmsg("Sanal X360 pad eklenemedi");
        vigem_target_free(g_pad); g_pad = nullptr;
        vigem_disconnect(g_vigem); vigem_free(g_vigem); g_vigem = nullptr;
        return false;
    }
    logmsg("[OK] Sanal Xbox 360 gamepad hazır (ViGEmBus)");
    return true;
}

static USHORT mapButtons(uint32_t b) {
    USHORT x = 0;
    if (b & RP_BTN_A) x |= XUSB_GAMEPAD_A;
    if (b & RP_BTN_B) x |= XUSB_GAMEPAD_B;
    if (b & RP_BTN_X) x |= XUSB_GAMEPAD_X;
    if (b & RP_BTN_Y) x |= XUSB_GAMEPAD_Y;
    if (b & RP_BTN_LSTICK) x |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b & RP_BTN_RSTICK) x |= XUSB_GAMEPAD_RIGHT_THUMB;
    if (b & RP_BTN_L) x |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b & RP_BTN_R) x |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b & RP_BTN_PLUS) x |= XUSB_GAMEPAD_START;
    if (b & RP_BTN_MINUS) x |= XUSB_GAMEPAD_BACK;
    if (b & RP_BTN_DLEFT) x |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b & RP_BTN_DUP) x |= XUSB_GAMEPAD_DPAD_UP;
    if (b & RP_BTN_DRIGHT) x |= XUSB_GAMEPAD_DPAD_RIGHT;
    if (b & RP_BTN_DDOWN) x |= XUSB_GAMEPAD_DPAD_DOWN;
    return x;
}

static void tapVk(WORD vk) {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void handleSpecialKey(uint8_t k) {
    switch (k) {
    case RP_KEY_ENTER: tapVk(VK_RETURN); break;
    case RP_KEY_BACKSPACE: tapVk(VK_BACK); break;
    case RP_KEY_ESCAPE: tapVk(VK_ESCAPE); break;
    case RP_KEY_TAB: tapVk(VK_TAB); break;
    case RP_KEY_UP: tapVk(VK_UP); break;
    case RP_KEY_DOWN: tapVk(VK_DOWN); break;
    case RP_KEY_LEFT: tapVk(VK_LEFT); break;
    case RP_KEY_RIGHT: tapVk(VK_RIGHT); break;
    case RP_KEY_LWIN: tapVk(VK_LWIN); break;
    case RP_KEY_ALTTAB: {
        INPUT in[4]{};
        for (auto& i : in) i.type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_MENU;
        in[1].ki.wVk = VK_TAB;
        in[2].ki.wVk = VK_TAB; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].ki.wVk = VK_MENU; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
        break;
    }
    }
}

static void typeText(const char* utf8, int len) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    if (wlen <= 0) return;
    std::vector<wchar_t> w(wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, w.data(), wlen);
    std::vector<INPUT> ins;
    ins.reserve((size_t)wlen * 2);
    for (wchar_t c : w) {
        INPUT in{}; in.type = INPUT_KEYBOARD;
        in.ki.wScan = c; in.ki.dwFlags = KEYEVENTF_UNICODE;
        ins.push_back(in);
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
        ins.push_back(in);
    }
    SendInput((UINT)ins.size(), ins.data(), sizeof(INPUT));
}

void inputThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(RP_PORT_INPUT);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        logmsg("Girdi portu %d bind edilemedi", RP_PORT_INPUT);
        return;
    }
    vigemInit();
    DWORD timeout = 50;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    char buf[2048];
    uint8_t prevMouse = 0;
    uint32_t lastSeq = 0;
    ULONGLONG lastInput = 0;
    auto releaseInput = [&] {
        if (g_pad) { XUSB_REPORT neutral{}; vigem_target_x360_update(g_vigem, g_pad, neutral); }
        if (prevMouse) {
            INPUT in{}; in.type = INPUT_MOUSE;
            if (prevMouse & 1) in.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
            if (prevMouse & 2) in.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
            SendInput(1, &in, sizeof(in));
        }
        prevMouse = 0; lastSeq = 0; lastInput = 0;
    };
    while (true) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (lastInput && (!g_session.active || GetTickCount64() - lastInput > 250))
            releaseInput();
        if (!g_session.active) continue;
        if (n < (int)sizeof(RpInputPkt)) continue;
        RpInputPkt* pkt = (RpInputPkt*)buf;
        if (pkt->magic != RP_MAGIC) continue;
        if (lastInput && !rp::newerSequence(pkt->seq, lastSeq)) continue;
        lastSeq = pkt->seq;
        lastInput = GetTickCount64();
        g_session.inputPkts++;

        if (g_pad) {
            XUSB_REPORT rep{};
            rep.wButtons = mapButtons(pkt->buttons);
            rep.bLeftTrigger = (pkt->buttons & RP_BTN_ZL) ? 255 : 0;
            rep.bRightTrigger = (pkt->buttons & RP_BTN_ZR) ? 255 : 0;
            rep.sThumbLX = pkt->lx; rep.sThumbLY = pkt->ly;
            rep.sThumbRX = pkt->rx; rep.sThumbRY = pkt->ry;
            vigem_target_x360_update(g_vigem, g_pad, rep);
        }

        if (pkt->mouseDx || pkt->mouseDy) {
            INPUT in{}; in.type = INPUT_MOUSE;
            in.mi.dx = pkt->mouseDx; in.mi.dy = pkt->mouseDy;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof(in));
        }
        uint8_t mb = pkt->mouseButtons, diff = mb ^ prevMouse;
        if (diff) {
            INPUT in{}; in.type = INPUT_MOUSE;
            if (diff & 1) { in.mi.dwFlags = (mb & 1) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; SendInput(1, &in, sizeof(in)); }
            if (diff & 2) { in.mi.dwFlags = (mb & 2) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; SendInput(1, &in, sizeof(in)); }
            prevMouse = mb;
        }
        if (pkt->wheel) {
            INPUT in{}; in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = (DWORD)(pkt->wheel * WHEEL_DELTA);
            SendInput(1, &in, sizeof(in));
        }
        if (pkt->specialKey) handleSpecialKey(pkt->specialKey);
        if (pkt->textLen && n >= (int)(sizeof(RpInputPkt) + pkt->textLen))
            typeText(buf + sizeof(RpInputPkt), pkt->textLen);
    }
}
