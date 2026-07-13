// Graph Database Oscar.cpp : This file contains the 'main' function. Program execution begins and ends there.

// Included libraries
#include <iostream>
#include <fstream>
#include <memory>
#include <string>

// Included header files
#include "Types.h"
#include "ErrorCodes.h"
#include "Graph.h"
#include "BFS_Searcher.h"
#include "CSR_Representation.h"
#include "CSR_Searcher.h"
#include "Query.h"
#include "GraphHandler.h"
#include "FileHandling.h"
#include "InteractiveApp.h"   // runInteractive() — the entire GUI behind one call

// test file
#include "test_graph_suite.h"


// openAndRunCommandsFile
static int openAndRunCommandsFile(const std::string& commandsFilePath) {
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
    // try building the file to write results to
    std::string resultsFile = commandsFilePath.substr(0, commandsFilePath.size() - 4) + ".results";
    std::ofstream out(resultsFile);  // creates if missing, truncates if exists
    if (!out) {
        std::cerr << "Error: could not open results file '" << resultsFile << "'\n";
        return 1;
    }

    // Ask about persistence up front, before we start streaming results, so the
    // storage prompt doesn't appear in the middle of batch execution.
    auto storage = initializeStorage();

    // build the handler once, reuse across all lines
    auto graph = std::make_unique<Graph>();
    GraphHandler handler(std::move(graph), std::move(storage));

    // read and interpret each line
    std::string line;
    size_t lineNo = 0;
    bool errorFound = false; // halt execution on first error
    while (std::getline(in, line) && !errorFound) {
        // count this physical line first, so blank/comment lines don't throw
        // off the numbering used in "[lineNo] ERROR" reporting.
        ++lineNo;

        // skip blank lines and comments
        if (line.empty())
            continue;
        if (line[0] == '#')
            continue;

        // parse and run
        QueryResult result = handler.executeCommand(line);
        if (!result.success) {
            // errors to console only (immediate feedback)
            std::cerr << "[" << lineNo << "] ERROR: " << result.message << "\n";
            errorFound = true;
        }
        else {
            // output results
            std::cout << result.message;
            // add results to file (permanent record)
            out << "[" << lineNo << "] " << line << " -> " << result.message << "\n";
            out.flush();
        }
    }

    return 0;
}

// testing purposes
//int main() {
//    int iErr = run_tests();
//    return iErr;
//}

int main(int argc, char* argv[]) {
    // check we have two arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <batch|interactive>\n";
        return 1;
    }
    // get mode (batch spawns CLI whereas interactive spawns desktop UI)
    std::string mode = argv[1];

    if (mode == "batch") {
        // get commands file from the user
        std::string commandsFile = "";
        std::cout << "Enter the name and path of the commands text file: ";
        std::getline(std::cin, commandsFile);
        // open the file and begin running
        int iErr = openAndRunCommandsFile(commandsFile);
        if (iErr != 0)
            return iErr;
    }
    else if (mode == "interactive") {
        // launch the desktop UI — the entire GUI (storage prompt, handler,
        // window, event loop) lives behind this one call.
        return runInteractive(argc, argv);
    }
    // Unknown
    else {
        std::cerr << "Unknown mode: " << mode << "\n";
        return 1;
    }

    return 0;
}

