#pragma once

#if PSYDOOM_3DS

#include "Doom/doomdef.h"

// A read only reference screen listing what each Nintendo 3DS control does.
//
// PsyDoom removed the original PlayStation 'Configuration' screen (see 'Old_cn_main.cpp'), which drew the eight
// bindable PSX pad buttons from the disc's 'BUTTONS' lump. This is the 3DS equivalent of that screen: the button
// glyphs are drawn from primitives in a 3DS style rather than sampled from the PlayStation artwork, so it works
// for all three game variants without patching any disc data.
void Controls3DS_Init() noexcept;
void Controls3DS_Shutdown(const gameaction_t exitAction) noexcept;
gameaction_t Controls3DS_Update() noexcept;
void Controls3DS_Draw() noexcept;

#endif  // #if PSYDOOM_3DS
