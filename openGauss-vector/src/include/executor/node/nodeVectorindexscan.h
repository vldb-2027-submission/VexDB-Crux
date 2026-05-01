/**
 * Copyright ...
 */

#ifndef NODE_VECTOR_INDEXSCAN_H
#define NODE_VECTOR_INDEXSCAN_H

#include "nodes/execnodes.h"

extern VectorIndexScanState* ExecInitVectorIndexScan(VectorIndexScan* node, EState* estate, int eflags);
extern void ExecEndVectorIndexScan(VectorIndexScanState* state);
extern void ExecVectorIndexMarkPos(VectorIndexScanState* state);
extern void ExecVectorIndexRestrPos(VectorIndexScanState* state);
extern void ExecReScanVectorIndexScan(VectorIndexScanState* state);

#endif /* NODE_VECTOR_INDEXSCAN_H */
