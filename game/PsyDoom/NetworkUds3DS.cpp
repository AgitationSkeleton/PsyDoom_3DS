//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: two player local wireless. See 'NetworkUds3DS.h' for why this needs a reliability layer.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "NetworkUds3DS.h"

#include "Platform_3DS.h"

#if PSYDOOM_3DS

#include <3ds.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

BEGIN_NAMESPACE(NetworkUds3DS)

//------------------------------------------------------------------------------------------------------------------------------------------
// UDS parameters.
//
// The communication id and passphrase are what make two consoles find each other, so they must be unique to PsyDoom
// and identical on both sides. Anything else on local wireless with a different id is invisible to us and we to it.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr uint32_t   WLAN_COMM_ID        = 0x50534431;    // 'PSD1'
static constexpr uint8_t    WLAN_SUB_ID         = 0;
static constexpr uint8_t    DATA_CHANNEL        = 1;
static constexpr uint8_t    MAX_NODES           = 2;             // The link cable was two players, so is this
static const char* const    PASSPHRASE          = "PsyDoom3DS-local-link-v1";

// Beacon payload: what a joiner can see about a game before connecting. UDS allows 0x14 bytes of application data.
static constexpr uint8_t    APPDATA_MAGIC_0     = 'P';
static constexpr uint8_t    APPDATA_MAGIC_1     = 'D';
static constexpr uint8_t    APPDATA_VERSION     = 1;

struct BeaconAppData {
    uint8_t     magic0;
    uint8_t     magic1;
    uint8_t     version;
    uint8_t     gameType;
    uint8_t     skill;
    uint8_t     pad;
    int16_t     startMap;
    uint8_t     reserved[12];
};

static_assert(sizeof(BeaconAppData) == 0x14);

//------------------------------------------------------------------------------------------------------------------------------------------
// Reliability layer
//------------------------------------------------------------------------------------------------------------------------------------------

// Biggest game message. The largest thing the engine sends is the 'GameSettings' block during the connect handshake;
// a UDS data frame can hold 0x5C6 bytes so there is plenty of room to spare.
static constexpr int32_t MAX_MSG_BYTES = 512;

// How many messages can be in flight unacknowledged, and how many out of order arrivals can be held.
// The game is lockstep and only ever has a couple of messages outstanding, so these are small.
static constexpr int32_t WINDOW_SIZE = 16;

// How long to wait for an acknowledgement before sending a message again
static constexpr int64_t RETRANSMIT_INTERVAL_MS = 24;

// A keep alive is sent if nothing else has gone out for this long, so each side keeps seeing the other
static constexpr int64_t KEEPALIVE_INTERVAL_MS = 100;

// If nothing at all arrives for this long the link is considered lost
static constexpr int64_t LINK_TIMEOUT_MS = 5000;

enum class MsgKind : uint8_t {
    Data = 0,       // Carries a game message; must be delivered in order
    Ack = 1,        // Pure acknowledgement, carries no payload
    Ping = 2,       // Round trip probe
    Pong = 3        // Reply to a probe, echoes its timestamp
};

// Every frame put on the wire starts with this
struct MsgHeader {
    uint8_t     kind;
    uint8_t     pad;
    uint16_t    seq;            // Sequence of this message (Data only)
    uint16_t    ackSeq;         // Highest contiguous sequence the sender has received
    uint16_t    payloadBytes;
    uint32_t    timestamp;      // Ping/Pong only: milliseconds since connecting, echoed back by Pong
};

static_assert(sizeof(MsgHeader) == 12);

struct OutMsg {
    bool        bInUse;
    uint16_t    seq;
    int32_t     numBytes;
    int64_t     lastSendTimeMs;
    uint8_t     bytes[MAX_MSG_BYTES];
};

struct InMsg {
    bool        bInUse;
    uint16_t    seq;
    int32_t     numBytes;
    int64_t     recvTimeMs;     // When this arrived, so the engine can tell how late a tick packet was
    uint8_t     bytes[MAX_MSG_BYTES];
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Link state
//------------------------------------------------------------------------------------------------------------------------------------------
static bool             gbUdsInited = false;
static bool             gbIsHost = false;
static bool             gbNetworkUp = false;        // The link is answering: cleared as soon as it stops
static bool             gbNetworkOwned = false;     // A network was created or joined and still has to be taken down
static udsBindContext   gBindCtx = {};
static bool             gbBound = false;

static std::array<OutMsg, WINDOW_SIZE>  gSendWindow = {};
static std::array<InMsg, WINDOW_SIZE>   gRecvWindow = {};

static uint16_t gNextSendSeq = 0;       // Sequence to give the next queued message
static uint16_t gPeerAckSeq = 0;        // Highest contiguous sequence the peer says it has
static uint16_t gNextRecvSeq = 0;       // Sequence we are waiting to deliver next
static bool     gbHavePeerAck = false;

static int64_t  gConnectTimeMs = 0;
static int64_t  gLastSendTimeMs = 0;
static int64_t  gLastRecvTimeMs = 0;
static int64_t  gLastPingSentMs = 0;
static int32_t  gPingMs = -1;
static uint32_t gRetransmits = 0;
static bool     gbSearching = false;

// Beacon scan results are kept between scanning and joining
static std::array<uint8_t, 0x4000>  gScanBuffer = {};
static udsNetworkScanInfo*          gpScannedNetworks = nullptr;
static size_t                       gNumScannedNetworks = 0;

//------------------------------------------------------------------------------------------------------------------------------------------
// Milliseconds since some arbitrary fixed point; only differences are ever used
//------------------------------------------------------------------------------------------------------------------------------------------
static int64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Sequence comparison that copes with the 16-bit counter wrapping around
//------------------------------------------------------------------------------------------------------------------------------------------
static bool seqLessThan(const uint16_t a, const uint16_t b) noexcept {
    return ((int16_t)(a - b) < 0);
}

static bool seqLessOrEqual(const uint16_t a, const uint16_t b) noexcept {
    return ((int16_t)(a - b) <= 0);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Resets everything to do with an established link, but leaves the UDS service up
//------------------------------------------------------------------------------------------------------------------------------------------
static void resetLinkState() noexcept {
    gSendWindow = {};
    gRecvWindow = {};
    gNextSendSeq = 0;
    gPeerAckSeq = 0;
    gNextRecvSeq = 0;
    gbHavePeerAck = false;
    gPingMs = -1;
    gRetransmits = 0;

    const int64_t now = nowMs();
    gConnectTimeMs = now;
    gLastSendTimeMs = now;
    gLastRecvTimeMs = now;
    gLastPingSentMs = now;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Puts one frame on the wire. Note a full send buffer is not an error; the message will be retried.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool sendRaw(const MsgKind kind, const uint16_t seq, const void* const pPayload, const int32_t payloadBytes, const uint32_t timestamp) noexcept {
    if (!gbNetworkUp)
        return false;

    uint8_t frame[sizeof(MsgHeader) + MAX_MSG_BYTES];
    MsgHeader header = {};
    header.kind = (uint8_t) kind;
    header.seq = seq;
    header.ackSeq = (uint16_t)(gNextRecvSeq - 1);   // Everything below 'gNextRecvSeq' has been delivered
    header.payloadBytes = (uint16_t) std::max<int32_t>(payloadBytes, 0);
    header.timestamp = timestamp;

    std::memcpy(frame, &header, sizeof(MsgHeader));

    if ((pPayload) && (payloadBytes > 0)) {
        std::memcpy(frame + sizeof(MsgHeader), pPayload, (size_t) payloadBytes);
    }

    const Result result = udsSendTo(
        UDS_BROADCAST_NETWORKNODEID,
        DATA_CHANNEL,
        UDS_SENDFLAG_Default,
        frame,
        sizeof(MsgHeader) + (size_t) std::max<int32_t>(payloadBytes, 0)
    );

    if (UDS_CHECK_SENDTO_FATALERROR(result)) {
        PSYDOOM_3DS_LOG("uds: send failed, link considered down (0x%08lX)", (unsigned long) result);
        gbNetworkUp = false;
        return false;
    }

    gLastSendTimeMs = nowMs();
    return R_SUCCEEDED(result);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Drops send window entries the peer has acknowledged
//------------------------------------------------------------------------------------------------------------------------------------------
static void retireAcknowledged() noexcept {
    if (!gbHavePeerAck)
        return;

    for (OutMsg& msg : gSendWindow) {
        if (msg.bInUse && seqLessOrEqual(msg.seq, gPeerAckSeq)) {
            msg.bInUse = false;
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Files an arrival into the receive window, ignoring duplicates and anything already delivered
//------------------------------------------------------------------------------------------------------------------------------------------
static void storeReceived(const uint16_t seq, const void* const pPayload, const int32_t numBytes) noexcept {
    // Already delivered this one
    if (seqLessThan(seq, gNextRecvSeq))
        return;

    // Too far ahead to hold on to; it will be retransmitted once the gap closes
    if (!seqLessThan(seq, (uint16_t)(gNextRecvSeq + WINDOW_SIZE)))
        return;

    InMsg& slot = gRecvWindow[seq % WINDOW_SIZE];

    if (slot.bInUse && (slot.seq == seq))
        return;     // Duplicate

    slot.bInUse = true;
    slot.seq = seq;
    slot.numBytes = std::min(numBytes, MAX_MSG_BYTES);
    slot.recvTimeMs = nowMs();

    if ((pPayload) && (slot.numBytes > 0)) {
        std::memcpy(slot.bytes, pPayload, (size_t) slot.numBytes);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Pulls everything waiting out of the UDS receive buffer
//------------------------------------------------------------------------------------------------------------------------------------------
static void pullArrivals() noexcept {
    if ((!gbNetworkUp) || (!gbBound))
        return;

    uint8_t frame[sizeof(MsgHeader) + MAX_MSG_BYTES];

    while (true) {
        size_t actualSize = 0;
        uint16_t srcNodeId = 0;
        const Result result = udsPullPacket(&gBindCtx, frame, sizeof(frame), &actualSize, &srcNodeId);

        if (R_FAILED(result)) {
            PSYDOOM_3DS_LOG("uds: receive failed, link considered down (0x%08lX)", (unsigned long) result);
            gbNetworkUp = false;
            return;
        }

        if (actualSize < sizeof(MsgHeader))
            return;     // Nothing (or nothing usable) left

        MsgHeader header = {};
        std::memcpy(&header, frame, sizeof(MsgHeader));

        gLastRecvTimeMs = nowMs();

        // Take the peer's acknowledgement from any frame that carries one
        if ((!gbHavePeerAck) || seqLessThan(gPeerAckSeq, header.ackSeq)) {
            gPeerAckSeq = header.ackSeq;
            gbHavePeerAck = true;
        }

        const int32_t payloadBytes = std::min<int32_t>(
            (int32_t) header.payloadBytes,
            (int32_t)(actualSize - sizeof(MsgHeader))
        );

        switch ((MsgKind) header.kind) {
            case MsgKind::Data:
                storeReceived(header.seq, frame + sizeof(MsgHeader), payloadBytes);
                // Acknowledge straight away so the peer can retire it and stop retransmitting
                sendRaw(MsgKind::Ack, 0, nullptr, 0, 0);
                break;

            case MsgKind::Ping:
                sendRaw(MsgKind::Pong, 0, nullptr, 0, header.timestamp);
                break;

            case MsgKind::Pong: {
                const uint32_t sentAt = header.timestamp;
                const uint32_t now = (uint32_t)(nowMs() - gConnectTimeMs);
                gPingMs = (int32_t)(now - sentAt);
            }   break;

            default:
                break;
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Link lifecycle
//------------------------------------------------------------------------------------------------------------------------------------------
bool init() noexcept {
    if (gbUdsInited)
        return true;

    // The username shown to the other console. UDS needs a non empty one.
    constexpr int32_t NUM_INIT_ATTEMPTS = 4;
    Result result = 0;

    for (int32_t attempt = 0; attempt < NUM_INIT_ATTEMPTS; ++attempt) {
        result = udsInit(0x3000, "PsyDoom");

        if (R_SUCCEEDED(result)) {
            gbUdsInited = true;
            return true;
        }

        // Not up yet: almost always a previous session still letting go. Wait a frame and ask again.
        PSYDOOM_3DS_LOG("uds: udsInit attempt %d failed (0x%08lX)", (int)(attempt + 1), (unsigned long) result);
        Platform3DS::idleWait();
    }

    PSYDOOM_3DS_LOG("uds: giving up on udsInit (0x%08lX)", (unsigned long) result);
    return false;
}

void shutdown() noexcept {
    disconnect();

    if (gbUdsInited) {
        PSYDOOM_3DS_LOG("uds: shutdown");
        udsExit();
        gbUdsInited = false;
    }
}

void disconnect() noexcept {
    if (gbNetworkOwned || gbBound) {
        PSYDOOM_3DS_LOG(
            "uds: disconnect (host=%d, owned=%d, up=%d, bound=%d)",
            (int) gbIsHost, (int) gbNetworkOwned, (int) gbNetworkUp, (int) gbBound
        );
    }

    // Note: keyed off 'gbNetworkOwned' rather than whether the link is still answering. A network that has gone quiet
    // still exists as far as the service is concerned and still has to be destroyed, or the next one cannot be made.
    //
    // The two roles unwind in opposite orders, and getting it the wrong way round leaks the data channel.
    if (gbIsHost) {
        if (gbNetworkOwned) {
            const Result result = udsDestroyNetwork();

            if (R_FAILED(result)) {
                PSYDOOM_3DS_LOG("uds: udsDestroyNetwork failed (0x%08lX)", (unsigned long) result);
            }
        }

        if (gbBound) {
            const Result result = udsUnbind(&gBindCtx);

            if (R_FAILED(result)) {
                PSYDOOM_3DS_LOG("uds: udsUnbind failed (0x%08lX)", (unsigned long) result);
            }
        }
    } else {
        if (gbBound) {
            const Result result = udsUnbind(&gBindCtx);

            if (R_FAILED(result)) {
                PSYDOOM_3DS_LOG("uds: udsUnbind failed (0x%08lX)", (unsigned long) result);
            }
        }

        if (gbNetworkOwned) {
            const Result result = udsDisconnectNetwork();

            if (R_FAILED(result)) {
                PSYDOOM_3DS_LOG("uds: udsDisconnectNetwork failed (0x%08lX)", (unsigned long) result);
            }
        }
    }

    gBindCtx = {};
    gbBound = false;
    gbNetworkOwned = false;
    gbNetworkUp = false;
    gbIsHost = false;       // Whoever starts first hosts, so this has to be settled fresh every session
    gbSearching = false;
    gpScannedNetworks = nullptr;
    gNumScannedNetworks = 0;
    resetLinkState();
}

bool hostGame(const uint8_t gameType, const uint8_t skill, const int16_t startMap) noexcept {
    if (!init())
        return false;

    disconnect();

    udsNetworkStruct network = {};
    udsGenerateDefaultNetworkStruct(&network, WLAN_COMM_ID, WLAN_SUB_ID, MAX_NODES);

    const Result result = udsCreateNetwork(
        &network,
        PASSPHRASE,
        std::strlen(PASSPHRASE) + 1,
        &gBindCtx,
        DATA_CHANNEL,
        UDS_DEFAULT_RECVBUFSIZE
    );

    if (R_FAILED(result)) {
        PSYDOOM_3DS_LOG("uds: udsCreateNetwork failed (0x%08lX)", (unsigned long) result);
        return false;
    }

    PSYDOOM_3DS_LOG("uds: hosting");
    gbBound = true;
    gbNetworkOwned = true;
    gbNetworkUp = true;
    gbIsHost = true;
    resetLinkState();

    // Advertise what game this is so a joiner can see it in the list before committing
    BeaconAppData appData = {};
    appData.magic0 = APPDATA_MAGIC_0;
    appData.magic1 = APPDATA_MAGIC_1;
    appData.version = APPDATA_VERSION;
    appData.gameType = gameType;
    appData.skill = skill;
    appData.startMap = startMap;

    udsSetApplicationData(&appData, sizeof(appData));
    return true;
}

int32_t scanForGames(FoundGame* const outGames, const int32_t maxGames) noexcept {
    if ((!init()) || (!outGames) || (maxGames <= 0))
        return 0;

    gbSearching = true;
    gpScannedNetworks = nullptr;
    gNumScannedNetworks = 0;
    gScanBuffer.fill(0);

    const Result result = udsScanBeacons(
        gScanBuffer.data(),
        gScanBuffer.size(),
        &gpScannedNetworks,
        &gNumScannedNetworks,
        WLAN_COMM_ID,
        WLAN_SUB_ID,
        nullptr,
        false
    );

    if (R_FAILED(result)) {
        gpScannedNetworks = nullptr;
        gNumScannedNetworks = 0;
        return 0;
    }

    int32_t numGames = 0;

    for (size_t i = 0; (i < gNumScannedNetworks) && (numGames < maxGames); ++i) {
        const udsNetworkScanInfo& scanInfo = gpScannedNetworks[i];

        // Only list games whose beacon we understand
        BeaconAppData appData = {};
        size_t appDataSize = 0;

        if (R_FAILED(udsGetNetworkStructApplicationData(&scanInfo.network, &appData, sizeof(appData), &appDataSize)))
            continue;

        if ((appDataSize != sizeof(appData)) ||
            (appData.magic0 != APPDATA_MAGIC_0) ||
            (appData.magic1 != APPDATA_MAGIC_1) ||
            (appData.version != APPDATA_VERSION))
        {
            continue;
        }

        FoundGame& game = outGames[numGames];
        std::memset(&game, 0, sizeof(game));
        game.gameType = appData.gameType;
        game.skill = appData.skill;
        game.startMap = appData.startMap;
        game.networkIdx = (uint8_t) i;

        // The first node is the host
        char username[32] = {};

        if (udsCheckNodeInfoInitialized(&scanInfo.nodes[0]) && R_SUCCEEDED(udsGetNodeInfoUsername(&scanInfo.nodes[0], username))) {
            std::strncpy(game.hostName, username, sizeof(game.hostName) - 1);
        } else {
            std::strncpy(game.hostName, "PsyDoom", sizeof(game.hostName) - 1);
        }

        numGames++;
    }

    return numGames;
}

bool joinGame(const int32_t networkIdx) noexcept {
    if ((!gpScannedNetworks) || (networkIdx < 0) || ((size_t) networkIdx >= gNumScannedNetworks))
        return false;

    const Result result = udsConnectNetwork(
        &gpScannedNetworks[networkIdx].network,
        PASSPHRASE,
        std::strlen(PASSPHRASE) + 1,
        &gBindCtx,
        UDS_BROADCAST_NETWORKNODEID,
        UDSCONTYPE_Client,
        DATA_CHANNEL,
        UDS_DEFAULT_RECVBUFSIZE
    );

    if (R_FAILED(result)) {
        PSYDOOM_3DS_LOG("uds: udsConnectNetwork failed (0x%08lX)", (unsigned long) result);
        return false;
    }

    PSYDOOM_3DS_LOG("uds: joined a game");
    gbBound = true;
    gbNetworkOwned = true;
    gbNetworkUp = true;
    gbIsHost = false;
    gbSearching = false;
    resetLinkState();
    return true;
}

bool isLinkUp() noexcept {
    if (!gbNetworkUp)
        return false;

    // Both sides need to actually be present. For the host that means a second node has joined.
    udsConnectionStatus status = {};

    if (R_FAILED(udsGetConnectionStatus(&status)))
        return false;

    return (status.total_nodes >= 2);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Reliable messaging
//------------------------------------------------------------------------------------------------------------------------------------------
void update() noexcept {
    if (!gbNetworkUp)
        return;

    pullArrivals();
    retireAcknowledged();

    const int64_t now = nowMs();

    // Retransmit anything the peer has not acknowledged in time
    for (OutMsg& msg : gSendWindow) {
        if (msg.bInUse && (now - msg.lastSendTimeMs >= RETRANSMIT_INTERVAL_MS)) {
            if (sendRaw(MsgKind::Data, msg.seq, msg.bytes, msg.numBytes, 0)) {
                msg.lastSendTimeMs = now;
                gRetransmits++;
            }
        }
    }

    // Measure the round trip every so often, for the health indicator
    if (now - gLastPingSentMs >= 500) {
        gLastPingSentMs = now;
        sendRaw(MsgKind::Ping, 0, nullptr, 0, (uint32_t)(now - gConnectTimeMs));
    }

    // Keep the peer seeing us even when the game has nothing to say
    if (now - gLastSendTimeMs >= KEEPALIVE_INTERVAL_MS) {
        sendRaw(MsgKind::Ack, 0, nullptr, 0, 0);
    }

    // Give up if the other side has gone silent for too long. Note this counts silence on the link, not in the game's
    // own traffic: a console busy loading a level keeps answering, so this only fires when one has really gone.
    if (now - gLastRecvTimeMs >= LINK_TIMEOUT_MS) {
        if (gbNetworkUp) {
            PSYDOOM_3DS_LOG("uds: nothing heard for %lldms, link considered down", (long long)(now - gLastRecvTimeMs));
        }

        gbNetworkUp = false;
    }
}

bool send(const void* const pBuffer, const int32_t numBytes) noexcept {
    if ((!gbNetworkUp) || (numBytes < 0) || (numBytes > MAX_MSG_BYTES))
        return false;

    OutMsg& slot = gSendWindow[gNextSendSeq % WINDOW_SIZE];

    // Window full: the peer is too far behind to take another message
    if (slot.bInUse)
        return false;

    slot.bInUse = true;
    slot.seq = gNextSendSeq;
    slot.numBytes = numBytes;
    slot.lastSendTimeMs = nowMs();

    if ((pBuffer) && (numBytes > 0)) {
        std::memcpy(slot.bytes, pBuffer, (size_t) numBytes);
    }

    gNextSendSeq++;
    sendRaw(MsgKind::Data, slot.seq, slot.bytes, slot.numBytes, 0);
    return true;
}

int32_t receive(void* const pBuffer, const int32_t maxBytes, int64_t* const pOutRecvTimeMs) noexcept {
    if (!gbNetworkUp)
        return -1;

    InMsg& slot = gRecvWindow[gNextRecvSeq % WINDOW_SIZE];

    // The next message in order has not arrived yet
    if ((!slot.bInUse) || (slot.seq != gNextRecvSeq))
        return 0;

    const int32_t numBytes = std::min(slot.numBytes, maxBytes);

    if ((pBuffer) && (numBytes > 0)) {
        std::memcpy(pBuffer, slot.bytes, (size_t) numBytes);
    }

    if (pOutRecvTimeMs) {
        *pOutRecvTimeMs = slot.recvTimeMs;
    }

    slot.bInUse = false;
    gNextRecvSeq++;
    return numBytes;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Link quality
//------------------------------------------------------------------------------------------------------------------------------------------
LinkHealth getHealth() noexcept {
    if (!gbNetworkUp)
        return (gbSearching) ? LinkHealth::Searching : LinkHealth::Disconnected;

    const int64_t sinceRecvMs = nowMs() - gLastRecvTimeMs;

    // Nothing at all for a while is the clearest sign of trouble
    if (sinceRecvMs > 500)
        return LinkHealth::Poor;

    // Otherwise judge on round trip time. A 30 Hz lockstep tick is 33ms, so anything past that costs frames.
    if (gPingMs < 0)
        return LinkHealth::Fair;     // Not measured yet

    if (gPingMs > 120)
        return LinkHealth::Poor;

    if (gPingMs > 40)
        return LinkHealth::Fair;

    return LinkHealth::Good;
}

int32_t getPingMs() noexcept {
    return gPingMs;
}

uint32_t getRetransmitCount() noexcept {
    return gRetransmits;
}

END_NAMESPACE(NetworkUds3DS)

#endif  // #if PSYDOOM_3DS
