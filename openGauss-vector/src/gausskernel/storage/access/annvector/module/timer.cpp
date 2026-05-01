/**
 * Copyright ...
 */

#include "access/annvector/module/timer.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "funcapi.h"
#include "storage/ipc.h"

using namespace ann_helper;

#define MAX_INDEX_PROGRESS_NUM 128
Timer* create_index_progress[MAX_INDEX_PROGRESS_NUM] = { nullptr };
ThreadId create_index_threadid[MAX_INDEX_PROGRESS_NUM] = { 0 };
slock_t	index_progress_mutex;

Timer::Timer(size_t nloop, size_t step_size, char* index_name, char* part_index_name) 
    : _start(std::chrono::high_resolution_clock::now()), _nloop(nloop), _step_size(step_size)
{
    if (index_name != nullptr) {
        _index_progress_slot = -1;
        _index_name = (char *)palloc(strlen(index_name) + 1);
        _part_index_name = (char *)palloc(strlen(part_index_name) + 1);
        sprintf(_index_name, "%s", index_name);
        sprintf(_part_index_name, "%s", part_index_name);

        SpinLockAcquire(&index_progress_mutex);
        for (int i = 0 ; i < MAX_INDEX_PROGRESS_NUM; ++i) {
            if (create_index_progress[i] == nullptr) {
                create_index_progress[i] = this;
                _index_progress_slot = i;
                break;
            }
        }

        if (_index_progress_slot == -1) {
            elog(WARNING, "Index Progress slot is used up, %s's process will not report!", _index_name);
        } else {
            create_index_threadid[_index_progress_slot] = t_thrd.proc->sessMemorySessionid;
            on_proc_exit(clear_session_timer_arg, UInt64GetDatum(t_thrd.proc->sessMemorySessionid), true);
        }

        SpinLockRelease(&index_progress_mutex);
    }
}

void Timer::set_stage(char* stage) 
{
    if (stage != NULL) {
        if (_stage) {
            pfree(_stage);
        }
        _stage = (char *)palloc(strlen(stage) + 1);
        sprintf(_stage, "%s", stage);
    }
}

void Timer::destroy() 
{
    if (_index_progress_slot != -1) {
        SpinLockAcquire(&index_progress_mutex);
        create_index_progress[_index_progress_slot] = nullptr;
        create_index_threadid[_index_progress_slot] = 0;
        SpinLockRelease(&index_progress_mutex);
        cancel_proc_exit(clear_session_timer_arg, UInt64GetDatum(t_thrd.proc->sessMemorySessionid));
    }

    if (_index_name != nullptr) {
        pfree_ext(_index_name);
    }

    if (_part_index_name != nullptr) {
        pfree_ext(_part_index_name);
    }

    if (_stage != nullptr) {
        pfree_ext(_stage);
    }

    LeakChecker::destroy();
}


/* RTO statistics */
typedef struct IndexCreationProgress {
    char index_name[NAMEDATALEN + 1];
    char partition_index_name[NAMEDATALEN + 1];
    char stage[NAMEDATALEN];
    int64 total;
    int64 current;
    int1   progress;
    char  duration[Timer::time_buf_len];
} IndexCreationProgress;


IndexCreationProgress* generate_index_creation_progress(uint32 *num)
{
    IndexCreationProgress *result = (IndexCreationProgress *)palloc(MAX_INDEX_PROGRESS_NUM * sizeof(IndexCreationProgress));
    uint32 count = 0;
    SpinLockAcquire(&index_progress_mutex);
    for (int i = 0 ; i < MAX_INDEX_PROGRESS_NUM; ++i) {
        if (create_index_progress[i] != nullptr) {
            Timer * timer = create_index_progress[i];
            sprintf(result[count].index_name, "%s", timer->_index_name);
            sprintf(result[count].partition_index_name, "%s", timer->_part_index_name);
            sprintf(result[count].stage, "%s", timer->_stage);
            result[count].total = (int64)timer->_nloop;
            result[count].current = timer->_nloop_count_unknown ? -1 : (int64)timer->_nloop_count;
            result[count].progress = timer->_nloop != 0 ? int1(100.0 * timer->_nloop_count / timer->_nloop) : 0;
            Timer::ns_to_str(timer->elapsed_nanos(), result[count].duration);
            count++;
        }
    }
    SpinLockRelease(&index_progress_mutex);
    
    *num = count;
    return result;
}

#define INDEX_CREATION_PROGRESS_VIEW_COL 7
Datum index_creation_progress(PG_FUNCTION_ARGS)
{
    FuncCallContext* funcctx = NULL;
    MemoryContext oldcontext = NULL;

    /* stuff done only on the first call of the function */
    if (SRF_IS_FIRSTCALL()) {
        TupleDesc tupdesc = NULL;

        /* create a function context for cross-call persistence */
        funcctx = SRF_FIRSTCALL_INIT();

        /*
         * switch to memory context appropriate for multiple function
         * calls
         */
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* need a tuple descriptor representing 6 columns */
        tupdesc = CreateTemplateTupleDesc(INDEX_CREATION_PROGRESS_VIEW_COL, false);

        TupleDescInitEntry(tupdesc, (AttrNumber)1, "index_name", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)2, "partition", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)3, "stage", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)4, "total", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)5, "currennt", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)6, "progress", INT1OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)7, "duration", TEXTOID, -1, 0);

        /* complete descriptor of the tupledesc */
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        
        funcctx->user_fctx = (void*)generate_index_creation_progress(&(funcctx->max_calls));

        (void)MemoryContextSwitchTo(oldcontext);
    }

    /* stuff done on every call of the function */
    funcctx = SRF_PERCALL_SETUP();

    IndexCreationProgress* entry = (IndexCreationProgress*)funcctx->user_fctx;
    if (funcctx->call_cntr < funcctx->max_calls) { /* do when there is more left to send */
        Datum values[INDEX_CREATION_PROGRESS_VIEW_COL];
        bool nulls[INDEX_CREATION_PROGRESS_VIEW_COL] = {false};
        HeapTuple tuple = NULL;

        entry += funcctx->call_cntr;
        values[0] = CStringGetTextDatum(entry->index_name);
        values[1] = CStringGetTextDatum(entry->partition_index_name);
        nulls[1]  = entry->partition_index_name[0] == '\0' ? true : false;
        values[2] = CStringGetTextDatum(entry->stage);
        values[3] = Int64GetDatum(entry->total);
        nulls[3]  = entry->total == 0 ? true : false;
        values[4] = Int64GetDatum(entry->current);
        nulls[4]  = entry->current == -1 ? true : false;
        values[5] = Int8GetDatum(entry->progress);
        nulls[5]  = entry->total == 0 ? true : false;
        values[6] = CStringGetTextDatum(entry->duration);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    /* do when there is no more left */
    SRF_RETURN_DONE(funcctx);
}

void clear_session_timer(ThreadId threadid) 
{
    SpinLockAcquire(&index_progress_mutex);
    for (int i = 0; i < MAX_INDEX_PROGRESS_NUM; ++i) {
        if (create_index_threadid[i] == threadid) { 
            create_index_progress[i] = nullptr;
            create_index_threadid[i] = 0;
        }
    }
    SpinLockRelease(&index_progress_mutex);
}

void clear_session_timer_arg(int status, Datum threadId) 
{
    ThreadId id = DatumGetUInt64(threadId);
    clear_session_timer(id);
}

void init_index_progress_mutex()
{
    SpinLockInit(&index_progress_mutex);
}
