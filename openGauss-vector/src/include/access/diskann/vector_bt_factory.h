/**
 * Copyright ...
 * Hybrid vector index factory interface.
 */

#ifndef VECTOR_BPTREE_FACTORY_H
#define VECTOR_BPTREE_FACTORY_H

#include "access/diskann/vector_bt.h"
#include "access/diskann/vector_bt_diskann.h"
#include "access/diskann/vector_bt_ivf.h"
#include "access/diskann/vector_bt_graph.h"

class VectorIndexFactory {
public:
    static VectorIndex *create(Relation rel, Relation heap, BlockNumber blkno)
    {
        Buffer meta_buf = ReadBuffer(rel, blkno);
        auto meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(BufferGetPage(meta_buf)));
        switch (meta->type) {
            case None:
                return NEW VectorIndex(rel, heap, meta_buf);
            case IVF:
                return NEW IVFVectorIndex(rel, heap, meta_buf);
            case GRAPH:
                return NEW GraphVectorIndex(rel, heap, meta_buf);
            case ANN:
                return NEW DiskAnnVectorIndex(rel, heap, meta_buf);
            default:
                ereport(ERROR, (errmsg("unsupported vector index type: %d", meta->type)));
        }
        return (VectorIndex *)NULL; /* keep compiler quiet */
    }
};

#endif /* VECTOR_BPTREE_FACTORY_H */
