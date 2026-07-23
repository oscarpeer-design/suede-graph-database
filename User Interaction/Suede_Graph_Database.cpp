// Graph Database Oscar.cpp : This file contains the 'main' function. Program execution begins and ends there.

// Included libraries
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
// lower case using std::transform()
#include <algorithm>
#include <cctype>     // std::tolower

// Included header files
#include "../Graph and Searchers/Types.h"
#include "../Graph and Searchers/ErrorCodes.h"
#include "../Graph and Searchers/Graph.h"
#include "../Graph and Searchers/BFS_Searcher.h"
#include "../Graph and Searchers/CSR_Representation.h"
#include "../Graph and Searchers/CSR_Searcher.h"
#include "../Queries and Graph Handlers/Query.h"
#include "../Queries and Graph Handlers/GraphHandler.h"
#include "../Storage/FileHandling.h"
#include "../User Interaction/InteractiveApp.h"   // runInteractive() : the entire GUI behind one call

// test file
#include "../Tests/test_graph_suite.h"

// command modes
enum CommandMode {
    DEBUG,
    INTERACTIVE,
    BATCH,
    UNKNOWN
};

const std::unordered_map<std::string, CommandMode> modes = { {"batch", BATCH}, {"interactive", INTERACTIVE}, {"debug", DEBUG}};

// get commands mode
static CommandMode getMode(std::string sMode) {
    // convert to lowercase. Cast each char to unsigned before std::tolower:
    // passing a negative char (high-bit set) straight to tolower(int) is undefined behaviour.
    std::transform(sMode.begin(), sMode.end(), sMode.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // get value
    auto it = modes.find(sMode);
    if (it == modes.end()) {
        // unknown command
        return UNKNOWN;
    }
    return it->second;
}

// runCommandsFile
// The shared engine of both batch and debug modes: read a commands .txt file,
// run every line, and write each result to a results file. It halts on nothing
// (a failing command is recorded and the run continues), matching the "execute
// every command" contract. `resultsFilePath` is where the record is written; if
// empty, it defaults to the commands file with its ".txt" replaced by ".results".
// Returns 0 on success, non-zero if a file could not be opened.
static int runCommandsFile(const CommandMode mode, const std::string& commandsFilePath, const std::string& resultsFilePath = "") {
    // check file ending is .txt
    if (!endsWithExtension(commandsFilePath, ".txt")) {
        std::cerr << "Error: file must have .txt ending. Instead got '" << commandsFilePath << "'\n";
        return 1;
    }
    // try opening the file to read commands from
    std::ifstream in(commandsFilePath);
    if (!in) {
        std::cerr << "Error: could not open '" << commandsFilePath << "'\n";
        return 1;
    }
    // Results file: use the caller-supplied path if given, else derive it from
    // the commands file name ("foo.txt" -> "foo.results").
    std::string resultsFile = resultsFilePath.empty()
        ? commandsFilePath.substr(0, commandsFilePath.size() - 4) + ".results"
        : resultsFilePath;
    std::ofstream out(resultsFile);  // creates if missing, truncates if exists
    if (!out) {
        std::cerr << "Error: could not open results file '" << resultsFile << "'\n";
        return 1;
    }

    // Build the handler with NO storage engine. The session is purely in-memory:
    // no persistence prompt, no .bin file, no StorageEngine required. Commands
    // like FLUSH / LOAD that would need storage simply return their normal
    // failure result and the run keeps going.
    auto graph = std::make_unique<Graph>();
    GraphHandler handler(std::move(graph), nullptr);

    // read and interpret each line
    std::string line;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        // count this physical line first, so blank/comment lines don't throw
        // off the numbering used in "[lineNo]" reporting.
        ++lineNo;

        // skip blank lines and comments
        if (line.empty())
            continue;
        if (line[0] == '#')
            continue;

        // Parse and run. Every command executes; a failure does NOT halt the
        // run. Both successes and errors are written to the results file.
        // Importantly, if the command passed is BATCH, we don't display outputs
        QueryResult result = handler.executeCommand(line);
        if (!result.success) {
            // only print to console if mode is DEBUG
            if (mode == DEBUG)
                std::cerr << "[" << lineNo << "] ERROR: " << result.message << "\n";
            // output to results file
            out << "[" << lineNo << "] " << line << " -> ERROR: " << result.message << "\n";
        }
        else {
            // only print to console if mode is DEBUG
            if (mode == DEBUG)
                std::cout << result.message << std::endl;
            // output to results file
            out << "[" << lineNo << "] " << line << " -> " << result.message << "\n";
        }
        out.flush();
    }
    return 0;
}

// print the usage/help text to stderr.
static void printUsage(const char* exe) {
    std::cerr
        << "Usage:\n"
        << "  " << exe << " batch <commands.txt> [results.txt]\n"
        << "        Run a commands file headlessly (no prompts). Results are\n"
        << "        written to [results.txt], or to <commands>.results if omitted.\n"
        << "  " << exe << " debug\n"
        << "        Interactive console: prompts for a commands file, then runs it.\n"
        << "  " << exe << " interactive\n"
        << "        Launch the desktop editor (Windows).\n";
}

// testing purposes
//int main() {
//    int iErr = run_tests(true, true);
//    return iErr;
//}

int main(int argc, char* argv[]) {
    // Need at least a mode word.
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // get mode (batch runs a file headlessly; debug prompts; interactive uses GUI)
    std::string sMode = argv[1];
    CommandMode mode = getMode(sMode);

    switch (mode) {
    case BATCH: {
        // Headless: the commands file is a required argument, the results
        // file an optional one. No console prompt -- this is the scriptable
        // entry point (CI, pipelines, subprocess).
        //   Suede batch <commands.txt> [results.txt]
        if (argc < 3) {
            std::cerr << "Error: batch mode requires a commands file.\n\n";
            printUsage(argv[0]);
            return 1;
        }
        // get the commands file from second position
        std::string commandsFile = argv[2];
        // get results file
        std::string resultsFile = (argc >= 4) ? argv[3] : "";
        return runCommandsFile(mode, commandsFile, resultsFile);
    }
    case DEBUG: {
        // Human console flow: prompt for the commands file, then run it.
        std::string commandsFile;
        std::cout << "Enter the name and path of the commands text file: ";
        std::getline(std::cin, commandsFile);
        return runCommandsFile(mode, commandsFile);
    }
    case INTERACTIVE:
        // Launch the desktop UI -- the entire GUI (handler, window, event
        // loop) lives behind this one call.
        return runInteractive(argc, argv);
    default:
        // unknown mode called
        std::cerr << "Unknown mode: " << sMode << "\n\n";
        printUsage(argv[0]);
        return 1;
    }
}