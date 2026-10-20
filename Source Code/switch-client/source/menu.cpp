#include "menu.h"
#include <algorithm>
#include <cmath>

static void monitorIcon(int x, int y, int size, SDL_Color color) {
    fillRoundedRect(x, y, size, size * 2 / 3, 9, color);
    fillRoundedRect(x + 4, y + 4, size - 8, size * 2 / 3 - 8, 6, theme::card);
    fillRect(x + size / 2 - 3, y + size * 2 / 3, 6, size / 6, color);
    fillRoundedRect(x + size / 4, y + size * 5 / 6, size / 2, 5, 2, color);
}

static void tileArt(const MenuTile& tile, int x, int y, int size) {
    if (tile.icon) {
        SDL_Rect dst{x, y, size, size};
        SDL_RenderCopy(g_ren, tile.icon, nullptr, &dst);
    } else if (tile.kind == 0) monitorIcon(x + 8, y + 12, size - 16, theme::accent);
    else if (tile.kind == 1) {
        fillRoundedRect(x + 12, y + 12, size - 24, size - 24, 20, theme::cardSel);
        fillRoundedRect(x + size / 2 - 2, y + 28, 4, size - 56, 2, theme::accent);
        fillRoundedRect(x + 28, y + size / 2 - 2, size - 56, 4, 2, theme::accent);
    } else {
        fillRoundedRect(x, y, size, size, 22, theme::cardSel);
        char initial[5]{};
        int length = 1;
        unsigned char lead = static_cast<unsigned char>(tile.name[0]);
        if (lead >= 0xF0) length = 4; else if (lead >= 0xE0) length = 3; else if (lead >= 0xC0) length = 2;
        for (int i = 0; i < length && tile.name[i]; ++i) initial[i] = tile.name[i];
        drawText(x + (size - textWidth(FS_BIG, initial)) / 2, y + (size - 56) / 2, FS_BIG, theme::accent, "%s", initial);
    }
}

void drawLibrary(const std::vector<MenuTile>& tiles, int selected, int top,
                 const char* host, const char* ip, bool loading, int hostCount) {
    drawBackground();
    drawHeader(tr("KÜTÜPHANE", "LIBRARY"));
    if (!host) {
        fillRoundedRect(456, 193, 368, 220, 36, theme::card);
        monitorIcon(588, 228, 104, theme::accent);
        for (int i = 0; i < 3; ++i) {
            int active = int(nowMs() / 350) % 3;
            fillRoundedRect(615 + i * 20, 367, 8, 8, 4, i == active ? theme::accent : theme::cardSel);
        }
        const char* title = tr("PC'n seni bekliyor.", "Your PC. Anywhere at home.");
        drawText((1280 - textWidth(FS_TITLE, title)) / 2, 440, FS_TITLE, theme::text, "%s", title);
        const char* detail = tr("PC'de Lumiri'yi aç. Aynı ağda otomatik buluşalım.", "Open Lumiri on your PC. We will find it on the same network.");
        drawText((1280 - textWidth(FS_BODY, detail)) / 2, 493, FS_BODY, theme::dim, "%s", detail);
        drawHints(tr("(X) IP Gir   (Y) Ayarlar   (+) Çıkış", "(X) Enter IP   (Y) Settings   (+) Exit"));
        return;
    }
    const char* selectedName = tiles.empty() ? "Lumiri" : tiles[selected].name;
    drawClippedText(56, 104, 735, FS_BIG, theme::text, selectedName);
    drawText(58, 166, FS_SMALL, theme::dim, "%s  /  %d %s", tr("OYNA", "PLAY"), std::max(0, int(tiles.size()) - 2), tr("oyun", "games"));
    fillRoundedRect(864, 110, 360, 80, 22, theme::card);
    fillRoundedRect(886, 133, 8, 8, 4, theme::accent);
    drawClippedText(906, 122, 290, FS_BODY, theme::text, host);
    drawText(906, 155, FS_SMALL, theme::dim, "%s%s", ip, hostCount > 1 ? "   L / R" : "");
    const int first = top * 5;
    for (int i = first; i < int(tiles.size()) && i < first + 10; ++i) {
        int x = 56 + (i % 5) * 236, y = 224 + (i / 5 - top) * 206;
        fillRoundedRect(x, y + 4, 224, 190, 24, theme::shadow);
        fillRoundedRect(x, y, 224, 190, 24, i == selected ? theme::cardSel : theme::card);
        tileArt(tiles[i], x + 70, y + 16, 84);
        drawClippedText(x + 18, y + 116, 188, FS_BODY, theme::text, tiles[i].name);
        drawClippedText(x + 18, y + 152, 188, FS_SMALL, theme::dim, tiles[i].badge);
    }
    if (!tiles.empty()) drawFocus(0, 56 + (selected % 5) * 236, 224 + (selected / 5 - top) * 206, 224, 190, 24);
    if (loading) drawText(57, 635, FS_SMALL, theme::dim, tr("Kütüphane yenileniyor…", "Refreshing library…"));
    else if (tiles.size() > 10) drawText(57, 635, FS_SMALL, theme::dim, "%d / %d", selected + 1, int(tiles.size()));
    drawHints(tr("(A) Başlat   (X) IP Gir   (Y) Ayarlar   (-) Yenile   (+) Çıkış", "(A) Play   (X) Enter IP   (Y) Settings   (-) Refresh   (+) Exit"));
}

void drawPreferences(const char* const* names, const char values[][32], int selected, bool streaming) {
    drawBackground(); drawHeader(tr("AYARLAR", "SETTINGS"));
    const char* groups[] = {tr("Görüntü ve ses", "Picture & sound"), tr("Deneyim", "Experience"), tr("Frekanslar", "Clock limits"), tr("Görünüm ve dil", "Appearance & language")};
    const int starts[] = {0,4,7,11,13};
    int group = selected < 4 ? 0 : selected < 7 ? 1 : selected < 11 ? 2 : 3;
    drawText(56, 112, FS_BIG, theme::text, tr("Sana göre.", "Make it yours."));
    drawText(59, 177, FS_SMALL, theme::dim, tr("L / R ile bölüm değiştir", "L / R to switch section"));
    for (int i = 0; i < 4; ++i) {
        int y = 256 + i * 72;
        if (i == group) fillRoundedRect(48, y, 320, 60, 18, theme::cardSel);
        drawText(70, y + 15, FS_BODY, i == group ? theme::accent : theme::dim, "%s", groups[i]);
    }
    drawText(424, 120, FS_TITLE, theme::text, "%s", groups[group]);
    drawText(1172, 130, FS_SMALL, theme::dim, "%02d", group + 1);
    for (int i = starts[group]; i < starts[group + 1]; ++i) {
        int y = 190 + (i - starts[group]) * 92;
        bool active = i == selected;
        fillRoundedRect(416, y, 808, 78, 20, active ? theme::cardSel : theme::card);
        drawText(440, y + 25, FS_BODY, theme::text, "%s", names[i]);
        int valueWidth = textWidth(FS_BODY, values[i]);
        drawText(1162 - valueWidth, y + 25, FS_BODY, active ? theme::accent : theme::dim, "%s", values[i]);
        if (active) {
            drawText(1138 - valueWidth, y + 25, FS_BODY, theme::accent, "‹");
            drawText(1186, y + 25, FS_BODY, theme::accent, "›");
        }
    }
    drawFocus(1, 416, 190 + (selected - starts[group]) * 92, 808, 78);
    const char* help = tr("Yukarı / aşağı ile seç. Sol / sağ ile değiştir.", "Choose with Up / Down. Adjust with Left / Right.");
    if (selected == 5) help = tr("Yayın her zaman H.264. NVDEC: Switch donanımı; CPU: yazılım. Donanım açılamazsa yazılıma dönülür.", "Video is always H.264. NVDEC uses Switch hardware; CPU uses software. Falls back to software if hardware is unavailable.");
    if (selected == 7) help = tr("Sistem / harici seçilirse saatler uygulama tarafından ayarlanmaz.", "System / external leaves clock management to the system.");
    if (selected == 8 || selected == 9) help = tr("Bu bir üst sınırdır. Dengeli mod daha düşük bir hız kullanabilir.", "This is an upper limit. Balanced mode may use a lower clock.");
    if (selected == 10) help = tr("Sistemin desteklediği ve bu sınırı aşmayan en yüksek RAM hızı kullanılır.", "Uses the highest system-supported RAM clock within this limit.");
    drawWrappedText(426, 586, 780, 2, FS_SMALL, theme::dim, help);
    drawHints(streaming ? tr("(B) Uygula   (X) İptal", "(B) Apply   (X) Cancel") : tr("(B) Kaydet ve Geri", "(B) Save & Back"));
}

void drawFolder(const std::vector<RpBrowseEntry>& entries, int selected, int top, const char* path, bool failed) {
    drawBackground(); drawHeader(tr("OYUN EKLE", "ADD GAME"));
    drawText(56, 110, FS_BIG, theme::text, tr("Kütüphaneni büyüt.", "Room for one more."));
    drawClippedText(59, 175, 1120, FS_SMALL, theme::dim, path[0] ? path : tr("PC sürücüleri", "PC drives"));
    if (failed || entries.empty()) drawWrappedText(60, 320, 1100, 3, FS_BODY, failed ? theme::danger : theme::dim,
        failed ? tr("Klasör açılamadı veya oyun eklenemedi. Geri dönüp tekrar dene.", "Could not open the folder or add the game. Go back and try again.") : tr("Bu klasörde alt klasör veya .exe bulunamadı.", "No subfolders or .exe files here."));
    for (int i = top; i < int(entries.size()) && i < top + 6; ++i) {
        int y = 224 + (i - top) * 66;
        fillRoundedRect(56, y, 1168, 56, 16, i == selected ? theme::cardSel : theme::card);
        drawText(78, y + 13, FS_BODY, theme::accent, entries[i].type == RP_FS_DIR ? "+" : "›");
        drawClippedText(120, y + 13, 1060, FS_BODY, theme::text, entries[i].name);
    }
    if (!entries.empty()) drawFocus(2, 56, 224 + (selected - top) * 66, 1168, 56, 16);
    drawHints(tr("(A) Aç / Seç   (B) Geri", "(A) Open / Select   (B) Back"));
}

void drawConnectionState(const char* title, const char* detail, bool error) {
    drawBackground(); drawHeader(tr("BAĞLANTI", "CONNECTION"));
    fillRoundedRect(160, 204, 960, 328, 32, theme::card);
    fillRoundedRect(208, 250, 60, 60, 20, error ? theme::errorBg : theme::cardSel);
    drawText(229, 258, FS_TITLE, error ? theme::danger : theme::accent, error ? "!" : "›");
    drawClippedText(302, 258, 755, FS_TITLE, theme::text, title);
    drawWrappedText(208, 353, 848, 4, FS_BODY, theme::dim, detail);
    if (error) drawHints(tr("(A) / (B) Geri", "(A) / (B) Back"));
}

void drawSessionMenu(const char* const* labels, int selected, int count) {
    static uint64_t last = 0, opened = 0;
    uint64_t now = nowMs();
    if (!last || now - last > 100) opened = now;
    last = now;
    float progress = std::min(1.f, float(now - opened) / 160.f);
    float remaining = 1.f - progress;
    int offset = int(64.f * remaining * remaining * remaining);
    fillRect(0, 0, 1280, 720, SDL_Color{15,30,31,115});
    SDL_Rect viewport{offset, 0, 1280, 720};
    SDL_RenderSetViewport(g_ren, &viewport);
    fillRoundedRect(752, 20, 508, 680, 30, theme::bgTop);
    drawText(790, 48, FS_TITLE, theme::text, tr("Kısa bir mola.", "Take a moment."));
    drawText(792, 95, FS_SMALL, theme::dim, tr("Yayın kontrolleri", "Session controls"));
    for (int i = 0; i < count; ++i) {
        int y = 146 + i * 48;
        if (i == selected) fillRoundedRect(774, y, 464, 44, 14, theme::cardSel);
        drawText(794, y + 9, FS_BODY, i == count - 1 ? theme::danger : (i == selected ? theme::accent : theme::text), "%s", labels[i]);
        if (i == selected) drawText(1200, y + 9, FS_BODY, theme::accent, "›");
    }
    drawFocus(3, 774, 146 + selected * 48, 464, 44, 14);
    drawText(792, 654, FS_SMALL, theme::dim, tr("(A) Seç    (B) Devam et", "(A) Select    (B) Resume"));
    SDL_RenderSetViewport(g_ren, nullptr);
}
