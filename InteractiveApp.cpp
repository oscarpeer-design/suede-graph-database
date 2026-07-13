// InteractiveApp.cpp
//
// Win32 RichEdit implementation of interactive mode. ALL platform GUI code is
// contained in this one translation unit and exposed through the single
// runInteractive() entry point, so main() stays clean and Win32-free.
//
// The window is a small code editor for Suede Graph Database commands:
//   - a top RichEdit "command editor" with a line-number gutter beside it and
//     live syntax highlighting of the query keywords,
//   - a bottom read-only RichEdit "results console" (errors shown inline in red),
//   - a menu bar:
//        File  -> Open Commands / Save Commands / Save Commands As / Exit
//        Graph -> Save Graph / Load Graph / Save Snapshot / Load Snapshot
//        Run   -> Run (F5)
//
// Persistence is entirely in-window: NO console prompt. The session starts
// in-memory; the user saves/loads live graphs or snapshots to files chosen from
// standard dialogs, or issues FLUSH / LOAD / SNAPSHOT commands in the editor.
//
// Structure (to avoid the "one giant WndProc" mess):
//   - an InteractiveState struct holds the handler and the child window handles,
//   - helper functions own one job each (layout, run, highlight, file dialogs),
//   - WndProc only routes messages to those helpers.
//
// Guarded with _WIN32 so the rest of the project still builds on Linux/Mac.

#include "InteractiveApp.h"

#ifdef _WIN32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <richedit.h>
#include <commdlg.h>          // GetOpenFileName / GetSaveFileName

#include <memory>
#include <string>
#include <sstream>
#include <vector>
#include <cwctype>
#include <utility>

#include "Graph.h"
#include "GraphHandler.h"
#include "StorageEngine.h"

// ---------------------------------------------------------------------------
// Identifiers and layout constants
// ---------------------------------------------------------------------------
namespace {

    constexpr int ID_EDIT_COMMANDS = 1001;   // top RichEdit: command editor
    constexpr int ID_EDIT_RESULTS = 1002;   // bottom RichEdit: results console
    constexpr int ID_GUTTER = 1003;   // line-number gutter (static child)

    // Menu command ids
    constexpr int IDM_OPEN_CMDS = 2001;
    constexpr int IDM_SAVE_CMDS = 2002;
    constexpr int IDM_SAVE_CMDS_AS = 2003;
    constexpr int IDM_EXIT = 2004;
    constexpr int IDM_SAVE_GRAPH = 2101;
    constexpr int IDM_LOAD_GRAPH = 2102;
    constexpr int IDM_SAVE_SNAP = 2103;
    constexpr int IDM_LOAD_SNAP = 2104;
    constexpr int IDM_RUN = 2201;

    constexpr int MARGIN = 8;    // padding around/between panes
    constexpr int GUTTER_W = 48;   // width of the line-number gutter
    constexpr int SPLIT_PCT = 58;   // editor pane % of the vertical space

    // Everything the window and its message handler share. A pointer to one of these
    // is stored in the window's user data (GWLP_USERDATA) so WndProc reaches it
    // without globals.
    struct InteractiveState {
        std::unique_ptr<GraphHandler> handler;   // owned database handler
        HWND commands = nullptr;                 // command editor RichEdit
        HWND results = nullptr;                 // results console RichEdit
        HWND gutter = nullptr;                 // line-number gutter (STATIC)
        HFONT mono = nullptr;                 // shared fixed-width font
        std::wstring currentCmdFile;             // path of the open commands .txt ("" if none)
        bool highlighting = false;               // reentrancy guard for syntax highlight
    };

    // ------------------------- string conversions ------------------------------

    std::string wideToUtf8(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
            nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
            &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring utf8ToWide(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }

    std::wstring getCtrlTextW(HWND hwnd) {
        int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return {};
        std::wstring buf(len + 1, L'\0');
        GetWindowTextW(hwnd, &buf[0], len + 1);
        buf.resize(len);
        return buf;
    }

    // ------------------------- results console ---------------------------------

    void appendResult(HWND edit, const std::wstring& text, bool isError) {
        int len = GetWindowTextLengthW(edit);
        SendMessageW(edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);

        CHARFORMAT2W cf;
        ZeroMemory(&cf, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = isError ? RGB(200, 0, 0) : RGB(0, 0, 0);
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

        std::wstring line = text + L"\r\n";
        SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    }

    // --------------------------- run the editor --------------------------------
    //
    // The GUI twin of batch mode's openAndRunCommandsFile loop: count each physical
    // line first, skip blanks and '#' comments, run each through executeCommand, and
    // HALT on the first error. Only the output sink differs (results console).
    void runCommands(InteractiveState* state) {
        SetWindowTextW(state->results, L"");

        const std::wstring all = getCtrlTextW(state->commands);
        std::wstringstream ss(all);
        std::wstring line;
        size_t lineNo = 0;

        while (std::getline(ss, line, L'\n')) {
            ++lineNo;
            if (!line.empty() && line.back() == L'\r')
                line.pop_back();

            std::wstring trimmed = line;
            size_t b = trimmed.find_first_not_of(L" \t");
            if (b == std::wstring::npos)
                continue;                       // blank
            trimmed = trimmed.substr(b);
            if (trimmed[0] == L'#')
                continue;                       // comment

            std::string command = wideToUtf8(line);
            QueryResult result = state->handler->executeCommand(command);

            std::wstring prefix = L"[" + std::to_wstring(lineNo) + L"] ";
            if (!result.success) {
                appendResult(state->results,
                    prefix + L"ERROR: " + utf8ToWide(result.message),
                    /*isError=*/true);
                return;                          // halt on first error
            }
            appendResult(state->results,
                prefix + line + L" -> " + utf8ToWide(result.message),
                /*isError=*/false);
        }
    }

    // --------------------------- syntax highlighting ---------------------------
    //
    // Colour the query keywords in the command editor. Runs on each edit (EN_CHANGE).
    // It saves and restores the caret/selection and suppresses redraw+notifications
    // while recolouring, so typing stays smooth and we don't recurse via EN_CHANGE.
    const wchar_t* const KEYWORDS[] = {
        L"SELECT", L"INSERT", L"INTO", L"VALUES", L"FROM", L"WHERE", L"AND",
        L"DELETE", L"UPDATE", L"SET", L"MATCH", L"TOP", L"NODES", L"EDGES",
        L"LABEL", L"ID", L"SNAPSHOT", L"LIVE", L"LOAD", L"SAVE", L"FLUSH",
        L"CREATE", L"RELEASE", L"NODE", L"EDGE", L"COUNT",
    };

    bool isWordChar(wchar_t c) {
        return iswalnum(c) || c == L'_';
    }

    bool equalsNoCase(const std::wstring& a, const wchar_t* b) {
        size_t i = 0;
        for (; a[i] && b[i]; ++i)
            if (towupper(a[i]) != towupper(b[i]))
                return false;
        return a[i] == 0 && b[i] == 0;
    }

    bool isKeyword(const std::wstring& word) {
        for (const wchar_t* kw : KEYWORDS)
            if (equalsNoCase(word, kw))
                return true;
        return false;
    }

    // Apply one colour to a [start,end) character range in the editor.
    void colorRange(HWND edit, int start, int end, COLORREF colour) {
        CHARFORMAT2W cf;
        ZeroMemory(&cf, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = colour;
        SendMessageW(edit, EM_SETSEL, (WPARAM)start, (LPARAM)end);
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }

    void highlightSyntax(InteractiveState* state) {
        if (state->highlighting) return;        // guard against EN_CHANGE recursion
        state->highlighting = true;

        HWND edit = state->commands;

        // Save the caret/selection so recolouring doesn't move it.
        DWORD selStart = 0, selEnd = 0;
        SendMessageW(edit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);

        // Suppress redraw + change notifications while we recolour.
        SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
        DWORD savedMask = (DWORD)SendMessageW(edit, EM_SETEVENTMASK, 0, 0);

        const std::wstring text = getCtrlTextW(edit);
        const int n = (int)text.size();

        // Reset everything to the default colour first.
        colorRange(edit, 0, n, RGB(0, 0, 0));

        // Walk words; colour keywords blue, numbers teal, quoted strings dark red.
        int i = 0;
        while (i < n) {
            wchar_t c = text[i];

            // quoted string '...'
            if (c == L'\'') {
                int start = i++;
                while (i < n && text[i] != L'\'') ++i;
                if (i < n) ++i;                  // include closing quote
                colorRange(edit, start, i, RGB(163, 21, 21));
                continue;
            }
            // identifier / keyword
            if (isWordChar(c) && !iswdigit(c)) {
                int start = i;
                while (i < n && isWordChar(text[i])) ++i;
                std::wstring word = text.substr(start, i - start);
                if (isKeyword(word))
                    colorRange(edit, start, i, RGB(0, 0, 200));
                continue;
            }
            // number
            if (iswdigit(c)) {
                int start = i;
                while (i < n && (iswdigit(text[i]) || text[i] == L'.')) ++i;
                colorRange(edit, start, i, RGB(9, 134, 88));
                continue;
            }
            ++i;
        }

        // Restore caret/selection, notifications, and redraw.
        SendMessageW(edit, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
        SendMessageW(edit, EM_SETEVENTMASK, 0, (LPARAM)savedMask);
        SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(edit, nullptr, TRUE);

        state->highlighting = false;
    }

    // --------------------------- line-number gutter ----------------------------
    //
    // The gutter is a STATIC child we owner-draw. We repaint it whenever the editor
    // scrolls or its text changes, numbering the lines currently visible in the
    // editor so the numbers stay aligned with the wrapped text view.
    void paintGutter(InteractiveState* state) {
        HWND edit = state->commands;
        HWND gutter = state->gutter;
        if (!edit || !gutter) return;

        HDC dc = GetDC(gutter);
        if (!dc) return;

        RECT rc;
        GetClientRect(gutter, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        HFONT old = (HFONT)SelectObject(dc, state->mono);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(120, 120, 120));

        // First visible character index, and total line count.
        int firstVisible = (int)SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        int lineCount = (int)SendMessageW(edit, EM_GETLINECOUNT, 0, 0);

        // Height of one line: measure the font.
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int lineH = tm.tmHeight + tm.tmExternalLeading;
        if (lineH <= 0) lineH = 14;

        int y = 2;
        for (int ln = firstVisible; ln < lineCount; ++ln) {
            if (y > rc.bottom) break;
            std::wstring num = std::to_wstring(ln + 1);
            RECT lineRc = { rc.left, y, rc.right - 4, y + lineH };
            DrawTextW(dc, num.c_str(), (int)num.size(), &lineRc,
                DT_RIGHT | DT_SINGLELINE | DT_TOP);
            y += lineH;
        }

        SelectObject(dc, old);
        ReleaseDC(gutter, dc);
    }

    // ----------------------------- file dialogs --------------------------------

    // Show an Open/Save dialog; return the chosen path or "" if cancelled.
    std::wstring chooseFile(HWND owner, const wchar_t* filter,
        const wchar_t* defExt, bool saving) {
        wchar_t buf[MAX_PATH];
        buf[0] = 0;

        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = defExt;
        ofn.Flags = saving
            ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
            : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);

        BOOL ok = saving ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        return ok ? std::wstring(buf) : std::wstring();
    }

    // Load a commands .txt file into the editor.
    void openCommandsFile(HWND hwnd, InteractiveState* state) {
        std::wstring path = chooseFile(hwnd, L"Text files\0*.txt\0All files\0*.*\0",
            L"txt", /*saving=*/false);
        if (path.empty()) return;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            MessageBoxW(hwnd, L"Could not open the file.", L"Open Commands", MB_OK | MB_ICONERROR);
            return;
        }
        DWORD size = GetFileSize(h, nullptr);
        std::string bytes(size, '\0');
        DWORD read = 0;
        ReadFile(h, &bytes[0], size, &read, nullptr);
        CloseHandle(h);
        bytes.resize(read);

        SetWindowTextW(state->commands, utf8ToWide(bytes).c_str());
        state->currentCmdFile = path;
        highlightSyntax(state);
        InvalidateRect(state->gutter, nullptr, TRUE);
    }

    // Save the editor contents to a commands .txt file. If forceDialog is false and
    // we already have a path, save there silently; otherwise prompt.
    void saveCommandsFile(HWND hwnd, InteractiveState* state, bool forceDialog) {
        std::wstring path = state->currentCmdFile;
        if (forceDialog || path.empty()) {
            path = chooseFile(hwnd, L"Text files\0*.txt\0All files\0*.*\0",
                L"txt", /*saving=*/true);
            if (path.empty()) return;
        }

        std::string bytes = wideToUtf8(getCtrlTextW(state->commands));

        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            MessageBoxW(hwnd, L"Could not save the file.", L"Save Commands", MB_OK | MB_ICONERROR);
            return;
        }
        DWORD written = 0;
        WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
        CloseHandle(h);
        state->currentCmdFile = path;
    }

    // Graph / snapshot persistence via dialogs -> handler methods. Reports the
    // outcome in the results console so the user gets feedback.
    void saveGraph(HWND hwnd, InteractiveState* state) {
        std::wstring path = chooseFile(hwnd, L"Graph binary\0*.bin\0All files\0*.*\0",
            L"bin", /*saving=*/true);
        if (path.empty()) return;
        bool ok = state->handler->flush(wideToUtf8(path));
        appendResult(state->results,
            ok ? L"Graph saved to " + path : L"Failed to save graph to " + path, !ok);
    }

    void loadGraph(HWND hwnd, InteractiveState* state) {
        std::wstring path = chooseFile(hwnd, L"Graph binary\0*.bin\0All files\0*.*\0",
            L"bin", /*saving=*/false);
        if (path.empty()) return;
        bool ok = state->handler->loadLive(wideToUtf8(path));
        appendResult(state->results,
            ok ? L"Graph loaded from " + path : L"Failed to load graph from " + path, !ok);
    }

    void saveSnapshot(HWND hwnd, InteractiveState* state) {
        std::wstring path = chooseFile(hwnd, L"Snapshot binary\0*.bin\0All files\0*.*\0",
            L"bin", /*saving=*/true);
        if (path.empty()) return;
        // Create a fresh snapshot of the current graph, then persist it.
        uint64_t id = state->handler->createSnapshot();
        bool ok = state->handler->persistSnapshot(id, wideToUtf8(path));
        appendResult(state->results,
            ok ? L"Snapshot saved to " + path : L"Failed to save snapshot to " + path, !ok);
    }

    void loadSnapshot(HWND hwnd, InteractiveState* state) {
        std::wstring path = chooseFile(hwnd, L"Snapshot binary\0*.bin\0All files\0*.*\0",
            L"bin", /*saving=*/false);
        if (path.empty()) return;
        bool ok = state->handler->loadSnapshot(wideToUtf8(path));
        appendResult(state->results,
            ok ? L"Snapshot loaded from " + path : L"Failed to load snapshot from " + path, !ok);
    }

    // ----------------------------- controls/layout -----------------------------

    void createControls(HWND parent, InteractiveState* state) {
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);

        // Shared fixed-width font.
        state->mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        // Line-number gutter: a plain STATIC we owner-draw ourselves.
        state->gutter = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 0, 0, parent, (HMENU)(INT_PTR)ID_GUTTER, inst, nullptr);

        // Command editor (top).
        state->commands = CreateWindowExW(
            WS_EX_CLIENTEDGE, RICHEDIT_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN |
            WS_VSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0, parent, (HMENU)(INT_PTR)ID_EDIT_COMMANDS, inst, nullptr);

        // Results console (bottom).
        state->results = CreateWindowExW(
            WS_EX_CLIENTEDGE, RICHEDIT_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
            WS_VSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0, parent, (HMENU)(INT_PTR)ID_EDIT_RESULTS, inst, nullptr);

        SendMessageW(state->commands, WM_SETFONT, (WPARAM)state->mono, TRUE);
        SendMessageW(state->results, WM_SETFONT, (WPARAM)state->mono, TRUE);

        // Ask the command editor to notify us on text change (for highlight + gutter).
        DWORD mask = (DWORD)SendMessageW(state->commands, EM_GETEVENTMASK, 0, 0);
        SendMessageW(state->commands, EM_SETEVENTMASK, 0, mask | ENM_CHANGE | ENM_SCROLL);

        // A starter template so the editor isn't an intimidating blank.
        SetWindowTextW(state->commands,
            L"# Type commands, one per line. '#' starts a comment.\r\n"
            L"INSERT INTO NODES (label, name) VALUES ('Person', 'Alice')\r\n"
            L"INSERT INTO NODES (label, name) VALUES ('Person', 'Bob')\r\n"
            L"SELECT * FROM NODES WHERE LABEL = 'Person'\r\n"
            L"NODE COUNT\r\n");
        highlightSyntax(state);
    }

    void layoutControls(HWND parent, InteractiveState* state) {
        RECT rc;
        GetClientRect(parent, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int contentH = h - 3 * MARGIN;
        int editorH = (contentH * SPLIT_PCT) / 100;
        int resultsH = contentH - editorH;

        int y = MARGIN;
        // gutter | command editor  (top row)
        MoveWindow(state->gutter, MARGIN, y, GUTTER_W, editorH, TRUE);
        MoveWindow(state->commands, MARGIN + GUTTER_W + 2, y, w - 2 * MARGIN - GUTTER_W - 2, editorH, TRUE);
        y += editorH + MARGIN;
        // results console (bottom, full width)
        MoveWindow(state->results, MARGIN, y, w - 2 * MARGIN, resultsH, TRUE);

        InvalidateRect(state->gutter, nullptr, TRUE);
    }

    // Build the menu bar.
    HMENU buildMenu() {
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, IDM_OPEN_CMDS, L"&Open Commands...\tCtrl+O");
        AppendMenuW(file, MF_STRING, IDM_SAVE_CMDS, L"&Save Commands\tCtrl+S");
        AppendMenuW(file, MF_STRING, IDM_SAVE_CMDS_AS, L"Save Commands &As...");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");

        HMENU graph = CreatePopupMenu();
        AppendMenuW(graph, MF_STRING, IDM_SAVE_GRAPH, L"&Save Graph...");
        AppendMenuW(graph, MF_STRING, IDM_LOAD_GRAPH, L"&Load Graph...");
        AppendMenuW(graph, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(graph, MF_STRING, IDM_SAVE_SNAP, L"Save S&napshot...");
        AppendMenuW(graph, MF_STRING, IDM_LOAD_SNAP, L"Load Sn&apshot...");

        HMENU run = CreatePopupMenu();
        AppendMenuW(run, MF_STRING, IDM_RUN, L"&Run\tF5");

        HMENU bar = CreateMenu();
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)graph, L"&Graph");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)run, L"&Run");
        return bar;
    }

    // ------------------------------ WndProc ------------------------------------

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* state = reinterpret_cast<InteractiveState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);

            createControls(hwnd, state);
            // FIX for the "only a Run button showed" bug: lay the controls out
            // immediately, with the real client size, instead of waiting for a
            // WM_SIZE that may arrive before the controls exist.
            layoutControls(hwnd, state);
            return 0;
        }
        case WM_SIZE: {
            auto* state = reinterpret_cast<InteractiveState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (state && state->commands) layoutControls(hwnd, state);
            return 0;
        }
        case WM_DRAWITEM: {
            // Owner-draw the gutter.
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis->CtlID == ID_GUTTER) {
                auto* state = reinterpret_cast<InteractiveState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (state) paintGutter(state);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            auto* state = reinterpret_cast<InteractiveState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!state) break;
            int id = LOWORD(wParam);

            // RichEdit change notifications: re-highlight + repaint gutter.
            if (HIWORD(wParam) == EN_CHANGE && id == ID_EDIT_COMMANDS) {
                highlightSyntax(state);
                InvalidateRect(state->gutter, nullptr, TRUE);
                return 0;
            }

            switch (id) {
            case IDM_OPEN_CMDS:    openCommandsFile(hwnd, state);           return 0;
            case IDM_SAVE_CMDS:    saveCommandsFile(hwnd, state, false);    return 0;
            case IDM_SAVE_CMDS_AS: saveCommandsFile(hwnd, state, true);     return 0;
            case IDM_EXIT:         DestroyWindow(hwnd);                     return 0;
            case IDM_SAVE_GRAPH:   saveGraph(hwnd, state);                  return 0;
            case IDM_LOAD_GRAPH:   loadGraph(hwnd, state);                  return 0;
            case IDM_SAVE_SNAP:    saveSnapshot(hwnd, state);               return 0;
            case IDM_LOAD_SNAP:    loadSnapshot(hwnd, state);               return 0;
            case IDM_RUN:          runCommands(state);                      return 0;
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace

// ---------------------------------------------------------------------------
// runInteractive — the single entry point main() calls (Windows build)
// ---------------------------------------------------------------------------
int runInteractive(int /*argc*/, char* /*argv*/[]) {
    // RichEdit lives in a DLL that must be loaded before RICHEDIT_CLASSW can be
    // created. Msftedit.dll provides RichEdit 4.1.
    HMODULE richLib = LoadLibraryW(L"Msftedit.dll");
    if (!richLib) {
        MessageBoxW(nullptr, L"Failed to load RichEdit (Msftedit.dll).",
            L"Suede Graph Database", MB_ICONERROR | MB_OK);
        return 1;
    }

    // NO console prompt: interactive mode starts in-memory. The user saves/loads
    // graphs and snapshots from the Graph menu, or via typed FLUSH/LOAD commands.
    auto graph = std::make_unique<Graph>();
    auto handlerPtr = std::make_unique<GraphHandler>(std::move(graph), nullptr);

    InteractiveState state;
    state.handler = std::move(handlerPtr);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* className = L"SuedeGraphMainWindow";

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, className, L"Suede Graph Database - Interactive",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680,
        nullptr, buildMenu(), inst, &state);

    if (!hwnd) {
        FreeLibrary(richLib);
        return 1;
    }

    // Keyboard accelerators: F5 = Run, Ctrl+O = Open, Ctrl+S = Save.
    ACCEL accels[] = {
        { FVIRTKEY,               VK_F5, IDM_RUN },
        { FVIRTKEY | FCONTROL,    'O',   IDM_OPEN_CMDS },
        { FVIRTKEY | FCONTROL,    'S',   IDM_SAVE_CMDS },
    };
    HACCEL hAccel = CreateAcceleratorTableW(accels, 3);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyAcceleratorTable(hAccel);
    FreeLibrary(richLib);
    return (int)msg.wParam;
}

#else  // not _WIN32

#include <iostream>

int runInteractive(int /*argc*/, char* /*argv*/[]) {
    std::cerr << "Interactive (GUI) mode is only supported on Windows.\n"
        "Use batch mode instead: run with the 'batch' argument.\n";
    return 1;
}

#endif // _WIN32