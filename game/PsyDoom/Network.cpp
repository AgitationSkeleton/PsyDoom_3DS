#include "Network.h"

#if PSYDOOM_3DS

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: the PlayStation link cable is replaced by 3DS local wireless (UDS).
//
// The engine's netgame is strict lockstep and cannot survive a lost or reordered tick, so 'NetworkUds3DS' puts a
// sequenced, acknowledged, retransmitting layer over UDS data frames. Everything here is just adapting that layer to
// the blocking, byte oriented interface the engine expects, with the waits pumping the platform so the console stays
// responsive and the player can back out.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "Doom/Base/i_main.h"
#include "Doom/d_main.h"
#include "Doom/Game/p_tick.h"
#include "Doom/UI/m_main.h"
#include "Input.h"
#include "NetworkUds3DS.h"
#include "ProgArgs.h"
#include "Utils.h"
#include "Controls.h"
#include "Video.h"

#if PSYDOOM_3DS
    #include "Platform_3DS.h"
#endif

#include <cstring>

BEGIN_NAMESPACE(Network)

// A flag set to true if network init was aborted by the user
bool gbWasInitAborted = false;

static bool gbConnected = false;

// The next tick packet taken off the link, held until the game asks for it
static NetPacket_Tick                           gPendingTickPacket = {};
static std::chrono::system_clock::time_point    gPendingTickRecvTime = {};
static bool                                     gbHavePendingTickPacket = false;

//------------------------------------------------------------------------------------------------------------------------------------------
// Does the user want to give up on connecting?
//------------------------------------------------------------------------------------------------------------------------------------------
static bool isCancelRequested() noexcept {
    TickInputs inputs;
    P_GatherTickInputs(inputs);
    return inputs.fMenuBack();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Keeps the console alive while waiting on the link. Returns false if the wait should be given up on.
//
// 'bRedraw' is for the waits that happen while connecting. Nothing else is drawing then - the screen showing the other
// console being found, and the ping beside it, come from here and nowhere else - so without it that screen sits frozen
// and the consoles look like they are not pairing even when they are.
//
// It has to be off for the waits during play. Those happen part way through a frame, where the framebuffer still holds
// whichever pass was last drawn into it, and repainting from there puts the touch screen's own image on the top screen.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool pumpWhileWaiting(const bool bIsAbortable, const bool bRedraw) noexcept {
    if (Input::isQuitRequested())
        return false;

    NetworkUds3DS::update();
    Utils::doPlatformUpdates();

    // That update threw away whatever the previous one saw. This is the only place that happens faster than the game
    // ticks, so it is the only place a press can go missing - hold on to the ones the tick reads as a press.
    Controls::latchTickPresses();

    if (bIsAbortable && isCancelRequested()) {
        gbWasInitAborted = true;
        return false;
    }

    if (bRedraw) {
        Video::displayFramebuffer();
    }
    #if PSYDOOM_3DS
        else {
            Platform3DS::idleWait();    // Somewhere to idle for a frame that does not touch either screen
        }
    #endif

    Utils::threadYield();
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Waits until both consoles are present on the wireless network
//------------------------------------------------------------------------------------------------------------------------------------------
static bool waitForLink(const int64_t timeoutMs) noexcept {
    const auto startTime = std::chrono::steady_clock::now();

    while (!NetworkUds3DS::isLinkUp()) {
        if (!pumpWhileWaiting(true, true))
            return false;

        if (timeoutMs > 0) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime
            ).count();

            if (waited > timeoutMs) {
                PSYDOOM_3DS_LOG("uds: no link after %lldms, giving up on this one", (long long) waited);
                return false;
            }
        }
    }

    gbConnected = true;
    return true;
}

// How long to give a game we have joined to actually produce a link before deciding its beacon was stale
static constexpr int64_t JOIN_LINK_TIMEOUT_MS = 5000;

//------------------------------------------------------------------------------------------------------------------------------------------
// Hosts a game and waits for the other console to join
//------------------------------------------------------------------------------------------------------------------------------------------
bool initForServer() noexcept {
    shutdown();
    gbWasInitAborted = false;

    // Advertise what is being played, so the joining console can show it before connecting
    if (!NetworkUds3DS::hostGame((uint8_t) gStartGameType, (uint8_t) gStartSkill, (int16_t) gStartMapOrEpisode))
        return false;

    if (!waitForLink(0)) {
        NetworkUds3DS::disconnect();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Scans for a hosted game and joins the first one found
//------------------------------------------------------------------------------------------------------------------------------------------
bool initForClient() noexcept {
    shutdown();
    gbWasInitAborted = false;

    if (!NetworkUds3DS::init())
        return false;

    // Keep scanning until a game shows up or the player backs out. Scanning is a blocking radio operation that takes
    // a moment, so the display is pumped either side of it.
    while (true) {
        NetworkUds3DS::FoundGame games[NetworkUds3DS::MAX_FOUND_GAMES] = {};
        const int32_t numGames = NetworkUds3DS::scanForGames(games, NetworkUds3DS::MAX_FOUND_GAMES);

        if (numGames > 0) {
            if (NetworkUds3DS::joinGame(games[0].networkIdx))
                break;
        }

        if (!pumpWhileWaiting(true, true))
            return false;
    }

    if (!waitForLink(JOIN_LINK_TIMEOUT_MS)) {
        NetworkUds3DS::disconnect();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Works out which console should host.
//
// There is no command line on a 3DS, and asking the player to pick a role for a two console game is friction that the
// hardware does not need: whoever starts first ends up hosting. This scans for a moment, joins a game if it finds one,
// and otherwise hosts and waits. That matches how the link cable worked, where one side waited for the other.
//------------------------------------------------------------------------------------------------------------------------------------------
bool initFor3DSLocalWireless() noexcept {
    shutdown();
    gbWasInitAborted = false;

    if (!NetworkUds3DS::init())
        return false;

    // Take turns looking and advertising, rather than looking a few times and then advertising for good.
    //
    // Both players usually reach this at the same moment - they agree to play, or they both back out of one game to
    // start another - so both scan while neither is advertising yet, both find nothing, and both settle down to host.
    // Two hosts never find each other and neither ever looks again, which is a game that simply will not start.
    //
    // Alternating breaks that. Whoever is advertising when the other happens to scan gets found, and since a scan
    // takes a varying amount of time on the radio the two consoles drift apart rather than staying in step.
    constexpr int32_t NUM_ROUNDS = 8;
    constexpr int64_t HOST_WAIT_MS = 3000;      // Long enough for the other console to complete a scan and join

    for (int32_t round = 0; round < NUM_ROUNDS; ++round) {
        // Is anyone advertising right now?
        NetworkUds3DS::FoundGame games[NetworkUds3DS::MAX_FOUND_GAMES] = {};
        const int32_t numGames = NetworkUds3DS::scanForGames(games, NetworkUds3DS::MAX_FOUND_GAMES);

        if ((numGames > 0) && NetworkUds3DS::joinGame(games[0].networkIdx)) {
            ProgArgs::gbIsNetServer = false;
            ProgArgs::gbIsNetClient = true;

            if (waitForLink(JOIN_LINK_TIMEOUT_MS))
                return true;

            // Nothing came of it. Let go and keep looking: the console that was advertising has most likely just shut
            // its game down, and its beacon has outlived it. Backing out is the player's decision, not a stale beacon's.
            NetworkUds3DS::disconnect();

            if (gbWasInitAborted)
                return false;

            continue;
        }

        if (!pumpWhileWaiting(true, true))
            return false;

        // Nobody about, so advertise for a while and see if anyone turns up
        if (NetworkUds3DS::hostGame((uint8_t) gStartGameType, (uint8_t) gStartSkill, (int16_t) gStartMapOrEpisode)) {
            ProgArgs::gbIsNetServer = true;
            ProgArgs::gbIsNetClient = false;

            if (waitForLink(HOST_WAIT_MS))
                return true;

            NetworkUds3DS::disconnect();

            if (gbWasInitAborted)
                return false;
        }
    }

    // Taking turns has not found anyone, so settle down and advertise until the other console appears or the player
    // gives up. Whoever is slower to start ends up here, and is found by the one still looking.
    ProgArgs::gbIsNetServer = true;
    ProgArgs::gbIsNetClient = false;
    return initForServer();
}

void shutdown() noexcept {
    NetworkUds3DS::disconnect();
    gbConnected = false;
    gbHavePendingTickPacket = false;
    gPendingTickPacket = {};
}

bool isConnected() noexcept {
    return (gbConnected && NetworkUds3DS::isLinkUp());
}

void doUpdates() noexcept {
    NetworkUds3DS::update();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Blocking send of an arbitrary block of bytes, used by the connect handshake.
// Blocks only if the send window is momentarily full, which the lockstep game should never actually cause.
//------------------------------------------------------------------------------------------------------------------------------------------
bool sendBytes(const void* const pBuffer, const int32_t numBytes) noexcept {
    if (!isConnected())
        return false;

    while (!NetworkUds3DS::send(pBuffer, numBytes)) {
        if (!pumpWhileWaiting(false, true))
            return false;

        if (!isConnected())
            return false;
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Blocking receive of a block of bytes, used by the connect handshake
//------------------------------------------------------------------------------------------------------------------------------------------
bool recvBytes(void* pBuffer, const int32_t numBytes) noexcept {
    if (!isConnected())
        return false;

    while (true) {
        const int32_t received = NetworkUds3DS::receive(pBuffer, numBytes);

        if (received > 0)
            return (received == numBytes);

        if (received < 0) {
            shutdown();
            return false;
        }

        if (!pumpWhileWaiting(false, true))
            return false;

        if (!isConnected())
            return false;
    }
}

bool sendTickPacket(const NetPacket_Tick& packet) noexcept {
    if (!isConnected())
        return false;

    // Note: waits for room rather than giving up, and with no time limit of its own. Room frees up when the other
    // console acknowledges, and one that has stopped acknowledging altogether is caught by the link layer - which
    // 'isConnected' reports. See the note in 'recvTickPacket'.
    while (!NetworkUds3DS::send(&packet, sizeof(packet))) {
        if (!pumpWhileWaiting(false, false))
            return false;

        if (!isConnected())
            return false;
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Nothing to do here: the reliability layer pulls arrivals continuously in 'update'
//------------------------------------------------------------------------------------------------------------------------------------------
bool requestTickPackets() noexcept {
    return isConnected();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Blocks for the other console's packet for this tick. This is the lockstep wait, and is where a poor wireless link
// shows up as lost frames.
//------------------------------------------------------------------------------------------------------------------------------------------
bool recvTickPacket(NetPacket_Tick& packet, std::chrono::system_clock::time_point& receiveTime) noexcept {
    if (!isConnected())
        return false;

    // Note: no time limit here on purpose. A console that has gone is the link layer's business, and it keeps a
    // keepalive going and gives up after five seconds of real silence - see 'LINK_TIMEOUT_MS'. The 'isConnected' check
    // below is what ends this wait when that happens. Timing the game's own ticks instead would end a healthy game
    // every time the other console took a while over a level.
    while (!gbHavePendingTickPacket) {
        int64_t recvTimeMs = 0;
        const int32_t received = NetworkUds3DS::receive(&gPendingTickPacket, sizeof(gPendingTickPacket), &recvTimeMs);

        if (received > 0) {
            if (received != (int32_t) sizeof(gPendingTickPacket))
                return false;       // Not a tick packet: the two sides disagree about what is being exchanged

            // Convert the link's monotonic arrival time into the clock the engine measures lateness against
            const int64_t monotonicNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();

            gPendingTickRecvTime = std::chrono::system_clock::now() - std::chrono::milliseconds(monotonicNowMs - recvTimeMs);
            gbHavePendingTickPacket = true;
            break;
        }

        if (received < 0) {
            shutdown();
            return false;
        }

        // Note: abortable, unlike before. Without this the only way out of a game whose other console had gone was to
        // turn this one off too.
        if (!pumpWhileWaiting(false, false))
            return false;

        if (!isConnected())
            return false;
    }

    packet = gPendingTickPacket;
    receiveTime = gPendingTickRecvTime;
    gbHavePendingTickPacket = false;
    return true;
}

END_NAMESPACE(Network)

#else

#include "Doom/Base/i_main.h"
#include "Doom/Game/p_tick.h"
#include "Doom/UI/m_main.h"
#include "Input.h"
#include "NetPacketReader.h"
#include "NetPacketWriter.h"
#include "ProgArgs.h"
#include "PsxPadButtons.h"
#include "PsyQ/LIBETC.h"
#include "Utils.h"
#include "Video.h"

// This prevents warnings in ASIO about the Windows SDK target version not being specified
#if _WIN32
    #include <sdkddkver.h>
#endif

BEGIN_DISABLE_HEADER_WARNINGS
    #include <asio.hpp>
END_DISABLE_HEADER_WARNINGS

BEGIN_NAMESPACE(Network)

// A flag set to true if network init was aborted by the user
bool gbWasInitAborted = false;

// Maximum number of input/output tick packets that can be buffered
static constexpr int32_t MAX_TICK_PKTS = 2;

static std::unique_ptr<asio::io_context>                                    gpIoContext;
static std::unique_ptr<asio::ip::tcp::socket>                               gpSocket;
static std::unique_ptr<NetPacketReader<NetPacket_Tick, MAX_TICK_PKTS>>      gTickPacketReader;
static std::unique_ptr<NetPacketWriter<NetPacket_Tick, MAX_TICK_PKTS>>      gTickPacketWriter;
static bool                                                                 gbWasWaitForAsyncNetOpAborted;

//------------------------------------------------------------------------------------------------------------------------------------------
// Checks for user input to cancel an abortable network operation like establishing a connection
//------------------------------------------------------------------------------------------------------------------------------------------
static bool isCancelNetworkConnectionRequested() noexcept {
    // Cancel the network operation if the menu 'back' button is pressed
    TickInputs inputs;
    P_GatherTickInputs(inputs);
    return inputs.fMenuBack();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Blocks until an asynchronous network operation completes, or is aborted; returns 'true' if the operation completed fully (not aborted).
// Queries the given flag (passed by reference) to tell if the operation is completed.
// While the operation is being waited on, platform updates are performed. The operation can also be marked non abortable too.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool waitForAsyncNetworkOp(bool& bFinishedFlag, const bool bIsAbortable) noexcept {
    gbWasWaitForAsyncNetOpAborted = false;

    if (bFinishedFlag)
        return true;

    while (true) {
        // If app quit is requested then abort now with failure
        if (Input::isQuitRequested())
            return false;

        // Update the platform, and handle network related events: this might cause the operation to complete.
        doUpdates();
        Utils::doPlatformUpdates();

        if (bFinishedFlag)
            break;

        // Not yet complete: check for abortion requests from the user - if that is allowed.
        if (bIsAbortable && isCancelNetworkConnectionRequested()) {
            gbWasWaitForAsyncNetOpAborted = true;
            return false;
        }

        // While we are waiting update the display to help prevent stutter after long pauses (if we are waiting a long time).
        // See the 'Utils.cpp' file for more comments on this issue; we don't do this for the Vulkan backend also.
        if (Video::gBackendType != Video::BackendType::Vulkan) {
            Video::displayFramebuffer();
        } else {
            // Quick hack for the Vulkan renderer, in case the user resizes the screen while we are connecting - redraw everything...
            M_DrawNetworkConnectDisplay();
        }

        Utils::threadYield();
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Setup options and preferences for the given socket among other stuff
//------------------------------------------------------------------------------------------------------------------------------------------
static void postConnectSetup(asio::ip::tcp::socket& socket) noexcept {
    // Set all the socket options
    auto trySetOption = [&](auto option) noexcept {
        try {
            socket.set_option(option);
        }
        catch (...) {
            // Ignore if not supported on this platform...
        }
    };

    trySetOption(asio::ip::tcp::no_delay(true));                        // Disable Nagles algorithm
    trySetOption(asio::socket_base::send_buffer_size(1024 * 16));       // 16 KiB
    trySetOption(asio::socket_base::receive_buffer_size(1024 * 16));    // 16 KiB
    trySetOption(asio::socket_base::keep_alive(false));                 // Shouldn't be neccessary when client + server are talking constantly
    trySetOption(asio::socket_base::send_low_watermark(0));             // Don't wait to batch up sends/receives (Doom only sends a small amount of data)
    trySetOption(asio::socket_base::receive_low_watermark(0));          // Don't wait to batch up sends/receives (Doom only sends a small amount of data)

    // Setup the tick packet reader/writers
    gTickPacketReader.reset(new NetPacketReader<NetPacket_Tick, MAX_TICK_PKTS>(*gpSocket));
    gTickPacketWriter.reset(new NetPacketWriter<NetPacket_Tick, MAX_TICK_PKTS>(*gpSocket));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Create a connection for the server of a game (player 1).
// Waits until an incomming connection is received from the client (player 2).
// Updates the window etc. while all this is happening and allows the connection attempt to be aborted.
//------------------------------------------------------------------------------------------------------------------------------------------
bool initForServer() noexcept {
    // Clean up any previous connection first
    shutdown();
    bool bWasSuccessful = false;

    try {
        // Create the io context and socket
        gpIoContext.reset(new asio::io_context());
        gpSocket.reset(new asio::ip::tcp::socket(*gpIoContext));

        // Start asynchronously waiting to accept a connection
        bool bDoneAsyncOp = false;
        asio::ip::tcp::acceptor acceptor(*gpIoContext, asio::ip::tcp::endpoint(asio::ip::tcp::v6(), ProgArgs::gServerPort));

        acceptor.async_accept(
            *gpSocket,
            [&](const asio::error_code& error) noexcept {
                bDoneAsyncOp = true;
                bWasSuccessful = (!error);
            }
        );

        waitForAsyncNetworkOp(bDoneAsyncOp, true);

        if (gpSocket && bWasSuccessful) {
            postConnectSetup(*gpSocket);
        }
    }
    catch (...) {
        // Connection attempt failed for some reason...
    }

    // If the connection attempt failed then cleanup before exiting
    if (!bWasSuccessful) {
        gbWasInitAborted = gbWasWaitForAsyncNetOpAborted;
        shutdown();
    }

    return bWasSuccessful;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Create a connection for a client of a game (player 2).
// Waits until a connection is made to the server (player 1).
// Updates the window etc. while all this is happening and allows the connection attempt to be aborted.
//------------------------------------------------------------------------------------------------------------------------------------------
bool initForClient() noexcept {
    // Clean up any previous connection first
    shutdown();
    bool bWasSuccessful = false;

    try {
        // Create the io context and socket
        gpIoContext.reset(new asio::io_context());
        gpSocket.reset(new asio::ip::tcp::socket(*gpIoContext));

        // Start asynchronously resolving the host address
        asio::ip::tcp::resolver tcpResolver(*gpIoContext);
        asio::ip::tcp::resolver::query tcpResolverQuery(ProgArgs::getServerHost(), std::to_string(ProgArgs::gServerPort));

        asio::ip::tcp::resolver::iterator resolverIter;
        bool bDoneAsyncOp = false;

        tcpResolver.async_resolve(
            tcpResolverQuery,
            [&](const asio::error_code& error, asio::ip::tcp::resolver::iterator iter) noexcept {
                bDoneAsyncOp = true;
                resolverIter = iter;
                bWasSuccessful = (!error);
            }
        );

        // If we finished that and were successful then start trying to connect to the server
        if (waitForAsyncNetworkOp(bDoneAsyncOp, true) && bWasSuccessful) {
            // Doing a new operation: so reset done/success flags
            bDoneAsyncOp = false;
            bWasSuccessful = false;

            while (resolverIter != asio::ip::tcp::resolver::iterator{}) {
                gpSocket->async_connect(
                    *resolverIter,
                    [&](const asio::error_code& error) noexcept {
                        bDoneAsyncOp = true;
                        bWasSuccessful = (!error);
                    }
                );

                waitForAsyncNetworkOp(bDoneAsyncOp, true);

                if (gpSocket && bWasSuccessful) {
                    postConnectSetup(*gpSocket);
                }

                break;
            }
        }
    }
    catch (...) {
        // Connection attempt failed for some reason...
    }

    // If the connection attempt failed then cleanup before exiting
    if (!bWasSuccessful) {
        gbWasInitAborted = gbWasWaitForAsyncNetOpAborted;
        shutdown();
    }

    return bWasSuccessful;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Closes up the current network connection (if any)
//------------------------------------------------------------------------------------------------------------------------------------------
void shutdown() noexcept {
    gpSocket.reset();
    gpIoContext.reset();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Tells if there is a network connection established
//------------------------------------------------------------------------------------------------------------------------------------------
bool isConnected() noexcept {
    return (gpSocket && gpSocket->is_open());
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the scoket for the current network connection
//------------------------------------------------------------------------------------------------------------------------------------------
asio::ip::tcp::socket* getSocket() noexcept {
    return gpSocket.get();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Process any network related events that need to be handled
//------------------------------------------------------------------------------------------------------------------------------------------
void doUpdates() noexcept {
    if (gpIoContext) {
        gpIoContext->restart();
        gpIoContext->poll();
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Send the specified number of bytes along the network connection and wait for completion.
// Returns 'true' on success completion, 'false' in all other error cases.
//
// Notes:
//  (1) If the write attempt fails then the connection is killed immediately
//------------------------------------------------------------------------------------------------------------------------------------------
bool sendBytes(const void* const pBuffer, const int32_t numBytes) noexcept {
    // If there is no valid socket or the byte count is bad then this fails immediately
    if (!isConnected())
        return false;

    if (numBytes < 0) {
        shutdown();
        return false;
    }

    // Begin writing the bytes and wait until that is done
    bool bWasSuccessful = false;
    bool bDoneAsyncOp = false;

    try {
        asio::async_write(
            *gpSocket,
            asio::buffer(pBuffer, (size_t) numBytes),
            [&](const asio::error_code& error, const std::size_t bytesWritten) noexcept {
                bDoneAsyncOp = true;
                bWasSuccessful = ((!error) && (bytesWritten == (size_t) numBytes));
            }
        );

        waitForAsyncNetworkOp(bDoneAsyncOp, false);
    }
    catch (...) {
        // Send attempt failed for some reason - kill the connection...
        shutdown();
        return false;
    }

    // If the send attempt failed for some reason then kill the connection
    if (!bWasSuccessful) {
        shutdown();
    }

    return bWasSuccessful;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Read the specified number of bytes from the network connection and wait for completion.
// Returns 'true' on success completion, 'false' in all other error cases.
//
// Notes:
//  (1) If the read attempt fails then the connection is killed immediately
//------------------------------------------------------------------------------------------------------------------------------------------
bool recvBytes(void* pBuffer, const int32_t numBytes) noexcept {
    // If there is no valid socket or the byte count is bad then this fails immediately
    if (!isConnected())
        return false;

    if (numBytes < 0) {
        shutdown();
        return false;
    }

    // Begin receiving the bytes and wait until that is done
    bool bWasSuccessful = false;
    bool bDoneAsyncOp = false;

    try {
        asio::async_read(
            *gpSocket,
            asio::buffer(pBuffer, (size_t) numBytes),
            [&](const asio::error_code error, const std::size_t bytesRead) noexcept {
                bDoneAsyncOp = true;
                bWasSuccessful = ((!error) && (bytesRead == (size_t) numBytes));
            }
        );

        waitForAsyncNetworkOp(bDoneAsyncOp, false);
    }
    catch (...) {
        // Receive attempt failed for some reason - kill the connection...
        shutdown();
        return false;
    }

    // If the receive attempt failed for some reason then kill the connection
    if (!bWasSuccessful) {
        shutdown();
    }

    return bWasSuccessful;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Send a tick update packet: this call may or may not block, depending on whether the outgoing packet queue is full or not.
//------------------------------------------------------------------------------------------------------------------------------------------
bool sendTickPacket(const NetPacket_Tick& packet) noexcept {
    if (!isConnected())
        return false;

    if (!gTickPacketWriter->writePacket(packet, nullptr)) {
        shutdown();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Request that the tick packet buffer be filled as much as possible and kick off as many packet reads as we can.
//------------------------------------------------------------------------------------------------------------------------------------------
bool requestTickPackets() noexcept {
    if (!isConnected())
        return false;

    if (!gTickPacketReader->asyncFillPacketBuffer()) {
        shutdown();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Read one tick packet and block until it is available or an error occurs, in which case 'false' is returned.
// Returns the time that the packet was received at also.
//------------------------------------------------------------------------------------------------------------------------------------------
bool recvTickPacket(NetPacket_Tick& packet, std::chrono::system_clock::time_point& receiveTime) noexcept {
    if (!isConnected())
        return false;

    if (!gTickPacketReader->popRequestedPacket(packet, receiveTime, nullptr)) {
        shutdown();
        return false;
    }

    return true;
}

END_NAMESPACE(Network)

#endif
