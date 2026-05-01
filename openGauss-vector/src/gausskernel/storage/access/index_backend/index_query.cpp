#include "access/index_backend/index_backend.h"
#include "access/index_backend/taskpool.h"
#include "commands/user.h"
#include "utils/postinit.h"

bool vector_indexer_has_query()
{
    if (!LWLockConditionalAcquire(VectorQueryRunnerListLock, LW_SHARED)) {
        return false;
    }
    if (list_length(QueryRunnersList) <= 0) {
        LWLockRelease(VectorQueryRunnerListLock);
        return false;
    }
    LWLockRelease(VectorQueryRunnerListLock);
    return true;
}

static uint32 get_vec_query_idle_count()
{
    size_t idle = 0;
    for (size_t i = 0; i < ThreadInfoValidIdx; ++i) {
        if (QueryThreadsArr[i].thread_status.load(std::memory_order_relaxed) == QueryThreadStatus::Idle) {
            ++idle;
        }
    }
    return idle;
}

static uint32 get_max_vector_indexer_query_count()
{
    return uint32(g_instance.diskann_cxt.max_indexer_query_threads);
}

void launch_query_workers(void *task_runner)
{
    auto old_ctx = MemoryContextSwitchTo(IndexerCtx);
    RunnerParam *runner_param = (RunnerParam *)palloc(sizeof(RunnerParam));
    runner_param->trx_cxt.txnId = GetCurrentTransactionIdIfAny();
    runner_param->trx_cxt.snapshot = CopySnapshotByCurrentMcxt(GetActiveSnapshot());
    runner_param->trx_cxt.RecentGlobalXmin = u_sess->utils_cxt.RecentGlobalXmin;
    runner_param->task_runner = (TaskRunner *)task_runner;
    runner_param->dbid = t_thrd.proc->databaseId;
    runner_param->_cooperatestate = CooperateState::Init;
    ((TaskRunner*)task_runner)->set_backrunnerpara(runner_param);
    runner_param->_refcount = 2;
    TASKPOOLLOG("DEBUG: RunnerParam %p TASK %p run in database %u", runner_param, task_runner, t_thrd.proc->databaseId);
    stream_save_txn_context(&runner_param->trx_cxt);

    LWLockAcquire(VectorQueryRunnerListLock, LW_EXCLUSIVE);
    QueryRunnersList = lappend(QueryRunnersList, runner_param);
    TASKPOOLLOG("DEBUG: RunnerParam %p TASK %p LAUNCH", runner_param, task_runner);
    LWLockRelease(VectorQueryRunnerListLock);
    if (LauncherLatch) {
        SetLatch(LauncherLatch);
    }
    MemoryContextSwitchTo(old_ctx);
}

void withdraw_query_workers(void *task_runner_in)
{
    TaskRunner *task_runner = (TaskRunner *)task_runner_in;
    RunnerParam *runner_para = (RunnerParam *)task_runner->get_backrunnerpara();
    CooperateState state = runner_para->get_CooperateState();
    while (state == CooperateState::Init) {
        runner_para->compare_exchange_cooperatestate(state, CooperateState::FrontendDone);
        state = runner_para->get_CooperateState();
    }
    
    while (runner_para->backendAssigning()) {
        CHECK_FOR_INTERRUPTS();
        pg_usleep(10);
    }

    runner_para->destroy();
    runner_para = NULL;
}

static size_t get_free_slot()
{
    size_t tidx = 0;
    for (; tidx < ThreadInfoValidIdx; ++tidx) {
        if (QueryThreadsArr[tidx].thread_status.load(std::memory_order_relaxed) == QueryThreadStatus::Zombie) {
            break;
        }
    }
    return tidx;
}

static void start_query_threads(float create_factor)
{
    uint32 max_count = get_max_vector_indexer_query_count();
    uint32 idle_count = get_vec_query_idle_count();
    uint32 create_num = 0;
    if (idle_count < max_count) {
        create_num = max_count - idle_count;
    }
    if (create_num > 0) {
        create_num *= create_factor;
        for (uint32 i = 0; i < create_num; ++i) {
            VectorIndexerArgs *args = (VectorIndexerArgs *)palloc(sizeof(VectorIndexerArgs));
            size_t free_slot_idx = get_free_slot();
            if (free_slot_idx == max_count) {
                pfree(args);
                return;
            }
            args->tinfo_ptr = QueryThreadsArr + free_slot_idx;
            args->task = NULL;
            args->mark = NULL;

            int slot = AssignPostmasterChildSlot();
            if (slot == -1) {
                pfree(args);
                continue;
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
                continue;
            }
            QueryThreadInfo *tinfo_ptr_copy = args->tinfo_ptr;
            ThreadId pid = initialize_util_thread(VECINDEX_WORKER, args);
            t_thrd.proc_cxt.MyPMChildSlot = child_slot;
            if (pid == 0) {
                pfree_ext(args);
                ReleasePostmasterChildSlot(slot);
                bn->pid = 0;
                bn->role = (knl_thread_role)0;
                bn = nullptr;
                return;
            }
            bn->pid = pid;
            bn->is_autovacuum = false;
            DLInitElem(&bn->elem, bn);
            DLAddHead(g_instance.backend_list, &bn->elem);
            if (g_threadPoolControler) {
                g_threadPoolControler->BindThreadToAllAvailCpu(pid);
            }
            new (tinfo_ptr_copy) QueryThreadInfo();
            TASKPOOLLOG("DEBUG: thread create in slot:%lu, pid:%lu", free_slot_idx, pid);
            if (ThreadInfoValidIdx == free_slot_idx) {
                ++ThreadInfoValidIdx;
            }
        }
    }
}

size_t handle_query_task()
{
    auto old_ctx = MemoryContextSwitchTo(IndexerCtx);
    size_t handle_task_num = 0;
    size_t dispatch_thread_num = 0;
    static float idle_factor = 0.25;
    static float create_factor = 1.0;
    static float avg_dispatch_thread_num = 0.0;
    static size_t dispatch_thread_sum = 0;
    static size_t count = 0;
    start_query_threads(create_factor);
    RunnerParam *runner_param = NULL;

    const auto fetch_effective_runner_para = [&]() {
        RunnerParam *local_rp = NULL;
        LWLockAcquire(VectorQueryRunnerListLock, LW_EXCLUSIVE);
        while(list_length(QueryRunnersList) > 0) {
            local_rp = (RunnerParam *)linitial(QueryRunnersList);
            if (!local_rp->frontendDone()) {
                CooperateState state = local_rp->get_CooperateState();
                if (state == CooperateState::Init) {
                    if (local_rp->compare_exchange_cooperatestate(state, 
                        CooperateState::BackendAssigning)) {
                        break;
                    }
                }
            }
            TASKPOOLLOG("DEBUG: RunnerParam %p TASK %p delete from list", local_rp, local_rp->task_runner); 
            QueryRunnersList = list_delete_first(QueryRunnersList);
            local_rp->destroy();
            local_rp = NULL;
        }
        LWLockRelease(VectorQueryRunnerListLock);
        return local_rp;
    };

    int query_num =0;
    int idle_num = get_vec_query_idle_count();
    if (idle_num == 0) {
        goto end;
    }

    if(LWLockConditionalAcquire(VectorQueryRunnerListLock, LW_SHARED)) {
        query_num = list_length(QueryRunnersList);
        LWLockRelease(VectorQueryRunnerListLock);
    }
    if (query_num == 0) {
        goto end;
    }
    if (query_num < idle_num) {
        int dispatch_num = std::min(idle_num / query_num, std::max(int(idle_num * idle_factor), 1));
        int dnum = 0;
        runner_param = fetch_effective_runner_para();
        if (!runner_param) {
            goto end;
        }

        for (size_t i = 0; i < ThreadInfoValidIdx; ++i) {
            QueryThreadInfo &qti = QueryThreadsArr[i];
            QueryThreadStatus status = qti.thread_status.load(std::memory_order_relaxed);
            if (status != QueryThreadStatus::Idle) {
                continue;
            }
            if (qti.dbid != InvalidOid && qti.dbid != runner_param->dbid) {
                continue;
            }
            
            bool assigned = true;
            do {
                if (status != QueryThreadStatus::Idle) {
                    assigned = false;
                    break;
                }
            } while (!qti.thread_status.compare_exchange_weak(status, QueryThreadStatus::Assigned,
                                            std::memory_order_release,
                                            std::memory_order_relaxed));
            

            if (!assigned) {
                continue;
            }
            TASKPOOLLOG("DEBUG: qti: %p, slot: %u staus set to assigned", &QueryThreadsArr[i], i);

            qti.runner_param = runner_param->copy();
            qti.dbid = runner_param->dbid;
            qti.runner_param->task_runner->record_consumer();
            pg_memory_barrier();
            TASKPOOLLOG("DEBUG: RunnerParam %p TASK %p in database %u dispatched to slot %lu, qti: %p, latch: %p", runner_param, qti.runner_param->task_runner, qti.dbid, i, &QueryThreadsArr[i], qti.proc_latch);
            SetLatch(qti.proc_latch);
            if (dnum == 0) {
                ++handle_task_num;
                --query_num;
                TASKPOOLLOG("DEBUG: RunnerParam %p TASK %p delete from list", runner_param, runner_param->task_runner); 
                LWLockAcquire(VectorQueryRunnerListLock, LW_EXCLUSIVE);
                QueryRunnersList = list_delete_first(QueryRunnersList);
                LWLockRelease(VectorQueryRunnerListLock);
            }
            ++dispatch_thread_num;
            ++dnum;
            if (dnum == dispatch_num) {
                runner_param->set_cooperatestate(CooperateState::BackendAssiged);
                runner_param->destroy();
                runner_param = NULL;

                runner_param = fetch_effective_runner_para();
                if (!runner_param) {
                    goto end;
                }
                dnum = 0;
            }
        }
        if (runner_param) {
            if (dnum > 0) {
                runner_param->set_cooperatestate(CooperateState::BackendAssiged);
                runner_param->destroy();
                runner_param = NULL;
            } else {
                Assert(runner_param->get_CooperateState() == CooperateState::BackendAssigning);
                runner_param->set_cooperatestate(CooperateState::Init);
            }
        }

    } else {
        for (size_t i = 0; idle_num > 0 && i < ThreadInfoValidIdx; ++i) {
            QueryThreadInfo &qti = QueryThreadsArr[i];
            QueryThreadStatus status = qti.thread_status.load(std::memory_order_relaxed);
            if (status == QueryThreadStatus::Idle) {
                bool assigned = true;
                if (status == QueryThreadStatus::Idle) {
                    do {
                        if (status != QueryThreadStatus::Idle) {
                            assigned = false;
                            break;
                        }
                    } while (!qti.thread_status.compare_exchange_weak(status, QueryThreadStatus::Assigned,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed));
                }

                if (!assigned) {
                    continue;
                }


                TASKPOOLLOG("DEBUG: qti: %p, slot: %u staus set to assigned",  &QueryThreadsArr[i], i);

                runner_param = fetch_effective_runner_para();

                if (!runner_param) {
                    qti.thread_status.store(QueryThreadStatus::Idle, std::memory_order_relaxed);
                    goto end;
                }

                if (qti.dbid != InvalidOid && qti.dbid != runner_param->dbid) {
                    Assert(runner_param->get_CooperateState() == CooperateState::BackendAssigning);
                    runner_param->set_cooperatestate(CooperateState::Init);
                    qti.thread_status.store(QueryThreadStatus::Idle, std::memory_order_relaxed);
                    continue;
                }
                qti.runner_param = runner_param->copy();
                qti.dbid = runner_param->dbid;
                qti.runner_param->task_runner->record_consumer();
                runner_param->set_cooperatestate(CooperateState::BackendAssiged);  
                pg_memory_barrier();
                TASKPOOLLOG("DEBUG: RunnerParam %p TASK %p in database %u dispatched to slot %lu, qti: %p", runner_param, qti.runner_param->task_runner, qti.dbid, i, &QueryThreadsArr[i]);
                SetLatch(qti.proc_latch);
                LWLockAcquire(VectorQueryRunnerListLock, LW_EXCLUSIVE);
                TASKPOOLLOG("DEBUG: RunnerParam %p TASK %p delete from list", runner_param, runner_param->task_runner);
                QueryRunnersList = list_delete_first(QueryRunnersList);
                LWLockRelease(VectorQueryRunnerListLock);
                runner_param->destroy();
                runner_param = NULL;
                ++handle_task_num;
                ++dispatch_thread_num;
                --idle_num;
            }
        }
    }
end:
    if (unlikely(dispatch_thread_sum >= 4096 || count >= 1024)) {
        count = 0;
        dispatch_thread_sum = 0;
        avg_dispatch_thread_num = 0.0;
    }
    ++count;
    dispatch_thread_sum += dispatch_thread_num;
    avg_dispatch_thread_num = (float)dispatch_thread_sum / (float)count;
    TASKPOOLLOG("avg:%f, this route:%f", avg_dispatch_thread_num, (float)dispatch_thread_num);
    idle_factor = (float)dispatch_thread_num >= avg_dispatch_thread_num ?
        std::min(1.0, idle_factor + 0.125) : std::max(0.25, idle_factor - 0.125);
    MemoryContextSwitchTo(old_ctx);
    return handle_task_num;
}

static void set_db(Oid dbid)
{
    char user[NAMEDATALEN];
    t_thrd.proc_cxt.PostInit->SetDatabaseAndUser(NULL, dbid, NULL);
    t_thrd.proc_cxt.PostInit->InitAutoVacWorker();
    char dbname[NAMEDATALEN];
    t_thrd.proc_cxt.PostInit->GetDatabaseName(dbname);
    SetProcessingMode(NormalProcessing);
    set_ps_display(dbname, false);
    t_thrd.utils_cxt.CurrentResourceOwner = ResourceOwnerCreate(NULL, "VectorQueryWorker",
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
    u_sess->proc_cxt.MyProcPort->user_name = pstrdup(GetSuperUserName(user));
    exec_init_poolhandles();
}

static void singal_handling_query_worker()
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

void vector_query_worker(void)
{
    sigjmp_buf local_sigjmp_buf;
    IsUnderPostmaster = true;
    t_thrd.role = VECINDEX_WORKER;
    t_thrd.proc_cxt.MyProcPid = gs_thread_self();
    t_thrd.proc_cxt.MyStartTime = time(NULL);
    t_thrd.proc_cxt.MyProgName = "IndexQueryWorker";
    init_ps_display("vector indexer query worker process", "", "", "");
    bool *consumer_resigned = (bool*)palloc(sizeof(bool));
    *consumer_resigned = true;

    SetProcessingMode(InitProcessing);
    singal_handling_query_worker();
    BaseInit();
#ifndef EXEC_BACKEND
    InitProcess();
#endif
    /* error handler */
    int curTryCounter;
    int *oldTryCounter = NULL;
    if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
        gstrace_tryblock_exit(true, oldTryCounter);
        HOLD_INTERRUPTS();
        EmitErrorReport();
        AtEOXact_SysDBCache(false);
        LWLockReleaseAll();
        if (!*consumer_resigned && ThreadInfoPtr->runner_param != NULL) {
            ThreadInfoPtr->runner_param->task_runner->sub_consumer();
        }
        ThreadInfoPtr->thread_status.store(QueryThreadStatus::Zombie, std::memory_order_relaxed);
        if (t_thrd.utils_cxt.CurrentResourceOwner) {
            ResourceOwnerRelease(t_thrd.utils_cxt.CurrentResourceOwner, RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
        }
        /* don't abort the transaction in workers that might cause crash */
        ResetTransactionInfo();
        /* we directly exit, let the launcher handle everything */
        ereport(LOG, (errmsg("Vector Query worker got error")));
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

    Latch latch;
    InitLatch(&latch);
    ThreadInfoPtr->proc_latch = &latch;
    ResetLatch(&latch);
    ThreadInfoPtr->thread_status.store(QueryThreadStatus::Idle, std::memory_order_relaxed);
    t_thrd.proc->databaseId = InvalidOid;
    [[maybe_unused]]
    uint32 slot = ThreadInfoPtr - QueryThreadsArr;
    for (;;) {
        int rc = WaitLatch(&latch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 20'000);
        if (rc & WL_POSTMASTER_DEATH) {
            gs_thread_exit(1);
        }
        if (rc & WL_TIMEOUT) {
            QueryThreadStatus status = ThreadInfoPtr->thread_status.load(std::memory_order_relaxed);
            bool set_zombie = true;
            if (status == QueryThreadStatus::Idle) {
                do {
                    if (status != QueryThreadStatus::Idle) {
                        TASKPOOLLOG("DEBUG: thread:%lu get a new task, exit fail", t_thrd.proc->pid);
                        set_zombie = false;
                        break;
                    }
                } while (!ThreadInfoPtr->thread_status.compare_exchange_weak(status, QueryThreadStatus::Zombie,
                                                std::memory_order_release,
                                                std::memory_order_relaxed));

                if (set_zombie) {
                    ThreadInfoPtr->proc_latch = NULL;
                    ThreadInfoPtr->runner_param = NULL;
                    TASKPOOLLOG("DEBUG: thread:%lu exit", t_thrd.proc->pid);
                    proc_exit(0);   
                } else {
                    continue;
                }    
            }
        }
        if (ThreadInfoPtr->thread_status.load(std::memory_order_relaxed) != QueryThreadStatus::Assigned) {
            continue;
        }

        if (t_thrd.proc->databaseId == InvalidOid) {
            TASKPOOLLOG("DEBUG: thread:%lu run in database %u", t_thrd.proc->pid, ThreadInfoPtr->dbid);
            set_db(ThreadInfoPtr->dbid);
        }
        TASKPOOLLOG("DEBUG: thread:%lu slot: %u run TASK %p START, latch: %p", t_thrd.proc->pid, slot, ThreadInfoPtr->runner_param->task_runner,&t_thrd.proc->procLatch);
        *consumer_resigned = false;
        ThreadInfoPtr->thread_status.store(QueryThreadStatus::Running, std::memory_order_relaxed);
        StartTransactionCommand();
        setup_worker_transaction(ThreadInfoPtr->runner_param->trx_cxt);
        TASK_RUNNER_PTR = ThreadInfoPtr->runner_param->task_runner;
        ThreadInfoPtr->runner_param->task_runner->consume(false);
        *consumer_resigned = true;
        TASK_RUNNER_PTR = NULL;
        PopActiveSnapshot();
        CommitTransactionCommand();
        ResetTransactionInfo();
        ResetLatch(&latch);

        TASKPOOLLOG("DEBUG: thread:%lu run TASK %p FINISH", t_thrd.proc->pid, ThreadInfoPtr->runner_param->task_runner);
        pfree_ext(ThreadInfoPtr->runner_param->trx_cxt.CurrentTransactionState);
        pfree(ThreadInfoPtr->runner_param);
        ThreadInfoPtr->runner_param = NULL;
        ThreadInfoPtr->thread_status.store(QueryThreadStatus::Idle, std::memory_order_relaxed);
        TASKPOOLLOG("DEBUG: thread:%lu, ThreadInfoPtr: %p, slot: %u status set to idle", t_thrd.proc->pid,  ThreadInfoPtr, slot);
    }
}
