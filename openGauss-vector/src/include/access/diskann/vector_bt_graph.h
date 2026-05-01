/**
 * Copyright ...
 * Graph hibrid index interface.
 */

#ifndef GRAPH_VECTOR_BPTREE_H
#define GRAPH_VECTOR_BPTREE_H

#include <vtl/disk_container/freespace.hpp>
#include "access/diskann/vector_bt.h"
#include "access/diskann/diskann_internal.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/hybridann/bplustree/disk_impl.h"
#include "access/hnsw/hnsw.h"

class GraphVectorIndex : public VectorIndex {
    using super = VectorIndex;
public:
    GraphVectorIndex(Relation rel, Relation heap, Buffer meta_buf)
        : super(rel, heap, meta_buf), _hnsw_so(NULL) {}

    void create_vector_index(vector_pair_vector &data, int parallel_workers,
                             int maintenance_work_mem, bool need_wal)
    {

        Buffer buf = AnnLoadBuffer(_rel, HYBRIDANN_METAPAGE_BLKNO);
        HybridAnnMetaPage *meta = (HybridAnnMetaPage *)(HybridAnnPageGetMeta(BufferGetPage(buf)));

        if (unlikely(meta->magicNumber != HYBRIDANN_MAGIC_NUMBER)) {
            UnlockReleaseBuffer(buf);
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Hybrid index is not valid")));
        }
        int m = (int)meta->m;
        int ef_construction = (int)meta->ef_construction;
        int dimensions = (int)meta->dimensions;
        Metric metric = meta->metric;
        UnlockReleaseBuffer(buf);

        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        _meta->index_meta_blkno = createHnswMetaPage(_rel, MAIN_FORKNUM, m, dimensions,
            ef_construction, metric, QuantizerType::NONE, NULL, DistPrecisionType::FLOAT, need_wal).metablkno;
        MarkBufferDirty(_meta_buf);
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
    }

    void build(vector_pair_vector &vec_data, int parallel_workers, int maintenance_work_mem)
    {
        BlockNumber metablkno = BuildHnswIndex(NULL, _rel, NULL, MAIN_FORKNUM, hybridannGetM(_rel),
            hybridannGetEfConstruction(_rel), QuantizerType::NONE, parallel_workers, maintenance_work_mem, &vec_data);
        update_index_meta_blkno(metablkno);
    }

    void split_to(VectorIndex *new_index, vector_pair_vector &right_data, int parallel_workers)
    {
        split_to_impl(new_index, right_data, parallel_workers);
    }

    void insert(size_t idx, ItemPointerData tid)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_SHARE);
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(BufferGetPage(_meta_buf)));
        uint32_t dimensions = meta->dim;
        BlockNumber metablkno = meta->index_meta_blkno;
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);

        Datum values[1];
        bool isnull[1] = {false};
        FloatVector *vector = InitFloatVector((int)dimensions);
        VecBuffer buffer = vec_read_buffer(_rel, idx, dimensions * sizeof(float));
        errno_t rc = memcpy_s(vector->x, sizeof(float) * dimensions, buffer.get_vecbuf(), sizeof(float) * dimensions);
        buffer.release();
        securec_check(rc, "", "");
        values[0] = PointerGetDatum(vector);
        hnswinsert_internal(_rel, _heap, values, isnull, &tid, metablkno, idx);
        pfree(vector);
    }

    void batch_insert(vector_pair_vector &data, int parallel_workers)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_SHARE);
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(BufferGetPage(_meta_buf)));
        uint32_t dimensions = meta->dim;
        BlockNumber metablkno = meta->index_meta_blkno;
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);

        const auto insertTask = [this, &data, &dimensions, &metablkno](size_t batchIndex, size_t start, size_t end) {
            Oid base_relid;
            Oid part_id = InvalidOid;
            Relation target;
            Relation parent;
            Partition part;

            if (IsBgWorkerProcess()) {
                if (RelationIsPartition(_rel)) {
                    base_relid = GetBaseRelOidOfParition(_rel);
                    part_id = RelationGetRelid(_rel);
                } else {
                    base_relid = RelationGetRelid(_rel);
                }

                if (part_id == InvalidOid) {
                    target = index_open(base_relid, NoLock);
                } else {
                    parent = index_open(base_relid, NoLock);
                    part = partitionOpen(parent, part_id, NoLock);
                    target = partitionGetRelation(parent, part);
                }
                RelationOpenSmgr(target);
            } else {
                target = _rel;
            }

            Datum values[1];
            bool isnull[1] = {false};
            FloatVector *vector = InitFloatVector((int)dimensions);
            for (size_t j = start; j < end; ++j)
            {
                CHECK_FOR_INTERRUPTS();
                VecBuffer buffer = vec_read_buffer(target, data[j].vid, dimensions * sizeof(float));
                errno_t rc = memcpy_s(vector->x, sizeof(float) * dimensions, buffer.get_vecbuf(), sizeof(float) * dimensions);
                buffer.release();
                securec_check(rc, "", "");
                values[0] = PointerGetDatum(vector);
                hnswinsert_internal(target, _heap, values, isnull, &data[j].tid, metablkno, data[j].vid);
            }

            pfree(vector);

            if (IsBgWorkerProcess()) {
                index_close(target, NoLock);
                if (part_id != InvalidOid) {
                    partitionClose(parent, part, NoLock);
                    index_close(parent, NoLock);
                }
            }
        };

		INIT_TASK_RUNNER();
		if (parallel_workers > 0) {
			LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(parallel_workers);
		}
		START_TASK_POOL();
		PARALLEL_BATCH_RUN_INIT();

        int totalParaWorkers = parallel_workers + 1;
        PARALLEL_BATCH_RUN_TASK_WAIT(data.size(), totalParaWorkers, insertTask);
		WAIT_AND_END_TASK_POOL();
		DESTROY_TASK_RUNNER();
    }

    void vacuum(IndexBulkDeleteCallback callback, const void *callback_state, IdxSet &delete_set, int parallel_workers)
    {
        BlockNumber metablkno = get_meta_blkno();
        [[maybe_unused]]
        IndexBulkDeleteResult *stats = hnswbulkdelete_internal(_rel, NULL, parallel_workers,
            callback, (void *)callback_state, metablkno, &delete_set);
        pfree_ext(stats);
    }

    void recycle_to_fsm()
    {
        RecordFreeIndexPage(_rel, index_meta_blkno());
        free_index_meta_pages();
    }

    size_t search(IndexScanDesc scan, float *dist_out, ItemPointerData *iptr, void *param)
    {
        VectorIndexNoneSearchParam *search_param = (VectorIndexNoneSearchParam *)param;
        uint32 ef_search = u_sess->attr.attr_storage.ef_search;
        ef_search = std::max(search_param->top_k, ef_search);
        if (!_hnsw_so) {
            _hnsw_so = create_hnsw_scanopaque(scan->indexRelation);
        }
        size_t count = 0;
        while (count < search_param->top_k && hnswgettuple_internal(scan, _hnsw_so, get_meta_blkno(), (int)ef_search, dist_out + count))
        {
            iptr[count] = scan->xs_ctup.t_self;
            ++count;
        }
        return count;
    }

    void destroy()
    {
        if (_hnsw_so) {
            free_hnsw_scanopaque(_hnsw_so);
            _hnsw_so = NULL;
        }
        super::destroy();
    }

private:
    void *_hnsw_so;
};

#endif /* GRAPH_VECTOR_BPTREE_H */
