#include "net.h"
#include "decoder.h"
#include "../../common/stream_utils.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <thread>
#include <cstdio>
#include <cerrno>

NetStats g_netStats;
SDL_AudioDeviceID g_audioDev = 0;

static int g_discSock = -1;
static int g_ctrlSock = -1;
static int g_mediaSock = -1;
static int g_inputSock = -1;
static sockaddr_in g_hostAddr{};
static std::atomic<bool> g_running{false};
static std::thread g_mediaThread, g_ctrlThread;
static std::atomic<int> g_rttMs{-1};
static std::atomic<uint64_t> g_pingSentAt{0};
static uint64_t g_lastBroadcast = 0;
static uint32_t g_audioBytesPerMs = 192;

bool netInit() {

    static const SocketInitConfig cfg = {
        .tcp_tx_buf_size     = 0x8000,
        .tcp_rx_buf_size     = 0x10000,
        .tcp_tx_buf_max_size = 0x40000,
        .tcp_rx_buf_max_size = 0x40000,
        .udp_tx_buf_size     = 0x10000,
        .udp_rx_buf_size     = 0x100000,
        .sb_efficiency       = 4,
    };
    if (R_FAILED(socketInitialize(&cfg))) return false;
    nifmInitialize(NifmServiceType_User);
    return true;
}
void netExit() { nifmExit(); socketExit(); }

void discoveryStart() {
    if (g_discSock >= 0) return;
    g_discSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1;
    setsockopt(g_discSock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    fcntl(g_discSock, F_SETFL, O_NONBLOCK);
    g_lastBroadcast = 0;
}

void discoveryStop() {
    if (g_discSock >= 0) { close(g_discSock); g_discSock = -1; }
}

void discoveryTick(std::vector<HostInfo>& hosts) {
    if (g_discSock < 0) return;
    uint64_t now = nowMs();
    if (now - g_lastBroadcast > 1000) {
        g_lastBroadcast = now;
        RpDiscoverReq req{ RP_MAGIC, RP_PROTO_VERSION };
        sockaddr_in b{};
        b.sin_family = AF_INET;
        b.sin_port = htons(RP_PORT_DISCOVERY);
        b.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(g_discSock, &req, sizeof(req), 0, (sockaddr*)&b, sizeof(b));

        u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2)) && ip && mask) {
            b.sin_addr.s_addr = ip | ~mask;
            sendto(g_discSock, &req, sizeof(req), 0, (sockaddr*)&b, sizeof(b));
        }
    }
    char buf[128];
    sockaddr_in from{}; socklen_t flen = sizeof(from);
    int n;
    while ((n = recvfrom(g_discSock, buf, sizeof(buf), 0, (sockaddr*)&from, &flen)) > 0) {
        if (n < (int)sizeof(RpDiscoverResp)) continue;
        RpDiscoverResp* r = (RpDiscoverResp*)buf;
        if (r->magic != RP_MAGIC) continue;
        char ip[16];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        bool found = false;
        for (auto& h : hosts)
            if (!strcmp(h.ip, ip)) { h.lastSeenMs = now; found = true; }
        if (!found && hosts.size() < 8) {
            HostInfo h{};
            strncpy(h.name, r->hostname, 32);
            strncpy(h.ip, ip, 15);
            h.lastSeenMs = now;
            hosts.push_back(h);
        }
    }

    for (auto it = hosts.begin(); it != hosts.end();)
        it = (now - it->lastSeenMs > 5000) ? hosts.erase(it) : it + 1;
}

static std::atomic<bool> g_resyncReq{false};
void sessionResync() { g_resyncReq = true; }

static void mediaLoop() {
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
    std::vector<uint8_t> frame;
    std::vector<bool> have;
    std::vector<uint8_t> parity;
    std::vector<bool> parityHave;
    uint32_t curId = 0xFFFFFFFF;
    uint16_t parts = 0, got = 0;
    uint32_t frameBytes = 0;
    uint8_t curFlags = 0;
    bool waitingKey = true;
    uint32_t lastAudioId = 0;
    uint64_t lastRecoveryAt = 0;
    uint64_t lastMediaAt = nowMs();

    constexpr unsigned BATCH = 16;
    alignas(16) uint8_t bufs[BATCH][sizeof(RpMediaHdr) + RP_MAX_PAYLOAD];
    iovec iov[BATCH]{};
    mmsghdr msgs[BATCH]{};
    for (unsigned i = 0; i < BATCH; ++i) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = sizeof(bufs[i]);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    auto recoverGroup = [&](size_t g) {
        if (g >= parityHave.size() || !parityHave[g] || got >= parts) return;
        int missing = rp::recoverFecGroup(frame.data(), parity.data() + g * RP_MAX_PAYLOAD,
                                         have, parts, g);
        if (missing < 0) return;
        have[missing] = true;
        ++got;
    };

    while (g_running) {
        if (g_resyncReq.exchange(false)) {

            pollfd dp{ g_mediaSock, POLLIN, 0 };
            for (int discarded = 0; discarded < 4096 && g_running && poll(&dp, 1, 0) > 0; ++discarded)
                if (recv(g_mediaSock, bufs[0], sizeof(bufs[0]), MSG_DONTWAIT) <= 0) break;
            parts = 0; got = 0; curId = 0xFFFFFFFF;
            waitingKey = true;
            decoderFlush();
            if (g_audioDev) {
                SDL_PauseAudioDevice(g_audioDev, 1);
                SDL_ClearQueuedAudio(g_audioDev);
            }
            sendCtrl(RP_CTRL_IDR);
        }

        uint64_t tick = nowMs();
        if (waitingKey && (!lastRecoveryAt || tick - lastRecoveryAt >= 250)) {
            lastRecoveryAt = tick;
            sendCtrl(RP_CTRL_IDR);
        }
        pollfd ready{g_mediaSock, POLLIN, 0};
        int polled = poll(&ready, 1, 20);
        if (polled < 0) { if (errno == EINTR) continue; g_running = false; break; }
        if (!polled) {
            if (tick - lastMediaAt > 5000) g_running = false;
            continue;
        }
        if (!(ready.revents & POLLIN)) { g_running = false; break; }
        int count = recvmmsg(g_mediaSock, msgs, BATCH, MSG_DONTWAIT, nullptr);
        if (count <= 0) continue;
        for (int mi = 0; mi < count; ++mi) {
            int n = (int)msgs[mi].msg_len;
            msgs[mi].msg_len = 0;
            uint8_t* buf = bufs[mi];
            if (n < (int)sizeof(RpMediaHdr)) continue;
            RpMediaHdr* h = (RpMediaHdr*)buf;
            if (h->payloadLen > RP_MAX_PAYLOAD ||
                n < (int)(sizeof(RpMediaHdr) + h->payloadLen)) continue;
            const uint8_t* payload = buf + sizeof(RpMediaHdr);
            lastMediaAt = nowMs();

            if (h->channel == RP_CH_AUDIO) {
                if (g_audioDev) {
                    if (lastAudioId && !rp::newerSequence(h->frameId, lastAudioId)) continue;
                    if (h->partCount != 1 || h->partIdx != 0 || h->payloadLen % 4) continue;
                    lastAudioId = h->frameId;

                    uint32_t queued = SDL_GetQueuedAudioSize(g_audioDev);
                    if (queued > g_audioBytesPerMs * 120 || !queued) {
                        SDL_PauseAudioDevice(g_audioDev, 1);
                        if (queued) SDL_ClearQueuedAudio(g_audioDev);
                    }
                    SDL_QueueAudio(g_audioDev, payload, h->payloadLen);
                    if (SDL_GetAudioDeviceStatus(g_audioDev) == SDL_AUDIO_PAUSED &&
                        SDL_GetQueuedAudioSize(g_audioDev) >= g_audioBytesPerMs * 40)
                        SDL_PauseAudioDevice(g_audioDev, 0);
                }
                continue;
            }
            if (h->channel != RP_CH_VIDEO) continue;
            g_netStats.videoBytes += n;

            if (h->frameId != curId) {

                if (curId != 0xFFFFFFFF && !rp::newerSequence(h->frameId, curId)) continue;
                if (parts && got < parts) {
                    g_netStats.droppedFrames++;
                    waitingKey = true;
                    decoderFlush();
                    uint64_t now = nowMs();
                    if (!lastRecoveryAt || now - lastRecoveryAt >= 250) {
                        lastRecoveryAt = now;
                        sendCtrl(RP_CTRL_IDR);
                    }
                }
                curId = h->frameId;
                parts = h->partCount; got = 0;
                frameBytes = h->frameBytes;
                curFlags = h->flags & RP_FLAG_KEYFRAME;
                if (!rp::validVideoLayout(frameBytes, parts)) {
                    parts = 0;
                    continue;
                }
                frame.assign((size_t)parts * RP_MAX_PAYLOAD, 0);
                have.assign(parts, false);
                size_t ng = (parts + RP_FEC_GROUP - 1) / RP_FEC_GROUP;
                parity.assign(ng * RP_MAX_PAYLOAD, 0);
                parityHave.assign(ng, false);
            }
            if (!parts || h->partCount != parts || h->frameBytes != frameBytes) continue;

            size_t group = 0;
            if (h->flags & RP_FLAG_PARITY) {
                group = h->partIdx;
                if (group < parityHave.size() && !parityHave[group] &&
                    h->payloadLen == RP_MAX_PAYLOAD) {
                    memcpy(parity.data() + group * RP_MAX_PAYLOAD,
                           payload, RP_MAX_PAYLOAD);
                    parityHave[group] = true;
                }
            } else {
                if (h->partIdx >= parts || have[h->partIdx]) continue;
                uint32_t off = (uint32_t)h->partIdx * RP_MAX_PAYLOAD;
                uint16_t expected = (uint16_t)((frameBytes - off) > RP_MAX_PAYLOAD
                    ? RP_MAX_PAYLOAD : frameBytes - off);
                if (h->payloadLen != expected) continue;
                memcpy(frame.data() + off, payload, expected);
                have[h->partIdx] = true;
                ++got;
                group = h->partIdx / RP_FEC_GROUP;
            }
            recoverGroup(group);

            if (got == parts) {
                if (waitingKey && !(curFlags & RP_FLAG_KEYFRAME)) {
                    g_netStats.droppedFrames++;
                } else {
                    waitingKey = false;
                    g_netStats.videoFrames++;
                    frame.resize(frameBytes);
                    if (!decoderPush(std::move(frame), (curFlags & RP_FLAG_KEYFRAME) != 0)) {

                        g_netStats.droppedFrames++;
                        waitingKey = true;
                        decoderFlush();
                        uint64_t now = nowMs();
                        if (!lastRecoveryAt || now - lastRecoveryAt >= 250) {
                            lastRecoveryAt = now;
                            sendCtrl(RP_CTRL_IDR);
                        }
                    }
                    frame.clear();
                }
                parts = 0; got = 0;
            }
        }
    }
}

static void ctrlLoop() {
    char c;
    uint64_t lastPing = 0, lastReply = nowMs();
    while (g_running) {
        uint64_t tick = nowMs();
        if (tick - lastPing >= 1000) { sendPing(); lastPing = tick; }
        if (tick - lastReply > 10000) { g_running = false; break; }
        pollfd pf{ g_ctrlSock, POLLIN, 0 };
        int r = poll(&pf, 1, 200);
        if (r < 0) { if (errno == EINTR) continue; g_running = false; break; }
        if (r == 0) continue;
        if (recv(g_ctrlSock, &c, 1, 0) != 1) { g_running = false; break; }
        lastReply = nowMs();
        if (c == RP_CTRL_PONG) {
            uint64_t s = g_pingSentAt.exchange(0);
            if (s) g_rttMs = (int)(nowMs() - s);
        }
    }
}

void sendCtrl(uint8_t type) {
    if (g_ctrlSock >= 0) send(g_ctrlSock, &type, 1, 0);
}

void sendPing() {
    uint64_t expected = 0;
    if (g_pingSentAt.compare_exchange_strong(expected, nowMs())) {
        sendCtrl(RP_CTRL_PING);
    }
}

int getRttMs() { return g_rttMs; }
uint32_t audioQueueMs() {
    return g_audioDev && g_audioBytesPerMs ? SDL_GetQueuedAudioSize(g_audioDev) / g_audioBytesPerMs : 0;
}

static bool recvAllTimeout(int s, char* buf, int len, int timeoutMs) {
    while (len > 0) {
        pollfd pf{ s, POLLIN, 0 };
        if (poll(&pf, 1, timeoutMs) <= 0) return false;
        int n = recv(s, buf, len, 0);
        if (n <= 0) return false;
        buf += n; len -= n;
    }
    return true;
}

static int tcpConnect(const char* ip, int timeoutMs) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(RP_PORT_CONTROL);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -1; }
    fcntl(s, F_SETFL, O_NONBLOCK);
    connect(s, (sockaddr*)&a, sizeof(a));
    pollfd pf{ s, POLLOUT, 0 };
    if (poll(&pf, 1, timeoutMs) <= 0) { close(s); return -1; }
    int soErr = 0; socklen_t sl = sizeof(soErr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &soErr, &sl);
    if (soErr) { close(s); return -1; }
    fcntl(s, F_SETFL, 0);
    return s;
}

bool fetchGames(const char* ip, std::vector<GameItem>& out) {
    int s = tcpConnect(ip, 2000);
    if (s < 0) return false;

    RpGamesReq req{ RP_MAGIC_GAMES, RP_PROTO_VERSION };
    send(s, &req, sizeof(req), 0);
    RpGamesRespHdr hdr{};
    if (!recvAllTimeout(s, (char*)&hdr, sizeof(hdr), 3000) || hdr.count > 512) {
        close(s); return false;
    }
    std::vector<RpGameEntry> ents(hdr.count);
    if (hdr.count &&
        !recvAllTimeout(s, (char*)ents.data(), (int)(hdr.count * sizeof(RpGameEntry)), 5000)) {
        close(s);
        return false;
    }
    out.clear();
    out.resize(hdr.count);
    for (uint16_t i = 0; i < hdr.count; i++) {
        out[i].type = ents[i].type;
        memcpy(out[i].name, ents[i].name, 64);
        out[i].name[63] = 0;
    }

    const int iconBytes = RP_ICON_SIZE * RP_ICON_SIZE * 4;
    for (uint16_t i = 0; i < hdr.count; i++) {
        if (!ents[i].hasIcon) continue;
        out[i].icon.resize(iconBytes);
        if (!recvAllTimeout(s, (char*)out[i].icon.data(), iconBytes, 5000)) {
            out[i].icon.clear();
            break;
        }
    }
    close(s);
    return true;
}

bool browsePath(const char* ip, const char* path, std::vector<RpBrowseEntry>& out) {
    int s = tcpConnect(ip, 2000);
    if (s < 0) return false;
    RpBrowseReq req{};
    req.magic = RP_MAGIC_BROWSE;
    req.version = RP_PROTO_VERSION;
    strncpy(req.path, path, sizeof(req.path) - 1);
    send(s, &req, sizeof(req), 0);
    RpBrowseRespHdr hdr{};
    if (!recvAllTimeout(s, (char*)&hdr, sizeof(hdr), 4000) || hdr.count > 512) {
        close(s);
        return false;
    }
    out.assign(hdr.count, {});
    if (hdr.count &&
        !recvAllTimeout(s, (char*)out.data(), (int)(hdr.count * sizeof(RpBrowseEntry)), 6000)) {
        out.clear();
        close(s);
        return false;
    }
    close(s);
    return true;
}

bool addCustomGame(const char* ip, const char* name, const char* path) {
    int s = tcpConnect(ip, 2000);
    if (s < 0) return false;
    RpAddGameReq req{};
    req.magic = RP_MAGIC_ADDGAME;
    req.version = RP_PROTO_VERSION;
    strncpy(req.name, name, sizeof(req.name) - 1);
    strncpy(req.path, path, sizeof(req.path) - 1);
    send(s, &req, sizeof(req), 0);
    uint8_t ok = 0;
    recvAllTimeout(s, (char*)&ok, 1, 3000);
    close(s);
    return ok == 1;
}

bool sessionConnect(const char* ip, RpHandshakeAck& ack, char* err, size_t errLen,
                    uint16_t launchGame) {
    g_ctrlSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(RP_PORT_CONTROL);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        snprintf(err, errLen, tr("Geçersiz IP adresi", "Invalid IP address"));
        close(g_ctrlSock); g_ctrlSock = -1;
        return false;
    }

    fcntl(g_ctrlSock, F_SETFL, O_NONBLOCK);
    connect(g_ctrlSock, (sockaddr*)&a, sizeof(a));
    pollfd pf{ g_ctrlSock, POLLOUT, 0 };
    if (poll(&pf, 1, 3000) <= 0) {
        snprintf(err, errLen, tr("Bağlantı zaman aşımı (%s)", "Connection timed out (%s)"), ip);
        close(g_ctrlSock); g_ctrlSock = -1;
        return false;
    }
    int soErr = 0; socklen_t sl = sizeof(soErr);
    getsockopt(g_ctrlSock, SOL_SOCKET, SO_ERROR, &soErr, &sl);
    if (soErr) {
        snprintf(err, errLen, tr("Bağlanılamadı: %s", "Connection failed: %s"), strerror(soErr));
        close(g_ctrlSock); g_ctrlSock = -1;
        return false;
    }
    fcntl(g_ctrlSock, F_SETFL, 0);
    int nd = 1;
    setsockopt(g_ctrlSock, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    timeval sendTimeout{1, 0};
    setsockopt(g_ctrlSock, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

    RpHandshake hs{};
    hs.magic = RP_MAGIC; hs.version = RP_PROTO_VERSION;
    hs.width = g_cfg.width(); hs.height = g_cfg.height();
    hs.fps = (uint16_t)g_cfg.fps;
    hs.launchGame = launchGame;
    hs.bitrateKbps = (uint32_t)g_cfg.bitrateMbps * 1000;
    hs.audioEnabled = g_cfg.audio ? 1 : 0;
    size_t sent = 0;
    while (sent < sizeof(hs)) {
        const int n = send(g_ctrlSock, (const char*)&hs + sent, sizeof(hs) - sent, 0);
        if (n <= 0) {
            snprintf(err, errLen, tr("Handshake gonderilemedi: %s", "Could not send handshake: %s"), strerror(errno));
            close(g_ctrlSock); g_ctrlSock = -1;
            return false;
        }
        sent += n;
    }

    int rcvd = 0;
    const uint64_t handshakeDeadline = nowMs() + 15000;
    while (rcvd < (int)sizeof(ack)) {
        pollfd pr{ g_ctrlSock, POLLIN, 0 };
        const uint64_t now = nowMs();
        const int ready = now < handshakeDeadline ? poll(&pr, 1, int(handshakeDeadline - now)) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            snprintf(err, errLen, tr("Handshake yaniti gelmedi (15 sn, %s)", "No handshake reply (15 sec, %s)"), ip);
            close(g_ctrlSock); g_ctrlSock = -1;
            return false;
        }
        int n = recv(g_ctrlSock, (char*)&ack + rcvd, sizeof(ack) - rcvd, 0);
        if (n <= 0) {
            snprintf(err, errLen, tr("Bağlantı koptu", "Connection lost"));
            close(g_ctrlSock); g_ctrlSock = -1;
            return false;
        }
        rcvd += n;
    }
    if (ack.magic != RP_MAGIC || !ack.ok) {
        snprintf(err, errLen, tr("Host reddetti (sürüm uyumsuzluğu?)", "Host rejected connection (version mismatch?)"));
        close(g_ctrlSock); g_ctrlSock = -1;
        return false;
    }
    if (ack.audioRate)
        g_audioBytesPerMs = (uint32_t)(((uint64_t)ack.audioRate * 2 * 2) / 1000);

    g_mediaSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(g_mediaSock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in ma{};
    ma.sin_family = AF_INET;
    ma.sin_addr.s_addr = INADDR_ANY;
    ma.sin_port = htons(RP_PORT_MEDIA);
    if (bind(g_mediaSock, (sockaddr*)&ma, sizeof(ma)) != 0) {
        snprintf(err, errLen, tr("Medya portu açılamadı", "Could not open media port"));
        close(g_ctrlSock); g_ctrlSock = -1;
        close(g_mediaSock); g_mediaSock = -1;
        return false;
    }

    g_inputSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    g_hostAddr = a;
    g_hostAddr.sin_port = htons(RP_PORT_INPUT);

    g_netStats.videoBytes = 0; g_netStats.videoFrames = 0; g_netStats.droppedFrames = 0;
    g_rttMs = -1; g_pingSentAt = 0;
    g_resyncReq = false;
    g_running = true;
    return true;
}

void sessionStart() {
    g_mediaThread = std::thread(mediaLoop);
    g_ctrlThread = std::thread(ctrlLoop);
}

bool sessionAlive() { return g_running; }

void sessionDisconnect() {
    if (g_ctrlSock >= 0) sendCtrl(RP_CTRL_BYE);
    g_running = false;
    if (g_ctrlSock >= 0) shutdown(g_ctrlSock, SHUT_RDWR);
    if (g_mediaThread.joinable()) g_mediaThread.join();
    if (g_ctrlThread.joinable()) g_ctrlThread.join();
    if (g_ctrlSock >= 0) { close(g_ctrlSock); g_ctrlSock = -1; }
    if (g_mediaSock >= 0) { close(g_mediaSock); g_mediaSock = -1; }
    if (g_inputSock >= 0) { close(g_inputSock); g_inputSock = -1; }
}

void sendInput(const RpInputPkt& pkt, const char* text) {
    if (g_inputSock < 0) return;
    char buf[sizeof(RpInputPkt) + 512];
    memcpy(buf, &pkt, sizeof(pkt));
    size_t len = sizeof(pkt);
    if (text && pkt.textLen) {
        memcpy(buf + len, text, pkt.textLen);
        len += pkt.textLen;
    }
    sendto(g_inputSock, buf, len, 0, (sockaddr*)&g_hostAddr, sizeof(g_hostAddr));
}
