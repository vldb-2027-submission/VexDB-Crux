/**
 * Copyright ...
 * Hybrid vector index interface.
 */

#ifndef VECTOR_BPTREE_H
#define VECTOR_BPTREE_H

#include <mutex>
#include <memory>
#include <functional>
#include <algorithm>
#include <vtl/hashtable>

#include "access/diskann/diskann.h"
#include "access/annvector/distance/distance.h"
#include "access/diskann/utils/parameters.h"
#include "access/annvector/module/leak_checker.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/diskann/storage_interface/storage_interface.h"
#include "storage/indexfsm.h"
#include "access/annvector/xlog/log_manager.h"
#include "access/hybridann/hybridann.h"

using IdxSet = UnorderedSet<size_t, impl::DefaultHasher<size_t>, std::equal_to<size_t>,
                            HUGE_ALLOCATOR<size_t>>;

class IndexMagnitude : public BaseObject {
public:
    IndexMagnitude(Relation rel, int64 graph_magnitude_entry_size)
        : _graph_magnitude_entry_size(graph_magnitude_entry_size) { init(rel); }
    IndexMagnitude(Vector<size_t> &index_magnitudes, int64 graph_magnitude_entry_size)
        : _graph_magnitude_entry_size(graph_magnitude_entry_size)  { init(index_magnitudes); }
    IndexMagnitude(size_t *index_magnitudes, size_t size, int64 graph_magnitude_entry_size)
        : _graph_magnitude_entry_size(graph_magnitude_entry_size) { init(index_magnitudes, size); }

    size_t size() { return _index_magnitude_size; }
    size_t get(size_t level) { return _index_magnitudes[level]; }
    size_t get_half(size_t level) { return _index_magnitudes_half[level]; }
    size_t split_threshold(size_t level) { return _index_split_thresholds[level]; }
    size_t graph_entry_level() { return _graph_magnitude_entry_level; }
    const Vector<size_t>& magnitudes() const { return _index_magnitudes; }
    Vector<size_t>& magnitudes() { return _index_magnitudes; }

    void destroy()
    {
        ann_helper::optional_destroy(_index_magnitudes);
        ann_helper::optional_destroy(_index_split_thresholds);
        ann_helper::optional_destroy(_index_magnitudes_half);
    }

private:
    size_t _index_magnitude_size = 0ul;
    Vector<size_t> _index_magnitudes;
    Vector<size_t> _index_split_thresholds;
    Vector<size_t> _index_magnitudes_half;
    size_t _graph_magnitude_entry_level = max_index_magnitude_size;
    int64 _graph_magnitude_entry_size = 20'000;

    void init(Relation rel) {
        ann_helper::StringParamExtractor spe(hybridAnnGetVecIndexMagnitudes(rel));
        spe.extract(_index_magnitudes);
        spe.destroy();
        populate();
    }

    void init(Vector<size_t> &index_magnitudes) {
        for (size_t magnitude : index_magnitudes) {
            _index_magnitudes.push_back(magnitude);
        }
        populate();
    }

    void init(size_t *index_magnitudes, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            _index_magnitudes.push_back(index_magnitudes[i]);
        }
        populate();
    }

    void populate() {
        if (_index_magnitudes.empty()) {
            for (size_t magnitude : default_index_magnitudes) {
                _index_magnitudes.push_back(magnitude);
            }
        } else {
            std::sort(_index_magnitudes.begin(), _index_magnitudes.end());
        }
        _index_magnitude_size = _index_magnitudes.size();
        for (size_t i = 0; i < _index_magnitude_size; ++i) {
            _index_split_thresholds.push_back(_index_magnitudes[i] * 2);
            _index_magnitudes_half.push_back(_index_magnitudes[i] / 2);
            Assert(_graph_magnitude_entry_size > 0);
            if (_index_magnitudes[i] >= (size_t)_graph_magnitude_entry_size &&
                _graph_magnitude_entry_level > i) {
                _graph_magnitude_entry_level = i;
            } 
        }
    }
};

typedef struct DiskAnnDataMetaPage {
    size_t         num_vectors;
} DiskAnnDataMetaPage;
typedef DiskAnnDataMetaPage *DiskAnnDataMeta;

typedef struct DiskAnnMetaPageV2 : public DiskAnnMetaPageBase {
    BlockNumber     BTMetaBlkNo;
    BlockNumber     dataMetaBlkNo;
    BlockNumber     graphMetaBlkNo[max_index_magnitude_size];
    size_t          sizeIndexMagnitudes;
    size_t          indexMagnitudes[max_index_magnitude_size];
} DiskAnnMetaPageV2;

enum VectorIndexType : uint8 { None, IVF, GRAPH, ANN };
inline const char *VectorIndexTypeToString(VectorIndexType type)
{
    switch (type) {
        case VectorIndexType::None:
            return "None";
        case VectorIndexType::IVF:
            return "IVF";
        case VectorIndexType::GRAPH:
            return "GRAPH";    
        case VectorIndexType::ANN:
            return "ANN";
        default:
            return "Invalid";
    }
}

typedef struct VectorIndexMetaPage {
    uint32          magic;
    VectorIndexType type;
    bool            idle;
    bool            deleted;
    bool            under_vacuum;
    uint32          dim;
    BlockNumber     node_blkno;
    size_t          index_magnitude_level;
    BlockNumber     index_meta_blkno;
    BlockNumber     leftmost_node_blkno;
    BlockNumber     next_idx_blkno;
    /* 0 means right map scan done or normal state
     * InvalidBlockNumber means right map scan not started
     * other normal blkno means in the process of scan */
    BlockNumber     split_scanned_blkno;

    void init()
    {
        magic = disk_container::VECTOR_INDEX_META_MAGIC;
        type = VectorIndexType::None;
        idle = true;
        deleted = false;
        under_vacuum = false;
        dim = 1u;
        node_blkno = InvalidBlockNumber;
        index_magnitude_level = 0u;
        index_meta_blkno = InvalidBlockNumber;
        leftmost_node_blkno = InvalidBlockNumber;
        next_idx_blkno = InvalidBlockNumber;
        split_scanned_blkno = 0;
    }
} VectorIndexMetaPage;
typedef VectorIndexMetaPage *VectorIndexMeta;

inline void allocate_buf_page(Relation rel, Buffer &buf, Page &page)
{
    LockRelationForExtension(rel, ExclusiveLock);
    buf = ReadBuffer(rel, P_NEW);
    UnlockRelationForExtension(rel, ExclusiveLock);
    page = BufferGetPage(buf);
    PageInit(page, BLCKSZ, 0); /* no opaque */
}

inline BlockNumber release_buf_page(Buffer buf, Page page)
{
    BlockNumber blkno = BufferGetBlockNumber(buf);
    PageSetChecksumInplace(page, blkno);
    MarkBufferDirty(buf);
    ReleaseBuffer(buf);
    return blkno;
}

struct VectorIndexNodePage {
    uint32          magic;
    uint32          nnode;
    BlockNumber     nodes[FLEXIBLE_ARRAY_MEMBER];

    void init();
    Buffer insert(Relation rel, BlockNumber blkno, Buffer buf, bool check_exists = true);
    Buffer remove(Relation rel, BlockNumber blkno);
    bool empty(Relation rel);
    BlockNumber *get_node_blkno(Relation rel, size_t &nblkno);
};
namespace nodepage_internal {
static constexpr uint32 max_nnode_per_page =
    (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - offsetof(VectorIndexNodePage, nodes)) /
    sizeof(BlockNumber) - 1u;
static constexpr size_t max_data_size =
    offsetof(VectorIndexNodePage, nodes) + sizeof(BlockNumber) * max_nnode_per_page;
} /* namespace nodepage_internal */

inline void VectorIndexNodePage::init()
{
    magic = disk_container::VECTOR_INDEX_NODE_MAGIC;
    nnode = 0;
    nodes[nodepage_internal::max_nnode_per_page] = 0;
}
inline Buffer VectorIndexNodePage::insert(Relation rel, BlockNumber blkno, Buffer buf, bool check_exists)
{
    if (check_exists) {
        for (uint32 i = 0; i < nnode; ++i) {
            if (nodes[i] == blkno) {
                return InvalidBuffer;
            }
        }
    }
    if (nnode < nodepage_internal::max_nnode_per_page) {
        nodes[nnode++] = blkno;
        return INT_MAX;
    }

    const auto get_next_page = [&](BlockNumber &next_blkno) -> VectorIndexNodePage * {
        Page page;
        if (next_blkno == 0) {
            Buffer old_buf = buf;
            allocate_buf_page(rel, buf, page);
            VectorIndexNodePage *res = (VectorIndexNodePage *)PageGetContents(page);
            res->init();
            ((PageHeader)page)->pd_lower =
                ((char *)res + nodepage_internal::max_data_size) - (char *)page;
            XLogBeginInsert();
            START_CRIT_SECTION();
            MarkBufferDirty(buf);
            XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
            XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_ADD_ANN_META);
            PageSetLSN(BufferGetPage(buf), recptr);
            END_CRIT_SECTION();
            next_blkno = BufferGetBlockNumber(buf);
            struct { BlockNumber b; char unused; } temp = {next_blkno, '\0'};
            XLogBeginInsert();
            START_CRIT_SECTION();
            MarkBufferDirty(old_buf);
            XLogRegisterBuffer(0, old_buf, REGBUF_STANDARD);
            XLogRegisterBufData(0, (char *)&temp, sizeof(BlockNumber) + 1ul);
            recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_INSERT_NODE);
            PageSetLSN(BufferGetPage(old_buf), recptr);
            END_CRIT_SECTION();
        } else {
            buf = ReadBuffer(rel, next_blkno);
            page = BufferGetPage(buf);
        }
        return (VectorIndexNodePage *)PageGetContents(page);
    };
    for (VectorIndexNodePage *np = get_next_page(nodes[nodepage_internal::max_nnode_per_page]);;) {
        if (check_exists) {
            for (uint32 i = 0; i < np->nnode; ++i) {
                if (np->nodes[i] == blkno) {
                    ReleaseBuffer(buf);
                    return InvalidBlockNumber;
                }
            }
        }
        if (np->nnode < nodepage_internal::max_nnode_per_page) {
            np->nodes[np->nnode++] = blkno;
            break;
        }
        Buffer old_buf = buf;
        np = get_next_page(np->nodes[nodepage_internal::max_nnode_per_page]);
        ReleaseBuffer(old_buf);
    }
    return buf;
}
inline Buffer VectorIndexNodePage::remove(Relation rel, BlockNumber blkno)
{
    for (uint32 i = 0; i < nnode; ++i) {
        if (nodes[i] == blkno) {
            nodes[i] = nodes[--nnode];
            return INT_MAX;
        }
    }

    Buffer buf;
    const auto get_next_page = [&](BlockNumber next_blkno) -> VectorIndexNodePage * {
        Page page;
        if (next_blkno == 0) {
            return NULL;
        } else {
            buf = ReadBuffer(rel, next_blkno);
            page = BufferGetPage(buf);
        }
        return (VectorIndexNodePage *)PageGetContents(page);
    };
    for (VectorIndexNodePage *np = get_next_page(nodes[nodepage_internal::max_nnode_per_page]); np != NULL;) {
        for (uint32 i = 0; i < np->nnode; ++i) {
            if (np->nodes[i] == blkno) {
                np->nodes[i] = np->nodes[--np->nnode];
                return buf;
            }
        }
        Buffer old_buf = buf;
        np = get_next_page(np->nodes[nodepage_internal::max_nnode_per_page]);
        ReleaseBuffer(old_buf);
    }
    return InvalidBuffer;
}
inline bool VectorIndexNodePage::empty(Relation rel)
{
    if (nnode != 0) {
        return false;
    }
    if (nodes[nodepage_internal::max_nnode_per_page] != 0) {
        BlockNumber next_blkno = nodes[nodepage_internal::max_nnode_per_page];
        VectorIndexNodePage *np = this;
        do {
            Buffer buf = ReadBuffer(rel, next_blkno);
            Page page = BufferGetPage(buf);
            np = (VectorIndexNodePage *)PageGetContents(page);
            if (np->nnode != 0) {
                ReleaseBuffer(buf);
                return false;
            }
            next_blkno = np->nodes[nodepage_internal::max_nnode_per_page];
            ReleaseBuffer(buf);
        } while (BlockNumberIsValid(next_blkno) && next_blkno != 0);
    }
    return true;
}
inline BlockNumber *VectorIndexNodePage::get_node_blkno(Relation rel, size_t &nblkno)
{
    nblkno = nnode;
    Vector<BlockNumber, DEFAULT_ALLOCATOR<BlockNumber>, false> res(nnode);
    res.push_back(nodes, nodes + nnode);
    BlockNumber next_blkno = nodes[nodepage_internal::max_nnode_per_page];
    while (next_blkno != 0 && BlockNumberIsValid(next_blkno)) {
        Buffer buf = ReadBuffer(rel, next_blkno);
        Page page = BufferGetPage(buf);
        VectorIndexNodePage *np = (VectorIndexNodePage *)PageGetContents(page);
        next_blkno = np->nodes[nodepage_internal::max_nnode_per_page];
        res.push_back(np->nodes, np->nodes + np->nnode);
        nblkno += np->nnode;
        ReleaseBuffer(buf);
    }
    return res.data();
}
typedef VectorIndexNodePage *VectorIndexNode;

struct DistData {
    float dist;
    ItemPointerData iptr;
    DistData() {}
    DistData(float d, ItemPointerData i) : dist(d), iptr(i) {}
    bool operator<(const DistData &rhs) const { return dist < rhs.dist; }
};

inline VectorIndexType get_vector_index_type(size_t magnIndex, size_t diskann_entry_level) 
{
    if (magnIndex < diskann_entry_level) {
        return IVF;
    } else {
        return GRAPH;
    }
}

inline BlockNumber create_vector_index_nodepage(Relation rel, Vector<BlockNumber> *nodes)
{
    Buffer node_buf;
    Page node_page;
    allocate_buf_page(rel, node_buf, node_page);
    VectorIndexNode node = (VectorIndexNode)PageGetContents(node_page);
    node->init();
    if (nodes != NULL) {
        for (BlockNumber blkno : *nodes) {
            node->insert(rel, blkno, node_buf);
        }
    }

    ((PageHeader)node_page)->pd_lower =
        ((char *)node + nodepage_internal::max_data_size) - (char *)node_page;
    return release_buf_page(node_buf, node_page);
}

inline BlockNumber create_vector_index_metapage(Relation rel, VectorIndexType type, size_t index_magnitude_level,
    BlockNumber leftMostNodeBlkno = InvalidBlockNumber, Vector<BlockNumber> *nodes = NULL)
{
    Buffer meta_buf;
    Page meta_page;
    BlockNumber node_blkno = create_vector_index_nodepage(rel, nodes);
    allocate_buf_page(rel, meta_buf, meta_page);
    VectorIndexMeta meta = (VectorIndexMeta)PageGetContents(meta_page);
    meta->init();
    meta->node_blkno = node_blkno;

    meta->type = type;
    meta->index_magnitude_level = index_magnitude_level;
    meta->dim = TupleDescAttr(rel->rd_att, 0)->atttypmod;
    meta->leftmost_node_blkno = leftMostNodeBlkno;

    ((PageHeader)meta_page)->pd_lower = 
        ((char *)meta + sizeof(VectorIndexMetaPage)) - (char *)meta_page;

    return release_buf_page(meta_buf, meta_page);
}

struct VectorIndexNoneSearchParam {
    float *query;
    TupleDesc tdesc;
    uint32 top_k;
    uint32 dim;
};

/* note that none-index is also a index */
class VectorIndex : public BaseObject, public ann_helper::LeakChecker {
    using leak_checker = ann_helper::LeakChecker;
public:
    VectorIndex(Relation rel, Relation heap, BlockNumber meta_blkno) : _rel(rel), _heap(heap)
    {
        _meta_buf = ReadBuffer(_rel, meta_blkno);
        _meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(BufferGetPage(_meta_buf)));
        _node_buf = ReadBuffer(_rel, _meta->node_blkno);
        _nodes = reinterpret_cast<VectorIndexNode>(PageGetContents(BufferGetPage(_node_buf)));

        Buffer buf = ReadBuffer(_rel, HYBRIDANN_METAPAGE_BLKNO);
        HybridAnnMetaPage *meta = reinterpret_cast<HybridAnnMetaPage *>(PageGetContents(BufferGetPage(buf)));
        _index_magnitude = NEW IndexMagnitude(meta->indexMagnitudes, meta->sizeIndexMagnitudes, meta->graphMagnitudeThreshold);
        ReleaseBuffer(buf);
    }

    VectorIndex(Relation rel, Relation heap, Buffer meta_buf) : _rel(rel), _heap(heap), _meta_buf(meta_buf)
    {
        _meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(BufferGetPage(_meta_buf)));
        _node_buf = ReadBuffer(_rel, _meta->node_blkno);
        _nodes = reinterpret_cast<VectorIndexNode>(PageGetContents(BufferGetPage(_node_buf)));

        Buffer buf = ReadBuffer(_rel, HYBRIDANN_METAPAGE_BLKNO);
        HybridAnnMetaPage *meta = reinterpret_cast<HybridAnnMetaPage *>(PageGetContents(BufferGetPage(buf)));
        _index_magnitude = NEW IndexMagnitude(meta->indexMagnitudes, meta->sizeIndexMagnitudes, meta->graphMagnitudeThreshold);
        ReleaseBuffer(buf);
    }

    virtual ~VectorIndex() {}

    virtual void create_vector_index(vector_pair_vector &data, int parallel_workers,
                                     int maintenance_work_mem, bool need_wal) {}

    virtual BlockNumber prepare_meta(vector_pair_vector &data)
    {
        VectorIndexType new_type = _meta->type == VectorIndexType::None ?
                get_vector_index_type(_meta->index_magnitude_level, _index_magnitude->graph_entry_level()) : _meta->type;
        BlockNumber new_index_blkno = create_vector_index_metapage(_rel, new_type, _meta->index_magnitude_level);
        LogManager logmgr(_rel);
        logmgr.log_vecindex_meta_and_node(new_index_blkno);
        logmgr.destroy();
        return new_index_blkno;
    }

    BlockNumber get_meta_blkno() { return _meta->index_meta_blkno; }

    virtual void build(vector_pair_vector &data, int parallel_workers, int maintenance_work_mem) {}

    virtual void split_to(VectorIndex *new_index, vector_pair_vector &right_data, int parallel_workers) {}

    virtual void merge(BlockNumber merge_vec_meta_blkno) {}

    virtual void insert(size_t idx, ItemPointerData tid) {}

    virtual void batch_insert(vector_pair_vector &data, int parallel_workers) {}

    virtual void vacuum(IndexBulkDeleteCallback callback, const void *callback_state, IdxSet &delete_set, int parallel_workers) {}

    virtual void free_index_meta_pages() { RecordFreeIndexPage(_rel, node_ptr()); }

    virtual void recycle_to_fsm() {}

    virtual size_t search(IndexScanDesc scan, float *dist_out, ItemPointerData *iptr, void *param);

    bool idle() { return _meta->idle; }

    bool deleted() { return _meta->deleted; }

    bool start_operation()
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        if (_meta->deleted || !_meta->idle) {
            LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
            return false;
        }
        _meta->idle = false;
        MarkBufferDirty(_meta_buf);

        LogManager logmgr(_rel);
        logmgr.log_vecindex_set_operation(_meta_buf, _meta->idle);
        logmgr.destroy();
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
        return true;
    }

    void finish_operation()
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        Assert(!_meta->idle);
        _meta->idle = true;
        MarkBufferDirty(_meta_buf);

        LogManager logmgr(_rel);
        logmgr.log_vecindex_set_operation(_meta_buf, _meta->idle);
        logmgr.destroy();
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
    }

    BlockNumber *get_node_blkno(size_t &nblkno)
    {
        LockBuffer(_node_buf, BUFFER_LOCK_SHARE);
        BlockNumber *res = _nodes->get_node_blkno(_rel, nblkno);
        LockBuffer(_node_buf, BUFFER_LOCK_UNLOCK);
        return res;
    }

    bool node_empty()
    {
        LockBuffer(_node_buf, BUFFER_LOCK_SHARE);
        bool res = _nodes->empty(_rel);
        LockBuffer(_node_buf, BUFFER_LOCK_UNLOCK);
        return res;
    }
    
    void insert_node_blkno(BlockNumber blkno)
    {
        LockBuffer(_node_buf, BUFFER_LOCK_EXCLUSIVE);
        Buffer res = _nodes->insert(_rel, blkno, _node_buf);
        if (BufferIsInvalid(res)) {
            LockBuffer(_node_buf, BUFFER_LOCK_UNLOCK);
            return;
        }

        Buffer insert_buf = res == INT_MAX ? _node_buf : res;
        MarkBufferDirty(insert_buf);

        XLogBeginInsert();
        START_CRIT_SECTION();
        XLogRegisterBuffer(0, insert_buf, REGBUF_STANDARD);
        XLogRegisterBufData(0, (char *)&blkno, sizeof(BlockNumber));
        XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_INSERT_NODE);
        PageSetLSN(BufferGetPage(insert_buf), recptr);
        END_CRIT_SECTION();
        if (insert_buf != _node_buf) {
            ReleaseBuffer(insert_buf);
        }
        LockBuffer(_node_buf, BUFFER_LOCK_UNLOCK);
    }

    void remove_node_blkno(BlockNumber blkno)
    {
        LockBuffer(_node_buf, BUFFER_LOCK_EXCLUSIVE);
        Buffer res = _nodes->remove(_rel, blkno);
        if (BufferIsInvalid(res)) {
            LockBuffer(_node_buf, BUFFER_LOCK_UNLOCK);
            return;
        }

        Buffer remove_buf = res == INT_MAX ? _node_buf : res;
        MarkBufferDirty(remove_buf);

        XLogBeginInsert();
        START_CRIT_SECTION();
        XLogRegisterBuffer(0, remove_buf, REGBUF_STANDARD);
        XLogRegisterBufData(0, (char *)&blkno, sizeof(BlockNumber));
        XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_REMOVE_NODE);
        PageSetLSN(BufferGetPage(remove_buf), recptr);
        END_CRIT_SECTION();
        if (remove_buf != _node_buf) {
            ReleaseBuffer(remove_buf);
        }
        LockBuffer(_node_buf, BUFFER_LOCK_UNLOCK);
    }

    void set_split_scanned_blkno(BlockNumber blkno)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        _meta->split_scanned_blkno = blkno;
        MarkBufferDirty(_meta_buf);

        XLogBeginInsert();
        START_CRIT_SECTION();
        XLogRegisterBuffer(0, _meta_buf, REGBUF_STANDARD);
        XLogRegisterBufData(0, (char *)&blkno, sizeof(BlockNumber));
        XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_SPLIT_SCAN_BLKNO);
        PageSetLSN(BufferGetPage(_meta_buf), recptr);
        END_CRIT_SECTION();
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
    }

    void set_leftmost_node_blkno(BlockNumber blkno)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        _meta->leftmost_node_blkno = blkno;
        MarkBufferDirty(_meta_buf);

        XLogBeginInsert();
        START_CRIT_SECTION();
        XLogRegisterBuffer(0, _meta_buf, REGBUF_STANDARD);
        XLogRegisterBufData(0, (char *)&blkno, sizeof(BlockNumber));
        XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_SET_LEFTMOST_NODE_BLKNO);
        PageSetLSN(BufferGetPage(_meta_buf), recptr);
        END_CRIT_SECTION();
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
    }

    void update_index_meta_blkno(BlockNumber index_meta_blkno)
    {
        _meta->index_meta_blkno = index_meta_blkno;
        MarkBufferDirty(_meta_buf);
    }

    void update_next_index_meta_blkno(VectorIndex *new_index, bool need_wal)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_SHARE);
        BlockNumber next_blkno = _meta->next_idx_blkno;
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
        new_index->update_next_index_meta_blkno(next_blkno, need_wal);
        update_next_index_meta_blkno(new_index->ptr(), need_wal);
    }

    void update_next_index_meta_blkno(BlockNumber next, bool need_wal)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        _meta->next_idx_blkno = next;
        MarkBufferDirty(_meta_buf);
        if (need_wal) {
            XLogBeginInsert();
            START_CRIT_SECTION();
            XLogRegisterBuffer(0, _meta_buf, REGBUF_STANDARD);
            XLogRegisterBufData(0, (char *)&next, sizeof(BlockNumber));
            XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_UPDATE_NEXT_META_BLKNO);
            PageSetLSN(BufferGetPage(_meta_buf), recptr);
            END_CRIT_SECTION();
        }
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
    }

    BlockNumber get_leftmost_node() { return leftmost_node(); }

    void set_deleted(const bool deleted) {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        _meta->deleted = deleted;
        MarkBufferDirty(_meta_buf);

        XLogBeginInsert();
        START_CRIT_SECTION();
        XLogRegisterBuffer(0, _meta_buf, REGBUF_STANDARD);
        XLogRegisterBufData(0, (char *)&deleted, sizeof(bool));
        XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_SET_DELETED);
        PageSetLSN(BufferGetPage(_meta_buf), recptr);
        END_CRIT_SECTION();
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
    }

    virtual size_t size() { return 0; }
    BlockNumber &next() { return _meta->next_idx_blkno; }
    BlockNumber &leftmost_node() { return _meta->leftmost_node_blkno; }
    void r_lock() { LockBuffer(_meta_buf, BUFFER_LOCK_SHARE); }
    void w_lock() { LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE); }
    void r_unlock() { LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK); }
    void w_unlock() { LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK); }

    uint32 dim() { return _meta->dim; }
    VectorIndexType type() { return _meta->type; }
    const char *type_name() { return VectorIndexTypeToString(_meta->type); }
    BlockNumber ptr() const { return BufferGetBlockNumber(_meta_buf); }
    BlockNumber node_ptr() const { return BufferGetBlockNumber(_node_buf); }
    size_t index_magnitude_level() { return _meta->index_magnitude_level; }
    BlockNumber index_meta_blkno() { return _meta->index_meta_blkno; }
    BlockNumber split_scanned_blkno() { return _meta->split_scanned_blkno; }
    Buffer meta_buf() { return _meta_buf; }

    virtual void destroy()
    {
        ReleaseBuffer(_node_buf);
        ReleaseBuffer(_meta_buf);
        _index_magnitude->destroy();
        delete _index_magnitude;
        leak_checker::destroy();
    }
protected:
    void split_to_impl(VectorIndex *new_index, vector_pair_vector &right_data, int parallel_workers)
    {
        new_index->batch_insert(right_data, parallel_workers);
        update_next_index_meta_blkno(new_index, true);
    }

    Relation _rel;
    Relation _heap;
    Buffer _meta_buf;
    Buffer _node_buf;
    VectorIndexMeta _meta;
    VectorIndexNode _nodes;
    IndexMagnitude *_index_magnitude;
};

/**
 * Split algorithm:
 * @brief try to split/merge the vector index meta page
 * procedure:
 *  0. check whether index can be splitted/merged
 *  1. vec index -> bt node blk mapping
 *  2. quick scan of bt node blk to get the size
 *  3. start operation, generate criteria for split (only for split of course)
 *  4. split/merge the vec index meta page
 * requirements:
 *  1. all operations should be atomic and reentrant
 *  2. all operations should be thread-safe, as we expect it to run at backend
 */
#endif /* VECTOR_BPTREE_H */
