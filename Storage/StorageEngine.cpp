#include "StorageEngine.h"

// constructor
StorageEngine::StorageEngine(std::string path) : graphFilePath(path), nodeCount(0), edgeCount(0) {}

// ValidateGraph
// verifies that the reconstructed graph matches the file header
bool StorageEngine::ValidateGraph(const Graph& graph)
{
    // get all nodes
    std::vector<NodeId> nodeIds;
    graph.GetNodeIdOrder(nodeIds);

    // incorrect number of nodes
    if (nodeIds.size() != nodeCount)
        return false;

    // get all edges
    std::vector<EdgeId> edgeIds;
    graph.GetAllEdgeIds(edgeIds);

    // incorrect number of edges
    if (edgeIds.size() != edgeCount)
        return false;

    // graph reconstructed successfully
    return true;
}

// Load Graph into memory.
//
// The load funnel, cheapest gate first, so a corrupt file dies early and cheap:
//   1. magic + version           (ReadHeader)          -- is this even our format?
//   2. streaming bounds-checks   (ReadNodes/ReadEdges) -- every length/index sane?
//   3. CRC32 trailer             (verified below)      -- bytes intact since save?
//   4. count match               (ValidateGraph)       -- reconstructed == header?
//
// CRITICAL: we reconstruct into a TEMPORARY graph and only swap it into the
// caller's `graph` if EVERY gate passes. A rejected load therefore leaves the
// caller's existing in-memory graph completely untouched -- a bad file can never
// wipe good data. (The old code reconstructed directly into `graph`, so a load
// that failed halfway left it half-built.)
bool StorageEngine::Load(Graph& graph)
{
    // open graph file for reading
    if (!OpenInputFile())
        return false;

    // Set up the payload byte budget: total file size, minus the header, minus
    // the 4-byte CRC trailer. Every read is bounds-checked against this so a
    // corrupt length can't over-read or over-allocate.
    graphFile.seekg(0, std::ios::end);
    std::streamoff fileSize = graphFile.tellg();
    graphFile.seekg(0, std::ios::beg);
    const std::streamoff overhead =
        (std::streamoff)sizeof(FileHeader) + (std::streamoff)sizeof(uint32_t);
    if (fileSize < overhead) { CloseInputFile(); return false; }  // too small to be valid
    payloadRemaining = (uint64_t)(fileSize - overhead);

    // Fresh CRC accumulator for this read pass.
    crc = 0xFFFFFFFFu;

    // Reconstruct into a TEMP graph; the caller's graph is untouched until success.
    Graph temp;
    bool ok = ReadHeader()          // gate 1: magic + version (raw, not CRC'd)
        && ReadNodes(temp)          // gate 2a: nodes, bounds-checked, CRC folded
        && ReadEdges(temp);         // gate 2b: edges, bounds-checked, CRC folded

    // gate 3: after the payload, the remaining bytes must be exactly the 4-byte
    // trailer, and our recomputed CRC must equal it. `payloadRemaining` should be
    // 0 here (we consumed exactly the payload); anything else means a size mismatch.
    if (ok) {
        if (payloadRemaining != 0) {           // payload didn't line up with the file
            ok = false;
        }
        else {
            uint32_t storedCrc = 0;
            graphFile.read(reinterpret_cast<char*>(&storedCrc), sizeof(storedCrc));
            uint32_t computed = crc ^ 0xFFFFFFFFu;   // finalise
            if (!graphFile.good() || computed != storedCrc)
                ok = false;                     // truncated trailer or CRC mismatch
        }
    }

    // gate 4: reconstructed counts match the header's promise.
    if (ok) 
        ok = ValidateGraph(temp);

    // Always close the input, whether we succeeded or failed.
    CloseInputFile();

    // Only on FULL success do we hand the reconstructed graph to the caller.
    if (ok) 
        graph = std::move(temp);
    return ok;
}

// save Graph -- ATOMIC. Never overwrites the target file in place; instead it
// writes the whole thing to a sibling temp file, and only once that has fully
// succeeded (data + CRC trailer flushed) does it RENAME the temp over the target.
// The rename is atomic on the filesystem, so at every instant the target file is
// either the complete OLD graph or the complete NEW graph -- never a half-written
// mixture. If anything fails before the rename, the target is left UNTOUCHED and
// the temp is discarded, so a failed flush can never destroy existing good data.
bool StorageEngine::Save(const Graph& graph)
{
    const std::string finalPath = graphFilePath;
    const std::string tempPath = TempPath();

    // Write to the TEMP path (retarget the Open helpers by swapping graphFilePath).
    graphFilePath = tempPath;

    if (!OpenOutputFile()) { graphFilePath = finalPath; return false; }

    // Fresh CRC accumulator for the payload (header is not part of the CRC).
    crc = 0xFFFFFFFFu;

    bool ok = WriteHeader(graph)    // header (raw, outside the CRC)
        && WriteNodes(graph)        // nodes (CRC folded via crcWrite)
        && WriteEdges(graph);       // edges (CRC folded via crcWrite)

    // Append the 4-byte CRC trailer over the payload just written.
    if (ok) {
        uint32_t finalCrc = crc ^ 0xFFFFFFFFu;   // finalise
        graphFile.write(reinterpret_cast<const char*>(&finalCrc), sizeof(finalCrc));
        ok = graphFile.good();
    }

    // Flush + close the temp file so the bytes are committed before the rename.
    CloseOutputFile();

    // Restore the engine's real target path regardless of outcome.
    graphFilePath = finalPath;

    if (!ok) {
        // Writing the temp failed: discard it and leave the target untouched.
        std::remove(tempPath.c_str());
        return false;
    }

    // Atomic swap: temp -> target. std::rename replaces the destination on POSIX;
    // on Windows, replacing an existing file needs a remove-then-rename (std::rename
    // fails if the destination exists). We try the direct rename first and fall back.
    if (std::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        // Windows / destination-exists path: remove the old target, then rename.
        std::remove(finalPath.c_str());
        if (std::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
            std::remove(tempPath.c_str());   // give up cleanly; temp not left behind
            return false;
        }
    }
    return true;
}

// Load the live graph from an explicit runtime-chosen path.
// Retarget graphFilePath (all the private Open/Read helpers read this member),
// then delegate to the existing Load. The path persists as the current file.
bool StorageEngine::Load(Graph& graph, const std::string& path)
{
    graphFilePath = path;
    return Load(graph);
}

// Save the live graph to an explicit runtime-chosen path (same approach).
bool StorageEngine::Save(const Graph& graph, const std::string& path)
{
    graphFilePath = path;
    return Save(graph);
}

// Import CSV
bool StorageEngine::ImportCSV(Graph& graph, const std::string& csvFilePath)
{
    // open csv file
    if (!OpenCSV(csvFilePath))
        return false;

    // import all nodes
    if (!ImportNodes(graph))
        return false;

    // import all edges
    if (!ImportEdges(graph))
        return false;

    // close csv file
    CloseCSV();

    return true;
}

// Export CSV
bool StorageEngine::ExportCSV(const Graph& graph, const std::string& csvFilePath)
{
    // create csv file
    if (!CreateCSV(csvFilePath))
        return false;

    // export nodes
    if (!ExportNodes(graph))
        return false;

    // export edges
    if (!ExportEdges(graph))
        return false;

    // close csv file
    CloseCSV();

    return true;
}

// SaveSnapshot
// Serializes a CSR_Representation to disk in binary format.
// Format:
//   [snapshotVersion:uint64_t]
//   [nodeCount:uint64_t]
//   [csrToNode:NodeId[nodeCount]]
//   [rowOffsetsSize:uint64_t]
//   [rowOffsets:size_t[rowOffsetsSize]]
//   [columnsSize:uint64_t]
//   [columns:size_t[columnsSize]]
bool StorageEngine::SaveSnapshot(const CSR_Representation& snapshot, const std::string& snapshotFilePath)
{
    // open snapshot file for writing
    std::fstream snapshotFile;
    snapshotFile.open(snapshotFilePath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!snapshotFile.is_open())
        return false;

    // write snapshot version
    uint64_t version = snapshot.GetSnapshotVersion();
    snapshotFile.write(reinterpret_cast<const char*>(&version), sizeof(uint64_t));

    // get the node mapping (CSR ID -> NodeId)
    const std::vector<NodeId>& csrToNode = snapshot.GetCSRNodeMapping();

    // write node count
    uint64_t nodeCount = csrToNode.size();
    snapshotFile.write(reinterpret_cast<const char*>(&nodeCount), sizeof(uint64_t));

    // write node IDs (directly as NodeId, respecting type constraints)
    for (const NodeId& nodeId : csrToNode)
    {
        snapshotFile.write(reinterpret_cast<const char*>(&nodeId), sizeof(NodeId));
    }

    // get row offsets
    const std::vector<size_t>& rowOffsets = snapshot.GetRowOffsets();

    // write row offsets size
    uint64_t rowOffsetsSize = rowOffsets.size();
    snapshotFile.write(reinterpret_cast<const char*>(&rowOffsetsSize), sizeof(uint64_t));

    // write row offsets
    for (size_t offset : rowOffsets)
    {
        snapshotFile.write(reinterpret_cast<const char*>(&offset), sizeof(size_t));
    }

    // get columns (adjacency list)
    const std::vector<size_t>& columns = snapshot.GetColumns();

    // write columns size
    uint64_t columnsSize = columns.size();
    snapshotFile.write(reinterpret_cast<const char*>(&columnsSize), sizeof(uint64_t));

    // write columns
    for (size_t col : columns)
    {
        snapshotFile.write(reinterpret_cast<const char*>(&col), sizeof(size_t));
    }

    // check if write succeeded
    bool success = snapshotFile.good();

    // close file
    snapshotFile.close();

    return success;
}

// LoadSnapshot
// Deserializes a CSR_Representation from disk. Note: the snapshot object
// is NOT connected to a graph (owner_ stays null). This is a frozen view
// for read-only access. To use it for queries, the caller must integrate
// it with a live Graph.
bool StorageEngine::LoadSnapshot(CSR_Representation& snapshot, const std::string& snapshotFilePath)
{
    // open snapshot file for reading
    std::fstream snapshotFile;
    snapshotFile.open(snapshotFilePath, std::ios::binary | std::ios::in);

    if (!snapshotFile.is_open())
        return false;

    // read snapshot version
    uint64_t version;
    snapshotFile.read(reinterpret_cast<char*>(&version), sizeof(uint64_t));
    if (!snapshotFile.good())
    {
        snapshotFile.close();
        return false;
    }

    // read node count
    uint64_t nodeCount;
    snapshotFile.read(reinterpret_cast<char*>(&nodeCount), sizeof(uint64_t));
    if (!snapshotFile.good())
    {
        snapshotFile.close();
        return false;
    }

    // read node IDs into csrToNode
    std::vector<NodeId> csrToNode;
    csrToNode.reserve(nodeCount);
    for (uint64_t i = 0; i < nodeCount; ++i)
    {
        NodeId nodeId;
        snapshotFile.read(reinterpret_cast<char*>(&nodeId), sizeof(NodeId));
        if (!snapshotFile.good())
        {
            snapshotFile.close();
            return false;
        }
        csrToNode.push_back(nodeId);
    }

    // read row offsets size
    uint64_t rowOffsetsSize;
    snapshotFile.read(reinterpret_cast<char*>(&rowOffsetsSize), sizeof(uint64_t));
    if (!snapshotFile.good())
    {
        snapshotFile.close();
        return false;
    }

    // read row offsets
    std::vector<size_t> rowOffsets;
    rowOffsets.reserve(rowOffsetsSize);
    for (uint64_t i = 0; i < rowOffsetsSize; ++i)
    {
        size_t offset;
        snapshotFile.read(reinterpret_cast<char*>(&offset), sizeof(size_t));
        if (!snapshotFile.good())
        {
            snapshotFile.close();
            return false;
        }
        rowOffsets.push_back(offset);
    }

    // read columns size
    uint64_t columnsSize;
    snapshotFile.read(reinterpret_cast<char*>(&columnsSize), sizeof(uint64_t));
    if (!snapshotFile.good())
    {
        snapshotFile.close();
        return false;
    }

    // read columns
    std::vector<size_t> columns;
    columns.reserve(columnsSize);
    for (uint64_t i = 0; i < columnsSize; ++i)
    {
        size_t col;
        snapshotFile.read(reinterpret_cast<char*>(&col), sizeof(size_t));
        if (!snapshotFile.good())
        {
            snapshotFile.close();
            return false;
        }
        columns.push_back(col);
    }

    // close file
    snapshotFile.close();

    // Reconstruct snapshot using setters
    snapshot.SetSnapshotVersion(version);
    snapshot.SetCSRNodeMapping(std::move(csrToNode));
    snapshot.SetRowOffsets(std::move(rowOffsets));
    snapshot.SetColumns(std::move(columns));

    return true;
}