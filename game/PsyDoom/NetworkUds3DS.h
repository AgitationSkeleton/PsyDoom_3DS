#pragma once

#if PSYDOOM_3DS

#include "Macros.h"

#include <cstdint>

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: two player local wireless, standing in for the PlayStation link cable.
//
// The engine's netgame is strict lockstep: every 30 Hz tick each side sends the inputs it will use on the NEXT tick and
// then blocks until the other side's packet for this tick arrives. Both machines then simulate identically and compare
// a checksum of both players' positions to catch divergence.
//
// That model needs an ordered, lossless link. The link cable was one; 3DS local wireless is not - 'udsSendTo' data
// frames can be dropped, and the game cannot tolerate a missing or reordered tick. So this layer adds:
//
//   * A sequence number on every message, and an acknowledgement of the highest contiguous sequence received.
//   * A send window that keeps unacknowledged messages and retransmits them until they are acknowledged.
//   * A receive window that holds out of order arrivals until the gap ahead of them is filled.
//
// Rollback netcode is deliberately not attempted. Rolling back would mean snapshotting and restoring the whole Doom
// simulation every tick, which this engine has no facility for. Instead the sync insurance is: guarantee the transport
// never loses or reorders a tick, let the engine's existing 'lastPacketDelayMs' clock adjustment absorb jitter, and
// fall back on the engine's existing checksum + resync handshake if the two sides ever do diverge.
//------------------------------------------------------------------------------------------------------------------------------------------
BEGIN_NAMESPACE(NetworkUds3DS)

// How the local wireless link is doing. Drives the connection health icon on the bottom screen.
enum class LinkHealth : uint8_t {
    Disconnected,
    Searching,
    Good,       // Green:  packets arriving on time, few or no retransmits
    Fair,       // Yellow: noticeable delay or occasional retransmits
    Poor        // Red:    heavy delay or frequent retransmits, a stall is likely
};

// A network found while scanning, as shown in the join list
struct FoundGame {
    char        hostName[32];   // The host console's username
    uint8_t     gameType;       // 'gametype_t': co-op or deathmatch
    uint8_t     skill;          // 'skill_t'
    int16_t     startMap;
    uint8_t     networkIdx;     // Index into the scan results, used to connect
};

static constexpr int32_t MAX_FOUND_GAMES = 8;

//------------------------------------------------------------------------------------------------------------------------------------------
// Link lifecycle
//------------------------------------------------------------------------------------------------------------------------------------------

// Brings up the UDS service. Safe to call repeatedly; returns false if local wireless is unavailable.
bool init() noexcept;
void shutdown() noexcept;

// Host a game and advertise it. The game settings are put in the beacon so joiners can see them before connecting.
bool hostGame(const uint8_t gameType, const uint8_t skill, const int16_t startMap) noexcept;

// Scan for advertised games. Returns how many were found and fills 'outGames'.
int32_t scanForGames(FoundGame* const outGames, const int32_t maxGames) noexcept;

// Join a game found by the last scan
bool joinGame(const int32_t networkIdx) noexcept;

// Has a second player connected (host), or are we connected to a host (client)?
bool isLinkUp() noexcept;

// Drop the link and stop advertising
void disconnect() noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Reliable messaging, used to implement PsyDoom's 'Network' interface
//------------------------------------------------------------------------------------------------------------------------------------------

// Pumps the link: retransmits unacknowledged messages, pulls in arrivals and updates health. Call often.
void update() noexcept;

// Queues a message for reliable, in order delivery. Returns false if the send window is full or the link is down.
bool send(const void* const pBuffer, const int32_t numBytes) noexcept;

// Takes the next in order message if one is ready. Returns the byte count, 0 if nothing is ready yet, -1 on error.
// 'pOutRecvTimeMs' optionally receives when the message actually arrived, which the engine uses to measure lateness.
int32_t receive(void* const pBuffer, const int32_t maxBytes, int64_t* const pOutRecvTimeMs = nullptr) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Link quality, for the on screen indicator
//------------------------------------------------------------------------------------------------------------------------------------------
LinkHealth getHealth() noexcept;

// Round trip time in milliseconds, or -1 if not measured yet
int32_t getPingMs() noexcept;

// How many messages have had to be retransmitted since connecting
uint32_t getRetransmitCount() noexcept;

END_NAMESPACE(NetworkUds3DS)

#endif  // #if PSYDOOM_3DS
