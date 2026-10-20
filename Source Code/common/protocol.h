#pragma once
#include <stdint.h>

#define RP_PROTO_VERSION  3
#define RP_MAGIC          0x52504C59u
#define RP_MAGIC_GAMES    0x5250474Cu

#define RP_PORT_DISCOVERY 47799
#define RP_PORT_CONTROL   47800
#define RP_PORT_MEDIA     47801
#define RP_PORT_INPUT     47802

#pragma pack(push, 1)

typedef struct { uint32_t magic; uint32_t version; } RpDiscoverReq;
typedef struct { uint32_t magic; uint32_t version; char hostname[32]; } RpDiscoverResp;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t launchGame;
    uint32_t bitrateKbps;
    uint8_t  audioEnabled;
    uint8_t  _pad2[3];
} RpHandshake;

typedef struct {
    uint32_t magic;
    uint32_t ok;
    uint32_t audioRate;
    uint16_t audioChannels;
    uint16_t _pad;
} RpHandshakeAck;

#define RP_GAME_NONE 0xFFFF
enum { RP_GAME_STEAM = 0, RP_GAME_EPIC = 1, RP_GAME_CUSTOM = 2 };

#define RP_ICON_SIZE 64

typedef struct { uint32_t magic; uint32_t version; } RpGamesReq;
typedef struct { uint16_t count; uint16_t _pad; } RpGamesRespHdr;

typedef struct { uint8_t type; uint8_t hasIcon; char name[64]; uint8_t _pad[2]; } RpGameEntry;

#define RP_MAGIC_BROWSE  0x52504252u
#define RP_MAGIC_ADDGAME 0x52504147u
enum { RP_FS_DIR = 0, RP_FS_EXE = 1 };

typedef struct { uint32_t magic; uint32_t version; char path[260]; } RpBrowseReq;
typedef struct { uint16_t count; uint16_t _pad; } RpBrowseRespHdr;
typedef struct { uint8_t type; char name[128]; uint8_t _pad[3]; } RpBrowseEntry;
typedef struct { uint32_t magic; uint32_t version; char name[64]; char path[260]; } RpAddGameReq;

enum {
    RP_CTRL_IDR  = 1,
    RP_CTRL_PING = 2,
    RP_CTRL_PONG = 3,
    RP_CTRL_BYE  = 4,
    RP_CTRL_NEXT_MONITOR = 5,
};

#define RP_CH_VIDEO 0
#define RP_CH_AUDIO 1
#define RP_FLAG_KEYFRAME 0x01
#define RP_FLAG_PARITY   0x02
#define RP_MAX_PAYLOAD 1400
#define RP_FEC_GROUP 4

typedef struct {
    uint8_t  channel;
    uint8_t  flags;
    uint16_t partIdx;
    uint16_t partCount;
    uint16_t payloadLen;
    uint32_t frameId;
    uint32_t frameBytes;
} RpMediaHdr;

enum {
    RP_BTN_A      = 1u << 0,  RP_BTN_B      = 1u << 1,
    RP_BTN_X      = 1u << 2,  RP_BTN_Y      = 1u << 3,
    RP_BTN_LSTICK = 1u << 4,  RP_BTN_RSTICK = 1u << 5,
    RP_BTN_L      = 1u << 6,  RP_BTN_R      = 1u << 7,
    RP_BTN_ZL     = 1u << 8,  RP_BTN_ZR     = 1u << 9,
    RP_BTN_PLUS   = 1u << 10, RP_BTN_MINUS  = 1u << 11,
    RP_BTN_DLEFT  = 1u << 12, RP_BTN_DUP    = 1u << 13,
    RP_BTN_DRIGHT = 1u << 14, RP_BTN_DDOWN  = 1u << 15,
};

enum {
    RP_KEY_NONE = 0, RP_KEY_ENTER, RP_KEY_BACKSPACE, RP_KEY_ESCAPE,
    RP_KEY_TAB, RP_KEY_UP, RP_KEY_DOWN, RP_KEY_LEFT, RP_KEY_RIGHT,
    RP_KEY_LWIN, RP_KEY_ALTTAB,
};

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t buttons;
    int16_t  lx, ly, rx, ry;
    int16_t  mouseDx, mouseDy;
    uint8_t  mouseButtons;
    int8_t   wheel;
    uint8_t  specialKey;
    uint8_t  _pad;
    uint16_t textLen;
} RpInputPkt;

#pragma pack(pop)
