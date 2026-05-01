/**
 * Copyright ...
 * read write implementation for vector forknum
 */

#define RECORD_BUFFER_STATS TRUE

#include <algorithm>    /* std::min */
#include <atomic>
#if RECORD_BUFFER_STATS && !defined(BOOST_UNORDERED_ENABLE_STATS)
#define BOOST_UNORDERED_ENABLE_STATS
#endif /* BOOST_UNORDERED_ENABLE_STATS */
#ifndef BOOST_ASSERT_IS_VOID
#define BOOST_ASSERT_IS_VOID
#endif
#ifndef BOOST_UNORDERED_DISABLE_REENTRANCY_CHECK
#define BOOST_UNORDERED_DISABLE_REENTRANCY_CHECK
#endif
#ifndef BOOST_NO_EXCEPTIONS
#define BOOST_NO_EXCEPTIONS
#endif
#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/lockfree/queue.hpp>

#include <vtl/hashtable>
#include <vtl/holder>

#include "c.h"
#include "pgstat.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "storage/vfd.h"
#include "storage/ipc.h"
#include "storage/smgr/fd.h"
#include "storage/smgr/relfilenode.h"
#include "access/extreme_rto/standby_read/standby_read_base.h"
#include "access/diskann/diskann.h"
#include "access/diskann/vector_bt.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/annvector/distance/distance.h"
#include "access/annvector/xlog/log_manager.h"
#include "catalog/storage_xlog.h"
#include "funcapi.h"
#include "access/annvector/module/size_format.h"

using namespace ann_helper;

#define RELAXED_ORDER std::memory_order_relaxed
#define RELEASE_ORDER std::memory_order_release
#define ACQUIRE_ORDER std::memory_order_acquire

struct VecBufferManager;
#define VecBufMgr ((VecBufferManager *)g_instance.diskann_cxt.vec_buffer_mgr)
constexpr size_t max_file_size = RELSEG_SIZE * BLCKSZ;
constexpr uint32 min_cached_dim = 8u;
constexpr uint32 vec_block_size_bits = 20u; /* log2(2MB) */
constexpr uint32 vec_block_size = 1u << vec_block_size_bits;    /* 2MB per block */
constexpr uint32 vec_block_float_size_bits = vec_block_size_bits - 2u;  /* log2(2MB / sizeof(float)) */
constexpr uint32 cqueue_capacity = vec_block_size / sizeof(float) / min_cached_dim * 1.5;
constexpr uint32 cqueue_edge = 8u * 4u;
/* at least 550MB needed for sift1m */
/* at least 2896MB needed for gist1m */
#define NVecBuf (uint32(g_instance.diskann_cxt.vector_buffers) >> 10)
constexpr int16 NVecPool =
    int16(DISKANN_MAX_DIM + vector_step_size - 1) / vector_step_size + 1;
constexpr long eviction_time_interval = 10l;

constexpr static bool verify_file_content(const char *content)
{
    return true;
}

static SMGR_READ_STATUS read_vector_no_error(Relation rel, size_t loc, size_t elem_size, char *vec, VecStorageType vec_storage_type)
{
    off_t offset = loc * elem_size;
    return vec_read(rel->rd_smgr, offset, elem_size, vec, vec_storage_type);
}

static void report_read_vector_error(SMGR_READ_STATUS status, Relation rel, size_t loc)
{
    if (status == SMGR_RD_NO_BLOCK) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("could not read vector at loc %lu in relation \"%s\"",
                               loc, rel->rd_rel->relname.data)));
    } else {
        Assert(status == SMGR_RD_CRC_ERROR);
        errfinish(0);
    }
}

struct __attribute__((packed)) BufferSignature {
    Oid rel_id;
    size_t offset;
    bool operator==(const BufferSignature &rhs) const noexcept
        { return offset == rhs.offset && rel_id == rhs.rel_id; }
    struct Hasher {
        size_t operator()(const BufferSignature &sig) const noexcept
        {
            /* 1. most useful bits of offset is located at the least 32 significant bits,
             * and it's ok for 0x10000001 and 0x1 to collide, they are not likely to exist togather */
            /* 2. addition behaves similar to boost combine
             * if we don't think of hash attack which is not a thing in our case */
            /* 3. it seems that our uint32_hash is much worse than std::hash,
             * I did not measure their speed diff but their hash qualities are not even close */
            return std::hash<Oid>()(sig.rel_id) + std::hash<uint32>()(uint32(sig.offset));
        }
        /* theoretically best hash quality */
        size_t reserved_hash(const BufferSignature &sig) const
        {
            size_t seed = std::hash<size_t>()(sig.offset);
            /* boost hash_combine algorithm */
            return seed ^ (std::hash<Oid>()(sig.rel_id) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
        }
    };
};
static_assert(sizeof(BufferSignature) == 12ul);

struct VecBufferTag {
    std::atomic<uint32> ref_count{0};
    BufferSignature sig;
};
struct BufferPoolStats {
    slock_t lock;
    uint32 nblock{0u};
    uint32 ndata{0u};
    uint32 *blocks{NULL};
    TimestampTz first_evict_time{0};
    std::atomic<size_t> nevict{0};
    uint32 get_rand_block()
    {
        Assert(nblock > 0);
        return blocks[random() % nblock];
    }
#if VERIFY_BUFFER
    void verify_loc(const VecBufferLoc &loc) const
    {
        uint32 start_idx = nblock - 1u;
        for (int i = (int)start_idx; i >= 0; --i) {
            if (loc.buf_offset == blocks[i]) {
                return;
            }
        }
        for (uint32 i = start_idx; i < nblock; ++i) {
            if (loc.buf_offset == blocks[i]) {
                return;
            }
        }
        elog(PANIC, "got loc (%u,%u) that does not in corresponding block", loc.buf_offset, loc.offset);
    }
#else
    FORCE_INLINE static void verify_loc(const VecBufferLoc &loc) {}
#endif /* VERIFY_BUFFER */
};

template<>
struct std::hash<VecBufferLoc> {
    size_t operator()(const VecBufferLoc& loc) const noexcept
        { return std::hash<uint64>()((uint64(loc.buf_offset) << 32) | loc.offset); }
};

static_assert(!constructor_need_ctx<VecBufferLoc>, "compiler error on VecBufferLoc");
static_assert(!constructor_need_ctx<std::pair<const BufferSignature, VecBufferLoc>>,
              "compiler error on std::pair<const BufferSignature, VecBufferLoc>");

using bufmap_ctx = HUGE_ALLOCATOR<std::pair<const BufferSignature, VecBufferLoc>>;
using bufmap = boost::unordered::concurrent_flat_map<
    BufferSignature, VecBufferLoc,
    BufferSignature::Hasher, std::equal_to<BufferSignature>, bufmap_ctx>;
using cqueue_ctx = CONTEXT_ALLOCATOR<VecBufferLoc>;
using cqueue = boost::lockfree::queue<VecBufferLoc,
    boost::lockfree::allocator<cqueue_ctx>,
    boost::lockfree::capacity<cqueue_capacity + cqueue_edge>>;

struct VecBufferPool : public BaseObject {
    BufferPoolStats stats{};
    uint32 freezing_block{0u};
    std::atomic<uint32> nfreeze{0u};
    std::atomic<uint32> nfreed{0u};
    MemoryContext ctx;
    Holder<cqueue> freelist{};
    Holder<bufmap> locmap{};
    bool accepting_block{false};
#if RECORD_BUFFER_STATS
    std::atomic<size_t> hit{0};
    std::atomic<size_t> miss{0};
#endif /* RECORD_BUFFER_STATS */

    explicit VecBufferPool(MemoryContext in_ctx) : ctx(in_ctx)
    {
        freelist.emplace(cqueue_ctx(ctx));
        constexpr uint32 init_size = cqueue_capacity * 2u;
        locmap.emplace(init_size, bufmap_ctx(ctx));
        SpinLockInit(&stats.lock);
        stats.blocks = (uint32 *)MemoryContextAllocZero(ctx, NVecBuf * sizeof(uint32));
    }
    void destroy()
    {
        Assert(stats.nblock == 0);
        freelist->~cqueue();
        locmap->~bufmap();
        SpinLockFree(&stats.lock);
        pfree_ext(stats.blocks);
        MemoryContextDelete(ctx);
    }

    void wait_freelist_freeze()
    {
        Assert(freezing_block > 0);
        uint32 target_block = freezing_block - 1;
        const uint32 total = nfreeze.load(RELAXED_ORDER) + cqueue_edge;
        UnorderedSet<VecBufferLoc> seen(total);
        uint32 count = 0;
        VecBufferLoc loc;
        while (freelist->pop(loc)) {
            if (seen.contains(loc)) {
                while (!freelist->push(loc)) {}
                break;
            }
            if (loc.buf_offset != target_block) {
                seen.insert(loc);
                while (!freelist->push(loc)) {}
            } else {
                nfreed.fetch_add(1u, RELAXED_ORDER);
                nfreeze.fetch_sub(1u, RELAXED_ORDER);
            }
            if (++count >= total) {
                break;
            }
        }
        optional_destroy(seen);
    }
    void wait_locmap_freeze(uint32 pool_max_offset);
    bool wait_freeze(uint32 block, uint32 pool_max_offset)
    {
        if (freezing_block != 0u) {
            Assert(freezing_block != block + 1u);
            return false;
        }
        freezing_block = block + 1u;
        do {
            wait_freelist_freeze();
            wait_locmap_freeze(pool_max_offset);
        } while (nfreed.load(RELEASE_ORDER) < pool_max_offset);
        return true;
    }
    void assign_block(uint32 block, uint32 max_offset)
    {
        for (uint32 i = 0; i < max_offset; ++i) {
            VecBufferLoc loc(block, i);
            push_freelist(loc);
        }
    }

    bool pop_freelist(VecBufferLoc &loc)
    {
        while (freelist->pop(loc)) {
            nfreeze.fetch_sub(1u, RELAXED_ORDER);
            if (loc.buf_offset != freezing_block - 1u) {
                return true;
            }
            nfreed.fetch_add(1u, RELAXED_ORDER);
        }
        return false;
    }
    bool try_push_freelist(const VecBufferLoc &loc)
    {
        bool res = freelist->push(loc);
        if (res) {
            nfreeze.fetch_add(1u, RELAXED_ORDER);
        }
        return res;
    }
    void push_freelist(const VecBufferLoc &loc)
    {
        nfreeze.fetch_add(1u, RELAXED_ORDER);
        bool res = freelist->push(loc);
        if (unlikely(!res)) {
            do {
                if (t_thrd.vector_cxt.shutdown_requested) {
                    return;
                }
                SPIN_DELAY();
                res = freelist->push(loc);
            } while (!res);
        }
    }
    bool need_evict()
    {
        uint32 nf = nfreeze.load(ACQUIRE_ORDER);
        if (nf >= cqueue_capacity - cqueue_edge || accepting_block) {
            return false;
        }
        /* VEC TD: make it dynamic */
        return nf < stats.ndata * 0.004 + 2u && stats.nblock > (freezing_block == 0 ? 0 : 1);
    }
    FORCE_INLINE void verify_loc(const VecBufferLoc &loc) const { stats.verify_loc(loc); }
};

struct VecBufferManager : public BaseObject {
    float *buf{NULL};
    slock_t *tag_locks;
    VecBufferTag **tag{NULL};
    VecBufferPool *pool[NVecPool];
    uint32 nalloced{0};
    bool buffer_inited{false};
    VecBufferManager()
    {
        for (size_t i = 0; i < NVecPool; ++i) {
            pool[i] = NULL;
        }
    }
    void init_buffer()
    {
        Assert(!buffer_inited);
        auto old_ctx = MemoryContextSwitchTo(g_instance.diskann_cxt.vec_buf_ctx);
        const size_t nvecbuf = NVecBuf;
        tag_locks = (slock_t *)palloc(nvecbuf * sizeof(slock_t));
        tag = (VecBufferTag **)palloc0(nvecbuf * sizeof(VecBufferTag *));
        Size buf_size = nvecbuf * vec_block_size + vector_aligned_size;
        void *temp = buf_size > MaxAllocSize ? palloc_huge(CurrentMemoryContext, buf_size) : palloc(buf_size);
        if (!temp || !tag ||
            !std::align(vector_aligned_size, nvecbuf * vec_block_size,
                        temp, buf_size)) {
            ereport(PANIC, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Vector shared buffer init failed")));
        }
        buf = (float *)temp;
        MemoryContextSwitchTo(old_ctx);
        buffer_inited = true;
    }

    static int16 get_pool_offset(uint32 dim)
    {
        Assert(dim >= min_cached_dim);
        return (dim + vector_step_size - 1) / vector_step_size;
    }

    static uint32 get_pool_max_offset(int16 pool_offset)
    {
        return vec_block_size /
            (pool_offset * vector_step_size) /
            sizeof(float);
    }

    char *get_vector(uint32 block, uint32 offset, uint32 dim)
    {
        return (char *)(buf + (uint64(block) << vec_block_float_size_bits) +
                     offset * get_aligned_dim(dim));
    }

    void try_init_pool(int16 pool_offset)
    {
        if (!pool[pool_offset]) {
            MemoryContext ctx = AllocSetContextCreate(g_instance.diskann_cxt.vec_buf_ctx,
                "VecBufferPool", ALLOCSET_DEFAULT_SIZES, SHARED_CONTEXT);
            VecBufferPool *temp = New (ctx) VecBufferPool(ctx);
            pg_memory_barrier();
            pool[pool_offset] = temp;
        }
    }

    void append_block(int16 pool_offset, uint32 block)
    {
        try_init_pool(pool_offset);
        VecBufferPool &cur_pool = *pool[pool_offset];
        const uint32 max_offset = get_pool_max_offset(pool_offset);
        Assert(tag[block] == NULL);
        tag[block] = (VecBufferTag *)MemoryContextAllocZero(
            g_instance.diskann_cxt.vec_buf_ctx, max_offset * sizeof(VecBufferTag));
        cur_pool.stats.blocks[cur_pool.stats.nblock] = block;
        pg_memory_barrier();
        ++cur_pool.stats.nblock;
        cur_pool.assign_block(block, max_offset);
        cur_pool.stats.ndata += max_offset;
    }
    
    void remove_block(int16 pool_offset, uint32 block)
    {
        Assert(pool[pool_offset]);
        VecBufferPool &cur_pool = *pool[pool_offset];
        Assert(cur_pool.freezing_block == block + 1u);
        Assert(cur_pool.stats.nblock > 0);
        uint32 max_offset = get_pool_max_offset(pool_offset);
        SpinLockAcquire(&cur_pool.stats.lock);
        uint32 *pos = std::find(cur_pool.stats.blocks, cur_pool.stats.blocks + cur_pool.stats.nblock, block);
        Assert(pos != cur_pool.stats.blocks + cur_pool.stats.nblock);
        *pos = cur_pool.stats.blocks[cur_pool.stats.nblock - 1u];
        --cur_pool.stats.nblock;
        cur_pool.stats.ndata -= max_offset;
        SpinLockRelease(&cur_pool.stats.lock);
        SpinLockAcquire(tag_locks + block);
        pfree_ext(tag[block]);
        SpinLockRelease(tag_locks + block);
    }

    bool do_evict(int16 pool_offset)
    {
        if (!pool[pool_offset]) {
            return false;
        }
        auto &cur_pool = *pool[pool_offset];
        if (cur_pool.accepting_block || cur_pool.stats.nblock == 0) {
            return false;
        }
        SpinLockAcquire(&cur_pool.stats.lock);
        uint32 block = cur_pool.stats.get_rand_block();
        if (block == cur_pool.freezing_block - 1u) {
            SpinLockRelease(&cur_pool.stats.lock);
            return false;
        }
        SpinLockAcquire(tag_locks + block);
        SpinLockRelease(&cur_pool.stats.lock);
        if (!tag[block]) {
            SpinLockRelease(tag_locks + block);
            return false;
        }
        const uint32 max_offset = get_pool_max_offset(pool_offset);
        uint32 offset = random() % max_offset;
        bool can_push = true;
        for (uint32 count = 0; count < max_offset; ++count) {
            Assert(tag[block]);
            BufferSignature &sig = tag[block][offset].sig;
            if (!OidIsValid(sig.rel_id)) {
                offset = (offset + 1) % max_offset;
                continue;
            }
            if (cur_pool.locmap->erase_if(sig, [&](auto &x) -> bool {
                if (x.second.empty()) {
                    can_push = false;
                    return true;
                }
                if (!x.second.valid()) {
                    sig.rel_id = InvalidOid;
                    if (x.second.valid_buf_offset() == cur_pool.freezing_block - 1u) {
                        can_push = false;
                        cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                        return true;
                    }
                    VecBufferLoc loc{x.second.valid_buf_offset(), x.second.offset};
                    can_push = cur_pool.try_push_freelist(loc);
                    return can_push;
                }
                const VecBufferTag &t = tag[x.second.buf_offset][x.second.offset];
                if (t.ref_count.load(ACQUIRE_ORDER) > 0) {
                    return false;
                }
                sig.rel_id = InvalidOid;
                if (x.second.buf_offset == cur_pool.freezing_block - 1u) {
                    can_push = false;
                    cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                    return true;
                }
                cur_pool.verify_loc(x.second);
                can_push = cur_pool.try_push_freelist(x.second);
                return can_push;
            }) > 0) {
                SpinLockRelease(tag_locks + block);
                return can_push;
            }
            if (!can_push) {
                SpinLockRelease(tag_locks + block);
                return false;
            }
            offset = (offset + 1) % max_offset;
        }
        SpinLockRelease(tag_locks + block);
        return false;
    }

    void expand_or_recollect_space(int16 pool_offset, bool &evicted)
    {
        const bool zero_buf = pool[pool_offset] == NULL;
        evicted = false;
        if (nalloced < NVecBuf || zero_buf) {
            if (!LWLockConditionalAcquire(VectorBufferLock, LW_EXCLUSIVE)) {
                if (zero_buf) {
                    constexpr long delay = 5'000l;
                    pg_usleep(delay);
                }
                return;
            }
            if (!buffer_inited) {
                init_buffer();
            }
            if (nalloced < NVecBuf) {
                SpinLockInit(tag_locks + nalloced);
                append_block(pool_offset, nalloced);
                ++nalloced;
            } else if (zero_buf) {
                try_init_pool(pool_offset);
                int16 src_pool_offset, unused;
                find_eviction_min_max_freq_offset(src_pool_offset, unused);
                if (src_pool_offset != -1 && src_pool_offset != pool_offset) {
                    redistribute_block(src_pool_offset, pool_offset);
                    pool[pool_offset]->stats.first_evict_time = GetCurrentTimestamp();
                }
            }
            LWLockRelease(VectorBufferLock);
            return;
        }
        if (!zero_buf) {
            evicted = do_evict(pool_offset);
        }
    }

    void try_redistribute_block()
    {
        if (nalloced < NVecBuf) {
            return;
        }
        if (!LWLockConditionalAcquire(VectorBufferLock, LW_EXCLUSIVE)) {
            return;
        }
        int16 src_pool_offset, dest_pool_offset;
        if (find_eviction_min_max_freq_offset(src_pool_offset, dest_pool_offset)) {
            redistribute_block(src_pool_offset, dest_pool_offset);
        }
        LWLockRelease(VectorBufferLock);
    }

    bool find_eviction_min_max_freq_offset(int16 &min_offset, int16 &max_offset)
    {
        bool do_redist = false;
        min_offset = -1;
        max_offset = -1;
        double min_score = DBL_MAX;
        double max_score = 0;
        for (int16 i = 0; i < NVecPool; ++i) {
            if (!pool[i]) {
                continue;
            }
            auto &cur_pool = *pool[i];
            if (cur_pool.stats.nblock == 0) {
                continue;
            }
            if (cur_pool.freezing_block != 0) {
                continue;
            }
            long sec;
            int usec;
            TimestampTz cur_time = GetCurrentTimestamp();
            TimestampDifference(cur_pool.stats.first_evict_time, cur_time, &sec, &usec);
            if (sec <= eviction_time_interval + 1l) {
                do_redist = true;
            } else if (cur_pool.stats.nblock > 2u && sec > 2 * eviction_time_interval) {
                min_offset = i;
                min_score = -1.0;
                continue;
            }
            double freq = cur_pool.stats.nevict.load(RELAXED_ORDER) / std::max(1e-5, sec + (double)usec / 1e6);
            double score = freq / std::pow(cur_pool.stats.nblock, 0.75);
            if (score > max_score) {
                max_score = score;
                max_offset = i;
            }
            constexpr long min_sec_to_empty = 30l * 60l + 997l;
            if (score < min_score && (cur_pool.stats.nblock > 2u || sec > min_sec_to_empty)) {
                min_score = score;
                min_offset = i;
            }
        }
        constexpr double min_score_threshold = 0.25;
        return do_redist && min_offset != -1 && max_offset != -1 &&
            min_score * (1 + min_score_threshold) < max_score;
    }

    void redistribute_block(int16 src_pool_offset, int16 dest_pool_offset)
    {
        auto &src_pool = *pool[src_pool_offset];
        auto &dest_pool = *pool[dest_pool_offset];
        Assert(!dest_pool.accepting_block);
        START_CRIT_SECTION();
        dest_pool.accepting_block = true;
        pg_read_barrier();
        if (dest_pool.nfreeze + get_pool_max_offset(dest_pool_offset) <= cqueue_capacity) {
            uint32 block = src_pool.stats.get_rand_block();
            if (src_pool.wait_freeze(block, get_pool_max_offset(src_pool_offset))) {
                remove_block(src_pool_offset, block);
                append_block(dest_pool_offset, block);
                if (src_pool.stats.nblock > 0) {
                    src_pool.freezing_block = 0;
                    src_pool.nfreed = 0;
                } else {
                    VecBufferPool *cur_pool = pool[src_pool_offset];
                    pool[src_pool_offset] = NULL;
                    constexpr long sleep_interval = 32768l; /* 32ms */
                    pg_usleep(sleep_interval);
                    cur_pool->destroy();
                }
            }
        }
        dest_pool.accepting_block = false;
        END_CRIT_SECTION();
    }

    void async_expand_or_recollect_space(int16 pool_offset)
    {
        g_instance.diskann_cxt.pool_offset_to_write = pool_offset;
        int offset = random() % g_instance.diskann_cxt.vec_writer_nproc;
        Latch *latch = g_instance.diskann_cxt.vec_writer_latch[offset];
        if (latch) {
            SetLatch(latch);
        }
    }

    VecBuffer get_buffer(Relation rel, size_t loc, uint32 elem_size, VecStorageType st, bool &success)
    {
        uint32 dim = (elem_size + 3) / 4;
        if (dim < min_cached_dim) {
            success = false;
            return VecBuffer();
        }
        int16 pool_offset = get_pool_offset(dim);
        if (!pool[pool_offset]) { /* don't try anything when the pool is not inited */
            async_expand_or_recollect_space(pool_offset);
            success = false;
            return VecBuffer();
        }
        VecBufferPool &cur_pool = *pool[pool_offset];
        BufferSignature sig = {rel->rd_smgr->smgr_rnode.node.relNode, loc};
        BufferParams params = {rel, loc, elem_size, pool_offset, st};
        uint32 retry_count = 0;
        /* we need a high retry to ensure high evict rate */
        constexpr uint32 max_retry = 16u;
retry:
        if (cur_pool.locmap->try_emplace_or_cvisit(sig, params,
            [&](const auto &x) {
                Assert(loc == x.first.offset);
                params.buf_offset = x.second.buf_offset;
                params.offset = x.second.offset;
                if (!x.second.valid()) {
                    return;
                }
#if VERIFY_BUFFER
                if (tag[x.second.buf_offset][x.second.offset].sig.rel_id != rel->rd_smgr->smgr_rnode.node.relNode) {
                    elog(PANIC, "Assert error on accessing (%u,%u), where id is %u which is supposed to be %u",
                                x.second.buf_offset, x.second.offset,
                                tag[x.second.buf_offset][x.second.offset].sig.rel_id,
                                rel->rd_smgr->smgr_rnode.node.relNode);
                }
#endif /* VERIFY_BUFFER */
                tag[x.second.buf_offset][x.second.offset].ref_count.fetch_add(1u, ACQUIRE_ORDER);
                ResourceOwnerEnlargeVectorBuffers(t_thrd.utils_cxt.CurrentResourceOwner);
                ResourceOwnerRememberVectorBuffer(t_thrd.utils_cxt.CurrentResourceOwner, x.second);
            })) {
#if RECORD_BUFFER_STATS
            cur_pool.miss.fetch_add(1ul, RELAXED_ORDER);
        } else {
            cur_pool.hit.fetch_add(1ul, RELAXED_ORDER);
#endif /* RECORD_BUFFER_STATS */
        }
        if (params.buf_offset & VecBufferLoc::invalid_mask) {
            cur_pool.locmap->erase_if(sig, [](auto &x) { return x.second.empty(); });
            if (unlikely(params.status != SMGR_RD_OK)) {
                report_read_vector_error(params.status, params.rel, params.loc);
            }
            if (InterruptPending) {
                ProcessInterrupts();
                goto failed;
            }
            ++retry_count;
            if (retry_count > max_retry || !pool[pool_offset]) {
                goto failed;
            }
            if (nalloced >= NVecBuf) {
                if (do_evict(params.pool_offset)) {
                    cur_pool.stats.nevict.fetch_add(1ul, RELAXED_ORDER);
                } else if (cur_pool.nfreed.load(RELAXED_ORDER) <= 1ul) {
                    goto failed;
                }
            }
            cur_pool.miss.fetch_sub(1ul, RELAXED_ORDER);
            goto retry;
        }
        success = true;
#if VERIFY_BUFFER
        return VecBuffer(rel, (int16)pool_offset, params.buf_offset, params.offset
            get_vector(params.buf_offset, params.offset, dim));
#else
        return VecBuffer((int16)pool_offset, params.buf_offset, params.offset,
            get_vector(params.buf_offset, params.offset, dim));
#endif /* VERIFY_BUFFER */
failed:
        success = false;
        return VecBuffer();
    }
};

VecBufferLoc::VecBufferLoc(BufferParams &params)
{
    constexpr uint32 spins_per_delay = 40u;
    constexpr uint32 max_spins = 3u * spins_per_delay;
    constexpr long min_delay_usec = 512l;
    constexpr long max_delay_usec = 65'536l;
    auto &mgr = *VecBufMgr;
    uint32 spins = 0; /* we start with wait */
    long delay = min_delay_usec;
    Assert(mgr.pool[params.pool_offset]);
    auto &pool = *mgr.pool[params.pool_offset];
    VecBufferLoc loc;
    while (!pool.pop_freelist(loc)) {
        if (spins % spins_per_delay == 0) {
            /* release the lock for eviction contention */
            if (InterruptPending || spins > max_spins) {
                loc.set_empty();
                loc.set_invalid();
                break;
            }
            mgr.async_expand_or_recollect_space(params.pool_offset);
            pg_usleep(delay);
            delay += (delay * ((double) random() / (double) MAX_RANDOM_VALUE) + 0.5);
            if (delay > max_delay_usec) {
                delay = max_delay_usec;
            }
        }
        SPIN_DELAY();
        ++spins;
    }
    buf_offset = params.buf_offset = loc.buf_offset;
    offset = params.offset = loc.offset;
    if (!loc.valid()) {
        return;
    }
    pool.verify_loc(loc);
    VecBufferTag &tag = mgr.tag[loc.buf_offset][loc.offset];
    tag.ref_count.fetch_add(1u, ACQUIRE_ORDER);
    tag.sig = {params.rel->rd_smgr->smgr_rnode.node.relNode, params.loc};
    ResourceOwnerEnlargeVectorBuffers(t_thrd.utils_cxt.CurrentResourceOwner);
    ResourceOwnerRememberVectorBuffer(t_thrd.utils_cxt.CurrentResourceOwner, loc);
    size_t dim = (params.elem_size + 3) / 4;
    switch (params.storage_type) {
        case VecStorageType::PureVec: {
            char *temp = mgr.get_vector(loc.buf_offset, loc.offset, dim);
            params.status = read_vector_no_error(params.rel, params.loc, params.elem_size, temp);
        } break;
        case VecStorageType::PureCode: {
            char *temp = (char *)mgr.get_vector(loc.buf_offset, loc.offset, dim);
            params.status = read_qtcode_no_error(params.rel, params.loc, params.elem_size, temp, false);
        } break;
        case VecStorageType::VecWithCode: {
            char *temp = mgr.get_vector(loc.buf_offset, loc.offset, dim);
            params.status = read_vector_no_error(params.rel, params.loc, params.elem_size, temp, true);
        } break;
        case VecStorageType::CodeWithVec: {
            char *temp = (char *)mgr.get_vector(loc.buf_offset, loc.offset, dim);
            params.status = read_qtcode_no_error(params.rel, params.loc, params.elem_size, temp);
        } break;
    }
    if (unlikely(params.status != SMGR_RD_OK)) {
        if (pool.try_push_freelist(loc)) {
            loc.set_empty();
        }
        loc.set_invalid();
        buf_offset = params.buf_offset = loc.buf_offset;
        offset = params.offset = loc.offset;
    }
}

bool enable_vec_buffer_manager() { return true; }

void init_vector_smgr()
{
    if (enable_vec_buffer_manager() && !dummyStandbyMode) {
        g_instance.diskann_cxt.vec_buffer_mgr =
            New (g_instance.diskann_cxt.vec_buf_ctx) VecBufferManager();
    }
}

size_t vec_buffer_verify(uint32 dim, size_t &total_slot)
{
    total_slot = 0;
    if (dim < min_cached_dim) {
        ereport(ERROR, (errmsg("Dimension %u less than the minimum dim %u.", dim, min_cached_dim)));
    }

    auto *mgr_ptr = VecBufMgr->pool[VecBufferManager::get_pool_offset(dim)];
    if (!mgr_ptr) {
        ereport(WARNING, (errmsg("Buffer pool for dimension %u is empty", dim)));
        return 0;
    }
    auto &mgr = *mgr_ptr;
    struct rel_holder {
        Relation rel;
        bool valid;
        rel_holder(Relation r, bool v) : rel(r), valid(v) {}
    };
    UnorderedMap<Oid, rel_holder> rel_cache;
    const size_t buf_size = sizeof(float) * dim;
    char *buf = (char *)palloc(buf_size);
    size_t cnt = 0;
    mgr.locmap->cvisit_all([&](const auto &kv) {
        const BufferSignature &sig = kv.first;
        const VecBufferLoc &loc = kv.second;
        if (loc.empty()) {
            ereport(WARNING, (errmsg("Found invalid buffer cache at Relation %u loc %lu.",
                                     sig.rel_id, sig.offset)));
            return;
        }
        auto it = rel_cache.find(sig.rel_id);
        if (it == rel_cache.end()) {
            Relation rel = try_relation_open(sig.rel_id, AccessShareLock);
            if (RelationIsValid(rel)) {
                RelationOpenSmgr(rel);
            }
            it = rel_cache.emplace(sig.rel_id, rel, RelationIsValid(rel)).first;
        }
        if (!it->second.valid) {
            return;
        }
        Relation rel = it->second.rel;
        read_vector(rel, sig.offset, dim, buf);
        const float *res = (float *)VecBufMgr->get_vector(loc.buf_offset, loc.offset, dim);
        if (memcmp(buf, res, buf_size) != 0) {
            ereport(WARNING, (
                errmsg("Found corrupted buffer cache at Relation %u loc %lu, at %u:%u.",
                       sig.rel_id, sig.offset, loc.buf_offset, loc.offset)));
            ++cnt;
        }
        ++total_slot;
    });
    for (auto &kv : rel_cache) {
        if (kv.second.valid) {
            index_close(kv.second.rel, AccessShareLock);
        }
    }
    optional_destroy(rel_cache);
    pfree(buf);
    return cnt;
}

VecBuffer vec_read_buffer(Relation rel, size_t loc, size_t vec_size, VecStorageType st)
{
    bool success;
    if (unlikely(!rel->rd_smgr)) {
        RelationOpenSmgr(rel);
    }
    VecBuffer res = VecBufMgr->get_buffer(rel, loc, vec_size, st, success);
    if (!success) {
        res.pool_offset = -1;
        res.buf = alloc_vector(vec_size);
        bool with_code = st == VecStorageType::VecWithCode;
        read_vector(rel, loc, vec_size, res.buf, with_code);
    }
    return res;
}

VecBuffer vec_read_quant(Relation rel, size_t loc, size_t code_len, VecStorageType st)
{
    bool success;
    if (unlikely(!rel->rd_smgr)) {
        RelationOpenSmgr(rel);
    }
    VecBuffer res = VecBufMgr->get_buffer(rel, loc, code_len, st, success);
    if (!success) {
        res.pool_offset = -1;
        res.buf = alloc_vector(code_len);
        switch (st) {
            case VecStorageType::PureCode: {
                read_qtcode(rel, loc, code_len, (char *)res.buf, false);
            } break;
            case VecStorageType::CodeWithVec: {
                read_qtcode(rel, loc, code_len, (char *)res.buf);
            } break;
            default: {
                res.release();
                ereport(ERROR, (errmsg("Unsupported vector storage type: %d", (int)st)));
            } break;
        }
    }
    return res;
}

/* TD: send sig to read-only node, but we should really do it at wal */
void vec_invalidate_buffer_cache(Oid relNode, size_t loc, size_t elem_size)
{
    uint32 dim = (elem_size + 3) / 4;
    if (dim < min_cached_dim) {
        return;
    }
    int16 pool_offset = VecBufMgr->get_pool_offset(dim);
    if (!VecBufMgr->pool[pool_offset]) {
        return;
    }

    VecBufferPool &cur_pool = *VecBufMgr->pool[pool_offset];
    BufferSignature sig = {relNode, loc};
    bool res = true;
    cur_pool.locmap->erase_if(sig, [&res, &cur_pool](auto &x) {
        if (x.second.empty()) {
            return true;
        }
        auto loc = x.second;
        loc.set_valid();
        if (VecBufMgr->tag[loc.buf_offset][loc.offset].ref_count.load(ACQUIRE_ORDER) == 0) {
            if (loc.buf_offset == cur_pool.freezing_block - 1u) {
                cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
            if (cur_pool.freelist->push(loc)) {
                cur_pool.nfreeze.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
        }
        x.second.set_invalid();
        return false;
    });
}

void vec_invalidate_buffer_cache(Oid relNode, size_t elem_size)
{
    uint32 dim = (elem_size + 3) / 4;
    if (dim < min_cached_dim) {
        return;
    }
    int16 pool_offset = VecBufMgr->get_pool_offset(dim);
    if (!VecBufMgr->pool[pool_offset]) {
        return;
    }

    VecBufferPool &cur_pool = *VecBufMgr->pool[pool_offset];
    cur_pool.locmap->erase_if([&cur_pool, relNode](auto &x) {
        if (x.second.empty()) {
            return true;
        }
        if (x.first.rel_id != relNode) {
            return false;
        }
        auto loc = x.second;
        loc.set_valid();
        if (VecBufMgr->tag[loc.buf_offset][loc.offset].ref_count.load(ACQUIRE_ORDER) == 0) {
            if (loc.buf_offset == cur_pool.freezing_block - 1u) {
                cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
            if (cur_pool.freelist->push(loc)) {
                cur_pool.nfreeze.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
        }
        x.second.set_invalid();
        return false;
    });
}

size_t push_back_vector(Relation rel, BlockNumber data_meta, const float *point, uint32 dim)
{
    Buffer buf = ReadBuffer(rel, data_meta);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buf);
    auto meta = (DiskAnnDataMeta)PageGetContents(page);
    size_t res = meta->num_vectors++;
    MarkBufferDirty(buf);
    /* wal here */
    UnlockReleaseBuffer(buf);
    LogManager logmgr(rel);
    logmgr.log_write_vector(res * dim * sizeof(float), dim * sizeof(float), (char *)point, true);
    logmgr.destroy();
    write_vector(rel, res, dim, (char *)point);
    return res;
}

FORCE_INLINE void release_vector_buffer(const VecBufferLoc &loc)
{
    [[maybe_unused]]
    uint32 res = VecBufMgr->tag[loc.buf_offset][loc.offset].ref_count.fetch_sub(1u, RELEASE_ORDER);
    Assert(res > 0);
    ResourceOwnerForgetVectorBuffer(t_thrd.utils_cxt.CurrentResourceOwner, loc);
}

FORCE_INLINE void VecBuffer::release()
{
    if (pool_offset >= 0) {
#if VERIFY_BUFFER
        if (rel_id != VecBufMgr->tag[loc.buf_offset][loc.offset].sig.rel_id) {
            elog(PANIC, "Assert error on releasing (%u,%u), where id is %u which is supposed to be %u",
                        loc.buf_offset, loc.offset, VecBufMgr->tag[loc.buf_offset][loc.offset].sig.rel_id, rel_id);
        }
#endif /* VERIFY_BUFFER */
        release_vector_buffer(loc);
        return;
    }
    Assert(pool_offset == -1);
    free_vector(buf);
}

void VecBufferPool::wait_locmap_freeze(uint32 pool_max_offset)
{
    Assert(freezing_block > 0);
    const auto wait_until_no_ref = [](const VecBufferTag &tag) {
        uint32 count = 0;
        constexpr uint32 delay_per_count = 32u;
        constexpr uint32 max_spin = 8u * delay_per_count;
        constexpr long delay = 768l;
        do {
            ++count;
            if (count % delay_per_count == 0) {
                if (count > max_spin) {
                    return false;
                }
                pg_usleep(delay);
            }
            SPIN_DELAY();
        } while (tag.ref_count != 0);
        return true;
    };

    VecBufferTag *tag_arr = VecBufMgr->tag[freezing_block - 1u];
    size_t nremove = 0;
    for (uint32 i = 0; i < pool_max_offset; ++i) {
        VecBufferTag &tag = tag_arr[i];
        if (!OidIsValid(tag.sig.rel_id)) {
            continue;
        }

        for (bool removed;;) {
            removed = true;
            nremove += locmap->erase_if(tag.sig, [&](auto &x) {
                    while (tag.ref_count != 0 && !wait_until_no_ref(tag)) {
                        removed = false;
                        return false;
                    }
                    tag.sig.rel_id = InvalidOid;
                    return true;
                });
            if (removed) {
                break;
            }
            if (t_thrd.vector_cxt.shutdown_requested) {
                /* we don't expect database to be shut down with high contension */
                break;
            }
        }
    }
    nfreed.fetch_add(nremove, RELAXED_ORDER);
}

void create_vec_data(Relation rel, bool need_wal)
{
    RelationOpenSmgr(rel);
    smgrcreate(rel->rd_smgr, VECTOR_FORKNUM, false);
    if (need_wal) {
        log_smgrcreate(&rel->rd_smgr->smgr_rnode.node, VECTOR_FORKNUM);
    }
    smgrimmedsync(rel->rd_smgr, VECTOR_FORKNUM);
}

static void report_read_error(off_t offset, const char *rel_name, int read_bytes)
{
    if (errno > 0) {
        ereport(ERROR, (errcode_for_file_access(),
            errmsg("could not read offset %ld in file \"%s\": %m", offset, rel_name)));
    } else if (read_bytes == 0) {
        ereport(ERROR, (errcode_for_file_access(),
            errmsg("could not read offset %ld in file \"%s\": read zero byte", offset, rel_name)));
    } else {
        ereport(ERROR, (errcode_for_file_access(),
            errmsg("could not read offset %ld in file \"%s\": unknown error", offset, rel_name)));
    }
}

static void report_write_error(off_t offset, const char *rel_name, int write_bytes, int amount)
{
    if (write_bytes < 0) {
        ereport(ERROR, (errcode_for_file_access(),
            errmsg("could not write offset %ld in file \"%s\": %m", offset, rel_name)));
    }
    ereport(ERROR, (errcode(ERRCODE_DISK_FULL),
        errmsg("could not write offset %ld in file \"%s\": wrote only %d of %d bytes",
               offset, rel_name, write_bytes, amount),
        errhint("Check free disk space.")));
}

int pread_file(File file, char *buffer, int amount, off_t offset)
{
    int res = FileAccess(file);
    if (res < 0) {
        return res;
    }

    int count = 0;
    vfd *vfdcache = GetVfdCache();
retry:
    res = pread(vfdcache[file].fd, buffer, (size_t)amount, offset);
    if (res >= 0) {
        vfdcache[file].seekPos += res;
    } else {
        /* OK to retry if interrupted */
        if (errno == EINTR) {
            goto retry;
        }
        if (errno == EIO) {
            if (count < EIO_RETRY_TIMES) {
                ++count;
                ereport(WARNING, (errmsg("FilePRead: %d (%s) " INT64_FORMAT " %d \
                    failed, then retry: Input/Output ERROR",
                        file, vfdcache[file].fileName, (int64)vfdcache[file].seekPos, amount)));
                goto retry;
            }
        }
        /* Trouble, so assume we don't know the file position anymore */
        vfdcache[file].seekPos = (off_t)-1;
    }

    return res;
}

static int pwrite_file(File file, const char *buffer, int amount, off_t offset)
{
    int res = FileAccess(file);
    if (unlikely(res < 0)) {
        return res;
    }

    int count = 0;
    vfd *vfdcache = GetVfdCache();
retry:
    errno = 0;
    res = pwrite(vfdcache[file].fd, buffer, (size_t)amount, offset);

    /* if write didn't set errno, assume problem is no disk space */
    if (res != amount && errno == 0) {
        errno = ENOSPC;
    }

    if (res >= 0) {
        vfdcache[file].seekPos += res;
    } else {
        /* OK to retry if interrupted */
        if (errno == EINTR) {
            goto retry;
        }
        if (errno == EIO) {
            if (count < EIO_RETRY_TIMES) {
                ++count;
                ereport(WARNING, (errmsg("FilePWrite: %d (%s) " INT64_FORMAT " %d \
                    failed, then retry: Input/Output ERROR",
                        file, vfdcache[file].fileName, (int64)vfdcache[file].seekPos, amount)));
                goto retry;
            }
        }
        /* Trouble, so assume we don't know the file position anymore */
        vfdcache[file].seekPos = (off_t)-1;
    }

    return res;
}

void truncate_vector_file(Relation rel)
{
    RelationOpenSmgr(rel);
    if (!smgrexists(rel->rd_smgr, VECTOR_FORKNUM)) {
        return;
    }
    smgrtruncatefunc(rel->rd_smgr, VECTOR_FORKNUM, 0);
    XLogTruncateRelation(rel->rd_node, VECTOR_FORKNUM, 0);
}

template <bool allow_null = false>
static MdfdVec *vec_openseg(SMgrRelation reln, uint32 segno, int oflag)
{
    MdfdVec *v = _mdfd_openseg(reln, VECTOR_FORKNUM, segno, oflag);
    if (!allow_null && !v) {   /* failed to read/write seg */
        if (check_unlink_rel_hashtbl(reln->smgr_rnode.node, VECTOR_FORKNUM)) {
            ereport(DEBUG1, (errmsg("could not open file \"%s\": %m",
                mdsegpath(reln->smgr_rnode.node, VECTOR_FORKNUM, segno * RELSEG_SIZE))));
            return NULL;
        }
        int elevel =
            FILE_POSSIBLY_DELETED(errno) && (IsBgwriterProcess() || IsPagewriterProcess()) ?
            ERROR : PANIC;
        ereport(elevel, (errcode_for_file_access(),
            errmsg("could not open file \"%s\": %m",
                   mdsegpath(reln->smgr_rnode.node, VECTOR_FORKNUM, segno * RELSEG_SIZE))));
    }
    return v;
}

template <bool create_seg>
static MdfdVec *vec_getseg(SMgrRelation reln, off_t offset, VecStorageTypeInfo vec_storage_type_info)
{
    constexpr ExtensionBehavior behavior = create_seg ? EXTENSION_CREATE : EXTENSION_RETURN_NULL;
    MdfdVec *v = mdopen(reln, VECTOR_FORKNUM, behavior);
    if (!v) {
        return NULL;
    }
    const uint32 target_segno = (offset / max_file_size) * vec_storage_type_info.ntype + vec_storage_type_info.type;
    for (uint32 cur_segno = 1u; cur_segno <= target_segno; ++cur_segno) {
        Assert(cur_segno == v->mdfd_segno + 1u);
        if (!v->mdfd_chain) {
            v->mdfd_chain = vec_openseg<true>(reln, cur_segno, create_seg ? O_CREAT : 0);
        }
        v = v->mdfd_chain;
    }
    return v;
}

template <bool create_seg>
static MdfdVec *vec_nextseg(SMgrRelation reln, MdfdVec *v, VecStorageTypeInfo vec_storage_type_info)
{
    uint32 next_segno = v->mdfd_segno + 1u;
    const uint32 target_segno = v->mdfd_segno + vec_storage_type_info.ntype;
    for (; next_segno <= target_segno; ++next_segno) {
        Assert(next_segno == v->mdfd_segno + 1u);
        if (!v->mdfd_chain) {
            v->mdfd_chain = vec_openseg(reln, next_segno, create_seg ? O_CREAT : 0);
        }
        v = v->mdfd_chain;
    }
    return v;
}

void copy_vector_file(Relation rel, SMgrRelation *dstptr, char relpersistence)
{
    SMgrRelation dst = *dstptr;
    const bool use_wal = XLogIsNeeded() && (relpersistence == RELPERSISTENCE_PERMANENT ||
        ((relpersistence == RELPERSISTENCE_TEMP) && STMT_RETRY_ENABLED));
    const bool need_sync = !SmgrIsTemp(dst) && use_wal;

    RelationOpenSmgr(rel);
    SMgrRelation src = rel->rd_smgr;
    dst = *dstptr = smgropen(dst->smgr_rnode.node, dst->smgr_rnode.backend);

    MdfdVec *src_v = vec_getseg<false>(src, 0, get_vec_storage_type_info(VecStorageType::PureVec));
    if (!src_v) {
        return;
    }
    MdfdVec *des_v = vec_getseg<true>(dst, 0, get_vec_storage_type_info(VecStorageType::PureVec));
    LogManager logmgr(rel);
    constexpr size_t buf_size = BLCKSZ * 4ul;
    char *buffer = (char *)palloc(buf_size);
    uint32 cur_f = 0;
    for (;;) {
        off_t offset_true = FileSeek(src_v->mdfd_vfd, 0l, SEEK_END);
        for (off_t cur_offset = 0; cur_offset < offset_true; cur_offset += buf_size) {
            const int amount = std::min(offset_true - cur_offset, off_t(buf_size));
            int read_bytes = pread_file(src_v->mdfd_vfd, buffer, amount, cur_offset);
            if (unlikely(amount != read_bytes)) {
                if (unlikely(read_bytes <= 0)) {
                    report_read_error(cur_offset, FilePathName(src_v->mdfd_vfd), read_bytes);
                }
                check_file_stat(FilePathName(src_v->mdfd_vfd));
                force_backtrace_messages = true;
                extreme_rto_standby_read::dump_error_all_info(src->smgr_rnode.node, VECTOR_FORKNUM,
                                                              cur_offset / BLCKSZ);
                ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                                errmsg("could not read offset %ld in file \"%s\": read only %d of %d bytes",
                                       cur_offset, FilePathName(src_v->mdfd_vfd), read_bytes, amount)));
            }
            int write_bytes = pwrite_file(des_v->mdfd_vfd, buffer, amount, cur_offset);
            if (unlikely(write_bytes != amount)) {
                if (check_unlink_rel_hashtbl(src->smgr_rnode.node, VECTOR_FORKNUM)) {
                    ereport(ERROR, (errmsg("could not write offset %ld in file \"%s\": %m, "
                                           "this relation has been removed",
                                           cur_offset, FilePathName(des_v->mdfd_vfd))));
                } else {
                    report_write_error(cur_offset, FilePathName(des_v->mdfd_vfd), write_bytes, amount);
                }
            }
            logmgr.log_write_vector(off_t(cur_f) * RELSEG_SIZE * BLCKSZ + cur_offset,
                                    amount, buffer, true);
    
            if (need_sync) {
                register_dirty_segment(dst, VECTOR_FORKNUM, des_v);
            }
        }
        ++cur_f;
        src_v = vec_getseg<false>(src, off_t(cur_f) * RELSEG_SIZE * BLCKSZ,
                                  get_vec_storage_type_info(VecStorageType::PureVec));
        if (!src_v) {
            break;
        }
        des_v = vec_nextseg<true>(dst, des_v, get_vec_storage_type_info(VecStorageType::PureVec));
    }
    pfree(buffer);
    logmgr.destroy();
}

SMGR_READ_STATUS vec_read(SMgrRelation reln, off_t offset, int nbytes, char *buffer,
                          VecStorageType vec_storage_type)
{
    int read_bytes = 0;
    VecStorageTypeInfo vec_storage_type_info = get_vec_storage_type_info(vec_storage_type);
    MdfdVec *v = vec_getseg<false>(reln, offset, vec_storage_type_info);
    do {
        if (unlikely(!v)) {
            return SMGR_RD_NO_BLOCK;
        }
        const off_t cur_offset = offset % max_file_size;
        const int target_nbytes = std::min(nbytes - read_bytes, int(max_file_size - cur_offset));
        int res = pread_file(v->mdfd_vfd, buffer, target_nbytes, cur_offset);
        read_bytes += res;
        if (unlikely(res != target_nbytes)) {
            if (unlikely(res <= 0)) {
                errstart(ERROR, __FILE__, __LINE__, PG_FUNCNAME_MACRO, TEXTDOMAIN);
                errcode_for_file_access();
                if (errno > 0) {
                    errmsg("could not read offset %ld in file \"%s\": %m",
                           offset, FilePathName(v->mdfd_vfd));
                } else if (res == 0) {
                    errmsg("could not read offset %ld in file \"%s\": read zero byte",
                            offset, FilePathName(v->mdfd_vfd));
                } else {
                    errmsg("could not read offset %ld in file \"%s\": unknown error",
                           offset, FilePathName(v->mdfd_vfd));
                }
                read_bytes = -1;
                break;
            }
            if (!t_thrd.xlog_cxt.InRecovery) {
                check_file_stat(FilePathName(v->mdfd_vfd));
                force_backtrace_messages = true;
                extreme_rto_standby_read::dump_error_all_info(reln->smgr_rnode.node, VECTOR_FORKNUM, offset / BLCKSZ);
                errstart(ERROR, __FILE__, __LINE__, PG_FUNCNAME_MACRO, TEXTDOMAIN);
                errcode(ERRCODE_DATA_CORRUPTED);
                errmsg("could not read offset %ld in file \"%s\": read only %d of %d bytes",
                       offset, FilePathName(v->mdfd_vfd), res, nbytes);
                read_bytes = -1;
            }
            break;
        }
        if (likely(read_bytes >= nbytes)) {
            break;
        }
        offset += res;
        buffer += res;
        v = vec_nextseg<false>(reln, v, vec_storage_type_info);
    } while (unlikely(read_bytes < nbytes));
    return read_bytes >= 0 && verify_file_content(buffer) ? SMGR_RD_OK : SMGR_RD_CRC_ERROR;
}

void vec_write(SMgrRelation reln, off_t offset, int nbytes, const char *buffer, bool skip_fsync, VecStorageType vec_storage_type)
{
    int write_bytes = 0;
    VecStorageTypeInfo vec_storage_type_info = get_vec_storage_type_info(vec_storage_type);
    MdfdVec *v = vec_getseg<true>(reln, offset, vec_storage_type_info);
    do {
        if (unlikely(!v)) {
            ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not write offset %ld in file \"%s\": %m",
                        offset, FilePathName(v->mdfd_vfd))));
        }
        const off_t cur_offset = offset % max_file_size;
        const int target_nbytes = std::min(nbytes - write_bytes, int(max_file_size - cur_offset));
        int res = pwrite_file(v->mdfd_vfd, buffer, target_nbytes, cur_offset);
        write_bytes += res;
        offset += res;
        buffer += res;
        if (unlikely(res != target_nbytes)) {
            if (check_unlink_rel_hashtbl(reln->smgr_rnode.node, VECTOR_FORKNUM)) {
                ereport(DEBUG1,
                    (errmsg("could not write offset %ld in file \"%s\": %m, this relation has been removed",
                            offset, FilePathName(v->mdfd_vfd))));
                /* this file need skip sync */
                skip_fsync = true;
            } else {
                report_write_error(offset, FilePathName(v->mdfd_vfd), res, nbytes);
            }
        }

        if (!skip_fsync && !SmgrIsTemp(reln)) {
            register_dirty_segment(reln, VECTOR_FORKNUM, v);
        }
        v = vec_nextseg<true>(reln, v, vec_storage_type_info);
    } while (unlikely(write_bytes < nbytes));
}

SMGR_READ_STATUS read_vector_no_error(Relation rel, size_t loc, size_t elem_size, char *vec, bool with_code)
{
    return with_code ?
        read_vector_no_error(rel, loc, elem_size, vec, VecStorageType::VecWithCode) :
        read_vector_no_error(rel, loc, elem_size, vec, VecStorageType::PureVec);
}

void read_vector(Relation rel, size_t loc, size_t vec_size, char *vec, bool with_code)
{
    SMGR_READ_STATUS status = with_code ?
        read_vector_no_error(rel, loc, vec_size, vec, VecStorageType::VecWithCode) :
        read_vector_no_error(rel, loc, vec_size, vec, VecStorageType::PureVec);
    if (unlikely(status != SMGR_RD_OK)) {
        report_read_vector_error(status, rel, loc);
    }
}

void write_vector(Relation rel, size_t loc, size_t vec_size, const char *vec, bool with_code)
{
    off_t offset = loc * vec_size;
    with_code ?
        vec_write(rel->rd_smgr, offset, vec_size, vec, false, VecStorageType::VecWithCode) :
        vec_write(rel->rd_smgr, offset, vec_size, vec, false, VecStorageType::PureVec);
}

SMGR_READ_STATUS read_qtcode_no_error(Relation rel, size_t loc, size_t code_len, char *code, bool with_vec)
{
    off_t offset = loc * code_len;
    return with_vec ?
        vec_read(rel->rd_smgr, offset, code_len, code, VecStorageType::CodeWithVec) :
        vec_read(rel->rd_smgr, offset, code_len, code, VecStorageType::PureCode);
}

void read_qtcode(Relation rel, size_t loc, size_t code_len, char *code, bool with_vec)
{
    off_t offset = loc * code_len;
    SMGR_READ_STATUS status = with_vec ?
        vec_read(rel->rd_smgr, offset, code_len, code, VecStorageType::CodeWithVec) :
        vec_read(rel->rd_smgr, offset, code_len, code, VecStorageType::PureCode);
    if (unlikely(status != SMGR_RD_OK)) {
        report_read_vector_error(status, rel, loc);
    }
}

void write_qtcode(Relation rel, size_t loc, size_t code_len, const char *code, bool with_vec)
{
    off_t offset = loc * code_len;
    with_vec ?
        vec_write(rel->rd_smgr, offset, code_len, code, false, VecStorageType::CodeWithVec) :
        vec_write(rel->rd_smgr, offset, code_len, code, false, VecStorageType::PureCode);
}

static void vecwriter_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    t_thrd.vector_cxt.got_SIGHUP = true;
    if (t_thrd.proc) {
        SetLatch(&t_thrd.proc->procLatch);
    }
    errno = save_errno;
}

static void vecwriter_request_shutdown_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    t_thrd.vector_cxt.shutdown_requested = true;
    if (t_thrd.proc) {
        SetLatch(&t_thrd.proc->procLatch);
    }
    errno = save_errno;
}

static void vecwriter_sigusr1_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    latch_sigusr1_handler();
    errno = save_errno;
}

static void set_up_sig_handler()
{
    (void)gspqsignal(SIGHUP, vecwriter_sighup_handler);
    (void)gspqsignal(SIGURG, print_stack);
    (void)gspqsignal(SIGINT, SIG_IGN);
    (void)gspqsignal(SIGTERM, vecwriter_request_shutdown_handler);
    (void)gspqsignal(SIGQUIT, quickdie);
    (void)gspqsignal(SIGALRM, handle_sig_alarm);
    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR1, vecwriter_sigusr1_handler);
    (void)gspqsignal(SIGUSR2, SIG_IGN);
    (void)gspqsignal(SIGFPE, SIG_DFL);
    (void)gspqsignal(SIGCHLD, SIG_DFL);
    (void)gspqsignal(SIGTTIN, SIG_DFL);
    (void)gspqsignal(SIGTTOU, SIG_DFL);
    (void)gspqsignal(SIGCONT, SIG_DFL);
    (void)gspqsignal(SIGWINCH, SIG_DFL);
}

void vec_writer_main(void)
{
    t_thrd.proc_cxt.MyProcPid = gs_thread_self();
    t_thrd.proc_cxt.MyStartTime = time(NULL);
    t_thrd.proc_cxt.MyProgName = "Vector Buffer Manager";
    u_sess->attr.attr_common.application_name = pstrdup("Vector Buffer Manager");
    init_ps_display("vector buffer process", "", "", "");
    SetProcessingMode(InitProcessing);
    BaseInit();
    SetProcessingMode(NormalProcessing);
    sigjmp_buf localSigjmpBuf;

    set_up_sig_handler();
    (void)sigdelset(&t_thrd.libpq_cxt.BlockSig, SIGQUIT);

    ereport(LOG, (errmsg("VecBuffer thread started")));

    if (sigsetjmp(localSigjmpBuf, 1) != 0) {
        t_thrd.log_cxt.error_context_stack = NULL;
        t_thrd.log_cxt.call_stack = NULL;
        /* Prevent interrupts while cleaning up */
        HOLD_INTERRUPTS();
        /* Report the error to the server log */
        EmitErrorReport();
        LWLockReleaseAll();
        if (t_thrd.utils_cxt.CurrentResourceOwner) {
            ResourceOwnerRelease(t_thrd.utils_cxt.CurrentResourceOwner, RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
        }
        FlushErrorState();
        /* Now we can allow interrupts again */
        RESUME_INTERRUPTS();
        /*
         * Sleep at least 1 second after any error.  A write error is likely
         * to be repeated, and we don't want to be filling the error logs as
         * fast as we can.
         */
        pg_usleep(1000000L);
    }
    t_thrd.log_cxt.PG_exception_stack = &localSigjmpBuf;

    /*
     * Unblock signals (they were blocked when the postmaster forked us)
     */
    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
    (void)gs_signal_unblock_sigusr2();

    int id = -1;
    while (id < 0) {
        pg_memory_barrier();
        ThreadId tid = t_thrd.proc_cxt.MyProcPid;
        for (int i = 0; i < g_instance.diskann_cxt.vec_writer_nproc; ++i) {
            if (g_instance.pid_cxt.VecWriterPID[i] == tid) {
                id = i;
                break;
            }
        }
        SPIN_DELAY();
    }
    g_instance.diskann_cxt.vec_writer_latch[id] = &t_thrd.proc->procLatch;

    const auto record_evict = [](VecBufferPool &pool, size_t nevicted) -> void {
        long sec;
        int usec;
        TimestampTz cur_time = GetCurrentTimestamp();
        TimestampDifference(pool.stats.first_evict_time, cur_time, &sec, &usec);
        if (sec > eviction_time_interval) {
            pool.stats.first_evict_time = cur_time;
            pool.stats.nevict = 0;
        } else {
            pool.stats.nevict.fetch_add(nevicted, RELAXED_ORDER);
        }
    };

    for (;;) {
        if (t_thrd.vector_cxt.shutdown_requested) {
            break;
        }
        if (t_thrd.vector_cxt.got_SIGHUP) {
            t_thrd.vector_cxt.got_SIGHUP = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        int16 pool_offset = g_instance.diskann_cxt.pool_offset_to_write;
        if (pool_offset < 0) {
            constexpr long timeout = 1000;
            ResetLatch(&t_thrd.proc->procLatch);
            uint32 rc = WaitLatch(&t_thrd.proc->procLatch,
                WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, timeout);
            if (rc & WL_POSTMASTER_DEATH) {
                gs_thread_exit(1);
            }
            if (rc & WL_LATCH_SET) {
                continue;
            }
            /* check all pool */
            for (int16 i = 0; i < NVecPool; ++i) {
                if (!VecBufMgr->pool[i]) {
                    continue;
                }
                auto &cur_pool = *VecBufMgr->pool[i];
                if (cur_pool.stats.nblock == 0) {
                    continue;
                }
                if (VecBufMgr->nalloced >= NVecBuf && cur_pool.need_evict()) {
                    pool_offset = i;
                    break;
                }
            }
            if (pool_offset < 0) {
                continue;
            }
        } else {
            g_instance.diskann_cxt.pool_offset_to_write = -1;
        }

        bool first_evict = true;
        size_t nevic = 0u;
        bool evicted;
        constexpr int max_loop = 1000;
        int loop_count = max_loop;
        for (; loop_count > 0; --loop_count) {
            if (t_thrd.vector_cxt.shutdown_requested) {
                break;
            }
            VecBufMgr->expand_or_recollect_space(pool_offset, evicted);
            if (!VecBufMgr->pool[pool_offset] || !evicted) {
                break;
            }
            auto &cur_pool = *VecBufMgr->pool[pool_offset];
            if (first_evict) {
                first_evict = false;
                if (cur_pool.stats.first_evict_time == 0) {
                    cur_pool.stats.first_evict_time = GetCurrentTimestamp();
                }
            }
            ++nevic;
            constexpr size_t step_size = 20ul;
            if (nevic > step_size) {
                record_evict(cur_pool, nevic);
                nevic = 0;
            }
            if (!cur_pool.need_evict()) {
                break;
            }
        }
        if (nevic > 0) {
            if (VecBufMgr->pool[pool_offset]) {
                record_evict(*VecBufMgr->pool[pool_offset], nevic);
            }
        }
        if (loop_count <= 0) {
            g_instance.diskann_cxt.pool_offset_to_write = pool_offset;
        }
        VecBufMgr->try_redistribute_block();
    }
    g_instance.diskann_cxt.vec_writer_latch[id] = NULL;
    ereport(LOG, (errmsg("VecBuffer thread is shutting down.")));
}

static Vector<VectorBufferInspect> get_inspect()
{
    Vector<VectorBufferInspect> result;
    if (!VecBufMgr || !VecBufMgr->buffer_inited) {
        return result;
    }
    for (int16 pool_offset = 0; pool_offset < NVecPool; ++pool_offset) {
        if (!VecBufMgr->pool[pool_offset]) {
            continue;
        }
        VecBufferPool &cur_pool = *VecBufMgr->pool[pool_offset];
#ifdef BOOST_UNORDERED_ENABLE_STATS
        auto stats = cur_pool.locmap->get_stats();
        ereport(NOTICE, (errmsg("locmap stats:\n"
            "\tinsertion: count %lu, probe length %lf;\n"
            "\tsuccessful_lookup: count %lu, probe length %lf, num_comparisons %lf;\n"
            "\tunsuccessful_lookup: count %lu, probe length %lf, num_comparisons %lf",
            stats.insertion.count, stats.insertion.probe_length.average,
            stats.successful_lookup.count, stats.successful_lookup.probe_length.average,
            stats.successful_lookup.num_comparisons.average,
            stats.unsuccessful_lookup.count, stats.unsuccessful_lookup.probe_length.average,
            stats.unsuccessful_lookup.num_comparisons.average)));
        cur_pool.locmap->reset_stats();
# endif /* BOOST_UNORDERED_ENABLE_STATS */
        VectorBufferInspect inspect_info;
    
        int16 start_dim = pool_offset * vector_step_size;
        int16 end_dim = (pool_offset + 1) * vector_step_size;
        if (end_dim > DISKANN_MAX_DIM) {
            end_dim = DISKANN_MAX_DIM;
        }
        size_t min_size = start_dim * sizeof(float);
        size_t max_size = end_dim * sizeof(float) - 1;
        auto sf_min_size = ann_helper::format_size(min_size);
        auto sf_max_size = ann_helper::format_size(max_size);

        inspect_info.elem_size = psprintf("%.0f %s ~ %.0f %s",
            sf_min_size.n, sf_min_size.unit_str(), sf_max_size.n, sf_max_size.unit_str());
        
        size_t used_space = cur_pool.stats.nblock * vec_block_size;
        auto sf_space = ann_helper::format_size(used_space);
        inspect_info.used_space = psprintf("%.2f %s", sf_space.n, sf_space.unit_str());
        inspect_info.elem_nums = cur_pool.stats.ndata;
        inspect_info.hit = cur_pool.hit;
        inspect_info.miss = cur_pool.miss;
        inspect_info.evict = cur_pool.stats.nevict.load();
        result.push_back(inspect_info);
    }
    return result;
}

Datum vectorbuffer_inspect(PG_FUNCTION_ARGS)
{
    /* used_space, elem_size, elem_nums, hit, miss, evict */
    FuncCallContext *funcctx;
    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        Vector<VectorBufferInspect> inspect_results = get_inspect();
        TupleDesc tupdesc = CreateTemplateTupleDesc(6, false);
        TupleDescInitEntry(tupdesc, (AttrNumber)1, "used_space", CSTRINGOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)2, "elem_size", CSTRINGOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)3, "elem_nums", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)4, "hit", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)5, "miss", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)6, "evict", INT8OID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->max_calls = inspect_results.size();
        funcctx->user_fctx = inspect_results.data();
        MemoryContextSwitchTo(oldcontext);
    }
    funcctx = SRF_PERCALL_SETUP();
    if (funcctx->user_fctx && funcctx->call_cntr < funcctx->max_calls) {
        VectorBufferInspect *inspect = (VectorBufferInspect *)funcctx->user_fctx + funcctx->call_cntr;
        Datum values[6];
        bool nulls[6] = {false};
        values[0] = CStringGetDatum(inspect->used_space);
        values[1] = CStringGetDatum(inspect->elem_size);
        values[2] = Int64GetDatum(inspect->elem_nums);
        values[3] = Int64GetDatum(inspect->hit);
        values[4] = Int64GetDatum(inspect->miss);
        values[5] = Int64GetDatum(inspect->evict);
        
        HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        if (!tuple) {
            SRF_RETURN_DONE(funcctx);
        }
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
    SRF_RETURN_DONE(funcctx);
}
