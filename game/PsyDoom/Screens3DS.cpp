//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: screen roles and bottom screen touch input. See 'Screens3DS.h' for the design.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "Screens3DS.h"

#if PSYDOOM_3DS

#include <3ds.h>

#include "PlayerPrefs.h"

#include <algorithm>
#include <array>

BEGIN_NAMESPACE(Screens3DS)

//------------------------------------------------------------------------------------------------------------------------------------------
// What the screens are showing
//------------------------------------------------------------------------------------------------------------------------------------------
static BottomScreen gBottomScreen = BottomScreen::Blank;
static bool     gbTopScreenAlreadyDrawn = false;
static int32_t      gMenuSplitY = DEFAULT_MENU_SPLIT_Y;
static int32_t      gMenuHeaderWidth = DEFAULT_MENU_HEADER_WIDTH;

void setBottomScreen(const BottomScreen mode, const int32_t splitY, const int32_t headerContentWidth) noexcept {
    // A different screen is composing now, so any half finished frame from the last one is void
    if (mode != gBottomScreen) {
        gbTopScreenAlreadyDrawn = false;
    }

    gBottomScreen = mode;
    gMenuSplitY = std::clamp<int32_t>(splitY, 0, 200);
    gMenuHeaderWidth = std::clamp<int32_t>(headerContentWidth, 32, 256);
}

BottomScreen getBottomScreen() noexcept {
    return gBottomScreen;
}

int32_t getMenuSplitY() noexcept {
    return gMenuSplitY;
}

int32_t getMenuHeaderWidth() noexcept {
    return gMenuHeaderWidth;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Two pass menus
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t  gMenuTopPassSrcY = 0;
static int32_t  gMenuBottomPassSrcY = 0;

void setMenuTwoPassRows(const int32_t topSrcY, const int32_t bottomSrcY) noexcept {
    gMenuTopPassSrcY = std::clamp<int32_t>(topSrcY, 0, 240 - MENU_TOP_PASS_ROWS);
    gMenuBottomPassSrcY = std::clamp<int32_t>(bottomSrcY, 0, 240 - BOT_PASS_ROWS);
}

int32_t getMenuTopPassSrcY() noexcept {
    return gMenuTopPassSrcY;
}

int32_t getBottomPassSrcY() noexcept {
    return gMenuBottomPassSrcY;
}

void setTopScreenAlreadyDrawn(const bool bDrawn) noexcept {
    gbTopScreenAlreadyDrawn = bDrawn;
}

bool wasTopScreenAlreadyDrawn() noexcept {
    return gbTopScreenAlreadyDrawn;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// How the bottom screen was laid out on the last presented frame
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t gMapDestX = 0;
static int32_t gMapDestY = 0;
static int32_t gMapDestW = 0;
static int32_t gMapDestH = 0;
static int32_t gMapSrcX = 0;
static int32_t gMapSrcY = 0;
static int32_t gMapSrcW = 0;
static int32_t gMapSrcH = 0;

void setBottomScreenMapping(
    const int32_t destX,
    const int32_t destY,
    const int32_t destW,
    const int32_t destH,
    const int32_t srcX,
    const int32_t srcY,
    const int32_t srcW,
    const int32_t srcH
) noexcept {
    gMapDestX = destX;  gMapDestY = destY;  gMapDestW = destW;  gMapDestH = destH;
    gMapSrcX = srcX;    gMapSrcY = srcY;    gMapSrcW = srcW;    gMapSrcH = srcH;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Touch input
//------------------------------------------------------------------------------------------------------------------------------------------
static Touch    gTouch = {};
static int32_t  gRawTouchX = 0;
static int32_t  gRawTouchY = 0;

void updateTouch() noexcept {
    const bool bWasDown = gTouch.bDown;

    // Note: the touch position latched by 'hidScanInput' reads as (0,0) when nothing is touching the screen.
    // The video driver's event pump has already scanned HID by the time this runs, so just read the latched state.
    touchPosition touchPos = {};
    hidTouchRead(&touchPos);

    const bool bIsDown = ((hidKeysHeld() & KEY_TOUCH) != 0);

    gTouch.bJustPressed = (bIsDown && (!bWasDown));
    gTouch.bJustReleased = ((!bIsDown) && bWasDown);
    gTouch.bDown = bIsDown;

    // Keep the last position on release so a tap can be resolved against where the finger actually lifted
    if (bIsDown) {
        gRawTouchX = (int32_t) touchPos.px;
        gRawTouchY = (int32_t) touchPos.py;

        if ((gMapDestW > 0) && (gMapDestH > 0)) {
            const int32_t localX = (int32_t) touchPos.px - gMapDestX;
            const int32_t localY = (int32_t) touchPos.py - gMapDestY;

            gTouch.x = gMapSrcX + (localX * gMapSrcW) / gMapDestW;
            gTouch.y = gMapSrcY + (localY * gMapSrcH) / gMapDestH;
        } else {
            gTouch.x = -1;
            gTouch.y = -1;
        }
    }
}

const Touch& getTouch() noexcept {
    return gTouch;
}

int32_t getRawTouchX() noexcept {
    return gRawTouchX;
}

int32_t getRawTouchY() noexcept {
    return gRawTouchY;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Stereoscopic 3D
//------------------------------------------------------------------------------------------------------------------------------------------
static bool     gbStereoActive = false;
static int32_t  gEyeSeparation = 0;
static int32_t  gRenderEye = 0;

// How far apart the eyes are at full slider, in world units. Doom's player radius is 16 units, so a couple of units of
// separation is a believable interpupillary distance at this scale and does not break the illusion at close range.
static constexpr int32_t MAX_EYE_SEPARATION = 3 * 65536;

//------------------------------------------------------------------------------------------------------------------------------------------
// Turns the hardware's stereo output on or off. Doing this changes the top screen's buffer layout, so only do it when
// the state actually changes rather than every frame.
//------------------------------------------------------------------------------------------------------------------------------------------
static void setHardwareStereo(const bool bEnable) noexcept {
    static bool bHardwareStereoOn = false;

    if (bEnable != bHardwareStereoOn) {
        bHardwareStereoOn = bEnable;
        gfxSet3D(bEnable);
    }
}

void updateStereoState() noexcept {
    // 0.0 to 1.0; the slider is a physical control so this can change at any time.
    // There is deliberately no setting for this: the slider itself is the on/off switch, and at the bottom of its
    // travel nothing extra is rendered, so leaving it alone costs nothing.
    const float slider = osGet3DSliderState();

    if (slider <= 0.01f) {
        setHardwareStereo(false);
        gbStereoActive = false;
        gEyeSeparation = 0;
        gRenderEye = 0;
        return;
    }

    setHardwareStereo(true);
    gbStereoActive = true;
    gEyeSeparation = (int32_t)(slider * (float) MAX_EYE_SEPARATION);
}

bool isStereoActive() noexcept {
    return gbStereoActive;
}

int32_t getEyeSeparation() noexcept {
    return gEyeSeparation;
}

void setRenderEye(const int32_t eye) noexcept {
    gRenderEye = eye;
}

int32_t getRenderEye() noexcept {
    return gRenderEye;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Touchable menu items
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t MAX_TOUCH_ITEMS = 32;

struct TouchItem {
    int32_t itemIdx;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

static std::array<TouchItem, MAX_TOUCH_ITEMS>   gTouchItems = {};
static int32_t                                  gNumTouchItems = 0;
static int32_t                                  gPressedItem = -1;       // Item the finger went down on
static int32_t                                  gTappedItem = -1;        // Item a completed tap landed on
static bool                                     gbTappedOutside = false;

void clearTouchItems() noexcept {
    gNumTouchItems = 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Finds the registered item containing the given framebuffer position, or -1
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t findItemAt(const int32_t x, const int32_t y) noexcept {
    for (int32_t i = 0; i < gNumTouchItems; ++i) {
        const TouchItem& item = gTouchItems[i];

        if ((x >= item.x) && (x < item.x + item.w) && (y >= item.y) && (y < item.y + item.h))
            return item.itemIdx;
    }

    return -1;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Resolves the current touch state against the items registered this frame.
//
// Called by the presenter, which runs after the menu has finished drawing, so the full item list for the frame is
// known by this point.
//------------------------------------------------------------------------------------------------------------------------------------------
void resolveTouchItems() noexcept {
    if (gTouch.bJustPressed) {
        gPressedItem = findItemAt(gTouch.x, gTouch.y);
    }
    else if (gTouch.bJustReleased) {
        const int32_t releasedOver = findItemAt(gTouch.x, gTouch.y);

        if ((gPressedItem >= 0) && (releasedOver == gPressedItem)) {
            gTappedItem = gPressedItem;
        } else if ((gPressedItem < 0) && (releasedOver < 0)) {
            gbTappedOutside = true;
        }

        gPressedItem = -1;
    }
}

void addTouchItemRect(const int32_t itemIdx, const int32_t x, const int32_t y, const int32_t w, const int32_t h) noexcept {
    if (gNumTouchItems >= MAX_TOUCH_ITEMS)
        return;

    gTouchItems[gNumTouchItems] = TouchItem{ itemIdx, x, y, w, h };
    gNumTouchItems++;
}

void addTouchItem(const int32_t itemIdx, const int32_t y, const int32_t height) noexcept {
    addTouchItemRect(itemIdx, 0, y, 256, height);
}

int32_t getHoveredItem() noexcept {
    if (!gTouch.bDown)
        return -1;

    return findItemAt(gTouch.x, gTouch.y);
}

int32_t consumeTappedItem() noexcept {
    const int32_t item = gTappedItem;
    gTappedItem = -1;
    return item;
}

bool consumeTapOutsideItems() noexcept {
    const bool bTapped = gbTappedOutside;
    gbTappedOutside = false;
    return bTapped;
}

END_NAMESPACE(Screens3DS)

#endif  // #if PSYDOOM_3DS
