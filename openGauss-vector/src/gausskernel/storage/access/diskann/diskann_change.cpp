#include "access/diskann/diskann_internal.h"
#include "access/annvector/ann_utils.h"
#include "storage/indexfsm.h"

/*
 * Insert a tuple into the index
 */
bool diskanninsert_internal(Relation index, Datum *values, const bool *isnull, ItemPointer heap_tid,
                            Relation heap, IndexUniqueCheck checkUnique)
{
    if (*isnull) { /* null not handled */
        return false;
    }
    RelationOpenSmgr(index);

    Buffer buf = AnnLoadBuffer(index, DISKANN_METAPAGE_BLKNO);
    DiskAnnMetaPageBase *meta = DiskAnnPageGetMeta(BufferGetPage(buf));
    if (unlikely(meta->magicNumber != DISKANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("diskann index is not valid")));
    }

    FloatVector *vector = DatumGetFloatVector(*values);
    float *query = vector->x;

    if (meta->version == DISKANN_VERSION_ONE) {
        UnlockReleaseBuffer(buf);
        DiskANNIndex ann_index(index);
        bool my_isnull[index->rd_att->natts];
        my_isnull[0] = true;
        for (int i = 1; i < index->rd_att->natts; ++i) {
            my_isnull[i] = isnull[i];
        }
        ann_index.insert_point(query, *heap_tid, values, my_isnull);
        ann_index.destroy();
    } else if (meta->version == DISKANN_VERSION_TWO) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Old Hybrid index data is not supported. Please upgrade database and reindex.")));
    } else {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("unexpected diskann meta page version number: %d", meta->version)));
    }

    /* release mem used for detoasting by DatumGetFloatVector */
    if ((void *)vector != DatumGetPointer(*values)) {
        pfree(vector);
    }

    return true;
}

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *diskannbulkdelete_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                                                  IndexBulkDeleteCallback callback, void *callback_state)
{
    RelationOpenSmgr(info->index);
    if (!stats) {
        stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
    }

    Buffer buf = AnnLoadBuffer(info->index, DISKANN_METAPAGE_BLKNO);
    DiskAnnMetaPageBase *meta = DiskAnnPageGetMeta(BufferGetPage(buf));
    if (unlikely(meta->magicNumber != DISKANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("diskann index is not valid")));
    }

    if (meta->version == DISKANN_VERSION_TWO) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Old Hybrid index data is not supported. Please upgrade database and reindex.")));

    } else {
        UnlockReleaseBuffer(buf);
    }

    /* 1. cache all deleted points into set for faster judge */
    DiskANNIndex ann_index(info->index);
    size_t total_size;
    IdxSet delete_set;
    ann_index.get_deleted_point_idx(callback, callback_state, delete_set, total_size);
    /* 2. iterate all undeleted point to update their neighbors */
    ann_index.consolidate_all_points(total_size, delete_set);
    /* 3. invalidate all points and recollect */
    auto result = ann_index.retrieve_deleted_slots(total_size, delete_set);
    ann_helper::optional_destroy(delete_set);
    ann_index.destroy();

    stats->estimated_count = true;
    stats->num_index_tuples = result.num_point_remained;
    stats->tuples_removed = result.num_point_deleted;
    stats->num_pages = result.num_page_remained;
    stats->pages_deleted = 0;
    stats->pages_removed = 0;
    stats->pages_free = result.num_page_freed;
    return stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *diskannvacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
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
