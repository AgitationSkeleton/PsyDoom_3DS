#pragma once

#include "Macros.h"

#include <cstdint>

// Stat display modes
enum class StatDisplayMode : int32_t {
    None = 0,
    Kills = 1,
    KillsAndSecrets = 2,
    KillsSecretsAndItems = 3
};

BEGIN_NAMESPACE(PlayerPrefs)

// Length of PSX Doom passwords
constexpr int32_t PASSWORD_LEN = 10;

// Holds the ASCII readable characters for a game password
struct Password {
    char pwChars[PASSWORD_LEN];
};

// Minimum and maximum values for sound and music volume
constexpr int32_t VOLUME_MIN = 0;
constexpr int32_t VOLUME_MAX = 100;

// Minimum and maximum values for the turn speed multiplier (expressed in whole percentages, 0-500%)
constexpr int32_t TURN_SPEED_MULT_MIN = 1;
constexpr int32_t TURN_SPEED_MULT_MAX = 500;

extern int32_t              gTurnSpeedMult100;
extern bool                 gbAlwaysRun;
extern bool                 gbUncapFramerate;
extern StatDisplayMode      gStatDisplayMode;
extern Password             gLastPassword_Doom;
extern Password             gLastPassword_FDoom;
extern Password             gLastPassword_GecMe;

#if PSYDOOM_3DS
    // How much detail the classic renderer shades walls and floors at.
    // '-1' means auto (decided from the console model), otherwise: 0 = full, 1 = low, 2 = lowest.
    constexpr int32_t DETAIL_MODE_AUTO = -1;
    constexpr int32_t DETAIL_MODE_MIN = 0;
    constexpr int32_t DETAIL_MODE_MAX = 2;

    extern int32_t          gDetailMode;

    // Stretch the game image across the whole width of the top screen, rather than keeping the PlayStation's aspect
    // ratio and leaving pillarbox bars either side.
    extern bool             gbFullWidthVideo;

    // Where the status bar lives. Moving it off the top screen gives its rows to the 3D view, which is the same trade
    // vanilla Doom's largest screen size makes: a taller view, more of the world below the horizon and more of the
    // weapon, at the cost of the automap having less room on the touch screen.
    constexpr int32_t STATUS_BAR_TOP_SCREEN = 0;    // Above the automap on the top screen, as the PlayStation had it
    constexpr int32_t STATUS_BAR_TOUCH_TOP = 1;     // Along the top of the touch screen
    constexpr int32_t STATUS_BAR_TOUCH_BOTTOM = 2;  // Along the bottom of the touch screen
    constexpr int32_t STATUS_BAR_POS_COUNT = 3;

    extern int32_t          gStatusBarPos;

    // Which named set of control bindings is in use. See 'ControlSchemes3DS.h'.
    extern int32_t          gControlScheme;

    // Puts the saved control scheme into effect, or notices that the bindings file no longer agrees with it.
    // Must run after the bindings file has been read.
    void applyControlScheme() noexcept;

    // Selects a scheme and puts it into effect straight away
    void setControlScheme(const int32_t scheme) noexcept;

    // Puts the default layout in place as a custom one, so it can then be edited freely
    void resetControlsToDefault() noexcept;

    // Writes the preferences and the control bindings out now, rather than waiting for the game to exit.
    //
    // Nothing on a 3DS exits the way a desktop program does: the player presses HOME and closes the app, and the
    // process is ended where it stands. The cleanup at the end of 'psx_main' never runs, so anything only written
    // there is never written at all - which is why settings changed in a menu did not survive a restart.
    //
    // So every menu that can change something calls this as it closes, and the settings are on the card from that
    // moment on. Call it from the main thread only: it builds paths and writes files, which is more than an APT hook
    // has the stack for, and an attempt to catch the HOME button that way crashed the console on close.
    void flushToDisk() noexcept;

    // Applies 'gStatusBarPos' to the renderer's view height
    void applyStatusBarPlacement() noexcept;

    // Resolves 'gDetailMode' (including 'auto') and pushes it to the rasterizer
    void applyDetailMode() noexcept;

    // The detail mode actually in effect, with 'auto' already resolved
    int32_t getEffectiveDetailMode() noexcept;
#endif

void setToDefaults() noexcept;
void load() noexcept;
void save() noexcept;
float getTurnSpeedMultiplier() noexcept;
void pushSoundAndMusicPrefs() noexcept;
void pullSoundAndMusicPrefs() noexcept;
void pushLastPassword() noexcept;
void pullLastPassword() noexcept;
bool shouldStartupWithVulkanRenderer() noexcept;

END_NAMESPACE(PlayerPrefs)
