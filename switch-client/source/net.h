#pragma once
#include "app.h"

bool netInit();
void netExit();

void discoveryStart();
void discoveryStop();
void discoveryTick(std::vector<HostInfo>& hosts);

struct GameItem {
    uint8_t type = 0;
    char name[64] = {};
    std::vector<uint8_t> icon;
};
bool fetchGames(const char* ip, std::vector<GameItem>& out);
bool browsePath(const char* ip, const char* path, std::vector<RpBrowseEntry>& out);
bool addCustomGame(const char* ip, const char* name, const char* path);

bool sessionConnect(const char* ip, RpHandshakeAck& ackOut, char* errBuf, size_t errLen,
                    uint16_t launchGame = RP_GAME_NONE);
void sessionDisconnect();
void sessionStart();
bool sessionAlive();

void sendCtrl(uint8_t type);
void sessionResync();
void sendPing();
int  getRttMs();
uint32_t audioQueueMs();

struct NetStats {
    std::atomic<uint64_t> videoBytes{0};
    std::atomic<uint32_t> videoFrames{0};
    std::atomic<uint32_t> droppedFrames{0};
};
extern NetStats g_netStats;

extern SDL_AudioDeviceID g_audioDev;

void sendInput(const RpInputPkt& pkt, const char* text);
