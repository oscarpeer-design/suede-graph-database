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

// Load Graph into memory
bool StorageEngine::Load(Graph& graph)
{
    // open graph file for reading
    if (!OpenInputFile())
        return false;

    // From here the file is OPEN: every exit path must close it. Previously only
    // the success path called CloseInputFile(), so a failed load (e.g. a bad or
    // wrong-format header) left the shared graphFile stream open with its error
    // bits set -- and a subsequent Save could not re-open it, making the file
    // permanently un-writable for the rest of the session. `ok` + a single close
    // at the end guarantees the handle is always released.
    bool ok = ReadHeader()      // verify file header
        && ReadNodes(graph)     // reconstruct nodes
        && ReadEdges(graph)     // reconstruct edges
        && ValidateGraph(graph); // verify graph integrity

    // Always close, whether we succeeded or failed.
    CloseInputFile();

    return ok;
}

// save Graph
bool StorageEngine::Save(const Graph& graph)
{
    // open graph file for writing
    if (!OpenOutputFile())
        return false;

    // As with Load: the file is open now, so close on EVERY exit path. A failed
    // write otherwise leaked the handle and blocked all later saves/loads.
    bool ok = WriteHeader(graph)  // write file header
        && WriteNodes(graph)      // write every node
        && WriteEdges(graph);     // write every edge

    // Always flush and close, whether we succeeded or failed.
    CloseOutputFile();

    return ok;
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