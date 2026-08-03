//------------------------------------------------------------------------------------------------------------------------------------------
// Management of some in-game player preferences and saving passwords for the last level completed so it can be restored on relaunch
//------------------------------------------------------------------------------------------------------------------------------------------
#include "PlayerPrefs.h"

#include "Asserts.h"
#include "Doom/Base/s_sound.h"
#include "Doom/UI/o_main.h"
#include "Doom/UI/pw_main.h"
#include "FileUtils.h"
#include "Game.h"
#include "IniUtils.h"
#include "Utils.h"
#include "Video.h"
#include "Wess/psxspu.h"

#if PSYDOOM_3DS
    #include "Config/ConfigSerialization.h"
    #include "ControlSchemes3DS.h"
    #include "Doom/Renderer/r_main.h"
    #include "Gpu.h"
    #include "Platform_3DS.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

BEGIN_NAMESPACE(PlayerPrefs)

 // Sanity check!
static_assert(C_ARRAY_SIZE(gPasswordCharBuffer) == C_ARRAY_SIZE(Password::pwChars));

// Name of the user prefs file: it can reside in either the writable user data folder (default) or in the current working directory.
// We save to the current working directory if the file is found existing there on launch.
static constexpr const char* const PREFS_FILE_NAME = "saved_prefs.ini";

// Globally exposed settings
int32_t             gTurnSpeedMult100;          // In-game tweakable turn speed multiplier expressed in integer percentage points (0-500)
bool                gbAlwaysRun;                // If set then the player runs by default and the run action causes slower walking
bool                gbUncapFramerate;           // Run the game with an uncapped framerate?
StatDisplayMode     gStatDisplayMode;           // Which stats should be displayed
Password            gLastPassword_Doom;         // Password for the current level the player is on: Doom
Password            gLastPassword_FDoom;        // Password for the current level the player is on: Final Doom
Password            gLastPassword_GecMe;        // Password for the current level the player is on: GEC Master Edition

#if PSYDOOM_3DS
    int32_t         gDetailMode;                // Rasterizer detail: -1 auto, 0 full, 1 low, 2 lowest
    bool            gbFullWidthVideo;           // Stretch the game image to the full width of the top screen
    int32_t         gStatusBarPos;              // Off the top screen gives the status bar rows to the 3D view
    int32_t         gControlScheme;             // Which named set of control bindings is in use
#endif

// Internally kept settings
static int32_t      gSoundVol;                      // Option for sound volume
static int32_t      gMusicVol;                      // Option for music volume
static bool         gbStartupWithVulkanRenderer;    // Startup using the Vulkan renderer? (if enabled, and the host machine is capable)

// If true then we save the prefs file to the current working directory rather than to the user data folder
static bool gbUseWorkingDirPrefsFile = false;

//------------------------------------------------------------------------------------------------------------------------------------------
// Converts a 'Password' object into a null terminated 'String16'
//------------------------------------------------------------------------------------------------------------------------------------------
static String16 getPasswordCString(const Password& password) noexcept {
    static_assert(sizeof(String16) > sizeof(Password));
    return String16(password.pwChars, sizeof(password.pwChars));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns the path to the ini file used to hold player prefs
//------------------------------------------------------------------------------------------------------------------------------------------
static std::string getPrefsFilePath() noexcept {
    if (gbUseWorkingDirPrefsFile) {
        return PREFS_FILE_NAME;
    } else {
        const std::string userDataFolder = Utils::getOrCreateUserDataFolder();
        return userDataFolder + PREFS_FILE_NAME;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns the 'Password' struct associated with the specified preferences field name
//------------------------------------------------------------------------------------------------------------------------------------------
static Password* getPasswordForPrefsFieldName(const char* const fieldName) noexcept {
    if (std::strcmp(fieldName, "lastPassword_Doom") == 0)
        return &gLastPassword_Doom;

    if (std::strcmp(fieldName, "lastPassword_FinalDoom") == 0)
        return &gLastPassword_FDoom;

    if (std::strcmp(fieldName, "lastPassword_GecMe") == 0)
        return &gLastPassword_GecMe;

    return nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Converts a character to a PSX Doom password character index.
// Returns -1 if there is no valid conversion.
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t charToPwCharIndex(const char c) noexcept {
    const char cUpper = (char) std::toupper(c);

    switch (cUpper) {
        case 'B': return 0;     case 'L': return 8;     case 'V': return 16;    case '3': return 24;
        case 'C': return 1;     case 'M': return 9;     case 'W': return 17;    case '4': return 25;
        case 'D': return 2;     case 'N': return 10;    case 'X': return 18;    case '5': return 26;
        case 'F': return 3;     case 'P': return 11;    case 'Y': return 19;    case '6': return 27;
        case 'G': return 4;     case 'Q': return 12;    case 'Z': return 20;    case '7': return 28;
        case 'H': return 5;     case 'R': return 13;    case '0': return 21;    case '8': return 29;
        case 'J': return 6;     case 'S': return 14;    case '1': return 22;    case '9': return 30;
        case 'K': return 7;     case 'T': return 15;    case '2': return 23;    case '!': return 31;
    }

    return -1;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Converts PSX Doom password character index to a character.
// Returns 'NUL' if there is no valid conversion.
//------------------------------------------------------------------------------------------------------------------------------------------
static char pwCharIndexToChar(const int32_t pwCharIdx) noexcept {
    switch (pwCharIdx) {
        case 0: return 'B';     case 8:  return 'L';    case 16: return 'V';    case 24: return '3';
        case 1: return 'C';     case 9:  return 'M';    case 17: return 'W';    case 25: return '4';
        case 2: return 'D';     case 10: return 'N';    case 18: return 'X';    case 26: return '5';
        case 3: return 'F';     case 11: return 'P';    case 19: return 'Y';    case 27: return '6';
        case 4: return 'G';     case 12: return 'Q';    case 20: return 'Z';    case 28: return '7';
        case 5: return 'H';     case 13: return 'R';    case 21: return '0';    case 29: return '8';
        case 6: return 'J';     case 14: return 'S';    case 22: return '1';    case 30: return '9';
        case 7: return 'K';     case 15: return 'T';    case 23: return '2';    case 31: return '!';
    }

    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Handle the loading of an entry in the .ini prefs file
//------------------------------------------------------------------------------------------------------------------------------------------
static void loadPrefsFileIniEntry(const IniUtils::IniEntry& entry) noexcept {
    if (entry.key == "soundVol") {
        gSoundVol = std::clamp(entry.value.tryGetAsInt(gSoundVol), VOLUME_MIN, VOLUME_MAX);
    } 
    else if (entry.key == "musicVol") {
        gMusicVol = std::clamp(entry.value.tryGetAsInt(gMusicVol), VOLUME_MIN, VOLUME_MAX);
    }
    else if (Password* const pPassword = getPasswordForPrefsFieldName(entry.key.c_str())) {
        // Read the password field up to the password length
        std::memset(pPassword, 0, sizeof(Password));
        std::memcpy(pPassword->pwChars, entry.value.strValue.c_str(), std::min(entry.value.strValue.length(), (size_t) PASSWORD_LEN));

        // Sanitize the password chars: set unrecognised ones to null and uppercase everything
        for (char& c : pPassword->pwChars) {
            c = (char) std::toupper(c);
            c = (charToPwCharIndex(c) >= 0) ? c : 0;
        }
    }
    else if (entry.key == "turnSpeedPercentMultiplier") {
        gTurnSpeedMult100 = std::clamp(entry.value.tryGetAsInt(gTurnSpeedMult100), TURN_SPEED_MULT_MIN, TURN_SPEED_MULT_MAX);
    }
    else if (entry.key == "alwaysRun") {
        gbAlwaysRun = entry.value.tryGetAsBool(gbAlwaysRun);
    }
#if PSYDOOM_3DS
    else if (entry.key == "detailMode") {
        gDetailMode = std::clamp(entry.value.tryGetAsInt(gDetailMode), DETAIL_MODE_AUTO, DETAIL_MODE_MAX);
    }
    else if (entry.key == "fullWidthVideo") {
        gbFullWidthVideo = entry.value.tryGetAsBool(gbFullWidthVideo);
    }
    else if (entry.key == "statusBarPos") {
        gStatusBarPos = std::clamp<int32_t>(entry.value.tryGetAsInt(gStatusBarPos), 0, STATUS_BAR_POS_COUNT - 1);
    }
    else if (entry.key == "controlScheme") {
        gControlScheme = std::clamp<int32_t>(entry.value.tryGetAsInt(gControlScheme), 0, ControlSchemes3DS::NUM_SCHEMES - 1);
    }
#else
    else if (entry.key == "uncapFramerate") {
        gbUncapFramerate = entry.value.tryGetAsBool(gbUncapFramerate);
    }
#endif
    else if (entry.key == "statDisplayMode") {
        gStatDisplayMode = (StatDisplayMode) entry.value.tryGetAsInt((int32_t) gStatDisplayMode);
        gStatDisplayMode = std::clamp(gStatDisplayMode, StatDisplayMode::None, StatDisplayMode::KillsSecretsAndItems);  // Ensure it's in range
    }
#if !PSYDOOM_3DS
    else if (entry.key == "startupWithVulkanRenderer") {
        gbStartupWithVulkanRenderer = entry.value.tryGetAsBool(gbStartupWithVulkanRenderer);
    }
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Set all preferences to the defaults
//------------------------------------------------------------------------------------------------------------------------------------------
void setToDefaults() noexcept {
    // Note: make sound volume 85 by default (Final Doom volume level) to make the music pop a bit more
    gSoundVol = 85;
    gMusicVol = 100;

    // Password is empty by default
    std::memset(&gLastPassword_Doom, 0, sizeof(gLastPassword_Doom));
    std::memset(&gLastPassword_FDoom, 0, sizeof(gLastPassword_FDoom));
    std::memset(&gLastPassword_GecMe, 0, sizeof(gLastPassword_GecMe));

    // Turn speed is normal by default, auto-run off and no stat display
    gTurnSpeedMult100 = 100;
    gbAlwaysRun = false;
    gStatDisplayMode = StatDisplayMode::None;

    // PsyDoom 3DS: never enough headroom to render in between game ticks, so stay on the original frame pacing.
    // Detail level is decided from the console model unless the player overrides it.
#if PSYDOOM_3DS
    gbUncapFramerate = false;
    gDetailMode = DETAIL_MODE_AUTO;
    gbFullWidthVideo = false;
    gStatusBarPos = STATUS_BAR_TOP_SCREEN;
    gControlScheme = (int32_t) ControlSchemes3DS::DEFAULT_SCHEME;
#else
    gbUncapFramerate = true;
#endif

    // Prefer the Vulkan renderer by default
#if PSYDOOM_3DS
    gbStartupWithVulkanRenderer = false;
#else
    gbStartupWithVulkanRenderer = true;
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Load the player preferences from the preferences file.
// If the file does not exist then the preferences are defaulted.
//------------------------------------------------------------------------------------------------------------------------------------------
void load() noexcept {
    // Firstly set everything to the defaults and determine whether we use a prefs file found in the current working directory.
    // We only do that if the .ini file is found there on launch!
    setToDefaults();
    gbUseWorkingDirPrefsFile = FileUtils::fileExists(PREFS_FILE_NAME);

    // Read the .ini file if it exists, otherwise stop here
    const std::string prefsFilePath = getPrefsFilePath();

    if (FileUtils::fileExists(prefsFilePath.c_str())) {
        const FileData prefsFileData = FileUtils::getContentsOfFile(prefsFilePath.c_str(), 1, std::byte(0));
        IniUtils::parseIniFromString((const char*) prefsFileData.bytes.get(), prefsFileData.size - 1, loadPrefsFileIniEntry);
    }

    // PsyDoom 3DS: push the loaded preferences to the parts of the engine that cache them
    #if PSYDOOM_3DS
        applyDetailMode();
        applyStatusBarPlacement();
        applyControlScheme();
    #endif
}

#if PSYDOOM_3DS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: resolve the detail preference and push it to the classic renderer's wall/floor fast paths
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t getEffectiveDetailMode() noexcept {
    if (gDetailMode >= DETAIL_MODE_MIN)
        return std::min(gDetailMode, DETAIL_MODE_MAX);

    // Auto: a New 3DS runs at 804 MHz with an L2 cache and can afford an extra column of detail
    return (Platform3DS::isNew3DS()) ? 1 : 2;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: give the 3D view the status bar's rows when the status bar has moved to the touch screen
//------------------------------------------------------------------------------------------------------------------------------------------
void applyStatusBarPlacement() noexcept {
    R_SetViewHeight((gStatusBarPos == STATUS_BAR_TOP_SCREEN) ? BASE_VIEW_3D_H : MAX_VIEW_3D_H);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: put the saved control scheme into effect.
//
// This runs after the bindings file has been read, so it can tell a scheme that is still intact from one the player
// has since edited underneath - in which case the selection quietly becomes 'Custom' and their edits stand.
//------------------------------------------------------------------------------------------------------------------------------------------
void applyControlScheme() noexcept {
    const ControlSchemes3DS::Scheme saved = (ControlSchemes3DS::Scheme) gControlScheme;
    gControlScheme = (int32_t) ControlSchemes3DS::resolveOnStartup(saved);
}

void setControlScheme(const int32_t scheme) noexcept {
    gControlScheme = std::clamp<int32_t>(scheme, 0, ControlSchemes3DS::NUM_SCHEMES - 1);
    ControlSchemes3DS::applyScheme((ControlSchemes3DS::Scheme) gControlScheme);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: put the default layout in place, but as a custom one.
//
// The bindings become the default scheme's, and the selection becomes 'Custom' rather than the default scheme, so the
// player can then edit them without the next launch putting the scheme back over the top of their changes.
//------------------------------------------------------------------------------------------------------------------------------------------
void resetControlsToDefault() noexcept {
    ControlSchemes3DS::applyScheme(ControlSchemes3DS::DEFAULT_SCHEME);
    gControlScheme = (int32_t) ControlSchemes3DS::Scheme::Custom;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: write everything out now. See the header for why this cannot wait until the game exits.
//------------------------------------------------------------------------------------------------------------------------------------------
void flushToDisk() noexcept {
    save();
    ConfigSerialization::writeAllConfigFiles(false);
}

void applyDetailMode() noexcept {
    switch (getEffectiveDetailMode()) {
        case 0:     Gpu::setLowDetailSteps(1, 1);   break;
        case 1:     Gpu::setLowDetailSteps(2, 1);   break;
        default:    Gpu::setLowDetailSteps(3, 2);   break;
    }
}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Save the player preferences the preferences file
//------------------------------------------------------------------------------------------------------------------------------------------
void save() noexcept {
    // Open up the prefs file
    const std::string prefsFilePath = getPrefsFilePath();
    std::FILE* const pFile = std::fopen(prefsFilePath.c_str(), "wt");

    if (!pFile)
        return;

    // Write out the preferences
    std::fprintf(pFile, "; WARNING: this file is auto-generated by PsyDoom, it may be overwritten at any time!\n");
    std::fprintf(pFile, "soundVol = %d\n", static_cast<int>(gSoundVol));
    std::fprintf(pFile, "musicVol = %d\n", static_cast<int>(gMusicVol));
    std::fprintf(pFile, "lastPassword_Doom = %s\n", getPasswordCString(gLastPassword_Doom).chars);
    std::fprintf(pFile, "lastPassword_FinalDoom = %s\n", getPasswordCString(gLastPassword_FDoom).chars);
    std::fprintf(pFile, "lastPassword_GecMe = %s\n", getPasswordCString(gLastPassword_GecMe).chars);
    std::fprintf(pFile, "turnSpeedPercentMultiplier = %d\n", static_cast<int>(gTurnSpeedMult100));
    std::fprintf(pFile, "alwaysRun = %d\n", (int) gbAlwaysRun);
#if PSYDOOM_3DS
    std::fprintf(pFile, "detailMode = %d\n", (int) gDetailMode);
    std::fprintf(pFile, "fullWidthVideo = %d\n", (int) gbFullWidthVideo);
    std::fprintf(pFile, "statusBarPos = %d\n", (int) gStatusBarPos);
    std::fprintf(pFile, "controlScheme = %d\n", (int) gControlScheme);
#else
    std::fprintf(pFile, "uncapFramerate = %d\n", (int) gbUncapFramerate);
#endif
    std::fprintf(pFile, "statDisplayMode = %d\n", (int) gStatDisplayMode);
#if !PSYDOOM_3DS
    std::fprintf(pFile, "startupWithVulkanRenderer = %d\n", (int) Video::isUsingVulkanRenderPath());
#endif

    // Flush and close to finish up
    std::fflush(pFile);
    std::fclose(pFile);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the turn speed multiplier expressed as a floating point fraction where 1.0 is 100%
//------------------------------------------------------------------------------------------------------------------------------------------
float getTurnSpeedMultiplier() noexcept {
    return (float) gTurnSpeedMult100 / 100.0f;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Apply the current sound and music preferences to the options sound and music preferences
//------------------------------------------------------------------------------------------------------------------------------------------
void pushSoundAndMusicPrefs() noexcept {
    gOptionsSndVol = gSoundVol;
    gOptionsMusVol = gMusicVol;
    gCdMusicVol = (gMusicVol * PSXSPU_MAX_CD_VOL) / S_MAX_VOL;      // Calculation copied from the options menu volume adjust
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Remember the current sound and music preferences stored in the options menu
//------------------------------------------------------------------------------------------------------------------------------------------
void pullSoundAndMusicPrefs() noexcept {
    gSoundVol = gOptionsSndVol;
    gMusicVol = gOptionsMusVol;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Apply the saved last password to the password system
//------------------------------------------------------------------------------------------------------------------------------------------
void pushLastPassword() noexcept {
    // Clear the current password
    gNumPasswordCharsEntered = 0;
    std::memset(gPasswordCharBuffer, 0, sizeof(gPasswordCharBuffer));

    // Add in password characters to the buffer until we encounter an invalid one
    ASSERT(Game::gConstants.pLastPasswordField);
    Password& lastPassword = *Game::gConstants.pLastPasswordField;

    for (const char pwChar : lastPassword.pwChars) {
        const int32_t pwCharIdx = charToPwCharIndex(pwChar);

        if (pwCharIdx < 0)
            break;

        gPasswordCharBuffer[gNumPasswordCharsEntered] = (uint8_t) pwCharIdx;
        gNumPasswordCharsEntered++;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Remember current password input in the password system, so that it may be saved later to player preferences
//------------------------------------------------------------------------------------------------------------------------------------------
void pullLastPassword() noexcept {
    ASSERT(Game::gConstants.pLastPasswordField);
    Password& lastPassword = *Game::gConstants.pLastPasswordField;
    std::memset(&lastPassword, 0, sizeof(lastPassword));

    for (int32_t i = 0; i < gNumPasswordCharsEntered; ++i) {
        lastPassword.pwChars[i] = pwCharIndexToChar(gPasswordCharBuffer[i]);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Should we startup using the Vulkan renderer where possible?
//------------------------------------------------------------------------------------------------------------------------------------------
bool shouldStartupWithVulkanRenderer() noexcept {
    return gbStartupWithVulkanRenderer;
}

END_NAMESPACE(PlayerPrefs)





