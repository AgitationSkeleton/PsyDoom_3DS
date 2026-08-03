//------------------------------------------------------------------------------------------------------------------------------------------
// A read only reference screen listing what each Nintendo 3DS control does.
//
// This is the 3DS replacement for the original PlayStation 'Configuration' screen, which PsyDoom removed along with
// the PSX pad button sprites it drew from the disc's 'BUTTONS' lump. Rather than reusing those PlayStation glyphs the
// buttons here are drawn from flat shaded primitives in a 3DS style: round face buttons, rounded shoulder buttons and
// text labels for the sticks and D-Pad. Doing it this way keeps the screen identical across the Doom, Final Doom and
// Master Edition packages, none of which have 3DS artwork on their discs.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "controls3ds_main.h"

#if PSYDOOM_3DS

#include "Doom/Base/i_drawcmds.h"
#include "Doom/Base/i_main.h"
#include "Doom/Base/i_misc.h"
#include "Doom/Base/i_texcache.h"
#include "Doom/Base/s_sound.h"
#include "Doom/Base/sounds.h"
#include "Doom/d_main.h"
#include "Doom/Game/g_game.h"
#include "Doom/Game/p_tick.h"
#include "Doom/Renderer/r_data.h"
#include "m_main.h"
#include "o_main.h"
#include "PsyDoom/Config/Config.h"
#include "PsyDoom/Controls.h"
#include "PsyDoom/ControlSchemes3DS.h"
#include "PsyDoom/Input.h"
#include "PsyDoom/Game.h"
#include "PsyDoom/PlayerPrefs.h"
#include "menu3ds.h"
#include "PsyDoom/Screens3DS.h"
#include "PsyDoom/Utils.h"
#include "PsyQ/LIBGPU.h"

#include <algorithm>
#include <cstring>

//------------------------------------------------------------------------------------------------------------------------------------------
// Glyph styling: the 3DS face buttons are pale grey discs with dark lettering, the shoulder buttons are pale grey
// lozenges. These are the colors that read best against the options screen background.
//------------------------------------------------------------------------------------------------------------------------------------------
// Colours taken from the PlayStation button artwork on the disc: a near black body, a grey bevel catching the light
// from the top left, and gold lettering with a darker gold shadow under it.
static constexpr uint8_t BTN_BODY_R = 16,   BTN_BODY_G = 16,    BTN_BODY_B = 16;
static constexpr uint8_t BTN_LIT_R = 96,    BTN_LIT_G = 96,     BTN_LIT_B = 96;
static constexpr uint8_t BTN_DARK_R = 48,   BTN_DARK_G = 48,    BTN_DARK_B = 48;
static constexpr uint8_t BTN_GOLD_R = 255,  BTN_GOLD_G = 225,   BTN_GOLD_B = 79;
static constexpr uint8_t BTN_GOLDDK_R = 184, BTN_GOLDDK_G = 144, BTN_GOLDDK_B = 40;

// Neutral shading for the 8x8 font, which the disc's palette already colours
static constexpr uint8_t BTN_TEXT_SHADE = 128;

enum class GlyphKind {
    Face,       // Round button with a single letter: A, B, X, Y
    Shoulder,   // Rounded lozenge with a short label: L, R, ZL, ZR
    Text        // Just a text label: D-PAD, START, and the two sticks
};

struct ControlEntry {
    GlyphKind   kind;
    const char* label;
    const char* action;
};

//------------------------------------------------------------------------------------------------------------------------------------------
// The controls to list. These must be kept in step with the 3DS defaults in 'ConfigSerialization_Controls.cpp'.
//------------------------------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------------------------------
// The actions that can be rebound here.
//
// Not every binding the engine has: only the ones worth changing on a handheld, kept short enough to fit on one screen
// alongside what they are currently bound to. Anything else is still editable in 'control_bindings.ini'.
//------------------------------------------------------------------------------------------------------------------------------------------
struct BindableAction {
    Controls::Binding   binding;
    const char*         name;
};

static constexpr BindableAction BINDABLE_ACTIONS[] = {
    { Controls::Binding::Action_Attack,         "Fire"          },
    { Controls::Binding::Action_Use,            "Use"           },
    { Controls::Binding::Modifier_Run,          "Run"           },
    { Controls::Binding::Modifier_Strafe,       "Strafe Mod"    },
    { Controls::Binding::Weapon_Next,           "Weapon Up"     },
    { Controls::Binding::Weapon_Previous,       "Weapon Down"   },
    { Controls::Binding::Weapon_GroupShotguns,  "Shotguns"      },
    { Controls::Binding::Weapon_GroupHeavy,     "Rocket/Saw"    },
    { Controls::Binding::Weapon_GroupRapid,     "Chaingun"      },
    { Controls::Binding::Weapon_GroupEnergy,    "Plasma/BFG"    },
    { Controls::Binding::Toggle_Pause,          "Pause"         },
    { Controls::Binding::Analog_MoveForward,    "Move Fwd"      },
    { Controls::Binding::Analog_MoveBackward,   "Move Back"     },
    { Controls::Binding::Analog_StrafeLeft,     "Strafe Left"   },
    { Controls::Binding::Analog_StrafeRight,    "Strafe Right"  },
    { Controls::Binding::Analog_TurnLeft,       "Turn Left"     },
    { Controls::Binding::Analog_TurnRight,      "Turn Right"    },
    { Controls::Binding::Digital_MoveForward,   "Dig Fwd"       },
    { Controls::Binding::Digital_MoveBackward,  "Dig Back"      },
    { Controls::Binding::Digital_TurnLeft,      "Dig Turn L"    },
    { Controls::Binding::Digital_TurnRight,     "Dig Turn R"    },
    { Controls::Binding::Digital_StrafeLeft,    "Dig Strafe L"  },
    { Controls::Binding::Digital_StrafeRight,   "Dig Strafe R"  },
    { Controls::Binding::Menu_Ok,               "Menu Ok"       },
    { Controls::Binding::Menu_Back,             "Menu Back"     },
    { Controls::Binding::Automap_ZoomIn,        "Map Zoom In"   },
    { Controls::Binding::Automap_ZoomOut,       "Map Zoom Out"  },
};

static constexpr int32_t NUM_BINDABLE_ACTIONS = (int32_t) C_ARRAY_SIZE(BINDABLE_ACTIONS);

// The rows the player can act on: three of their own, then one per bindable action
enum MenuRow : int32_t {
    row_scheme,
    row_reset,
    row_back,
    row_first_binding,
    num_menu_rows = row_first_binding + NUM_BINDABLE_ACTIONS
};

// Set while waiting for the player to press something to bind, or -1 when not waiting
static int32_t gCapturingRow = -1;

// How many binding rows fit on screen at once, and which one is at the top of them.
// There are more actions than rows, so the list scrolls to follow the cursor.
static constexpr int32_t VISIBLE_BINDING_ROWS = 11;
static int32_t gScrollOffset = 0;

static constexpr int16_t ROW_SCHEME_Y = 24;
static constexpr int16_t ROW_RESET_Y = 40;
static constexpr int16_t ROW_BACK_Y = 56;
static constexpr int16_t LIST_FIRST_Y = 74;
static constexpr int16_t LIST_ROW_H = 12;
static constexpr int16_t BUTTON_H = 9;

static int32_t gCursorRow = row_scheme;


//------------------------------------------------------------------------------------------------------------------------------------------
// Draws a flat shaded rectangle in UI space
//------------------------------------------------------------------------------------------------------------------------------------------
static void DrawFlatRect(
    const int16_t x,
    const int16_t y,
    const int16_t w,
    const int16_t h,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b
) noexcept {
    if ((w <= 0) || (h <= 0))
        return;

    POLY_F4 quad = {};
    LIBGPU_SetPolyF4(quad);
    LIBGPU_setRGB0(quad, r, g, b);
    LIBGPU_setXY4(quad, x, y, (int16_t)(x + w), y, x, (int16_t)(y + h), (int16_t)(x + w), (int16_t)(y + h));
    I_AddPrim(quad);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// What a binding is currently set to, shortened to fit the row.
//
// The engine writes bindings as a list like "Gamepad A, Gamepad B", which is both too long here and mostly the word
// "Gamepad" repeated. Only the first input is shown, without that prefix, and a binding with nothing on it says so.
//------------------------------------------------------------------------------------------------------------------------------------------
static void ShortenInputName(std::string& name) noexcept {
    // What the engine calls each input against what is actually printed on a 3DS
    struct InputName { const char* engine; const char* console; };

    static constexpr InputName INPUT_NAMES[] = {
        { "LeftShoulder",   "L"         },
        { "RightShoulder",  "R"         },
        { "LeftTrigger",    "ZL"        },
        { "RightTrigger",   "ZR"        },
        { "Back",           "SELECT"    },
        { "Start",          "START"     },
        { "DpUp",           "D-UP"      },
        { "DpDown",         "D-DOWN"    },
        { "DpLeft",         "D-LEFT"    },
        { "DpRight",        "D-RIGHT"   },
        { "LeftX-",         "CIRCLE L"  },
        { "LeftX+",         "CIRCLE R"  },
        { "LeftY-",         "CIRCLE U"  },
        { "LeftY+",         "CIRCLE D"  },
        { "RightX-",        "CSTICK L"  },
        { "RightX+",        "CSTICK R"  },
        { "RightY-",        "CSTICK U"  },
        { "RightY+",        "CSTICK D"  },
    };

    constexpr const char* const PREFIX = "Gamepad ";
    const size_t prefixLen = std::strlen(PREFIX);

    if (name.compare(0, prefixLen, PREFIX) == 0) {
        name.erase(0, prefixLen);
    }

    while ((!name.empty()) && (name.back() == ' ')) {
        name.pop_back();
    }

    for (const InputName& mapping : INPUT_NAMES) {
        if (name == mapping.engine) {
            name = mapping.console;
            break;
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Collects every input on a binding, named the way the console names them.
// Returns how many were found, which is zero for a binding with nothing on it.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t MAX_SHOWN_INPUTS = 3;

static int32_t GetBindingInputNames(const Controls::Binding binding, std::string* const outNames) noexcept {
    const Controls::BindingData& data = Controls::getBindingData(binding);
    const int32_t numInputs = std::min<int32_t>((int32_t) data.numInputSources, MAX_SHOWN_INPUTS);

    for (int32_t i = 0; i < numInputs; ++i) {
        outNames[i].clear();
        Controls::appendInputSrcToStr(data.inputSources[i], outNames[i]);
        ShortenInputName(outNames[i]);
    }

    return numInputs;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Which kind of button a label belongs on: a face button, a shoulder, or a plaque for everything else
//------------------------------------------------------------------------------------------------------------------------------------------
static GlyphKind GetGlyphKindForLabel(const char* const label) noexcept {
    if ((label[0] != 0) && (label[1] == 0)) {
        switch (label[0]) {
            case 'A': case 'B': case 'X': case 'Y':     return GlyphKind::Face;
            case 'L': case 'R':                         return GlyphKind::Shoulder;
            default:                                    break;
        }
    }

    if ((std::strcmp(label, "ZL") == 0) || (std::strcmp(label, "ZR") == 0))
        return GlyphKind::Shoulder;

    return GlyphKind::Text;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Takes whatever the player just pressed and puts it on the given binding.
// Returns false if nothing has been pressed yet, so the caller keeps waiting.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool TryCaptureBinding(const Controls::Binding binding) noexcept {
    const std::vector<GamepadInput>& justPressed = Input::getGamepadInputsJustPressed();

    if (justPressed.empty())
        return false;

    const GamepadInput input = justPressed[0];
    const bool bIsAxis = (input >= GamepadInput::AXIS_LEFT_X);

    Controls::InputSrc src = {};
    src.device = (bIsAxis) ? Controls::InputSrc::GAMEPAD_AXIS : Controls::InputSrc::GAMEPAD_BUTTON;
    src.subaxis = Controls::InputSrc::SUBAXIS_POS;
    src.input = (uint16_t) input;

    std::string bindingStr;
    Controls::appendInputSrcToStr(src, bindingStr);

    if (bindingStr.empty())
        return false;

    Controls::parseBinding(binding, bindingStr.c_str());

    // The layout is now the player's own, not one of the presets, and it has to be written out to survive
    PlayerPrefs::gControlScheme = (int32_t) ControlSchemes3DS::Scheme::Custom;
    Config::gbNeedSave_Controls = true;
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws the skull cursor at the given row, the same way the other menus do
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
// Points the GPU at the texture page holding the 8x8 font.
//
// 'I_DrawStringSmall' only emits sprites, and a sprite carries no texture page of its own, so it draws with whatever
// page the last draw mode left behind. Here that is the options background, which turns the text into garbage.
//------------------------------------------------------------------------------------------------------------------------------------------
static void SetSmallFontDrawMode() noexcept {
    DR_MODE drawModePrim = {};
    const SRECT texWindow = { (int16_t) gTex_STATUS.texPageCoordX, (int16_t) gTex_STATUS.texPageCoordY, 256, 256 };
    LIBGPU_SetDrawMode(drawModePrim, false, false, gTex_STATUS.texPageId, &texWindow);
    I_AddPrim(drawModePrim);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws a button body: a near black key with a bevel, lit from the top left the way the disc's button artwork is.
//
// The corner size decides how round it reads - a large one gives the disc of a face button, a small one the lozenge of
// a shoulder button - and is approximated by stacking rectangles rather than drawing a real curve, which at this size
// is the same handful of pixels either way.
//------------------------------------------------------------------------------------------------------------------------------------------
static void DrawButtonBody(const int16_t x, const int16_t y, const int16_t w, const int16_t h, const int16_t corner) noexcept {
    const auto roundedRect = [](
        const int16_t rx,
        const int16_t ry,
        const int16_t rw,
        const int16_t rh,
        const int16_t rc,
        const uint8_t r,
        const uint8_t g,
        const uint8_t b
    ) noexcept {
        DrawFlatRect((int16_t)(rx + rc), ry, (int16_t)(rw - rc * 2), rh, r, g, b);
        DrawFlatRect(rx, (int16_t)(ry + rc), rw, (int16_t)(rh - rc * 2), r, g, b);
        DrawFlatRect((int16_t)(rx + 1), (int16_t)(ry + 1), (int16_t)(rw - 2), (int16_t)(rh - 2), r, g, b);
    };

    // The bevel is the whole key drawn in the lit grey, with the shadow grey and then the body laid back over it,
    // each inset by a pixel from the corner the light does not reach
    roundedRect(x, y, w, h, corner, BTN_LIT_R, BTN_LIT_G, BTN_LIT_B);
    roundedRect((int16_t)(x + 1), (int16_t)(y + 1), (int16_t)(w - 1), (int16_t)(h - 1), corner, BTN_DARK_R, BTN_DARK_G, BTN_DARK_B);
    roundedRect((int16_t)(x + 1), (int16_t)(y + 1), (int16_t)(w - 2), (int16_t)(h - 2), corner, BTN_BODY_R, BTN_BODY_G, BTN_BODY_B);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// A 5x7 lettering set, just the characters the button faces need.
//
// The 8x8 font on the disc is red in this palette, and a sprite can only be darkened from the colour it already is, so
// there is no way to get gold lettering out of it. These characters are drawn as geometry instead, which also matches
// how the shoulder button artwork looks: gold with a darker gold shadow beneath.
//------------------------------------------------------------------------------------------------------------------------------------------
struct GlyphChar {
    char    c;
    uint8_t rows[7];    // Five bits used per row, most significant bit leftmost
};

static constexpr GlyphChar GLYPH_CHARS[] = {
    { 'A', { 0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } },
    { 'B', { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 } },
    { 'L', { 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 } },
    { 'R', { 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 } },
    { 'X', { 0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001 } },
    { 'Y', { 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100 } },
    { 'Z', { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111 } },
};

static constexpr int16_t GLYPH_CHAR_W = 5;
static constexpr int16_t GLYPH_CHAR_H = 7;
static constexpr int16_t GLYPH_CHAR_ADVANCE = 6;

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws one character of the lettering set in a flat colour, a row of pixels at a time
//------------------------------------------------------------------------------------------------------------------------------------------
static void DrawGlyphChar(
    const char c,
    const int16_t x,
    const int16_t y,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b
) noexcept {
    const GlyphChar* pChar = nullptr;

    for (const GlyphChar& candidate : GLYPH_CHARS) {
        if (candidate.c == c) {
            pChar = &candidate;
            break;
        }
    }

    if (!pChar)
        return;

    for (int16_t row = 0; row < GLYPH_CHAR_H; ++row) {
        const uint8_t bits = pChar->rows[row];

        // Emit one rectangle per unbroken run of pixels rather than one per pixel
        int16_t runStart = -1;

        for (int16_t col = 0; col <= GLYPH_CHAR_W; ++col) {
            const bool bSet = (col < GLYPH_CHAR_W) && ((bits & (1u << (GLYPH_CHAR_W - 1 - col))) != 0);

            if (bSet && (runStart < 0)) {
                runStart = col;
            } else if ((!bSet) && (runStart >= 0)) {
                DrawFlatRect((int16_t)(x + runStart), (int16_t)(y + row), (int16_t)(col - runStart), 1, r, g, b);
                runStart = -1;
            }
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws a short label in the lettering set, in gold over a darker gold shadow
//------------------------------------------------------------------------------------------------------------------------------------------
static void DrawGoldLabel(const char* const text, const int16_t x, const int16_t y) noexcept {
    int16_t drawX = x;

    for (const char* pChar = text; *pChar; ++pChar) {
        DrawGlyphChar(*pChar, (int16_t)(drawX + 1), (int16_t)(y + 1), BTN_GOLDDK_R, BTN_GOLDDK_G, BTN_GOLDDK_B);
        DrawGlyphChar(*pChar, drawX, y, BTN_GOLD_R, BTN_GOLD_G, BTN_GOLD_B);
        drawX = (int16_t)(drawX + GLYPH_CHAR_ADVANCE);
    }
}

static int16_t GetGoldLabelWidth(const char* const text) noexcept {
    int16_t width = 0;

    for (const char* pChar = text; *pChar; ++pChar) {
        width = (int16_t)(width + GLYPH_CHAR_ADVANCE);
    }

    return (width > 0) ? (int16_t)(width - (GLYPH_CHAR_ADVANCE - GLYPH_CHAR_W)) : 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws one 3DS button glyph and returns the width it occupied
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t DrawButtonGlyph(const ControlEntry& entry, const int16_t x, const int16_t y) noexcept {
    switch (entry.kind) {
        // A face button: a small disc with its letter on it
        case GlyphKind::Face: {
            constexpr int16_t SIZE = BUTTON_H;

            DrawButtonBody(x, y, SIZE, SIZE, 3);
            DrawGoldLabel(
                entry.label,
                (int16_t)(x + (SIZE - GetGoldLabelWidth(entry.label)) / 2),
                (int16_t)(y + (SIZE - GLYPH_CHAR_H) / 2)
            );

            return SIZE;
        }

        // A shoulder button: a wide lozenge sized to whatever label it carries
        case GlyphKind::Shoulder: {
            const int16_t labelWidth = GetGoldLabelWidth(entry.label);
            const int16_t width = (int16_t)(labelWidth + 6);
            constexpr int16_t HEIGHT = BUTTON_H;

            DrawButtonBody(x, y, width, HEIGHT, 2);
            DrawGoldLabel(entry.label, (int16_t)(x + 3), (int16_t)(y + (HEIGHT - GLYPH_CHAR_H) / 2));

            return width;
        }

        // Sticks, the D-Pad, START and SELECT: a dark plaque with its name on it, like the pause plaque on the disc
        default: {
            const int32_t labelWidth = (int32_t) std::strlen(entry.label) * SMALL_FONT_SIZE;

            if (labelWidth <= 0)
                return 0;

            DrawButtonBody(x, y, (int16_t)(labelWidth + 6), BUTTON_H + 1, 2);
            SetSmallFontDrawMode();
            I_DrawStringSmall(
                x + 3,
                y + 1,
                entry.label,
                Game::getTexClut_STATUS(),
                BTN_TEXT_SHADE,
                BTN_TEXT_SHADE,
                BTN_TEXT_SHADE,
                false,
                false
            );

            return labelWidth + 6;
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initializes the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void Controls3DS_Init() noexcept {
    S_StartSound(nullptr, sfx_pistol);

    // Make sure the shared options background is resident; this screen can be reached without passing through the
    // title or main menu (for example after a warp), and those are the only other places that cache it.
    I_LoadAndCacheTexLump(gTex_OptionsBg, Game::getTexLumpName_OptionsBg());
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Shuts down the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void Controls3DS_Shutdown([[maybe_unused]] const gameaction_t exitAction) noexcept {
    S_StartSound(nullptr, sfx_pistol);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Update logic: the screen is purely informational, so any 'back' or 'ok' input leaves it
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t Controls3DS_Update() noexcept {
    // PsyDoom: in all UIs tick only if vblanks are registered as elapsed
    const uint32_t playerIdx = gCurPlayerIndex;

    if (gPlayersElapsedVBlanks[playerIdx] <= 0) {
        gbKeepInputEvents = true;   // Don't consume 'key pressed' etc. events yet, not ticking...
        return ga_nothing;
    }

    const TickInputs& inputs = gTickInputs[playerIdx];
    const TickInputs& oldInputs = gOldTickInputs[playerIdx];

    const bool bMenuBack = (inputs.fMenuBack() && (!oldInputs.fMenuBack()));
    const bool bMenuOk = (inputs.fMenuOk() && (!oldInputs.fMenuOk()));
    const bool bMenuUp = (inputs.fMenuUp() && (!oldInputs.fMenuUp()));
    const bool bMenuDown = (inputs.fMenuDown() && (!oldInputs.fMenuDown()));
    const bool bMenuLeft = (inputs.fMenuLeft() && (!oldInputs.fMenuLeft()));
    const bool bMenuRight = (inputs.fMenuRight() && (!oldInputs.fMenuRight()));

    // Waiting for the player to press something to bind: everything else is on hold until they do, or back out
    if (gCapturingRow >= 0) {
        if (bMenuBack) {
            gCapturingRow = -1;
            S_StartSound(nullptr, sfx_swtchx);
            return ga_nothing;
        }

        const int32_t actionIdx = gCapturingRow - row_first_binding;

        if ((actionIdx >= 0) && (actionIdx < NUM_BINDABLE_ACTIONS)) {
            if (TryCaptureBinding(BINDABLE_ACTIONS[actionIdx].binding)) {
                gCapturingRow = -1;
                S_StartSound(nullptr, sfx_swtchx);
            }
        } else {
            gCapturingRow = -1;
        }

        return ga_nothing;
    }

    if (bMenuBack)
        return ga_exit;

    // A tap picks a row, and a tap on the row already picked acts on it
    const int32_t tappedRow = Screens3DS::consumeTappedItem();

    if (tappedRow >= 0) {
        if (tappedRow == gCursorRow) {
            if (gCursorRow == row_back)
                return ga_exit;

            if (gCursorRow == row_reset) {
                PlayerPrefs::resetControlsToDefault();
                S_StartSound(nullptr, sfx_swtchx);
            } else if (gCursorRow >= row_first_binding) {
                gCapturingRow = gCursorRow;
                Input::consumeEvents();     // So the tap that got here is not taken as the new binding
                S_StartSound(nullptr, sfx_swtchx);
            } else {
                PlayerPrefs::setControlScheme((PlayerPrefs::gControlScheme + 1) % ControlSchemes3DS::NUM_SCHEMES);
                S_StartSound(nullptr, sfx_swtchx);
            }
        } else {
            gCursorRow = tappedRow;
            S_StartSound(nullptr, sfx_pstop);
        }

        return ga_nothing;
    }

    if (bMenuUp || bMenuDown) {
        gCursorRow = (gCursorRow + ((bMenuDown) ? 1 : num_menu_rows - 1)) % num_menu_rows;
        S_StartSound(nullptr, sfx_pstop);

        // Scroll the list so whichever binding row the cursor is on stays visible
        if (gCursorRow >= row_first_binding) {
            const int32_t actionIdx = gCursorRow - row_first_binding;
            gScrollOffset = std::clamp<int32_t>(gScrollOffset, actionIdx - (VISIBLE_BINDING_ROWS - 1), actionIdx);
        } else {
            gScrollOffset = 0;
        }

        gScrollOffset = std::clamp<int32_t>(gScrollOffset, 0, std::max<int32_t>(NUM_BINDABLE_ACTIONS - VISIBLE_BINDING_ROWS, 0));
    }

    if (gCursorRow == row_scheme) {
        // Cycles rather than clamping, so it must act on a press or it spins
        const int32_t step = (bMenuRight) ? 1 : ((bMenuLeft) ? -1 : 0);

        if (step != 0) {
            const int32_t next = (PlayerPrefs::gControlScheme + step + ControlSchemes3DS::NUM_SCHEMES) % ControlSchemes3DS::NUM_SCHEMES;
            PlayerPrefs::setControlScheme(next);
            S_StartSound(nullptr, sfx_swtchx);
        }
    }

    if (bMenuOk) {
        if (gCursorRow == row_back)
            return ga_exit;

        if (gCursorRow == row_reset) {
            PlayerPrefs::resetControlsToDefault();
            S_StartSound(nullptr, sfx_swtchx);
        } else if (gCursorRow >= row_first_binding) {
            gCapturingRow = gCursorRow;
            Input::consumeEvents();     // So the press that got here is not taken as the new binding
            S_StartSound(nullptr, sfx_swtchx);
        }
    }

    return ga_nothing;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void Controls3DS_Draw() noexcept {
    I_IncDrawnFrameCount();
    Utils::onBeginUIDrawing();

    // The top screen is composed separately, with the background and the title only
    Menu3DS_DrawTitleScreen(gTex_OptionsBg, Game::getTexClut_OptionsBg(), "3DS Controls");

    O_DrawBackground(gTex_OptionsBg, Game::getTexClut_OptionsBg(), 128, 128, 128);

    if (gGameAction == ga_nothing) {
        const ControlSchemes3DS::Scheme scheme = (ControlSchemes3DS::Scheme) PlayerPrefs::gControlScheme;

        // The rows the player can act on
        char schemeLabel[48];
        std::snprintf(schemeLabel, sizeof(schemeLabel), "Scheme %s", ControlSchemes3DS::getSchemeName(scheme));

        I_DrawString(40, ROW_SCHEME_Y, schemeLabel);
        I_DrawString(40, ROW_RESET_Y, "Reset To Default");
        I_DrawString(40, ROW_BACK_Y, "Back");

        Screens3DS::addTouchItem(row_scheme, ROW_SCHEME_Y - 2, 18);
        Screens3DS::addTouchItem(row_reset, ROW_RESET_Y - 2, 18);
        Screens3DS::addTouchItem(row_back, ROW_BACK_Y - 2, 18);

        // One row per action that can be rebound, showing what it is on at the moment
        constexpr int16_t ACTION_X = 20;
        constexpr int16_t BINDING_X = 150;

        const uint16_t clutId = Game::getTexClut_STATUS();
        SetSmallFontDrawMode();

        const int32_t lastVisible = std::min<int32_t>(gScrollOffset + VISIBLE_BINDING_ROWS, NUM_BINDABLE_ACTIONS);

        for (int32_t i = gScrollOffset; i < lastVisible; ++i) {
            const int16_t y = (int16_t)(LIST_FIRST_Y + (i - gScrollOffset) * LIST_ROW_H);
            const bool bCapturingThis = (gCapturingRow == row_first_binding + i);

            I_DrawStringSmall(ACTION_X, y, BINDABLE_ACTIONS[i].name, clutId, 128, 128, 128, false, false);

            if (bCapturingThis) {
                // Flash so it is obvious the screen is waiting rather than stuck
                if ((gTicCon & 8) != 0) {
                    I_DrawStringSmall(BINDING_X, y, "PRESS...", clutId, 128, 128, 128, false, false);
                }
            } else {
                // Show every button on the action, as the buttons themselves rather than the engine's names for them
                std::string names[MAX_SHOWN_INPUTS];
                const int32_t numInputs = GetBindingInputNames(BINDABLE_ACTIONS[i].binding, names);

                if (numInputs <= 0) {
                    const ControlEntry noneEntry = { GlyphKind::Text, "None", "" };
                    DrawButtonGlyph(noneEntry, BINDING_X, (int16_t)(y - 1));
                } else {
                    int16_t glyphX = BINDING_X;

                    for (int32_t inputIdx = 0; inputIdx < numInputs; ++inputIdx) {
                        const char* const label = names[inputIdx].c_str();
                        const ControlEntry boundEntry = { GetGlyphKindForLabel(label), label, "" };
                        glyphX = (int16_t)(glyphX + DrawButtonGlyph(boundEntry, glyphX, (int16_t)(y - 1)) + 3);
                    }
                }

                SetSmallFontDrawMode();
            }

            Screens3DS::addTouchItem(row_first_binding + i, y - 1, LIST_ROW_H);
        }

        // The skull marks whichever row the player is on, including the rows of bindings
        const int16_t cursorY = (
            (gCursorRow == row_reset) ? ROW_RESET_Y :
            (gCursorRow == row_back) ? ROW_BACK_Y :
            (gCursorRow >= row_first_binding) ?
                (int16_t)(LIST_FIRST_Y + (gCursorRow - row_first_binding - gScrollOffset) * LIST_ROW_H - 2) :
            ROW_SCHEME_Y
        );

        DrawCursor((gCursorRow >= row_first_binding) ? 18 : 38, cursorY + 2);
    }

    // PsyDoom: draw any enabled performance counters
    I_DrawEnabledPerfCounters();

    I_SubmitGpuCmds();
    I_DrawPresent();
}

#endif  // #if PSYDOOM_3DS
