#include "alarm/gt_threads.h"
#include "miscadmin.h"
#include "catalog/pg_partition_fn.h"
#include "knl/knl_instance.h"
#include "knl/knl_session.h"
#include "utils/ps_status.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "postmaster/postmaster.h"
#include "pgxc/pgxc.h"
#include "tcop/tcopprot.h"
#include "utils/postinit.h"
#include "utils/resowner.h"
#include "commands/user.h"
#include "access/transam.h"
#include "utils/snapmgr.h"
#include "access/hybridann/hybridann.h"
#include "access/annvector/annkmeans.h"
#include "storage/buf/buffile.h"
#include "storage/copydir.h"
#include "access/hnsw/hnsw_xlog.h"
#include "access/index_backend/index_backend.h"
#include "storage/pmsignal.h"


constexpr size_t HEAVY_INDEX_TASK_ENTRY_LEVEL = 2;

extern void StreamTxnContextSaveComboCid(void *stc);
extern void StreamTxnContextSaveXact(StreamTxnContext *stc);
extern void StreamTxnContextSaveSnapmgr(void *stc);
extern void StreamTxnContextRestoreComboCid(void *stc);
extern void StreamTxnContextRestoreXact(StreamTxnContext *stc);
extern void StreamTxnContextRestoreSnapmgr(const void *stc);

static void stream_restore_txn_context(StreamTxnContext* stc)
{
    StreamTxnContextRestoreComboCid(stc);
    StreamTxnContextRestoreXact(stc);
    StreamTxnContextRestoreSnapmgr(stc);
}

void setup_worker_transaction(StreamTxnContext &trx_cxt)
{
    stream_restore_txn_context(&trx_cxt);

    /* transaction id */
#ifdef PGXC /* PGXC_DATANODE */
    SetNextTransactionId(trx_cxt.txnId, false);
#endif /* PGXC */
    StreamTxnContextSetTransactionState(&trx_cxt);

    /* snapshot */
    Snapshot snapshot = trx_cxt.snapshot;
    SetGlobalSnapshotData(snapshot->xmin, snapshot->xmax, snapshot->snapshotcsn, snapshot->timeline, false);
    StreamTxnContextSetSnapShot(snapshot);
    StreamTxnContextSetMyPgXactXmin(snapshot->xmin);

    /* TD: bgworker launcher needs active snapshot, so build it, but not sure if this is correct */
    if (u_sess->utils_cxt.ActiveSnapshot == NULL) {
        PushActiveSnapshot(snapshot);
    }

    u_sess->utils_cxt.RecentGlobalXmin = trx_cxt.RecentGlobalXmin;

#ifdef PGXC /* PGXC_DATANODE */
    SaveReceivedCommandId(trx_cxt.currentCommandId);
    SetCurrentGTMDeltaTimestamp();
#endif /* PGXC */
}

void stream_save_txn_context(StreamTxnContext* stc)
{
    StreamTxnContextSaveComboCid(stc);
    StreamTxnContextSaveXact(stc);
    StreamTxnContextSaveSnapmgr(stc);
    /* use new allocated transaction state to avoid access violation */
    TransactionState trx_state = (TransactionState)palloc(sizeof(TransactionStateData));
    errno_t rc = memcpy_s(trx_state, sizeof(TransactionStateData), stc->CurrentTransactionState, sizeof(TransactionStateData));
    securec_check(rc, "\0", "\0");
    stc->CurrentTransactionState = trx_state;
}

void sigusr2_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    if (t_thrd.vector_cxt.mark) {
        *t_thrd.vector_cxt.mark = true;
    }
    errno = save_errno;
}

static void sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    t_thrd.vector_cxt.shutdown_requested = true;
    if (t_thrd.proc) {
        SetLatch(&t_thrd.proc->procLatch);
    }
    errno = save_errno;
}

static void singal_handling_launcher()
{
    gspqsignal(SIGHUP, SIG_IGN);
    gspqsignal(SIGINT, SIG_IGN); /* we don't interrupt statement for internal index */
    gspqsignal(SIGTERM, sigterm_handler);
    gspqsignal(SIGQUIT, quickdie);
    gspqsignal(SIGALRM, handle_sig_alarm);
    gspqsignal(SIGPIPE, SIG_IGN);
    gspqsignal(SIGUSR1, procsignal_sigusr1_handler);
    gspqsignal(SIGUSR2, SIG_IGN);
    gspqsignal(SIGFPE, FloatExceptionHandler);
    gspqsignal(SIGCHLD, SIG_DFL);
    gspqsignal(SIGURG, print_stack);
}

NON_EXEC_STATIC void vector_indexer_launcher(void)
{
    sigjmp_buf local_sigjmp_buf;

    IsUnderPostmaster = true;
    t_thrd.role = VECINDEX_LAUNCHER;
    t_thrd.proc_cxt.MyProcPid = gs_thread_self();
    t_thrd.proc_cxt.MyStartTime = time(NULL);
    t_thrd.proc_cxt.MyProgName = "IndexManagerLauncher";
    u_sess->attr.attr_common.application_name = pstrdup("IndexManagerLauncher");
    init_ps_display("vector indexerlauncher process", "", "", "");

    ereport(LOG, (errmsg("Vector Indexer launcher started")));
    /* we don't consider PostAuthDelay as we only start this thread when there is a task */
    SetProcessingMode(InitProcessing);
    singal_handling_launcher();

    BaseInit();
#ifndef EXEC_BACKEND
    InitProcess();
#endif
    /* not sure whether we need PostInit yet */

    SetProcessingMode(NormalProcessing);
    /* on_proc_exit if needed here */
    /* error handler */
    int curTryCounter;
    int *oldTryCounter = NULL;
    if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
        gstrace_tryblock_exit(true, oldTryCounter);
        t_thrd.log_cxt.error_context_stack = NULL;
        t_thrd.log_cxt.call_stack = NULL;
        HOLD_INTERRUPTS();
        t_thrd.int_cxt.QueryCancelPending = false;
        disable_sig_alarm(true);
        t_thrd.int_cxt.QueryCancelPending = false; /* again in case timeout occurred */
        EmitErrorReport();
        AtEOXact_SysDBCache(false);
        FlushErrorState();
        RESUME_INTERRUPTS();
        if (t_thrd.vector_cxt.shutdown_requested) {
            goto shutdown;
        }
        pg_usleep(1000000L);
    }
    oldTryCounter = gstrace_tryblock_entry(&curTryCounter);
    t_thrd.log_cxt.PG_exception_stack = &local_sigjmp_buf;
    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
    gs_signal_unblock_sigusr2();

    ResetLatch(&t_thrd.proc->procLatch);
    pg_memory_barrier();
    LauncherLatch = &t_thrd.proc->procLatch;
    for (;;) {
        bool has_activity = vector_indexer_has_activity();
        bool has_query = vector_indexer_has_query();
        if (has_query) {
            /* handle query first */
            handle_query_task();
        }
        if (has_activity) {
            IndexerTask *task = pop_index_task(); /* worker is responsible to free the task */
            if (!task) {
                continue;
            }
            /*
            * When the task's index_magnitude_level is greater than or equal to LARGE_INDEX_TASK_ENTRY_LEVEL,
            * it means that the task will probably need longer time to finish, so we need to wait until previous
            * running tasks finish to make sure there are enough workers to run it in parallel.
            */
            bool heavy_task = task->index_magnitude_level >= HEAVY_INDEX_TASK_ENTRY_LEVEL;
            wait_until_has_worker(heavy_task);
            if (t_thrd.vector_cxt.shutdown_requested) {
                break;
            }
            while (!RegisterWorker(task)) {
                pg_usleep(200'000l);
            }
            if (heavy_task) {
                /* sleep for a while to make sure the heavy task has been pulled up with enough resources */
                pg_usleep(500'000l);
            }
        }
        ResetLatch(&t_thrd.proc->procLatch);
        int rc = WaitLatch(&t_thrd.proc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 20'000);
        if (rc & WL_POSTMASTER_DEATH) {
            gs_thread_exit(1);
        }
        if (t_thrd.vector_cxt.shutdown_requested) {
            break;
        }
    }
shutdown:
    LauncherLatch = NULL;
    ereport(LOG, (errmsg("Vector Indexer launcher shutdown")));
}
