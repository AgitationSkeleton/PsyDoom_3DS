#include "am_main.h"

#include "Doom/Base/i_drawcmds.h"
#include "Doom/Base/i_main.h"
#include "Doom/Base/m_fixed.h"
#include "Doom/doomdef.h"
#include "Doom/Game/doomdata.h"
#include "Doom/Game/g_game.h"
#include "Doom/Game/p_local.h"
#include "Doom/Game/p_setup.h"
#include "Doom/Game/p_tick.h"
#include "Doom/Renderer/r_local.h"
#include "Doom/Renderer/r_main.h"
#include "Doom/RendererVk/rv_automap.h"
#include "PsyDoom/Config/Config.h"
#include "PsyDoom/PlayerPrefs.h"

#if PSYDOOM_3DS
    #include "PsyDoom/Screens3DS.h"

    #include <chrono>
#endif
#include "PsyDoom/PsxPadButtons.h"
#include "PsyDoom/Utils.h"
#include "PsyDoom/Video.h"
#include "PsyQ/LIBETC.h"
#include "PsyQ/LIBGPU.h"

#include <algorithm>
#include <cstdlib>

static constexpr fixed_t MOVESTEP   = FRACUNIT * 128;   // Controls how fast manual automap movement happens
static constexpr fixed_t SCALESTEP  = 2;                // How fast to scale in/out
static constexpr int32_t MAXSCALE   = 64;               // Maximum map zoom
static constexpr int32_t MINSCALE   = 8;                // Minimum map zoom

static fixed_t  gAutomapXMin;
static fixed_t  gAutomapXMax;
static fixed_t  gAutomapYMin;
static fixed_t  gAutomapYMax;

// Internal module functions
static void DrawLine(const uint32_t color, const int32_t x1, const int32_t y1, const int32_t x2, const int32_t y2) noexcept;

#if PSYDOOM_3DS
    static AMExternalLineDrawer gpExternalLineDrawer = nullptr;
    static void* gpExternalLineDrawerUserData = nullptr;
#endif

#if PSYDOOM_MODS

// The position and rotation to use for the player this frame on the automap, and the free camera position and camera zoom
static fixed_t gAM_PlayerX;
static fixed_t gAM_PlayerY;
static angle_t gAM_PlayerAngle;
static fixed_t gAM_AutomapX;
static fixed_t gAM_AutomapY;
static fixed_t gAM_AutomapScale;

//------------------------------------------------------------------------------------------------------------------------------------------
// Compute the position and rotation to use for the automap for the player, taking into account framerate independent movement.
// Also does the same for the 'free camera' automap position that is used when the player is manually panning over the map.
//------------------------------------------------------------------------------------------------------------------------------------------
static void AM_CalcPlayerMapTransforms() noexcept {
    const player_t& player = gPlayers[gCurPlayerIndex];

    R_CalcLerpFactors();
    const bool bUncapFramerate = PlayerPrefs::gbUncapFramerate;

    if (bUncapFramerate) {
        const fixed_t lerpFactor = gPlayerLerpFactor;
        gAM_PlayerX = R_LerpCoord(gOldViewX, player.mo->x, lerpFactor);
        gAM_PlayerY = R_LerpCoord(gOldViewY, player.mo->y, lerpFactor);
        gAM_PlayerAngle = R_LerpAngle(gOldViewAngle, player.mo->angle, lerpFactor);
        gAM_AutomapX = R_LerpCoord(gOldAutomapX, player.automapx, lerpFactor);
        gAM_AutomapY = R_LerpCoord(gOldAutomapY, player.automapy, lerpFactor);
        gAM_AutomapScale = R_LerpCoord(gOldAutomapScale * FRACUNIT, player.automapscale * FRACUNIT, lerpFactor);
    } else {
        gAM_PlayerX = player.mo->x;
        gAM_PlayerY = player.mo->y;
        gAM_PlayerAngle = player.mo->angle;
        gAM_AutomapX = player.automapx;
        gAM_AutomapY = player.automapy;
        gAM_AutomapScale = player.automapscale * FRACUNIT;
    }
}

#endif  // #if PSYDOOM_MODS

//------------------------------------------------------------------------------------------------------------------------------------------
// Automap initialization logic
//------------------------------------------------------------------------------------------------------------------------------------------
void AM_Start() noexcept {
    gAutomapXMin = gBlockmapOriginX;
    gAutomapYMin = gBlockmapOriginY;
    gAutomapXMax = d_lshift<MAPBLOCKSHIFT>(gBlockmapWidth) + gBlockmapOriginX;
    gAutomapYMax = d_lshift<MAPBLOCKSHIFT>(gBlockmapHeight) + gBlockmapOriginY;
}

#if PSYDOOM_3DS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: drag the automap around with the stylus.
//
// The touch screen shows the automap the whole time the player is in a level, so panning it with a held button and the
// D-Pad, as the PlayStation version did, would waste buttons that gameplay needs. Dragging is the obvious gesture on a
// touch screen. It snaps back to the player once they have actually walked somewhere, so the map cannot be left
// stranded somewhere unhelpful.
//
// "Actually walked" rather than "moved at all": movement is on an analog stick, and a stick at rest still reports a
// little of something, so the player's position is never perfectly still. Snapping back on any change at all meant the
// map returned to the player the instant the stylus lifted, every time, which looked like panning was broken.
//
// There was a deliberate snap back on a quick double tap as well, but a touch screen being dragged produces stray taps
// constantly and it fired almost every time the map was touched. Walking snaps back on its own, so nothing is lost by
// dropping it.
//------------------------------------------------------------------------------------------------------------------------------------------
static void AM_Update3DSTouchPan(player_t& player) noexcept {
    // How the touch screen scales the automap's own coordinate space, as a fraction: it draws six pixels for every
    // five units, the same in both directions. Panning has to undo exactly that to keep up with the stylus.
    constexpr int64_t AUTOMAP_SCALE_NUM = 6;
    constexpr int64_t AUTOMAP_SCALE_DEN = 5;

    // How far the player has to travel from where the map was pulled away before it goes back to them.
    // Comfortably more than an analog stick's drift, comfortably less than a step.
    constexpr fixed_t SNAP_BACK_DIST = 48 * FRACUNIT;

    static bool     bWasDown = false;
    static int32_t  lastTouchX = 0;
    static int32_t  lastTouchY = 0;
    static fixed_t  panAnchorX = 0;     // Where the player was when the map was pulled away from them
    static fixed_t  panAnchorY = 0;

    const Screens3DS::Touch& touch = Screens3DS::getTouch();
    const int32_t touchX = Screens3DS::getRawTouchX();
    const int32_t touchY = Screens3DS::getRawTouchY();

    // Once the player has walked away from where they were when the map was pulled off them, take it back
    if (((player.automapflags & AF_FOLLOW) != 0) && (!touch.bDown)) {
        const fixed_t movedX = std::abs(player.mo->x - panAnchorX);
        const fixed_t movedY = std::abs(player.mo->y - panAnchorY);

        if ((movedX > SNAP_BACK_DIST) || (movedY > SNAP_BACK_DIST)) {
            player.automapflags &= ~AF_FOLLOW;
        }
    }

    // Nothing being touched: whatever drag there was is over
    if (!touch.bDown) {
        bWasDown = false;
        return;
    }

    // First look at a new touch: note where it started and wait for it to move. Deliberately keyed off the stylus
    // being down rather than off the frame it went down on, which this may never see.
    if (!bWasDown) {
        bWasDown = true;
        lastTouchX = touchX;
        lastTouchY = touchY;
        return;
    }

    const int32_t dragX = touchX - lastTouchX;
    const int32_t dragY = touchY - lastTouchY;
    lastTouchX = touchX;
    lastTouchY = touchY;

    if ((dragX == 0) && (dragY == 0))
        return;

    // A stylus cannot travel this far in one tick. If it appears to have, tracking was lost rather than the player
    // having flicked across the screen, and acting on it would throw the map somewhere unrelated.
    constexpr int32_t MAX_DRAG_PIXELS_PER_TICK = 80;

    if ((std::abs(dragX) > MAX_DRAG_PIXELS_PER_TICK) || (std::abs(dragY) > MAX_DRAG_PIXELS_PER_TICK))
        return;

    // Starting a drag takes the map off the player; seed the free camera where the player currently is so the map does
    // not jump before it starts moving
    if ((player.automapflags & AF_FOLLOW) == 0) {
        player.automapflags |= AF_FOLLOW;
        player.automapx = player.mo->x;
        player.automapy = player.mo->y;
        panAnchorX = player.mo->x;
        panAnchorY = player.mo->y;
    }

    // Undo the automap's world to screen transform to work out how far the world moved under the stylus.
    //
    // A line is drawn at 'screen = ((world - origin) / SCREEN_W) * scale >> FRACBITS', and the touch screen then draws
    // that at six pixels per five units. Inverting both gives the world delta for one pixel of drag. This is done in
    // 64 bits because the intermediate easily exceeds a 32 bit fixed_t.
    const int32_t scale = std::max<int32_t>(player.automapscale, 1);
    const int64_t dragMapX = ((int64_t) dragX * AUTOMAP_SCALE_DEN) / AUTOMAP_SCALE_NUM;
    const int64_t dragMapY = -((int64_t) dragY * AUTOMAP_SCALE_DEN) / AUTOMAP_SCALE_NUM;

    // Dragging right should pull the map contents right, which means moving the camera left
    player.automapx -= (fixed_t)((dragMapX * FRACUNIT * SCREEN_W) / scale);
    player.automapy -= (fixed_t)((dragMapY * FRACUNIT * SCREEN_W) / scale);

    player.automapx = std::clamp(player.automapx, gAutomapXMin, gAutomapXMax);
    player.automapy = std::clamp(player.automapy, gAutomapYMin, gAutomapYMax);
}
#endif  // #if PSYDOOM_3DS

//------------------------------------------------------------------------------------------------------------------------------------------
// Update logic for the automap: handles player input & controls
//------------------------------------------------------------------------------------------------------------------------------------------
void AM_Control(player_t& player) noexcept {
    // If the game is paused we do nothing
    if (gbGamePaused)
        return;

    // PsyDoom: if the external camera is active do nothing and clear the current automap flags
    #if PSYDOOM_MODS
        if (gExtCameraTicsLeft > 0) {
            player.automapflags &= ~AF_ACTIVE;
            return;
        }
    #endif

    // Toggle the automap on and off if select has just been pressed
    #if PSYDOOM_MODS
        TickInputs& inputs = gTickInputs[gPlayerNum];
        const TickInputs& oldInputs = gOldTickInputs[gPlayerNum];

        const bool bMenuBack = (inputs.fToggleMap() && (!oldInputs.fToggleMap()));
        const bool bAutomapPan = inputs.fAutomapPan();
        const bool bPanFast = inputs.fRun();
        const bool bAutomapMoveLeft = inputs.fAutomapMoveLeft();
        const bool bAutomapMoveRight = inputs.fAutomapMoveRight();
        const bool bAutomapMoveUp = inputs.fAutomapMoveUp();
        const bool bAutomapMoveDown = inputs.fAutomapMoveDown();
        const bool bAutomapZoomIn = inputs.fAutomapZoomIn();
        const bool bAutomapZoomOut = inputs.fAutomapZoomOut();
    #else
        const padbuttons_t ticButtons = gTicButtons[gPlayerNum];
        const padbuttons_t oldTicButtons = gOldTicButtons[gPlayerNum];

        const bool bMenuBack = ((ticButtons & PAD_SELECT) && ((oldTicButtons & PAD_SELECT) == 0));
        const bool bAutomapPan = (ticButtons & PAD_CROSS);
        const bool bPanFast = (ticButtons & PAD_SQUARE);
        const bool bAutomapMoveLeft = (ticButtons & PAD_LEFT);
        const bool bAutomapMoveRight = (ticButtons & PAD_RIGHT);
        const bool bAutomapMoveUp = (ticButtons & PAD_UP);
        const bool bAutomapMoveDown = (ticButtons & PAD_DOWN);
        const bool bAutomapZoomIn = (ticButtons & PAD_L1);
        const bool bAutomapZoomOut = (ticButtons & PAD_R1);
    #endif

    if (bMenuBack) {
        player.automapflags ^= AF_ACTIVE;
        player.automapx = player.mo->x;
        player.automapy = player.mo->y;
    }

    // PsyDoom 3DS: the touch screen shows the automap for the whole level, so stylus panning is handled before the
    // check below, which only concerns the full screen automap
    #if PSYDOOM_3DS
        // Note: only for the player sitting in front of this console. In a netgame 'AM_Control' runs for both players.
        if ((player.playerstate == PST_LIVE) && (gPlayerNum == gCurPlayerIndex)) {
            AM_Update3DSTouchPan(player);
        }
    #endif

    // If the automap is not active or the player dead then do nothing
    if ((player.automapflags & AF_ACTIVE) == 0)
        return;

    if (player.playerstate != PST_LIVE)
        return;

    // Follow the player unless the cross button is pressed.
    // The rest of the logic is for when we are NOT following the player.
    if (!bAutomapPan) {
        player.automapflags &= ~AF_FOLLOW;
        return;
    }

    // Snap the manual automap movement position to the player location once we transition from following to not following
    if ((player.automapflags & AF_FOLLOW) == 0) {
        player.automapflags |= AF_FOLLOW;
        player.automapx = player.mo->x;
        player.automapy = player.mo->y;
    }

    // Figure out the movement amount for manual camera movement
    const fixed_t moveStep = (bPanFast) ? MOVESTEP * 2 : MOVESTEP;

    // Not sure why this check was done, it can never be true due to the logic above.
    // PsyDoom: remove this block as it is useless...
    #if !PSYDOOM_MODS
        if ((player.automapflags & AF_FOLLOW) == 0)
            return;
    #endif

    // Left/right movement
    if (bAutomapMoveRight) {
        player.automapx += moveStep;

        if (player.automapx > gAutomapXMax) {
            player.automapx = gAutomapXMax;
        }
    }
    else if (bAutomapMoveLeft) {
        player.automapx -= moveStep;

        if (player.automapx < gAutomapXMin) {
            player.automapx = gAutomapXMin;
        }
    }

    // Up/down movement
    if (bAutomapMoveUp) {
        player.automapy += moveStep;

        if (player.automapy > gAutomapYMax) {
            player.automapy = gAutomapYMax;
        }
    }
    else if (bAutomapMoveDown) {
        player.automapy -= moveStep;

        if (player.automapy < gAutomapYMin) {
            player.automapy = gAutomapYMin;
        }
    }

    // Scale up and down
    if (bAutomapZoomOut) {
        player.automapscale -= SCALESTEP;

        if (player.automapscale < MINSCALE) {
            player.automapscale = MINSCALE;
        }
    }
    else if (bAutomapZoomIn) {
        player.automapscale += SCALESTEP;

        if (player.automapscale > MAXSCALE) {
            player.automapscale = MAXSCALE;
        }
    }

    // When not in follow mode, consume these inputs so that we don't move the player in the level
    #if PSYDOOM_MODS
        inputs.fMoveForward() = false;
        inputs.fMoveBackward() = false;
        inputs.fAttack() = false;
        inputs.fTurnLeft() = false;
        inputs.fTurnRight() = false;
        inputs.fStrafeLeft() = false;
        inputs.fStrafeRight() = false;
        inputs.setAnalogForwardMove(0);
        inputs.setAnalogSideMove(0);
        inputs.setAnalogTurn(0);

        // If zooming in and out then don't allow weapons to be changed.
        // This fixes situations where for example the mouse wheel is bound to both switch weapons and zoom in/out.
        if (bAutomapZoomIn || bAutomapZoomOut) {
            inputs.directSwitchToWeapon = wp_nochange;
            inputs.fNextWeapon() = false;
            inputs.fPrevWeapon() = false;
        }
    #else
        gTicButtons[gPlayerNum] &= ~(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_R1 | PAD_L1);
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Does drawing for the automap
//------------------------------------------------------------------------------------------------------------------------------------------
void AM_Drawer() noexcept {
    // Finish up the previous frame.
    // PsyDoom: moved this to the end of 'P_Drawer' instead - needed for the new Vulkan renderer integration.
    #if !PSYDOOM_MODS
        I_DrawPresent();
    #endif

    // PsyDoom: if the Vulkan renderer is active then delegate automap drawing to that.
    // Otherwise compute the player map transforms to use, taking into account framerate independent movement.
    #if PSYDOOM_MODS
        #if PSYDOOM_VULKAN_RENDERER
            if (Video::isUsingVulkanRenderPath()) {
                RV_DrawAutomap();
                return;
            }
        #endif

        AM_CalcPlayerMapTransforms();
    #endif

    // Determine the scale to render the map at.
    // PsyDoom: use framerate uncapped scaling here.
    const player_t& curPlayer = gPlayers[gCurPlayerIndex];

    #if PSYDOOM_MODS
        const fixed_t scale = gAM_AutomapScale;
    #else
        const int32_t scale = curPlayer.automapscale;
    #endif

    // Determine the map camera origin depending on follow mode status.
    // PsyDoom: use framerate uncapped positions here.
    fixed_t ox, oy;

    #if PSYDOOM_MODS
        if (curPlayer.automapflags & AF_FOLLOW) {
            ox = gAM_AutomapX;
            oy = gAM_AutomapY;
        } else {
            ox = gAM_PlayerX;
            oy = gAM_PlayerY;
        }
    #else
        if (curPlayer.automapflags & AF_FOLLOW) {
            ox = curPlayer.automapx;
            oy = curPlayer.automapy;
        } else {
            ox = curPlayer.mo->x;
            oy = curPlayer.mo->y;
        }
    #endif

    // Draw all the map lines
    {
        const line_t* pLine = gpLines;

        for (int32_t lineIdx = 0; lineIdx < gNumLines; ++lineIdx, ++pLine) {
            // See whether we should draw the automap line or not
            const bool bHiddenLine = (pLine->flags & ML_DONTDRAW);
            const bool bLineSeen = ((pLine->flags & ML_MAPPED) && (!bHiddenLine));

            #if PSYDOOM_MODS
                // PsyDoom: bug fix: if the line is marked as invisible then the allmap powerup shouldn't reveal it.
                // This change makes the behavior consistent with Linux Doom.
                const bool bLineMapped = ((curPlayer.powers[pw_allmap]) && (!bHiddenLine));
            #else
                const bool bLineMapped = (curPlayer.powers[pw_allmap]);
            #endif

            const bool bAllLinesCheatOn = (curPlayer.cheats & CF_ALLLINES);
            const bool bDraw = (bLineSeen || bLineMapped || bAllLinesCheatOn);

            if (!bDraw)
                continue;

            // Compute the line points in viewspace.
            // PsyDoom: scale is now a fixed point number due to framerate uncapped automap movement.
            #if PSYDOOM_MODS
                const int32_t x1 = d_fixed_to_int(FixedMul((pLine->vertex1->x - ox) / SCREEN_W, scale));
                const int32_t y1 = d_fixed_to_int(FixedMul((pLine->vertex1->y - oy) / SCREEN_W, scale));
                const int32_t x2 = d_fixed_to_int(FixedMul((pLine->vertex2->x - ox) / SCREEN_W, scale));
                const int32_t y2 = d_fixed_to_int(FixedMul((pLine->vertex2->y - oy) / SCREEN_W, scale));
            #else
                const int32_t x1 = d_fixed_to_int(((pLine->vertex1->x - ox) / SCREEN_W) * scale);
                const int32_t y1 = d_fixed_to_int(((pLine->vertex1->y - oy) / SCREEN_W) * scale);
                const int32_t x2 = d_fixed_to_int(((pLine->vertex2->x - ox) / SCREEN_W) * scale);
                const int32_t y2 = d_fixed_to_int(((pLine->vertex2->y - oy) / SCREEN_W) * scale);
            #endif

            // Decide on line color: start off with the normal two sided line color to begin with
            uint32_t color = AM_COLOR_BROWN;

            if (((curPlayer.cheats & CF_ALLLINES) + curPlayer.powers[pw_allmap] != 0) && ((pLine->flags & ML_MAPPED) == 0)) {
                // A known line (due to all map cheat/powerup) but unseen
                color = AM_COLOR_GREY;
            }
            else if (pLine->flags & ML_SECRET) {
                // Secret
                color = AM_COLOR_RED;
            }
            else if (pLine->special != 0) {
                // Special or activatable thing
                color = AM_COLOR_YELLOW;
            }
            else if ((pLine->flags & ML_TWOSIDED) == 0) {
                // One sided line
                color = AM_COLOR_RED;
            }

            DrawLine(color, x1, y1, x2, y2);
        }
    }

    // Show all map things cheat: display a little wireframe triangle for for all things
    if (curPlayer.cheats & CF_ALLMOBJ) {
        for (mobj_t* pMobj = gMobjHead.next; pMobj != &gMobjHead; pMobj = pMobj->next) {
            // Ignore the player for this particular draw
            if (pMobj == curPlayer.mo)
                continue;

            // Compute the the sine and cosines for the angles of the 3 points in the triangle
            const uint32_t fineAng1 = (pMobj->angle                ) >> ANGLETOFINESHIFT;
            const uint32_t fineAng2 = (pMobj->angle - ANG90 - ANG45) >> ANGLETOFINESHIFT;
            const uint32_t fineAng3 = (pMobj->angle + ANG90 + ANG45) >> ANGLETOFINESHIFT;

            const fixed_t cos1 = gFineCosine[fineAng1];
            const fixed_t cos2 = gFineCosine[fineAng2];
            const fixed_t cos3 = gFineCosine[fineAng3];

            const fixed_t sin1 = gFineSine[fineAng1];
            const fixed_t sin2 = gFineSine[fineAng2];
            const fixed_t sin3 = gFineSine[fineAng3];

            // Compute the line points.
            // PsyDoom: scale is now a fixed point number due to framerate uncapped automap movement. Also supporting interpolation here.
            #if PSYDOOM_MODS
                const fixed_t vx = pMobj->x.renderValue() - ox;
                const fixed_t vy = pMobj->y.renderValue() - oy;

                const int32_t x1 = d_fixed_to_int(FixedMul((vx + cos1 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
                const int32_t y1 = d_fixed_to_int(FixedMul((vy + sin1 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
                const int32_t x2 = d_fixed_to_int(FixedMul((vx + cos2 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
                const int32_t y2 = d_fixed_to_int(FixedMul((vy + sin2 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
                const int32_t x3 = d_fixed_to_int(FixedMul((vx + cos3 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
                const int32_t y3 = d_fixed_to_int(FixedMul((vy + sin3 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
            #else
                const fixed_t vx = pMobj->x - ox;
                const fixed_t vy = pMobj->y - oy;

                const int32_t x1 = d_fixed_to_int(((vx + cos1 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
                const int32_t y1 = d_fixed_to_int(((vy + sin1 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
                const int32_t x2 = d_fixed_to_int(((vx + cos2 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
                const int32_t y2 = d_fixed_to_int(((vy + sin2 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
                const int32_t x3 = d_fixed_to_int(((vx + cos3 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
                const int32_t y3 = d_fixed_to_int(((vy + sin3 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
            #endif

            // Figure out what color to draw with
            #if PSYDOOM_MODS
                const uint32_t color = AM_GetMobjColor(*pMobj, false);
            #else
                const uint32_t color = AM_COLOR_AQUA;
            #endif

            // Draw the triangle
            DrawLine(color, x1, y1, x2, y2);
            DrawLine(color, x2, y2, x3, y3);
            DrawLine(color, x1, y1, x3, y3);
        }
    }

    // Draw map things for players: again display a little triangle for each player
    for (int32_t playerIdx = 0; playerIdx < MAXPLAYERS; ++playerIdx) {
        // In deathmatch only show this player's triangle
        if ((gNetGame != gt_coop) && (playerIdx != gCurPlayerIndex))
            continue;

        // Flash the player's triangle when alive
        const player_t& player = gPlayers[playerIdx];

        #if PSYDOOM_MODS
            const bool bIsLocalPlayer = (gCurPlayerIndex == playerIdx);
        #endif

        if ((player.playerstate == PST_LIVE) && (gGameTic & 2))
            continue;

        // Figure out what color to draw with
        #if PSYDOOM_MODS
            const uint32_t color = AM_GetPlayerColor(playerIdx, false);
        #else
            // Change the colors of this player in COOP to distinguish
            uint32_t color = AM_COLOR_GREEN;

            if ((gNetGame == gt_coop) && (playerIdx == gCurPlayerIndex)) {
                color = AM_COLOR_YELLOW;
            }
        #endif

        // Compute the the sine and cosines for the angles of the 3 points in the triangle.
        // PsyDoom: use a (potentially) framerate uncapped rotation if it is the local player.
        mobj_t& mobj = *player.mo;

        #if PSYDOOM_MODS
            const angle_t playerAngle = (bIsLocalPlayer) ? gAM_PlayerAngle : mobj.angle;
        #else
            const angle_t playerAngle = mobj.angle;
        #endif

        const uint32_t fineAng1 = (playerAngle                ) >> ANGLETOFINESHIFT;
        const uint32_t fineAng2 = (playerAngle - ANG90 - ANG45) >> ANGLETOFINESHIFT;
        const uint32_t fineAng3 = (playerAngle + ANG90 + ANG45) >> ANGLETOFINESHIFT;

        const fixed_t cos1 = gFineCosine[fineAng1];
        const fixed_t cos2 = gFineCosine[fineAng2];
        const fixed_t cos3 = gFineCosine[fineAng3];

        const fixed_t sin1 = gFineSine[fineAng1];
        const fixed_t sin2 = gFineSine[fineAng2];
        const fixed_t sin3 = gFineSine[fineAng3];

        // Compute the line points.
        // PsyDoom: use a (potentially) framerate uncapped rotation if it is the local player.
        #if PSYDOOM_MODS
            const fixed_t playerX = (bIsLocalPlayer) ? gAM_PlayerX : mobj.x.renderValue();
            const fixed_t playerY = (bIsLocalPlayer) ? gAM_PlayerY : mobj.y.renderValue();
        #else
            const fixed_t playerX = mobj.x;
            const fixed_t playerY = mobj.y;
        #endif

        const fixed_t vx = playerX - ox;
        const fixed_t vy = playerY - oy;

        #if PSYDOOM_MODS
            // PsyDoom: scale is now a fixed point number due to framerate uncapped automap movement
            const int32_t x1 = d_fixed_to_int(FixedMul((vx + cos1 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
            const int32_t y1 = d_fixed_to_int(FixedMul((vy + sin1 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
            const int32_t x2 = d_fixed_to_int(FixedMul((vx + cos2 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
            const int32_t y2 = d_fixed_to_int(FixedMul((vy + sin2 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
            const int32_t x3 = d_fixed_to_int(FixedMul((vx + cos3 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
            const int32_t y3 = d_fixed_to_int(FixedMul((vy + sin3 * AM_THING_TRI_SIZE) / SCREEN_W, scale));
        #else
            const int32_t x1 = d_fixed_to_int(((vx + cos1 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
            const int32_t y1 = d_fixed_to_int(((vy + sin1 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
            const int32_t x2 = d_fixed_to_int(((vx + cos2 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
            const int32_t y2 = d_fixed_to_int(((vy + sin2 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
            const int32_t x3 = d_fixed_to_int(((vx + cos3 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
            const int32_t y3 = d_fixed_to_int(((vy + sin3 * AM_THING_TRI_SIZE) / SCREEN_W) * scale);
        #endif

        // Draw the triangle
        DrawLine(color, x1, y1, x2, y2);
        DrawLine(color, x2, y2, x3, y3);
        DrawLine(color, x1, y1, x3, y3);
    }
}

#if PSYDOOM_3DS
void AM_DrawerToExternal(AMExternalLineDrawer const lineDrawer, void* const pUserData) noexcept {
    gpExternalLineDrawer = lineDrawer;
    gpExternalLineDrawerUserData = pUserData;

    AM_Drawer();

    gpExternalLineDrawer = nullptr;
    gpExternalLineDrawerUserData = nullptr;
}
#endif

#if PSYDOOM_MODS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom specific helper: gets the color to draw the specified player in
//------------------------------------------------------------------------------------------------------------------------------------------
uint32_t AM_GetPlayerColor(const int32_t playerIdx, const bool bBrighten) noexcept {
    // Change the colors of this player in COOP to distinguish from the other player
    const bool bUseAltPlayerColor = ((gNetGame == gt_coop) && (playerIdx == gCurPlayerIndex));

    if (bUseAltPlayerColor) {
        return (bBrighten) ? BRIGHT_AM_COLOR_YELLOW : AM_COLOR_YELLOW;
    } else {
        return (bBrighten) ? BRIGHT_AM_COLOR_GREEN : AM_COLOR_GREEN;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom specific helper: gets the color to draw the specified map object in
//------------------------------------------------------------------------------------------------------------------------------------------
uint32_t AM_GetMobjColor(const mobj_t& mobj, const bool bBrighten) noexcept {
    // A live enemy?
    if ((mobj.flags & MF_COUNTKILL) && (mobj.health > 0)) {
        if (Config::gbUseExtendedAutomapColors) {
            return (bBrighten) ? BRIGHT_AM_COLOR_RED : AM_COLOR_RED;
        } else {
            return (bBrighten) ? BRIGHT_AM_COLOR_AQUA : AM_COLOR_AQUA;
        }
    }
    
    // Item?
    if (mobj.flags & MF_COUNTITEM) {
        if (Config::gbUseExtendedAutomapColors) {
            return (bBrighten) ? BRIGHT_AM_COLOR_MAGENTA : AM_COLOR_MAGENTA;
        } else {
            return (bBrighten) ? BRIGHT_AM_COLOR_AQUA : AM_COLOR_AQUA;
        }
    }

    // Player?
    if (mobj.player)
        return (bBrighten) ? BRIGHT_AM_COLOR_GREEN : AM_COLOR_GREEN;

    // All other map objects
    return (bBrighten) ? BRIGHT_AM_COLOR_AQUA : AM_COLOR_AQUA;
}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Draw an automap line in the specified color
//------------------------------------------------------------------------------------------------------------------------------------------
static void DrawLine(const uint32_t color, const int32_t x1, const int32_t y1, const int32_t x2, const int32_t y2) noexcept {
    // Reject the line quickly using the 'Cohen-Sutherland' algorithm.
    // Note: no clipping is done since that is handled by the hardware.
    enum OutFlags : int32_t {
        INSIDE  = 0,
        LEFT    = 1,
        RIGHT   = 2,
        BOTTOM  = 4,
        TOP     = 8
    };

    uint32_t outcode1 = (x1 < -128) ? LEFT : INSIDE;

    if (x1 >  128) { outcode1 |= RIGHT;     }
    if (y1 < -100) { outcode1 |= BOTTOM;    }
    if (y1 >  100) { outcode1 |= TOP;       }

    uint32_t outcode2 = (x2 < -128) ? LEFT : INSIDE;

    if (x2 >  128) { outcode2 |= RIGHT;     }
    if (y2 < -100) { outcode2 |= BOTTOM;    }
    if (y2 >  100) { outcode2 |= TOP;       }

    if (outcode1 & outcode2)
        return;

#if PSYDOOM_3DS
    if (gpExternalLineDrawer) {
        gpExternalLineDrawer(gpExternalLineDrawerUserData, color, x1, y1, x2, y2);
        return;
    }
#endif

    // Setup the map line primitive and draw it.
    //
    // Use the 1 KiB scratchpad also as temp storage space for the primitive.
    // PsyDoom: use local instead of scratchpad draw primitives; compiler can optimize better, and removes reliance on global state
    #if PSYDOOM_MODS
        LINE_F2 line = {};
    #else
        LINE_F2& line = *(LINE_F2*) LIBETC_getScratchAddr(128);
    #endif

    LIBGPU_SetLineF2(line);
    LIBGPU_setRGB0(line, (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t) color);
    LIBGPU_setXY2(line, (int16_t)(x1 + 128), (int16_t)(100 - y1), (int16_t)(x2 + 128), (int16_t)(100 - y2));

    I_AddPrim(line);
}
