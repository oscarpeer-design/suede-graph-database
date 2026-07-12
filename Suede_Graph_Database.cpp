// Graph Database Oscar.cpp : This file contains the 'main' function. Program execution begins and ends there.

// Included libraries
#include <iostream>

// Included header files
#include "Types.h"
#include "ErrorCodes.h"
#include "Graph.h"
#include "BFS_Searcher.h"
#include "CSR_Representation.h"
#include "CSR_Searcher.h"
#include "Query.h"
#include "GraphHandler.h"

// test file
#include "test_graph_suite.h"

// check correct file extension 
static bool endsWithExtension(const std::string& path, const std::string& ext) {
    // determine if the file ends with a .txt
    return path.size() >= ext.size() &&
        path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

// Initialize storage engine based on user input
// Returns a unique_ptr to StorageEngine, or nullptr if user chooses no persistence
// or provides an invalid file path
static std::unique_ptr<StorageEngine> initializeStorage() {
    // get name of <graph>.bin file from user
    std::string graphFile;
    std::cout << "Enter the path of the graph file for persistence (leave blank for no persistence): ";
    std::getline(std::cin, graphFile);
    // if empty, no persistence
    if (graphFile.empty()) {
        return nullptr;
    }
    // check for .bin extension
    if (!endsWithExtension(graphFile, ".bin")) {
        std::cerr << "Error: graph file must have .bin extension. Got '" << graphFile << "'\n";
        return nullptr;
    }
    // create and return storage engine
    return std::make_unique<StorageEngine>(graphFile);
}

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

    // build the handler once, reuse across all lines
    auto graph = std::make_unique<Graph>();
    auto storage = initializeStorage();
    GraphHandler handler(std::move(graph), std::move(storage));
    // read and interpret each line
    std::string line;
    size_t lineNo = 1;
    bool errorFound = false; // halt execution on first error
    while (std::getline(in, line) && !errorFound) {
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
        // increment line number
        lineNo ++;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    // check we habe two arguments
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
        // launch UI
    }
    // Unknown
    else {
        std::cerr << "Unknown mode: " << mode << "\n";
        return 1;
    }

    return 0;
}