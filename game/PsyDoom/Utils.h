#pragma once

#include "Macros.h"

#include <cstdint>
#include <memory>
#include <string>

enum SequenceStatus : uint8_t;
struct DiscInfo;
struct IsoFileSys;

BEGIN_NAMESPACE(Utils)

#if defined(PSYDOOM_3DS_BENCHMARK) && PSYDOOM_3DS_BENCHMARK
    // Benchmark-only coarse profiler: accumulates wall time under a handful of named scopes and periodically
    // dumps the per-frame averages to 'sdmc:/3ds/PsyDoom/<variant>/scopes.csv'. Compiled out of shipping builds.
    enum class ProfScope : int {
        RenderPlayerView,
        StatusBar,
        PresentTop,
        PresentBottom,
        PresentSwap,
        Bsp,            // BSP traversal and visibility
        Walls,          // Wall column setup and rasterization
        Flats,          // Floor and ceiling row setup and rasterization
        Sprites,        // Things in the world
        Sky,
        Weapon,         // The player's own weapon sprite
        Count
    };

    void profBegin(const ProfScope scope) noexcept;
    void profEnd(const ProfScope scope) noexcept;
    void profEndFrame() noexcept;

    #define PSYDOOM_PROF_BEGIN(SCOPE)   Utils::profBegin(Utils::ProfScope::SCOPE)
    #define PSYDOOM_PROF_END(SCOPE)     Utils::profEnd(Utils::ProfScope::SCOPE)
    #define PSYDOOM_PROF_END_FRAME()    Utils::profEndFrame()
#else
    #define PSYDOOM_PROF_BEGIN(SCOPE)
    #define PSYDOOM_PROF_END(SCOPE)
    #define PSYDOOM_PROF_END_FRAME()
#endif


//------------------------------------------------------------------------------------------------------------------------------------------
// A simple container for data read from a file on the game disc.
// Holds the byte array and the number of bytes for the data.
//------------------------------------------------------------------------------------------------------------------------------------------
struct DiscFileData {
    std::unique_ptr<std::byte[]>    pBytes;
    uint32_t                        numBytes;
};

const char* getGameVersionString() noexcept;
void installFatalErrorHandler() noexcept;
void uninstallFatalErrorHandler() noexcept;
std::string getOrCreateUserDataFolder() noexcept;
void doPlatformUpdates() noexcept;
bool waitForSeconds(const float seconds) noexcept;
bool waitForCdAudioPlaybackStart() noexcept;
bool waitUntilSeqEnteredStatus(const int32_t sequenceIdx, const SequenceStatus status) noexcept;
bool waitUntilSeqExitedStatus(const int32_t sequenceIdx, const SequenceStatus status) noexcept;
bool waitForCdAudioFadeOut() noexcept;
void threadYield() noexcept;
void onBeginUIDrawing() noexcept;
void checkForRendererToggleInput() noexcept;
void checkForUncappedFramerateToggleInput() noexcept;

DiscFileData getDiscFileData(
    const DiscInfo& discInfo,
    const IsoFileSys& isoFileSys,
    const char* const filePath,
    const uint32_t readOffset = 0,
    const int32_t numBytesToRead = -1
) noexcept;

bool getDiscFileMD5Hash(
    const DiscInfo& discInfo,
    const IsoFileSys& isoFileSys,
    const char* const filePath,
    uint64_t& hashWord1,
    uint64_t& hashWord2
) noexcept;

bool checkDiscFileMD5Hash(
    const DiscInfo& discInfo,
    const IsoFileSys& isoFileSys,
    const char* const filePath,
    const uint64_t checkHashWord1,
    const uint64_t checkHashWord2
) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Tells if a pad button has just been pressed by examining the currently pressed pad buttons versus the last pressed
//------------------------------------------------------------------------------------------------------------------------------------------
inline constexpr bool padBtnJustPressed(const uint32_t btn, const uint32_t curPadBtns, const uint32_t oldPadBtns) noexcept {
    if (curPadBtns & btn) {
        return ((oldPadBtns & btn) == 0);
    } else {
        return false;
    }
}

END_NAMESPACE(Utils)
