#include "UI/Titlebar.h"
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "icon_data.h"
#include <GL/glew.h>
#include <algorithm>
#include <cstdio>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

void Titlebar::buildMenu() {
    menuItems_ = {
        {"File", ">", nullptr, false, 0},
        {"Edit", ">", nullptr, false, 1},
        {"Selection", ">", nullptr, false, 2},
        {"Find", ">", nullptr, false, 3},
        {"View", ">", nullptr, false, 4},
        {"Goto", ">", nullptr, false, 5},
        {"Tools", ">", nullptr, false, 6},
        {"Project", ">", nullptr, false, 8},
        {"Preferences", ">", nullptr, false, 7},
        {"Help", ">", nullptr, false, 9},
    };
    submenus_ = {
        {{"New File","Ctrl+N",[]{ Application::instance().newBuffer(); }},{"New Window","Ctrl+Shift+N",[]{ Application::instance().newWindow(); }},{"Open File...","Ctrl+O",[]{ Application::instance().openFileDialog(); }},{"Open Folder...","",[]{ Application::instance().openFolderDialog(); }},{"Open Recent",">",nullptr},{"","",nullptr,true},{"Save","Ctrl+S",[]{ Application::instance().saveFile(); }},{"Save As...","Ctrl+Shift+S",[]{ Application::instance().saveFileAs(); }},{"Save All","",[]{ Application::instance().saveAll(); }},{"","",nullptr,true},{"Toggle Bold Text","",[]{ Application::instance().toggleBoldText(); }},{"","",nullptr,true},{"Close File","Ctrl+W",[]{ Application::instance().closeCurrentTab(); }},{"Split View","",[]{ Application::instance().splitView(); }},{"Close All Files","",[]{ Application::instance().closeAllFiles(); }},{"","",nullptr,true},{"Reopen with Encoding...","",[]{ Application::instance().openReopenEncodingPicker(); }},{"Save with Encoding...","",[]{ Application::instance().openSaveEncodingPicker(); }},{"Print...","",[]{ Application::instance().printDocument(); }},{"Close Window","Ctrl+Shift+W",[]{ Application::instance().closeWindow(); }},{"Reopen Closed Tab","Ctrl+Shift+T",[]{ Application::instance().reopenClosedTab(); }},{"Revert File","",[]{ Application::instance().revertFile(); }},{"","",nullptr,true},{"Exit","Alt+F4",[]{ Application::instance().quit(); }}},
        {{"Undo","Ctrl+Z",[]{ Application::instance().commandUndo(); }},{"Redo","Ctrl+Y",[]{ Application::instance().commandRedo(); }},{"","",nullptr,true},{"Cut","Ctrl+X",[]{ Application::instance().commandCut(); }},{"Copy","Ctrl+C",[]{ Application::instance().commandCopy(); }},{"Paste","Ctrl+V",[]{ Application::instance().commandPaste(); }},{"Paste and Indent","Ctrl+Shift+V",[]{ Application::instance().commandPasteAndIndent(); }},{"","",nullptr,true},{"Select All","Ctrl+A",[]{ Application::instance().commandSelectAll(); }},{"","",nullptr,true},{"Indent Line","Ctrl+]",[]{ Application::instance().indentSelection(); }},{"Unindent Line","Ctrl+[",[]{ Application::instance().unindentSelection(); }},{"","",nullptr,true},{"Swap Line Up","Ctrl+Shift+Up",[]{ Application::instance().swapLineUp(); }},{"Swap Line Down","Ctrl+Shift+Down",[]{ Application::instance().swapLineDown(); }},{"Duplicate Line","Ctrl+Shift+D",[]{ Application::instance().duplicateLine(); }},{"Delete Line","Ctrl+Shift+K",[]{ Application::instance().deleteLine(); }},{"Join Lines","Ctrl+Shift+J",[]{ Application::instance().joinLines(); }},{"","",nullptr,true},{"Insert Line Before","Ctrl+Shift+Enter",[]{ Application::instance().insertLineBefore(); }},{"Insert Line After","Ctrl+Enter",[]{ Application::instance().insertLineAfter(); }},{"Delete Word Forward","Ctrl+Delete",[]{ Application::instance().deleteWordForward(); }},{"Delete Word Backward","Ctrl+Backspace",[]{ Application::instance().deleteWordBackward(); }},{"Delete to End","Ctrl+K Ctrl+K",[]{ Application::instance().deleteToLineEnd(); }},{"Delete to Beginning","Ctrl+K Ctrl+Backspace",[]{ Application::instance().deleteToLineStart(); }},{"Transpose","Ctrl+T",[]{ Application::instance().transposeCharacters(); }},{"","",nullptr,true},{"Sort Lines","F9",[]{ Application::instance().sortLines(false); }},{"Sort Lines (Case Sensitive)","Ctrl+F9",[]{ Application::instance().sortLines(true); }},{"Reverse Lines","",[]{ Application::instance().reverseLines(); }},{"Unique Lines","",[]{ Application::instance().uniqueLines(); }},{"","",nullptr,true},{"Fold","Ctrl+Shift+[",[]{ Application::instance().foldCurrent(); }},{"Unfold","Ctrl+Shift+]",[]{ Application::instance().unfoldCurrent(); }},{"Fold All","Ctrl+K Ctrl+1",[]{ Application::instance().commandFoldAll(); }},{"Unfold All","Ctrl+K Ctrl+J",[]{ Application::instance().commandUnfoldAll(); }},{"Fold Level 2","Ctrl+K Ctrl+2",[]{ Application::instance().foldByLevel(2); }},{"Fold Level 3","Ctrl+K Ctrl+3",[]{ Application::instance().foldByLevel(3); }},{"","",nullptr,true},{"Set Mark","Ctrl+K Ctrl+Space",[]{ Application::instance().setMark(); }},{"Select to Mark","Ctrl+K Ctrl+A",[]{ Application::instance().selectToMark(); }},{"Delete to Mark","Ctrl+K Ctrl+W",[]{ Application::instance().deleteToMark(); }},{"Swap with Mark","Ctrl+K Ctrl+X",[]{ Application::instance().swapWithMark(); }},{"Yank","Ctrl+K Ctrl+Y",[]{ Application::instance().yank(); }},{"","",nullptr,true},{"Wrap Paragraph","Alt+Q",[]{ Application::instance().wrapParagraph(80); }},{"Wrap Paragraph at 70 Characters","",[]{ Application::instance().wrapParagraph(70); }},{"Wrap Paragraph at 72 Characters","",[]{ Application::instance().wrapParagraph(72); }},{"Wrap Paragraph at 78 Characters","",[]{ Application::instance().wrapParagraph(78); }},{"Wrap Paragraph at 100 Characters","",[]{ Application::instance().wrapParagraph(100); }},{"Wrap Paragraph at 120 Characters","",[]{ Application::instance().wrapParagraph(120); }},{"","",nullptr,true},{"Show Completions","Ctrl+Space",[]{ Application::instance().showCompletions(); }},{"Wrap Selection With Tag","Alt+Shift+W",[]{ Application::instance().wrapSelectionWithTag(); }},{"","",nullptr,true},{"Toggle Line Comment","Ctrl+/",[]{ Application::instance().toggleLineComment(); }},{"Toggle Block Comment","Ctrl+Shift+/",[]{ Application::instance().toggleBlockComment(); }},{"","",nullptr,true},{"Upper Case","Ctrl+K Ctrl+U",[]{ Application::instance().convertCaseUpper(); }},{"Lower Case","Ctrl+K Ctrl+L",[]{ Application::instance().convertCaseLower(); }},{"Title Case","",[]{ Application::instance().convertCaseTitle(); }},{"Swap Case","",[]{ Application::instance().convertCaseSwap(); }},{"Copy as HTML","",[]{ Application::instance().copyAsHtml(); }},{"lowerCamelCase","",[]{ Application::instance().convertIdentifierCase(0); }},{"UpperCamelCase","",[]{ Application::instance().convertIdentifierCase(1); }},{"snake_case","",[]{ Application::instance().convertIdentifierCase(2); }},{"kebab-case","",[]{ Application::instance().convertIdentifierCase(3); }},{"","",nullptr,true},{"Undo Selection","Ctrl+U",[]{ Application::instance().softUndo(); }},{"Redo Selection","Ctrl+Shift+U",[]{ Application::instance().softRedo(); }},{"Paste from History","Ctrl+K Ctrl+V",[]{ Application::instance().openPasteHistory(); }},{"Reindent","Ctrl+K Ctrl+]",[]{ Application::instance().reindent(); }},{"Close Tag","Alt+.",[]{ Application::instance().closeTag(); }},{"","",nullptr,true},{"Shuffle Lines","",[]{ Application::instance().shuffleLines(); }},{"Revert Hunk","Ctrl+K Ctrl+Z",[]{ Application::instance().revertHunk(); }},{"Revert All Modifications","Ctrl+K Ctrl+Shift+Z",[]{ Application::instance().revertAllModifications(); }},{"Selections: Sort","",[]{ Application::instance().permuteSelections(0); }},{"Selections: Sort (Case Sensitive)","",[]{ Application::instance().permuteSelections(1); }},{"Selections: Reverse","",[]{ Application::instance().permuteSelections(2); }},{"Selections: Unique","",[]{ Application::instance().permuteSelections(3); }},{"Selections: Shuffle","",[]{ Application::instance().permuteSelections(4); }}},
        {{"Select All","Ctrl+A",[]{ Application::instance().commandSelectAll(); }},{"Expand Selection to Line","Ctrl+L",[]{ Application::instance().expandSelectionToLine(); }},{"Expand Selection to Word","Ctrl+D",[]{ Application::instance().expandSelectionToWord(); }},{"Expand Selection to Brackets","Ctrl+Shift+M",[]{ Application::instance().expandSelectionToBrackets(); }},{"Expand Selection to Indentation","",[]{ Application::instance().expandSelectionToIndentation(); }},{"Expand Selection to Scope","Ctrl+Shift+Space",[]{ Application::instance().expandSelectionToScope(); }},{"","",nullptr,true},{"Split into Lines","Ctrl+Shift+L",[]{ Application::instance().splitSelectionIntoLines(); }},{"Single Selection","Escape",[]{ Application::instance().singleSelection(); }},{"","",nullptr,true},{"Add Previous Line","Ctrl+Alt+Up",[]{ Application::instance().addPreviousLine(); }},{"Add Next Line","Ctrl+Alt+Down",[]{ Application::instance().addNextLine(); }}},
        {{"Find...","Ctrl+F",[]{ Application::instance().commandFind(); }},{"Find Next","F3",[]{ Application::instance().findNext(); }},{"Find Previous","Shift+F3",[]{ Application::instance().findPrevious(); }},{"Find All","Alt+F3",[]{ Application::instance().findAllSelect(); }},{"","",nullptr,true},{"Replace...","Ctrl+H",[]{ Application::instance().commandReplace(); }},{"","",nullptr,true},{"Replace Next","Ctrl+Shift+H",[]{ Application::instance().replaceNext(); }},{"","",nullptr,true},{"Quick Find","Ctrl+F3",[]{ Application::instance().findUnder(true); }},{"Quick Find All","Alt+F3",[]{ Application::instance().findAllUnder(); }},{"Quick Add Next","Ctrl+D",[]{ Application::instance().expandSelectionToWord(); }},{"Quick Skip Next","Ctrl+K Ctrl+D",[]{ Application::instance().quickSkipNext(); }},{"","",nullptr,true},{"Use Selection for Find","Ctrl+E",[]{ Application::instance().useSelectionForFind(); }},{"Use Selection for Replace","Ctrl+Shift+E",[]{ Application::instance().useSelectionForReplace(); }},{"","",nullptr,true},{"Find in Files...","Ctrl+Shift+F",[]{ Application::instance().commandFindInFiles(); }},{"Next Result","F4",[]{ Application::instance().nextResult(); }},{"Previous Result","Shift+F4",[]{ Application::instance().previousResult(); }}},
        {{"Toggle Side Bar","Ctrl+K Ctrl+B",[]{ Application::instance().toggleSidebar(); }},{"Toggle Minimap","",[]{ Application::instance().toggleMinimap(); }},{"Hide Tabs","",[]{ Application::instance().toggleTabs(); }},{"Hide Status Bar","",[]{ Application::instance().toggleStatusBar(); }},{"","",nullptr,true},{"Show Console","Ctrl+`",[]{ Application::instance().toggleConsole(); }},{"","",nullptr,true},{"Word Wrap","Alt+Z",[]{ Application::instance().toggleWordWrap(); }},{"","",nullptr,true},{"Enter Full Screen","F11",[]{ Application::instance().toggleFullscreen(); }},{"Enter Distraction Free Mode","Shift+F11",[]{ Application::instance().toggleDistractionFree(); }},{"","",nullptr,true},{"Syntax...","",[]{ Application::instance().openSyntaxPicker(); }},{"Layout: Single","Alt+Shift+1",[]{ Application::instance().setLayoutColumns(1); }},{"Layout: Columns 2","Alt+Shift+2",[]{ Application::instance().setLayoutColumns(2); }},{"Layout: Columns 3","Alt+Shift+3",[]{ Application::instance().setLayoutColumns(3); }},{"Layout: Columns 4","Alt+Shift+4",[]{ Application::instance().setLayoutColumns(4); }},{"","",nullptr,true},{"New Group","Ctrl+K Ctrl+Up",[]{ Application::instance().newGroup(); }},{"Close Group","Ctrl+K Ctrl+Down",[]{ Application::instance().closeGroup(); }},{"Focus Next Group","Ctrl+K Ctrl+Right",[]{ Application::instance().focusNeighboringGroup(true); }},{"Focus Previous Group","Ctrl+K Ctrl+Left",[]{ Application::instance().focusNeighboringGroup(false); }},{"Move File to Next Group","Ctrl+K Ctrl+Shift+Right",[]{ Application::instance().moveFileToNeighboringGroup(true); }},{"Move File to Previous Group","Ctrl+K Ctrl+Shift+Left",[]{ Application::instance().moveFileToNeighboringGroup(false); }},{"","",nullptr,true},{"Indentation...","",[]{ Application::instance().openIndentationPicker(); }},{"","",nullptr,true},{"Line Endings: Windows","",[]{ Application::instance().convertLineEndings(true); }},{"Line Endings: Unix","",[]{ Application::instance().convertLineEndings(false); }},{"","",nullptr,true},{"Ruler: None","",[]{ Application::instance().setRuler(0); }},{"Ruler: 70","",[]{ Application::instance().setRuler(70); }},{"Ruler: 72","",[]{ Application::instance().setRuler(72); }},{"Ruler: 78","",[]{ Application::instance().setRuler(78); }},{"Ruler: 80","",[]{ Application::instance().setRuler(80); }},{"Ruler: 100","",[]{ Application::instance().setRuler(100); }},{"Ruler: 120","",[]{ Application::instance().setRuler(120); }},{"","",nullptr,true},{"Spell Check","F6",[]{ Application::instance().toggleSpellCheck(); }},{"Next Misspelling","Ctrl+F6",[]{ Application::instance().nextMisspelling(); }},{"Previous Misspelling","Ctrl+Shift+F6",[]{ Application::instance().previousMisspelling(); }},{"Spelling Suggestions...","",[]{ Application::instance().openSpellSuggestions(); }},{"Add Word to Dictionary","",[]{ Application::instance().addWordToDictionary(); }},{"Dictionary...","",[]{ Application::instance().openDictionaryPicker(); }},{"","",nullptr,true},{"Word Wrap Column: Automatic","",[]{ Application::instance().setWrapColumn(0); }},{"Word Wrap Column: 70","",[]{ Application::instance().setWrapColumn(70); }},{"Word Wrap Column: 72","",[]{ Application::instance().setWrapColumn(72); }},{"Word Wrap Column: 78","",[]{ Application::instance().setWrapColumn(78); }},{"Word Wrap Column: 80","",[]{ Application::instance().setWrapColumn(80); }},{"Word Wrap Column: 100","",[]{ Application::instance().setWrapColumn(100); }},{"Word Wrap Column: 120","",[]{ Application::instance().setWrapColumn(120); }}},
        {{"Goto Anything...","Ctrl+P",[]{ Application::instance().commandGotoAnything(); }},{"Goto Symbol...","Ctrl+R",[]{ Application::instance().commandGotoSymbol(); }},{"Goto Line...","Ctrl+G",[]{ Application::instance().commandGotoLine(); }},{"Goto Word...","Ctrl+;",[]{ Application::instance().commandGotoWord(); }},{"","",nullptr,true},{"Jump Back","Alt+-",[]{ Application::instance().jumpBack(); }},{"Jump Forward","Alt+Shift+-",[]{ Application::instance().jumpForward(); }},{"","",nullptr,true},{"Next Bookmark","F2",[]{ Application::instance().nextBookmark(); }},{"Previous Bookmark","Shift+F2",[]{ Application::instance().prevBookmark(); }},{"Toggle Bookmark","Ctrl+F2",[]{ Application::instance().toggleBookmark(); }},{"Clear All Bookmarks","Ctrl+Shift+F2",[]{ Application::instance().clearBookmarks(); }},{"Next Modification","Ctrl+.",[]{ Application::instance().nextModification(); }},{"Previous Modification","Ctrl+,",[]{ Application::instance().previousModification(); }},{"","",nullptr,true},{"Goto Symbol in Project...","Ctrl+Shift+R",[]{ Application::instance().commandGotoSymbolInProject(); }},{"Goto Definition...","F12",[]{ Application::instance().gotoDefinition(); }},{"Goto Reference...","Shift+F12",[]{ Application::instance().gotoReference(); }},{"","",nullptr,true},{"Jump to Matching Bracket","Ctrl+M",[]{ Application::instance().jumpToMatchingBracket(); }},{"Scroll to Selection","Ctrl+K Ctrl+C",[]{ Application::instance().showAtCenter(); }},{"","",nullptr,true},{"Next File","Ctrl+PageDown",[]{ Application::instance().nextTab(); }},{"Previous File","Ctrl+PageUp",[]{ Application::instance().previousTab(); }},{"Switch Header/Implementation","Alt+O",[]{ Application::instance().switchHeaderImplementation(); }}},
        {{"Command Palette...","Ctrl+Shift+P",[]{ Application::instance().commandPalette(); }},{"Snippets...","",[]{ Application::instance().openSnippetPicker(); }},{"","",nullptr,true},{"New Plugin...","",[]{ Application::instance().newPlugin(); }},{"New Snippet...","",[]{ Application::instance().newSnippet(); }},{"New Syntax...","",[]{ Application::instance().newSyntax(); }},{"New Build System...","",[]{ Application::instance().newBuildSystem(); }},{"View Package File...","",[]{ Application::instance().openPackageFilePicker(); }},{"","",nullptr,true},{"Build","Ctrl+B",[]{ Application::instance().runBuild(); }},{"Build With...","Ctrl+Shift+B",[]{ Application::instance().openBuildPicker(); }},{"Cancel Build","Ctrl+Break",[]{ Application::instance().cancelBuild(); }},{"","",nullptr,true},{"Record Macro","Ctrl+Q",[]{ Application::instance().toggleMacroRecording(); }},{"Playback Macro","Ctrl+Shift+Q",[]{ Application::instance().playMacro(); }},{"Save Macro","",[]{ Application::instance().saveMacro(); }}},
        {{"Settings","",[]{ Application::instance().openSettingsFile(); }},{"Import Vim Settings","",[]{ Application::instance().importVimSettings(false); }},{"Import Neovim Settings","",[]{ Application::instance().importVimSettings(true); }},{"","",nullptr,true},{"Key Bindings","",[]{ Application::instance().openKeyBindingsFile(); }},{"Mouse Bindings","",[]{ Application::instance().openMouseBindings(); }},{"Browse Packages...","",[]{ Application::instance().browsePackages(); }},{"Select Color Scheme...","",[]{ Application::instance().openColorSchemePicker(); }},{"Customize Color Scheme","",[]{ Application::instance().customizeColorScheme(); }},{"Increase Font Size","Ctrl+=",[]{ Application::instance().changeFontSize(1); }},{"Decrease Font Size","Ctrl+-",[]{ Application::instance().changeFontSize(-1); }},{"Reset Font Size","",[]{ Application::instance().resetFontSize(); }}},
        {{"Open Project...","",[]{ Application::instance().openProject(); }},{"Switch Project...","Alt+Shift+P",[]{ Application::instance().openRecentProjectPicker(); }},{"Save Project As...","",[]{ Application::instance().saveProjectAs(); }},{"Close Project","",[]{ Application::instance().closeProject(); }},{"Edit Project","",[]{ Application::instance().editProject(); }},{"","",nullptr,true},{"Add Folder to Project...","",[]{ Application::instance().openFolderDialog(); }},{"Remove all Folders from Project","",[]{ Application::instance().closeFolder(); }},{"Refresh Folders","",[]{ Application::instance().refreshFolder(); }}},
        {{"Documentation","",[]{ Application::instance().openDocumentation(); }},{"Report a Bug","",[]{ Application::instance().reportBug(); }},{"","",nullptr,true},{"About Moreno Text","",[]{ Application::instance().showAbout(); }}},
    };
}

static void flushSolid(std::vector<float>& v) {
    if (v.empty()) return;
    GLRenderer::setDrawMode(2);
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 8));
    glBindVertexArray(0);
    GLRenderer::setDrawMode(0);
    v.clear();
}

void Titlebar::getMenuBounds(float& x, float& y, float& w, float& h) const {
    x = 0.f;
    y = height_;
    float itemH = 24.f;
    w = 150.f;
    h = 2.f + static_cast<float>(menuItems_.size()) * itemH;
    if (submenuOpen_ >= 0 && submenuOpen_ < static_cast<int>(submenus_.size())) {
        w = 480.f;
        float sy = 2.f + static_cast<float>(submenuOpen_) * itemH;
        float sh = sy + 2.f + static_cast<float>(submenus_[submenuOpen_].size()) * itemH;
        if (sh > h) h = sh;
    }
}

void Titlebar::getMenuPopupBounds(FontAtlas& font, float& x, float& y, float& w, float& h) const {
    SDL_Rect mainRect{}, submenuRect{}; bool hasSubmenu = false;
    getMenuPanelRects(font, 32000, 32000, mainRect, submenuRect, hasSubmenu);
    int minX = mainRect.x, minY = mainRect.y, maxX = mainRect.x + mainRect.w, maxY = mainRect.y + mainRect.h;
    if (hasSubmenu) {
        minX = std::min(minX, submenuRect.x);
        minY = std::min(minY, submenuRect.y);
        maxX = std::max(maxX, submenuRect.x + submenuRect.w);
        maxY = std::max(maxY, submenuRect.y + submenuRect.h);
        if (submenuOpen_ == 0 && submenuHovered_ == 3) {
            auto& recent = Application::instance().recentFiles();
            int count = std::max(1, static_cast<int>(recent.size()));
            int rw = 360;
            for (auto& path : recent) rw = std::max(rw, static_cast<int>(font.measureText(path) + 28.f));
            int rh = 2 + count * 24 + 1;
            int rx = submenuRect.x + submenuRect.w + 2;
            int ry = submenuRect.y + 2 + 3 * 24;
            if (rx + rw > 32000 - 4) rx = submenuRect.x - rw - 2;
            minX = std::min(minX, rx);
            minY = std::min(minY, ry);
            maxX = std::max(maxX, rx + rw);
            maxY = std::max(maxY, ry + rh);
        }
    }
    x = static_cast<float>(minX) - 3.f;
    y = height_ + static_cast<float>(minY) - 3.f;
    w = static_cast<float>(maxX - minX) + 6.f;
    h = static_cast<float>(maxY - minY) + 6.f;
}

void Titlebar::getMenuPanelRects(FontAtlas& font, int windowW, int windowH, SDL_Rect& mainRect, SDL_Rect& submenuRect, bool& hasSubmenu) const {
    float maxW = 140.f;
    for (auto& item : menuItems_) {
        float w = font.measureText(item.label);
        if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 24.f;
        if (w + 32.f > maxW) maxW = w + 32.f;
    }
    float itemH = 24.f;
    float ddW = maxW, ddH = 2.f + static_cast<float>(menuItems_.size()) * itemH;
    mainRect = {0, 0, static_cast<int>(ddW + 1.f), static_cast<int>(ddH + 1.f)};
    hasSubmenu = submenuOpen_ >= 0 && submenuOpen_ < static_cast<int>(submenus_.size());
    submenuRect = {0, 0, 0, 0};
    if (!hasSubmenu) return;
    auto& sm = submenus_[submenuOpen_];
    float sw = 180.f;
    for (auto& item : sm) {
        float w = font.measureText(item.label);
        if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 36.f;
        if (w + 48.f > sw) sw = w + 48.f;
    }
    float sx = ddW + 2.f, sy = 2.f + submenuOpen_ * itemH;
    float maxH = windowH * 0.8f, sh = 2.f + sm.size() * itemH;
    if (sy + sh > windowH - 8.f) sy = windowH - sh - 8.f;
    if (sh > maxH) sh = maxH;
    if (windowW < 30000 && sx + sw > windowW - 4.f) sx = -sw - 2.f;
    submenuRect = {static_cast<int>(sx), static_cast<int>(sy), static_cast<int>(sw + 1.f), static_cast<int>(sh + 1.f)};
}

void Titlebar::drawMenu(FontAtlas& font) {
    if (!menuOpen_) return;
    if (menuDeferred_) { menuDeferred_ = false; return; }
    drawMenuPopup(font, 0.f, 0.f);
}

void Titlebar::drawMenuPopup(FontAtlas& font, float ox, float oy) {
    if (!menuOpen_) return;
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    int ww = 1280, wh = 720;
    if (window) SDL_GetWindowSize(window, &ww, &wh);
    float maxW = 140.f;
    for (auto& item : menuItems_) {
        float w = font.measureText(item.label);
        if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 24.f;
        if (w + 32.f > maxW) maxW = w + 32.f;
    }
    float ddX = -ox, ddY = height_ - oy;
    float itemH = 24.f;
    int visibleCount = (int)menuItems_.size();
    float arrowH = 0.f;
    float ddW = maxW, ddH = 2.f + visibleCount * itemH + arrowH * 2.f;
    lastMenuX_ = ddX + ox; lastMenuY_ = ddY + oy; lastMenuW_ = ddW; lastMenuH_ = ddH;
    lastHasSubmenu_ = false; lastSubmenuW_ = 0.f; lastSubmenuH_ = 0.f;
    lastHasRecent_ = false; lastRecentW_ = 0.f; lastRecentH_ = 0.f;
    std::vector<float> v;
    auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        v.insert(v.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    ar(ddX, ddY, ddX + ddW, ddY + ddH, 0.17f, 0.17f, 0.20f, 0.98f);
    ar(ddX, ddY, ddX + ddW, ddY + 1, 0.30f, 0.30f, 0.35f, 1.f);
    if (menuHovered_ >= menuScroll_ && menuHovered_ < menuScroll_ + visibleCount && !menuItems_[menuHovered_].separator)
        ar(ddX + 2, ddY + arrowH + 2 + (menuHovered_ - menuScroll_) * itemH, ddX + ddW - 2, ddY + arrowH + 2 + (menuHovered_ - menuScroll_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
    flushSolid(v);
    glEnable(GL_SCISSOR_TEST);
    GLRenderer::scissor((int)ddX, wh - (int)(ddY + ddH + 4), (int)ddW, (int)(ddH + 4));
    for (int i = menuScroll_; i < menuScroll_ + visibleCount && i < (int)menuItems_.size(); ++i) {
        auto& item = menuItems_[i];
        float rowY = ddY + arrowH + 2 + (i - menuScroll_) * itemH;
        float iy = rowY + 4.f;
        if (item.separator) {
            ar(ddX + 8, rowY + 11, ddX + ddW - 8, rowY + 12, 0.3f, 0.3f, 0.33f, 1.f);
            flushSolid(v);
            continue;
        }
        float ib = (menuHovered_ == i) ? 1.f : 0.88f;
        if (!item.action && item.submenu < 0 && !item.separator) ib = 0.4f;
        font.drawText(item.label, ddX + 12.f, iy, ib, ib, ib, 1.f);
        if (!item.shortcut.empty())
            font.drawText(item.shortcut, ddX + ddW - font.measureText(item.shortcut) - 12.f, iy, 0.5f, 0.5f, 0.55f, 1.f);
    }
    glDisable(GL_SCISSOR_TEST);
    if (submenuOpen_ >= 0 && submenuOpen_ < (int)submenus_.size()) {
        auto& sm = submenus_[submenuOpen_];
        float sx = ddX + ddW + 2.f, sy = ddY + 2.f + submenuOpen_ * itemH;
        float sw = 180.f;
        for (auto& item : sm) {
            float w = font.measureText(item.label);
            if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 36.f;
            if (w + 48.f > sw) sw = w + 48.f;
        }
        float sh = 2.f + static_cast<float>(sm.size()) * itemH;
        bool popupLocal = ox != 0.f || oy != 0.f;
        if (!popupLocal) {
            float maxH = wh * 0.8f;
            if (sy + sh > wh - 8.f) sy = wh - sh - 8.f;
            if (sh > maxH) sh = maxH;
            if (sx + sw > ww - 4.f) sx = ddX - sw - 2.f;
        }
        lastHasSubmenu_ = true; lastSubmenuX_ = sx + ox; lastSubmenuY_ = sy + oy; lastSubmenuW_ = sw; lastSubmenuH_ = sh;
        v.clear();
        ar(sx, sy, sx + sw, sy + sh, 0.17f, 0.17f, 0.20f, 0.98f);
        if (submenuHovered_ >= (int)sm.size() || (submenuHovered_ >= 0 && sm[submenuHovered_].separator))
            submenuHovered_ = -1;
        if (submenuHovered_ >= 0 && submenuHovered_ < (int)sm.size())
            ar(sx + 2, sy + 2 + submenuHovered_ * itemH, sx + sw - 2, sy + 2 + (submenuHovered_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
        flushSolid(v);
        glEnable(GL_SCISSOR_TEST);
        GLRenderer::scissor((int)sx, wh - (int)(sy + sh), (int)sw, (int)sh);
        for (int i = 0; i < (int)sm.size(); ++i) {
            auto& item = sm[i];
            float iy = sy + 6.f + i * itemH;
            if (item.separator) { v.clear(); ar(sx + 8, sy + 13.f + i * itemH, sx + sw - 8, sy + 14.f + i * itemH, 0.3f, 0.3f, 0.33f, 1.f); flushSolid(v); continue; }
            float b = (submenuHovered_ == i) ? 1.f : 0.78f;
            if (!item.action && !item.separator) b = 0.4f;
            font.drawText(item.label, sx + 12.f, iy, b, b, b, 1.f);
            if (!item.shortcut.empty()) font.drawText(item.shortcut, sx + sw - font.measureText(item.shortcut) - 12.f, iy, 0.5f, 0.5f, 0.55f, 1.f);
        }
        glDisable(GL_SCISSOR_TEST);
        if (submenuOpen_ == 0 && submenuHovered_ == 3) {
            auto& recent = Application::instance().recentFiles();
            int count = std::max(1, static_cast<int>(recent.size()));
            float rx = sx + sw + 2.f, ry = sy + 2.f + 3.f * itemH;
            float rw = 360.f, rh = 2.f + count * itemH;
            for (auto& path : recent) rw = std::max(rw, font.measureText(path) + 28.f);
            bool popupLocal = ox != 0.f || oy != 0.f;
            if (!popupLocal && rx + rw > ww - 4.f) rx = sx - rw - 2.f;
            lastHasRecent_ = true; lastRecentX_ = rx + ox; lastRecentY_ = ry + oy; lastRecentW_ = rw; lastRecentH_ = rh;
            v.clear();
            ar(rx, ry, rx + rw, ry + rh, 0.17f, 0.17f, 0.20f, 0.98f);
            if (recentHovered_ >= 0 && recentHovered_ < static_cast<int>(recent.size()))
                ar(rx + 2, ry + 2 + recentHovered_ * itemH, rx + rw - 2, ry + 2 + (recentHovered_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
            flushSolid(v);
            if (recent.empty()) {
                font.drawText("(Empty)", rx + 12.f, ry + 6.f, 0.42f, 0.42f, 0.46f, 1.f);
            } else {
                for (int i = 0; i < static_cast<int>(recent.size()); ++i) {
                    float b = recentHovered_ == i ? 1.f : 0.78f;
                    font.drawText(recent[i], rx + 12.f, ry + 6.f + i * itemH, b, b, b, 1.f);
                }
            }
        }
    }
}

void Titlebar::init(int windowWidth) {
    buttons_[0].type = TitlebarButton::MENU;
    buttons_[1].type = TitlebarButton::MINIMIZE;
    buttons_[2].type = TitlebarButton::MAXIMIZE;
    buttons_[3].type = TitlebarButton::CLOSE;
    layout(windowWidth);
    buildMenu();
    if (!iconTex_ && icon_width > 0) {
        glGenTextures(1, &iconTex_);
        glBindTexture(GL_TEXTURE_2D, iconTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon_width, icon_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        iconW_ = icon_width;
        iconH_ = icon_height;
    }
}

void Titlebar::layout(int windowWidth) { updateButtonPositions(windowWidth); }

void Titlebar::updateButtonPositions(int ww) {
    float bw = buttonSize_, bh = height_;
    buttons_[3].x = ww - bw; buttons_[3].y = 0; buttons_[3].w = bw; buttons_[3].h = bh;
    buttons_[2].x = ww - bw*2; buttons_[2].y = 0; buttons_[2].w = bw; buttons_[2].h = bh;
    buttons_[1].x = ww - bw*3; buttons_[1].y = 0; buttons_[1].w = bw; buttons_[1].h = bh;
    buttons_[0].x = 0; buttons_[0].y = 0; buttons_[0].w = 36; buttons_[0].h = bh;
}

void Titlebar::appendSolidRects(std::vector<float>& v) {
    auto ar = [&](float x0,float y0,float x1,float y1,float cr,float cg,float cb,float ca) {
        v.insert(v.end(),{x0,y0,0,0,cr,cg,cb,ca, x0,y1,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x0,y0,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x1,y0,0,0,cr,cg,cb,ca});
    };
    ar(0, 0, buttons_[1].x, height_, 0.16f, 0.16f, 0.19f, 1.f);
    for (int i = 1; i < 4; ++i) {
        auto& btn = buttons_[i];
        if (btn.type == TitlebarButton::CLOSE) {
            float br = btn.hovered ? 0.85f : 0.35f;
            ar(btn.x, btn.y, btn.x+btn.w, btn.y+btn.h, br, btn.hovered?0.15f:0.15f, btn.hovered?0.15f:0.18f, 1.f);
        } else {
            float br = btn.hovered ? 0.28f : 0.2f;
            ar(btn.x, btn.y, btn.x+btn.w, btn.y+btn.h, br, br, br+0.03f, 1.f);
        }
    }
    auto& mb = buttons_[0];
    float mbr = (mb.hovered || menuOpen_) ? 0.28f : 0.16f;
    ar(mb.x, mb.y, mb.x+mb.w, mb.y+mb.h, mbr, mbr, mbr+0.03f, 1.f);
}

void Titlebar::drawForeground(FontAtlas& font) {
    if (iconTex_) {
        GLRenderer::setDrawMode(1);
        float padX = 8.f;
        float padY = (height_ - iconH_) / 2.f;
        std::vector<float> iv = {padX,padY,0,0,1,1,1,1, padX,padY+iconH_,0,1,1,1,1,1, padX+iconW_,padY+iconH_,1,1,1,1,1,1, padX,padY,0,0,1,1,1,1, padX+iconW_,padY+iconH_,1,1,1,1,1,1, padX+iconW_,padY,1,0,1,1,1,1};
        glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, iv.size()*sizeof(float), iv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, iconTex_); glDrawArrays(GL_TRIANGLES, 0, 6); glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
    }
    float titleLeft = buttons_[0].x + buttons_[0].w + 8.f, titleRight = buttons_[1].x - 8.f;
    float titleW = font.measureText(title_);
    float titleX = (titleLeft + titleRight) / 2.f - titleW / 2.f;
    if (titleX < titleLeft) titleX = titleLeft;
    float titleY = 8.f;
    font.drawText(title_, titleX, titleY, 0.7f, 0.7f, 0.7f, 1.f);
    float mid = height_ / 2.f;
    std::vector<float> iconRects;
    auto ar = [&](float x0,float y0,float x1,float y1,float cr,float cg,float cb,float ca) {
        iconRects.insert(iconRects.end(),{x0,y0,0,0,cr,cg,cb,ca, x0,y1,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x0,y0,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x1,y0,0,0,cr,cg,cb,ca});
    };
    ar(buttons_[1].x + 16.f, mid + 5.f, buttons_[1].x + 30.f, mid + 6.5f, 0.70f, 0.70f, 0.72f, 1.f);
    flushSolid(iconRects);
    font.drawText("\xe2\x96\xa1", buttons_[2].x + 16.f, mid - 8.f, 0.7f, 0.7f, 0.7f, 1.f);
    font.drawText("\xc3\x97", buttons_[3].x + 16.f, mid - 8.f, 1.f, 1.f, 1.f, 1.f);
    drawMenu(font);
}

void Titlebar::draw(FontAtlas& font, float, float, float, float) {
    std::vector<float> v;
    appendSolidRects(v);
    flushSolid(v);
    drawForeground(font);
}

bool Titlebar::handleMenuEvent(const SDL_Event& e) {
    if (!menuOpen_) return false;
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    int ww = 1280, wh = 720;
    if (window) SDL_GetWindowSize(window, &ww, &wh);
    float ddX = lastMenuX_, ddY = lastMenuY_;
    float ddW = lastMenuW_, itemH = 24.f;
    int visibleCount = (int)menuItems_.size();
    float arrowH = 0.f;
    float ddH = lastMenuH_ > 0.f ? lastMenuH_ : 2.f + visibleCount * itemH + arrowH * 2.f;
    auto clampMenuScroll = [&] {
        int maxScroll = (int)menuItems_.size() - visibleCount;
        if (menuScroll_ < 0) menuScroll_ = 0;
        if (menuScroll_ > maxScroll) menuScroll_ = maxScroll;
    };
    if (e.type == SDL_MOUSEMOTION) {
        float mx = (float)e.motion.x, my = (float)e.motion.y;
        menuHovered_ = -1;
        recentHovered_ = -1;
        if (lastHasRecent_ && mx >= lastRecentX_ && mx < lastRecentX_ + lastRecentW_ && my >= lastRecentY_ && my < lastRecentY_ + lastRecentH_) {
            auto& recent = Application::instance().recentFiles();
            int idx = (int)((my - lastRecentY_ - 2.f) / itemH);
            if (idx >= 0 && idx < static_cast<int>(recent.size())) recentHovered_ = idx;
            return true;
        }
        if (mx >= ddX && mx < ddX + ddW && my >= ddY && my < ddY + ddH) {
            int idx = menuScroll_ + (int)((my - ddY - arrowH - 2) / itemH);
            if (idx >= 0 && idx < (int)menuItems_.size() && !menuItems_[idx].separator) { menuHovered_ = idx; submenuOpen_ = menuItems_[idx].submenu; submenuHovered_ = -1; }
        } else if (lastHasSubmenu_) {
            float sx = lastSubmenuX_, sy = lastSubmenuY_, sw = lastSubmenuW_;
            if (mx >= sx && mx < sx + sw && my >= sy + 2.f && my < sy + 2.f + submenus_[submenuOpen_].size() * itemH) {
                int idx = (int)((my - sy - 2.f) / itemH);
                submenuHovered_ = (idx >= 0 && submenuOpen_ >= 0 && submenuOpen_ < (int)submenus_.size() && idx < (int)submenus_[submenuOpen_].size() && !submenus_[submenuOpen_][idx].separator) ? idx : -1;
            }
            else submenuHovered_ = -1;
        }
        return true;
    }
    if (e.type == SDL_MOUSEWHEEL) {
        menuScroll_ -= e.wheel.y;
        clampMenuScroll();
        return true;
    }
    if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_ESCAPE) { closeMenu(); return true; }
        if (e.key.keysym.sym == SDLK_UP) { --menuScroll_; clampMenuScroll(); return true; }
        if (e.key.keysym.sym == SDLK_DOWN) { ++menuScroll_; clampMenuScroll(); return true; }
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        float mx = (float)e.button.x, my = (float)e.button.y;
        if (lastHasRecent_ && mx >= lastRecentX_ && mx < lastRecentX_ + lastRecentW_ && my >= lastRecentY_ && my < lastRecentY_ + lastRecentH_) {
            auto& recent = Application::instance().recentFiles();
            int idx = (int)((my - lastRecentY_ - 2.f) / itemH);
            if (idx >= 0 && idx < static_cast<int>(recent.size())) {
                Application::instance().openRecentFile(static_cast<size_t>(idx));
                closeMenu();
            }
            return true;
        }
        if (lastHasSubmenu_) {
            float sx = lastSubmenuX_, sy = lastSubmenuY_, sw = lastSubmenuW_;
            if (mx >= sx && mx < sx + sw && my >= sy + 2.f && my < sy + 2.f + submenus_[submenuOpen_].size() * itemH) {
                int idx = (int)((my - sy - 2.f) / itemH);
                auto& sm = submenus_[submenuOpen_];
                if (submenuOpen_ == 0 && idx == 3) return true;
                if (idx >= 0 && idx < (int)sm.size() && !sm[idx].separator && sm[idx].action) { sm[idx].action(); closeMenu(); submenuOpen_ = -1; return true; }
                return true;
            }
        }
        if (mx >= ddX && mx < ddX + ddW && my >= ddY && my < ddY + ddH) {
            int idx = menuScroll_ + (int)((my - ddY - arrowH - 2) / itemH);
            if (idx >= 0 && idx < (int)menuItems_.size() && !menuItems_[idx].separator && menuItems_[idx].action) {
                menuItems_[idx].action();
                closeMenu();
                return true;
            }
        }
        closeMenu(); submenuOpen_ = -1;
        return true;
    }
    return false;
}

SDL_HitTestResult Titlebar::hitTest(int mx, int my, SDL_Window* window) {
    if (Application::instance().isBorderlessMaximized()) return SDL_HITTEST_NORMAL;
    int ww, wh; SDL_GetWindowSize(window, &ww, &wh);
    int re = static_cast<int>(resizeEdge_);
    bool left = mx >= 0 && mx < re;
    bool right = mx >= ww - re && mx < ww;
    bool top = my >= 0 && my < re;
    bool bottom = my >= wh - re && my < wh;
    if (left && top) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (right && top) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (left && bottom) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (right && bottom) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
    if (my < static_cast<int>(height_)) {
        for (auto& btn : buttons_)
            if (mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x + btn.w)) return SDL_HITTEST_NORMAL;
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

bool Titlebar::handleEvent(const SDL_Event& e, SDL_Window* window) {
    if (handleMenuEvent(e)) return true;
    if (e.type == SDL_MOUSEMOTION) {
        int mx = e.motion.x, my = e.motion.y;
        for (auto& btn : buttons_)
            btn.hovered = mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x+btn.w)
                       && my >= static_cast<int>(btn.y) && my <= static_cast<int>(btn.y+btn.h);
        return false;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        int mx = e.button.x, my = e.button.y;
        if (my < static_cast<int>(height_)) {
            for (auto& btn : buttons_) {
                if (mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x+btn.w)) {
                    switch (btn.type) {
                        case TitlebarButton::CLOSE: { SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); return true; }
                        case TitlebarButton::MAXIMIZE:
                            Application::instance().toggleMaximize();
                            return true;
                        case TitlebarButton::MINIMIZE: SDL_MinimizeWindow(window); return true;
                        case TitlebarButton::MENU: menuOpen_ = !menuOpen_; if (!menuOpen_) menuHovered_ = -1; return true;
                    }
                }
            }
            if (e.button.clicks == 2) {
                Application::instance().toggleMaximize();
                return true;
            }
        }
    }
    return false;
}
