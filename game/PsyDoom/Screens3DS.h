#pragma once

#if PSYDOOM_3DS

#include "Macros.h"

#include <cstdint>

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: decides what the two screens show, and turns touches on the bottom screen into menu input.
//
// The engine only ever renders one 256x240 PlayStation framebuffer, so the two screens are produced by presenting
// different parts of that one image:
//
//   Blank      Bottom screen is black. Used by the title screen and the movie/logo players, where a second copy of the
//              same image would just be noise.
//   Automap    Bottom screen draws live automap geometry. Used for the whole of gameplay, which is why there is no
//              automap toggle binding on 3DS.
//   MenuSplit  The framebuffer is split at a row: everything above it (the logo or screen title) goes on the top
//              screen, everything below it (the menu items) fills the bottom screen where it can be touched.
//   MenuTwoPass      The menu draws itself once per screen; see below.
//   BackgroundOnly   The top screen shows the screen exactly as the PlayStation composed it, and the bottom screen
//              shows a second pass over just that screen's background. Used by the intermission, the finale text
//              screens and the credits: they are presentations rather than menus, so there is nothing to put on the
//              touch screen, but mirroring the top screen onto it looked like a fault rather than a design.
//------------------------------------------------------------------------------------------------------------------------------------------
BEGIN_NAMESPACE(Screens3DS)

enum class BottomScreen : uint8_t {
    Blank,
    Automap,
    AutomapStatusBarTop,    // Status bar along the top of the screen, the automap squeezed underneath it
    AutomapStatusBarBottom, // Automap above, the status bar blitted along the bottom
    MenuSplit,
    MenuTwoPass,
    BackgroundOnly
};

// How many rows of the touch screen the status bar takes when it lives there.
// The status bar is 256x40 in the framebuffer, and 320/256 scales that to 50 rows.
// The part of the framebuffer the status bar actually draws on. The sprite is forty rows tall but the artwork stops
// at row 232, and the blank remainder is not worth any of the touch screen.
static constexpr int32_t BOTTOM_STATUS_BAR_SRC_Y = 200;
static constexpr int32_t BOTTOM_STATUS_BAR_SRC_H = 33;

// How much of the touch screen that comes to, at the same scale the width is blown up by (320 over 256)
static constexpr int32_t BOTTOM_STATUS_BAR_H = (BOTTOM_STATUS_BAR_SRC_H * 320) / 256;

//------------------------------------------------------------------------------------------------------------------------------------------
// 'MenuTwoPass': a menu that composes each screen separately.
//
// 'MenuSplit' cuts one drawn menu in half, which means the two screens share a seam - a cursor or a letter sitting on
// the boundary appears sliced across both displays - and neither screen gets a background of its own. A menu that
// cares about how it looks instead draws itself twice: once with just the background and the logo, laid out for the
// 400x240 top screen and banked there with 'Video::presentTopScreenOnly', and once with the background and the options
// laid out for the 320x240 touch screen. Each pass declares the framebuffer rows it wants presented.
//
// Both rects are chosen so the scale works out exactly uniform: 154 rows of a 256 wide framebuffer scale by 1.5625 to
// fill 400x240, and 192 rows scale by 1.25 to fill 320x240.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t MENU_TOP_PASS_ROWS = 154;      // 256 x 154, scaled by 1.5625, fills the 400x240 top screen
static constexpr int32_t BOT_PASS_ROWS = 192;      // 256 x 192, scaled by 1.25, fills the 320x240 touch screen

// Both bands are centred in the framebuffer, so the background tiling lines up the same way in either pass
static constexpr int32_t MENU_TOP_PASS_SRC_Y = (240 - MENU_TOP_PASS_ROWS) / 2;
static constexpr int32_t BOT_PASS_SRC_Y = (240 - BOT_PASS_ROWS) / 2;

void setMenuTwoPassRows(const int32_t topSrcY, const int32_t bottomSrcY) noexcept;
int32_t getMenuTopPassSrcY() noexcept;
int32_t getBottomPassSrcY() noexcept;

// Set when a menu has already banked the top screen itself, so the final present leaves it alone.
// Cleared automatically once the frame is presented.
void setTopScreenAlreadyDrawn(const bool bDrawn) noexcept;
bool wasTopScreenAlreadyDrawn() noexcept;

// The row that 'MenuSplit' defaults to. PsyDoom's menus draw their title with the big font at y=20, which is 16 pixels
// tall, so everything from row 40 down is menu content.
static constexpr int32_t DEFAULT_MENU_SPLIT_Y = 40;

// How wide the actual artwork in the header band is, in framebuffer pixels, centred on the screen.
//
// The band is only ever a logo or a title on a tiled background, so it is much narrower than the 256 pixel wide
// framebuffer. Knowing how much of it matters lets the presenter blow the band up until it fills the top screen and
// crop the leftover background off the sides, instead of scaling the whole 256 pixels down and letterboxing it.
// The default is deliberately generous, so a screen that does not say gets scaled conservatively rather than clipped.
static constexpr int32_t DEFAULT_MENU_HEADER_WIDTH = 200;

// What the screens show. Set every frame by whatever is drawing; the presenter consumes it.
void setBottomScreen(
    const BottomScreen mode,
    const int32_t splitY = DEFAULT_MENU_SPLIT_Y,
    const int32_t headerContentWidth = DEFAULT_MENU_HEADER_WIDTH
) noexcept;

BottomScreen getBottomScreen() noexcept;
int32_t getMenuSplitY() noexcept;
int32_t getMenuHeaderWidth() noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Touch input, expressed in PlayStation framebuffer coordinates so menus can hit test against the same coordinates
// they draw with. Only meaningful while the bottom screen is showing framebuffer content.
//------------------------------------------------------------------------------------------------------------------------------------------
struct Touch {
    bool    bDown;
    bool    bJustPressed;
    bool    bJustReleased;
    int32_t x;
    int32_t y;
};

// Samples the touch screen; called once per frame before the game ticks
void updateTouch() noexcept;
const Touch& getTouch() noexcept;

// Records how the bottom screen was laid out this frame, so touches can be mapped back into framebuffer coordinates.
// Called by the presenter. A width or height of zero means the bottom screen is not showing framebuffer content.
void setBottomScreenMapping(
    const int32_t destX,
    const int32_t destY,
    const int32_t destW,
    const int32_t destH,
    const int32_t srcX,
    const int32_t srcY,
    const int32_t srcW,
    const int32_t srcH
) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Touchable menu items.
//
// A menu registers its rows while drawing and reads the result while ticking, so a tap is acted on the frame after it
// is drawn. Items are identified by the menu's own cursor index.
//------------------------------------------------------------------------------------------------------------------------------------------

// Forget all registered items; called automatically at the start of each frame's UI drawing
void clearTouchItems() noexcept;

// Register a touchable row in framebuffer coordinates. The row is widened to the full screen width for easier tapping.
void addTouchItem(const int32_t itemIdx, const int32_t y, const int32_t height) noexcept;

// Register a touchable rectangle in framebuffer coordinates
void addTouchItemRect(const int32_t itemIdx, const int32_t x, const int32_t y, const int32_t w, const int32_t h) noexcept;

// Matches the current touch state against the items registered this frame; called by the presenter once drawing is done
void resolveTouchItems() noexcept;

// The item currently under the finger, or -1
int32_t getHoveredItem() noexcept;

// The item that was tapped (pressed and released over the same row), or -1. Reading it clears it.
int32_t consumeTappedItem() noexcept;

// Was the bottom screen tapped anywhere outside of every registered item? Reading it clears it.
bool consumeTapOutsideItems() noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Raw touch position in bottom screen pixels (0-319, 0-239), for things that draw the bottom screen themselves rather
// than presenting part of the framebuffer - namely the automap.
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t getRawTouchX() noexcept;
int32_t getRawTouchY() noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Stereoscopic 3D.
//
// The slider is read every frame. At zero there is no separation and nothing extra is rendered, so leaving this on
// costs nothing until the player actually pushes the slider up. Above zero the 3D view has to be rendered a second
// time from the other eye, which roughly halves the frame rate.
//------------------------------------------------------------------------------------------------------------------------------------------

// Reads the slider and the user preference; call once per frame before drawing
void updateStereoState() noexcept;

// Is a second eye being rendered this frame?
bool isStereoActive() noexcept;

// Half the eye separation, in world units (16.16 fixed point). Positive for the right eye.
int32_t getEyeSeparation() noexcept;

// Which eye the renderer is currently producing: -1 left, 0 mono, +1 right
void setRenderEye(const int32_t eye) noexcept;
int32_t getRenderEye() noexcept;

END_NAMESPACE(Screens3DS)

#endif  // #if PSYDOOM_3DS
