#pragma once

#if PSYDOOM_3DS

#include "Macros.h"

#include <cstdint>

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: named sets of control bindings.
//
// Bindings themselves live in 'control_bindings.ini' and can be edited by hand, but a handheld with no keyboard is a
// poor place to ask someone to do that. These are three ready made layouts, chosen from the controls screen, plus
// 'Custom' for whatever is in the file.
//
// While a preset is selected it is reapplied over the file on every launch, so the file simply follows the preset. The
// moment the file says something the preset does not, the player has edited it, and the selection becomes 'Custom' so
// their edits are kept rather than overwritten on the next launch.
//------------------------------------------------------------------------------------------------------------------------------------------
BEGIN_NAMESPACE(ControlSchemes3DS)

enum class Scheme : int32_t {
    Modern,         // Sticks for movement and turning, D-Pad for weapon groups: the Xbox 360 Doom II layout
    Playstation,    // The PlayStation layout: D-Pad moves and turns, face buttons as the original pad had them
    PlaystationAlt, // The PlayStation layout with use and strafe swapped between B and A
    Custom,         // Whatever is in 'control_bindings.ini'
    Count
};

static constexpr int32_t NUM_SCHEMES = (int32_t) Scheme::Count;

// The default, and what 'reset to default' goes back to
static constexpr Scheme DEFAULT_SCHEME = Scheme::Modern;

// Short name for the controls screen
const char* getSchemeName(const Scheme scheme) noexcept;

// Puts a scheme's bindings into effect. Does nothing for 'Custom', which by definition has no bindings of its own.
// Marks the bindings for saving, so the file is rewritten to match.
void applyScheme(const Scheme scheme) noexcept;

// Are the bindings currently in effect the ones this scheme defines?
// Used after loading to tell a scheme that is still intact from one the player has edited underneath.
bool bindingsMatchScheme(const Scheme scheme) noexcept;

// Resolves what should be in effect at startup, having loaded both the preferences and the bindings file.
// Returns the scheme that ended up selected, which is 'Custom' if the bindings no longer match the saved scheme.
Scheme resolveOnStartup(const Scheme savedScheme) noexcept;

END_NAMESPACE(ControlSchemes3DS)

#endif  // #if PSYDOOM_3DS
