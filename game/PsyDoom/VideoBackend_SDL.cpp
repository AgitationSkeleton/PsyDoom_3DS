#include "VideoBackend_SDL.h"

#include "Asserts.h"
#include "Config/Config.h"
#include "Gpu.h"
#include "PsxVm.h"
#include "NetworkUds3DS.h"
#include "PlayerPrefs.h"
#include "Screens3DS.h"
#include "Utils.h"
#include "Video.h"
#include "VideoSurface_SDL.h"

#if PSYDOOM_3DS
    #include <3ds.h>
    #include "Doom/Base/i_main.h"
    #include "Doom/Base/w_wad.h"
    #include "Doom/d_main.h"
    #include "Doom/Game/p_tick.h"
    #include "Doom/UI/am_main.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <SDL.h>

BEGIN_NAMESPACE(Video)
#if PSYDOOM_3DS
namespace {

static constexpr int32_t BOTTOM_MAP_MIN_X = -128;
static constexpr int32_t BOTTOM_MAP_MAX_X = 127;
static constexpr int32_t BOTTOM_MAP_MIN_Y = -100;
static constexpr int32_t BOTTOM_MAP_MAX_Y = 100;

// How many pixels of touch screen one unit of the automap's own coordinate space covers.
//
// The same in both directions, so the map keeps its shape. It used to be 1.25 across and 1.0 down, which squashed the
// map vertically and still left forty rows of the screen unused. At 1.2 the full height of the map fills the screen
// exactly, and the extra width that leaves over shows a little more of the level rather than stretching what is there.
static constexpr int32_t BOTTOM_MAP_SCALE_NUM = 6;
static constexpr int32_t BOTTOM_MAP_SCALE_DEN = 5;

enum BottomMapOutCode : uint32_t {
    BOTTOM_MAP_INSIDE = 0,
    BOTTOM_MAP_LEFT = 1,
    BOTTOM_MAP_RIGHT = 2,
    BOTTOM_MAP_BOTTOM = 4,
    BOTTOM_MAP_TOP = 8
};

// What the clip is against. Defaults to the whole of the automap's own coordinate space, which is what the callers
// that present part of the framebuffer want; the 3DS automap draws itself and says how much of it fits on screen.
struct BottomMapBounds {
    int32_t minX = BOTTOM_MAP_MIN_X;
    int32_t maxX = BOTTOM_MAP_MAX_X;
    int32_t minY = BOTTOM_MAP_MIN_Y;
    int32_t maxY = BOTTOM_MAP_MAX_Y;
};

static uint32_t getBottomMapOutCode(const int32_t x, const int32_t y, const BottomMapBounds& bounds) noexcept {
    uint32_t outCode = BOTTOM_MAP_INSIDE;

    if (x < bounds.minX) outCode |= BOTTOM_MAP_LEFT;
    if (x > bounds.maxX) outCode |= BOTTOM_MAP_RIGHT;
    if (y < bounds.minY) outCode |= BOTTOM_MAP_BOTTOM;
    if (y > bounds.maxY) outCode |= BOTTOM_MAP_TOP;

    return outCode;
}

static bool clipBottomMapLine(
    int32_t& x1,
    int32_t& y1,
    int32_t& x2,
    int32_t& y2,
    const BottomMapBounds& bounds = BottomMapBounds{}
) noexcept {
    uint32_t outCode1 = getBottomMapOutCode(x1, y1, bounds);
    uint32_t outCode2 = getBottomMapOutCode(x2, y2, bounds);

    while (true) {
        if ((outCode1 | outCode2) == 0)
            return true;

        if (outCode1 & outCode2)
            return false;

        const uint32_t outCode = (outCode1 != 0) ? outCode1 : outCode2;
        int32_t clippedX = 0;
        int32_t clippedY = 0;

        if (outCode & BOTTOM_MAP_TOP) {
            clippedX = x1 + (int32_t)(((int64_t)(x2 - x1) * (bounds.maxY - y1)) / (y2 - y1));
            clippedY = bounds.maxY;
        } else if (outCode & BOTTOM_MAP_BOTTOM) {
            clippedX = x1 + (int32_t)(((int64_t)(x2 - x1) * (bounds.minY - y1)) / (y2 - y1));
            clippedY = bounds.minY;
        } else if (outCode & BOTTOM_MAP_RIGHT) {
            clippedY = y1 + (int32_t)(((int64_t)(y2 - y1) * (bounds.maxX - x1)) / (x2 - x1));
            clippedX = bounds.maxX;
        } else {
            clippedY = y1 + (int32_t)(((int64_t)(y2 - y1) * (bounds.minX - x1)) / (x2 - x1));
            clippedX = bounds.minX;
        }

        if (outCode == outCode1) {
            x1 = clippedX;
            y1 = clippedY;
            outCode1 = getBottomMapOutCode(x1, y1, bounds);
        } else {
            x2 = clippedX;
            y2 = clippedY;
            outCode2 = getBottomMapOutCode(x2, y2, bounds);
        }
    }
}

static void drawBottomMapLine(
    void* const pUserData,
    const uint32_t color,
    int32_t x1,
    int32_t y1,
    int32_t x2,
    int32_t y2
) noexcept {
    if (!clipBottomMapLine(x1, y1, x2, y2))
        return;

    x1 += 128;
    x2 += 128;
    y1 = 120 - y1;
    y2 = 120 - y2;

    const int32_t deltaX = (x2 >= x1) ? (x2 - x1) : (x1 - x2);
    const int32_t stepX = (x1 < x2) ? 1 : -1;
    const int32_t deltaY = (y2 >= y1) ? (y2 - y1) : (y1 - y2);
    const int32_t stepY = (y1 < y2) ? 1 : -1;
    int32_t error = deltaX - deltaY;

    const uint32_t red = color >> 16;
    const uint32_t green = (color >> 8) & 0xFF;
    const uint32_t blue = color & 0xFF;
    const uint32_t pixelColor = 0xFF000000 | (blue << 16) | (green << 8) | red;
    uint32_t* const pPixels = static_cast<uint32_t*>(pUserData);

    while (true) {
        pPixels[x1 + y1 * ORIG_DRAW_RES_X] = pixelColor;

        if ((x1 == x2) && (y1 == y2))
            break;

        const int32_t doubledError = error * 2;

        if (doubledError > -deltaY) {
            error -= deltaY;
            x1 += stepX;
        }

        if (doubledError < deltaX) {
            error += deltaX;
            y1 += stepY;
        }
    }
}

struct NativeFramebuffer16 {
    uint16_t* pixels;
    int32_t pitch;
    int32_t width;
    int32_t height;
};

// Where the automap lands on the touch screen; see the automap drawing below
// Where the automap's own coordinate space lands on the touch screen. Making room for the status bar shrinks the map
// about its centre rather than squashing or cropping it, so it keeps its shape and none of it is lost.
struct AutomapTarget {
    NativeFramebuffer16 framebuffer;
    int32_t             centerX;
    int32_t             centerY;
    BottomMapBounds     bounds;     // How much of the map fits in the room it has been given
    int32_t             minScreenX; // The columns and rows that room actually covers, which the map is clamped to
    int32_t             maxScreenX;
    int32_t             minScreenY;
    int32_t             maxScreenY;
};

static NativeFramebuffer16 getFramebuffer(const gfxScreen_t screen, const gfx3dSide_t side = GFX_LEFT) noexcept {
    u16 framebufferWidth = 0;
    u16 framebufferHeight = 0;
    uint16_t* const pixels = reinterpret_cast<uint16_t*>(
        gfxGetFramebuffer(screen, side, &framebufferWidth, &framebufferHeight)
    );
    return NativeFramebuffer16 {
        pixels,
        static_cast<int32_t>(framebufferWidth),
        static_cast<int32_t>(framebufferHeight),
        static_cast<int32_t>(framebufferWidth)
    };
}

static uint16_t psxToRgb5A1(const Gpu::Color16 color) noexcept {
    const uint16_t red = color.bits & 0x1Fu;
    const uint16_t green = (color.bits >> 5) & 0x1Fu;
    const uint16_t blue = (color.bits >> 10) & 0x1Fu;
    return static_cast<uint16_t>((red << 11) | (green << 6) | (blue << 1) | 1u);
}

static void clearFramebuffer(const NativeFramebuffer16& framebuffer) noexcept {
    if (framebuffer.pixels) {
        std::fill_n(
            framebuffer.pixels,
            static_cast<size_t>(framebuffer.pitch) * framebuffer.width,
            static_cast<uint16_t>(1u)
        );
    }
}

// Clears only the part of the screen that the image about to be blitted will not cover.
// The 3DS framebuffer is column major (one screen column is one contiguous run), so a screen column is either
// fully outside the image or has a run of unused pixels at each end.
static void clearAroundRect(
    const NativeFramebuffer16& framebuffer,
    const int32_t keepStartX,
    const int32_t keepEndX,
    const int32_t keepStartY,
    const int32_t keepEndY
) noexcept {
    constexpr uint16_t BLACK = 1u;      // Opaque black in RGB5A1

    for (int32_t x = 0; x < framebuffer.width; ++x) {
        uint16_t* const column = framebuffer.pixels + static_cast<size_t>(framebuffer.pitch) * x;

        if ((x < keepStartX) || (x >= keepEndX)) {
            std::fill_n(column, framebuffer.pitch, BLACK);
            continue;
        }

        // Column contents run bottom-to-top, so the rows above the image sit at the start of the column
        std::fill_n(column, framebuffer.pitch - keepEndY, BLACK);
        std::fill_n(column + framebuffer.pitch - keepStartY, keepStartY, BLACK);
    }
}

static void copyToScreen(
    const Gpu::Color16* const sourcePixels,
    const int32_t sourcePitch,
    const int32_t sourceX,
    const int32_t sourceY,
    const int32_t sourceWidth,
    const int32_t sourceHeight,
    const NativeFramebuffer16& framebuffer,
    const int32_t destinationX,
    const int32_t destinationY,
    const int32_t destinationWidth,
    const int32_t destinationHeight,
    const bool bClearAround = true      // Clear the part of the screen the image will not cover
) noexcept {
    if ((!framebuffer.pixels) || (destinationWidth <= 0) || (destinationHeight <= 0))
        return;

    const int32_t copyStartX = std::max(destinationX, static_cast<int32_t>(0));
    const int32_t copyStartY = std::max(destinationY, static_cast<int32_t>(0));
    const int32_t copyEndX = std::min(destinationX + destinationWidth, framebuffer.width);
    const int32_t copyEndY = std::min(destinationY + destinationHeight, framebuffer.height);

    if (bClearAround) {
        clearAroundRect(framebuffer, copyStartX, copyEndX, copyStartY, copyEndY);
    }

    if ((copyStartX >= copyEndX) || (copyStartY >= copyEndY))
        return;

    std::array<uint32_t, 400> sourceColumns {};
    std::array<uint32_t, 240> sourceRows {};

    for (int32_t x = copyStartX; x < copyEndX; ++x) {
        sourceColumns[x - copyStartX] = static_cast<uint32_t>(
            sourceX + ((static_cast<int64_t>(x - destinationX) * sourceWidth) / destinationWidth)
        );
    }
    for (int32_t y = copyStartY; y < copyEndY; ++y) {
        sourceRows[y - copyStartY] = static_cast<uint32_t>(
            sourceY + ((static_cast<int64_t>(y - destinationY) * sourceHeight) / destinationHeight)
        ) * sourcePitch;
    }

    // This is a transpose: PSX VRAM is row major but a 3DS framebuffer is column major, so the naive loop reads
    // down a VRAM column with a two kilobyte stride and misses cache on essentially every pixel.
    //
    // Blocking by rows fixes that. One band of BAND_H source rows is about 8 KiB of VRAM, which fits in the
    // ARM11's data cache, so it is read once and then reused across all of the destination columns.
    constexpr int32_t BAND_H = 16;

    for (int32_t bandStartY = copyStartY; bandStartY < copyEndY; bandStartY += BAND_H) {
        const int32_t bandEndY = std::min(bandStartY + BAND_H, copyEndY);

        for (int32_t x = copyStartX; x < copyEndX; ++x) {
            const uint32_t sourceColumn = sourceColumns[x - copyStartX];
            uint16_t* const pDst = framebuffer.pixels + static_cast<size_t>(framebuffer.pitch) * x + framebuffer.pitch - 1;

            for (int32_t y = bandStartY; y < bandEndY; ++y) {
                pDst[-y] = psxToRgb5A1(sourcePixels[sourceColumn + sourceRows[y - copyStartY]]);
            }
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Puts the current PlayStation framebuffer on the top screen.
//
// Which eye it goes to comes from 'Screens3DS::getRenderEye': mono and the left eye both go to the left buffer, the
// right eye to the right buffer. With the 3D slider at zero the right buffer is never written and never rendered.
//------------------------------------------------------------------------------------------------------------------------------------------
// 'bUseDrawArea' matters when this is called part way through a frame, to bank an image before the framebuffer is
// redrawn over. The buffers are only swapped at the end of the frame, so until then the display area still points at
// the previous frame's image and the one just drawn lives in the draw area.
static void drawTopScreen(const bool bUseDrawArea = false) noexcept {
    Gpu::Core& gpu = PsxVm::gGpu;
    const Gpu::Color16* const vramPixels = reinterpret_cast<const Gpu::Color16*>(gpu.pRam);

    const int32_t srcOriginX = (bUseDrawArea) ? gpu.drawAreaLx : gpu.displayAreaX;
    const int32_t srcOriginY = (bUseDrawArea) ? gpu.drawAreaTy : gpu.displayAreaY;

    const gfx3dSide_t side = (Screens3DS::getRenderEye() > 0) ? GFX_RIGHT : GFX_LEFT;
    const NativeFramebuffer16 topFramebuffer = getFramebuffer(GFX_TOP, side);

    const Screens3DS::BottomScreen bottomScreenMode = Screens3DS::getBottomScreen();
    const int32_t menuSplitY = Screens3DS::getMenuSplitY();

    // Note the 'bUseDrawArea' test as well as the mode.
    //
    // Taking the top screen's rows out of the frame is only right while a two pass menu is banking its first pass,
    // which is the only time this is called with the draw area. Reaching here any other way means something presented
    // a frame that the menu had only drawn its second pass into - and taking the top screen's rows out of that puts
    // the touch screen's contents on the top screen, which is what starting a game used to look like. Fall through to
    // presenting the frame whole in that case: it is never right, but it is never nonsense either.
    if ((bottomScreenMode == Screens3DS::BottomScreen::MenuTwoPass) && bUseDrawArea) {
        // A menu that composes each screen separately: present exactly the rows it asked for, which are chosen so the
        // scale comes out uniform and fills the display
        copyToScreen(
            vramPixels,
            gpu.ramPixelW,
            srcOriginX,
            srcOriginY + Screens3DS::getMenuTopPassSrcY(),
            ORIG_DRAW_RES_X,
            Screens3DS::MENU_TOP_PASS_ROWS,
            topFramebuffer,
            0,
            0,
            400,
            240
        );

        return;
    }

    if ((bottomScreenMode == Screens3DS::BottomScreen::MenuSplit) && (menuSplitY > 0)) {
        // Menus: the top screen carries the logo or screen title band, scaled up uniformly so nothing is distorted.
        //
        // Fitting the band's full 256 pixel width into 400 pixels can only ever produce about 120 of the 240 rows, so
        // that alone would letterbox it badly. But the band is a logo or a title on a tiled background, and only the
        // artwork in the middle matters - the sides are just more background. So the band is blown up until it fills
        // the height and the leftover background is cropped off the sides, which fills the screen without stretching
        // anything. The scale is capped so the crop never eats into the artwork itself.
        const int32_t bandH = menuSplitY;
        const int32_t contentW = std::max<int32_t>(Screens3DS::getMenuHeaderWidth(), 1);

        // Scale in 1/256ths: enough to fill the height, but never so much that the artwork would be cropped
        const int32_t scale256 = std::min<int32_t>((240 * 256) / bandH, (400 * 256) / contentW);

        // How much of the band's width survives the crop, and where it lands on screen
        const int32_t srcW = std::min<int32_t>((400 * 256) / scale256, ORIG_DRAW_RES_X);
        const int32_t srcX = (ORIG_DRAW_RES_X - srcW) / 2;
        const int32_t drawW = std::min<int32_t>((srcW * scale256) / 256, 400);
        const int32_t drawH = std::min<int32_t>((bandH * scale256) / 256, 240);

        copyToScreen(
            vramPixels,
            gpu.ramPixelW,
            srcOriginX + srcX,
            srcOriginY,
            srcW,
            bandH,
            topFramebuffer,
            (400 - drawW) / 2,
            (240 - drawH) / 2,
            drawW,
            drawH
        );

        return;
    }

    // With the status bar on the touch screen the top screen shows the 3D view alone, which now occupies the whole
    // framebuffer. Otherwise it shows the framebuffer as the PlayStation composed it: view plus status bar.
    const int32_t visibleTopHeight = (PlayerPrefs::gStatusBarPos != PlayerPrefs::STATUS_BAR_TOP_SCREEN) ?
        gViewHeight :
        (ORIG_DRAW_RES_Y - Video::gTopOverscan - Video::gBotOverscan);

    // Either stretch the image across the whole top screen, or keep the PlayStation's aspect ratio with pillarbox bars
    // either side. Full width trades a horizontal stretch for using all 400 pixels of the display.
    int32_t destX, destY, destW, destH;

    if (PlayerPrefs::gbFullWidthVideo) {
        destX = 0;
        destY = 0;
        destW = 400;
        destH = 240;
    } else {
        float topX = 0.0f;
        float topY = 0.0f;
        float topWidth = 0.0f;
        float topHeight = 0.0f;
        Video::getClassicFramebufferWindowRect(400.0f, 240.0f, topX, topY, topWidth, topHeight);

        destX = static_cast<int32_t>(topX);
        destY = static_cast<int32_t>(topY);
        destW = static_cast<int32_t>(std::ceil(topWidth));
        destH = static_cast<int32_t>(std::ceil(topHeight));
    }

    copyToScreen(
        vramPixels,
        gpu.ramPixelW,
        srcOriginX,
        srcOriginY + ((PlayerPrefs::gStatusBarPos != PlayerPrefs::STATUS_BAR_TOP_SCREEN) ? 0 : Video::gTopOverscan),
        ORIG_DRAW_RES_X,
        visibleTopHeight,
        topFramebuffer,
        destX,
        destY,
        destW,
        destH
    );
}

}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Creates the backend with the SDL renderer uninitialized
//------------------------------------------------------------------------------------------------------------------------------------------
VideoBackend_SDL::VideoBackend_SDL() noexcept 
    : mpSdlWindow(nullptr)
    , mpRenderer(nullptr)
    , mpFramebufferTexture(nullptr)
    , mpFramebufferPixels(nullptr)
    #if PSYDOOM_3DS
        , mpBottomSdlWindow(nullptr)
        , mpBottomRenderer(nullptr)
        , mpBottomFramebufferTexture(nullptr)
        , mpBottomFramebufferPixels(nullptr)
    #endif
{
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Ensures everything is cleaned up
//------------------------------------------------------------------------------------------------------------------------------------------
VideoBackend_SDL::~VideoBackend_SDL() noexcept {
    destroyRenderers();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the window create flags required for an SDL video backend window
//------------------------------------------------------------------------------------------------------------------------------------------
uint32_t VideoBackend_SDL::getSdlWindowCreateFlags() noexcept {
    // Use OpenGL where it is supported since that is the main implementation for SDL renderer.
    // On MacOS it's better to use Metal however since OpenGL is deprecated.
    #if defined(__3DS__)
        return 0;
    #elif __APPLE__
        return SDL_WINDOW_METAL;
    #else
        return SDL_WINDOW_OPENGL;
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initializes the SDL renderer used by this backend
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::initRenderers(SDL_Window* const pSdlWindow) noexcept {
    ASSERT(pSdlWindow);

    // Must not already be initialized
    ASSERT(!mpRenderer);
    ASSERT(!mpFramebufferTexture);
    ASSERT(!mpFramebufferPixels);
    #if PSYDOOM_3DS
        ASSERT(!mpBottomSdlWindow);
        ASSERT(!mpBottomRenderer);
        ASSERT(!mpBottomFramebufferTexture);
        ASSERT(!mpBottomFramebufferPixels);
    #endif

    // Create the renderer and framebuffer texture
    mpSdlWindow = pSdlWindow;
    const Uint32 vsyncFlag = (Config::gbEnableVSync) ? SDL_RENDERER_PRESENTVSYNC : 0;
    #if defined(__3DS__)
        mpRenderer = SDL_CreateRenderer(pSdlWindow, -1, SDL_RENDERER_SOFTWARE | vsyncFlag);
    #else
        mpRenderer = SDL_CreateRenderer(pSdlWindow, -1, SDL_RENDERER_ACCELERATED | vsyncFlag);
    #endif

    if (!mpRenderer) {
        FatalErrors::raise("Failed to create renderer!");
    }

    mpFramebufferTexture = SDL_CreateTexture(
        mpRenderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        ORIG_DRAW_RES_X,
        ORIG_DRAW_RES_Y
    );

    if (!mpFramebufferTexture) {
        FatalErrors::raise("Failed to create a framebuffer texture!");
    }

    #if PSYDOOM_3DS
        mpBottomSdlWindow = SDL_CreateWindow(
            "PsyDoom Bottom Screen",
            SDL_WINDOWPOS_CENTERED_DISPLAY(1),
            SDL_WINDOWPOS_CENTERED_DISPLAY(1),
            320,
            240,
            0
        );

        if (!mpBottomSdlWindow) {
            FatalErrors::raise("Failed to create the bottom-screen window!");
        }

        mpBottomRenderer = SDL_CreateRenderer(mpBottomSdlWindow, -1, SDL_RENDERER_SOFTWARE | vsyncFlag);

        if (!mpBottomRenderer) {
            FatalErrors::raise("Failed to create the bottom-screen renderer!");
        }

        mpBottomFramebufferTexture = SDL_CreateTexture(
            mpBottomRenderer,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING,
            ORIG_DRAW_RES_X,
            ORIG_DRAW_RES_Y
        );

        if (!mpBottomFramebufferTexture) {
            FatalErrors::raise("Failed to create the bottom-screen texture!");
        }

        SDL_SetRenderDrawColor(mpBottomRenderer, 0, 0, 0, 0);
        SDL_RenderClear(mpBottomRenderer);
    #endif
    // Clear the renderer to black
    SDL_SetRenderDrawColor(mpRenderer, 0, 0, 0, 0);
    SDL_RenderClear(mpRenderer);

    #if PSYDOOM_3DS
        gfxSetScreenFormat(GFX_TOP, GSP_RGB5_A1_OES);
        gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB5_A1_OES);
    #endif

    // Immediately lock the framebuffer texture in preparation for the next update
    lockFramebufferTexture();
    #if PSYDOOM_3DS
        lockBottomFramebufferTexture();
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Cleans up and destroys the SDL renderer used by this video backend
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::destroyRenderers() noexcept {
    #if PSYDOOM_3DS
        if (mpBottomFramebufferPixels) {
            unlockBottomFramebufferTexture();
        }

        if (mpBottomFramebufferTexture) {
            SDL_DestroyTexture(mpBottomFramebufferTexture);
            mpBottomFramebufferTexture = nullptr;
        }

        if (mpBottomRenderer) {
            SDL_DestroyRenderer(mpBottomRenderer);
            mpBottomRenderer = nullptr;
        }

        if (mpBottomSdlWindow) {
            SDL_DestroyWindow(mpBottomSdlWindow);
            mpBottomSdlWindow = nullptr;
        }
    #endif

    if (mpFramebufferPixels) {
        unlockFramebufferTexture();
    }

    if (mpFramebufferTexture) {
        SDL_DestroyTexture(mpFramebufferTexture);
        mpFramebufferTexture = nullptr;
    }

    if (mpRenderer) {
        SDL_DestroyRenderer(mpRenderer);
        mpRenderer = nullptr;
    }

    mpSdlWindow = nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Copies the output from the classic renderer (PSX framebuffer) to an SDL texture and then blits that to the screen.
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::displayFramebuffer() noexcept {
    #if PSYDOOM_3DS
        presentNativeFramebuffers();
    #else
        copyPsxToSdlFramebufferTexture();
        presentSdlFramebufferTexture();
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// These functions are no-ops for the SDL backend.
// Don't need to do anything special to display an external surface at any given time.
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::beginExternalSurfaceDisplay() noexcept {
    #if PSYDOOM_3DS
        gfxSetScreenFormat(GFX_TOP, GSP_RGBA8_OES);
    #endif
}

void VideoBackend_SDL::endExternalSurfaceDisplay() noexcept {
    #if PSYDOOM_3DS
        gfxSetScreenFormat(GFX_TOP, GSP_RGB5_A1_OES);
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Displays the specified surface to the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::displayExternalSurface(
    IVideoSurface& surface,
    const int32_t displayX,
    const int32_t displayY,
    const uint32_t displayW,
    const uint32_t displayH,
    const bool bUseFiltering
) noexcept {
    // Must be an SDL video surface!
    ASSERT(dynamic_cast<VideoSurface_SDL*>(&surface));
    VideoSurface_SDL& sdlSurface = static_cast<VideoSurface_SDL&>(surface);

    // Decide source and destination rectangles
    SDL_Rect srcRect = {};
    srcRect.w = (int) sdlSurface.getWidth();
    srcRect.h = (int) sdlSurface.getHeight();

    SDL_Rect dstRect = {};
    dstRect.x = displayX;
    dstRect.y = displayY;
    dstRect.w = (int) displayW;
    dstRect.h = (int) displayH;

    // Clear the screen and blit the surface to the display using the specified scaling.
    // If there is no valid texture then just clear the screen.
    SDL_RenderClear(mpRenderer);
    SDL_Texture* const pSdlTexture = sdlSurface.getTexture();

    if (pSdlTexture) {
        SDL_SetTextureScaleMode(pSdlTexture, (bUseFiltering) ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        SDL_RenderCopy(mpRenderer, pSdlTexture, &srcRect, &dstRect);
    }

    // Present the rendered frame
    SDL_RenderPresent(mpRenderer);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Gives the size of the swapchain/window in pixels
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::getScreenSizeInPixels(uint32_t& width, uint32_t& height) noexcept {
    int sdlWidth = 0;
    int sdlHeight = 0;

    if (mpRenderer) {
        if (SDL_GetRendererOutputSize(mpRenderer, &sdlWidth, &sdlHeight) != 0) {
            // Just to be safe, clear these again on an error...
            sdlWidth = 0;
            sdlHeight = 0;
        }
    }

    width = sdlWidth;
    height = sdlHeight;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Creates and returns an SDL format video surface.
// Fails if renderers have not been initialized.
//------------------------------------------------------------------------------------------------------------------------------------------
std::unique_ptr<IVideoSurface> VideoBackend_SDL::createSurface(const uint32_t width, const uint32_t height) noexcept {
    return (mpRenderer) ? std::make_unique<VideoSurface_SDL>(*mpRenderer, width, height) : std::unique_ptr<IVideoSurface>();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Lock the SDL texture we upload the PSX framebuffer to for writing
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::lockFramebufferTexture() noexcept {
    ASSERT(mpFramebufferTexture);
    ASSERT(!mpFramebufferPixels);
    #if PSYDOOM_3DS
        ASSERT(!mpBottomSdlWindow);
        ASSERT(!mpBottomRenderer);
        ASSERT(!mpBottomFramebufferTexture);
        ASSERT(!mpBottomFramebufferPixels);
    #endif

    int pitch = 0;

    if (SDL_LockTexture(mpFramebufferTexture, nullptr, reinterpret_cast<void**>(&mpFramebufferPixels), &pitch) != 0) {
        FatalErrors::raise("Failed to lock the framebuffer texture for writing!");
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Unlock the SDL texture containing the PSX framebuffer after we finish writing to it
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::unlockFramebufferTexture() noexcept {
    ASSERT(mpFramebufferPixels);
    ASSERT(mpFramebufferTexture);

    SDL_UnlockTexture(mpFramebufferTexture);
    mpFramebufferPixels = nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Copies the rendered PSX GPU framebuffer the locked SDL texture, in preparation for blitting to the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::copyPsxToSdlFramebufferTexture() noexcept {
    // Sanity checks
    ASSERT(mpFramebufferPixels);

    // Copy the framebuffer
    Gpu::Core& gpu = PsxVm::gGpu;
    const Gpu::Color16* const vramPixels = reinterpret_cast<const Gpu::Color16*>(gpu.pRam);
    uint32_t* pDstPixel = mpFramebufferPixels;

    for (uint32_t y = 0; y < ORIG_DRAW_RES_Y; ++y) {
        const Gpu::Color16* const rowPixels = vramPixels + ((intptr_t) y + gpu.displayAreaY) * gpu.ramPixelW;
        const uint32_t xStart = (uint32_t) gpu.displayAreaX;
        const uint32_t xEnd = xStart + ORIG_DRAW_RES_X;
        ASSERT(xEnd <= gpu.ramPixelW);

        // Note: don't bother doing multiple pixels at a time - compiler is smart and already optimizes this to use SIMD
        for (uint32_t x = xStart; x < xEnd; ++x, ++pDstPixel) {
            const Gpu::Color16 srcPixel = rowPixels[x];
            const uint32_t r = (uint32_t) srcPixel.getR() << 3;
            const uint32_t g = (uint32_t) srcPixel.getG() << 3;
            const uint32_t b = (uint32_t) srcPixel.getB() << 3;

            *pDstPixel = (0xFF000000 | (b << 16) | (g << 8 ) | (r << 0));
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Presents the SDL texture which contains a rendered frame from the PSX GPU to the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::presentSdlFramebufferTexture() noexcept {
    // Sanity checks
    ASSERT(mpSdlWindow);
    ASSERT(mpRenderer);
    ASSERT(mpFramebufferTexture);

    // Get the size of the window in pixels and don't bother outputting to it if zero sized
    int windowW = {};
    int windowH = {};
    SDL_GetRendererOutputSize(mpRenderer, &windowW, &windowH);

    if ((windowW <= 0) || (windowH <= 0))
        return;

    // Get the window area to output the PSX framebuffer to  and don't bother outputting if zero sized
    float outputRectX = {};
    float outputRectY = {};
    float outputRectW = {};
    float outputRectH = {};

    Video::getClassicFramebufferWindowRect(
        (float) windowW,
        (float) windowH,
        outputRectX,
        outputRectY,
        outputRectW,
        outputRectH
    );

    if ((outputRectW <= 0.0f) || (outputRectH <= 0.0f))
        return;

    // These are the source and destination regions to blit
    ASSERT((Video::gTopOverscan >= 0) && (Video::gTopOverscan < Video::ORIG_DRAW_RES_Y / 2));
    ASSERT((Video::gBotOverscan >= 0) && (Video::gBotOverscan < Video::ORIG_DRAW_RES_Y / 2));

    SDL_Rect srcRect = {};
    srcRect.y = Video::gTopOverscan;
    srcRect.w = Video::ORIG_DRAW_RES_X;
    srcRect.h = Video::ORIG_DRAW_RES_Y - Video::gTopOverscan - Video::gBotOverscan;

    SDL_Rect dstRect = {};
    dstRect.x = (int) outputRectX;
    dstRect.y = (int) outputRectY;
    dstRect.w = (int) std::ceil(outputRectW);
    dstRect.h = (int) std::ceil(outputRectH);

    // Done writing to the locked framebuffer, update the texture with whatever writes we made
    unlockFramebufferTexture();

    // Need to clear the window if we are not filling the whole screen.
    // Some stuff like NVIDIA video recording notifications can leave marks in the unused regions otherwise...
    if ((dstRect.w != windowW) || (dstRect.h != windowH)) {
        SDL_RenderClear(mpRenderer);
    }

    // Blit the framebuffer to the display
    SDL_RenderCopy(mpRenderer, mpFramebufferTexture, &srcRect, &dstRect);

    // Present the rendered frame and re-lock the framebuffer texture
    SDL_RenderPresent(mpRenderer);
    lockFramebufferTexture();
}

#if PSYDOOM_3DS
void VideoBackend_SDL::lockBottomFramebufferTexture() noexcept {
    ASSERT(mpBottomFramebufferTexture);
    ASSERT(!mpBottomFramebufferPixels);

    int pitch = 0;

    if (SDL_LockTexture(mpBottomFramebufferTexture, nullptr, reinterpret_cast<void**>(&mpBottomFramebufferPixels), &pitch) != 0) {
        FatalErrors::raise("Failed to lock the bottom-screen texture!");
    }
}

void VideoBackend_SDL::unlockBottomFramebufferTexture() noexcept {
    ASSERT(mpBottomFramebufferPixels);
    ASSERT(mpBottomFramebufferTexture);

    SDL_UnlockTexture(mpBottomFramebufferTexture);
    mpBottomFramebufferPixels = nullptr;
}

void VideoBackend_SDL::updateBottomFramebufferTexture() noexcept {
    ASSERT(mpBottomFramebufferPixels);
    ASSERT(mpFramebufferPixels);

    const size_t numPixels = ORIG_DRAW_RES_X * ORIG_DRAW_RES_Y;

    if (gbIsLevelDataCached && !gbGamePaused) {
        std::fill_n(mpBottomFramebufferPixels, numPixels, 0xFF000000u);
        AM_DrawerToExternal(drawBottomMapLine, mpBottomFramebufferPixels);
    } else {
        std::memcpy(mpBottomFramebufferPixels, mpFramebufferPixels, numPixels * sizeof(uint32_t));
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Stereoscopic 3D: bank the eye that has just been rendered onto the top screen, without touching the bottom screen
// or swapping. The second eye then renders over the same PlayStation framebuffer and is presented normally.
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::presentTopScreenOnly() noexcept {
    drawTopScreen(true);
}

void VideoBackend_SDL::presentNativeFramebuffers() noexcept {
    #if defined(PSYDOOM_3DS_BENCHMARK) && PSYDOOM_3DS_BENCHMARK
        const auto presentStartTime = std::chrono::high_resolution_clock::now();
    #endif
    Gpu::Core& gpu = PsxVm::gGpu;
    const Gpu::Color16* const vramPixels = reinterpret_cast<const Gpu::Color16*>(gpu.pRam);
    const NativeFramebuffer16 bottomFramebuffer = getFramebuffer(GFX_BOTTOM);

    const Screens3DS::BottomScreen bottomScreenMode = Screens3DS::getBottomScreen();
    const int32_t menuSplitY = Screens3DS::getMenuSplitY();
    const bool bMenuSplit = ((bottomScreenMode == Screens3DS::BottomScreen::MenuSplit) && (menuSplitY > 0));

    PSYDOOM_PROF_BEGIN(PresentTop);

    // A two pass menu banks the top screen itself, before it redraws the framebuffer for the touch screen
    if (!Screens3DS::wasTopScreenAlreadyDrawn()) {
        drawTopScreen();
    }

    Screens3DS::setTopScreenAlreadyDrawn(false);

    PSYDOOM_PROF_END(PresentTop);
    PSYDOOM_PROF_BEGIN(PresentBottom);

    // Unless the bottom screen is showing part of the framebuffer, touches cannot be mapped to menu coordinates
    Screens3DS::setBottomScreenMapping(0, 0, 0, 0, 0, 0, 0, 0);

    if (bottomScreenMode == Screens3DS::BottomScreen::Blank) {
        clearFramebuffer(bottomFramebuffer);
    }
    else if ((bottomScreenMode == Screens3DS::BottomScreen::Automap) ||
             (bottomScreenMode == Screens3DS::BottomScreen::AutomapStatusBarTop) ||
             (bottomScreenMode == Screens3DS::BottomScreen::AutomapStatusBarBottom))
    {
        clearFramebuffer(bottomFramebuffer);

        // The status bar takes a strip along the top or the bottom of the touch screen, leaving the rest for the automap
        const bool bStatusBarAtTop = (bottomScreenMode == Screens3DS::BottomScreen::AutomapStatusBarTop);
        const bool bDrawStatusBar = (bStatusBarAtTop || (bottomScreenMode == Screens3DS::BottomScreen::AutomapStatusBarBottom));
        const int32_t automapH = (bDrawStatusBar) ? (240 - Screens3DS::BOTTOM_STATUS_BAR_H) : 240;
        const int32_t automapY = (bStatusBarAtTop) ? Screens3DS::BOTTOM_STATUS_BAR_H : 0;

        // Show as much of the map as the room allows, at a fixed scale. Moving the status bar onto this screen
        // therefore shows less of the level rather than shrinking what is on it - the map does not change zoom just
        // because the status bar moved. A pixel is kept in hand on each edge so the very last row cannot land outside
        // the framebuffer.
        AutomapTarget mapTarget = {};
        mapTarget.framebuffer = bottomFramebuffer;
        mapTarget.centerX = bottomFramebuffer.width / 2;
        mapTarget.centerY = automapY + automapH / 2;
        // The rows and columns the map is allowed to occupy, which the status bar has already been kept out of
        mapTarget.minScreenX = 0;
        mapTarget.maxScreenX = bottomFramebuffer.width - 1;
        mapTarget.minScreenY = automapY;
        mapTarget.maxScreenY = automapY + automapH - 1;

        // Turn a distance in screen pixels into a map distance that scales back up to at least that far. Rounding the
        // other way leaves the map short of the edge, and short by more than it looks: the scale skips some screen
        // offsets entirely, so a bound that merely fits can be a further pixel in.
        const auto screenToMapExtent = [](const int32_t screenExtent) noexcept {
            return (screenExtent * BOTTOM_MAP_SCALE_DEN + BOTTOM_MAP_SCALE_NUM - 1) / BOTTOM_MAP_SCALE_NUM;
        };

        // Note the signs: map y counts upwards, so the larger bound is the one towards the top of the screen
        mapTarget.bounds.maxX = screenToMapExtent(mapTarget.maxScreenX - mapTarget.centerX);
        mapTarget.bounds.minX = -screenToMapExtent(mapTarget.centerX - mapTarget.minScreenX);
        mapTarget.bounds.maxY = screenToMapExtent(mapTarget.centerY - mapTarget.minScreenY);
        mapTarget.bounds.minY = -screenToMapExtent(mapTarget.maxScreenY - mapTarget.centerY);

        const auto drawNativeMapLine = [](
            void* const userData,
            const uint32_t color,
            int32_t x1,
            int32_t y1,
            int32_t x2,
            int32_t y2
        ) noexcept {
            const AutomapTarget& target = *static_cast<AutomapTarget*>(userData);

            if (!clipBottomMapLine(x1, y1, x2, y2, target.bounds))
                return;

            const NativeFramebuffer16& framebuffer = target.framebuffer;

            x1 = target.centerX + (x1 * BOTTOM_MAP_SCALE_NUM) / BOTTOM_MAP_SCALE_DEN;
            x2 = target.centerX + (x2 * BOTTOM_MAP_SCALE_NUM) / BOTTOM_MAP_SCALE_DEN;
            y1 = target.centerY - (y1 * BOTTOM_MAP_SCALE_NUM) / BOTTOM_MAP_SCALE_DEN;
            y2 = target.centerY - (y2 * BOTTOM_MAP_SCALE_NUM) / BOTTOM_MAP_SCALE_DEN;

            // The clip above works in map units and the bounds round outwards, so an endpoint can land a pixel past
            // the edge. Bring it back rather than dropping the line, which is what left a black margin before.
            x1 = std::clamp(x1, target.minScreenX, target.maxScreenX);
            x2 = std::clamp(x2, target.minScreenX, target.maxScreenX);
            y1 = std::clamp(y1, target.minScreenY, target.maxScreenY);
            y2 = std::clamp(y2, target.minScreenY, target.maxScreenY);

            const int32_t deltaX = std::abs(x2 - x1);
            const int32_t stepX = (x1 < x2) ? 1 : -1;
            const int32_t deltaY = std::abs(y2 - y1);
            const int32_t stepY = (y1 < y2) ? 1 : -1;
            int32_t error = deltaX - deltaY;
            const uint16_t red = static_cast<uint16_t>((color >> 19) & 0x1Fu);
            const uint16_t green = static_cast<uint16_t>((color >> 11) & 0x1Fu);
            const uint16_t blue = static_cast<uint16_t>((color >> 3) & 0x1Fu);
            const uint16_t pixelColor = static_cast<uint16_t>((red << 11) | (green << 6) | (blue << 1) | 1u);

            while (true) {
                framebuffer.pixels[static_cast<size_t>(framebuffer.pitch) * x1 + framebuffer.pitch - y1 - 1] = pixelColor;

                if ((x1 == x2) && (y1 == y2))
                    break;

                const int32_t doubledError = error * 2;
                if (doubledError > -deltaY) {
                    error -= deltaY;
                    x1 += stepX;
                }
                if (doubledError < deltaX) {
                    error += deltaX;
                    y1 += stepY;
                }
            }
        };

        AM_DrawerToExternal(drawNativeMapLine, &mapTarget);

        // Blit the status bar underneath the map. It was drawn into its usual place in the framebuffer by a second
        // pass over the menu-free part of the frame, after the 3D view had already been banked to the top screen.
        if (bDrawStatusBar) {
            copyToScreen(
                vramPixels,
                gpu.ramPixelW,
                gpu.displayAreaX,
                gpu.displayAreaY + BASE_VIEW_3D_H,
                ORIG_DRAW_RES_X,
                ORIG_DRAW_RES_Y - BASE_VIEW_3D_H,
                bottomFramebuffer,
                0,
                (bStatusBarAtTop) ? 0 : automapH,
                320,
                Screens3DS::BOTTOM_STATUS_BAR_H,
                false           // The automap is already on this screen; do not wipe it
            );
        }
    }
    else {
        // Either the rows a two pass menu asked for, the background pass of a presentation screen, or everything below
        // the menu split. Scaled to fit without distorting it, since menu text is small enough that a stretch shows.
        const bool bBackgroundOnly = (bottomScreenMode == Screens3DS::BottomScreen::BackgroundOnly);
        const bool bTwoPass = (bBackgroundOnly || (bottomScreenMode == Screens3DS::BottomScreen::MenuTwoPass));
        const int32_t srcY = (
            (bBackgroundOnly) ? Screens3DS::BOT_PASS_SRC_Y :
            (bTwoPass) ? Screens3DS::getBottomPassSrcY() :
            ((bMenuSplit) ? menuSplitY : 0)
        );
        const int32_t srcH = (bTwoPass) ? Screens3DS::BOT_PASS_ROWS : (ORIG_DRAW_RES_Y - srcY);
        const int32_t scaleNum = std::min((320 * 16) / ORIG_DRAW_RES_X, (240 * 16) / srcH);
        const int32_t drawW = (ORIG_DRAW_RES_X * scaleNum) / 16;
        const int32_t drawH = (srcH * scaleNum) / 16;
        const int32_t drawX = (320 - drawW) / 2;
        const int32_t drawY = (240 - drawH) / 2;

        copyToScreen(
            vramPixels,
            gpu.ramPixelW,
            gpu.displayAreaX,
            gpu.displayAreaY + srcY,
            ORIG_DRAW_RES_X,
            srcH,
            bottomFramebuffer,
            drawX,
            drawY,
            drawW,
            drawH
        );

        Screens3DS::setBottomScreenMapping(drawX, drawY, drawW, drawH, 0, srcY, ORIG_DRAW_RES_X, srcH);
    }

    Screens3DS::resolveTouchItems();

    // PsyDoom 3DS: link health for local wireless games. A small square in the top right corner of the touch screen:
    // green when packets are arriving on time, yellow when the round trip is eating into the 30 Hz tick budget, red
    // when the link is struggling, and flashing white while searching for the other console.
    {
        const NetworkUds3DS::LinkHealth health = NetworkUds3DS::getHealth();

        if ((health != NetworkUds3DS::LinkHealth::Disconnected) && bottomFramebuffer.pixels) {
            uint16_t iconColor = 0;

            switch (health) {
                case NetworkUds3DS::LinkHealth::Good:   iconColor = (uint16_t)((2 << 11) | (31 << 6) | (2 << 1) | 1);    break;
                case NetworkUds3DS::LinkHealth::Fair:   iconColor = (uint16_t)((31 << 11) | (31 << 6) | (2 << 1) | 1);   break;
                case NetworkUds3DS::LinkHealth::Poor:   iconColor = (uint16_t)((31 << 11) | (2 << 6) | (2 << 1) | 1);    break;

                default: {
                    // Searching: blink so it is obviously doing something rather than stuck
                    const bool bOn = (((gTicCon / 15) & 1) == 0);
                    iconColor = (bOn) ? (uint16_t)((31 << 11) | (31 << 6) | (31 << 1) | 1) : (uint16_t)((8 << 11) | (8 << 6) | (8 << 1) | 1);
                }   break;
            }

            constexpr int32_t ICON_SIZE = 10;
            constexpr int32_t ICON_MARGIN = 4;
            constexpr uint16_t ICON_BORDER = 1;     // Opaque black, so the icon reads against a bright automap line

            for (int32_t x = 0; x < ICON_SIZE + 2; ++x) {
                const int32_t screenX = 320 - ICON_MARGIN - ICON_SIZE - 1 + x;
                uint16_t* const column = bottomFramebuffer.pixels + (size_t) bottomFramebuffer.pitch * screenX;

                for (int32_t y = 0; y < ICON_SIZE + 2; ++y) {
                    const int32_t screenY = ICON_MARGIN - 1 + y;
                    const bool bIsBorder = ((x == 0) || (y == 0) || (x == ICON_SIZE + 1) || (y == ICON_SIZE + 1));
                    column[bottomFramebuffer.pitch - screenY - 1] = (bIsBorder) ? ICON_BORDER : iconColor;
                }
            }
        }
    }

    PSYDOOM_PROF_END(PresentBottom);
    PSYDOOM_PROF_BEGIN(PresentSwap);
    gfxFlushBuffers();
    gfxSwapBuffers();
    PSYDOOM_PROF_END(PresentSwap);
    PSYDOOM_PROF_END_FRAME();

    #if defined(PSYDOOM_3DS_BENCHMARK) && PSYDOOM_3DS_BENCHMARK
        static std::FILE* pPresentFile = nullptr;
        static uint32_t presentSamples = 0;
        static int64_t presentUsec = 0;
        presentUsec += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - presentStartTime
        ).count();
        ++presentSamples;

        if (presentSamples >= 30) {
            if (!pPresentFile) {
                pPresentFile = std::fopen(
                    "sdmc:/3ds/PsyDoom/" PSYDOOM_3DS_VARIANT_DIR "/present.csv",
                    "w"
                );
                if (pPresentFile) {
                    std::fputs("present_usec\n", pPresentFile);
                }
            }
            if (pPresentFile) {
                std::fprintf(pPresentFile, "%.3f\n", static_cast<double>(presentUsec) / presentSamples);
                std::fflush(pPresentFile);
            }
            presentSamples = 0;
            presentUsec = 0;
        }
    #endif
}

void VideoBackend_SDL::presentBottomFramebufferTexture() noexcept {
    ASSERT(mpBottomSdlWindow);
    ASSERT(mpBottomRenderer);
    ASSERT(mpBottomFramebufferTexture);

    int outputWidth = 0;
    int outputHeight = 0;
    SDL_GetRendererOutputSize(mpBottomRenderer, &outputWidth, &outputHeight);

    if ((outputWidth <= 0) || (outputHeight <= 0))
        return;

    SDL_Rect srcRect = {};
    srcRect.w = ORIG_DRAW_RES_X;
    srcRect.h = ORIG_DRAW_RES_Y;

    SDL_Rect dstRect = {};
    dstRect.w = outputWidth;
    dstRect.h = outputHeight;

    unlockBottomFramebufferTexture();
    SDL_RenderClear(mpBottomRenderer);
    SDL_SetTextureScaleMode(mpBottomFramebufferTexture, SDL_ScaleModeNearest);
    SDL_RenderCopy(mpBottomRenderer, mpBottomFramebufferTexture, &srcRect, &dstRect);
    SDL_RenderPresent(mpBottomRenderer);
    lockBottomFramebufferTexture();
}
#endif
END_NAMESPACE(Video)
