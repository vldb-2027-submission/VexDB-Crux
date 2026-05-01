/**
 * Copyright ...
 */

#ifndef CONTAINER_DISK_VECTOR_H
#define CONTAINER_DISK_VECTOR_H

#include <type_traits>  /* is_pod */
#include <algorithm>    /* min, fill_n */
#include <atomic>

#include <vtl/internal/container.hpp>
#include <vtl/disk_container/blockmgr.hpp>
#include <vtl/internal/type.hpp>
#include <vtl/internal/expr.hpp>
#include <vtl/disk_container/macro.hpp>

#include "access/annvector/xlog/log_manager.h"
#include "utils/relcache.h"
#include "utils/palloc.h"
#include "access/heapam.h"

namespace disk_container {
constexpr size_t DEFAULT_START_IDX = 8ul;
constexpr int DEFAULT_START_CLZ_IDX = __builtin_clzl(DEFAULT_START_IDX) + 1;
constexpr uint32 DISK_VECTOR_VERSION_ONE = 1u;
constexpr uint32 DISK_VECTOR_VERSION_TWO = 2u;

struct DiskVectorOpaqueData {
    uint16 type_id;
    uint16 page_id;
};
typedef DiskVectorOpaqueData *DiskVectorOpaque;

struct DiskVectorMetaPageData {
    uint32 magic;
    uint32 version;
    BlockNumber lock_page_for_base;  /* reserved */
    BlockNumber lock_page_for_nitem;  /* reserved */
    size_t nitem;
    uint32 npage;
    BlockNumber item_start_pages[FLEXIBLE_ARRAY_MEMBER];

    void init(BlockNumber start_blkno)
    {
        magic = DISK_VECTOR_META_MAGIC;
        version = DISK_VECTOR_VERSION_ONE;
        nitem = 0;
        npage = 1u;
        item_start_pages[0] = start_blkno;
    }
};
typedef DiskVectorMetaPageData *DiskVectorMetaPage;

namespace vtl {
template <typename T>
struct DiskVectorDataPageData {
    uint32 magic;
    uint32 version;
    T data[FLEXIBLE_ARRAY_MEMBER];

    /* 100% there are bugs here, but it may change anyway so I left it here */
    constexpr static size_t AvailableSize =
        PAGE_SIZE - offsetof(DiskVectorDataPageData, data) - MAXALIGN(sizeof(DiskVectorOpaqueData));

    void init() { magic = DISK_VECTOR_DATA_MAGIC; version = DISK_VECTOR_VERSION_ONE; }

    const T *get(uint32 idx) const { return data + idx; }
    T *get(uint32 idx) { return data + idx; }
};

template <typename T>
struct VarDiskVectorDataPageData {
    uint32 magic;
    uint32 version;
    void *data;

    /* 100% there are bugs here, but it may change anyway so I left it here */
    constexpr static size_t AvailableSize =
        PAGE_SIZE - offsetof(VarDiskVectorDataPageData, data) - MAXALIGN(sizeof(DiskVectorOpaqueData));

    void init() { magic = DISK_VECTOR_DATA_MAGIC; version = DISK_VECTOR_VERSION_TWO; }

    const T *get(uint32 idx, size_t it) const { return (const T *)(data + idx * it); }
    T *get(uint32 idx, size_t it) { return (T *)(data + idx * it); }
};

template <typename T>
struct FixedParam {
    using dpd = DiskVectorDataPageData<T>;
    using dp = DiskVectorDataPageData<T> *;

    explicit FixedParam(size_t) {}

    static constexpr bool is_var_length = false;
    static constexpr size_t data_size() { return sizeof(T); }
    static constexpr size_t n_data_per_block() { return dpd::AvailableSize / sizeof(T); }

    static constexpr size_t _get_offset(size_t i)
        { return MAXALIGN(SizeOfPageHeaderData) + offsetof(dpd, data) + i * data_size(); }
    
    template <typename P>
    static auto _get(P &&p, uint32 idx) -> decltype(std::forward<P>(p)->get(idx))
        { return std::forward<P>(p)->get(idx); }

    static const T *_offset_by(const T *t, size_t i) { return t + i; }
    static T *_offset_by(T *t, size_t i) { return t + i; }
};

template <typename T>
struct VarParam {
    using dpd = VarDiskVectorDataPageData<T>;
    using dp = VarDiskVectorDataPageData<T> *;

    explicit VarParam(size_t s) : vl(s) {}

    size_t vl;
    constexpr size_t _get_offset(size_t i) const
        { return MAXALIGN(SizeOfPageHeaderData) + offsetof(dpd, data) + i * vl; }
    constexpr size_t n_data_per_block() const { return dpd::AvailableSize / vl; }
    constexpr size_t data_size() { return vl; }
    static constexpr bool is_var_length = true;

    template <typename P>
    auto _get(P &&p, uint32 idx) -> decltype(std::forward<P>(p)->get(idx, vl))
        { return std::forward<P>(p)->get(idx, vl); }

    const T *_offset_by(const T *t, size_t i) { return (const T *)(((const void *)t) + i * vl); }
    T *_offset_by(T *t, size_t i) { return (T *)(((void *)t) + i * vl); }
    
};

/**
 * Persistent vector, thread safe for get/set, but in actual use the content must have version to achieve parallelism
 */
template <typename T, typename Param>
class DiskVector : public Param {
    using dp = typename Param::dp;
    using dpd = typename Param::dpd;
    static_assert(sizeof(T) <= dpd::AvailableSize, "data type too large for disk vector");

public:
    Relation _rel;
    DiskVectorMetaPage _meta;

    DiskVector(Relation rel, BlockNumber meta_blkno, bool is_wal, size_t elem_size = sizeof(T))
        : Param(elem_size),
          _rel(rel),
          _blkmgr(rel),
          _logmgr(_rel),
          _need_wal(is_wal && RelationNeedsWAL(rel))
    {
        static_assert(std::is_standard_layout<T>::value, "Diskvector only applies to POD types");
        get_meta_page(meta_blkno);
        _lock_pd = _blkmgr.get_page_data(_meta->lock_page_for_base);
        _lock_nitem_pd = _blkmgr.get_page_data(_meta->lock_page_for_nitem);
    }

    void destroy()
    {
        _blkmgr.lock_page_data_shared(_meta_pd);
        _blkmgr.unlock_release_page(_meta_pd);
        _blkmgr.release_page(_lock_pd);
        _blkmgr.release_page(_lock_nitem_pd);
        _blkmgr.destroy();
    }
    static BlockNumber get_disk_vector(Relation rel, bool isWal = true)
    {
        char data_buf[PAGE_SIZE] = {0};
        DiskVectorOpaqueData opaque_data = {ann_helper::GET_TYPE_ID(T), DISK_VECTOR_DATA_ID};
        BlockMgr blkmgr(rel);
        LogManager logmgr(rel);
        reinterpret_cast<dp>(data_buf)->init();
        BlockNumber blkno = blkmgr.reserve_new_pages(DEFAULT_START_IDX, data_buf, &opaque_data, sizeof(opaque_data));
        if (isWal) {
            logmgr.diskann_extend_newpages(blkno, blkno + DEFAULT_START_IDX); 
        }
        auto meta = reinterpret_cast<DiskVectorMetaPage>(data_buf);
        meta->init(blkno);
        opaque_data.page_id = DISK_VECTOR_DUMMY_ID;
        blkno = blkmgr.reserve_new_pages(2ul, NULL, &opaque_data, sizeof(opaque_data));
        if (isWal) {
            logmgr.diskann_extend_newpages(blkno, blkno + 2u);
        }
        meta->lock_page_for_base = blkno;
        meta->lock_page_for_nitem = blkno + 1u;
        opaque_data.page_id = DISK_VECTOR_META_ID;
        blkno = blkmgr.reserve_new_pages(1ul, (char *)meta, &opaque_data, sizeof(opaque_data));
        if (isWal) {
            logmgr.diskann_extend_newpages(blkno, blkno + 1u);
        }
        blkmgr.destroy();
        logmgr.destroy();
        return blkno;
    }

    template <AccessorLockType lock_type> T get(size_t idx) const
    {
        PageData pd;
        uint32 offset;
        navigate_page_offset(idx, pd, offset);
        _blkmgr.template lock_page_data_custom<lock_type>(pd);
        T res = *this->_get(reinterpret_cast<const dp>(pd.page), offset);
        _blkmgr.template unlock_page_data_custom<lock_type>(pd);
        _blkmgr.release_page(pd);
        return res;
    }
    template <AccessorLockType lock_type> void get_n(size_t idx, size_t n, T *dest) const
    {
        Assert(dest && n > 0);
        PageData pd;
        uint32 offset;
        uint32 read_amount;
        for (size_t i = 0; i < n; i += read_amount) {
            CHECK_FOR_INTERRUPTS();
            navigate_page_offset(idx + i, pd, offset);
            read_amount = std::min(n - i, this->n_data_per_block() - offset);
            Assert(read_amount > 0);
            _blkmgr.template lock_page_data_custom<lock_type>(pd);
            memcpy(this->_offset_by(dest, i), this->_get(reinterpret_cast<dp>(pd.page), offset),
                   read_amount * this->data_size());
            _blkmgr.template unlock_page_data_custom<lock_type>(pd);
            _blkmgr.release_page(pd);
        }
    }

    void rlock() { _blkmgr.lock_page_data_shared(_lock_pd); }
    void runlock() { _blkmgr.unlock_page_data(_lock_pd); }
    void wlock() { _blkmgr.lock_page_data_exclusive(_lock_pd); }
    void wunlock() { _blkmgr.unlock_page_data(_lock_pd); }

    template <AccessorLockType lock_type> void set(size_t idx, const T &elem)
    {
        PageData pd;
        uint32 offset;
        navigate_page_offset(idx, pd, offset);
        _blkmgr.template lock_page_data_custom<lock_type>(pd);
        *this->_get(reinterpret_cast<dp>(pd.page), offset) = elem;
        pd.mark_dirty();
        if (_need_wal) {
            _logmgr.diskann_xlog_add_elem(pd, (char *)&elem, this->data_size(),
                                          this->_get_offset(offset));
        }
        _blkmgr.template unlock_page_data_custom<lock_type>(pd);
        _blkmgr.release_page(pd);
    }
    template <AccessorLockType lock_type> void set_n(size_t idx, size_t n, const T *elem)
    {
        Assert(elem || n == 0);
        PageData pd;
        uint32 offset;
        uint32 write_amount;
        for (size_t i = 0; i < n; i += write_amount) {
            CHECK_FOR_INTERRUPTS();
            navigate_page_offset(idx + i, pd, offset);
            write_amount = std::min(n - i, this->n_data_per_block() - offset);
            Assert(write_amount > 0);
            _blkmgr.template lock_page_data_custom<lock_type>(pd);
            errno_t rc = memcpy_s(this->_get(reinterpret_cast<dp>(pd.page), offset),
                write_amount * this->data_size(), this->_offset_by(elem, i),
                write_amount * this->data_size());
            securec_check(rc, "\0", "\0");
            pd.mark_dirty();
            if (_need_wal) {
                _logmgr.diskann_xlog_add_elem(pd, (char*)this->_offset_by(elem, offset),
                    write_amount * this->data_size(), this->_get_offset(offset));
            }
            _blkmgr.template unlock_page_data_custom<lock_type>(pd);
            _blkmgr.release_page(pd);
        }
    }
    template <AccessorLockType lock_type> void set_n(size_t idx, size_t n, const T &elem)
    {
        static_assert(!Param::is_var_length, "This function can only be called by DiskVector");
        PageData pd;
        uint32 offset;
        uint32 write_amount;
        for (size_t i = 0; i < n; i += write_amount) {
            CHECK_FOR_INTERRUPTS();
            navigate_page_offset(idx + i, pd, offset);
            write_amount = std::min(n - i, this->n_data_per_block() - offset);
            Assert(write_amount > 0);
            _blkmgr.template lock_page_data_custom<lock_type>(pd);
            T *dest = this->_get(reinterpret_cast<dp>(pd.page), offset);
            std::fill_n(dest, write_amount, elem);
            pd.mark_dirty();
            if (_need_wal) {
                _logmgr.diskann_xlog_add_elem(pd, (char *)dest,
                    write_amount * this->data_size(), this->_get_offset(offset));
            }
            _blkmgr.template unlock_page_data_custom<lock_type>(pd);
            _blkmgr.release_page(pd);
        }
    }
    template <AccessorLockType lock_type, class F> struct Applier {
        static constexpr bool reference_input = IS_INVOCABLE_R(F, bool, T &);
        Applier(DiskVector &vector, F &&func) : _vector(vector), _apply_func(std::forward<F>(func))
        {
            static_assert(IS_INVOCABLE_R(F, bool, T &) || IS_INVOCABLE_R(F, bool, T *),
                          "F must be invocable with T & and return bool");
        }
        bool operator()(size_t idx)
        {
            PageData pd;
            uint32 offset;
            _vector.navigate_page_offset(idx, pd, offset);
            _vector._blkmgr.template lock_page_data_custom<lock_type>(pd);
            auto data = reinterpret_cast<dp>(pd.page);
            bool res;
            CONSTEXPR_IF (reference_input) {
                res = _apply_func(*_vector._get(data, offset));
            } else {
                res = _apply_func(_vector._get(data, offset));
            }
            if (res) {
                pd.mark_dirty();
                if (_vector._need_wal) {
                    _vector._logmgr.diskann_xlog_add_elem(pd, (char *)_vector._get(data, offset),
                        _vector.data_size(), _vector._get_offset(offset));
                }
            }
            _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
            _vector._blkmgr.release_page(pd);
            return res;
        }

        friend class DiskVector;
    private:
        DiskVector &_vector;
        F _apply_func;
    };
    template <AccessorLockType lock_type, class F> Applier<lock_type, F> apply(F &&func)
        { return Applier<lock_type, F>(*this, std::forward<F>(func)); }
    template <AccessorLockType lock_type, class F> struct Visitor {
        Visitor(DiskVector &vector, F &&func) : _vector(vector), _visit_func(std::forward<F>(func))
        {
            static_assert(IS_INVOCABLE_R(F, void, const T *, size_t),
                          "F must be invocable with (const T *, size_t) and return void");
        }
        void operator()(size_t idx, size_t n)
        {
            PageData pd;
            uint32 offset;
            uint32 read_amount;
            for (size_t i = 0; i < n; i += read_amount) {
                CHECK_FOR_INTERRUPTS();
                _vector.navigate_page_offset(idx + i, pd, offset);
                read_amount = std::min(n - i, _vector.n_data_per_block() - offset);
                Assert(read_amount > 0);
                _vector._blkmgr.template lock_page_data_custom<lock_type>(pd);
                _visit_func(_vector._get(reinterpret_cast<const dp>(pd.page), offset), read_amount);
                _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                _vector._blkmgr.release_page(pd);
            }
        }
    private:
        DiskVector &_vector;
        F _visit_func;
    };
    template <AccessorLockType lock_type, class F> Visitor<lock_type, F> visit(F &&func)
        { return Visitor<lock_type, F>(*this, std::forward<F>(func)); }

    size_t push_back(const T &elem)
    {
        _blkmgr.lock_page_data_exclusive(_lock_nitem_pd);
        size_t cur_size = _meta->nitem; 
        ++_meta->nitem;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            _logmgr.diskann_update_meta_nitem(_meta_pd, cur_size + 1);
        }
        _blkmgr.unlock_page_data(_lock_nitem_pd);
        reserve(cur_size + 1);
        set<AccessorLockType::WriteLock>(cur_size, elem);
        return cur_size;
    }
    size_t push_back_n(const T &elem, size_t n)
    {
        Assert(n > 0);
        _blkmgr.lock_page_data_exclusive(_lock_nitem_pd);
        size_t cur_size = _meta->nitem;
        _meta->nitem += n;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            _logmgr.diskann_update_meta_nitem(_meta_pd, cur_size + n);
        }
        _blkmgr.unlock_page_data(_lock_nitem_pd);
        reserve(cur_size + n);
        set_n<AccessorLockType::WriteLock>(cur_size, n, elem);
        return cur_size;
    }
    size_t push_back_n(const T *elem, size_t n)
    {
        Assert(elem && n > 0);
        _blkmgr.lock_page_data_exclusive(_lock_nitem_pd);
        size_t cur_size = _meta->nitem;
        _meta->nitem += n;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            _logmgr.diskann_update_meta_nitem(_meta_pd, cur_size + n);
        }
        _blkmgr.unlock_page_data(_lock_nitem_pd);
        reserve(cur_size + n);
        set_n<AccessorLockType::WriteLock>(cur_size, n, elem);
        return cur_size;
    }
    template <AccessorLockType lock_type> bool pop_back(T &elem)
    {
        _blkmgr.lock_page_data_exclusive(_lock_nitem_pd);
        size_t cur_size = _meta->nitem;
        if (cur_size == 0) {
            _blkmgr.unlock_page_data(_lock_nitem_pd);
            return false;
        }
        _meta->nitem = cur_size - 1ul;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            _logmgr.diskann_update_meta_nitem(_meta_pd, cur_size - 1);
        }
        _blkmgr.unlock_page_data(_lock_nitem_pd);

        PageData pd;
        uint32 offset;
        navigate_page_offset(cur_size - 1, pd, offset);
        _blkmgr.template lock_page_data_custom<lock_type>(pd);
        elem = *this->_get(reinterpret_cast<const dp>(pd.page), offset);
        _blkmgr.template unlock_page_data_custom<lock_type>(pd);
        _blkmgr.release_page(pd);
        return true;
    }

    void reserve(size_t size)
    {
        BlockNumber blkno;
        uint32 offset;
        if (navigate_blkno_offset(size, blkno, offset)) {
            return;
        }
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        if (navigate_blkno_offset(size, blkno, offset)) {
            _blkmgr.unlock_page_data(_meta_pd);
            return;
        }

        char data_buf[PAGE_SIZE] = {0};
        reinterpret_cast<dp>(data_buf)->init();
        DiskVectorOpaqueData opaque_data = {ann_helper::GET_TYPE_ID(T), DISK_VECTOR_DATA_ID};
        do {
            /* extend vector */
            BlockNumber start_blkno = _blkmgr.reserve_new_pages(DEFAULT_START_IDX << _meta->npage,
                data_buf, &opaque_data, sizeof(opaque_data));
            if (_need_wal) {
                _logmgr.diskann_extend_newpages(start_blkno, start_blkno + (DEFAULT_START_IDX << _meta->npage));
            }
            _meta->item_start_pages[_meta->npage] = start_blkno;
            ++_meta->npage;
            if (_need_wal) {
                _meta_pd.mark_dirty();
                _logmgr.diskann_update_meta_start_npages(_meta_pd, _meta->npage, start_blkno);
            }
        } while (!navigate_blkno_offset(size, blkno, offset));
        _meta_pd.mark_dirty();
        _blkmgr.unlock_page_data(_meta_pd);
    }
    void extend(size_t size)
    {
        _blkmgr.lock_page_data_exclusive(_lock_nitem_pd);
        size_t nitem_size = _meta->nitem;
        if (size > nitem_size) {
            _meta->nitem = size;
            _meta_pd.mark_dirty();
            if (_need_wal) {
                _logmgr.diskann_update_meta_nitem(_meta_pd, size);
            }
        }
        _blkmgr.unlock_page_data(_lock_nitem_pd);
        if (size > nitem_size) {
            reserve(size);
        }
    }
    /* only set size, no space released */
    void shrink(size_t size) { _meta->nitem = size; }
    size_t size() const { return _meta->nitem; }
    BlockNumber get_nblocks() const
    {
        BlockNumber res = 1u;   /* meta page */
        for (uint32 i = 0; i < _meta->npage; ++i) {
            res += (DEFAULT_START_IDX << i);
        }
        return res;
    }
    size_t capacity() const { return size_t(get_nblocks()) * this->n_data_per_block(); }
    void prefetch(size_t start, size_t len) { /* no-op */ }

private:
    PageData _meta_pd;
    PageData _lock_pd;
    PageData _lock_nitem_pd;
    BlockMgr _blkmgr;
    LogManager _logmgr;
    bool _need_wal;

    void get_meta_page(BlockNumber meta_blkno)
    {
        _meta_pd = _blkmgr.get_page_data(meta_blkno);
        _meta = reinterpret_cast<DiskVectorMetaPage>(_meta_pd.page);
    }

    bool navigate_blkno_offset(size_t idx, BlockNumber &blkno, uint32 &offset) const
    {
        constexpr size_t SIZE_T_BYTE = sizeof(size_t) * CHAR_BIT;
        size_t page_idx = idx / this->n_data_per_block();
        /* __builtin_clzl UB when input is zero */
        int idx_clz =  idx < this->n_data_per_block() ? SIZE_T_BYTE : __builtin_clzl(page_idx);
        int target_page = DEFAULT_START_CLZ_IDX - idx_clz;
        uint32 target_page_group_idx = target_page > 0 ? uint32(target_page) : 0;
        if (unlikely(target_page_group_idx >= _meta->npage)) {
            return false;
        }
        /* no need to worry about idx_clz causing overflow, that's too large to happen */
        const size_t mask = ~(1ul << (SIZE_T_BYTE - 1 - idx_clz)) | 7ul;
        blkno = _meta->item_start_pages[target_page_group_idx] + (page_idx & mask);
        offset = idx % this->n_data_per_block();
        return true;
    }
    
    void navigate_page_offset(size_t idx, PageData &pd, uint32 &offset) const
    {
        BlockNumber blkno;
        if (navigate_blkno_offset(idx, blkno, offset)) {
            pd = _blkmgr.get_page_data(blkno);
        } else {
#if VERIFY_DATA
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("Diskvector invalid access idx: %lu", idx)));
#else
            __builtin_unreachable();
#endif /* VERIFY_DATA */
        }
    }
};
}

template <typename T>
using DiskVector = vtl::DiskVector<T, vtl::FixedParam<T>>;
template <typename T>
using VarDiskVector = vtl::DiskVector<T, vtl::VarParam<T>>;
} /* namespace disk_container */

#endif /* CONTAINER_DISK_VECTOR_H */
