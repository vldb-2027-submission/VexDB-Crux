#include "access/hybridann/hybridann.h"
#include "access/hybridann/bplustree/disk_bplustree.h"

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *hybridannbulkdelete_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                                                  IndexBulkDeleteCallback callback, void *callback_state)
{
    RelationOpenSmgr(info->index);
    if (!stats) {
        stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
    }

    Buffer buf = AnnLoadBuffer(info->index, HYBRIDANN_METAPAGE_BLKNO);
    HybridAnnMetaPage *meta = HybridAnnPageGetMeta(BufferGetPage(buf));

    if (unlikely(meta->magicNumber == DISKANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Old Hybrid index data is not supported. Please REINDEX.")));
    }

    if (unlikely(meta->magicNumber != HYBRIDANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Hybridann index is not valid")));
    }

    BlockNumber btMetaPageBlkno = meta->BTMetaBlkNo;
    UnlockReleaseBuffer(buf);

    disk_container::DiskBPlusTree btree(info->index, info->index->parent, btMetaPageBlkno, true);
    btree.vacuumVectorIndexes(callback, callback_state);
    btree.bulkdelete(info, stats, callback, callback_state);
    btree.destroy();
    
    return stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *hybridannvacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    Relation rel = info->index;

    if (info->analyze_only) {
        return stats;
    }

    /* stats is NULL if ambulkdelete not called */
    /* OK to return NULL if index not changed */
    if (!stats) {
        return NULL;
    }

    /* Finally, vacuum the FSM */
    IndexFreeSpaceMapVacuum(rel);

    stats->num_pages = RelationGetNumberOfBlocks(rel);

    return stats;
}
