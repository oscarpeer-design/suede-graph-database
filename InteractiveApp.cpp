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
// Request RichEdit 4.1 symbols (MSFTEDIT_CLASS) BEFORE including richedit.h.
// MSFTEDIT_CLASS ("RICHEDIT50W") is the window class registered by Msftedit.dll,
// which is the DLL we LoadLibrary below. The older RICHEDIT_CLASSW
// ("RichEdit20W") belongs to Riched20.dll, which we do NOT load -- creating a
// control with that class after loading only Msftedit.dll yields a control that
// never renders (the "blank editor" bug). This macro must precede <richedit.h>.
#ifndef _RICHEDIT_VER
#define _RICHEDIT_VER 0x0410
#endif
#include <richedit.h>
#include <richole.h>          // IRichEditOle (EM_GETOLEINTERFACE)
#include <commdlg.h>          // GetOpenFileName / GetSaveFileName
#include <tom.h>              // ITextDocument: suspend undo while syntax-highlighting

#include <memory>
#include <string>
#include <sstream>
#include <vector>
#include <cwctype>
#include <utility>
#include <cstring>            // std::memcmp for the graph-magic file check

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
        // The command editor's Text Object Model document, fetched once. Used to
        // suspend the undo recorder while the syntax highlighter writes character
        // colours, so Ctrl+Z reverses real text edits -- not invisible colour
        // changes. Null if the interface couldn't be obtained (undo still works,
        // just with colour ops back on the stack, i.e. the old behaviour).
        ITextDocument* textDoc = nullptr;
    };

    const int undoLimit = 100; // only 100 previous edits are saved

    // RAII guard: suspends the RichEdit's undo recorder for its lifetime, then
    // resumes it -- even if the scope is left early. This is what keeps the
    // highlighter's EM_SETCHARFORMAT calls off the undo stack. Pairing suspend and
    // resume in a destructor is important: a missed Resume would leave undo
    // permanently OFF, so we never rely on reaching a manual resume call.
    struct UndoSuspend {
        ITextDocument* doc;
        explicit UndoSuspend(ITextDocument* d) : doc(d) {
            if (doc) doc->Undo(tomSuspend, nullptr);
        }
        ~UndoSuspend() {
            if (doc) doc->Undo(tomResume, nullptr);
        }
        UndoSuspend(const UndoSuspend&) = delete;
        UndoSuspend& operator=(const UndoSuspend&) = delete;
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

    // Normalize any mix of line endings (lone "\n" from Unix files, lone "\r",
    // or already-correct "\r\n") to Windows "\r\n". The Win32 RichEdit control
    // only breaks lines on "\r\n" when text is set via SetWindowTextW: a file
    // saved with bare "\n" would otherwise collapse onto a single unreadable
    // line. Call this on any text loaded from disk before showing it.
    std::wstring normalizeNewlines(const std::wstring& in) {
        std::wstring out;
        out.reserve(in.size() + in.size() / 8 + 16);
        for (size_t i = 0; i < in.size(); ++i) {
            wchar_t c = in[i];
            if (c == L'\r') {
                // collapse "\r" or "\r\n" into a single "\r\n"
                out += L"\r\n";
                if (i + 1 < in.size() && in[i + 1] == L'\n')
                    ++i;                          // skip the paired '\n'
            }
            else if (c == L'\n') {
                out += L"\r\n";                   // lone '\n' -> "\r\n"
            }
            else {
                out += c;
            }
        }
        return out;
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

        // Reset to a clean, empty graph before every Run. The interactive session
        // otherwise holds ONE persistent graph, so pressing Run twice would
        // execute the INSERTs twice and duplicate everything (2x, 3x, ... the
        // rows). Rebuilding the handler here makes each Run reproducible: the
        // buffer always executes against an empty graph, exactly like a fresh
        // batch invocation. (Any snapshots from a prior Run are dropped too.)
        state->handler = std::make_unique<GraphHandler>(
            std::make_unique<Graph>(), nullptr);

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

    // Fetch the text of ONE logical line (line index `line`) directly from the
    // control via EM_GETLINE, and report where that line begins in the control's
    // own character coordinates (EM_LINEINDEX). This is the crux of the rewrite:
    // every colour position is computed as (control line-start) + (offset within
    // this one line). Neither half involves counting newlines in a big buffer, so
    // there is no scanned-vs-addressed offset mismatch to drift -- the previous
    // approach scanned one giant getCtrlTextW string and fed those offsets to
    // EM_SETSEL, whose newline accounting could differ, shifting every colour.
    //
    // Returns the line text (without its line-break) and sets `lineStartOut` to
    // the line's first-character index. Returns "" for an empty line.
    std::wstring getLineText(HWND edit, int line, int& lineStartOut) {
        lineStartOut = (int)SendMessageW(edit, EM_LINEINDEX, (WPARAM)line, 0);
        if (lineStartOut < 0) { lineStartOut = 0; return L""; }
        int len = (int)SendMessageW(edit, EM_LINELENGTH, (WPARAM)lineStartOut, 0);
        if (len <= 0) return L"";

        // EM_GETLINE requires the FIRST WORD of the buffer to hold its capacity in
        // TCHARs, and does NOT null-terminate; it returns the count actually copied.
        std::wstring buf(len + 1, L'\0');
        *reinterpret_cast<WORD*>(&buf[0]) = (WORD)(len + 1);
        int copied = (int)SendMessageW(edit, EM_GETLINE, (WPARAM)line, (LPARAM)&buf[0]);
        buf.resize(copied < 0 ? 0 : copied);
        return buf;
    }

    // Colour every token on a SINGLE line. `lineText` is that line's text and
    // `lineStart` is where the line begins in the control. Each token is coloured
    // at lineStart + (its position within lineText), so positions are exact. The
    // caller has already reset [lineStart, lineStart+len) to default black.
    // Quotes are naturally single-line here: lineText contains no newline, so an
    // unterminated quote can at most reach the end of this line.
    void colourLineTokens(HWND edit, const std::wstring& lineText, int lineStart) {
        const int n = (int)lineText.size();
        int i = 0;
        while (i < n) {
            wchar_t c = lineText[i];

            // quoted string '...'
            if (c == L'\'') {
                int start = i++;
                while (i < n && lineText[i] != L'\'') ++i;
                if (i < n && lineText[i] == L'\'') ++i;   // include closing quote
                colorRange(edit, lineStart + start, lineStart + i, RGB(163, 21, 21));
                continue;
            }
            // identifier / keyword
            if (isWordChar(c) && !iswdigit(c)) {
                int start = i;
                while (i < n && isWordChar(lineText[i])) ++i;
                std::wstring word = lineText.substr(start, i - start);
                if (isKeyword(word))
                    colorRange(edit, lineStart + start, lineStart + i, RGB(0, 0, 200));
                continue;
            }
            // number
            if (iswdigit(c)) {
                int start = i;
                while (i < n && (iswdigit(lineText[i]) || lineText[i] == L'.')) ++i;
                colorRange(edit, lineStart + start, lineStart + i, RGB(9, 134, 88));
                continue;
            }
            ++i;
        }
    }

    // Recolour a RANGE of logical lines [firstLine, lastLine], one line at a time
    // in the control's own coordinates. This is the single highlighting engine:
    // the full-document pass and the caret-line pass both call it, differing only
    // in the line range. Redraw, undo-suspension, and caret restoration are done
    // once around the whole range.
    void highlightLineRange(InteractiveState* state, int firstLine, int lastLine) {
        if (state->highlighting) return;        // guard against EN_CHANGE recursion
        state->highlighting = true;

        HWND edit = state->commands;

        // Save caret/selection so recolouring doesn't move it.
        DWORD selStart = 0, selEnd = 0;
        SendMessageW(edit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);

        SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
        DWORD savedMask = (DWORD)SendMessageW(edit, EM_SETEVENTMASK, 0, 0);

        {
            // Keep all colour changes off the undo stack (so Ctrl+Z targets text).
            UndoSuspend noUndo(state->textDoc);

            for (int line = firstLine; line <= lastLine; ++line) {
                int lineStart = 0;
                std::wstring lineText = getLineText(edit, line, lineStart);
                int len = (int)lineText.size();
                // Reset this line to default, then colour its tokens. Reset and
                // colour use the SAME per-line coordinates, so nothing drifts.
                if (len > 0)
                    colorRange(edit, lineStart, lineStart + len, RGB(0, 0, 0));
                colourLineTokens(edit, lineText, lineStart);
            }
        }

        // Restore caret/selection, notifications, and redraw.
        SendMessageW(edit, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
        SendMessageW(edit, EM_SETEVENTMASK, 0, (LPARAM)savedMask);
        SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(edit, nullptr, TRUE);

        state->highlighting = false;
    }

    // Full-document highlight. Used once on load (open file / starter template).
    void highlightSyntax(InteractiveState* state) {
        int lineCount = (int)SendMessageW(state->commands, EM_GETLINECOUNT, 0, 0);
        if (lineCount <= 0) return;
        highlightLineRange(state, 0, lineCount - 1);
    }

    // Line-local highlight. Recolours ONLY the caret's line -- all a single-char
    // edit can affect -- so typing in a large file stays smooth. Called on
    // EN_CHANGE instead of highlightSyntax.
    void highlightCaretLine(InteractiveState* state) {
        DWORD selStart = 0, selEnd = 0;
        SendMessageW(state->commands, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
        int line = (int)SendMessageW(state->commands, EM_LINEFROMCHAR, (WPARAM)selStart, 0);
        highlightLineRange(state, line, line);
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

        // Total logical lines, and the first line currently scrolled into view.
        int firstVisible = (int)SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        int lineCount = (int)SendMessageW(edit, EM_GETLINECOUNT, 0, 0);

        // Font metrics for the row height used when drawing each number.
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int lineH = tm.tmHeight + tm.tmExternalLeading;
        if (lineH <= 0) lineH = 14;

        // Ask the RICHEDIT for the actual pixel Y of each line, instead of
        // guessing from a fixed start offset and an assumed line height. The old
        // code started at y=2 and stepped by its own lineH, which did not match
        // the editor's top inset or exact line spacing -- so the numbers drifted
        // out of alignment (landing between two text rows), worse further down
        // and while scrolling. EM_POSFROMCHAR gives the true position of a line's
        // first character in the editor's client coordinates; since the gutter
        // sits at the same vertical offset as the editor, that Y is exactly where
        // the number belongs.
        for (int ln = firstVisible; ln < lineCount; ++ln) {
            // First character index of this logical line.
            int chIndex = (int)SendMessageW(edit, EM_LINEINDEX, (WPARAM)ln, 0);
            if (chIndex < 0) break;

            // Pixel position of that character within the editor's client area.
            // RichEdit 4.1 returns the point packed in the result: x in the low
            // word, y in the high word (signed).
            POINTL pt = { 0, 0 };
            SendMessageW(edit, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)chIndex);
            int y = pt.y;

            if (y > rc.bottom) break;            // past the bottom of the view
            if (y + lineH < 0) continue;         // above the top (shouldn't happen)

            std::wstring num = std::to_wstring(ln + 1);
            RECT lineRc = { rc.left, y, rc.right - 4, y + lineH };
            DrawTextW(dc, num.c_str(), (int)num.size(), &lineRc,
                DT_RIGHT | DT_SINGLELINE | DT_TOP);
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
        // Check the ReadFile result: on failure `read` stays 0 and we'd otherwise
        // silently load an empty file. Report the error and bail instead.
        BOOL readOk = ReadFile(h, &bytes[0], size, &read, nullptr);
        CloseHandle(h);
        if (!readOk) {
            MessageBoxW(hwnd, L"Could not read the file.", L"Open Commands",
                MB_OK | MB_ICONERROR);
            return;
        }
        bytes.resize(read);

        // Normalize line endings so files saved with bare "\n" (e.g. produced on
        // Unix) display as separate lines rather than collapsing onto one line.
        SetWindowTextW(state->commands,
            normalizeNewlines(utf8ToWide(bytes)).c_str());
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
        // Check the WriteFile result so a failed save doesn't pass silently (and
        // so we don't record currentCmdFile for a file we never wrote).
        BOOL writeOk = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
        CloseHandle(h);
        if (!writeOk || written != (DWORD)bytes.size()) {
            MessageBoxW(hwnd, L"Could not save the file.", L"Save Commands",
                MB_OK | MB_ICONERROR);
            return;
        }
        state->currentCmdFile = path;
    }

    // --------- graph/snapshot file-kind guards (prevent format clobber) --------
    //
    // Graph binaries begin with the 8-byte magic "GRAPHDB" (see StorageEngine's
    // FileHeader). Snapshot files do NOT -- they start with a raw uint64 version.
    // These helpers peek the file so we can warn before a Save writes one format
    // over a file that holds the other (the corruption that made graph.bin
    // unusable: a snapshot written over a graph binary).

    bool fileExistsNonEmpty(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD size = GetFileSize(h, nullptr);
        CloseHandle(h);
        return size > 0;
    }

    // True if the file exists and starts with the graph magic "GRAPHDB".
    bool fileLooksLikeGraph(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        char magic[7] = { 0 };
        DWORD read = 0;
        BOOL ok = ReadFile(h, magic, 7, &read, nullptr);
        CloseHandle(h);
        if (!ok || read < 7) return false;
        return std::memcmp(magic, "GRAPHDB", 7) == 0;
    }

    // Ask the user to confirm overwriting a file that holds the OTHER format.
    // savingGraph = true  -> we're about to write a graph over a non-graph file.
    // savingGraph = false -> we're about to write a snapshot over a graph file.
    bool confirmOverwriteWrongKind(HWND hwnd, const std::wstring& path, bool savingGraph) {
        std::wstring msg = savingGraph
            ? L"This file does not look like a graph binary (it may be a snapshot).\n\n"
            L"Overwrite it with a GRAPH binary anyway?\n\n" + path
            : L"This file looks like a GRAPH binary, not a snapshot.\n\n"
            L"Overwriting it with a snapshot will corrupt the graph. Continue?\n\n" + path;
        int r = MessageBoxW(hwnd, msg.c_str(), L"Overwrite different file type?",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        return r == IDYES;
    }

    // Graph / snapshot persistence via dialogs -> handler methods. Reports the
    // outcome in the results console so the user gets feedback.
    void saveGraph(HWND hwnd, InteractiveState* state) {
        std::wstring path = chooseFile(hwnd, L"Graph binary\0*.bin\0All files\0*.*\0",
            L"bin", /*saving=*/true);
        if (path.empty()) return;
        // Guard: if the chosen file is a snapshot (or at least isn't a graph
        // binary) but already exists with content, warn before overwriting it.
        if (fileExistsNonEmpty(path) && !fileLooksLikeGraph(path) &&
            !confirmOverwriteWrongKind(hwnd, path, /*savingGraph=*/true))
            return;
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
        // Snapshots use a DISTINCT extension (.snap) from graph files (.bin) so
        // the dialog defaults away from graph binaries -- this is what stops a
        // "Save Snapshot" from silently clobbering a graph.bin (the two formats
        // are incompatible; a snapshot written over a graph corrupts it).
        std::wstring path = chooseFile(hwnd, L"Snapshot\0*.snap\0All files\0*.*\0",
            L"snap", /*saving=*/true);
        if (path.empty()) return;
        // Guard: if the chosen file already holds a GRAPH binary, warn before we
        // overwrite it with snapshot bytes.
        if (fileLooksLikeGraph(path) &&
            !confirmOverwriteWrongKind(hwnd, path, /*savingGraph=*/false))
            return;
        // Create a fresh snapshot of the current graph, then persist it.
        uint64_t id = state->handler->createSnapshot();
        bool ok = state->handler->persistSnapshot(id, wideToUtf8(path));
        appendResult(state->results,
            ok ? L"Snapshot saved to " + path : L"Failed to save snapshot to " + path, !ok);
    }

    void loadSnapshot(HWND hwnd, InteractiveState* state) {
        std::wstring path = chooseFile(hwnd, L"Snapshot\0*.snap\0All files\0*.*\0",
            L"snap", /*saving=*/false);
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
            WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            // ES_AUTOHSCROLL + WS_HSCROLL disable word-wrap: long lines scroll
            // horizontally instead of wrapping. This keeps ONE logical line == ONE
            // display line, so the gutter's line numbers and the [n] prefixes in
            // the results console (which count logical lines) stay in sync. With
            // wrapping on, a wrapped line counted as 2 display rows in the gutter
            // but 1 logical line when run, drifting the numbers apart.
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN |
            WS_VSCROLL | ES_AUTOVSCROLL | WS_HSCROLL | ES_AUTOHSCROLL,
            0, 0, 0, 0, parent, (HMENU)(INT_PTR)ID_EDIT_COMMANDS, inst, nullptr);

        // Force RichEdit to NOT word-wrap. The documented "line width 0" special
        // case (EM_SETTARGETDEVICE(NULL, 0)) is unreliable in Msftedit.dll and did
        // NOT disable wrapping here. The robust approach is to give a HUGE fixed
        // layout width: RichEdit lays text out as if the page were enormously
        // wide, so no line ever reaches a wrap boundary at any window size. Text
        // then scrolls horizontally (we have WS_HSCROLL/ES_AUTOHSCROLL) instead of
        // wrapping, keeping one logical line == one display line.
        SendMessageW(state->commands, EM_SETTARGETDEVICE, 0, (LPARAM)0x30000);

        // Enable undo with a BOUNDED stack. A fixed limit (100 operations) keeps
        // Ctrl+Z working while capping how much edit history the control retains,
        // so a very long editing session can't grow the undo buffer without limit
        // -- the "overrun" guard. RichEdit's default already allows undo, but
        // setting an explicit limit makes the behaviour deterministic.
        SendMessageW(state->commands, EM_SETUNDOLIMIT, (WPARAM)undoLimit, 0);

        // Fetch the Text Object Model document ONCE (not per-keystroke). We use it
        // to suspend undo recording around syntax highlighting. EM_GETOLEINTERFACE
        // returns an IRichEditOle*; we QueryInterface it for ITextDocument. If any
        // step fails, textDoc stays null and the highlighter simply skips the
        // suspend (undo then behaves as before -- colour ops back on the stack).
        {
            IRichEditOle* richOle = nullptr;
            if (SendMessageW(state->commands, EM_GETOLEINTERFACE, 0,
                (LPARAM)&richOle) && richOle) {
                richOle->QueryInterface(__uuidof(ITextDocument),
                    reinterpret_cast<void**>(&state->textDoc));
                richOle->Release();   // we hold our own ref via textDoc now
            }
        }

        // Results console (bottom).
        state->results = CreateWindowExW(
            WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
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

        // Re-assert no-wrap AFTER resizing the editor. RichEdit re-flows its text
        // to the new client width on resize, which re-enables word-wrapping unless
        // we re-apply the large fixed layout width. Without this, resizing made
        // long lines wrap, changing how many display lines exist and thus which
        // line numbers were visible. The huge width keeps one logical line == one
        // display line at every size. (See createControls for why width 0 fails.)
        SendMessageW(state->commands, EM_SETTARGETDEVICE, 0, (LPARAM)0x30000);

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

            // RichEdit change notifications: recolour ONLY the caret's line (cheap)
            // and repaint the gutter. Full-document highlightSyntax runs only on
            // load, not on every keystroke -- that was the cause of the typing lag.
            if (HIWORD(wParam) == EN_CHANGE && id == ID_EDIT_COMMANDS) {
                highlightCaretLine(state);
                InvalidateRect(state->gutter, nullptr, TRUE);
                return 0;
            }

            // Editor scrolled (ENM_SCROLL was enabled): the first visible line
            // changed, so the gutter's numbers must be redrawn from the new top.
            // Without this the gutter only repainted incidentally and the numbers
            // appeared "stuck" until the next edit/resize.
            if (HIWORD(wParam) == EN_VSCROLL && id == ID_EDIT_COMMANDS) {
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
// runInteractive � the single entry point main() calls (Windows build)
// ---------------------------------------------------------------------------
int runInteractive(int /*argc*/, char* /*argv*/[]) {
    // RichEdit lives in a DLL that must be loaded before its window class
    // (MSFTEDIT_CLASS / "RICHEDIT50W") can be created. Msftedit.dll provides
    // RichEdit 4.1 and registers that class on load.
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

    // Release the cached ITextDocument (COM ref) before the DLL is freed.
    if (state.textDoc) {
        state.textDoc->Release();
        state.textDoc = nullptr;
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