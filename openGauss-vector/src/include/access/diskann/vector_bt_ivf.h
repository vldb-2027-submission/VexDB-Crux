/**
 * Copyright ...
 * IVF hybrid vector index iterface.
 */

#ifndef VECTOR_BPTREE_IVF_H
#define VECTOR_BPTREE_IVF_H

#include <vtl/disk_container/freespace.hpp>
#include "access/diskann/vector_bt.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/annvector/ivf.h"
#include "access/index_backend/taskpool.h"
#include "postmaster/bgworker.h"
#include "catalog/pg_partition_fn.h"
#include "access/diskann/diskann.h"

class IVFVectorIndex : public VectorIndex {
    using super = VectorIndex;
public:
    IVFVectorIndex(Relation rel, Relation heap, Buffer meta_buf)
        : super(rel, heap, meta_buf), _ivf_so(NULL), _tupDesc(NULL)
    {
        // the hybrid func metric should correctly mappedd to ivf func metric.
        FmgrInfo *tempprocinfo = hybridAnnOptionalProcInfo(_rel, HYBRIDANN_DISTANCE_PROC);
        _ivf_metric = get_func_metric(tempprocinfo->fn_oid);
    }

    void create_vector_index(vector_pair_vector &data, int parallel_workers, int maintenance_work_mem, bool need_wal)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        IvfBuildState buildstate;
        buildstate.metric = _ivf_metric;
        BlockNumber meta_blkno = IvfflatComputeCenters(NULL, _rel, NULL, &buildstate, MAIN_FORKNUM, &data, parallel_workers, maintenance_work_mem);
        _meta->index_meta_blkno = meta_blkno;
        MarkBufferDirty(_meta_buf);
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
    }

    void build(vector_pair_vector &vectorIds, int parallel_workers, int maintenance_work_mem)
    {
        IvfBuildState buildstate;
        buildstate.metric = _ivf_metric;
        BlockNumber metablkno = IvfflatBuildIndex(NULL, _rel, NULL, &buildstate, MAIN_FORKNUM,
            &vectorIds, parallel_workers, maintenance_work_mem);
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
        IvfInsertState * state = createIvfInsertState(_rel, _ivf_metric, false, metablkno);
        ivfinsert_internal(_rel, state, values, isnull, &tid, false, metablkno, idx);
        pfree(state);
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
            IvfInsertState * state = createIvfInsertState(target, _ivf_metric, false, metablkno);
            for (size_t j = start; j < end; ++j)
            {
                CHECK_FOR_INTERRUPTS();
                VecBuffer buffer = vec_read_buffer(target, data[j].vid, dimensions * sizeof(float));
                errno_t rc = memcpy_s(vector->x, sizeof(float) * dimensions, buffer.get_vecbuf(), sizeof(float) * dimensions);
                buffer.release();
                securec_check(rc, "", "");
                values[0] = PointerGetDatum(vector);
                ivfinsert_internal(target, state, values, isnull, &data[j].tid, false, metablkno, data[j].vid);
            }

            pfree(vector);
            pfree(state);

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
        IndexBulkDeleteResult *stats = ivfbulkdelete_internal(_rel, NULL, callback, callback_state, false, metablkno);
        Assert(stats == NULL);
    }

    size_t search(IndexScanDesc scan, float *dist_out, ItemPointerData *iptr, void *param)
    {
        BlockNumber metablkno = get_meta_blkno();
        VectorIndexNoneSearchParam *search_param = (VectorIndexNoneSearchParam *)param;

        if (!_ivf_so) {
            int lists = IvfGetlists(_rel, metablkno);
            _tupDesc = CreateTemplateTupleDesc(1, false);
            Size size =  sizeof(FormData_pg_attribute);
            errno_t rc = memcpy_s(&(_tupDesc->attrs), size,  &(RelationGetDescr(_rel)->attrs), size);
            securec_check(rc, "\0", "\0");
            int probes = int(lists * u_sess->attr.attr_storage.hybrid_query_ivf_probes_factor / 100.0);
            if (probes <= 0) {
                probes = 1;
            }
            _ivf_so = ivfcreatescanopaque(_rel, _ivf_metric, false, metablkno, probes, _tupDesc);
        }

        size_t count = 0;
        while (count < search_param->top_k && ivfgettuple_internal(scan, _ivf_so, dist_out + count))
        {
            iptr[count] = scan->xs_ctup.t_self;
            ++count;
        }
        return count;
    }

    void recycle_to_fsm()
    {
        BlockNumber metablkno = get_meta_blkno();
        ivf_recycle_to_fsm(_rel, metablkno);
        free_index_meta_pages();
    }

    size_t size() { return 0; }

    void destroy()
    {
        if (_ivf_so) {
            ivfendscan_internal(_ivf_so);
            _ivf_so = NULL;
            pfree(_tupDesc);
            _tupDesc = NULL;
        }
        super::destroy();
    }
    
protected:
    Metric    _ivf_metric;
    void*     _ivf_so;
    TupleDesc _tupDesc;
};

#endif /* DISKANN_VECTOR_BPTREE_H */
