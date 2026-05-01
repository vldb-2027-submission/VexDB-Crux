/**
 * Copyright ...
 */

#include <vtl/hashtable>

#include "alarm/gt_threads.h"
#include "miscadmin.h"
#include "catalog/pg_partition_fn.h"
#include "knl/knl_instance.h"
#include "knl/knl_session.h"
#include "utils/ps_status.h"
#include "utils/postinit.h"
#include "utils/resowner.h"
#include "commands/user.h"
#include "access/transam.h"
#include "utils/snapmgr.h"
#include "access/hybridann/hybridann.h"
#include "access/index_backend/index_backend.h"
#include "access/index_backend/taskpool.h"
#include "access/hybridann/bplustree/disk_bplustree.h"
#include "access/diskann/vector_bt_factory.h"

using namespace disk_container;

bool vector_indexer_has_activity()
{
    if (!LWLockConditionalAcquire(VectorIndexerLock, LW_SHARED)) {
        return false;
    }
    if (list_length(g_instance.diskann_cxt.vec_indexer_tasks) <= 0) {
        LWLockRelease(VectorIndexerLock);
        return false;
    }
    const IndexerTask *task = (IndexerTask *)linitial(g_instance.diskann_cxt.vec_indexer_tasks);
    if (!task) {
        LWLockRelease(VectorIndexerLock);
        return false;
    }
    const bool has_activity = task->delay_timer.start_time <= GetCurrentTimestamp();
    LWLockRelease(VectorIndexerLock);
    return has_activity;
}

static long max_task_delay(size_t level)
{
    constexpr long max_delays[max_index_magnitude_size] = {3'000, 10'000, 50'000, 300'000, 1'500'000};
    return max_delays[level];
}

static long init_task_delay(size_t level)
{
    constexpr long init_delays[max_index_magnitude_size] = {100, 1'000, 2'000, 5'000, 10'000};
    return init_delays[level];
}

[[maybe_unused]]
static void set_task_delay(TaskDelayTimer &timer)
{
    timer.cur_delay *= 2;
    if (timer.cur_delay > timer.max_delay) {
        timer.cur_delay = timer.max_delay;
    }
    timer.start_time = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timer.cur_delay);
}

static uint32 get_vector_indexer_worker_count()
{
    return g_instance.diskann_cxt.vec_indexer_worker_count;
}

static uint32 get_max_vector_indexer_worker_count()
{
    return uint32(g_instance.diskann_cxt.max_indexer_worker_threads);
}

int IndexerBaseTaskParam::get_parallel_workers() const
{
    if (parallel_workers <= 1) {
        return 1;
    }
    return parallel_workers / get_vector_indexer_worker_count();
}

static void free_task(IndexerTask *task)
{
    if (task) {
        task->destroy();
        pfree(task);
    }
}

extern void insert_task(IndexerTask *task)
{
    /* firstly, check whether there are alreay duplicated task in the list */
    LWLockAcquire(VectorIndexerLock, LW_SHARED);
    bool exist = false;
    foreach_cell(lc, g_instance.diskann_cxt.vec_indexer_tasks) {
        IndexerTask *t = (IndexerTask *)lfirst(lc);
        if (t->get_id() == task->get_id() && t->index_blkno == task->index_blkno &&
            t->type == task->type) {
            exist = true;
            break;
        }
    }
    LWLockRelease(VectorIndexerLock);

    if (exist) {
         ereport(DEBUG2, (errcode(ERRCODE_LOG),
            errmsg("Skip insert_task for relation(%u) Index(meta:%u, type:%u) "
                   "on magnitude level %lu as it is duplicated",
                    task->get_id(), task->index_blkno, static_cast<uint32>(task->type),
                    task->index_magnitude_level)));
        free_task(task);
        return;
    }

    LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    bool inserted = false;
    ListCell *prev = NULL;
    foreach_cell(lc, g_instance.diskann_cxt.vec_indexer_tasks) {
        IndexerTask *t = (IndexerTask *)lfirst(lc);
        if (t->delay_timer.start_time > task->delay_timer.start_time) {
            if (prev) {
                lappend_cell(g_instance.diskann_cxt.vec_indexer_tasks, prev, task);
            } else {
                g_instance.diskann_cxt.vec_indexer_tasks =
                    lcons(task, g_instance.diskann_cxt.vec_indexer_tasks);
            }
            inserted = true;
            break;
        }
        prev = lc;
    }
    if (!inserted) {
        g_instance.diskann_cxt.vec_indexer_tasks =
            lappend(g_instance.diskann_cxt.vec_indexer_tasks, task);
    }

    LWLockRelease(VectorIndexerLock);
}

static void redispatch_task(IndexerTask *task, const char *cause)
{
    ereport(WARNING, (errcode(ERRCODE_INTERNAL_ERROR),
        errmsg("Put indexer task {%u, %u, %u} back into the list for redispatching due to %s",
               task->get_id(), task->index_blkno, static_cast<uint32>(task->type), cause)));
    /* wait for a while to avoid frequent retries */
    pg_usleep(500'000l);
    /* put the task back into the list */
    auto old_ctx = MemoryContextSwitchTo(IndexerCtx);
    insert_task(task);
    MemoryContextSwitchTo(old_ctx);
}

static bool has_diff_types_of_tasks(Oid rd_id, BlockNumber index_blkno,
    IndexerTaskType curr_task_type, bool need_locking = true)
{
    if (need_locking) {
        LWLockAcquire(VectorIndexerLock, LW_SHARED);
    }
    bool found = false;
    uint32 num_types = static_cast<uint32>(IndexerTaskType::IndexerTaskTypeCount);
    for (uint32 i = 0; i < num_types; ++i) {
        IndexerTaskType task_type = static_cast<IndexerTaskType>(i);
        if (task_type == curr_task_type) {
            /* must be current task, so ignore */
            continue;
        }
        IndexerThreadStatusKey key = {rd_id, index_blkno, task_type};
        IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(IndexerThreadsHTAB, &key, HASH_FIND, NULL);
        if (status && status->status != IndexerTaskStatus::IndexerTaskStatusInvalid &&
            status->status != IndexerTaskStatus::IndexerTaskStatusDone) {
            found = true;
            break;
        }
    }
    if (need_locking) {
        LWLockRelease(VectorIndexerLock);
    }
    return found;
}

static bool is_vector_index_idle(Relation rel, VectorIndex *index, IndexerTaskType curr_task_type)
{
    /*
     * Idle means that the idle flag of the index is true OR no any
     * current index related task is running even if false (i.e. the
     * idle flag might be left as false from previous crash).
     * For now, in such crash recovery scenario the first index task
     * touching the flag just ignores it and continue current task
     * normally regardless of what info should be cleared or resumed
     * from the previously crashed task.
     * TODO in the future: complete crash recovery and consistency.
     */
    if (index->idle()) {
        return true;
    }
    bool has_running_task = has_diff_types_of_tasks(RelationGetRelid(rel), index->ptr(), curr_task_type);
    if (!has_running_task) {
        /* the idle=false should be left from previous crash, so reset it and continue current task */
        index->finish_operation();
    }
    return !has_running_task;
}

static void copy_task_params(IndexerTask *task, void *params)
{
    switch (task->type) {
        case IndexerTaskType::IndexerTaskTypeSplit:
            task->params = NEW IndexerSplitTaskParam(*(IndexerSplitTaskParam *)params);
            break;
        case IndexerTaskType::IndexerTaskTypeBuild:
            task->params = NEW IndexerBuildTaskParam(*(IndexerBuildTaskParam *)params);
            break;
        case IndexerTaskType::IndexerTaskTypeMerge:
            task->params = NEW IndexerMergeTaskParam(*(IndexerMergeTaskParam *)params);
            break;
        case IndexerTaskType::IndexerTaskTypeRecycle:
            task->params = NEW IndexerRecycleTaskParam(*(IndexerRecycleTaskParam *)params);
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("invalid indexer task type %s", IndexerTaskTypeToString(task->type))));
            task->params = NULL;
            break;
    }
}

void add_vector_index_task(Relation rel, BlockNumber blkno, size_t level,
                           TupleDesc tuple_desc, IndexerTaskType type, void *params)
{
    Assert(!IS_PGXC_COORDINATOR);   /* coordinators hold no data */
    auto old_ctx = MemoryContextSwitchTo(IndexerCtx);
    auto *task = (IndexerTask *)palloc(sizeof(IndexerTask));

    task->dbid = t_thrd.proc->databaseId;
    if (RelationIsPartition(rel)) {
        task->rd_id = GetBaseRelOidOfParition(rel);
        task->part_id = RelationGetRelid(rel);
    } else {
        task->rd_id = RelationGetRelid(rel);
        task->part_id = InvalidOid;
    }

    task->type = type;
    task->index_blkno = blkno;
    task->index_magnitude_level = level;
    /* make sure it can be launched immediately even with small clock discrepancy */
    task->delay_timer.start_time = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), -1);
    task->delay_timer.cur_delay = init_task_delay(level);
    task->delay_timer.max_delay = max_task_delay(level);

    IndexerBaseTaskParam *base_params = (IndexerBaseTaskParam *)params;
    base_params->parallel_workers = hybridAnnGetNumParallel(rel);
    base_params->maintenance_work_mem = u_sess->attr.attr_memory.maintenance_work_mem;
    base_params->tuple_desc = CreateTupleDescCopy(tuple_desc);

    Buffer buf = AnnLoadBuffer(rel, HYBRIDANN_METAPAGE_BLKNO);
    HybridAnnMetaPage *meta = (HybridAnnMetaPage *)DiskAnnPageGetMeta(BufferGetPage(buf));
    if (unlikely(meta->magicNumber != HYBRIDANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("hybrid index is not valid")));
    }
    IndexMagnitude index_magnitude(meta->indexMagnitudes, meta->sizeIndexMagnitudes, meta->graphMagnitudeThreshold);
    Assert(level < index_magnitude.size());
    base_params->size_index_magnitudes = index_magnitude.size();
    for (size_t i = 0; i < index_magnitude.size(); ++i) {
        base_params->index_magnitudes[i] = index_magnitude.get(i);
    }
    base_params->graph_magnitude_threshold = meta->graphMagnitudeThreshold;
    UnlockReleaseBuffer(buf);
    index_magnitude.destroy();

    copy_task_params(task, params);

    task->trx_cxt.txnId = GetCurrentTransactionIdIfAny();
    task->trx_cxt.snapshot = CopySnapshotByCurrentMcxt(GetActiveSnapshot());
    stream_save_txn_context(&task->trx_cxt);

    insert_task(task);
    if (LauncherLatch) {
        SetLatch(LauncherLatch);
    }
    MemoryContextSwitchTo(old_ctx);
};

template <class F>
static void iterate_leafchildrens(Relation rel, BlockNumber blkno, bool need_wal, F &&act_on_leaf)
{
    const auto iterate_to_leaf = [rel, blkno](const bool isleftmost, BlockNumber &next) -> BlockNumber {
        BlockNumber nextlevelblkno = blkno;
        while(BlockNumberIsValid(nextlevelblkno)) {
            DiskNodeImpl node(rel, nextlevelblkno);
            node.r_lock();
            if (node.is_leaf()) {
                next = node.next();
                node.r_unlock_destroy();
                return nextlevelblkno;
            }
            nextlevelblkno = isleftmost ? node.get_ptr(p_firstdatakey(node)) : node.get_ptr(node.size());
            node.r_unlock_destroy();
        }

        return InvalidBlockNumber;
    };

    BlockNumber leftnext;
    const BlockNumber leftmostleaf = iterate_to_leaf(true, leftnext);
    BlockNumber rightnext = InvalidBlockNumber;
    [[maybe_unused]]
    const BlockNumber rightmostleaf = iterate_to_leaf(false, rightnext);

    if (BlockNumberIsValid(rightnext)) {
        DiskNodeImpl rightnextnode(rel, rightnext);
        rightnextnode.w_lock();
        rightnextnode.set_temporarily_unlinkable(true);
        rightnextnode.mark_dirty();
        if (need_wal) {
            LogManager logmgr(rel);
            logmgr.log_vecindex_set_temp_unlink(rightnextnode._buf, rightnextnode._page, true);
            logmgr.destroy();
        }
        rightnextnode.w_unlock_destroy();
    }

    BlockNumber cur = leftmostleaf;
    while (BlockNumberIsValid(cur) && cur != rightnext) {
        DiskNodeImpl node(rel, cur);
        node.r_lock();
        OffsetNumber offset = p_firstdatakey(node);
        OffsetNumber max_offset = node.size();
        act_on_leaf(node, offset, max_offset);
        cur = node.next();
        node.r_unlock_destroy();
    }

    if (BlockNumberIsValid(rightnext)) {
        DiskNodeImpl rightnextnode(rel, rightnext);
        rightnextnode.w_lock();
        rightnextnode.set_temporarily_unlinkable(false);
        rightnextnode.mark_dirty();
        if (need_wal) {
            LogManager logmgr(rel);
            logmgr.log_vecindex_set_temp_unlink(rightnextnode._buf, rightnextnode._page, false);
            logmgr.destroy();
        }
        rightnextnode.w_unlock_destroy();
    }
}

static size_t get_node_size(Relation rel, BlockNumber blkno, bool need_wal)
{
    size_t node_size = 0;
    iterate_leafchildrens(rel, blkno, need_wal, [&node_size](DiskNodeImpl &node, OffsetNumber offset, OffsetNumber max_offset) {
        node_size += max_offset;
    });
    return node_size;
}

void collect_vector_info(Relation rel, BlockNumber blkno, vector_pair_vector &map, TupleDesc &tuple_desc, bool need_wal, VectorIndex *new_index)
{
    iterate_leafchildrens(rel, blkno, need_wal, [&map, &tuple_desc, &new_index](DiskNodeImpl &node, OffsetNumber offset, OffsetNumber max_offset) {
        while (offset <= max_offset) {
            BTTupleData *data = node.get_data(offset);
            size_t vec_id = get_vector_id(data->tuple(), tuple_desc);
            map.emplace_back(vec_id, data->t_tid);
            ++offset;
        }
        if (new_index) {
            new_index->set_split_scanned_blkno(node.ptr());
        }
    });
}

static BTTupleData *find_most_tuple(Relation rel, BlockNumber blkno, bool left_most)
{
    DiskNodeImpl node(rel, blkno);
    node.r_lock();
    OffsetNumber offset = left_most ? p_firstdatakey(node) : node.size();
    BTTupleData *data = node.get_data(offset);
    if (node.is_leaf()) {
        node.r_unlock_destroy();
        return data;
    }
    data = find_most_tuple(rel, data->get_ptr(), left_most);
    node.r_unlock_destroy();
    return data;
}

static bool check_equal(TupleDesc &tuple_desc, BTTupleData *last_left, BTTupleData *first_right)
{
    for (int i = 2; i <= tuple_desc->natts; ++i) {
        bool is_null_1, is_null_2;
        Datum datum1 = index_getattr(last_left->tuple(), i, tuple_desc, &is_null_1);
        Datum datum2 = index_getattr(first_right->tuple(), i, tuple_desc, &is_null_2);
        Form_pg_attribute att = TupleDescAttr(tuple_desc, i - 1);
        if (is_null_1 != is_null_2) {
            return false;
        }
        if (!is_null_1 && !DatumImageEq(datum1, datum2, att->attbyval, att->attlen)) {
            return false;
        }
    }
    return true;
}

static size_t check_duplicate(Relation rel, const Vector<BlockNumber> &blknos, size_t mid_nblk, TupleDesc &tuple_desc)
{
    size_t nblkno = blknos.size();
    if (mid_nblk >= nblkno - 1) {
        /* already moved to the end */
        return nblkno;
    }
    BTTupleData *last_left_tuple = find_most_tuple(rel, blknos[mid_nblk], false);
    BTTupleData *first_right_tuple = find_most_tuple(rel, blknos[mid_nblk + 1], true);
    if (check_equal(tuple_desc, last_left_tuple, first_right_tuple)) {
        /* move right so that left side can contain all the same scalar data */
        return check_duplicate(rel, blknos, mid_nblk + 1, tuple_desc);
    } else {
        return mid_nblk;
    }
}

static bool data_size_pass_threshold(Relation rel, const Vector<BlockNumber> &blknos, size_t threshold,
                                     size_t &mid_nblk, TupleDesc &tuple_desc)
{
    size_t total_size = 0;
    size_t nblkno = blknos.size();
    if (nblkno <= 1) {
        /* not split */
        return false;
    }
    size_t node_size[nblkno];
    for (size_t i = 0; i < nblkno; ++i) {
        node_size[i] = get_node_size(rel, blknos[i], true);
        total_size += node_size[i];
    }
    if (total_size < threshold) {
        /* not split */
        return false;
    }
    mid_nblk = nblkno;
    size_t mid_size = total_size / 2;
    size_t cumulative_size = 0;
    for (size_t i = 0; i < nblkno; ++i) {
        cumulative_size += node_size[i];
        if (cumulative_size >= mid_size) {
            mid_nblk = check_duplicate(rel, blknos, i, tuple_desc);
            break;
        }
    }
    if (mid_nblk == nblkno) {
        /* all data are the same from original mid_nblk to the last node, so we have to recheck from the first node */
        mid_nblk = check_duplicate(rel, blknos, 0, tuple_desc);
    }
    /* split if find a new mid_nblk greater than original one, otherwise not split since all data are the same */
    return mid_nblk < nblkno;
}

template <class F>
static void travel_nodes(Relation rel, BlockNumber index_blkno, size_t index_magnitude_level,
                         BlockNumber start_blkno, F &&act_on_node)
{
    BlockNumber next;
    while (BlockNumberIsValid(start_blkno)) {
        DiskNodeImpl node(rel, start_blkno);
        node.r_lock();
        if (node.index_ptr()[index_magnitude_level] != index_blkno) {
            node.r_unlock_destroy();
            break;
        }
        next = node.next();
        node.r_unlock_destroy();
        /* when calling act_on_node, should release node's lock, otherwise may have deadlock */
        act_on_node(start_blkno);
        start_blkno = next;
    }
}

static void relink_index(Relation rel, Relation heap, VectorIndex *new_index, VectorIndex *old_index)
{
    /* find previous index of the old index to update its next to new index */
    DiskNodeImpl start_node(rel, old_index->get_leftmost_node());
    start_node.r_lock();
    BlockNumber prev_node_blkno = start_node.prev();
    if (BlockNumberIsValid(prev_node_blkno)) {
        DiskNodeImpl prev_node(rel, prev_node_blkno);
        prev_node.r_lock();
        BlockNumber prev_index_blkno = prev_node.index_ptr()[old_index->index_magnitude_level()];
        if (BlockNumberIsValid(prev_index_blkno)) {
            VectorIndex *prev_index = VectorIndexFactory::create(rel, heap, prev_index_blkno);
            prev_index->update_next_index_meta_blkno(new_index->ptr(), true);
            delete_vector_index(prev_index);
        }
        prev_node.r_unlock_destroy();
    }
    start_node.r_unlock_destroy();
    /* update new index's next */
    new_index->update_next_index_meta_blkno(old_index->next(), true);
    /* invalidate old index's info */
    old_index->update_next_index_meta_blkno(InvalidBlockNumber, true);
    old_index->set_leftmost_node_blkno(InvalidBlockNumber);
}

template <class F1, class F2, class F3, class F4>
static bool handle_task_internal(Relation rel, Relation heap, VectorIndex *index, BlockNumber start_blkno,
                                 IndexerBaseTaskParam *params, const char *action,
                                 F1 &&cleanup, F2 &&update, F3 &&handle, F4 &&finish)
{
    const BlockNumber index_blkno = index->ptr();
    const size_t index_magnitude_level = index->index_magnitude_level();

    if (!index->start_operation()) {
        ereport(LOG, (errmsg("Can't start operation. Skip %s since %s Index (%d:%d) on magnitude level %lu is busy", action,
                             index->type_name(), index->ptr(), index->index_meta_blkno(), index_magnitude_level)));
        cleanup();
        delete_vector_index(index);
        return false;
    }

    ereport(LOG, (errmsg("Start to %s %s Index (%d:%d) on magnitude level %lu",
                         action, index->type_name(), index->ptr(), index->index_meta_blkno(), index_magnitude_level)));

    /* collect data for preparation */
    vector_pair_vector data;
    travel_nodes(rel, index_blkno, index_magnitude_level, start_blkno, [&](BlockNumber blkno) {
        collect_vector_info(rel, blkno, data, params->tuple_desc, true, NULL);
    });

    /* prepare new meta */
    BlockNumber new_index_blkno = index->prepare_meta(data);
    if (!BlockNumberIsValid(new_index_blkno)) {
        index->finish_operation();
        cleanup();
        ann_helper::optional_destroy(data);
        delete_vector_index(index);
        return false;
    }
    
    VectorIndex *new_index = VectorIndexFactory::create(rel, heap, new_index_blkno);
    new_index->create_vector_index(data, params->get_parallel_workers(),
                                   params->maintenance_work_mem, true);

    XLogBeginInsert();
    new_index->w_lock();
    VectorIndexType type = new_index->type();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(uint8));
    XLogRegisterBuffer(0, new_index->meta_buf(), REGBUF_STANDARD);
    BlockNumber index_meta_blkno = new_index->index_meta_blkno();
    XLogRegisterBufData(0, (char *)&index_meta_blkno, sizeof(BlockNumber));
    uint32 dim = new_index->dim();
    if (type == VectorIndexType::GRAPH) {
        XLogRegisterBufData(0, (char *)&dim, sizeof(uint32));
    }
    XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_MODIFY_META);
    PageSetLSN(BufferGetPage(new_index->meta_buf()), recptr);
    END_CRIT_SECTION();

    new_index->w_unlock();
    new_index->set_leftmost_node_blkno(start_blkno);
    new_index->set_split_scanned_blkno(InvalidBlockNumber);

    /* make the new index effective and assign nodes to new index */
    travel_nodes(rel, index_blkno, index_magnitude_level, start_blkno, [&](BlockNumber blkno) {
        new_index->insert_node_blkno(blkno);
        update(blkno);
        DiskNodeImpl node(rel, blkno);
        node.w_lock();
        node.new_index_ptr()[index_magnitude_level] = new_index_blkno;
        node.mark_dirty();

        LogManager logmgr(rel);
        logmgr.log_vecindex_set_index_ptr(node._buf, node._page, index_magnitude_level, new_index_blkno, true);
        logmgr.destroy();

        node.w_unlock_destroy();
    });

    /* collect data for formal action */
    data.clear();
    travel_nodes(rel, index_blkno, index_magnitude_level, start_blkno, [&](BlockNumber blkno) {
        collect_vector_info(rel, blkno, data, params->tuple_desc, true, new_index);
    });

    new_index->set_split_scanned_blkno(0);

    /* do vector index action */
    handle(new_index, data);

    /* swap to new index */
    travel_nodes(rel, index_blkno, index_magnitude_level, start_blkno, [&](BlockNumber blkno) {
        DiskNodeImpl node(rel, blkno);
        node.w_lock();
        node.index_ptr()[index_magnitude_level] = new_index_blkno;
        node.new_index_ptr()[index_magnitude_level] = InvalidBlockNumber;
        node.mark_dirty();

        LogManager logmgr(rel);
        logmgr.log_vecindex_set_index_ptr(node._buf, node._page, index_magnitude_level, new_index_blkno, false);
        logmgr.log_vecindex_set_index_ptr(node._buf, node._page, index_magnitude_level, InvalidBlockNumber, true);
        logmgr.destroy();

        node.w_unlock_destroy();
    });

    finish();
    index->finish_operation();

    ereport(LOG, (errmsg("Done to %s %s Index (%d:%d -> %d:%d) on magnitude level %lu",
                         action, index->type_name(), index->ptr(), index->index_meta_blkno(),
                         new_index->ptr(), new_index->index_meta_blkno(), index_magnitude_level)));

    cleanup();
    ann_helper::optional_destroy(data);
    delete_vector_index(index);
    delete_vector_index(new_index);

    return true;
}

static bool handle_split_task(Relation rel, Relation heap, BlockNumber blkno, IndexerSplitTaskParam *params)
{
    IndexMagnitude index_magnitude(params->index_magnitudes, params->size_index_magnitudes, params->graph_magnitude_threshold);
    VectorIndex *index = VectorIndexFactory::create(rel, heap, blkno);
    const size_t index_magnitude_level = index->index_magnitude_level();
    const size_t threshold = index_magnitude.split_threshold(index_magnitude_level);

    if (index->deleted()) {
        ereport(LOG, (errmsg("Skip splitting since %s Index (%d:%d) on magnitude level %lu is deleted",
                             index->type_name(), blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return true;
    }

    if (!is_vector_index_idle(rel, index, IndexerTaskType::IndexerTaskTypeSplit)) {
        ereport(LOG, (errmsg("Skip splitting since %s Index (%d:%d) on magnitude level %lu is busy",
                             index->type_name(), blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return false;
    }

    /* get all nodes belong to this index */
    Vector<BlockNumber> blknos;
    travel_nodes(rel, blkno, index_magnitude_level, index->get_leftmost_node(), [&](BlockNumber blkno) {
        blknos.emplace_back(blkno);
    });

    /* check data threshold and find the middle block */
    size_t mid_nblk;
    if (!data_size_pass_threshold(rel, blknos, threshold, mid_nblk, params->tuple_desc)) {
        /* do nothing since threashold is not reached */
        ereport(LOG, (errmsg("Skip splitting since %s Index (%d:%d) on magnitude level %lu doesn't exceed threshold %lu",
                             index->type_name(), blkno, index->index_meta_blkno(), index_magnitude_level, threshold)));
        ann_helper::optional_destroy(blknos);
        delete_vector_index(index);
        return true;
    }

    bool ret = handle_task_internal(rel, heap, index, blknos[mid_nblk + 1], (IndexerBaseTaskParam *)params, "split",
        [&]() { /* cleanup */
            ann_helper::optional_destroy(blknos);
        },
        [&](BlockNumber node_blkno) { /* update */
            index->remove_node_blkno(node_blkno);
        },
        [&](VectorIndex *new_index, vector_pair_vector &data) { /* handle */
            index->split_to(new_index, data, params->get_parallel_workers());
        },
        [&]() { /* finish */
            if (index->type() != VectorIndexType::None) {
                IndexerBuildTaskParam localparams;
                localparams.root_blkno = InvalidBlockNumber;
                add_vector_index_task(rel, index->ptr(), index_magnitude_level, params->tuple_desc,
                                      IndexerTaskType::IndexerTaskTypeBuild, &localparams);
            }
        }
    );

    index_magnitude.destroy();
    return ret;
};

static bool handle_recycle_task(Relation rel, Relation heap, BlockNumber index_blkno, IndexerRecycleTaskParam *params)
{
    VectorIndex *index = VectorIndexFactory::create(rel, heap, index_blkno);
    const size_t index_magnitude_level = index->index_magnitude_level();

    if (index->deleted()) {
        ereport(LOG, (errmsg("Skip recycle since %s Index (%d:%d) on magnitude level %lu is deleted",
                             index->type_name(), index_blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return true;
    }

    if (!is_vector_index_idle(rel, index, IndexerTaskType::IndexerTaskTypeRecycle) || !index->start_operation() ) {
        ereport(LOG, (errmsg("Can't start operation. Skip recycle since %s Index (%d:%d) on magnitude level %lu is busy",
                             index->type_name(), index_blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return false;
    }

    if (index->node_empty()) {
        ereport(LOG, (errmsg("Start to recycle %s Index (%d:%d) on magnitude level %lu directly, because no nodes binds to this index",
                index->type_name(), index->ptr(), index->index_meta_blkno(), index_magnitude_level)));

        index->recycle_to_fsm();

        index->set_deleted(true);

        ereport(LOG, (errmsg("Done to recycle %s Index (%d:%d) on magnitude level %lu",
                         index->type_name(), index->ptr(), index->index_meta_blkno(), index_magnitude_level)));
    } else {
        ereport(LOG, (errmsg("Skip recycle since %s Index (%d:%d) on magnitude level %lu is binding to new nodes",
                             index->type_name(), index_blkno, index->index_meta_blkno(), index_magnitude_level)));
    }

    index->finish_operation();
    delete_vector_index(index);
    return true;
};

static bool handle_merge_task(Relation rel, Relation heap, BlockNumber index_blkno, IndexerMergeTaskParam *params)
{
    IndexMagnitude index_magnitude(params->index_magnitudes, params->size_index_magnitudes, params->graph_magnitude_threshold);
    VectorIndex *index = VectorIndexFactory::create(rel, heap, index_blkno);
    const size_t index_magnitude_level = index->index_magnitude_level();
    const size_t threshold = index_magnitude.get_half(index_magnitude_level);

    if (index->deleted()) {
        ereport(LOG, (errmsg("Skip merge since %s Index (%d:%d) on magnitude level %lu is deleted",
                             index->type_name(), index_blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return true;
    }

    if (!is_vector_index_idle(rel, index, IndexerTaskType::IndexerTaskTypeMerge)) {
        ereport(LOG, (errmsg("Skip merge since %s Index (%d:%d) on magnitude level %lu is busy",
                             index->type_name(), index_blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return false;
    }

    size_t total_size = 0;
    travel_nodes(rel, index_blkno, index_magnitude_level, index->get_leftmost_node(), [&](BlockNumber blkno) {
        total_size += get_node_size(rel, blkno, true);
    });

    if (total_size == 0 || total_size > threshold) {
        /* do nothing since threashold is not reached */
        ereport(LOG, (errmsg("Skip merge since %s Index (%d:%d) on magnitude level %lu, total_size(%lu) doesn't decrease to threshold %lu",
                             index->type_name(), index_blkno, index->index_meta_blkno(), index_magnitude_level, total_size, threshold)));
        delete_vector_index(index);
        return true;
    }

    BlockNumber target_index_blkno = InvalidBlockNumber;
    bool target_is_right = false;

    const auto getadjecentIndex = [rel, index_magnitude_level, index_blkno](BlockNumber start_blkno, bool reverse) -> BlockNumber {
        BlockNumber target = InvalidBlockNumber;
        BlockNumber curblkno = start_blkno;
        while (BlockNumberIsValid(curblkno)) {
            DiskNodeImpl node(rel, curblkno);
            node.r_lock();
            if (node.index_ptr()[index_magnitude_level] != index_blkno) {
                node.r_unlock_destroy();
                target = node.index_ptr()[index_magnitude_level];
                break;
            }
            curblkno = reverse ? node.prev() : node.next();
            node.r_unlock_destroy();
        }
        return target;
    };

    const auto indexMergeable = [rel, heap, &index_magnitude, index_magnitude_level](BlockNumber metaBlkno) -> bool {
        size_t total_size = 0;
        VectorIndex *targetIndex = VectorIndexFactory::create(rel, heap, metaBlkno);
        travel_nodes(rel, metaBlkno, index_magnitude_level, targetIndex->get_leftmost_node(), [&](BlockNumber blkno) {
            total_size += get_node_size(rel, blkno, true);
        });
        delete_vector_index(targetIndex);

        if (total_size < index_magnitude.split_threshold(index_magnitude_level)) {
            return true;
        }
        return false;
    };

    BlockNumber right_index_blkno = getadjecentIndex(index->get_leftmost_node(), false);
    BlockNumber left_index_blkno = getadjecentIndex(index->get_leftmost_node(), true);
    if (BlockNumberIsValid(right_index_blkno) && indexMergeable(right_index_blkno)) {
        target_index_blkno = right_index_blkno;
        target_is_right = true;
    }

    if (!BlockNumberIsValid(target_index_blkno)) {
        if (BlockNumberIsValid(left_index_blkno) && indexMergeable(left_index_blkno)) {
            target_index_blkno = left_index_blkno;
        }
    }

    if (!BlockNumberIsValid(target_index_blkno)) {
        /* do nothing since there are no suitable index to merge */
        ereport(LOG, (errmsg("Skip merge since %s Index (%d:%d) on magnitude level %lu doesn't have mergeable index",
                             index->type_name(), index_blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return true;
    }

    if (!index->start_operation()) {
        ereport(LOG, (errmsg("Can't start operation, Skip merge since %s Index (%d:%d) on magnitude level %lu is busy",
                             index->type_name(), index->ptr(), index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return false;
    }

    VectorIndex *targetIndex = VectorIndexFactory::create(rel, heap, target_index_blkno);
    if (!is_vector_index_idle(rel, targetIndex, IndexerTaskType::IndexerTaskTypeMerge) || !targetIndex->start_operation()) {
        ereport(LOG, (errmsg("Can't start operation. Skip merge since %s Index (%d:%d)'s right index (%d:%d) on magnitude level %lu is busy",
                             index->type_name(), index_blkno, index->index_meta_blkno(), target_index_blkno, targetIndex->index_meta_blkno(), index_magnitude_level)));
        index->finish_operation();
        delete_vector_index(index);
        delete_vector_index(targetIndex);
        return false;
    }

    ereport(LOG, (errmsg("Start to merge %s Index (%d:%d) on magnitude level %lu, total size:%lu",
                         index->type_name(), index->ptr(), index->index_meta_blkno(), index_magnitude_level, total_size)));

    targetIndex->set_split_scanned_blkno(InvalidBlockNumber);

    travel_nodes(rel, index_blkno, index_magnitude_level, index->get_leftmost_node(), [&](BlockNumber blkno) {
        targetIndex->insert_node_blkno(blkno);
        index->remove_node_blkno(blkno);
        DiskNodeImpl node(rel, blkno);
        node.w_lock();
        node.new_index_ptr()[index_magnitude_level] = right_index_blkno;
        node.mark_dirty();

        LogManager logmgr(rel);
        logmgr.log_vecindex_set_index_ptr(node._buf, node._page, index_magnitude_level, right_index_blkno, true);
        logmgr.destroy();

        node.w_unlock_destroy();
    });

    /* collect data for formal action */
    vector_pair_vector data;
    travel_nodes(rel, index_blkno, index_magnitude_level, index->get_leftmost_node(), [&](BlockNumber blkno) {
        collect_vector_info(rel, blkno, data, params->tuple_desc, true, targetIndex);
    });

    targetIndex->set_split_scanned_blkno(0);
    targetIndex->batch_insert(data, params->get_parallel_workers());

    if (target_is_right) {
        targetIndex->set_leftmost_node_blkno(index->get_leftmost_node());
        if (BlockNumberIsValid(left_index_blkno)) {
            VectorIndex *lindex = VectorIndexFactory::create(rel, heap, left_index_blkno);
            lindex->update_next_index_meta_blkno(index->next(), true);
            delete_vector_index(lindex);
        }
    } else {
        targetIndex->update_next_index_meta_blkno(index->next(), true);
    }

    /* swap to new index */
    travel_nodes(rel, index_blkno, index_magnitude_level, index->get_leftmost_node(), [&](BlockNumber blkno) {
        DiskNodeImpl node(rel, blkno);
        node.w_lock();
        node.index_ptr()[index_magnitude_level] = target_index_blkno;
        node.new_index_ptr()[index_magnitude_level] = InvalidBlockNumber;
        node.mark_dirty();

        LogManager logmgr(rel);
        logmgr.log_vecindex_set_index_ptr(node._buf, node._page, index_magnitude_level, target_index_blkno, false);
        logmgr.log_vecindex_set_index_ptr(node._buf, node._page, index_magnitude_level, InvalidBlockNumber, true);
        logmgr.destroy();

        node.w_unlock_destroy();
    });

    /* recycle all the old index's page to fsm */
    index->recycle_to_fsm();

    index->set_deleted(true);

    index->finish_operation();
    targetIndex->finish_operation();

    ereport(LOG, (errmsg("Done to merge %s Index (%d:%d -> %d:%d) on magnitude level %lu",
                         index->type_name(), index->ptr(), index->index_meta_blkno(),
                         targetIndex->ptr(), targetIndex->index_meta_blkno(), index_magnitude_level)));

    /* here rebuild the ivf index because base data changed, need redo kmeans */
    if (index->type() == VectorIndexType::IVF) {
        IndexerBuildTaskParam localparams;
        localparams.root_blkno = InvalidBlockNumber;
        add_vector_index_task(rel, targetIndex->ptr(), index_magnitude_level, params->tuple_desc,
                                IndexerTaskType::IndexerTaskTypeBuild, &localparams);
    }

    ann_helper::optional_destroy(data);
    delete_vector_index(index);
    delete_vector_index(targetIndex);
    index_magnitude.destroy();
    return true;
};

static bool handle_build_task(Relation rel, Relation heap, BlockNumber blkno, IndexerBuildTaskParam *params)
{
    IndexMagnitude index_magnitude(params->index_magnitudes, params->size_index_magnitudes, params->graph_magnitude_threshold);
    VectorIndex *index = VectorIndexFactory::create(rel, heap, blkno);
    const size_t index_magnitude_level = index->index_magnitude_level();

    if (index->deleted()) {
        ereport(LOG, (errmsg("Skip rebuilding since %s Index (%d:%d) on magnitude level %lu is deleted",
                             index->type_name(), blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return true;
    }

    if (!is_vector_index_idle(rel, index, IndexerTaskType::IndexerTaskTypeBuild)) {
        /* the n-1 index (original root) is still being split or merged */
        ereport(LOG, (errmsg("Skip rebuilding since %s Index (%d:%d) on magnitude level %lu is busy",
                             index->type_name(), blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return false;
    }

    const BlockNumber start_blkno = BlockNumberIsValid(params->root_blkno) ? params->root_blkno : index->get_leftmost_node();

    if (!BlockNumberIsValid(start_blkno)) {
        ereport(LOG, (errmsg("Skip rebuilding since %s Index (%d:%d) on magnitude level %lu is already rebuilt",
                             index->type_name(), blkno, index->index_meta_blkno(), index_magnitude_level)));
        delete_vector_index(index);
        return true;
    }

    if (index->type() == VectorIndexType::None) {
        size_t total_size = 0;
        travel_nodes(rel, blkno, index_magnitude_level, index->get_leftmost_node(), [&](BlockNumber blkno) {
            total_size += get_node_size(rel, blkno, true);
        });

        if(total_size < index_magnitude.get_half(index_magnitude_level)) {
            ereport(LOG, (errmsg("Skip to rebuilding %s Index (%d:%d) on magnitude level %lu because total size(%lu) doesn't execeed threshold %lu",
                           index->type_name(), index->ptr(), index->index_meta_blkno(), index_magnitude_level, total_size,
                           index_magnitude.get_half(index_magnitude_level))));
            delete_vector_index(index);
            return true;
        }
    }

    bool ret = handle_task_internal(rel, heap, index, start_blkno, (IndexerBaseTaskParam *)params, "rebuild",
        [&]() { /* cleanup */
            /* do nothing */
        },
        [&](BlockNumber node_blkno) { /* update */
            /* do nothing */
        },
        [&](VectorIndex *new_index, vector_pair_vector &data) { /* handle */
            new_index->batch_insert(data, params->get_parallel_workers());
            if (!BlockNumberIsValid(params->root_blkno)) {
                relink_index(rel, heap, new_index, index);
            }
        },
        [&]() { /* finish */
            if (!BlockNumberIsValid(params->root_blkno)) {
                /* recycle all the old index's page to fsm */
                index->recycle_to_fsm();
                index->set_deleted(true);
            }
        }
    );

    index_magnitude.destroy();
    return ret;
};

static bool handle_vector_index_task(Relation index, Relation heap, BlockNumber blkno, IndexerTaskType type, void *params)
{
    bool res = false;
    switch (type) {
        case IndexerTaskType::IndexerTaskTypeSplit:
            res = handle_split_task(index, heap, blkno, (IndexerSplitTaskParam *)params);
            break;
        case IndexerTaskType::IndexerTaskTypeBuild:
            res = handle_build_task(index, heap, blkno, (IndexerBuildTaskParam *)params);
            break;
        case IndexerTaskType::IndexerTaskTypeMerge:
            res = handle_merge_task(index, heap, blkno, (IndexerMergeTaskParam *)params);
            break;
        case IndexerTaskType::IndexerTaskTypeRecycle:
            res = handle_recycle_task(index, heap, blkno, (IndexerRecycleTaskParam *)params);
            break;
        case IndexerTaskType::QuantizerUpdate:
            res = handle_quantizer_update_task(index, heap, (QuantizerUpdateParam *)params);
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("invalid indexer task type %s", IndexerTaskTypeToString(type))));
            break;
    }
    return res;
}

bool vector_indexer_task_hang(Relation rel, BlockNumber blkno, IndexerTaskType type)
{
    IndexerThreadStatusKey key = {RelationGetRelid(rel), blkno, type};
    LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    bool *mark = NULL;
    ThreadId pid = 0;
    IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(
        IndexerThreadsHTAB, &key, HASH_FIND, NULL);
    if (status && status->status == IndexerTaskStatus::IndexerTaskStatusRunning) {
        mark = status->mark;
        pid = status->pid;
    }
    LWLockRelease(VectorIndexerLock);
    if (!mark) {
        return false;
    }
    *mark = false;
    constexpr long sleep_time = 2'000;
    if (pid != 0) {
        while (gs_signal_send(pid, SIGUSR2) != 0) {
            pg_usleep(sleep_time);
        }
    } else {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("unexpected vector index worker pid:%lu", pid)));
    }
    constexpr uint32 max_retry = 1000;
    uint32 retry_count = 0;
    while (retry_count < max_retry) {
        pg_usleep(sleep_time);
        if (*mark) {
            return false;
        }
        ++retry_count;
    }
    return true;
}

bool all_indexer_thread_exited() { return get_vector_indexer_worker_count() == 0; }

void signal_all_vec_indexer_thread(int sig)
{
    if (hash_get_num_entries(IndexerThreadsHTAB) <= 0) {
        return;
    }
    HASH_SEQ_STATUS status;
    hash_seq_init(&status, IndexerThreadsHTAB);
    IndexerThreadStatus *entry;
    if (t_thrd.proc) {
        LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    }
    while ((entry = (IndexerThreadStatus *)hash_seq_search(&status)) != NULL) {
        if (entry->pid != 0 &&
            (entry->status == IndexerTaskStatus::IndexerTaskStatusRunning ||
             entry->status == IndexerTaskStatus::IndexerTaskStatusPending)) {
            gs_signal_send(entry->pid, sig);
        }
    }
    if (t_thrd.proc) {
        LWLockRelease(VectorIndexerLock);
    }
}

void vector_indexer_kill_task(Relation rel, BlockNumber blkno, IndexerTaskType type)
{
    IndexerThreadStatusKey key = {RelationGetRelid(rel), blkno, type};
    LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(
        IndexerThreadsHTAB, &key, HASH_FIND, NULL);
    if (status && status->status == IndexerTaskStatus::IndexerTaskStatusRunning) {
        if (status->pid != 0) {
            gs_signal_send(status->pid, SIGTERM);
        }
        pfree_ext(status->mark);
    }
    status->status = IndexerTaskStatus::IndexerTaskStatusFailed;
    LWLockRelease(VectorIndexerLock);
}

void vector_indexer_kill_tasks(Relation rel)
{
    if (IS_PGXC_COORDINATOR || !IndexerThreadsHTAB) {
        return;
    }

    UnorderedSet<Oid> index_oids;
    if (RelationIsPartitioned(rel)) {
        List *l = indexGetPartitionList(rel, AccessShareLock);
        foreach_cell(lc, l) {
            Partition part = (Partition)lfirst(lc);
            index_oids.insert(PartitionGetPartid(part));
        }
        releasePartitionList(rel, &l, AccessShareLock);
    } else {
        index_oids.insert(RelationGetRelid(rel));
    }

    UnorderedSet<IndexerThreadStatus *> entries;
    bool sigset = false;
    do {
        sigset = false;
        HASH_SEQ_STATUS status;
        hash_seq_init(&status, IndexerThreadsHTAB);
        IndexerThreadStatus *entry;
        LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
        while ((entry = (IndexerThreadStatus *)hash_seq_search(&status)) != NULL) {
            if (index_oids.contains(entry->key.rd_id)) {
                if (entry->status == IndexerTaskStatus::IndexerTaskStatusRunning && entry->pid != 0) {
                    gs_signal_send(entry->pid, SIGTERM);
                    sigset = true;
                }
                if (!entries.contains(entry)) {
                    entries.insert(entry);
                }
            }
        }
        LWLockRelease(VectorIndexerLock);
        if (sigset) {
            pg_usleep(1000);
        }
    } while(sigset);

    LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    for (const auto& entry : entries) {
        pfree_ext(entry->mark);
        hash_search(IndexerThreadsHTAB, &entry->key, HASH_REMOVE, NULL);
    }
    LWLockRelease(VectorIndexerLock);

    ann_helper::optional_destroy(entries);
    ann_helper::optional_destroy(index_oids);
}

IndexerTaskStatus vector_indexer_task_status(Relation rel, BlockNumber blkno, IndexerTaskType type)
{
    IndexerThreadStatusKey key = {RelationGetRelid(rel), blkno, type};
    LWLockAcquire(VectorIndexerLock, LW_SHARED);
    IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(
        IndexerThreadsHTAB, &key, HASH_FIND, NULL);
    IndexerTaskStatus res = status ? status->status : IndexerTaskStatus::IndexerTaskStatusInvalid;
    LWLockRelease(VectorIndexerLock);
    return res;
}

IndexerTask *pop_index_task()
{
    IndexerTask *res = NULL;
    LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    List *task_list = g_instance.diskann_cxt.vec_indexer_tasks;
    if (list_length(task_list) > 0) {
        res = (IndexerTask *)linitial(task_list);
        if (res->delay_timer.start_time > GetCurrentTimestamp()) {
            res = NULL;
        } else {
            g_instance.diskann_cxt.vec_indexer_tasks = list_delete_first(task_list);
        }
    }
    LWLockRelease(VectorIndexerLock);
    return res;
}

bool RegisterWorker(IndexerTask *task)
{
    Assert(task);
    auto old_ctx = MemoryContextSwitchTo(IndexerCtx);
    VectorIndexerArgs *args = (VectorIndexerArgs *)palloc(sizeof(VectorIndexerArgs));
    args->tinfo_ptr = NULL;
    args->task = task;
    args->mark = (bool *)palloc(sizeof(bool));
    *args->mark = false;
    MemoryContextSwitchTo(old_ctx);

    LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    bool found = has_diff_types_of_tasks(task->get_id(), task->index_blkno, task->type, false);
    if (found) {
        pfree(args);
        LWLockRelease(VectorIndexerLock);
        redispatch_task(task, "another running task with different type");
        return true;
    }
    IndexerThreadStatusKey key = {task->get_id(), task->index_blkno, task->type};
    IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(
        IndexerThreadsHTAB, &key, HASH_ENTER, &found);
    /* reuse the key if the task is already done with the same type */
    if (found && status->status != IndexerTaskStatus::IndexerTaskStatusDone &&
        status->status != IndexerTaskStatus::IndexerTaskStatusInvalid &&
        status->status != IndexerTaskStatus::IndexerTaskStatusFailed) {
        pfree(args);
        LWLockRelease(VectorIndexerLock);
        /* free the duplicate task */
        free_task(task);
        ereport(WARNING, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Indexer task {%u, %u, %u} already exists and isn't done, skip",
                               key.rd_id, key.index_blkno, static_cast<uint32>(key.task_type))));
        return true;
    }
    status->pid = 0;
    status->mark = args->mark;
    status->status = IndexerTaskStatus::IndexerTaskStatusIniting;
    LWLockRelease(VectorIndexerLock);

    int slot = AssignPostmasterChildSlot();
    if (slot == -1) {
        pfree(args);
        return false;
    }
    Backend *bn = AssignFreeBackEnd(slot);
    int child_slot = t_thrd.proc_cxt.MyPMChildSlot;
    if (bn) {
        GenerateCancelKey(false);
        bn->cancel_key = t_thrd.proc_cxt.MyCancelKey;
        bn->child_slot = t_thrd.proc_cxt.MyPMChildSlot = slot;
        bn->role = VECINDEX_WORKER;
    } else {
        ReleasePostmasterChildSlot(slot);
        pfree_ext(args);
        ereport(LOG, (errcode(ERRCODE_LOG), errmsg("Vector Indexer assign backend failed")));
        return false;
    }

    ThreadId pid = status->pid = initialize_util_thread(VECINDEX_WORKER, args);
    t_thrd.proc_cxt.MyPMChildSlot = child_slot;
    if (pid == 0) {
        pfree_ext(args);
        ReleasePostmasterChildSlot(slot);
        bn->pid = 0;
        bn->role = (knl_thread_role)0;
        bn = nullptr;
        return false;
    }
    ++g_instance.diskann_cxt.vec_indexer_worker_count;
    
    bn->pid = pid;
    bn->is_autovacuum = false;
    DLInitElem(&bn->elem, bn);
    DLAddHead(g_instance.backend_list, &bn->elem);

    status->status = IndexerTaskStatus::IndexerTaskStatusPending;
    if (g_threadPoolControler) {
        g_threadPoolControler->BindThreadToAllAvailCpu(pid);
    }
    return true;
}

static void worker_onexit(int, Datum)
{
    --g_instance.diskann_cxt.vec_indexer_worker_count;
}

void wait_until_has_worker(bool all_idle)
{
    constexpr long min_delay = 5;
    constexpr long max_delay = 10'000;
    long cur_delay = min_delay;
    const auto has_worker = [&]() -> bool {
        /* all_idle = true means all workers are idle, otherwise at least one worker is idle */
        return all_idle ? get_vector_indexer_worker_count() > 0 :
                          get_vector_indexer_worker_count() >= get_max_vector_indexer_worker_count();
    };
    while (has_worker()) {
        int rc = WaitLatch(&t_thrd.proc->procLatch, WL_TIMEOUT | WL_POSTMASTER_DEATH, cur_delay);
        if (rc & WL_POSTMASTER_DEATH) {
            gs_thread_exit(1);
        }
        if (t_thrd.vector_cxt.shutdown_requested) {
            return;
        }
        cur_delay += (cur_delay * ((double)random() / (double)MAX_RANDOM_VALUE) + 0.5);
        if (cur_delay > max_delay) {
            cur_delay = min_delay;
        }
    }
}

static void singal_handling_worker()
{
    (void)gspqsignal(SIGINT, SIG_IGN);
    (void)gspqsignal(SIGTERM, die);
    (void)gspqsignal(SIGQUIT, quickdie);
    (void)gspqsignal(SIGALRM, handle_sig_alarm);
    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR1, procsignal_sigusr1_handler);
    (void)gspqsignal(SIGUSR2, sigusr2_handler);
    (void)gspqsignal(SIGFPE, FloatExceptionHandler);
    (void)gspqsignal(SIGCHLD, SIG_DFL);
    (void)gspqsignal(SIGHUP, SIG_IGN);
    (void)gspqsignal(SIGURG, print_stack);
}

void vector_indexer_worker(void)
{
    sigjmp_buf local_sigjmp_buf;
    char user[NAMEDATALEN];
    IndexerThreadStatusKey key =
        {InvalidOid, InvalidBlockNumber, IndexerTaskType::IndexerTaskTypeInvalid};

    IsUnderPostmaster = true;
    t_thrd.role = VECINDEX_WORKER;
    t_thrd.proc_cxt.MyProcPid = gs_thread_self();
    t_thrd.proc_cxt.MyStartTime = time(NULL);
    t_thrd.proc_cxt.MyProgName = "IndexManagerWorker";
    init_ps_display("vector indexerworker process", "", "", "");

    SetProcessingMode(InitProcessing);
    singal_handling_worker();
    BaseInit();
#ifndef EXEC_BACKEND
    InitProcess();
#endif
    on_proc_exit(worker_onexit, 0);

    /* error handler */
    int curTryCounter;
    int *oldTryCounter = NULL;
    if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
        gstrace_tryblock_exit(true, oldTryCounter);
        HOLD_INTERRUPTS();
        EmitErrorReport();
        AtEOXact_SysDBCache(false);
        BgworkerListSyncQuit();
        DESTROY_TASK_RUNNER();
        if (OidIsValid(key.rd_id)) {
            t_thrd.vector_cxt.mark = NULL;
            LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
            IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(
                IndexerThreadsHTAB, &key, HASH_FIND, NULL);
            if (status) {
                pfree_ext(status->mark);
                status->latch = NULL;
                status->status = IndexerTaskStatus::IndexerTaskStatusFailed;
            }
            LWLockRelease(VectorIndexerLock);
        }
        LWLockReleaseAll();
        if (t_thrd.utils_cxt.CurrentResourceOwner) {
            ResourceOwnerRelease(t_thrd.utils_cxt.CurrentResourceOwner, RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
        }
        /* don't abort the transaction in workers that might cause crash */
        ResetTransactionInfo();
        /* we directly exit, let the launcher handle everything */
        ereport(LOG, (errmsg("Vector Indexer worker got error")));
        proc_exit(0);
    }
    oldTryCounter = gstrace_tryblock_entry(&curTryCounter);
    t_thrd.log_cxt.PG_exception_stack = &local_sigjmp_buf;
    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
    gs_signal_unblock_sigusr2();

    /* critical default settings */
    SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);
    SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
    SetConfigOption("default_transaction_isolation", "read committed", PGC_SUSET, PGC_S_OVERRIDE);

    IndexerTask *task = (IndexerTask *)t_thrd.vector_cxt.task;
    if (!task || !OidIsValid(task->dbid)) {
        ereport(WARNING, (errmsg("Vector indexer worker launched without a valid task, shut down")));
        proc_exit(0);
    }
    t_thrd.proc_cxt.PostInit->SetDatabaseAndUser(NULL, task->dbid, NULL);
    /* there is currently no need to set a special initiation */
    t_thrd.proc_cxt.PostInit->InitAutoVacWorker();
    char dbname[NAMEDATALEN];
    t_thrd.proc_cxt.PostInit->GetDatabaseName(dbname);
    SetProcessingMode(NormalProcessing);
    set_ps_display(dbname, false);
    ereport(LOG, (errmsg("Start vector index worker on database \"%s\"", dbname)));
    t_thrd.utils_cxt.CurrentResourceOwner = ResourceOwnerCreate(NULL, "VectorIndexerWorker",
        THREAD_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_STORAGE));
    
    /* seems to be only used for distributed settings */
    t_thrd.mem_cxt.msg_mem_cxt = AllocSetContextCreate(t_thrd.top_mem_cxt,
        "MessageContext", ALLOCSET_DEFAULT_SIZES);
    t_thrd.mem_cxt.mask_password_mem_cxt = AllocSetContextCreate(t_thrd.top_mem_cxt,
        "MaskPasswordCtx", ALLOCSET_DEFAULT_SIZES);
    pfree_ext(u_sess->proc_cxt.MyProcPort->database_name);
    pfree_ext(u_sess->proc_cxt.MyProcPort->user_name);
    u_sess->proc_cxt.MyProcPort->database_name =
        MemoryContextStrdup(SESS_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_STORAGE), dbname);
    u_sess->proc_cxt.MyProcPort->user_name = GetSuperUserName(user);
    exec_init_poolhandles();

    StartTransactionCommand();
    setup_worker_transaction(task->trx_cxt);

    bool is_partition = OidIsValid(task->part_id);
    Relation index;
    Relation parent_index;
    Partition part_index;
    Oid heap_id = IndexGetRelation(task->rd_id, false);
    Relation parent_heap;
    Partition part_heap;
    Relation heap;
    if (is_partition) {
        parent_index = index_open(task->rd_id, NoLock);
        part_index = partitionOpen(parent_index, task->part_id, NoLock);
        index = partitionGetRelation(parent_index, part_index);
        parent_heap = heap_open(task->parent_heapoid, NoLock);
        part_heap = partitionOpen(parent_heap, task->part_heapoid, NoLock);
        heap = partitionGetRelation(parent_heap, part_heap);
    } else {
        index = index_open(task->rd_id, NoLock);
        heap = heap_open(heap_id, NoLock);
    }
    key = {task->get_id(), task->index_blkno, task->type};
    bool status_set = false;
    constexpr uint32 max_wait = 10'000u;
    for (uint32 i = 0; i < max_wait; ++i) {
        LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
        IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(
            IndexerThreadsHTAB, &key, HASH_FIND, NULL);
        if (!status) {
            LWLockRelease(VectorIndexerLock);
            break;
        }
        if (status->status > IndexerTaskStatus::IndexerTaskStatusIniting) {
            Assert(status->status == IndexerTaskStatus::IndexerTaskStatusPending);
            Assert(status->pid == t_thrd.proc_cxt.MyProcPid);
            status->latch = &t_thrd.proc->procLatch;
            status->status = IndexerTaskStatus::IndexerTaskStatusRunning;
            LWLockRelease(VectorIndexerLock);
            status_set = true;
            break;
        }
        LWLockRelease(VectorIndexerLock);
        constexpr long sleep_time = 1'000l;
        pg_usleep(sleep_time);
    }
    if (!status_set) {
        free_task(task);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Vector indexer did not get valid task")));
    }

    bool successful = handle_vector_index_task(index, heap, task->index_blkno, task->type, task->params);

    if (is_partition) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        releaseDummyRelation(&index);
        partitionClose(parent_index, part_index, NoLock);
        index_close(parent_index, NoLock);
        releaseDummyRelation(&heap);
        partitionClose(parent_heap, part_heap, NoLock);
        heap_close(parent_heap, NoLock);
#pragma GCC diagnostic pop /* -Wmaybe-uninitialized */
    } else {
        index_close(index, NoLock);
        heap_close(heap, NoLock);
    }

    if (successful) {
        free_task(task);
    } else {
        redispatch_task(task, "idle task conflict");
    }

    t_thrd.vector_cxt.mark = NULL;
    LWLockAcquire(VectorIndexerLock, LW_EXCLUSIVE);
    IndexerThreadStatus *status = (IndexerThreadStatus *)hash_search(
        IndexerThreadsHTAB, &key, HASH_FIND, NULL);
    Assert(status);
    pfree_ext(status->mark);
    status->latch = NULL;
    status->status = IndexerTaskStatus::IndexerTaskStatusDone;
    hash_search(IndexerThreadsHTAB, &key, HASH_REMOVE, NULL);
    LWLockRelease(VectorIndexerLock);

    EndParallelWorkerTransaction();
    ResetTransactionInfo();

    ereport(LOG, (errmsg("Vector Indexer worker finished")));
    proc_exit(0);
}

size_t VectorIndex::search(IndexScanDesc scan, float *dist_out, ItemPointerData *iptr, void *param)
{
    LockBuffer(_node_buf, BUFFER_LOCK_SHARE);
    uint32 nblkno = _nodes->nnode;
    BlockNumber *blknos = (BlockNumber *)palloc(sizeof(BlockNumber) * nblkno);
    for (uint32 i = 0; i < nblkno; ++i) {
        blknos[i] = _nodes->nodes[i];
    }
    LockBuffer(_node_buf, BUFFER_LOCK_UNLOCK);
    VectorIndexNoneSearchParam *search_param = (VectorIndexNoneSearchParam *)param;
    Vector<DistData> vec(search_param->top_k);
    float *query = search_param->query;
    const uint32 dim = search_param->dim;
    Assert(is_aligned(query));
    TupleDesc tdesc = search_param->tdesc;
    auto func = ann_helper::get_aligned_distance_func(Metric::L2, dim);
    const auto calc_leaf = [this, &vec, &func, query, tdesc, dim](DiskNodeImpl &node) {
        Assert(node.is_leaf());
        size_t n = node.size();
        bool is_null;
        for (size_t i = p_firstdatakey(node); i <= n; ++i) {
            const auto *data = node.get_data(i);
            Datum vid = index_getattr(data->tuple(), 1, tdesc, &is_null);
            Assert(!is_null);
            size_t vi = size_t(DatumGetInt64(vid));
            VecBuffer buf = vec_read_buffer(_rel, vi, dim * sizeof(float));
            float dist = func(query, (float *)buf.get_vecbuf(), dim);
            buf.release();
            vec.emplace_back(dist, data->t_tid);
        }
    };
    std::function<void (BlockNumber)> calc_node;
    calc_node = [this, &calc_leaf, &calc_node](BlockNumber blkno) {
        DiskNodeImpl node(_rel, blkno);
        node.r_lock();
        if (node.is_leaf()) {
            calc_leaf(node);
            node.r_unlock_destroy();
            return;
        }
        size_t n = node.size();
        Vector<BlockNumber> children(n);
        for (size_t i = p_firstdatakey(node); i <= n; ++i) {
            children.push_back(node.get_ptr(i));
        }
        node.r_unlock_destroy();
        for (BlockNumber i : children) {
            calc_node(i);
        }
        ann_helper::optional_destroy(children);
    };
    for (uint32 i = 0; i < nblkno; ++i) {
        calc_node(blknos[i]);
    }
    /* partial sort top 10 dist from vec */
    uint32 top_k = Min(search_param->top_k, vec.size());
    std::partial_sort(vec.begin(), vec.begin() + top_k, vec.end());
    for (uint32 i = 0; i < top_k; ++i) {
        dist_out[i] = vec[i].dist;
        iptr[i] = vec[i].iptr;
    }
    vec.destroy();
    pfree(blknos);
    return top_k;
}
