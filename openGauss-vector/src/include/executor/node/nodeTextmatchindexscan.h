/**
 * Copyright ...
 */

#ifndef NODE_TEXT_MATCH_INDEXSCAN_H
#define NODE_TEXT_MATCH_INDEXSCAN_H

#include "nodes/execnodes.h"

extern TextMatchIndexScanState* ExecInitTextMatchIndexScan(TextMatchIndexScan* node, EState* estate, int eflags);
extern void ExecEndTextMatchIndexScan(TextMatchIndexScanState* state);
extern void ExecTextMatchIndexMarkPos(TextMatchIndexScanState* state);
extern void ExecTextMatchIndexRestrPos(TextMatchIndexScanState* state);
extern void ExecReScanTextMatchIndexScan(TextMatchIndexScanState* state);

#endif /* NODE_TEXT_MATCH_INDEXSCAN_H */
