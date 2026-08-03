#include "ConfigSerialization_Controls.h"

#include "Config.h"
#include "IniUtils.h"
#include "PsyDoom/Controls.h"

#include <string>

BEGIN_NAMESPACE(ConfigSerialization)

using namespace Config;

//------------------------------------------------------------------------------------------------------------------------------------------
// Config field storage
//------------------------------------------------------------------------------------------------------------------------------------------
Config_Controls gConfig_Controls = {};

//------------------------------------------------------------------------------------------------------------------------------------------
// A documentation header to place at the start of the controls INI file
//------------------------------------------------------------------------------------------------------------------------------------------
#if PSYDOOM_3DS
const char* const CONTROL_BINDINGS_INI_HEADER =
R"(#---------------------------------------------------------------------------------------------------
# Nintendo 3DS control bindings.
#
# Every physical control is reachable through SDL's gamepad names. The 3DS hardware maps like so:
#
#   Gamepad A / B / X / Y           A / B / X / Y
#   Gamepad LeftShoulder            L
#   Gamepad RightShoulder           R
#   Gamepad LeftTrigger             ZL              (New 3DS / New 2DS XL only)
#   Gamepad RightTrigger            ZR              (New 3DS / New 2DS XL only)
#   Gamepad Start / Back            START / SELECT
#   Gamepad DpUp/Down/Left/Right    D-Pad
#   Gamepad LeftX / LeftY           Circle Pad
#   Gamepad RightX / RightY         C-Stick         (New 3DS / New 2DS XL only)
#
# Assign inputs to the actions below, separating multiple inputs for one action with commas (,).
# Names are case insensitive. Leave a value empty to unbind it.
#
# The shipped defaults are fully playable on an Old 3DS/2DS, which has no C-Stick, ZL or ZR:
#
#   Circle Pad      move forward/back, turn left/right
#   C-Stick         strafe left/right, move forward/back
#   D-Pad           move forward/back, turn left/right
#   A               use / open, confirm in menus
#   B               run, back out of menus
#   X / Y           previous / next weapon
#   L               strafe modifier (makes the D-Pad turn inputs strafe), automap zoom out
#   R               attack, automap zoom in
#   ZL / ZR         run / attack
#   START           pause
#   SELECT          unbound (the touch screen shows the automap the whole time)
#---------------------------------------------------------------------------------------------------

)";
#else
const char* const CONTROL_BINDINGS_INI_HEADER = 
R"(#---------------------------------------------------------------------------------------------------
# Control bindings: available input source names/identifiers.
#
# Assign these inputs to actions listed below to setup control bindings.
# Separate multiple input sources for one action using commas (,).
#
# Notes:
#   (1) Gamepad inputs (i.e. Xbox controller style inputs) can only be used for certain types of
#       game controllers which are supported and recognized by the SDL library that PsyDoom uses.
#       If you find your controller is not supported, use generic/numbered joystick inputs instead.
#   (2) All input source names are case insensitive.
#   (3) The ',' (comma) keyboard key must be escaped/prefixed by backslash (\) when used as an input:
#           \,
#   (4) Similar keyboard keys are collapsed into a range for brevity (e.g A-Z).
#   (5) For a full list of available keyboard key names, including very uncommon ones, see:
#         https://wiki.libsdl.org/SDL_Scancode
#
# Mouse buttons:
#       Mouse Left              Mouse X1
#       Mouse Right             Mouse X2
#       Mouse Middle
#
# Mouse wheel axes:
#       Mouse Wheel+
#       Mouse Wheel-
#
# SDL recognized gamepad, axes:
#       Gamepad LeftTrigger     Gamepad RightTrigger
#       Gamepad LeftX-          Gamepad LeftX+          Gamepad LeftY-          Gamepad LeftY+
#       Gamepad RightX-         Gamepad RightX+         Gamepad RightY-         Gamepad RightY+
#
# SDL recognized gamepad, buttons:
#       Gamepad A               Gamepad DpUp            Gamepad LeftStick
#       Gamepad B               Gamepad DpDown          Gamepad RightStick
#       Gamepad X               Gamepad DpLeft          Gamepad LeftShoulder
#       Gamepad Y               Gamepad DpRight         Gamepad RightShoulder
#       Gamepad Back            Gamepad Start           Gamepad Guide
#
# Generic joystick inputs: axes, buttons and hat/d-pad directions.
# Replace '[1-99]' with the desired button, hat or axis number:
#       Joystick Button[1-99]   Joystick Hat[1-99] Up
#       Joystick Axis[1-99]+    Joystick Hat[1-99] Down
#       Joystick Axis[1-99]-    Joystick Hat[1-99] Left
#                               Joystick Hat[1-99] Right
#
# Keyboard keys (commonly used, see link above for full list):
#       A-Z                     Return                  Backspace               Home
#       0-9                     Escape                  Pause                   End
#       Keypad 0-9              Space                   PageUp                  Insert
#       F1-F12                  Tab                     PageDown                Delete
#       Left                    Right                   PrintScreen             CapsLock
#       Up                      Down                    ScrollLock              Numlock
#       Left Ctrl               Left Shift              Left Alt                Application
#       Right Ctrl              Right Shift             Right Alt               Menu
#       Left GUI                Right GUI               -                       = 
#       [                       ]                       \                       #
#       ;                       '                       \,                      `
#       .                       /                       Keypad /                Keypad *
#       Keypad -                Keypad +                Keypad Enter            Keypad .
#---------------------------------------------------------------------------------------------------

)";
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Helpers that define a control binding config field
//------------------------------------------------------------------------------------------------------------------------------------------
static ConfigField makeControlConfigField(
    const char* const comment,
    const char* const name,
    Controls::Binding binding,
    const char* const defaultSetting
) noexcept {
    return makeConfigField(
        name,
        comment,
        [=](const IniUtils::IniValue& value) noexcept { Controls::parseBinding(binding, value.strValue.c_str()); },
        [=](IniUtils::IniValue& valueOut) noexcept { Controls::bindingToStr(binding, valueOut.strValue); },
        [=]() noexcept { Controls::parseBinding(binding, defaultSetting); }
    );
}

static ConfigField makeControlConfigField(
    const char* const name,
    Controls::Binding binding,
    const char* const defaultSetting
) noexcept {
    return makeControlConfigField("", name, binding, defaultSetting);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initialize the config serializers for control related config
//------------------------------------------------------------------------------------------------------------------------------------------
void initCfgSerialization_Controls() noexcept {
    auto& cfg = gConfig_Controls;

    // Helper macros to remove some of the repetition
    #define CONTROL_FIELD(BINDING_NAME, DEFAULT_VALUE)\
        makeControlConfigField(#BINDING_NAME, Controls::Binding::BINDING_NAME, DEFAULT_VALUE)

    #define CONTROL_FIELD_WITH_DOC(COMMENT, BINDING_NAME, DEFAULT_VALUE)\
        makeControlConfigField(COMMENT, #BINDING_NAME, Controls::Binding::BINDING_NAME, DEFAULT_VALUE)
    #if PSYDOOM_3DS
        #define PLATFORM_BINDING(DESKTOP_VALUE, HANDHELD_VALUE) HANDHELD_VALUE
    #else
        #define PLATFORM_BINDING(DESKTOP_VALUE, HANDHELD_VALUE) DESKTOP_VALUE
    #endif

    // Analog movement and turning actions
    cfg.analog_moveForward = CONTROL_FIELD_WITH_DOC(
        "Analog movement and turning actions.\n"
        "Note: analog turn sensitivity can be modified via the 'Input' section.",
        Analog_MoveForward,
        PLATFORM_BINDING("Gamepad LeftY-", "Gamepad LeftY-")
    );

    cfg.analog_moveBackward = CONTROL_FIELD(Analog_MoveBackward, "Gamepad LeftY+");
    cfg.analog_strafeLeft = CONTROL_FIELD(Analog_StrafeLeft, "Gamepad LeftX-");
    cfg.analog_strafeRight = CONTROL_FIELD(Analog_StrafeRight, "Gamepad LeftX+");
    cfg.analog_turnLeft = CONTROL_FIELD(Analog_TurnLeft, "Gamepad RightX-");
    cfg.analog_turnRight = CONTROL_FIELD(Analog_TurnRight, "Gamepad RightX+");

    // Digital movement and turning actions
    cfg.digital_moveForward = CONTROL_FIELD_WITH_DOC(
        "Digital movement and turning actions. Turn sensitivity and acceleration are based on the original\n"
        "PSX values, though greater precision can be achieved if using uncapped framerates.",
        Digital_MoveForward,
        PLATFORM_BINDING("W, Up, Gamepad DpUp", "")
    );

    cfg.digital_moveBackward = CONTROL_FIELD(Digital_MoveBackward, PLATFORM_BINDING("S, Down, Gamepad DpDown", ""));
    cfg.digital_strafeLeft = CONTROL_FIELD(Digital_StrafeLeft, PLATFORM_BINDING("A", ""));
    cfg.digital_strafeRight = CONTROL_FIELD(Digital_StrafeRight, PLATFORM_BINDING("D", ""));
    cfg.digital_turnLeft = CONTROL_FIELD(Digital_TurnLeft, PLATFORM_BINDING("Left, Gamepad DpLeft", ""));
    cfg.digital_turnRight = CONTROL_FIELD(Digital_TurnRight, PLATFORM_BINDING("Right, Gamepad DpRight", ""));

    // In-game actions & modifiers
    cfg.action_use = CONTROL_FIELD_WITH_DOC(
        "In-game actions & modifiers",
        Action_Use,
        PLATFORM_BINDING("Space, E, Mouse Right, Gamepad B", "Gamepad A, Gamepad B, Gamepad LeftShoulder")
    );

    cfg.action_attack = CONTROL_FIELD(Action_Attack, PLATFORM_BINDING("Mouse Left, Gamepad RightTrigger, Left Ctrl, Right Ctrl, Gamepad Y", "Gamepad RightShoulder, Gamepad X"));
    cfg.action_respawn = CONTROL_FIELD(Action_Respawn, PLATFORM_BINDING("Mouse Left, Gamepad RightTrigger, Left Ctrl, Right Ctrl, Gamepad Y", "Gamepad RightShoulder, Gamepad X"));
    cfg.modifier_run = CONTROL_FIELD(Modifier_Run, PLATFORM_BINDING("Left Shift, Right Shift, Gamepad X, Gamepad LeftTrigger", "Gamepad Y"));
    cfg.modifier_strafe = CONTROL_FIELD(Modifier_Strafe, PLATFORM_BINDING("Left Alt, Right Alt, Gamepad A", ""));
    cfg.toggle_autorun = CONTROL_FIELD(Toggle_Autorun, PLATFORM_BINDING("CapsLock", ""));
    cfg.quicksave = CONTROL_FIELD(Quicksave, PLATFORM_BINDING("F5", ""));
    cfg.quickload = CONTROL_FIELD(Quickload, PLATFORM_BINDING("F9", ""));

    // Toggles
    cfg.toggle_pause = CONTROL_FIELD_WITH_DOC(
        "Toggle in-game pause, automap, uncapped framerate, and between the Classic and Vulkan renderer (if possible).\n"
        "Also a control to toggle which player is viewed when playing back multiplayer demos.",
        Toggle_Pause,
        PLATFORM_BINDING("Escape, P, Pause, Gamepad Start", "Gamepad Start")
    );

    // PsyDoom 3DS: unbound, because the touch screen shows the automap for the whole of gameplay.
    // SELECT is left free so it can be rebound to something the player actually wants.
    cfg.toggle_map = CONTROL_FIELD(Toggle_Map, PLATFORM_BINDING("Tab, M, Gamepad Back", ""));
    cfg.toggle_renderer = CONTROL_FIELD(Toggle_Renderer, PLATFORM_BINDING("`", ""));
    cfg.toggle_uncappedFps = CONTROL_FIELD(Toggle_UncappedFps, "");
    cfg.toggle_viewPlayer = CONTROL_FIELD(Toggle_ViewPlayer, PLATFORM_BINDING("V", ""));

    // Weapon switching
    cfg.weapon_scrollUp = CONTROL_FIELD_WITH_DOC(
        "Weapon switching: relative and absolute.\n"
        "\n"
        "Note that the weapon 'scrollUp/Down' actions work much better with the mouse wheel since they\n"
        "allow multiple weapons to be scrolled past in one frame. This helps rapid scrolling feel much\n"
        "more responsive.",
        Weapon_ScrollUp,
        PLATFORM_BINDING("Mouse Wheel+", "")
    );
    cfg.weapon_scrollDown = CONTROL_FIELD(Weapon_ScrollDown, PLATFORM_BINDING("Mouse Wheel-", ""));
    cfg.weapon_previous = CONTROL_FIELD(Weapon_Previous, PLATFORM_BINDING("PageDown, [, Gamepad LeftShoulder", "Gamepad LeftTrigger"));
    cfg.weapon_next = CONTROL_FIELD(Weapon_Next, PLATFORM_BINDING("PageUp, ], Gamepad RightShoulder", "Gamepad RightTrigger"));
    cfg.weapon_fistChainsaw = CONTROL_FIELD(Weapon_FistChainsaw, PLATFORM_BINDING("1", ""));
    cfg.weapon_pistol = CONTROL_FIELD(Weapon_Pistol, PLATFORM_BINDING("2", ""));
    cfg.weapon_shotgun = CONTROL_FIELD(Weapon_Shotgun, PLATFORM_BINDING("3", ""));
    cfg.weapon_superShotgun = CONTROL_FIELD(Weapon_SuperShotgun, PLATFORM_BINDING("4", ""));
    cfg.weapon_chaingun = CONTROL_FIELD(Weapon_Chaingun, PLATFORM_BINDING("5", ""));
    cfg.weapon_rocketLauncher = CONTROL_FIELD(Weapon_RocketLauncher, PLATFORM_BINDING("6", ""));
    cfg.weapon_plasmaRifle = CONTROL_FIELD(Weapon_PlasmaRifle, PLATFORM_BINDING("7", ""));
    cfg.weapon_bfg = CONTROL_FIELD(Weapon_BFG, PLATFORM_BINDING("8", ""));

    cfg.weapon_groupShotguns = CONTROL_FIELD_WITH_DOC(
        "Weapon group toggles. Each one picks between two weapons, preferring the first, and swaps to the second\n"
        "when the first is already in hand or is not owned. Weapons that are not owned are skipped.",
        Weapon_GroupShotguns,
        PLATFORM_BINDING("", "Gamepad DpUp")
    );

    cfg.weapon_groupHeavy = CONTROL_FIELD(Weapon_GroupHeavy, PLATFORM_BINDING("", "Gamepad DpDown"));
    cfg.weapon_groupRapid = CONTROL_FIELD(Weapon_GroupRapid, PLATFORM_BINDING("", "Gamepad DpRight"));
    cfg.weapon_groupEnergy = CONTROL_FIELD(Weapon_GroupEnergy, PLATFORM_BINDING("", "Gamepad DpLeft"));

    // Menu & UI controls
    cfg.menu_up = CONTROL_FIELD_WITH_DOC(
        "Menu & UI controls",
        Menu_Up,
        PLATFORM_BINDING("Up, Gamepad DpUp, Gamepad LeftY-, Gamepad RightY-", "Gamepad DpUp, Gamepad LeftY-")
    );

    cfg.menu_down = CONTROL_FIELD(Menu_Down, PLATFORM_BINDING("Down, Gamepad DpDown, Gamepad LeftY+, Gamepad RightY+", "Gamepad DpDown, Gamepad LeftY+"));
    cfg.menu_left = CONTROL_FIELD(Menu_Left, PLATFORM_BINDING("Left, Gamepad DpLeft, Gamepad LeftX-, Gamepad RightX-", "Gamepad DpLeft, Gamepad LeftX-"));
    cfg.menu_right = CONTROL_FIELD(Menu_Right, PLATFORM_BINDING("Right, Gamepad DpRight, Gamepad LeftX+, Gamepad RightX+", "Gamepad DpRight, Gamepad LeftX+"));
    cfg.menu_ok = CONTROL_FIELD(Menu_Ok, PLATFORM_BINDING("Return, Space, Mouse Left, Left Ctrl, Right Ctrl, Gamepad A, Gamepad RightTrigger", "Gamepad A"));
    cfg.menu_back = CONTROL_FIELD(Menu_Back, PLATFORM_BINDING("Escape, Tab, Mouse Right, Gamepad B, Gamepad Back", "Gamepad Back"));
    cfg.menu_start = CONTROL_FIELD(Menu_Start, "Gamepad Start");
    cfg.menu_enterPasswordChar = CONTROL_FIELD(Menu_EnterPasswordChar, PLATFORM_BINDING("Return, Space, Mouse Left, Left Ctrl, Right Ctrl, Gamepad A, Gamepad RightTrigger", "Gamepad A"));
    cfg.menu_deletePasswordChar = CONTROL_FIELD(Menu_DeletePasswordChar, PLATFORM_BINDING("Delete, Backspace, Mouse Right, Gamepad X, Gamepad LeftTrigger", "Gamepad Back"));

    // Automap controls
    cfg.automap_zoomIn = CONTROL_FIELD_WITH_DOC(
        "Automap controls",
        Automap_ZoomIn,
        PLATFORM_BINDING("=, Keypad +, Gamepad RightTrigger", "Gamepad RightTrigger, Gamepad RightShoulder")
    );

    cfg.automap_zoomOut = CONTROL_FIELD(Automap_ZoomOut, PLATFORM_BINDING("-, Keypad -, Gamepad LeftTrigger", "Gamepad LeftTrigger, Gamepad LeftShoulder"));
    cfg.automap_moveUp = CONTROL_FIELD(Automap_MoveUp, PLATFORM_BINDING("Up, W, Gamepad DpUp, Gamepad LeftY-, Gamepad RightY-", "Gamepad DpUp, Gamepad LeftY-, Gamepad RightY-"));
    cfg.automap_moveDown = CONTROL_FIELD(Automap_MoveDown, PLATFORM_BINDING("Down, S, Gamepad DpDown, Gamepad LeftY+, Gamepad RightY+", "Gamepad DpDown, Gamepad LeftY+, Gamepad RightY+"));
    cfg.automap_moveLeft = CONTROL_FIELD(Automap_MoveLeft, PLATFORM_BINDING("Left, A, Gamepad DpLeft, Gamepad LeftX-, Gamepad RightX-", "Gamepad DpLeft, Gamepad LeftX-, Gamepad RightX-"));
    cfg.automap_moveRight = CONTROL_FIELD(Automap_MoveRight, PLATFORM_BINDING("Right, D, Gamepad DpRight, Gamepad LeftX+, Gamepad RightX+", "Gamepad DpRight, Gamepad LeftX+, Gamepad RightX+"));
    cfg.automap_pan = CONTROL_FIELD(Automap_Pan, PLATFORM_BINDING("F, Left Alt, Right Alt, Gamepad A", "Gamepad A"));

    // PSX button bindings for cheat codes
    cfg.psxCheatCode_up = CONTROL_FIELD_WITH_DOC(
        "Mappings to the original PlayStation controller buttons for the sole purpose of entering cheat\n"
        "code sequences on the pause menu, the original way.\n"
        "\n"
        "For example inputs mapped to the PSX 'Cross' button will be interpreted as that while attempting\n"
        "to enter an original cheat code sequence.",
        PSXCheatCode_Up,
        PLATFORM_BINDING("Up, W, Gamepad DpUp, Gamepad LeftY-, Gamepad RightY-", "Gamepad DpUp, Gamepad LeftY-")
    );

    cfg.psxCheatCode_down = CONTROL_FIELD(PSXCheatCode_Down, PLATFORM_BINDING("Down, S, Gamepad DpDown, Gamepad LeftY+, Gamepad RightY+", "Gamepad DpDown, Gamepad LeftY+"));
    cfg.psxCheatCode_left = CONTROL_FIELD(PSXCheatCode_Left, PLATFORM_BINDING("Left, Gamepad DpLeft, Gamepad LeftX-, Gamepad RightX-", "Gamepad DpLeft, Gamepad LeftX-"));
    cfg.psxCheatCode_right = CONTROL_FIELD(PSXCheatCode_Right, PLATFORM_BINDING("Right, Gamepad DpRight, Gamepad LeftX+, Gamepad RightX+", "Gamepad DpRight, Gamepad LeftX+"));
    cfg.psxCheatCode_triangle = CONTROL_FIELD(PSXCheatCode_Triangle, PLATFORM_BINDING("Mouse Left, Left Ctrl, Right Ctrl, Gamepad Y", "Gamepad Y"));
    cfg.psxCheatCode_circle = CONTROL_FIELD(PSXCheatCode_Circle, PLATFORM_BINDING("Space, Mouse Right, Gamepad B", "Gamepad B"));
    cfg.psxCheatCode_cross = CONTROL_FIELD(PSXCheatCode_Cross, PLATFORM_BINDING("F, Left Alt, Right Alt, Gamepad A", "Gamepad A"));
    cfg.psxCheatCode_square = CONTROL_FIELD(PSXCheatCode_Square, PLATFORM_BINDING("Left Shift, Right Shift, Gamepad X", "Gamepad X"));
    cfg.psxCheatCode_l1 = CONTROL_FIELD(PSXCheatCode_L1, PLATFORM_BINDING("A, Gamepad LeftShoulder", "Gamepad LeftShoulder"));
    cfg.psxCheatCode_r1 = CONTROL_FIELD(PSXCheatCode_R1, PLATFORM_BINDING("D, Gamepad RightShoulder", "Gamepad RightShoulder"));
    cfg.psxCheatCode_l2 = CONTROL_FIELD(PSXCheatCode_L2, PLATFORM_BINDING("PageDown, [, Gamepad LeftTrigger", "Gamepad LeftTrigger"));
    cfg.psxCheatCode_r2 = CONTROL_FIELD(PSXCheatCode_R2, PLATFORM_BINDING("PageUp, ], Gamepad RightTrigger", "Gamepad RightTrigger"));

    // Done with these
    #undef PLATFORM_BINDING
    #undef CONTROL_FIELD
    #undef CONTROL_FIELD_WITH_DOC
}

END_NAMESPACE(ConfigSerialization)
