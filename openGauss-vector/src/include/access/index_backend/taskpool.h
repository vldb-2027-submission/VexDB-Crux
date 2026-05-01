/**
 * Copyright ...
 * Taskpool managing all backend and frontend threads and tasks,
 *  all engaged threads can be both producers and consumers.
 * Main routines:
 *  INIT_TASK_RUNNER() - initialize task runner, you need a runner to run the task pool.
 *  LAUNCH_CONSUMER() - (optional) launch consumer threads in backend.
 *  START_TASK_POOL() - start task pool.
 *  RUN_TASK() - add whatever tasks to the pool, the task can launch other task recursively,
 *               but note since all threads only take tasks without order, it's very likely that
 *               the first task recursion will finish at last and with very long stack call, which
 *               may cause overflow and slow.
 *  WAIT_TASK() - wait for all launched tasks to finish.
 *  END_TASK_POOL() - destroy task pool,
 *                    you can start another pool at other place whenever the runner is alive.
 *  DESTROY_TASK_RUNNER() - destroy task runner.
 */

#ifndef TASKPOOL_H
#define TASKPOOL_H

#include <atomic>
#include <new>
#include <functional>
#include <vtl/vector>

#include "utils/palloc.h"
#include "knl/knl_thread.h"

#define TASK_RUNNER_PTR (t_thrd.bgworker_cxt.taskrunner)
#define TASK_RUNNER ((TaskRunner *)TASK_RUNNER_PTR)
#define TASK_RUNNER_CXT (TASK_RUNNER->get_context())
#define TASK_RUNNER_CXT_NAME "task runner"
#define TASK_RUNNER_ENROLLED t_thrd.bgworker_cxt.consumer_enrolled
#define TASK_DEBUG false

#define OUTPUT_TASKPOOL_LOG false
#if OUTPUT_TASKPOOL_LOG
#define TASKPOOLLOG(fmt, ...) ereport(LOG, (errcode(ERRCODE_LOG), errmsg(fmt, ##__VA_ARGS__)))
#else
#define TASKPOOLLOG(fmt, ...)
#endif /* OUTPUT_TASKPOOL_LOG */

#define MAX_QUERY_THREADS_NUM 256 /* must bigger than the maximum value of `max_vector_indexer_query_threads` */
#define USE_PARALLEL_QUERY (g_instance.diskann_cxt.max_indexer_query_threads > 0)

namespace ann_helper {
/* task interface */
class Task : public BaseObject {
public:
    Task() {}
    virtual void run() = 0;
    virtual void destroy() = 0;
    virtual ~Task() {}
    void set_done() { done = true; }
    bool is_done() const { return done; }  
private:
    bool done{false};
};

template <class F>
class LambdaTask : public Task {
public:
    LambdaTask(F &&f) : Task(), _f(std::forward<F>(f)) {}
    void run() override { _f(); }
    void destroy() override { _f.~F(); }
    ~LambdaTask() {}
private:
    F _f;
};

class TaskPool {
public:
    constexpr static size_t sleep_time = 10ul;
    constexpr static size_t max_num_consumers = 64ul;
    constexpr static size_t task_stack_size = max_num_consumers * max_num_consumers * 2ul;
    MemoryContext _ctx;

    explicit TaskPool(MemoryContext ctx);
    ~TaskPool() {}
    void producer_enroll() { _producer_count.fetch_add(1, std::memory_order_relaxed); }
    void producer_resign() { 
        size_t res = _producer_count.fetch_sub(1, std::memory_order_relaxed); 
        if (res == 1) { 
            _stop.store(true, std::memory_order_relaxed); 
        }
    }
    void consumer_enroll() { Assert(!TASK_RUNNER_ENROLLED); _consumer_count.fetch_add(1, std::memory_order_relaxed); TASK_RUNNER_ENROLLED = true; }
    void consumer_resign() { Assert(TASK_RUNNER_ENROLLED); _consumer_count.fetch_sub(1, std::memory_order_relaxed); TASK_RUNNER_ENROLLED = false; }

    void add_consumer() { _consumer_count.fetch_add(1, std::memory_order_relaxed); }
    void sub_consumer() { _consumer_count.fetch_sub(1, std::memory_order_relaxed); }
    /* mem-order unsafe */
    bool running() { return !_stop.load(std::memory_order_relaxed); }

    template <class F>
    Task *add_task(F &&f, const bool force)
    {
        size_t idx;
        if (!allocate_task_idx(idx, force)) {
            return NULL;
        }
        auto task = New(_ctx) LambdaTask<F>(std::forward<F>(f));
        while (_tasks[idx]) {
            SPIN_DELAY(); /* busy wait for consumer to consume */
            pg_read_barrier();
        }
        _tasks[idx] = task;
        record_task_added();
        return task;
    }
    bool run_task();
    void pure_consume();
    void consume(bool need_enroll = true);
    bool busy() const
    {
        size_t start = _task_start.load(std::memory_order_relaxed);
        size_t end = _task_end.load(std::memory_order_relaxed);
        return end > start && end - start >= _consumer_count * 2;
    }
    void wait_for_all();
    void destroy()
    {
        Assert(_consumer_count == 0);
        Assert(_producer_count == 0);
        Assert(_stop);
#if TASK_DEBUG
        Assert(_task_added == _task_done);
#endif /* TASK_DEBUG */
    }
private:
    std::atomic<size_t> _consumer_count{0};
    std::atomic<size_t> _producer_count{0};
    std::atomic<size_t> _task_start{0};
    std::atomic<size_t> _task_end{0};
    Task *_tasks[task_stack_size];
    std::atomic<bool> _stop{false};

#if TASK_DEBUG
    std::atomic<size_t> _task_added{0};
    std::atomic<size_t> _task_done{0};
    void record_task_added() { _task_added.fetch_add(1u, std::memory_order_relaxed); }
    void record_task_done() { _task_done.fetch_add(1u, std::memory_order_relaxed); }
#else
    void record_task_added() {}
    void record_task_done() {}
#endif /* TASK_DEBUG */
    bool allocate_task_idx(size_t &idx, const bool force);
};
} /* namespace ann_helper */

class TaskRunner : public BaseObject {
public:
    static TaskRunner *create_instance();
    explicit TaskRunner(MemoryContext ctx) : _pool(ctx) { _pool.producer_enroll(); }
    ~TaskRunner() {}
    bool launch_consumer(size_t num_consumers, bool withLockGroupLeader = true);
    void load_consumer();
    template <class F, typename ...Args>
    ann_helper::Task *run_task(F &&f, Args &&...args)
    {
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        ann_helper::Task *task_slot = _num_consumers > 0 || !_use_bgworker ?
            _pool.add_task(std::move(task), true) : NULL;
        if (!task_slot) {
            task();
        }
        return task_slot;
    }
    /* release all background worker, useful since all workers are doing busy waiting */
    void finish();
    void resign_producer();
    void wait(ann_helper::Task *task);
    void destroy();
    void consume(bool need_enroll = true) { _pool.consume(need_enroll); }
    void pure_consume() { _pool.pure_consume(); }
    void consumer_exit() { _pool.consumer_resign(); }
    void record_consumer() { _pool.add_consumer(); }
    void sub_consumer() { _pool.sub_consumer(); }
    void set_backrunnerpara(void *runner_para) { _backrunnerpara = runner_para; }
    void *get_backrunnerpara() { return _backrunnerpara; }
    MemoryContext get_context() const { return _pool._ctx; }
private:
    bool _use_bgworker{true};
    bool _vec_worker_launched{false};
    size_t _num_consumers{0};
    void *_backrunnerpara{nullptr};
    ann_helper::TaskPool _pool;
};

#define INIT_TASK_RUNNER()                  \
    do {                                    \
        if (!TASK_RUNNER_PTR) {             \
            TASK_RUNNER_PTR = TaskRunner::create_instance();    \
            pg_read_barrier();              \
        }                                   \
    } while(0)
#define LOAD_CONSUMER() TASK_RUNNER->load_consumer()
#define LAUNCH_CONSUMER(n) TASK_RUNNER->launch_consumer(n)
#define LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(n) TASK_RUNNER->launch_consumer(n, false)
#define START_TASK_POOL() Vector<ann_helper::Task *> __tp_tasks
#define START_TASK_POOL_NTASK(ntask) Vector<ann_helper::Task *> __tp_tasks(ntask)
/* note that the memory management of the lambda function is messed up,
 * try not to pass anything stl object by value into the lambda  */
#define RUN_TASK(f, ...)                    \
    do {                                    \
        auto *__tp_task = TASK_RUNNER->run_task(f, ##__VA_ARGS__); \
        if (__tp_task) {                    \
            __tp_tasks.push_back(__tp_task);\
        }                                   \
    } while(0)
/* better wait for task before leaving the scope of launched tasks,
 * I don't know the aftermath otherwise, should be fine though */
#define WAIT_TASK()                         \
    do {                                    \
        for (auto *task : __tp_tasks) {     \
            TASK_RUNNER->wait(task);        \
            task->destroy();                \
            pfree(task);                    \
        }                                   \
        __tp_tasks.clear();                 \
    } while(0)
#define END_TASK_POOL() __tp_tasks.destroy()
#define WAIT_AND_END_TASK_POOL()            \
    do {                                    \
        for (auto *task : __tp_tasks) {     \
            TASK_RUNNER->wait(task);        \
            task->destroy();                \
            pfree(task);                    \
        }                                   \
        END_TASK_POOL();                    \
    } while(0)
#define FINISH_TASK() TASK_RUNNER->finish()
#define RESIGN_PRODUCER() TASK_RUNNER->resign_producer()
#define DESTROY_TASK_RUNNER()               \
    do {                                    \
        if (TASK_RUNNER_PTR) {              \
            FINISH_TASK();                  \
            TASK_RUNNER->destroy();         \
            TASK_RUNNER_PTR = NULL;         \
            pg_memory_barrier();            \
        }                                   \
    } while(0)

#define PARALLEL_BATCH_RUN_INIT() \
	size_t __avgNum = 0;           \
	size_t __remainingNum = 0;     \
	size_t __start = 0;            \
	size_t __end = 0;   

/* note that total_para_workers include the client thread */
#define PARALLEL_BATCH_RUN_TASK_WAIT(total_task, total_para_workers, f, ...)          \
    if (total_para_workers <= 1) {                                                    \
         f(0, 0, total_task, ##__VA_ARGS__);                                          \
    } else {                                                                          \
        __avgNum = total_task / total_para_workers;                                   \
        __remainingNum = total_task % total_para_workers;                             \
        __start = 0;                                                                  \
        __end = __avgNum;                                                             \
        for (size_t batchIndex = 0; batchIndex < (size_t)total_para_workers; ++batchIndex) {     \
            if (__remainingNum > 0) {                                                 \
                ++__end;                                                              \
                --__remainingNum;                                                     \
            }                                                                         \
            RUN_TASK(f, batchIndex, __start, __end, ##__VA_ARGS__);                   \
            __start = __end;                                                          \
            __end += __avgNum;                                                        \
        }                                                                             \
        WAIT_TASK();                                                                  \
    }


#endif /* TASKPOOL_H */
