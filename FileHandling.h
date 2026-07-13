#pragma once

#include <string>
#include <iostream>
#include <memory>

#include "StorageEngine.h"   // StorageEngine (constructed below)

// NOTE: the two helpers below are defined in this header and included by more
// than one translation unit (Suede_Graph_Database.cpp and InteractiveApp.cpp).
// They are marked `inline` so the linker merges the duplicate definitions
// instead of reporting a "multiple definition" error.

// check correct file extension
inline bool endsWithExtension(const std::string& path, const std::string& ext) {
    // determine if the file ends with the given extension
    return path.size() >= ext.size() &&
        path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

// Initialize storage engine based on user input
// Returns a unique_ptr to StorageEngine, or nullptr if user chooses no persistence
// or provides an invalid file path
inline std::unique_ptr<StorageEngine> initializeStorage() {
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