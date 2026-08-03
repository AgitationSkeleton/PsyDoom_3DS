#pragma once

#if PSYDOOM_3DS

#include <cstdint>

struct texture_t;

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: shared helper for menus that compose each screen separately.
//
// A menu drawn once and cut in half has the two screens sharing a seam, so a cursor or a letter on the boundary is
// sliced across both, and neither screen gets a background of its own. Instead a menu draws itself twice: this puts the
// background and the title on the top screen and banks it, then the menu carries on and draws the background and its
// items normally for the touch screen.
//
// Call this first thing in the menu's drawer, then skip drawing the title in the menu's own pass.
//------------------------------------------------------------------------------------------------------------------------------------------
void Menu3DS_DrawTitleScreen(texture_t& bgTex, const uint16_t bgClutId, const char* const title) noexcept;

// Where a two pass menu should lay its items out: the rows the touch screen presents.
// Items must fit inside this, including the two pixels the cursor sits above its row.
static constexpr int32_t MENU3DS_ITEMS_MIN_Y = 28;
static constexpr int32_t MENU3DS_ITEMS_MAX_Y = 214;

#endif  // #if PSYDOOM_3DS
