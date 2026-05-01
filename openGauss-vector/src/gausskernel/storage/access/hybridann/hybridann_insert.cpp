#include "access/hybridann/hybridann.h"
#include "access/hybridann/bplustree/disk_bplustree.h"

static size_t reserve_global_location(Relation rel, HybridAnnMetaPage *meta, float *query, ItemPointer heap_tid)
{
    auto preprocessor = ann_helper::get_vector_preprocess_func(meta->metric);
    if (preprocessor) {
        preprocessor(query, meta->dimensions, query);
    }

    disk_container::FreeSpace<size_t> free_space(rel, meta->freespaceMetaBlkNo);

    size_t loc;
    bool success = free_space.pop(loc);
    free_space.destroy();
    if (success) {
        LogManager logmgr(rel);
        logmgr.log_write_vector(loc * meta->dimensions * sizeof(float), meta->dimensions * sizeof(float), (char *)query, true);
        logmgr.destroy();
        write_vector(rel, loc, meta->dimensions * sizeof(float), (char *)query);
        return loc;
    }
    
    loc = push_back_vector(rel, meta->dataMetaBlkNo, query, meta->dimensions);

    return loc;
}




/*
 * Insert a tuple into the index
 */
bool hybridanninsert_internal(Relation index, Datum *values, const bool *isnull, ItemPointer heap_tid,
                            Relation heap, IndexUniqueCheck checkUnique)
{
    if (*isnull) { /* null not handled */
        return false;
    }
    RelationOpenSmgr(index);

    Buffer buf = AnnLoadBuffer(index, HYBRIDANN_METAPAGE_BLKNO);
    HybridAnnMetaPage *meta = HybridAnnPageGetMeta(BufferGetPage(buf));

    if (unlikely(meta->magicNumber == DISKANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Old Hybrid index data is not supported. Please REINDEX.")));
    }
    if (unlikely(meta->magicNumber != HYBRIDANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Hybridann index is not valid")));
    }

    FloatVector *vector = DatumGetFloatVector(*values);
    float *query = vector->x;

    disk_container::DiskBPlusTree btree(index, heap, meta->BTMetaBlkNo, true);
    int nkeys = RelationGetDescr(index)->natts;
    Datum *nvalues = (Datum*)palloc(nkeys * sizeof(Datum));
    bool* nisnull = (bool*)palloc(nkeys * sizeof(bool));
    nvalues[0] = reserve_global_location(index, meta, query, heap_tid);
    nisnull[0] = false;
    for(int i = 1; i < nkeys; ++i) {
        nvalues[i] =  values[i];
        nisnull[i] =  isnull[i];
    }
    UnlockReleaseBuffer(buf);
    IndexTuple itup = index_form_tuple(btree.get_tupDesc(), nvalues, nisnull);
    itup->t_tid = *heap_tid;
    btree.insert(reinterpret_cast<disk_container::BTTupleData *>(itup));
    pfree(nvalues);
    pfree(nisnull);
    pfree(itup);
    btree.destroy();
    

    /* release mem used for detoasting by DatumGetFloatVector */
    if ((void *)vector != DatumGetPointer(*values)) {
        pfree(vector);
    }

    return true;
}
