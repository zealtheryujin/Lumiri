#include "../switch-client/source/menu.h"
#include <cstdio>
#include <cstring>
#include <string>

SDL_Renderer* g_ren = nullptr;
Settings g_cfg;

int main() {
    if (SDL_Init(0) != 0) return 1;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return 1;
    g_ren = SDL_CreateSoftwareRenderer(surface);
    if (!g_ren || !uiInit()) return 1;
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    auto save = [&](const char* name) {
        SDL_RenderPresent(g_ren);
        std::string path = std::string("out/ui-") + name + ".bmp";
        if (SDL_SaveBMP(surface, path.c_str()) != 0) { fprintf(stderr, "%s\n", SDL_GetError()); exit(1); }
    };
    std::vector<MenuTile> tiles = {
        {"Desktop", "Open your PC screen", nullptr, 0}, {"Add a game", "Grow your library", nullptr, 1},
        {"Hollow Knight", "Steam", nullptr, 2}, {"Stardew Valley", "Steam", nullptr, 2},
        {"Forza Horizon 5", "Custom game", nullptr, 2}, {"Hades II", "Steam", nullptr, 2},
        {"EA SPORTS FC 26", "Custom game", nullptr, 2}, {"Celeste", "Steam", nullptr, 2},
        {"Sea of Stars", "Steam", nullptr, 2}, {"A very long game title that should truncate safely", "Epic", nullptr, 2}
    };
    drawLibrary(tiles, 0, 0, "Living room PC", "192.168.1.20", false, 1); save("library");
    drawLibrary(tiles, 0, 0, nullptr, "", false, 0); save("search");
    const char* names[] = {"Resolution","Frame Rate","Bitrate","Audio","Performance","H.264 Decoder","Mouse Sensitivity","OC Control","CPU Limit","GPU Limit","RAM Limit","Language / Dil","Theme"};
    char values[13][32] = {"720p","60 fps","8 Mbps","On","Maximum","NVDEC","5","System / external","1785 MHz","768 MHz","2133 MHz","English","Light"};
    drawPreferences(names, values, 0, false); save("settings");
    drawPreferences(names, values, 10, true); save("clocks");
    g_english = false;
    const char* turkishNames[] = {"Çözünürlük","Kare Hızı","Bit Hızı","Ses","Performans","H.264 Çözücü","Fare Hassasiyeti","OC Kontrolü","CPU Tavanı","GPU Tavanı","RAM Tavanı","Language / Dil","Theme"};
    SDL_Delay(120);
    drawPreferences(turkishNames, values, 5, true); save("turkish");
    std::vector<RpBrowseEntry> entries(6);
    for (int i = 0; i < 6; ++i) {
        snprintf(entries[i].name, sizeof(entries[i].name), "Örnek klasör %d - uzun dosya ve klasör adları", i+1);
        entries[i].type = RP_FS_DIR;
    }
    drawFolder(entries, 0, 0, "C:\\Games\\My collection", false); save("folder");
    drawConnectionState("Bağlanamadık.", "Host bağlantısı kurulamadı. Bilgisayarın ve Switch'in aynı ağa bağlı olduğundan emin ol. Uzun hata mesajları bu alanda satırlara bölünür.", true); save("error");
    g_english = true;
    const char* actions[] = {"Resume","Open Keyboard","Enter","Esc","Alt+Tab","Windows Key","Statistics: On","Switch Monitor","Stream Settings","Disconnect"};
    fillRect(0,0,1280,720,SDL_Color{40,57,67,255});
    drawSessionMenu(actions, 7, 10);
    for (int i = 0; i < 12; ++i) {
        SDL_Delay(16);
        fillRect(0,0,1280,720,SDL_Color{40,57,67,255});
        drawSessionMenu(actions, 7, 10);
    }
    save("session");
    theme::apply(true);
    drawLibrary(tiles, 0, 0, "Living room PC", "192.168.1.20", false, 1); save("dark-library");
    snprintf(values[12], 32, "Dark");
    SDL_Delay(120);
    drawPreferences(names, values, 12, true); save("dark-settings");
    drawConnectionState("Could not connect.", "Check that the PC is online and on the same network.", true); save("dark-error");
    theme::apply(false);
    drawPreferences(names, values, 12, false); save("light-restored");
    uiExit(); SDL_DestroyRenderer(g_ren); SDL_FreeSurface(surface); SDL_Quit();
    puts("PASS: twelve screen previews including light/dark themes, English/Turkish and long text");
    return 0;
}
