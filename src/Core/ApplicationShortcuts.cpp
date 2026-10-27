// Keyboard shortcuts for the commands in ApplicationCommands/Editing/Nav/etc. Kept out of the big
// handleEvents else-if chain, which hit the compiler's nesting limit.
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include <SDL2/SDL.h>
#include <algorithm>

#ifdef None
#undef None
#endif

bool Application::handleCommandShortcut(SDL_Keycode sym, Uint16 mod) {
    bool ctrl = (mod & KMOD_CTRL) != 0, shift = (mod & KMOD_SHIFT) != 0;
    if ((mod & KMOD_ALT) && shift && sym == SDLK_w) { wrapSelectionWithTag(); return true; }
    if ((mod & KMOD_ALT) && shift && sym == SDLK_p) { openRecentProjectPicker(); return true; }
    if ((mod & KMOD_ALT) && shift && sym >= SDLK_1 && sym <= SDLK_4) { setLayoutColumns(static_cast<size_t>(sym - SDLK_1 + 1)); return true; }
    if (ctrl && shift && !(mod & KMOD_ALT) && sym >= SDLK_1 && sym <= SDLK_4) { moveFileToGroup(static_cast<size_t>(sym - SDLK_1)); return true; }
    if (ctrl && !shift && !(mod & KMOD_ALT) && sym >= SDLK_1 && sym <= SDLK_4) { focusGroup(static_cast<size_t>(sym - SDLK_1)); return true; }
    if (selections_.size() > 1 && !ctrl && !(mod & KMOD_ALT) && (sym == SDLK_LEFT || sym == SDLK_RIGHT || sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_HOME || sym == SDLK_END) && !acActive_) { moveAllCursors(sym, shift); return true; }
    if (selections_.size() > 1 && !ctrl && sym == SDLK_BACKSPACE) { deleteMulti(false); return true; }
    if (selections_.size() > 1 && !ctrl && sym == SDLK_DELETE) { deleteMulti(true); return true; }
    if (sym == SDLK_F7 || (ctrl && !shift && sym == SDLK_b)) { runBuild(); return true; }
    if (ctrl && shift && sym == SDLK_b) { openBuildPicker(); return true; }
    if (ctrl && sym == SDLK_PAUSE) { cancelBuild(); return true; }
    if (sym == SDLK_F12 && shift) { gotoReference(); return true; }
    if (sym == SDLK_F12) { gotoDefinition(); return true; }
    if (ctrl && shift && sym == SDLK_r) { commandGotoSymbolInProject(); return true; }
    if (ctrl && sym == SDLK_F3) { findUnder(!shift); return true; }
    if (ctrl && shift && sym == SDLK_w) { closeWindow(); return true; }
    if (ctrl && shift && sym == SDLK_h) { replaceNext(); return true; }
    if (sym == SDLK_F6 && ctrl) { { if (shift) previousMisspelling(); else nextMisspelling(); } return true; }
    if (sym == SDLK_F6) { toggleSpellCheck(); return true; }
    if (ctrl && !shift && !(mod & KMOD_ALT) && sym == SDLK_u) { softUndo(); return true; }
    if (ctrl && shift && sym == SDLK_u) { softRedo(); return true; }
    if ((mod & KMOD_ALT) && !ctrl && sym == SDLK_PERIOD) { closeTag(); return true; }
    if (ctrl && !shift && !(mod & KMOD_ALT) && sym == SDLK_PERIOD) { nextModification(); return true; }
    if (ctrl && !shift && !(mod & KMOD_ALT) && sym == SDLK_COMMA) { previousModification(); return true; }
    if ((mod & KMOD_ALT) && !ctrl && !shift && sym == SDLK_o) { switchHeaderImplementation(); return true; }
    if (ctrl && !shift && sym == SDLK_e) { useSelectionForFind(); return true; }
    if (ctrl && shift && sym == SDLK_e) { useSelectionForReplace(); return true; }
    if (ctrl && sym == SDLK_PAGEDOWN) { nextTab(); return true; }
    if (ctrl && sym == SDLK_PAGEUP) { previousTab(); return true; }
    if (ctrl && sym == SDLK_TAB) { { if (shift) previousTab(); else nextTab(); } return true; }
    if (ctrl && sym == SDLK_F4) { closeCurrentTab(); return true; }
    if ((mod & KMOD_ALT) && !ctrl && sym >= SDLK_1 && sym <= SDLK_8) { selectTabByIndex(static_cast<size_t>(sym - SDLK_1)); return true; }
    if ((mod & KMOD_ALT) && !ctrl && sym == SDLK_9) { selectLastTab(); return true; }
    if ((mod & KMOD_ALT) && !ctrl && sym == SDLK_q) { wrapParagraph(80); return true; }
    if (ctrl && !shift && sym == SDLK_m) { jumpToMatchingBracket(); return true; }
    if (ctrl && !shift && sym == SDLK_SPACE) { showCompletions(); return true; }
    if (ctrl && sym == SDLK_d) { expandSelectionToWord(); return true; }
    if (ctrl && shift && sym == SDLK_l) { splitSelectionIntoLines(); return true; }
    if (ctrl && sym == SDLK_l) { expandSelectionToLine(); return true; }
    if (ctrl && shift && sym == SDLK_m) { expandSelectionToBrackets(); return true; }
    if (ctrl && shift && sym == SDLK_SPACE) { expandSelectionToScope(); return true; }
    if (ctrl && shift && sym == SDLK_f) { commandFindInFiles(); return true; }
    if (ctrl && shift && sym == SDLK_n) { newWindow(); return true; }
    if (ctrl && shift && sym == SDLK_RETURN) { insertLineBefore(); return true; }
    if (ctrl && sym == SDLK_RETURN) { insertLineAfter(); return true; }
    if (ctrl && sym == SDLK_DELETE) { deleteWordForward(); return true; }
    if (ctrl && sym == SDLK_BACKSPACE) { deleteWordBackward(); return true; }
    if (ctrl && sym == SDLK_t) { transposeCharacters(); return true; }
    if (sym == SDLK_F9) { sortLines(ctrl); return true; }
    if (ctrl && !(mod & KMOD_ALT) && sym == SDLK_EQUALS) { changeFontSize(1); return true; }
    if (ctrl && !(mod & KMOD_ALT) && sym == SDLK_MINUS) { changeFontSize(-1); return true; }
    if (ctrl && sym == SDLK_RIGHTBRACKET) { indentSelection(); return true; }
    if (ctrl && sym == SDLK_LEFTBRACKET) { unindentSelection(); return true; }
    if (ctrl && sym == SDLK_g) { commandGotoLine(); return true; }
    if (ctrl && sym == SDLK_r) { commandGotoSymbol(); return true; }
    if (ctrl && sym == SDLK_SEMICOLON) { commandGotoWord(); return true; }
    if (sym == SDLK_F4 && shift) { previousResult(); return true; }
    if (sym == SDLK_F4 && !ctrl) { nextResult(); return true; }
    if (sym == SDLK_F3 && (mod & KMOD_ALT)) { findAllUnder(); return true; }
    if (sym == SDLK_F3 && shift) { findPrevious(); return true; }
    if (sym == SDLK_F3) { findNext(); return true; }
    if (sym == SDLK_F11 && shift) { toggleDistractionFree(); return true; }
    if ((mod & KMOD_ALT) && sym == SDLK_MINUS) { { if (shift) jumpForward(); else jumpBack(); } return true; }
    if (ctrl && !shift && !(mod & KMOD_ALT) && sym == SDLK_UP) { scrollY_ = std::max(0.f, scrollY_ - fontAtlas().lineHeight()); return true; }
    if (ctrl && !shift && !(mod & KMOD_ALT) && sym == SDLK_DOWN) { scrollY_ += fontAtlas().lineHeight(); return true; }
    return false;
}
