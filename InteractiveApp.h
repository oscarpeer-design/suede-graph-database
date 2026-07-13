// InteractiveApp.h
#pragma once

/// runInteractive
///
/// The single entry point for interactive (GUI) mode. main() calls exactly this
/// for the "interactive" argument and nothing else -- all Win32 usage, the
/// storage prompt, graph/handler construction, and the message loop live behind
/// here, so the main translation unit never needs to include <windows.h>.
///
/// On Windows it:
///   1. prompts for persistence via initializeStorage() (same as batch mode),
///   2. builds the Graph and the GraphHandler,
///   3. creates a window with a RichEdit command box, a Run button, and a
///      read-only RichEdit results box (errors shown inline in red),
///   4. runs the Win32 message loop until the window is closed.
///
/// On non-Windows platforms it prints a notice and returns non-zero, so the
/// rest of the project (batch mode, the engine, tests) still builds and runs.
///
/// Returns the process exit code (0 on normal quit).
int runInteractive(int argc, char* argv[]);