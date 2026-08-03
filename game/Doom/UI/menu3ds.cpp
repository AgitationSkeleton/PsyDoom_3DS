//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: shared helper for menus that compose each screen separately. See 'menu3ds.h'.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "menu3ds.h"

#if PSYDOOM_3DS

#include "Doom/Base/i_drawcmds.h"
#include "Doom/Base/i_main.h"
#include "Doom/Base/i_misc.h"
#include "Doom/Game/g_game.h"
#include "o_main.h"
#include "PsyDoom/Screens3DS.h"
#include "PsyDoom/Video.h"

void Menu3DS_DrawTitleScreen(texture_t& bgTex, const uint16_t bgClutId, const char* const title) noexcept {
    Screens3DS::setBottomScreen(Screens3DS::BottomScreen::MenuTwoPass);
    Screens3DS::setMenuTwoPassRows(Screens3DS::MENU_TOP_PASS_SRC_Y, Screens3DS::BOT_PASS_SRC_Y);

    // Note: done even while the menu is on its way out. The caller redraws the frame for the touch screen either way,
    // so skipping this would leave the top screen to be taken from whatever that second pass left behind - which shows
    // up as the touch screen's contents appearing on the top screen on the way out of a menu.
    O_DrawBackground(bgTex, bgClutId, 128, 128, 128);

    // Centre the title in the rows the top screen presents. The big font is 16 pixels tall.
    constexpr int32_t TITLE_H = 16;
    constexpr int32_t TITLE_Y = Screens3DS::MENU_TOP_PASS_SRC_Y + (Screens3DS::MENU_TOP_PASS_ROWS - TITLE_H) / 2;

    I_DrawString(-1, TITLE_Y, title);

    I_SubmitGpuCmds();
    Video::presentTopScreenOnly();
    Screens3DS::setTopScreenAlreadyDrawn(true);
}

#endif  // #if PSYDOOM_3DS
