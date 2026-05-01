/**
 * Copyright ...
 */

#include "access/index_backend/taskpool.h"
#include "utils/memutils.h"
#include "postmaster/bgworker.h"
#include "utils/snapmgr.h"
#include "access/index_backend/index_backend.h"

using namespace ann_helper;

TaskPool::TaskPool(MemoryContext ctx) : _ctx(ctx)
    { memset(_tasks, 0, sizeof(_tasks)); }

bool TaskPool::run_task()
{
    size_t cur = _task_start.load(std::memory_order_relaxed);
    do {
        if (cur >= _task_end) {
            return false;
        }
    } while (!_task_start.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed));
    cur %= task_stack_size;

    Task *task = _tasks[cur];
    while (!task) {
        SPIN_DELAY(); /* busy wait for producer to write */
        pg_read_barrier();
        task = _tasks[cur];
    }
    _tasks[cur] = NULL;
    task->run();
    task->set_done();
    record_task_done();
    return true;
}

struct Hibernator {
    static inline constexpr size_t max_sleep_time = 1'000'000ul; /* 1s */
    size_t cur_sleep_time{TaskPool::sleep_time};
    inline void reset() { cur_sleep_time = TaskPool::sleep_time; }
    inline void hibernate()
    {
        pg_usleep(cur_sleep_time);
        cur_sleep_time = std::min(size_t(cur_sleep_time * 1.5), max_sleep_time);
    }
    inline void destroy() {}
};

void TaskPool::pure_consume()
{
    Hibernator hibernator;
    for (;;) {
        CHECK_FOR_INTERRUPTS();
        if (run_task()) {
            hibernator.reset();
            continue;
        }
        if (!running()) {
            break;
        }
        hibernator.hibernate();
    }
    hibernator.destroy();
}

void TaskPool::consume(bool need_enroll)
{
    if (need_enroll) {
        consumer_enroll();
    }
    pure_consume();
    if (need_enroll) {
        consumer_resign();
    } else {
        sub_consumer();
    }
}

void TaskPool::wait_for_all()
{
    while (_producer_count.load(std::memory_order_relaxed) > 0) {
        CHECK_FOR_INTERRUPTS();
        pg_usleep(sleep_time);
    }
    while (_consumer_count.load(std::memory_order_relaxed) > 0) {
        CHECK_FOR_INTERRUPTS();
        pg_usleep(sleep_time);
    }
}

/* we don't care about offset overflow
 * since it's used inside one utility statement
 * which is not likely to surpass SIZE_MAX */
bool TaskPool::allocate_task_idx(size_t &idx, const bool force)
{
    if (!force && busy()) {
        return false;
    }
    idx = _task_end.load(std::memory_order_relaxed);
    do {
        if (idx >= _task_start + task_stack_size - max_num_consumers) { /* avoid race on the end */
            return false;
        }
    } while (!_task_end.compare_exchange_weak(idx, idx + 1));
    idx %= task_stack_size;
    return true;
}

static inline void consumer_cleanup(const BgWorkerContext *) {}
static inline void consumer_main(const BgWorkerContext *bwc)
{
    TASK_RUNNER->consume();
}

TaskRunner *TaskRunner::create_instance()
{
    auto ctx = AllocSetContextCreate(
        g_instance.instance_context,
        TASK_RUNNER_CXT_NAME,
        ALLOCSET_DEFAULT_SIZES,
        SHARED_CONTEXT);
    return New (ctx) TaskRunner(ctx);
}

/* thread unsafe */
bool TaskRunner::launch_consumer(size_t num_consumers, bool withLockGroupLeader)
{
    _use_bgworker = true;
    if (_num_consumers >= num_consumers) {
        return true;
    }
    size_t num_launched = num_consumers - _num_consumers;
    int res = LaunchBackgroundWorkers(int(num_launched), NULL, consumer_main, consumer_cleanup, withLockGroupLeader);
    if (res < 0) {
        return false;
    }
    _num_consumers += res;
    return size_t(res) < num_launched;
}

/* request several threads from backend */
void TaskRunner::load_consumer()
{
    if (!USE_PARALLEL_QUERY) {
        return;
    }
    if (!_vec_worker_launched) {
        _use_bgworker = false;
        _vec_worker_launched = true;
        launch_query_workers(TASK_RUNNER);
    }
}


void TaskRunner::resign_producer()
{
    if (_pool.running()) {
        _pool.producer_resign();
    }
}

void TaskRunner::finish()
{
    if (_vec_worker_launched) {
        withdraw_query_workers(TASK_RUNNER);
    }
    if (_pool.running()) {
        _pool.producer_resign();
        _pool.wait_for_all();
    } else {
        _pool.wait_for_all();
    }
    _num_consumers = 0;
    if (_use_bgworker) {
        BgworkerListSyncQuit();
    }
}

void TaskRunner::wait(Task *task)
{
    while(!task->is_done()) {
        CHECK_FOR_INTERRUPTS();
        if (!_pool.run_task()) {
            BgworkerListCheckStatus();
            pg_usleep(ann_helper::TaskPool::sleep_time);
        } else {
            pg_read_barrier();
        }
    }
}
void TaskRunner::destroy()
{
    TASKPOOLLOG("DEBUG: TASK %p DESTROY", TASK_RUNNER);
    auto ctx = get_context();
    _pool.destroy();
    MemoryContextDelete(ctx);
}
