//------------------------------------------------------------------------------------------------------------------------------------------
// This is an entirely new menu added for PsyDoom.
// It provides extra options for turn sensitivity, autorun and renderer etc.
// It is not available in multiplayer, similar to other nested menus in the options screen.
//------------------------------------------------------------------------------------------------------------------------------------------
#if PSYDOOM_MODS

#include "xoptions_main.h"

#include "Doom/Base/i_main.h"
#include "Doom/Base/i_drawcmds.h"
#include "Doom/Base/i_misc.h"
#include "Doom/Base/s_sound.h"
#include "Doom/Base/sounds.h"
#include "Doom/d_main.h"
#include "Doom/Game/g_game.h"
#include "Doom/Game/p_tick.h"
#include "Doom/Renderer/r_data.h"
#include "m_main.h"
#include "o_main.h"

#if PSYDOOM_3DS
    #include "controls3ds_main.h"
#endif

#include "PsyDoom/Game.h"
#include "PsyDoom/PlayerPrefs.h"
#include "PsyDoom/Utils.h"

#if PSYDOOM_3DS
    #include "menu3ds.h"
    #include "PsyDoom/Screens3DS.h"
#endif
#include "PsyDoom/Video.h"
#include "PsyDoom/Vulkan/VRenderer.h"

#include <algorithm>
#include <cstdio>

// Where each row sits.
// PsyDoom 3DS: the touch screen only presents rows 24 to 216 of the framebuffer and the title has moved to the top
// screen, so the rows are pulled up to fit inside that band.
#if PSYDOOM_3DS
    static constexpr int16_t MENU_ROW_TURN_SPEED   = 30;
    static constexpr int16_t MENU_ROW_ALWAYS_RUN   = 70;
    static constexpr int16_t MENU_ROW_STAT_DISPLAY = 90;
    static constexpr int16_t MENU_ROW_DETAIL       = 110;
    static constexpr int16_t MENU_ROW_SCREEN_WIDTH = 130;
    static constexpr int16_t MENU_ROW_STATUS_BAR   = 150;
    static constexpr int16_t MENU_ROW_CONTROLS     = 170;
    static constexpr int16_t MENU_ROW_BACK         = 190;
#else
    static constexpr int16_t MENU_ROW_TURN_SPEED   = 50;
    static constexpr int16_t MENU_ROW_ALWAYS_RUN   = 90;
    static constexpr int16_t MENU_ROW_STAT_DISPLAY = 115;
    static constexpr int16_t MENU_ROW_DETAIL       = 140;
    static constexpr int16_t MENU_ROW_SCREEN_WIDTH = 150;
    static constexpr int16_t MENU_ROW_STATUS_BAR   = 160;
    static constexpr int16_t MENU_ROW_CONTROLS     = 165;
    static constexpr int16_t MENU_ROW_BACK         = 205;
#endif

// The available menu items
enum MenuItem : int32_t {
    menu_turn_speed,
    menu_always_run,
    menu_stat_display,
#if PSYDOOM_3DS
    menu_detail,
    menu_screen_width,
    menu_status_bar,
    menu_controls,
#else
    menu_uncapped_framerate,
#endif
#if PSYDOOM_VULKAN_RENDERER
    menu_renderer,
#endif
    menu_exit,
    num_menu_items
};

#if PSYDOOM_3DS
//------------------------------------------------------------------------------------------------------------------------------------------
// Set the rasterizer detail level (0 = full, 1 = low, 2 = lowest) and apply it immediately.
// Note this makes the preference explicit; picking a level opts out of the automatic New/Old 3DS choice.
//------------------------------------------------------------------------------------------------------------------------------------------
static void SetDetailLevel(const int32_t level) noexcept {
    PlayerPrefs::gDetailMode = std::clamp(level, PlayerPrefs::DETAIL_MODE_MIN, PlayerPrefs::DETAIL_MODE_MAX);
    PlayerPrefs::applyDetailMode();
}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Draw the cursor at the specified position
//------------------------------------------------------------------------------------------------------------------------------------------
static void DrawCursor(const int16_t cursorX, const int16_t cursorY) noexcept {
    I_DrawSprite(
        gTex_STATUS.texPageId,
        Game::getTexClut_STATUS(),
        (int16_t) cursorX - 24,
        (int16_t) cursorY - 2,
        (int16_t)(gTex_STATUS.texPageCoordX + M_SKULL_TEX_U + (uint8_t) gCursorFrame * M_SKULL_W),
        (int16_t)(gTex_STATUS.texPageCoordY + M_SKULL_TEX_V),
        M_SKULL_W,
        M_SKULL_H
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initializes the menu
//------------------------------------------------------------------------------------------------------------------------------------------
void XOptions_Init() noexcept {
    S_StartSound(nullptr, sfx_pistol);

    // Initialize cursor position and vblanks until move
    gCursorFrame = 0;
    gCursorPos[gCurPlayerIndex] = 0;
    gVBlanksUntilMenuMove[gCurPlayerIndex] = 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Shuts down the menu
//------------------------------------------------------------------------------------------------------------------------------------------
void XOptions_Shutdown([[maybe_unused]] const gameaction_t exitAction) noexcept {
    // PsyDoom 3DS: settings are changed here, and the game may never get a clean exit in which to save them
    #if PSYDOOM_3DS
        PlayerPrefs::flushToDisk();
    #endif

    gCursorPos[gCurPlayerIndex] = 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Runs update logic for the menu: does menu controls
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t XOptions_Update() noexcept {
    // PsyDoom: in all UIs tick only if vblanks are registered as elapsed; this restricts the code to ticking at 30 Hz for NTSC
    const uint32_t playerIdx = gCurPlayerIndex;

    if (gPlayersElapsedVBlanks[playerIdx] <= 0) {
        gbKeepInputEvents = true;   // Don't consume 'key pressed' etc. events yet, not ticking...
        return ga_nothing;
    }

    // Animate the skull cursor
    if ((gGameTic > gPrevGameTic) && ((gGameTic & 3) == 0)) {
        gCursorFrame ^= 1;
    }

    // Gather menu inputs and exit if the back button has just been pressed
    const TickInputs& inputs = gTickInputs[playerIdx];
    const TickInputs& oldInputs = gOldTickInputs[playerIdx];

    const bool bMenuBack = (inputs.fMenuBack() && (!oldInputs.fMenuBack()));
    bool bMenuOk = (inputs.fMenuOk() && (!oldInputs.fMenuOk()));
    const bool bMenuUp = inputs.fMenuUp();
    const bool bMenuDown = inputs.fMenuDown();
    const bool bMenuLeft = inputs.fMenuLeft();
    bool bMenuRight = inputs.fMenuRight();
    bool bMenuMove = (bMenuUp || bMenuDown || bMenuLeft || bMenuRight);

    // PsyDoom 3DS: tapping a row selects it; tapping the selected row cycles its value, or enters it for the two
    // rows that lead somewhere ('Controls' and 'Back').
    #if PSYDOOM_3DS
    {
        const int32_t tappedItem = Screens3DS::consumeTappedItem();

        if (tappedItem >= 0) {
            if (gCursorPos[playerIdx] != tappedItem) {
                gCursorPos[playerIdx] = tappedItem;
                S_StartSound(nullptr, sfx_pstop);
            }
            else if ((tappedItem == menu_controls) || (tappedItem == menu_exit)) {
                bMenuOk = true;
            }
            else {
                bMenuRight = true;
                bMenuMove = true;
                gVBlanksUntilMenuMove[playerIdx] = 0;
            }
        }
    }
    #endif

    if (bMenuBack) {
        S_StartSound(nullptr, sfx_pistol);
        return ga_exit;
    }

    // Check for up/down movement
    if (!bMenuMove) {
        // If there are no direction buttons pressed then the next move is allowed instantly
        gVBlanksUntilMenuMove[playerIdx] = 0;
    } else {
        // Direction buttons pressed or held down, check to see if we can move up/down now
        gVBlanksUntilMenuMove[playerIdx] -= gPlayersElapsedVBlanks[playerIdx];

        if (gVBlanksUntilMenuMove[playerIdx] <= 0) {
            gVBlanksUntilMenuMove[playerIdx] = 15;

            if (bMenuDown) {
                gCursorPos[playerIdx]++;

                if (gCursorPos[playerIdx] >= num_menu_items) {
                    gCursorPos[playerIdx] = 0;
                }

                S_StartSound(nullptr, sfx_pstop);
            }
            else if (bMenuUp) {
                gCursorPos[playerIdx]--;

                if (gCursorPos[playerIdx] < 0) {
                    gCursorPos[playerIdx] = num_menu_items - 1;
                }

                S_StartSound(nullptr, sfx_pstop);
            }
        }
    }

    // Handle option actions and adjustment
    switch ((MenuItem) gCursorPos[playerIdx]) {
        // Adjust turn speed
        case menu_turn_speed: {
            // Only process audio updates for this player
            if (bMenuRight) {
                PlayerPrefs::gTurnSpeedMult100++;

                if (PlayerPrefs::gTurnSpeedMult100 > PlayerPrefs::TURN_SPEED_MULT_MAX) {
                    PlayerPrefs::gTurnSpeedMult100 = PlayerPrefs::TURN_SPEED_MULT_MAX;
                } else {
                    if ((PlayerPrefs::gTurnSpeedMult100 / 4) & 1) {
                        S_StartSound(nullptr, sfx_stnmov);
                    }
                }
            }
            else if (bMenuLeft) {
                if (PlayerPrefs::gTurnSpeedMult100 > PlayerPrefs::TURN_SPEED_MULT_MIN) {
                    PlayerPrefs::gTurnSpeedMult100--;;

                    if ((PlayerPrefs::gTurnSpeedMult100 / 4) & 1) {
                        S_StartSound(nullptr, sfx_stnmov);
                    }
                } else {
                    PlayerPrefs::gTurnSpeedMult100 = PlayerPrefs::TURN_SPEED_MULT_MIN;
                }
            }
        }   break;

        // Turn on/off always run
        case menu_always_run: {
            if (bMenuLeft && (!oldInputs.fMenuLeft()) && PlayerPrefs::gbAlwaysRun) {
                PlayerPrefs::gbAlwaysRun = false;
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!oldInputs.fMenuRight()) && (!PlayerPrefs::gbAlwaysRun)) {
                PlayerPrefs::gbAlwaysRun = true;
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;

        // Stat display setting
        case menu_stat_display: {
            if (bMenuLeft && (!oldInputs.fMenuLeft()) && (PlayerPrefs::gStatDisplayMode > StatDisplayMode::None)) {
                PlayerPrefs::gStatDisplayMode = (StatDisplayMode)((int32_t) PlayerPrefs::gStatDisplayMode - 1);
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!oldInputs.fMenuRight()) && (PlayerPrefs::gStatDisplayMode < StatDisplayMode::KillsSecretsAndItems)) {
                PlayerPrefs::gStatDisplayMode = (StatDisplayMode)((int32_t) PlayerPrefs::gStatDisplayMode + 1);
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;

#if PSYDOOM_3DS
        // 3DS: how much detail the wall/floor rasterizers run at.
        // Right lowers detail (more speed), left raises it, matching how the other sliders read.
        case menu_detail: {
            const int32_t detail = PlayerPrefs::getEffectiveDetailMode();

            if (bMenuLeft && (!oldInputs.fMenuLeft()) && (detail > 0)) {
                SetDetailLevel(detail - 1);
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!oldInputs.fMenuRight()) && (detail < 2)) {
                SetDetailLevel(detail + 1);
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;
        // 3DS: stretch the game image across the whole top screen, or keep the PlayStation's aspect ratio
        case menu_screen_width: {
            if (bMenuLeft && (!oldInputs.fMenuLeft()) && PlayerPrefs::gbFullWidthVideo) {
                PlayerPrefs::gbFullWidthVideo = false;
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!oldInputs.fMenuRight()) && (!PlayerPrefs::gbFullWidthVideo)) {
                PlayerPrefs::gbFullWidthVideo = true;
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;

        // 3DS: where the status bar lives. Off the top screen its rows go to the 3D view.
        // This one cycles rather than clamping at the ends, since there are three places to put it.
        case menu_status_bar: {
            const bool bPressedRight = (bMenuRight && (!oldInputs.fMenuRight()));
            const bool bPressedLeft = (bMenuLeft && (!oldInputs.fMenuLeft()));
            const int32_t step = (bPressedRight) ? 1 : ((bPressedLeft) ? -1 : 0);

            if (step != 0) {
                PlayerPrefs::gStatusBarPos = (
                    (PlayerPrefs::gStatusBarPos + step + PlayerPrefs::STATUS_BAR_POS_COUNT) %
                    PlayerPrefs::STATUS_BAR_POS_COUNT
                );

                PlayerPrefs::applyStatusBarPlacement();
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;
#else
        // Turn on/off uncapped framerate
        case menu_uncapped_framerate: {
            if (bMenuLeft && PlayerPrefs::gbUncapFramerate) {
                PlayerPrefs::gbUncapFramerate = false;
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!PlayerPrefs::gbUncapFramerate)) {
                PlayerPrefs::gbUncapFramerate = true;
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;
#endif

    #if PSYDOOM_VULKAN_RENDERER
        // Renderer toggle
        case menu_renderer: {
            const bool bCanSwitchRenderers = (Video::gBackendType == Video::BackendType::Vulkan);

            if (bCanSwitchRenderers) {
                if (bMenuLeft && (!Video::isUsingVulkanRenderPath())) {
                    VRenderer::switchToMainVulkanRenderPath();
                    S_StartSound(nullptr, sfx_swtchx);
                }
                else if (bMenuRight && Video::isUsingVulkanRenderPath()) {
                    VRenderer::switchToPsxRenderPath();
                    S_StartSound(nullptr, sfx_swtchx);
                }
            }

            // If renderer switch is not possible and an attempt was made to do so then play this sound
            if (!bCanSwitchRenderers) {
                if ((bMenuLeft && (!oldInputs.fMenuLeft())) || (bMenuRight && (!oldInputs.fMenuRight()))) {
                    S_StartSound(nullptr, sfx_itemup);
                }
            }
        }   break;
    #endif  // #if PSYDOOM_VULKAN_RENDERER

#if PSYDOOM_3DS
        // Show the 3DS control reference screen
        case menu_controls: {
            if (bMenuOk) {
                MiniLoop(Controls3DS_Init, Controls3DS_Shutdown, Controls3DS_Update, Controls3DS_Draw);
                gbKeepInputEvents = true;
            }
        }   break;
#endif

        // Exit to the options menu
        case menu_exit: {
            if (bMenuOk) {
                S_StartSound(nullptr, sfx_pistol);
                return ga_exit;
            }
        } break;

        default:
            break;
    }

    return ga_nothing;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws the menu
//------------------------------------------------------------------------------------------------------------------------------------------
void XOptions_Draw() noexcept {
    // Increment the frame count for the texture cache and draw the background
    I_IncDrawnFrameCount();
    Utils::onBeginUIDrawing();

    // PsyDoom 3DS: the top screen is composed separately, with the background and the title only
    #if PSYDOOM_3DS
        Menu3DS_DrawTitleScreen(gTex_OptionsBg, Game::getTexClut_OptionsBg(), "Extra Options");
    #endif

    O_DrawBackground(gTex_OptionsBg, Game::getTexClut_OptionsBg(), 128, 128, 128);

    // Don't do any rendering if we are about to exit the menu
    if (gGameAction == ga_nothing) {
        // Menu title: on 3DS this lives on the top screen instead
        #if !PSYDOOM_3DS
            I_DrawString(-1, 20, "Extra Options");
        #endif

        // Draw the turn speed slider
        int16_t cursorX = 62;
        int16_t cursorY = MENU_ROW_TURN_SPEED;

        {
            // Draw the label for the slider
            const int16_t menuItemX = 62;
            const int16_t menuItemY = MENU_ROW_TURN_SPEED;

            int32_t turnSpeed = PlayerPrefs::gTurnSpeedMult100;
            char turnSpeedLabel[32];

            std::snprintf(
                turnSpeedLabel,
                sizeof(turnSpeedLabel),
                "Turn Speed %d.%02d",
                static_cast<int>(turnSpeed / 100),
                static_cast<int>(turnSpeed % 100)
            );

            I_DrawString(menuItemX, menuItemY, turnSpeedLabel);

            // Draw the slider background
            I_DrawSprite(
                gTex_STATUS.texPageId,
                Game::getTexClut_STATUS(),
                (int16_t)(menuItemX + 13),
                (int16_t)(menuItemY + 20),
                (int16_t)(gTex_STATUS.texPageCoordX + 0),
                (int16_t)(gTex_STATUS.texPageCoordY + 184),
                108,
                11
            );

            // Draw the slider handle
            const int16_t sliderVal = (int16_t)(PlayerPrefs::gTurnSpeedMult100 / 5);

            I_DrawSprite(
                gTex_STATUS.texPageId,
                Game::getTexClut_STATUS(),
                (int16_t)(menuItemX + 14 + sliderVal),
                (int16_t)(menuItemY + 20),
                (int16_t)(gTex_STATUS.texPageCoordX + 108),
                (int16_t)(gTex_STATUS.texPageCoordY + 184),
                6,
                11
            );
        }

        // PsyDoom 3DS: make every row touchable
        #if PSYDOOM_3DS
            constexpr int16_t TOUCH_LEAD = 2;   // The band starts a little above the text it belongs to
            constexpr int16_t TOUCH_H = MENU_ROW_STAT_DISPLAY - MENU_ROW_ALWAYS_RUN;

            // The turn speed row owns its slider too, so its band runs down to the row underneath
            Screens3DS::addTouchItem(menu_turn_speed, MENU_ROW_TURN_SPEED - TOUCH_LEAD, MENU_ROW_ALWAYS_RUN - MENU_ROW_TURN_SPEED);
            Screens3DS::addTouchItem(menu_always_run, MENU_ROW_ALWAYS_RUN - TOUCH_LEAD, TOUCH_H);
            Screens3DS::addTouchItem(menu_stat_display, MENU_ROW_STAT_DISPLAY - TOUCH_LEAD, TOUCH_H);
            Screens3DS::addTouchItem(menu_detail, MENU_ROW_DETAIL - TOUCH_LEAD, TOUCH_H);
            Screens3DS::addTouchItem(menu_screen_width, MENU_ROW_SCREEN_WIDTH - TOUCH_LEAD, TOUCH_H);
            Screens3DS::addTouchItem(menu_status_bar, MENU_ROW_STATUS_BAR - TOUCH_LEAD, TOUCH_H);
            Screens3DS::addTouchItem(menu_controls, MENU_ROW_CONTROLS - TOUCH_LEAD, TOUCH_H);
            Screens3DS::addTouchItem(menu_exit, MENU_ROW_BACK - TOUCH_LEAD, TOUCH_H);
        #endif

        // Draw the always run option
        I_DrawString(62, MENU_ROW_ALWAYS_RUN, (PlayerPrefs::gbAlwaysRun) ? "Always Run On" : "Always Run Off");

        if (gCursorPos[gCurPlayerIndex] == menu_always_run) {
            cursorY = MENU_ROW_ALWAYS_RUN;
        }

        // Draw the stats display option
        const char* statDisplayStr = "Stat Display Off";

        if (PlayerPrefs::gStatDisplayMode >= StatDisplayMode::KillsSecretsAndItems) {
            statDisplayStr = "Stat Display KSI";
        } else if (PlayerPrefs::gStatDisplayMode >= StatDisplayMode::KillsAndSecrets) {
            statDisplayStr = "Stat Display KS";
        } else if (PlayerPrefs::gStatDisplayMode >= StatDisplayMode::Kills) {
            statDisplayStr = "Stat Display K";
        }

        I_DrawString(62, MENU_ROW_STAT_DISPLAY, statDisplayStr);

        if (gCursorPos[gCurPlayerIndex] == menu_stat_display) {
            cursorY = MENU_ROW_STAT_DISPLAY;
        }

#if PSYDOOM_3DS
        // Draw the detail level option
        {
            const int32_t detail = PlayerPrefs::getEffectiveDetailMode();
            const char* const detailStr =
                (detail <= 0) ? "Detail Full" :
                (detail == 1) ? "Detail Low" : "Detail Lowest";

            I_DrawString(62, MENU_ROW_DETAIL, detailStr);

            if (gCursorPos[gCurPlayerIndex] == menu_detail) {
                cursorY = MENU_ROW_DETAIL;
            }
        }

        // Draw the screen width option
        I_DrawString(62, MENU_ROW_SCREEN_WIDTH, (PlayerPrefs::gbFullWidthVideo) ? "Screen Full Width" : "Screen 4 By 3");

        if (gCursorPos[gCurPlayerIndex] == menu_screen_width) {
            cursorY = MENU_ROW_SCREEN_WIDTH;
        }

        // Draw the status bar placement option
        const char* const statusBarPosName = (
            (PlayerPrefs::gStatusBarPos == PlayerPrefs::STATUS_BAR_TOUCH_TOP) ? "Hud Touch Top" :
            (PlayerPrefs::gStatusBarPos == PlayerPrefs::STATUS_BAR_TOUCH_BOTTOM) ? "Hud Touch Below" :
            "Hud Top Screen"
        );

        I_DrawString(62, MENU_ROW_STATUS_BAR, statusBarPosName);

        if (gCursorPos[gCurPlayerIndex] == menu_status_bar) {
            cursorY = MENU_ROW_STATUS_BAR;
        }

        // Draw the 3DS control reference option
        I_DrawString(62, MENU_ROW_CONTROLS, "Controls");

        if (gCursorPos[gCurPlayerIndex] == menu_controls) {
            cursorY = MENU_ROW_CONTROLS;
        }
#else
        // Draw the uncapped framerate option
        I_DrawString(62, 140, (PlayerPrefs::gbUncapFramerate) ? "Uncapped FPS" : "Original FPS");

        if (gCursorPos[gCurPlayerIndex] == menu_uncapped_framerate) {
            cursorY = 140;
        }
#endif

        #if PSYDOOM_VULKAN_RENDERER
            // Draw the renderer option
            const bool bIsUsingVulkan = Video::isUsingVulkanRenderPath();
            I_DrawString(62, 165, (bIsUsingVulkan) ? "Vulkan Renderer" : "Classic Renderer");

            if (gCursorPos[gCurPlayerIndex] == menu_renderer) {
                cursorY = 165;
            }
        #endif

        // Draw the exit option
        I_DrawString(62, MENU_ROW_BACK, "Back");

        if (gCursorPos[gCurPlayerIndex] == menu_exit) {
            cursorY = MENU_ROW_BACK;
        }

        // PsyDoom 3DS: which build this is, along the bottom.
        //
        // It is in the startup log too, but reading that means taking the SD card out of both consoles. Two players
        // comparing what they are running should be able to do it by looking at the screen, and working out whether
        // two consoles are on the same build has already cost several rounds of testing.
        #if PSYDOOM_3DS
        {
            // The small font is sprites from the STATUS page, so point the GPU at it: the menu has left it elsewhere
            DR_MODE drawModePrim = {};
            const SRECT texWindow = { (int16_t) gTex_STATUS.texPageCoordX, (int16_t) gTex_STATUS.texPageCoordY, 256, 256 };
            LIBGPU_SetDrawMode(drawModePrim, false, false, gTex_STATUS.texPageId, &texWindow);
            I_AddPrim(drawModePrim);

            I_DrawStringSmall(8, MENU_ROW_BACK + 22, "BUILD " PSYDOOM_3DS_BUILD_ID, Game::getTexClut_STATUS(), 96, 96, 96, false, true);
        }
        #endif

        // Draw the skull cursor
        DrawCursor(cursorX, cursorY);
    }

    // PsyDoom: draw any enabled performance counters
    #if PSYDOOM_MODS
        I_DrawEnabledPerfCounters();
    #endif

    // Finish up the frame
    I_SubmitGpuCmds();
    I_DrawPresent();
}

#endif  // #if PSYDOOM_MODS






