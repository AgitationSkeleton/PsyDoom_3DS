#include "Gpu.h"

#include "Asserts.h"

#include <algorithm>
#include <cstring>

BEGIN_NAMESPACE(Gpu)

#if PSYDOOM_3DS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: current low detail steps for the wall/floor fast paths. Default to full detail.
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t gPixelStepX = 1;
int32_t gPixelStepY = 1;

void setLowDetailSteps(const int32_t stepX, const int32_t stepY) noexcept {
    gPixelStepX = std::clamp<int32_t>(stepX, 1, MAX_PIXEL_STEP_X);
    gPixelStepY = std::clamp<int32_t>(stepY, 1, MAX_PIXEL_STEP_Y);
}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Rounds the given number up to the next power of two if it's not a power of two
//------------------------------------------------------------------------------------------------------------------------------------------
static uint32_t nextPow2(const uint32_t numIn) noexcept {
    if (numIn == 0)
        return 0;

    uint32_t highestBit = 0;

    for (uint32_t bitsLeft = numIn >> 1; bitsLeft != 0; bitsLeft >>= 1) {
        ++highestBit;
    }

    const uint32_t floorPow2 = (uint32_t) 1u << highestBit;
    return (floorPow2 < numIn) ? floorPow2 << 1 : floorPow2;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Core initialization and teardown
//------------------------------------------------------------------------------------------------------------------------------------------
void initCore(Core& core, const uint16_t ramPixelW, const uint16_t ramPixelH) noexcept {
    // Zero init everything by default, allocate the VRAM and then default some state
    ASSERT(ramPixelW > 0);
    ASSERT(ramPixelH > 0);

    core = {};

    core.pRam = new std::uint16_t[(size_t) ramPixelW * ramPixelH];
    std::memset(core.pRam, 0, sizeof(uint16_t) * ramPixelW * ramPixelH);    // Zero init VRAM

    core.ramPixelW = (uint16_t) nextPow2(ramPixelW);
    core.ramPixelH = (uint16_t) nextPow2(ramPixelH);
    core.ramXMask = ramPixelW - 1;
    core.ramYMask = ramPixelH - 1;

    core.drawAreaLx = 0;
    core.drawAreaRx = 256;
    core.drawAreaTy = 0;
    core.drawAreaBy = 240;
    core.displayAreaX = 0;
    core.displayAreaY = 0;
    core.displayAreaW = 256;
    core.displayAreaH = 240;

    core.texPageX = 256;
    core.texPageY = 0;
    core.texPageXMask = 0xFF;
    core.texPageXMask = 0xFF;
    core.texWinX = 0;
    core.texWinY = 0;
    core.texWinXMask = 0xFF;
    core.texWinYMask = 0xFF;

    core.blendMode = BlendMode::Alpha50;
    core.texFmt = TexFmt::Bpp16;
    core.clutX = 0;
    core.clutY = 240;
    core.bDisableMasking = false;

    core.clutCacheX = UINT16_MAX;
    core.clutCacheY = UINT16_MAX;
    core.clutCacheFmt = {};
    std::memset(&core.clutCache->bits, 0, sizeof(core.clutCache));
}

void destroyCore(Core& core) noexcept {
    delete[] core.pRam;
    core = {};
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Sanity checks GPU state in debug to make sure it is good for drawing
//------------------------------------------------------------------------------------------------------------------------------------------
static void sanityCheckGpuDrawState([[maybe_unused]] const Core& core) noexcept {
    // Drawing area cannot wrap around in RAM
    ASSERT(core.drawAreaRx < core.ramPixelW);
    ASSERT(core.drawAreaBy < core.ramPixelH);

    // CLUT cannot wrap around in RAM
    #if ASSERTS_ENABLED
        if (core.texFmt == TexFmt::Bpp4) {
            ASSERT(core.clutX + 16 < core.ramPixelW);
            ASSERT(core.clutY < core.ramPixelH);
        }
        else if (core.texFmt == TexFmt::Bpp8) {
            ASSERT(core.clutX + 256 < core.ramPixelW);
            ASSERT(core.clutY < core.ramPixelH);
        }
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Read a single unsigned 16-bit value from VRAM at the given absolute coordinate and wrap to VRAM boundaries
//------------------------------------------------------------------------------------------------------------------------------------------
uint16_t vramReadU16(const Core& core, const uint16_t x, const uint16_t y) noexcept {
    const uint16_t xt = x & core.ramXMask;
    const uint16_t yt = y & core.ramYMask;
    return core.pRam[yt * core.ramPixelW + xt];
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Write a single unsigned 16-bit value to VRAM at the given absolute coordinate and wrap to VRAM boundaries
//------------------------------------------------------------------------------------------------------------------------------------------
void vramWriteU16(Core& core, const uint16_t x, const uint16_t y, const uint16_t value) noexcept {
    const uint16_t xt = x & core.ramXMask;
    const uint16_t yt = y & core.ramYMask;
    core.pRam[yt * core.ramPixelW + xt] = value;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Read a texture using the given texture coordinate within the current texture page and window.
// This internal version is templated for higher performance and assumes the CLUT cache is up to date.
//------------------------------------------------------------------------------------------------------------------------------------------
template <TexFmt TexFmt>
static Color16 readTexel(
    const Core& core,
    const uint16_t coordX,
    const uint16_t coordY
) noexcept {
    // First mask the coordinates by the texture window mask
    uint16_t vramX = coordX & core.texWinXMask;
    uint16_t vramY = coordY & core.texWinYMask;
    vramX += core.texWinX;
    vramY += core.texWinY;

    if constexpr (TexFmt == TexFmt::Bpp4) {
        vramX /= 4;
    } else if constexpr (TexFmt == TexFmt::Bpp8) {
        vramX /= 2;
    }

    vramX &= core.texPageXMask;
    vramY &= core.texPageYMask;
    vramX += core.texPageX;
    vramY += core.texPageY;

    // Read the VRAM pixel and then handle according to color mode
    const uint16_t vramPixel = vramReadU16(core, vramX, vramY);

    if constexpr (TexFmt == TexFmt::Bpp4) {
        // 4-bit color: each 16-bit pixel has 4 CLUT indexes. Get the CLUT index then lookup the color from the CLUT.
        const uint16_t clutIdx = (vramPixel >> ((coordX & 3) * 4)) & 0xF;
        return core.clutCache[clutIdx];
    }
    else if constexpr (TexFmt == TexFmt::Bpp8) {
        // 8-bit color: each 16-bit pixel has 2 CLUT indexes. Get the CLUT index then lookup the color from the CLUT.
        const uint16_t clutIdx = (vramPixel >> ((coordX & 1) * 8)) & 0xFF;
        return core.clutCache[clutIdx];
    }
    else {
        // Direct 16-bit color, no CLUT
        static_assert(TexFmt == TexFmt::Bpp16);
        return vramPixel;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Read a texel using dynamic dispatch (slower)
//------------------------------------------------------------------------------------------------------------------------------------------
Color16 readTexel(Core& core, const uint16_t coordX, const uint16_t coordY) noexcept {
    updateClutCache(core);

    switch (core.texFmt) {
        case TexFmt::Bpp4:  return readTexel<TexFmt::Bpp4>(core, coordX, coordY);
        case TexFmt::Bpp8:  return readTexel<TexFmt::Bpp8>(core, coordX, coordY);

        default:
            ASSERT(core.texFmt == TexFmt::Bpp16);
            return readTexel<TexFmt::Bpp16>(core, coordX, coordY);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Makes sure the CLUT cache is up to date for the current CLUT settings
//------------------------------------------------------------------------------------------------------------------------------------------
void updateClutCache(Core& core) noexcept {
    sanityCheckGpuDrawState(core);

    // Do we need to update the cache?
    const TexFmt newTexFmt = core.texFmt;

    const bool bCacheNeedsUpdate = (
        (core.clutCacheX != core.clutX) ||
        (core.clutCacheY != core.clutY) ||
        (core.clutCacheFmt != newTexFmt)
    );

    if (!bCacheNeedsUpdate)
        return;

    // Don't update unless changed
    core.clutCacheX = core.clutX;
    core.clutCacheY = core.clutY;
    core.clutCacheFmt = newTexFmt;

    // Save the CLUT cache entries unless in 16-bit mode
    if (newTexFmt != TexFmt::Bpp16) {
        ASSERT((newTexFmt == TexFmt::Bpp4) || (newTexFmt == TexFmt::Bpp8));
        std::memcpy(
            &core.clutCache->bits,
            core.pRam + (core.clutY * core.ramPixelW + core.clutX),
            sizeof(uint16_t) * ((newTexFmt == TexFmt::Bpp4) ? 16 : 256)
        );
    }
}

#if PSYDOOM_3DS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: rebuild the texel address tables if the texture window or page has changed since they were last built.
// Returns false if the texture window is bigger than the tables can hold, which Doom's textures never are.
//------------------------------------------------------------------------------------------------------------------------------------------
bool updateTexAddrCache(Core& core) noexcept {
    // The tables are indexed by the masked coordinate, so the mask has to fit. Bit 0 of the x mask must also be set,
    // because the table's shift entry assumes masking leaves the low bit of 'u' alone.
    if ((core.texWinXMask >= Core::TEX_ADDR_CACHE_SIZE) || (core.texWinYMask >= Core::TEX_ADDR_CACHE_SIZE))
        return false;

    if ((core.texWinXMask & 1) == 0)
        return false;

    const bool bStillValid = (
        core.bTexAddrCacheValid &&
        (core.texAddrCacheWinX == core.texWinX) &&
        (core.texAddrCacheWinY == core.texWinY) &&
        (core.texAddrCacheWinXMask == core.texWinXMask) &&
        (core.texAddrCacheWinYMask == core.texWinYMask) &&
        (core.texAddrCachePageX == core.texPageX) &&
        (core.texAddrCachePageY == core.texPageY) &&
        (core.texAddrCachePageXMask == core.texPageXMask) &&
        (core.texAddrCachePageYMask == core.texPageYMask)
    );

    if (bStillValid)
        return true;

    core.bTexAddrCacheValid = true;
    core.texAddrCacheWinX = core.texWinX;
    core.texAddrCacheWinY = core.texWinY;
    core.texAddrCacheWinXMask = core.texWinXMask;
    core.texAddrCacheWinYMask = core.texWinYMask;
    core.texAddrCachePageX = core.texPageX;
    core.texAddrCachePageY = core.texPageY;
    core.texAddrCachePageXMask = core.texPageXMask;
    core.texAddrCachePageYMask = core.texPageYMask;

    for (uint32_t i = 0; i <= core.texWinXMask; ++i) {
        uint16_t vramX = (uint16_t) i;
        vramX += core.texWinX;
        vramX /= 2;
        vramX &= core.texPageXMask;
        vramX += core.texPageX;

        core.texAddrCacheU[i] = vramX & core.ramXMask;
        core.texAddrCacheUShift[i] = (uint8_t)((i & 1) * 8);
    }

    for (uint32_t i = 0; i <= core.texWinYMask; ++i) {
        uint16_t vramY = (uint16_t) i;
        vramY += core.texWinY;
        vramY &= core.texPageYMask;
        vramY += core.texPageY;

        core.texAddrCacheV[i] = (uint32_t)(vramY & core.ramYMask) * core.ramPixelW;
    }

    return true;
}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Tells if the given pixel is inside the drawing area
//------------------------------------------------------------------------------------------------------------------------------------------
bool isPixelInDrawArea(const Core& core, const uint16_t x, const uint16_t y) noexcept {
    return (
        (x >= core.drawAreaLx) &&
        (x <= core.drawAreaRx) &&
        (y >= core.drawAreaTy) &&
        (y <= core.drawAreaBy)
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Clears a region of VRAM to the specified color
//------------------------------------------------------------------------------------------------------------------------------------------
void clearRect(Core& core, const Color16 color, const uint16_t x, const uint16_t y, const uint16_t w, const uint16_t h) noexcept {
    // Caching GPU state
    uint16_t* const pRam = core.pRam;
    const uint16_t ramPixelW = core.ramPixelW;
    const uint16_t ramPixelH = core.ramPixelH;

    // Clear the rectangle
    const uint16_t begX = std::min(x, ramPixelW);
    const uint16_t begY = std::min(y, ramPixelH);
    const uint16_t endX = (uint16_t) std::min<int32_t>((int32_t) x + w, (int32_t) ramPixelW);
    const uint16_t endY = (uint16_t) std::min<int32_t>((int32_t) y + h, (int32_t) ramPixelH);

    for (uint16_t curY = begY; curY < endY; ++curY) {
        for (uint16_t curX = begX; curX < endX; ++curX) {
            pRam[(uint32_t) curY * ramPixelW + curX] = color;
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert a 24-bit color to 16-bit.
// 
// Notes:
//  (1) The conversion calculations vary depending on the draw mode. For textured primitives on the PSX, '128' is regarded as full bright.
//      Anything over '128' for textured primitives is 'extra bright'. For untextured primitives on the PSX however, '255' is full bright.
//      Hence the conversions are handled differently depending on the draw mode.
//  (2) The blending/semi-transparency flag is set to '0' during this conversion and overbright colors are saturated.
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
Color16 color24FTo16(const Color24F colorIn) noexcept {
    constexpr bool bIsTextured = ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended));
    constexpr uint32_t COLOR_NORMALIZE = (bIsTextured) ? 128 : 255;
    constexpr uint32_t ROUND_UP = (bIsTextured) ? 0 : 128;

    return Color16::make(
        (uint16_t) std::min<uint32_t>(((uint32_t) colorIn.comp.r * 31u + ROUND_UP) / COLOR_NORMALIZE, 31u),
        (uint16_t) std::min<uint32_t>(((uint32_t) colorIn.comp.g * 31u + ROUND_UP) / COLOR_NORMALIZE, 31u),
        (uint16_t) std::min<uint32_t>(((uint32_t) colorIn.comp.b * 31u + ROUND_UP) / COLOR_NORMALIZE, 31u)
    );
}

// Instantiate the variants of this function
template Color16 color24FTo16<DrawMode::Colored>(const Color24F colorIn) noexcept;
template Color16 color24FTo16<DrawMode::ColoredBlended>(const Color24F colorIn) noexcept;
template Color16 color24FTo16<DrawMode::Textured>(const Color24F colorIn) noexcept;
template Color16 color24FTo16<DrawMode::TexturedBlended>(const Color24F colorIn) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Modulate a 16-bit color by a 24-bit one where the components are in 1.7 fixed point format
//------------------------------------------------------------------------------------------------------------------------------------------
Color16 colorMul(const Color16 color1, const Color24F color2) noexcept {
    return Color16::make(
        (uint16_t) std::min((color1.getR() * color2.comp.r) >> 7, 31),
        (uint16_t) std::min((color1.getG() * color2.comp.g) >> 7, 31),
        (uint16_t) std::min((color1.getB() * color2.comp.b) >> 7, 31),
        color1.getT()
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Blend the two colors (foreground and background) and return the result
//------------------------------------------------------------------------------------------------------------------------------------------
Color16 colorBlend(const Color16 bg, const Color16 fg, const BlendMode mode) noexcept {
    Color16 result = fg;

    switch (mode) {
        case BlendMode::Alpha50:
            result.setRGB(
                (uint16_t)((bg.getR() + fg.getR()) >> 1),
                (uint16_t)((bg.getG() + fg.getG()) >> 1),
                (uint16_t)((bg.getB() + fg.getB()) >> 1)
            );
            break;

        case BlendMode::Add:
            result.setRGB(
                (uint16_t) std::min(bg.getR() + fg.getR(), 31),
                (uint16_t) std::min(bg.getG() + fg.getG(), 31),
                (uint16_t) std::min(bg.getB() + fg.getB(), 31)
            );
            break;

        case BlendMode::Subtract:
            result.setRGB(
                (uint16_t) std::max<int32_t>((int32_t) bg.getR() - (int32_t) fg.getR(), 0),
                (uint16_t) std::max<int32_t>((int32_t) bg.getG() - (int32_t) fg.getG(), 0),
                (uint16_t) std::max<int32_t>((int32_t) bg.getB() - (int32_t) fg.getB(), 0)
            );
            break;

        case BlendMode::Add25:
            result.setRGB(
                (uint16_t) std::min(bg.getR() + (fg.getR() >> 2), 31),
                (uint16_t) std::min(bg.getG() + (fg.getG() >> 2), 31),
                (uint16_t) std::min(bg.getB() + (fg.getB() >> 2), 31)
            );
            break;
    }

    return result;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a rectangle - internal implementation tailored to each texture format
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode, TexFmt TexFmt>
static void draw(Core& core, const DrawRect& rect) noexcept {
    sanityCheckGpuDrawState(core);

    // According to the NO$PSX specs rectangle sizes cannot exceed 1023 x 511
    if ((rect.w >= 1024) || (rect.h >= 512))
        return;

    // If we're going to draw textured and with a CLUT make sure it is up to date
    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        if constexpr ((TexFmt == TexFmt::Bpp4) || (TexFmt == TexFmt::Bpp8)) {
            updateClutCache(core);
        }
    }

    // Clip the rectangle bounds to the draw area and generate adjustments to the uv coords if that happens.
    // Note must translate the rectangle according to the draw offset too...
    const int16_t rectTx = rect.x + core.drawOffsetX;
    const int16_t rectTy = rect.y + core.drawOffsetY;

    int16_t begX = rectTx;
    int16_t begY = rectTy;
    uint16_t topLeftU = rect.u;
    uint16_t topLeftV = rect.v;

    if (begX < (int16_t) core.drawAreaLx) {
        topLeftU += core.drawAreaLx - begX;
        begX = core.drawAreaLx;
    }

    if (begY < (int16_t) core.drawAreaTy) {
        topLeftV += core.drawAreaTy - begY;
        begY = core.drawAreaTy;
    }

    const int16_t endX = (int16_t) std::min((int) rectTx + rect.w, core.drawAreaRx + 1);
    const int16_t endY = (int16_t) std::min((int) rectTy + rect.h, core.drawAreaBy + 1);

    // If we are in flat colored mode then decide the foreground color for every pixel in the rectangle
    const Color24F rectColor = rect.color;
    Color16 fgColor;

    if constexpr ((DrawMode == DrawMode::Colored) || (DrawMode == DrawMode::ColoredBlended)) {
        fgColor = color24FTo16<DrawMode>(rectColor);
    }

    // Fill in the rectangle pixels
    const bool bEnableMasking = (!core.bDisableMasking);
    uint16_t curV = topLeftV;

    // PsyDoom 3DS: fast path for the case that covers the sky, status bar, HUD and menus - an unblended 8bpp blit.
    // The generic loop below recomputes the source and destination row addresses for every single pixel; here they
    // are hoisted out of the inner loop.
    //
    // Anything drawn with 'SetShadeTex' - the sky and every status bar and menu sprite - comes through with a colour of
    // 128,128,128, which 'colorMul' leaves completely unchanged. Detecting that up front and skipping the modulate
    // removes six multiplies, shifts and clamps from every one of those pixels. The sky alone is a full 256 pixel wide
    // sprite drawn under the whole world, so this is the single most drawn primitive in the frame.
    #if PSYDOOM_3DS
        if constexpr ((DrawMode == DrawMode::Textured) && (TexFmt == TexFmt::Bpp8)) {
            const uint16_t vramPixelW = core.ramPixelW;
            const uint16_t vramXMask = core.ramXMask;
            const uint16_t vramYMask = core.ramYMask;
            uint16_t* const pVram = core.pRam;
            const Color16* const pClut = core.clutCache;

            const bool bNeutralColor = (
                (rectColor.comp.r == 128) &&
                (rectColor.comp.g == 128) &&
                (rectColor.comp.b == 128)
            );

            for (int16_t y = begY; y < endY; ++y, ++curV) {
                // Where this row of the rectangle reads from and writes to
                uint16_t texVramY = curV & core.texWinYMask;
                texVramY += core.texWinY;
                texVramY &= core.texPageYMask;
                texVramY += core.texPageY;

                const uint16_t* const pTexRow = pVram + (uint32_t)(texVramY & vramYMask) * vramPixelW;
                uint16_t* const pDstRow = pVram + (uint32_t)(y & vramYMask) * vramPixelW;
                uint16_t curU = topLeftU;

                for (int16_t x = begX; x < endX; ++x, ++curU) {
                    uint16_t texVramX = curU & core.texWinXMask;
                    texVramX += core.texWinX;
                    texVramX /= 2;
                    texVramX &= core.texPageXMask;
                    texVramX += core.texPageX;

                    const uint16_t texPixel = pTexRow[texVramX & vramXMask];
                    const Color16 texel = pClut[(texPixel >> ((curU & 1) * 8)) & 0xFF];

                    if ((texel.bits == 0) && bEnableMasking)
                        continue;

                    pDstRow[x & vramXMask] = (bNeutralColor) ? texel : colorMul(texel, rectColor);
                }
            }

            return;
        }
    #endif

    for (int16_t y = begY; y < endY; ++y, ++curV) {
        uint16_t curU = topLeftU;

        for (int16_t x = begX; x < endX; ++x, ++curU) {
            // Get the foreground color for the rectangle pixel if the rectangle is textured.
            // If the pixel is transparent and masking is enabled then also skip it, otherwise modulate it by the primitive color...
            if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
                fgColor = readTexel<TexFmt>(core, curU, curV);

                if ((fgColor.bits == 0) && bEnableMasking)
                    continue;

                fgColor = colorMul(fgColor, rectColor);
            }

            // Do blending with the background if that is enabled
            if constexpr ((DrawMode == DrawMode::ColoredBlended) || (DrawMode == DrawMode::TexturedBlended)) {
                const Color16 bgColor = vramReadU16(core, x, y);
                fgColor = colorBlend(bgColor, fgColor, core.blendMode);
            }

            // Save the output pixel
            vramWriteU16(core, x, y, fgColor);
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a rectangle - external interface
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
void draw(Core& core, const DrawRect& rect) noexcept {
    if (core.texFmt == TexFmt::Bpp4) {
        draw<DrawMode, TexFmt::Bpp4>(core, rect);
    } else if (core.texFmt == TexFmt::Bpp8) {
        draw<DrawMode, TexFmt::Bpp8>(core, rect);
    } else {
        ASSERT(core.texFmt == TexFmt::Bpp16);
        draw<DrawMode, TexFmt::Bpp16>(core, rect);
    }
}

// Instantiate the variants of this function
template void draw<DrawMode::Colored>(Core& core, const DrawRect& rect) noexcept;
template void draw<DrawMode::ColoredBlended>(Core& core, const DrawRect& rect) noexcept;
template void draw<DrawMode::Textured>(Core& core, const DrawRect& rect) noexcept;
template void draw<DrawMode::TexturedBlended>(Core& core, const DrawRect& rect) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a line
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
void draw(Core& core, const DrawLine& line) noexcept {
    sanityCheckGpuDrawState(core);

    // Translate the line by the drawing offset
    const int32_t lineX1 = line.x1 + core.drawOffsetX;
    const int32_t lineY1 = line.y1 + core.drawOffsetY;
    const int32_t lineX2 = line.x2 + core.drawOffsetX;
    const int32_t lineY2 = line.y2 + core.drawOffsetY;

    // Firstly get the component deltas
    const int32_t dx = (int32_t) lineX2 - lineX1;
    const int32_t dy = (int32_t) lineY2 - lineY1;
    const int32_t absDx = std::abs(dx);
    const int32_t absDy = std::abs(dy);

    // According to the NO$PSX specs line distances cannot exceed 1023 x 511
    if ((absDx >= 1024) || (absDy >= 512))
        return;

    // Get the color to shade the line with
    const Color16 lineColor = color24FTo16<DrawMode>(line.color);

    // This is Bresenham's line drawing algorithm. In order to handle all 4 cases of line we swap components as required so that the
    // delta error increment is on the dimension with the smallest change in the line. Also we insure the primary dimension with the
    // largest increment always steps in the positive direction:
    bool bLineIsSteep;

    int32_t a1;
    int32_t a2;
    int32_t b1;
    int32_t b2;

    if (absDy > absDx) {
        bLineIsSteep = true;
        a1 = lineY1;
        a2 = lineY2;
        b1 = lineX1;
        b2 = lineX2;
    } else {
        bLineIsSteep = false;
        a1 = lineX1;
        a2 = lineX2;
        b1 = lineY1;
        b2 = lineY2;
    }

    if (a1 > a2) {
        std::swap(a1, a2);
        std::swap(b1, b2);
    }

    // Compute the deltas and delta error per pixel as per Bresenham's algorithm
    const int32_t da = a2 - a1;
    const int32_t db = b2 - b1;
    const int32_t derror = std::abs(db) * 2;
    const int32_t bstep = (db < 0) ? -1 : +1;

    int32_t error = 0;
    int32_t b = b1;

    // Plot pixels: this loop could be optimized more and clipping could be employed but Doom doesn't render lines too much.
    // It's probably not worth the effort going crazy on this...
    constexpr bool bBlend = (DrawMode == DrawMode::ColoredBlended);
    const BlendMode blendMode = core.blendMode;

    for (int32_t a = a1; a <= a2; ++a) {
        const uint16_t x = (uint16_t)((bLineIsSteep) ? b : a);
        const uint16_t y = (uint16_t)((bLineIsSteep) ? a : b);

        if (isPixelInDrawArea(core, x, y)) {
            const Color16 color = (bBlend) ? colorBlend(vramReadU16(core, x, y), lineColor, blendMode) : lineColor;
            vramWriteU16(core, x, y, color);
        }

        // Time to step the minor change dimension according to Bresenham's algorithm?
        error += derror;

        if (error > da) {
            b += bstep;
            error -= da * 2;
        }
    }
}

// Instantiate the variants of this function
template void draw<DrawMode::Colored>(Core& core, const DrawLine& line) noexcept;
template void draw<DrawMode::ColoredBlended>(Core& core, const DrawLine& line) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a triangle - internal implementation tailored to each texture format.
// Sources for the general technique and optimizations:
//  https://www.scratchapixel.com/lessons/3d-basic-rendering/rasterization-practical-implementation/rasterization-stage
//  https://fgiesen.wordpress.com/2013/02/06/the-barycentric-conspirac/
//  https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
//  https://fgiesen.wordpress.com/2013/02/10/optimizing-the-basic-rasterizer/
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode, TexFmt TexFmt>
static void draw(Core& core, const DrawTriangle& triangle) noexcept {
    sanityCheckGpuDrawState(core);

    // Apply the draw offset to the triangle points
    const int16_t drawOffsetX = core.drawOffsetX;
    const int16_t drawOffsetY = core.drawOffsetY;
    const int32_t p1x = triangle.x1 + drawOffsetX;
    const int32_t p1y = triangle.y1 + drawOffsetY;
    const int32_t p2x = triangle.x2 + drawOffsetX;
    const int32_t p2y = triangle.y2 + drawOffsetY;
    const int32_t p3x = triangle.x3 + drawOffsetX;
    const int32_t p3y = triangle.y3 + drawOffsetY;

    // Compute the rectangular area of the triangle to be rasterized.
    // Note that according to the No$PSX specs the right and bottom coordinates in polygons are NOT included.
    // Also, the triangle is skipped if the distances between the vertices exceed 1023x511 in the x and y dimensions.
    const int32_t minX = std::min(std::min(p1x, p2x), p3x);
    const int32_t minY = std::min(std::min(p1y, p2y), p3y);
    const int32_t maxX = std::max(std::max(p1x, p2x), p3x);
    const int32_t maxY = std::max(std::max(p1y, p2y), p3y);
    const int32_t xrange = maxX - minX;
    const int32_t yrange = maxY - minY;
    const int32_t lx = std::max((int32_t) core.drawAreaLx, minX);
    const int32_t rx = std::min((int32_t) core.drawAreaRx, maxX - 1);
    const int32_t ty = std::max((int32_t) core.drawAreaTy, minY);
    const int32_t by = std::min((int32_t) core.drawAreaBy, maxY - 1);

    if ((xrange >= 1024) || (yrange >= 512))
        return;

    // If we're going to draw textured and with a CLUT make sure it is up to date
    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        if constexpr ((TexFmt == TexFmt::Bpp4) || (TexFmt == TexFmt::Bpp8)) {
            updateClutCache(core);
        }
    }

    // Precompute the edge deltas used in the edge functions
    const float p1xf = (float) p1x;     const float p1yf = (float) p1y;
    const float p2xf = (float) p2x;     const float p2yf = (float) p2y;
    const float p3xf = (float) p3x;     const float p3yf = (float) p3y;

    const float e1dx = p2xf - p1xf;
    const float e1dy = p2yf - p1yf;
    const float e2dx = p3xf - p2xf;
    const float e2dy = p3yf - p2yf;
    const float e3dx = p1xf - p3xf;
    const float e3dy = p1yf - p3yf;

    // If we are in flat colored mode then decide the foreground color for every pixel in the triangle
    const Color24F triangleColor = triangle.color;
    Color16 fgColor;

    if constexpr ((DrawMode == DrawMode::Colored) || (DrawMode == DrawMode::ColoredBlended)) {
        fgColor = color24FTo16<DrawMode>(triangleColor);
    }

    // Cache the uv coords
    const float u1 = triangle.u1;    const float v1 = triangle.v1;
    const float u2 = triangle.u2;    const float v2 = triangle.v2;
    const float u3 = triangle.u3;    const float v3 = triangle.v3;

    // Compute the 'edge function' or the magnitude of the cross product between an edge and a vector.
    // We start by computing the edge function for the top left point checked by the rasterizer and step from there.
    // This function for all 3 edges tells us whether a point is inside the triangle, and also lets us compute barycentric coordinates.
    float row_ef1 = ((float) lx - p1xf) * e1dy - ((float) ty - p1yf) * e1dx;
    float row_ef2 = ((float) lx - p2xf) * e2dy - ((float) ty - p2yf) * e2dx;
    float row_ef3 = ((float) lx - p3xf) * e3dy - ((float) ty - p3yf) * e3dx;

    // Compute the bias for each edge function to use when texture mapping; sample as if we were 0.5x pixels in.
    // Note: have to go in the opposite direction for 'y' since the top row of the screen is at y = 0.
    const float ef1_bias = (e1dy - e1dx) * 0.5f;
    const float ef2_bias = (e2dy - e2dx) * 0.5f;
    const float ef3_bias = (e3dy - e3dx) * 0.5f;

    // Compute the total signed triangle area (x6) and from that a multiplier to normalize the barycentric vertex weights
    const float triArea = row_ef1 + row_ef2 + row_ef3;
    const float weightNormalize = 1.0f / triArea;

    // PsyDoom 3DS: the barycentric weights, and therefore the texture coordinates, are affine in 'x'.
    // Stepping them along the row replaces six multiplies and four adds per pixel with two adds, which matters a lot
    // on the ARM11's VFP unit. Each row still starts from an exactly computed value so error cannot build up.
    #if PSYDOOM_3DS
        const float dw1dx = e2dy * weightNormalize;
        const float dw2dx = e3dy * weightNormalize;
        const float dw3dx = e1dy * weightNormalize;
        const float dudx = u1 * dw1dx + u2 * dw2dx + u3 * dw3dx;
        const float dvdx = v1 * dw1dx + v2 * dw2dx + v3 * dw3dx;
    #endif

    // Process each pixel in the rectangular region being rasterized
    uint16_t* pDstPixelRow = core.pRam + ty * core.ramPixelW;
    const bool bEnableMasking = (!core.bDisableMasking);

    for (int32_t y = ty; y <= by; ++y, pDstPixelRow += core.ramPixelW) {
        // The edge function for the current column starts off as the edge function for the row
        float col_ef1 = row_ef1;
        float col_ef2 = row_ef2;
        float col_ef3 = row_ef3;

        #if PSYDOOM_3DS
            const float rowW1 = (row_ef2 + ef2_bias) * weightNormalize;
            const float rowW2 = (row_ef3 + ef3_bias) * weightNormalize;
            const float rowW3 = (row_ef1 + ef1_bias) * weightNormalize;

            // Note: the small nudges are folded in here; they account for float imprecision and prevent 'fuzzyness'
            float uf = u1 * rowW1 + u2 * rowW2 + u3 * rowW3 + 1.0f / 8192.0f;
            float vf = v1 * rowW1 + v2 * rowW2 + v3 * rowW3 - 1.0f / 8192.0f;
        #endif

        for (int32_t x = lx; x <= rx; ++x) {
            // Get the sign of each edge function
            const bool bSign1 = (col_ef1 <= 0);
            const bool bSign2 = (col_ef2 <= 0);
            const bool bSign3 = (col_ef3 <= 0);

            #if PSYDOOM_3DS
                const float curU = uf;
                const float curV = vf;
                uf += dudx;
                vf += dvdx;
            #else
                // Compute the vertex weights
                const float w1 = (col_ef2 + ef2_bias) * weightNormalize;
                const float w2 = (col_ef3 + ef3_bias) * weightNormalize;
                const float w3 = (col_ef1 + ef1_bias) * weightNormalize;
            #endif

            // Step the edge function to the next column
            col_ef1 += e1dy;
            col_ef2 += e2dy;
            col_ef3 += e3dy;

            // The point is inside the triangle if the sign of all edge functions matches.
            // This handles triangles that are wound the opposite way.
            if ((bSign1 != bSign2) || (bSign2 != bSign3))
                continue;

            // Compute the texture coordinate to use and nudge slightly if it's close to the next integer coord (to account for float inprecision and prevent 'fuzzyness')
            #if PSYDOOM_3DS
                [[maybe_unused]] const uint16_t u = (uint16_t) curU;
                [[maybe_unused]] const uint16_t v = (uint16_t) curV;
            #else
                [[maybe_unused]] const uint16_t u = (uint16_t)(u1 * w1 + u2 * w2 + u3 * w3 + 1.0f / 8192.0f);
                [[maybe_unused]] const uint16_t v = (uint16_t)(v1 * w1 + v2 * w2 + v3 * w3 - 1.0f / 8192.0f);
            #endif

            // Get the foreground color for the triangle pixel if the triangle is textured.
            // If the pixel is transparent and masking is enabled then also skip it, otherwise modulate it by the primitive color...
            if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
                fgColor = readTexel<TexFmt>(core, u, v);

                if ((fgColor.bits == 0) && bEnableMasking)
                    continue;

                fgColor = colorMul(fgColor, triangleColor);
            }

            // Do blending with the background if that is enabled
            if constexpr ((DrawMode == DrawMode::ColoredBlended) || (DrawMode == DrawMode::TexturedBlended)) {
                const Color16 bgColor = pDstPixelRow[x];
                fgColor = colorBlend(bgColor, fgColor, core.blendMode);
            }

            // Save the output pixel
            pDstPixelRow[x] = fgColor;
        }

        // Step the edge function onto the next row
        row_ef1 -= e1dx;
        row_ef2 -= e2dx;
        row_ef3 -= e3dx;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a triangle - external interface
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
void draw(Core& core, const DrawTriangle& triangle) noexcept {
    if (core.texFmt == TexFmt::Bpp4) {
        draw<DrawMode, TexFmt::Bpp4>(core, triangle);
    } else if (core.texFmt == TexFmt::Bpp8) {
        draw<DrawMode, TexFmt::Bpp8>(core, triangle);
    } else {
        ASSERT(core.texFmt == TexFmt::Bpp16);
        draw<DrawMode, TexFmt::Bpp16>(core, triangle);
    }
}

// Instantiate the variants of this function
template void draw<DrawMode::Colored>(Core& core, const DrawTriangle& triangle) noexcept;
template void draw<DrawMode::ColoredBlended>(Core& core, const DrawTriangle& triangle) noexcept;
template void draw<DrawMode::Textured>(Core& core, const DrawTriangle& triangle) noexcept;
template void draw<DrawMode::TexturedBlended>(Core& core, const DrawTriangle& triangle) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a gouraud shaded triangle - internal implementation tailored to each texture format.
// This is largely copied from the non-gouraud shaded triangle drawing function.
// 
// Sources for the general technique and optimizations:
//  https://www.scratchapixel.com/lessons/3d-basic-rendering/rasterization-practical-implementation/rasterization-stage
//  https://fgiesen.wordpress.com/2013/02/06/the-barycentric-conspirac/
//  https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
//  https://fgiesen.wordpress.com/2013/02/10/optimizing-the-basic-rasterizer/
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode, TexFmt TexFmt>
static void draw(Core& core, const DrawTriangleGouraud& triangle) noexcept {
    sanityCheckGpuDrawState(core);

    // Apply the draw offset to the triangle points
    const int16_t drawOffsetX = core.drawOffsetX;
    const int16_t drawOffsetY = core.drawOffsetY;
    const int32_t p1x = triangle.x1 + drawOffsetX;
    const int32_t p1y = triangle.y1 + drawOffsetY;
    const int32_t p2x = triangle.x2 + drawOffsetX;
    const int32_t p2y = triangle.y2 + drawOffsetY;
    const int32_t p3x = triangle.x3 + drawOffsetX;
    const int32_t p3y = triangle.y3 + drawOffsetY;

    // Compute the rectangular area of the triangle to be rasterized.
    // Note that according to the No$PSX specs the right and bottom coordinates in polygons are NOT included.
    // Also, the triangle is skipped if the distances between the vertices exceed 1023x511 in the x and y dimensions.
    const int32_t minX = std::min(std::min(p1x, p2x), p3x);
    const int32_t minY = std::min(std::min(p1y, p2y), p3y);
    const int32_t maxX = std::max(std::max(p1x, p2x), p3x);
    const int32_t maxY = std::max(std::max(p1y, p2y), p3y);
    const int32_t xrange = maxX - minX;
    const int32_t yrange = maxY - minY;
    const int32_t lx = std::max((int32_t) core.drawAreaLx, minX);
    const int32_t rx = std::min((int32_t) core.drawAreaRx, maxX - 1);
    const int32_t ty = std::max((int32_t) core.drawAreaTy, minY);
    const int32_t by = std::min((int32_t) core.drawAreaBy, maxY - 1);

    if ((xrange >= 1024) || (yrange >= 512))
        return;

    // If we're going to draw textured and with a CLUT make sure it is up to date
    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        if constexpr ((TexFmt == TexFmt::Bpp4) || (TexFmt == TexFmt::Bpp8)) {
            updateClutCache(core);
        }
    }

    // Precompute the edge deltas used in the edge functions
    const float p1xf = (float) p1x;     const float p1yf = (float) p1y;
    const float p2xf = (float) p2x;     const float p2yf = (float) p2y;
    const float p3xf = (float) p3x;     const float p3yf = (float) p3y;

    const float e1dx = p2xf - p1xf;
    const float e1dy = p2yf - p1yf;
    const float e2dx = p3xf - p2xf;
    const float e2dy = p3yf - p2yf;
    const float e3dx = p1xf - p3xf;
    const float e3dy = p1yf - p3yf;

    // Get the color for all 3 triangle points in floating point format
    const float color1R = triangle.color1.comp.r;
    const float color1G = triangle.color1.comp.g;
    const float color1B = triangle.color1.comp.b;
    const float color2R = triangle.color2.comp.r;
    const float color2G = triangle.color2.comp.g;
    const float color2B = triangle.color2.comp.b;
    const float color3R = triangle.color3.comp.r;
    const float color3G = triangle.color3.comp.g;
    const float color3B = triangle.color3.comp.b;

    // Cache the uv coords
    const float u1 = triangle.u1;    const float v1 = triangle.v1;
    const float u2 = triangle.u2;    const float v2 = triangle.v2;
    const float u3 = triangle.u3;    const float v3 = triangle.v3;

    // Compute the 'edge function' or the magnitude of the cross product between an edge and a vector.
    // We start by computing the edge function for the top left point checked by the rasterizer and step from there.
    // This function for all 3 edges tells us whether a point is inside the triangle, and also lets us compute barycentric coordinates.
    float row_ef1 = ((float) lx - p1xf) * e1dy - ((float) ty - p1yf) * e1dx;
    float row_ef2 = ((float) lx - p2xf) * e2dy - ((float) ty - p2yf) * e2dx;
    float row_ef3 = ((float) lx - p3xf) * e3dy - ((float) ty - p3yf) * e3dx;

    // Compute the bias for each edge function to use when texture mapping; sample as if we were 0.5x pixels in.
    // Note: have to go in the opposite direction for 'y' since the top row of the screen is at y = 0.
    const float ef1_bias = (e1dy - e1dx) * 0.5f;
    const float ef2_bias = (e2dy - e2dx) * 0.5f;
    const float ef3_bias = (e3dy - e3dx) * 0.5f;

    // Compute the total signed triangle area (x6) and from that a multiplier to normalize the barycentric vertex weights
    const float triArea = row_ef1 + row_ef2 + row_ef3;
    const float weightNormalize = 1.0f / triArea;

    // PsyDoom 3DS: the texture coordinates and the Gouraud color are all affine in 'x', so step them along the row
    // rather than recomputing fifteen multiplies per pixel. Each row starts from an exactly computed value.
    #if PSYDOOM_3DS
        const float dw1dx = e2dy * weightNormalize;
        const float dw2dx = e3dy * weightNormalize;
        const float dw3dx = e1dy * weightNormalize;
        const float dudx = u1 * dw1dx + u2 * dw2dx + u3 * dw3dx;
        const float dvdx = v1 * dw1dx + v2 * dw2dx + v3 * dw3dx;
        const float drdx = color1R * dw1dx + color2R * dw2dx + color3R * dw3dx;
        const float dgdx = color1G * dw1dx + color2G * dw2dx + color3G * dw3dx;
        const float dbdx = color1B * dw1dx + color2B * dw2dx + color3B * dw3dx;
    #endif

    // Process each pixel in the rectangular region being rasterized
    uint16_t* pDstPixelRow = core.pRam + ty * core.ramPixelW;
    const bool bEnableMasking = (!core.bDisableMasking);

    for (int32_t y = ty; y <= by; ++y, pDstPixelRow += core.ramPixelW) {
        // The edge function for the current column starts off as the edge function for the row
        float col_ef1 = row_ef1;
        float col_ef2 = row_ef2;
        float col_ef3 = row_ef3;

        #if PSYDOOM_3DS
            const float rowW1 = (row_ef2 + ef2_bias) * weightNormalize;
            const float rowW2 = (row_ef3 + ef3_bias) * weightNormalize;
            const float rowW3 = (row_ef1 + ef1_bias) * weightNormalize;

            float uf = u1 * rowW1 + u2 * rowW2 + u3 * rowW3 + 1.0f / 8192.0f;
            float vf = v1 * rowW1 + v2 * rowW2 + v3 * rowW3 - 1.0f / 8192.0f;
            float rf = color1R * rowW1 + color2R * rowW2 + color3R * rowW3 + 0.5f;
            float gf = color1G * rowW1 + color2G * rowW2 + color3G * rowW3 + 0.5f;
            float bf = color1B * rowW1 + color2B * rowW2 + color3B * rowW3 + 0.5f;
        #endif

        for (int32_t x = lx; x <= rx; ++x) {
            // Get the sign of each edge function
            const bool bSign1 = (col_ef1 <= 0);
            const bool bSign2 = (col_ef2 <= 0);
            const bool bSign3 = (col_ef3 <= 0);

            #if PSYDOOM_3DS
                const float curU = uf;
                const float curV = vf;
                const float curR = rf;
                const float curG = gf;
                const float curB = bf;
                uf += dudx;
                vf += dvdx;
                rf += drdx;
                gf += dgdx;
                bf += dbdx;
            #else
                // Compute the vertex weights
                const float w1 = (col_ef2 + ef2_bias) * weightNormalize;
                const float w2 = (col_ef3 + ef3_bias) * weightNormalize;
                const float w3 = (col_ef1 + ef1_bias) * weightNormalize;
            #endif

            // Step the edge function to the next column
            col_ef1 += e1dy;
            col_ef2 += e2dy;
            col_ef3 += e3dy;

            // The point is inside the triangle if the sign of all edge functions matches.
            // This handles triangles that are wound the opposite way.
            if ((bSign1 != bSign2) || (bSign2 != bSign3))
                continue;

            // Compute the texture coordinate to use and nudge slightly if it's close to the next integer coord (to account for float inprecision and prevent 'fuzzyness')
            // Also compute the triangle gouraud color at this pixel using the weights.
            #if PSYDOOM_3DS
                [[maybe_unused]] const uint16_t u = (uint16_t) curU;
                [[maybe_unused]] const uint16_t v = (uint16_t) curV;
                const uint8_t gColorR = (uint8_t) std::clamp(curR, 0.0f, 255.0f);
                const uint8_t gColorG = (uint8_t) std::clamp(curG, 0.0f, 255.0f);
                const uint8_t gColorB = (uint8_t) std::clamp(curB, 0.0f, 255.0f);
            #else
                [[maybe_unused]] const uint16_t u = (uint16_t)(u1 * w1 + u2 * w2 + u3 * w3 + 1.0f / 8192.0f);
                [[maybe_unused]] const uint16_t v = (uint16_t)(v1 * w1 + v2 * w2 + v3 * w3 - 1.0f / 8192.0f);

                const uint8_t gColorR = (uint8_t) std::clamp(color1R * w1 + color2R * w2 + color3R * w3 + 0.5f, 0.0f, 255.0f);
                const uint8_t gColorG = (uint8_t) std::clamp(color1G * w1 + color2G * w2 + color3G * w3 + 0.5f, 0.0f, 255.0f);
                const uint8_t gColorB = (uint8_t) std::clamp(color1B * w1 + color2B * w2 + color3B * w3 + 0.5f, 0.0f, 255.0f);
            #endif

            // Figure out the foreground color for the pixel for the current draw mode.
            // If the pixel is transparent and masking is enabled then also skip it, otherwise modulate it by the primitive color...
            Color16 fgColor;

            if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
                // Doing texture mapping in addition to gouraud shading
                fgColor = readTexel<TexFmt>(core, u, v);

                if ((fgColor.bits == 0) && bEnableMasking)
                    continue;

                const Color24F triangleColor = Color24F{ gColorR, gColorG, gColorB };
                fgColor = colorMul(fgColor, triangleColor);
            }
            else {
                // Not doing texture mapping: foreground color is just the interpolated color
                fgColor = Color16::make(
                    std::min<uint16_t>(((uint16_t) gColorR + 4) >> 3, 31u),
                    std::min<uint16_t>(((uint16_t) gColorG + 4) >> 3, 31u),
                    std::min<uint16_t>(((uint16_t) gColorB + 4) >> 3, 31u)
                );
            }

            // Do blending with the background if that is enabled
            if constexpr ((DrawMode == DrawMode::ColoredBlended) || (DrawMode == DrawMode::TexturedBlended)) {
                const Color16 bgColor = pDstPixelRow[x];
                fgColor = colorBlend(bgColor, fgColor, core.blendMode);
            }

            // Save the output pixel
            pDstPixelRow[x] = fgColor;
        }

        // Step the edge function onto the next row
        row_ef1 -= e1dx;
        row_ef2 -= e2dx;
        row_ef3 -= e3dx;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a gouraud shaded triangle - external interface
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
void draw(Core& core, const DrawTriangleGouraud& triangle) noexcept {
    if (core.texFmt == TexFmt::Bpp4) {
        draw<DrawMode, TexFmt::Bpp4>(core, triangle);
    } else if (core.texFmt == TexFmt::Bpp8) {
        draw<DrawMode, TexFmt::Bpp8>(core, triangle);
    } else {
        ASSERT(core.texFmt == TexFmt::Bpp16);
        draw<DrawMode, TexFmt::Bpp16>(core, triangle);
    }
}

// Instantiate the variants of this function
template void draw<DrawMode::Colored>(Core& core, const DrawTriangleGouraud& triangle) noexcept;
template void draw<DrawMode::ColoredBlended>(Core& core, const DrawTriangleGouraud& triangle) noexcept;
template void draw<DrawMode::Textured>(Core& core, const DrawTriangleGouraud& triangle) noexcept;
template void draw<DrawMode::TexturedBlended>(Core& core, const DrawTriangleGouraud& triangle) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws a single row of Doom floor pixels; texture format is assumed to be 8bpp.
// This is a new primitive added to help accelerate the classic renderer for PsyDoom.
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
void draw(Core& core, const DrawFloorRow& row) noexcept {
    sanityCheckGpuDrawState(core);

    // Apply the draw offset to the row coordinates
    const int16_t drawOffsetX = core.drawOffsetX;
    const int16_t drawOffsetY = core.drawOffsetY;
    const int32_t p1x = row.x1 + drawOffsetX;
    const int32_t p2x = row.x2 + drawOffsetX;
    const int32_t py = row.y + drawOffsetY;

    // Get the minimum and maximum values of the row, and the swap the uvs if required
    float u1, u2, v1, v2;
    int32_t minX, maxX;

    if (p1x < p2x) {
        u1 = row.u1;    u2 = row.u2;
        v1 = row.v1;    v2 = row.v2;
        minX = p1x;     maxX = p2x;
    } else {
        u1 = row.u2;    u2 = row.u1;
        v1 = row.v2;    v2 = row.v1;
        minX = p2x;     maxX = p1x;
    }

    // Compute how much of the row will be rasterized; similar to triangles the last column is NOT drawn
    const int32_t xrange = maxX - minX;
    const int32_t lx = std::max((int32_t) core.drawAreaLx, minX);
    const int32_t rx = std::min((int32_t) core.drawAreaRx, maxX - 1);

    // Compute the step in 't', the percentage along the line (used for interpolation of the uvs)
    const float tStep = 1.0f / (float) xrange;

    // Also similar to triangles, skip the row if the distances between the vertices exceed 1023 on the x dimension.
    // Also skip if the row itself is outside the draw area.
    if ((xrange >= 1024) || (py < core.drawAreaTy) || (py > core.drawAreaBy))
        return;

    // If we're going to draw textured and with a CLUT make sure it is up to date
    #if PSYDOOM_3DS
        bool bUseTexAddrCache = false;
    #endif

    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        updateClutCache(core);

        // PsyDoom 3DS: a floor span moves through the texture on both axes, so it pays the full address computation
        // twice per pixel. Tabulating it turns a dozen operations into two lookups.
        #if PSYDOOM_3DS
            bUseTexAddrCache = updateTexAddrCache(core);
        #endif
    }

    // If we are in flat colored mode then decide the foreground color for every pixel in the row
    const Color24F rowColor = row.color;
    Color16 fgColor;

    if constexpr ((DrawMode == DrawMode::Colored) || (DrawMode == DrawMode::ColoredBlended)) {
        fgColor = color24FTo16<DrawMode>(rowColor);
    }

    // Cache some GPU RAM related params
    const uint16_t vramPixelW = core.ramPixelW;
    const uint16_t vramXMask = core.ramXMask;
    const uint16_t vramYMask = core.ramYMask;
    uint16_t* const pVram = core.pRam;

    // Process each pixel in the line being rasterized.
    // PsyDoom 3DS: interpolate the uvs in 16.16 fixed point instead of float; VFP math is expensive on the ARM11.
    // Also allow columns to be skipped and replicated, to trade detail for speed.
    #if PSYDOOM_3DS
        static constexpr float UV_FIXED_SCALE = 65536.0f;
        const int32_t stepX = gPixelStepX;
        const float startT = (0.5f + (float) lx - minX) * tStep;
        const int32_t uStepFixed = (int32_t)((u2 - u1) * tStep * UV_FIXED_SCALE);
        const int32_t vStepFixed = (int32_t)((v2 - v1) * tStep * UV_FIXED_SCALE);
        int32_t uFixed = (int32_t)((u1 + (u2 - u1) * startT) * UV_FIXED_SCALE);
        int32_t vFixed = (int32_t)((v1 + (v2 - v1) * startT) * UV_FIXED_SCALE);
        const int32_t uStridedStep = uStepFixed * stepX;
        const int32_t vStridedStep = vStepFixed * stepX;
    #else
        float t = (0.5f + (float) lx - minX) * tStep;
        float tinv = 1.0f - t;
    #endif

    uint16_t* pDstPixelRow = pVram + py * vramPixelW;
    const bool bEnableMasking = (!core.bDisableMasking);

#if PSYDOOM_3DS
    for (int32_t x = lx; x <= rx; x += stepX) {
#else
    for (int32_t x = lx; x <= rx; ++x) {
#endif
        // Compute the texture coordinate to use
        #if PSYDOOM_3DS
            const uint16_t u = (uint16_t)(uFixed >> 16);
            const uint16_t v = (uint16_t)(vFixed >> 16);
            uFixed += uStridedStep;
            vFixed += vStridedStep;
        #else
            const uint16_t u = (uint16_t)(u1 * tinv + u2 * t);
            const uint16_t v = (uint16_t)(v1 * tinv + v2 * t);
            t += tStep;
            tinv -= tStep;
        #endif

        // Get the foreground color for the row pixel if the row is textured.
        // If the pixel is transparent and masking is enabled then also skip it, otherwise modulate it by the primitive color...
        if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
            uint16_t vramPixel;
            uint16_t clutIdx;

            #if PSYDOOM_3DS
            if (bUseTexAddrCache) {
                const uint32_t maskedU = u & core.texWinXMask;
                const uint32_t maskedV = v & core.texWinYMask;

                vramPixel = pVram[core.texAddrCacheV[maskedV] + core.texAddrCacheU[maskedU]];
                clutIdx = (vramPixel >> core.texAddrCacheUShift[maskedU]) & 0xFF;
            } else
            #endif
            {
                // Figure out the VRAM coordinates to read the VRAM pixel from
                uint16_t vramX = u & core.texWinXMask;
                uint16_t vramY = v & core.texWinYMask;
                vramX += core.texWinX;
                vramY += core.texWinY;
                vramX /= 2;
                vramX &= core.texPageXMask;
                vramY &= core.texPageYMask;
                vramX += core.texPageX;
                vramY += core.texPageY;

                // Read the VRAM pixel and lookup the actual texel using the clut index
                vramPixel = pVram[(vramY & vramYMask) * vramPixelW + (vramX & vramXMask)];
                clutIdx = (vramPixel >> ((u & 1) * 8)) & 0xFF;
            }

            fgColor = core.clutCache[clutIdx];

            if ((fgColor.bits == 0) && bEnableMasking)
                continue;

            fgColor = colorMul(fgColor, rowColor);
        }

        // Do blending with the background if that is enabled
        uint16_t& dstPixel = pDstPixelRow[x];

        if constexpr ((DrawMode == DrawMode::ColoredBlended) || (DrawMode == DrawMode::TexturedBlended)) {
            const Color16 bgColor = dstPixel;
            fgColor = colorBlend(bgColor, fgColor, core.blendMode);
        }

        // Save the output pixel and step to the next pixel
        dstPixel = fgColor;

        // PsyDoom 3DS: replicate over the columns that were skipped
        #if PSYDOOM_3DS
            for (int32_t repX = x + 1; (repX < x + stepX) && (repX <= rx); ++repX) {
                pDstPixelRow[repX] = fgColor;
            }
        #endif
    }
}

// Instantiate the variants of this function
template void draw<DrawMode::Colored>(Core& core, const DrawFloorRow& row) noexcept;
template void draw<DrawMode::ColoredBlended>(Core& core, const DrawFloorRow& row) noexcept;
template void draw<DrawMode::Textured>(Core& core, const DrawFloorRow& row) noexcept;
template void draw<DrawMode::TexturedBlended>(Core& core, const DrawFloorRow& row) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws a single column of Doom wall pixels; texture format is assumed to be 8bpp.
// This is a new primitive added to help accelerate the classic renderer for PsyDoom.
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
void draw(Core& core, const DrawWallCol& col) noexcept {
    sanityCheckGpuDrawState(core);

    // Apply the draw offset to the column coordinates
    const int16_t drawOffsetX = core.drawOffsetX;
    const int16_t drawOffsetY = core.drawOffsetY;
    const int32_t px = col.x + drawOffsetX;
    const int32_t p1y = col.y1 + drawOffsetY;
    const int32_t p2y = col.y2 + drawOffsetY;

    // Get the minimum and maximum values of the column, and the swap the uvs if required
    float v1, v2;
    int32_t minY, maxY;

    if (p1y < p2y) {
        v1 = col.v1;    v2 = col.v2;
        minY = p1y;     maxY = p2y;
    } else {
        v1 = col.v2;    v2 = col.v1;
        minY = p2y;     maxY = p1y;
    }

    // Compute how much of the column will be rasterized; similar to triangles the last row is NOT drawn
    const int32_t yrange = maxY - minY;
    const int32_t ty = std::max((int32_t) core.drawAreaTy, minY);
    const int32_t by = std::min((int32_t) core.drawAreaBy, maxY - 1);

    // Compute the step in 't', the percentage along the line (used for interpolation of the uvs)
    const float tStep = 1.0f / (float) yrange;

    // Also similar to triangles, skip the column if the distances between the vertices exceed 511 on the y dimension.
    // Also skip if the column itself is outside the draw area.
    if ((yrange >= 512) || (px < core.drawAreaLx) || (px > core.drawAreaRx))
        return;

    // PsyDoom 3DS: in low detail mode the renderer only submits every Nth column, so widen each one to cover the
    // columns that were skipped and shade only every Nth row.
    #if PSYDOOM_3DS
        const int32_t stepX = gPixelStepX;
        const int32_t stepY = gPixelStepY;
        const int32_t repEndX = std::min<int32_t>(px + stepX - 1, core.drawAreaRx);
    #endif

    // If we're going to draw textured and with a CLUT make sure it is up to date
    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        updateClutCache(core);
    }

    // If we are in flat colored mode then decide the foreground color for every pixel in the column
    const Color24F colColor = col.color;
    Color16 fgColor;

    if constexpr ((DrawMode == DrawMode::Colored) || (DrawMode == DrawMode::ColoredBlended)) {
        fgColor = color24FTo16<DrawMode>(colColor);
    }

    // Cache some GPU RAM related params
    const uint16_t vramPixelW = core.ramPixelW;
    uint16_t* const pVram = core.pRam;

    // Pre-compute the 'x' coordinate in VRAM to read for the texture since 'u' is constant
    const uint32_t u = col.u;
    uint16_t texVramX;

    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        texVramX = col.u & core.texWinXMask;
        texVramX += core.texWinX;
        texVramX /= 2;
        texVramX &= core.texPageXMask;
        texVramX += core.texPageX;
        texVramX &= core.ramXMask;
    }

    // Process each pixel in the line being rasterized
    #if PSYDOOM_3DS
        static constexpr float UV_FIXED_SCALE = 65536.0f;
        const float startT = (0.5f + (float) ty - minY) * tStep;
        int32_t vFixed = (int32_t)((v1 + (v2 - v1) * startT) * UV_FIXED_SCALE);
        const int32_t vStridedStep = (int32_t)((v2 - v1) * tStep * UV_FIXED_SCALE) * stepY;
    #else
        float t = (0.5f + (float) ty - minY) * tStep;
        float tinv = 1.0f - t;
    #endif

    uint16_t* pDstPixelCol = core.pRam + px;
    const bool bEnableMasking = (!core.bDisableMasking);

#if PSYDOOM_3DS
    for (int32_t y = ty; y <= by; y += stepY) {
#else
    for (int32_t y = ty; y <= by; ++y) {
#endif
        // Compute the 'v' texture coordinate to use
        #if PSYDOOM_3DS
            const uint16_t v = (uint16_t)(vFixed >> 16);
            vFixed += vStridedStep;
        #else
            const uint16_t v = (uint16_t)(v1 * tinv + v2 * t);
            t += tStep;
            tinv -= tStep;
        #endif

        // Get the foreground color for the column pixel if the column is textured.
        // If the pixel is transparent and masking is enabled then also skip it, otherwise modulate it by the primitive color...
        if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
            // Figure out the VRAM coordinates to read the VRAM pixel from
            uint16_t vramY = v & core.texWinYMask;
            vramY += core.texWinY;
            vramY &= core.texPageYMask;
            vramY += core.texPageY;
            vramY &= core.ramYMask;

            // Read the VRAM pixel and lookup the actual texel using the clut index
            const uint16_t vramPixel = pVram[vramY * vramPixelW + texVramX];
            const uint16_t clutIdx = (vramPixel >> ((u & 1) * 8)) & 0xFF;
            fgColor = core.clutCache[clutIdx];

            if ((fgColor.bits == 0) && bEnableMasking)
                continue;

            fgColor = colorMul(fgColor, colColor);
        }

        // Do blending with the background if that is enabled
        uint16_t& dstPixel = pDstPixelCol[y * vramPixelW];

        if constexpr ((DrawMode == DrawMode::ColoredBlended) || (DrawMode == DrawMode::TexturedBlended)) {
            const Color16 bgColor = dstPixel;
            fgColor = colorBlend(bgColor, fgColor, core.blendMode);
        }

        // Save the output pixel and step to the next pixel
        dstPixel = fgColor;

        // PsyDoom 3DS: fill in the rows and columns that were skipped by the low detail steps
        #if PSYDOOM_3DS
            for (int32_t repY = y; (repY < y + stepY) && (repY <= by); ++repY) {
                uint16_t* const pRepRow = pDstPixelCol + repY * vramPixelW;

                for (int32_t repX = px + ((repY == y) ? 1 : 0); repX <= repEndX; ++repX) {
                    pRepRow[repX - px] = fgColor;
                }
            }
        #endif
    }
}

// Instantiate the variants of this function
template void draw<DrawMode::Colored>(Core& core, const DrawWallCol& col) noexcept;
template void draw<DrawMode::ColoredBlended>(Core& core, const DrawWallCol& col) noexcept;
template void draw<DrawMode::Textured>(Core& core, const DrawWallCol& col) noexcept;
template void draw<DrawMode::TexturedBlended>(Core& core, const DrawWallCol& col) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws a single gouraud shaded column of Doom wall pixels; texture format is assumed to be 8bpp.
// This is a new primitive added to help accelerate the classic renderer for PsyDoom.
//------------------------------------------------------------------------------------------------------------------------------------------
template <DrawMode DrawMode>
void draw(Core& core, const DrawWallColGouraud& col) noexcept {
    sanityCheckGpuDrawState(core);

    // Apply the draw offset to the column coordinates
    const int16_t drawOffsetX = core.drawOffsetX;
    const int16_t drawOffsetY = core.drawOffsetY;
    const int32_t px = col.x + drawOffsetX;
    const int32_t p1y = col.y1 + drawOffsetY;
    const int32_t p2y = col.y2 + drawOffsetY;

    // Get the minimum and maximum values of the column, and the swap the uvs if required
    float v1, v2;
    float r1, g1, b1;
    float r2, g2, b2;

    int32_t minY, maxY;

    if (p1y < p2y) {
        minY = p1y;                 maxY = p2y;
        v1 = col.v1;                v2 = col.v2;
        r1 = col.color1.comp.r;     r2 = col.color2.comp.r;
        g1 = col.color1.comp.g;     g2 = col.color2.comp.g;
        b1 = col.color1.comp.b;     b2 = col.color2.comp.b;
    } else {
        minY = p2y;                 maxY = p1y;
        v1 = col.v2;                v2 = col.v1;
        r1 = col.color2.comp.r;     r2 = col.color1.comp.r;
        g1 = col.color2.comp.g;     g2 = col.color1.comp.g;
        b1 = col.color2.comp.b;     b2 = col.color1.comp.b;
    }

    // Compute how much of the column will be rasterized; similar to triangles the last row is NOT drawn
    const int32_t yrange = maxY - minY;
    const int32_t ty = std::max((int32_t) core.drawAreaTy, minY);
    const int32_t by = std::min((int32_t) core.drawAreaBy, maxY - 1);

    // Compute the step in 't', the percentage along the line (used for interpolation of the uvs)
    const float tStep = 1.0f / (float) yrange;

    // Also similar to triangles, skip the column if the distances between the vertices exceed 511 on the y dimension.
    // Also skip if the column itself is outside the draw area.
    if ((yrange >= 512) || (px < core.drawAreaLx) || (px > core.drawAreaRx))
        return;

    // PsyDoom 3DS: in low detail mode the renderer only submits every Nth column, so widen each one to cover the
    // columns that were skipped and shade only every Nth row.
    #if PSYDOOM_3DS
        const int32_t stepX = gPixelStepX;
        const int32_t stepY = gPixelStepY;
        const int32_t repEndX = std::min<int32_t>(px + stepX - 1, core.drawAreaRx);
    #endif

    // If we're going to draw textured and with a CLUT make sure it is up to date
    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        updateClutCache(core);
    }

    // Cache some GPU RAM related params
    const uint16_t vramPixelW = core.ramPixelW;
    uint16_t* const pVram = core.pRam;

    // Pre-compute the 'x' coordinate in VRAM to read for the texture since 'u' is constant
    const uint32_t u = col.u;
    uint16_t texVramX;

    if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
        texVramX = col.u & core.texWinXMask;
        texVramX += core.texWinX;
        texVramX /= 2;
        texVramX &= core.texPageXMask;
        texVramX += core.texPageX;
        texVramX &= core.ramXMask;
    }

    // Process each pixel in the line being rasterized
    #if PSYDOOM_3DS
        static constexpr float INTERP_FIXED_SCALE = 65536.0f;
        const float startT = (0.5f + (float) ty - minY) * tStep;
        int32_t vFixed = (int32_t)((v1 + (v2 - v1) * startT) * INTERP_FIXED_SCALE);
        int32_t rFixed = (int32_t)((r1 + (r2 - r1) * startT) * INTERP_FIXED_SCALE);
        int32_t gFixed = (int32_t)((g1 + (g2 - g1) * startT) * INTERP_FIXED_SCALE);
        int32_t bFixed = (int32_t)((b1 + (b2 - b1) * startT) * INTERP_FIXED_SCALE);
        const int32_t vStridedStep = (int32_t)((v2 - v1) * tStep * INTERP_FIXED_SCALE) * stepY;
        const int32_t rStridedStep = (int32_t)((r2 - r1) * tStep * INTERP_FIXED_SCALE) * stepY;
        const int32_t gStridedStep = (int32_t)((g2 - g1) * tStep * INTERP_FIXED_SCALE) * stepY;
        const int32_t bStridedStep = (int32_t)((b2 - b1) * tStep * INTERP_FIXED_SCALE) * stepY;
    #else
        float t = (0.5f + (float) ty - minY) * tStep;
        float tInv = 1.0f - t;
    #endif

    uint16_t* pDstPixelCol = core.pRam + px;
    const bool bEnableMasking = (!core.bDisableMasking);

#if PSYDOOM_3DS
    for (int32_t y = ty; y <= by; y += stepY) {
#else
    for (int32_t y = ty; y <= by; ++y) {
#endif
        // Compute the texture coordinate and Gouraud color to use
        #if PSYDOOM_3DS
            const uint16_t v = (uint16_t)(vFixed >> 16);
            const uint8_t gColorR = (uint8_t)((rFixed + 32768) >> 16);
            const uint8_t gColorG = (uint8_t)((gFixed + 32768) >> 16);
            const uint8_t gColorB = (uint8_t)((bFixed + 32768) >> 16);
            vFixed += vStridedStep;
            rFixed += rStridedStep;
            gFixed += gStridedStep;
            bFixed += bStridedStep;
        #else
            const uint16_t v = (uint16_t)(v1 * tInv + v2 * t);
            const uint8_t gColorR = (uint8_t)(r1 * tInv + r2 * t + 0.5f);
            const uint8_t gColorG = (uint8_t)(g1 * tInv + g2 * t + 0.5f);
            const uint8_t gColorB = (uint8_t)(b1 * tInv + b2 * t + 0.5f);
            t += tStep;
            tInv -= tStep;
        #endif

        // Figure out the foreground color for the pixel for the current draw mode.
        // If the pixel is transparent and masking is enabled then also skip it, otherwise modulate it by the primitive color...
        Color16 fgColor;

        if constexpr ((DrawMode == DrawMode::Textured) || (DrawMode == DrawMode::TexturedBlended)) {
            // Doing texture mapping in addition to gouraud shading.
            // Figure out the VRAM coordinates to read the VRAM pixel from.
            uint16_t vramY = v & core.texWinYMask;
            vramY += core.texWinY;
            vramY &= core.texPageYMask;
            vramY += core.texPageY;
            vramY &= core.ramYMask;

            // Read the VRAM pixel and lookup the actual texel using the clut index
            const uint16_t vramPixel = pVram[vramY * vramPixelW + texVramX];
            const uint16_t clutIdx = (vramPixel >> ((u & 1) * 8)) & 0xFF;
            fgColor = core.clutCache[clutIdx];

            if ((fgColor.bits == 0) && bEnableMasking)
                continue;

            const Color24F colColor = Color24F{ gColorR, gColorG, gColorB };
            fgColor = colorMul(fgColor, colColor);
        } else {
            // Not doing texture mapping: foreground color is just the interpolated color
            fgColor = Color16::make(
                std::min<uint16_t>(((uint16_t) gColorR + 4) >> 3, 31u),
                std::min<uint16_t>(((uint16_t) gColorG + 4) >> 3, 31u),
                std::min<uint16_t>(((uint16_t) gColorB + 4) >> 3, 31u)
            );
        }

        // Do blending with the background if that is enabled
        uint16_t& dstPixel = pDstPixelCol[y * vramPixelW];

        if constexpr ((DrawMode == DrawMode::ColoredBlended) || (DrawMode == DrawMode::TexturedBlended)) {
            const Color16 bgColor = dstPixel;
            fgColor = colorBlend(bgColor, fgColor, core.blendMode);
        }

        // Save the output pixel and step to the next pixel
        dstPixel = fgColor;

        // PsyDoom 3DS: fill in the rows and columns that were skipped by the low detail steps
        #if PSYDOOM_3DS
            for (int32_t repY = y; (repY < y + stepY) && (repY <= by); ++repY) {
                uint16_t* const pRepRow = pDstPixelCol + repY * vramPixelW;

                for (int32_t repX = px + ((repY == y) ? 1 : 0); repX <= repEndX; ++repX) {
                    pRepRow[repX - px] = fgColor;
                }
            }
        #endif
    }
}

// Instantiate the variants of this function
template void draw<DrawMode::Colored>(Core& core, const DrawWallColGouraud& col) noexcept;
template void draw<DrawMode::ColoredBlended>(Core& core, const DrawWallColGouraud& col) noexcept;
template void draw<DrawMode::Textured>(Core& core, const DrawWallColGouraud& col) noexcept;
template void draw<DrawMode::TexturedBlended>(Core& core, const DrawWallColGouraud& col) noexcept;

END_NAMESPACE(Gpu)
