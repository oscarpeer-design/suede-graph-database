#pragma once
// error codes
static const int
// success
operationSuccessful = 0,
// nodes and edges in raw graph
errEdgeDoesntExist = -1,
errNodeDoesntExist = -2,
errNodeLabelUndocumented = -3,
errEdgeListUndocumented = -4,
// csr-related issues
errCsrNotMappedForThisNode = -5,
errNodeNotMappedForThisCsr = -6;