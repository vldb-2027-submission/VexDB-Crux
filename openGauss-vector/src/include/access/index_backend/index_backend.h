/**
 * Copyright ...
 * Vector index management backend, currently only used for filtered hybrid vector indexes on bptree.
 */

#ifndef INDEX_BACKEND_H
#define INDEX_BACKEND_H

#include <vtl/vector>
#include <vtl/hashtable>

#include "postgres.h"
#include "gs_thread.h"
#include "utils/ps_status.h"
#include "knl/knl_thread.h"
#include "utils/relcache.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/pmsignal.h"
#include "storage/latch.h"
#include "storage/buf/block.h"
#include "access/xact.h"
#include "postmaster/postmaster.h"
#include "access/diskann/diskann.h"
#include "access/diskann/vector_bt.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/quantizer.h"

#define IndexerCtx g_instance.diskann_cxt.vec_indexer_ctx
#define IndexerThreadsHTAB g_instance.diskann_cxt.vec_indexer_threads
#define QueryRunnersList g_instance.diskann_cxt.vec_query_runners

enum class QueryThreadStatus : uint8 {
    Init = 0,
    Idle,
    Assigned,
    Running,
    Zombie
};

enum class CooperateState : uint8 {
    Init = 0,
    FrontendDone, /*means all task are already done by front thread*/
    BackendAssigning,
    BackendAssiged,
};

struct RunnerParam {
    std::atomic<CooperateState> _cooperatestate;
    std::atomic<uint8> _refcount;
    Oid dbid;
    TaskRunner *task_runner;
    StreamTxnContext trx_cxt;

    RunnerParam *copy() {
        RunnerParam *rp = (RunnerParam *)palloc(sizeof(RunnerParam));
        rp->_refcount = 1;
        rp->task_runner = task_runner;
        rp->dbid = dbid;
        rp->trx_cxt.snapshot = CopySnapshotByCurrentMcxt(trx_cxt.snapshot);
        rp->trx_cxt.RecentGlobalXmin = trx_cxt.RecentGlobalXmin;

        TransactionState trx_state = (TransactionState)palloc(sizeof(TransactionStateData));
        errno_t rc = memcpy_s(trx_state, sizeof(TransactionStateData),
            trx_cxt.CurrentTransactionState, sizeof(TransactionStateData));
        securec_check(rc, "\0", "\0");
        rp->trx_cxt.CurrentTransactionState = trx_state;
        return rp;
    }
    void destroy() {
        if (_refcount.fetch_sub(1, std::memory_order_relaxed) == 1) {
            if (GTM_LITE_MODE && (trx_cxt.snapshot->prepared_array != NULL)) {
                pfree_ext(trx_cxt.snapshot->prepared_array);
            }
            pfree_ext(trx_cxt.snapshot);
            pfree_ext(trx_cxt.CurrentTransactionState);
            pfree(this);
        }
    }

    CooperateState get_CooperateState() { return _cooperatestate.load(std::memory_order_relaxed); }
    bool backendAssigning() { return _cooperatestate.load(std::memory_order_relaxed) == CooperateState::BackendAssigning; }
    bool frontendDone() { return _cooperatestate.load(std::memory_order_relaxed) == CooperateState::FrontendDone; }
    void set_cooperatestate(const CooperateState &state) { 
        _cooperatestate.store(state, std::memory_order_relaxed);
        TASKPOOLLOG("DEBUG: RunnerParam %p cooperatestate status set to %u", this, _cooperatestate.load()); 
    }
    bool compare_exchange_cooperatestate(const CooperateState &expected, const CooperateState &desired) {
        bool success = true;
        CooperateState expect_state = expected;
        do {
            if (expect_state != expected) {
                success = false;
                break;
            }
        } while (!_cooperatestate.compare_exchange_weak(expect_state, desired,
                                        std::memory_order_release,
                                        std::memory_order_relaxed));
        TASKPOOLLOG("DEBUG: RunnerParam %p cooperatestate status set to %u", this, _cooperatestate.load());                           

        return success; 
    }
};

struct QueryThreadInfo {
    std::atomic<QueryThreadStatus> thread_status{QueryThreadStatus::Init};
    Oid dbid{InvalidOid};
    RunnerParam *runner_param{NULL};
    Latch *proc_latch{NULL};
};

#define QueryThreadsArr ((QueryThreadInfo *)(g_instance.diskann_cxt.vec_query_threads_arr))
#define ThreadInfoValidIdx g_instance.diskann_cxt.tinfo_valid_idx
#define ThreadInfoPtr ((QueryThreadInfo *)(t_thrd.vector_cxt.tinfo_ptr))
#define LauncherLatch g_instance.diskann_cxt.launcher_latch

enum class IndexerTaskType : uint32 {
    IndexerTaskTypeInvalid = 0,
    IndexerTaskTypeSplit,
    IndexerTaskTypeMerge,
    IndexerTaskTypeBuild,   /* for root index construction */
    IndexerTaskTypeRecycle,
    IndexerTaskTypeCount,
    QuantizerUpdate  /* for pq and rabitq */
};

inline const char *IndexerTaskTypeToString(IndexerTaskType type)
{
    switch (type) {
        case IndexerTaskType::IndexerTaskTypeSplit:
            return "IndexerTaskTypeSplit";
        case IndexerTaskType::IndexerTaskTypeMerge:
            return "IndexerTaskTypeMerge";
        case IndexerTaskType::IndexerTaskTypeBuild:
            return "IndexerTaskTypeBuild";
        case IndexerTaskType::IndexerTaskTypeRecycle:
            return "IndexerTaskTypeRecycle";
        case IndexerTaskType::IndexerTaskTypeCount:
            return "IndexerTaskTypeCount";
        case IndexerTaskType::QuantizerUpdate:
            return "QuantizerUpdate";
        default:
            return "IndexerTaskTypeInvalid";
    }
}

enum class IndexerTaskStatus {
    IndexerTaskStatusInvalid = 0,
    IndexerTaskStatusIniting,
    IndexerTaskStatusPending,
    IndexerTaskStatusRunning,
    IndexerTaskStatusDone,
    IndexerTaskStatusFailed,
};

struct IndexerBaseTaskParam : public BaseObject {
    int parallel_workers;
    int maintenance_work_mem;
    TupleDesc tuple_desc;
    size_t size_index_magnitudes;
    size_t index_magnitudes[max_index_magnitude_size];
    int64 graph_magnitude_threshold;
    Oid parent_id;

    int get_parallel_workers() const;
};

struct IndexerSplitTaskParam : public IndexerBaseTaskParam {
};

struct IndexerBuildTaskParam : public IndexerBaseTaskParam {
    BlockNumber root_blkno;
};

struct IndexerMergeTaskParam : public IndexerBaseTaskParam {
};

struct IndexerRecycleTaskParam : public IndexerBaseTaskParam {
};

struct QuantizerUpdateParam : public IndexerBaseTaskParam {
    bool enable; /* means has quantizer before, equal to not enabling */
    QuantizerType qt_type;
    BlockNumber metablkno;
    float freq_10min;
    bool force;
};

struct TaskDelayTimer {
    TimestampTz start_time;
    long cur_delay;
    long max_delay;
};

struct IndexerTask {
    Oid dbid;       /* database id of vector index,
                     * required for worker to launch under that database */
    Oid rd_id;      /* relation id that the vector index belongs to */
    Oid part_id;    /* partition id that the vector index belongs to */
    Oid parent_heapoid;
    Oid part_heapoid;
    IndexerTaskType type;
    BlockNumber index_blkno;
    size_t index_magnitude_level;
    TaskDelayTimer delay_timer;
    StreamTxnContext trx_cxt;
    void *params;
    Oid get_id() const { return OidIsValid(part_id) ? part_id : rd_id; }
    void destroy()
    {
        if (params) {
            if (((IndexerBaseTaskParam *)params)->tuple_desc) {
                pfree(((IndexerBaseTaskParam *)params)->tuple_desc);
            }
            pfree(params);
            params = NULL;
        }
        pfree_ext(trx_cxt.snapshot);
        pfree_ext(trx_cxt.CurrentTransactionState);
    }
};

struct IndexerThreadStatusKey {
    Oid rd_id;
    BlockNumber index_blkno;
    IndexerTaskType task_type;
};

struct IndexerThreadStatus {
    IndexerThreadStatusKey key;
    ThreadId pid;
    bool *mark;     /* to check whether threads hang */
    IndexerTaskStatus status;
    Latch *latch;   /* to wake up the thread if needed, valid only if status is running */

    void init_mark()
    {
        mark = (bool *)MemoryContextAlloc(g_instance.diskann_cxt.vec_indexer_ctx, sizeof(bool));
        *mark = false;
    }
    void destroy() { pfree_ext(mark); }
};

struct VectorIndexerArgs {
    QueryThreadInfo *tinfo_ptr;
    IndexerTask *task;
    bool *mark;
};

class VectorIndex;

/* launcher */
extern void vector_indexer_launcher(void);
extern void setup_worker_transaction(StreamTxnContext &trx_cxt);
extern void stream_save_txn_context(StreamTxnContext* stc);
extern void sigusr2_handler(SIGNAL_ARGS);

/* query */
extern void vector_query_worker(void);
extern bool vector_indexer_has_query();
extern size_t handle_query_task();
extern void launch_query_workers(void *);
extern void withdraw_query_workers(void *);

/* hybrid manager */
/**
 * @brief Add a vector index task to the indexer task queue.
 * @param rel The relation that the vector index belongs to.
 * @param blkno The block number of the vector index.
 * @param level The magnitude level of the vector index.
 * @param type The type of the indexer task.
 * @param params The parameters of the indexer task, it can point to stack mem as it should be copied.
 */
extern void vector_indexer_worker(void);
extern bool vector_indexer_has_activity();
extern void wait_until_has_worker(bool all_idle);
extern void insert_task(IndexerTask *task);
extern IndexerTask *pop_index_task();
extern bool RegisterWorker(IndexerTask *task);
extern void add_vector_index_task(Relation rel, BlockNumber blkno, size_t level, TupleDesc tuple_desc, IndexerTaskType type, void *params);
extern void signal_all_vec_indexer_thread(int sig);
extern bool all_indexer_thread_exited();
extern bool vector_indexer_task_hang(Relation rel, BlockNumber blkno, IndexerTaskType type);
extern void vector_indexer_kill_task(Relation rel, BlockNumber blkno, IndexerTaskType type);
extern void vector_indexer_kill_tasks(Relation rel);
extern IndexerTaskStatus vector_indexer_task_status(Relation rel, BlockNumber blkno, IndexerTaskType type);
extern void collect_vector_info(Relation rel, BlockNumber blkno, vector_pair_vector &map,
                                TupleDesc &tuple_desc, bool need_wal, VectorIndex *new_index = NULL);

extern bool handle_quantizer_update_task(Relation index, Relation heap, QuantizerUpdateParam *params);
#endif /* INDEX_BACKEND_H */
