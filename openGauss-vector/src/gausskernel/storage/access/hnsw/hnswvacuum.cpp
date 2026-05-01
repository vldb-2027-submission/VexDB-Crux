#include <math.h>
#include <vtl/hashtable>
#include <vtl/holder>

#include "postgres.h"
#include "commands/vacuum.h"
#include "storage/buf/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "storage/buf/bufpage.h"
#include "utils/snapmgr.h"
#include "access/tableam.h"

#include "access/hnsw/hnsw.h"
#include "access/hnsw/hnsw_xlog.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/diskann/vector_bt.h"
#include "access/annvector/module/timer.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/store/buffer_manager.h"

using namespace ann_helper;

struct HnswVacuumState {
    /* Info */
    Relation parent_index, parent_heap;
    Partition part_index, part_heap;
    Relation index, heap;
    IndexBulkDeleteResult *stats;
    IndexBulkDeleteCallback callback;
    void *callback_state;
    IdxSet *delete_set;
    slock_t *slock;
    Timer &timer;

    /* Settings */
    int nparallel;
    BlockNumber metablkno;
    BlockNumber headblkno;
    Buffer metabuf;
    Page metapage;
    HnswMetaPage metap;
    QuantizerParam qt_param;

    /* Support functions */
    distance_func dist_func;

    /* Variables */
    UnorderedSet<ItemPointerData> deleted;
    BufferAccessStrategy bas;
    HnswEntryPoint highestPoint;
    BlockNumber *blknos{NULL};
    size_t nblkno;
    size_t *cnt;
    bool is_frontend{true};

    void SetQuantParam(HnswMetaPage metap)
    {
        const QuantizerMetaInfo &qt_metainfo = metap->quantizer_metainfo;
        qt_param.set_type(qt_metainfo.get_type(), qt_metainfo.get_setting_type());
        qt_param.set_resource(index, metap, NULL, false, false);
    }

    HnswVacuumState(Relation index, IndexBulkDeleteResult *in_stats, int nparallel,
        IndexBulkDeleteCallback callback, void *callback_state, BlockNumber metablkno,
        IdxSet *delete_set, slock_t *slock, Timer &timer)
        : index(index),
          callback(callback),
          callback_state(callback_state),
          delete_set(delete_set),
          slock(slock),
          timer(timer),
          nparallel(nparallel),
          metablkno(metablkno)
    {
        stats = in_stats ? in_stats : (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
        metabuf = ReadBuffer(index, metablkno);
        metapage = BufferGetPage(metabuf);
        metap = HnswPageGetMeta(metapage);
        headblkno = isHybridIndex(index) ? metap->head_blkno : HNSW_HEAD_BLKNO;
        dist_func = hnsw_get_aligned_distance_func(index, metap->metric, metap->dimensions, metap->precision_type);
        bas = GetAccessStrategy(BAS_BULKREAD);
        cnt = (size_t *)palloc0(sizeof(size_t));
        if (metap->quantizer_metainfo.has_quant()) {
            if (RelationIsPartition(index)) {
                parent_heap = heap_open(index->rd_index->indrelid, NoLock);
                part_heap = partitionOpen(parent_heap, index->rd_partHeapOid, NoLock);
                heap = partitionGetRelation(parent_heap, part_heap);
            } else {
                parent_heap = InvalidRelation;
                part_heap = InvalidPartition;
                heap = heap_open(index->rd_index->indrelid, NoLock);
            }
        }
        SetQuantParam(metap);
    }
    HnswVacuumState(const HnswVacuumState &other)
        : stats(other.stats),
          callback(other.callback),
          callback_state(other.callback_state),
          delete_set(other.delete_set),
          slock(other.slock),
          timer(other.timer),
          nparallel(other.nparallel),
          metablkno(other.metablkno),
          headblkno(other.headblkno),
          dist_func(other.dist_func),
          deleted(other.deleted),
          blknos(other.blknos),
          nblkno(other.nblkno),
          cnt(other.cnt),
          is_frontend(false)
    {
        if (RelationIsPartition(other.index)) {
            parent_index = index_open(GetBaseRelOidOfParition(other.index), NoLock);
            part_index = partitionOpen(parent_index, RelationGetRelid(other.index), NoLock);
            index = partitionGetRelation(parent_index, part_index);
            if (other.metap->quantizer_metainfo.has_quant()) {
                parent_heap = heap_open(RelationGetRelid(other.parent_heap), NoLock);
                part_heap = partitionOpen(parent_heap, RelationGetRelid(other.heap), NoLock);
                heap = partitionGetRelation(parent_heap, part_heap);
            }
        } else {
            parent_index = InvalidRelation;
            part_index = InvalidPartition;
            index = index_open(RelationGetRelid(other.index), NoLock);
            if (other.metap->quantizer_metainfo.has_quant()) {
                parent_heap = InvalidRelation;
                part_heap = InvalidPartition;
                heap = heap_open(RelationGetRelid(other.heap), NoLock);
            }
        }
        metabuf = ReadBuffer(index, metablkno);
        metapage = BufferGetPage(metabuf);
        metap = HnswPageGetMeta(metapage);
        bas = GetAccessStrategy(BAS_BULKREAD);
        SetQuantParam(metap);
    }

    void destroy()
    {
        qt_param.release_resource();
        optional_destroy(deleted);
        if (bas) {
            FreeAccessStrategy(bas);
            bas = NULL;
        }
        bool has_heap = false;
        if (BufferIsValid(metabuf)) {
            has_heap = metap->quantizer_metainfo.has_quant();
            ReleaseBuffer(metabuf);
            metabuf = InvalidBuffer;
        } else {
            return;
        }
        if (!is_frontend) {
            if (PartitionIsValid(part_index)) {
                releaseDummyRelation(&index);
                partitionClose(parent_index, part_index, NoLock);
                index_close(parent_index, NoLock);
                if (has_heap) {
                    releaseDummyRelation(&heap);
                    partitionClose(parent_heap, part_heap, NoLock);
                    heap_close(parent_heap, NoLock);
                }
            } else {
                index_close(index, NoLock);
                if (has_heap) {
                    heap_close(heap, NoLock);
                }
            }
        } else {
            if (has_heap) {
                if (PartitionIsValid(part_heap)) {
                    releaseDummyRelation(&heap);
                    partitionClose(parent_heap, part_heap, NoLock);
                    index_close(parent_heap, NoLock);
                } else {
                    heap_close(heap, NoLock);
                }
            }
            if (blknos) {
                pfree(blknos);
            }
            pfree(cnt);
        }
    }
};

static bool check_vec_in_heap(HnswVacuumState &vacuumstate, ItemPointerData htid)
{
    if (vacuumstate.qt_param.get_type() == QuantizerType::NONE) {
        return true;
    }

    HeapTuple tuple = (HeapTupleData *)heaptup_alloc(BLCKSZ);
    tuple->t_data = (HeapTupleHeader)((char *)tuple + HEAPTUPLESIZE);
    tuple->t_self = htid;

    Assert(vacuumstate.heap->rd_tam_ops == TableAmHeap);
    Buffer buf;
    bool found = heap_fetch(vacuumstate.heap, SnapshotAny, tuple, &buf, false, NULL) ||
                 heap_fetch(vacuumstate.heap, SnapshotNow, tuple, &buf, false, NULL);
    if (found) {
        ReleaseBuffer(buf);
    }
    heap_freetuple(tuple);
    return found;
}

/*
 * Remove deleted heap TIDs
 *
 * OK to remove for entry point, since always considered for searches and inserts
 */
static void
RemoveHeapTids(HnswVacuumState &vacuumstate)
{
    HnswEntryPoint* highestPoint = &vacuumstate.highestPoint;
    Relation index = vacuumstate.index;
    const HnswMetaPage metap = vacuumstate.metap;
    BlockNumber blkno = vacuumstate.headblkno;
    BufferAccessStrategy bas = vacuumstate.bas;
    IndexBulkDeleteResult *stats = vacuumstate.stats;

    /* Store separately since highestPoint.level is uint8 */
    int highestLevel = -1;

    /* Initialize highest point */
    highestPoint->blkno = InvalidBlockNumber;
    highestPoint->offno = InvalidOffsetNumber;
    Holder<disk_container::PlainStore> ps;
    if (HnswUseCluster(index)) {
        ps.emplace(index, HNSW_PS_BLKNO, true);
    }

    bool page_has_deleted = false;
    Vector<BlockNumber> blknos;
    while (BlockNumberIsValid(blkno)) {
        page_has_deleted = false;
        vacuum_delay_point();

        Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, bas);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buf);
        const OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);

        /* Iterate over nodes */
        for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
            HnswTuple tuple = (HnswTuple) PageGetItem(page, PageGetItemId(page, offno));
            bool dirty;
            uint32 nremoved;
            if (vacuumstate.metap->cluster.cluster_pq) {
                nremoved = tuple->vacuum_tids_pq(vacuumstate.callback,
                    vacuumstate.callback_state, vacuumstate.index, ps,
                    vacuumstate.metap->quantizer_metainfo.metainfo.pq_metainfo.code_size(), dirty);
            } else {
                nremoved = tuple->vacuum_tids(vacuumstate.callback,
                    vacuumstate.callback_state, vacuumstate.index, ps, dirty);
            }
            stats->tuples_removed += nremoved;
            if (dirty) {
                MarkBufferDirty(buf);
                HnswXLogUpdateHeaptid(index, tuple->get_heaptids(), tuple->ntids(), offno, buf, page);
            }
            if (tuple->empty()) {
                /* only remove un-deleted tuples */
                if (!tuple->is_deleted()) {
                    ItemPointerData ip;
                    ItemPointerSet(&ip, blkno, offno);
                    vacuumstate.deleted.insert(ip);
                } else {
                    page_has_deleted = true;
                }
            } else if (tuple->level > highestLevel &&
                       !(blkno == metap->entryBlkno && offno == metap->entryOffno) &&
                       check_vec_in_heap(vacuumstate, tuple->heaptids[0])) {
                /* Keep track of highest non-entry point */
                highestPoint->blkno = blkno;
                highestPoint->offno = offno;
                highestPoint->level = tuple->level;
                highestPoint->floatVectorIndex = tuple->floatVectorIndex;
                highestLevel = tuple->level;
            }
        }

        blknos.push_back(blkno);
        blkno = HnswPageGetOpaque(page)->nextblkno;
        UnlockReleaseBuffer(buf);

        vacuumstate.timer.report_loop("Remove index point data");
    }

    if (HnswUseCluster(index)) {
        ps->destroy();
    }
    vacuumstate.blknos = blknos.data();
    vacuumstate.nblkno = blknos.size();

    if (blknos.empty()) {
        return;
    }

    if (!page_has_deleted) {
        LockPage(index, vacuumstate.metablkno, ExclusiveLock);
        metap->insertPage = blknos.back();
        UnlockPage(index, vacuumstate.metablkno, ExclusiveLock);
    } else {
        blkno = blknos.back();
        Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, bas);
        Page page = BufferGetPage(buf);
        if (!BlockNumberIsValid(HnswPageGetOpaque(page)->nextblkno)) {
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            if (!BlockNumberIsValid(HnswPageGetOpaque(page)->nextblkno)) {
                Buffer newbuf;
                Page newpage;
                HnswInsertAppendPage(index, &newbuf, &newpage, page);
                MarkBufferDirty(newbuf);
                MarkBufferDirty(buf);
                HnswXLogUpdateNextBlkno(index, buf, blkno);
                HnswXLogAppendPage(index, newbuf, newpage);
                UnlockReleaseBuffer(newbuf);
            }
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        }
        blkno = HnswPageGetOpaque(page)->nextblkno;
        ReleaseBuffer(buf);
        LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_EXCLUSIVE);
        metap->insertPage = blkno;
        LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_UNLOCK);
    }
}

/*
 * Check for deleted neighbors
 */
static bool
NeedsUpdated(HnswVacuumState &vacuumstate, HnswTuple tuple)
{
    const int count = (tuple->level + 2) * vacuumstate.metap->m;
    for (int i = 0; i < count; ++i) {
        const ItemPointerData &indextid = tuple->neighbors[i].indexTid;
        if (!ItemPointerIsValid(&indextid)) {
            continue;
        }
        if (vacuumstate.deleted.contains(indextid)) {
            return true;
        }
    }

    /* Also update if layer 0 is not full */
    /* This could indicate too many candidates being deleted during insert */
    return tuple->neighbors[count - 1].indexTid.ip_posid == 0;
}

/*
 * Repair graph for a single element
 */
static void RepairGraphElement(HnswVacuumState &vacuumstate, HnswTuple tuple, BlkOffsetNumEntry &entry)
{
    Relation index = vacuumstate.index;
    Relation heap = vacuumstate.heap;
    HnswMetaPage metap = vacuumstate.metap;
    const uint32 dim = metap->dimensions;
    BufferAccessStrategy bas = vacuumstate.bas;
    QuantizerParam &qt_param = vacuumstate.qt_param;

    BulkBuffer *bulkbuf = GET_BULKBUF(index);
    qt_param.bulkbuf = bulkbuf;
    VecStorageType st = GetVecStorageType(qt_param, true);
    const size_t vec_size = dim * VEC_ELEM_SIZE(metap->precision_type);
    char *vector = st != VecStorageType::PureCode && bulkbuf ? NULL : alloc_vector(vec_size);
    if (st == VecStorageType::PureCode) {
        if(!fetch_vec_from_heap(index, heap, tuple->heaptids[0], vector, dim, metap->precision_type)) {
            free_vector(vector);
            ereport(WARNING, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("could not fetch vector from heap for heaptid (%u:%u)",
                    ItemPointerGetBlockNumber(&tuple->heaptids[0]),
                    ItemPointerGetOffsetNumber(&tuple->heaptids[0]))));
            return;
        }
    } else {
        if (bulkbuf) {
            vector = bulkbuf->get(tuple->floatVectorIndex);
        } else {
            VecBuffer vec_buf = vec_read_buffer(index, tuple->floatVectorIndex, vec_size, st);
            error_t rc = memcpy_s(vector, vec_size, vec_buf.get_vecbuf(), vec_size);
            securec_check(rc, "\0", "\0");
            vec_buf.release();
        }
    }

    /* Find neighbors for element, skipping itself */
    HnswFindElementNeighborsonDisk(index, heap, vector, tuple, vacuumstate.metabuf, metap,
                                   &entry, false, qt_param);

    if (!bulkbuf || st == VecStorageType::PureCode) {
        free_vector(vector);
    }

    /* Do this before getting page to minimize locking */
    const uint16 m = metap->m;
    Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, entry.blkno, RBM_NORMAL, bas);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buf);
    ItemId itemid = PageGetItemId(page, entry.offno);
    HnswTuple tupleDisk = (HnswTuple)PageGetItem(page, itemid);
    Assert(tupleDisk->level == tuple->level);
    uint32 n = (tupleDisk->level + 2) * m;
    Size neighbor_size = n * sizeof(HnswNeighborData);
    errno_t rc = memcpy_s(tupleDisk->neighbors, neighbor_size, tuple->neighbors, neighbor_size);
    securec_check(rc, "\0", "\0");

    /* Commit */
    MarkBufferDirty(buf);
    HnswXLogWriteNeighbors(index, tupleDisk->neighbors, n, entry.offno, buf, page);
    UnlockReleaseBuffer(buf);

    /* Update neighbors */
    HnswUpdateNeighborsOnDisk(index, heap, vacuumstate.dist_func, tuple, &entry,
                              vacuumstate.metapage, true, false, qt_param);

    /* release bulk buffer */
    if (bulkbuf) {
        bulkbuf->release();
        qt_param.bulkbuf = NULL;
    }
}

static void RepairGraphEntryPoint(HnswVacuumState &vacuumstate)
{
    Relation index = vacuumstate.index;
    const HnswEntryPoint &highestPoint = vacuumstate.highestPoint;

    /*
     * Repair graph for highest non-entry point. Highest point may be outdated
     * due to inserts that happen during and after RemoveHeapTids.
     */
    if (BlockNumberIsValid(highestPoint.blkno)) {
        LockPage(index, vacuumstate.metablkno, ShareLock);
        Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, highestPoint.blkno, RBM_NORMAL, vacuumstate.bas);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        ItemId itemid = PageGetItemId(page, highestPoint.offno);
        HnswTuple highesttuple = (HnswTuple)PageGetItem(page, itemid);

        if (NeedsUpdated(vacuumstate, highesttuple)) {
            const uint16 tupleSize = ItemIdGetLength(itemid);
            HnswTuple tup_copy = (HnswTuple)palloc(tupleSize);
            error_t rc = memcpy_s(tup_copy, tupleSize, highesttuple, tupleSize);
            securec_check(rc, "\0", "\0");
            UnlockReleaseBuffer(buf);
            LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_SHARE);
            if (highestPoint.blkno == vacuumstate.metap->entryBlkno &&
                highestPoint.offno == vacuumstate.metap->entryOffno) {
                LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_UNLOCK);
                BlkOffsetNumEntry entry(highestPoint.blkno, highestPoint.offno);
                RepairGraphElement(vacuumstate, tup_copy, entry);
            } else {
                LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_UNLOCK);
            }
            pfree(tup_copy);
        } else {
            UnlockReleaseBuffer(buf);
        }
        UnlockPage(index, vacuumstate.metablkno, ShareLock);
    }

    LockPage(index, vacuumstate.metablkno, ExclusiveLock);
    /* Prevent concurrent inserts when possibly updating entry point */
    HnswMetaPage metap = vacuumstate.metap;
    LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_SHARE);
    const BlockNumber entry_blkno = metap->entryBlkno;
    const OffsetNumber entry_offno = metap->entryOffno;
    LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_UNLOCK);
    if (BlockNumberIsValid(entry_blkno)) {
        ItemPointerData epData;
        ItemPointerSet(&epData, entry_blkno, entry_offno);
        if (vacuumstate.deleted.contains(epData) ||
            !check_vec_in_heap(vacuumstate, get_heap_tid(index, epData))) {
            /*
             * Replace the entry point with the highest point. If highest
             * point is outdated and empty, the entry point will be empty
             * until an element is repaired.
             */
            LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_EXCLUSIVE);
            metap->entryBlkno = highestPoint.blkno;
            metap->entryOffno = highestPoint.offno;
            metap->entryLevel = highestPoint.level;
            MarkBufferDirty(vacuumstate.metabuf);
            HnswXLogUpdateMetaEntry(index, vacuumstate.metabuf, vacuumstate.metapage,
                {highestPoint.level, highestPoint.offno, highestPoint.blkno});
            LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_UNLOCK);
        } else {
            /*
             * Repair the entry point with the highest point. If highest point
             * is outdated, this can remove connections at higher levels in
             * the graph until they are repaired, but this should be fine.
             */
            Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, entry_blkno, RBM_NORMAL, vacuumstate.bas);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            Page page = BufferGetPage(buf);
            ItemId itemid = PageGetItemId(page, entry_offno);
            HnswTuple entrytuple = (HnswTuple)PageGetItem(page, itemid);

            if (NeedsUpdated(vacuumstate, entrytuple)) {
                const uint16 tupleSize = ItemIdGetLength(itemid);
                HnswTuple tup_copy = (HnswTuple)palloc(tupleSize);
                error_t rc = memcpy_s(tup_copy, tupleSize, entrytuple, tupleSize);
                securec_check(rc, "\0", "\0");
                UnlockReleaseBuffer(buf);
                BlkOffsetNumEntry entry(entry_blkno, entry_offno);
                RepairGraphElement(vacuumstate, tup_copy, entry);
                pfree(tup_copy);
            } else {
                UnlockReleaseBuffer(buf);
            }
        }
    }
    UnlockPage(index, vacuumstate.metablkno, ExclusiveLock);
}

static BlockNumber RepairGraphBlock(HnswVacuumState &vacuumstate, Vector<HnswTuple> &tuples,
    Vector<OffsetNumber> &entries, BlockNumber blkno)
{
    Relation index = vacuumstate.index;
    HnswMetaPage metap = vacuumstate.metap;
    Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, vacuumstate.bas);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buf);
    BlockNumber res = HnswPageGetOpaque(page)->nextblkno;
    const OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);

    /* Load items into memory to minimize locking */
    for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
        ItemId itemid = PageGetItemId(page, offno);
        HnswTuple tuple = (HnswTuple)PageGetItem(page, itemid);
        if (tuple->empty() || !NeedsUpdated(vacuumstate, tuple)) {
            continue;
        }

        /* Create an tuple copy */
        const int tupleSize = ItemIdGetLength(itemid);
        HnswTuple tup_copy = (HnswTuple)palloc(tupleSize);
        error_t rc = memcpy_s(tup_copy, tupleSize, tuple, tupleSize);
        securec_check(rc, "\0", "\0");
        tuples.push_back(tup_copy);
        entries.emplace_back(offno);
    }
    UnlockReleaseBuffer(buf);

    /* Update neighbor pages */
    size_t len = tuples.size();
    for (size_t i = 0; i < len; ++i) {
        HnswTuple tuple = tuples[i];
        BlkOffsetNumEntry entry = {blkno, entries[i]};

        LockPage(index, vacuumstate.metablkno, ShareLock);
        LOCKMODE lmode = ShareLock;
        /* Prevent concurrent inserts when likely updating entry point */
        if (!BlockNumberIsValid(metap->entryBlkno) || tuple->level > metap->entryLevel) {
            UnlockPage(index, vacuumstate.metablkno, ShareLock);
            lmode = ExclusiveLock;
            LockPage(index, vacuumstate.metablkno, ExclusiveLock);
        }

        /* Repair connections */
        RepairGraphElement(vacuumstate, tuple, entry);

        /*
         * Update metapage if needed. Should only happen if entry point
         * was replaced and highest point was outdated.
         */
        if (!BlockNumberIsValid(metap->entryBlkno) || tuple->level > metap->entryLevel) {
            LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_EXCLUSIVE);
            if ((!BlockNumberIsValid(metap->entryBlkno) || tuple->level > metap->entryLevel) &&
                check_vec_in_heap(vacuumstate, tuple->heaptids[0])) {
                metap->entryBlkno = entry.blkno;
                metap->entryOffno = entry.offno;
                metap->entryLevel = tuple->level;
                MarkBufferDirty(vacuumstate.metabuf);
                HnswXLogUpdateMetaEntry(index, vacuumstate.metabuf, vacuumstate.metapage,
                    {tuple->level, entry});
            }
            LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_UNLOCK);
        }
        UnlockPage(index, vacuumstate.metablkno, lmode);
        pfree(tuple);
    }
    return res;
}

static void RepairGraphInternal(HnswVacuumState &vacuumstate)
{
    const auto get_cnt = [&vacuumstate]() -> size_t {
        return __atomic_fetch_add(vacuumstate.cnt, 1ul, __ATOMIC_RELAXED);
    };
    Vector<HnswTuple> tuples(vacuumstate.metap->m);
    Vector<OffsetNumber> entries(vacuumstate.metap->m);
    MemoryContext ctx = AllocSetContextCreate(CurrentMemoryContext,
        "Hnsw vacuum temporary context", ALLOCSET_DEFAULT_SIZES);
    for (size_t i = get_cnt(); i < vacuumstate.nblkno; i = get_cnt()) {
        BlockNumber blkno = vacuumstate.blknos[i];
        vacuum_delay_point();
        MemoryContext old_ctx = MemoryContextSwitchTo(ctx);

        RepairGraphBlock(vacuumstate, tuples, entries, blkno);

        tuples.clear();
        entries.clear();
        vacuumstate.timer.inc_loop_count_forground_report("Modify index neighboring layout");

        MemoryContextSwitchTo(old_ctx);
        MemoryContextResetAndDeleteChildren(ctx);
    }
    MemoryContextDelete(ctx);
    optional_destroy(tuples);
    optional_destroy(entries);
}

static void RepairGraphParallelMain(const BgWorkerContext *bwc)
{
    HnswVacuumState *in_vacuumstate = *(HnswVacuumState **)bwc->bgshared;
    HnswVacuumState vacuumstate(*in_vacuumstate);
    RepairGraphInternal(vacuumstate);
    vacuumstate.destroy();
}
static void RepairGraphParallelCleanUp(const BgWorkerContext *bwc) {}

/*
 * Repair graph for all elements
 */
static void RepairGraph(HnswVacuumState &vacuumstate)
{
    if (vacuumstate.nblkno == 0) {
        return;
    }

    /* Repair entry point first */
    RepairGraphEntryPoint(vacuumstate);

    int nworker = vacuumstate.nparallel;
    if (nworker > 0) {
        HnswVacuumState **in_vacuumstate = (HnswVacuumState **)palloc(sizeof(HnswVacuumState *));
        *in_vacuumstate = &vacuumstate;
        nworker = LaunchBackgroundWorkers(nworker, in_vacuumstate, RepairGraphParallelMain,
            RepairGraphParallelCleanUp, false);
    }
    RepairGraphInternal(vacuumstate);
    if (nworker > 0) {
        BgworkerListWaitFinish(&nworker);
        BgworkerListSyncQuit();
    }

    Relation index = vacuumstate.index;
    Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM,
        vacuumstate.blknos[vacuumstate.nblkno - 1u], RBM_NORMAL, vacuumstate.bas);
    /* Wait for inserts to complete */
    LockPage(index, vacuumstate.metablkno, ExclusiveLock);
    BlockNumber blkno = HnswPageGetOpaque(BufferGetPage(buf))->nextblkno;
    UnlockPage(index, vacuumstate.metablkno, ExclusiveLock);
    ReleaseBuffer(buf);
    if (!BlockNumberIsValid(blkno)) {
        return;
    }

    Vector<HnswTuple> tuples(vacuumstate.metap->m);
    Vector<OffsetNumber> entries(vacuumstate.metap->m);
    MemoryContext ctx = AllocSetContextCreate(CurrentMemoryContext,
        "Hnsw vacuum temporary context", ALLOCSET_DEFAULT_SIZES);
    vacuumstate.timer.set_stage("Modify index additional neighboring layout");
    do {
        vacuum_delay_point();
        MemoryContext old_ctx = MemoryContextSwitchTo(ctx);

        blkno = RepairGraphBlock(vacuumstate, tuples, entries, blkno);

        tuples.clear();
        entries.clear();
        MemoryContextSwitchTo(old_ctx);
        MemoryContextResetAndDeleteChildren(ctx);
    } while (BlockNumberIsValid(blkno));
    MemoryContextDelete(ctx);
    optional_destroy(tuples);
    optional_destroy(entries);
}

static void MarkDeleted(HnswVacuumState &vacuumstate)
{
    if (vacuumstate.nblkno == 0) {
        return;
    }

    BlockNumber insertPage = InvalidBlockNumber;
    Relation index = vacuumstate.index;
    HnswMetaPage metap = vacuumstate.metap;
    const uint32 dim = metap->dimensions;
    const size_t vec_size = dim * VEC_ELEM_SIZE(metap->precision_type);

    for (size_t i = 0; i < vacuumstate.nblkno; ++i) {
        BlockNumber blkno = vacuumstate.blknos[i];
        vacuum_delay_point();

        Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, vacuumstate.bas);
        /*
         * ambulkdelete cannot delete entries from pages that are pinned by
         * other backends
         *
         * https://www.postgresql.org/docs/current/index-locking.html
         */
        LockBufferForCleanup(buf);
        Page page = BufferGetPage(buf);
        const OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);

        /* Update element and neighbors together */
        for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
            ItemId itemid = PageGetItemId(page, offno);
            HnswTuple tuple = (HnswTuple)PageGetItem(page, itemid);

            /* Skip deleted tuples */
            if (tuple->is_deleted()) {
                /* Set to first free page */
                if (!BlockNumberIsValid(insertPage)) {
                    insertPage = blkno;
                }
                continue;
            }

            /* Skip live tuples */
            if (tuple->ntids() > 0) {
                continue;
            }

            /* Overwrite element */
            tuple->set_deleted();
            LogManager logmgr(index);
            if (vacuumstate.qt_param.get_type() == QuantizerType::PQ) {
                PQParam &pq_param = vacuumstate.qt_param.get_pq_param();
                uint32 code_len = pq_param.code_len;
                vec_invalidate_buffer_cache(index->rd_smgr->smgr_rnode.node.relNode, tuple->floatVectorIndex, code_len);
                logmgr.log_invalidate_vector_cache(index, tuple->floatVectorIndex, code_len,
                    RM_HNSW_ID, XLOG_HNSW_INVALIDATE_VECTOR_CACHE);
            } else if (vacuumstate.qt_param.get_type() == QuantizerType::RABITQ) {
                RaBitQParam &rbq_param = vacuumstate.qt_param.get_rabitq_param();
                if (rbq_param.rbq_meta.keep_vecs) {
                    vec_invalidate_buffer_cache(index->rd_smgr->smgr_rnode.node.relNode, tuple->floatVectorIndex, vec_size);
                    logmgr.log_invalidate_vector_cache(index, tuple->floatVectorIndex, vec_size,
                        RM_HNSW_ID, XLOG_HNSW_INVALIDATE_VECTOR_CACHE);
                }
                int quant_size = rbq_param.rbq_meta.quant_size;
                vec_invalidate_buffer_cache(index->rd_smgr->smgr_rnode.node.relNode, tuple->floatVectorIndex, quant_size);
                logmgr.log_invalidate_vector_cache(index, tuple->floatVectorIndex, quant_size,
                    RM_HNSW_ID, XLOG_HNSW_INVALIDATE_VECTOR_CACHE);
            } else {
                vec_invalidate_buffer_cache(index->rd_smgr->smgr_rnode.node.relNode, tuple->floatVectorIndex, vec_size);
                logmgr.log_invalidate_vector_cache(index, tuple->floatVectorIndex, vec_size,
                    RM_HNSW_ID, XLOG_HNSW_INVALIDATE_VECTOR_CACHE);
            }
            logmgr.destroy();

            /* Overwrite neighbors */
            uint32 nneighbor = (tuple->level + 2) * metap->m;
            for (uint32 i = 0; i < nneighbor; ++i) {
                ItemPointerSetInvalid(&tuple->neighbors[i].indexTid);
            }

            if (vacuumstate.delete_set) {
                vacuumstate.delete_set->insert(tuple->floatVectorIndex);
            }

            /* Commit */
            MarkBufferDirty(buf);
            Assert(offno >= 1);
            HnswXLogMarkDelete(index, offno, buf, page);

            /* Set to first free page */
            if (!BlockNumberIsValid(insertPage)) {
                insertPage = blkno;
            }
        }

        UnlockReleaseBuffer(buf);
        vacuumstate.timer.report_loop("Recollect index space");
    }

    /* Update insert page last, after everything has been marked as deleted */
    LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_EXCLUSIVE);
    if (BlockNumberIsValid(insertPage) && vacuumstate.metap->insertPage != insertPage) {
        vacuumstate.metap->insertPage = insertPage;
        MarkBufferDirty(vacuumstate.metabuf);
        HnswXLogUpdateMetaInsertPage(index, vacuumstate.metabuf, vacuumstate.metapage, insertPage);
    }
    LockBuffer(vacuumstate.metabuf, BUFFER_LOCK_UNLOCK);
}

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *hnswbulkdelete_internal(Relation index, IndexBulkDeleteResult *stats,
    int nparallel, IndexBulkDeleteCallback callback, void *callback_state, BlockNumber metablkno,
    IdxSet *delete_set)
{
    slock_t slock;
    S_INIT_LOCK(&slock);
    char index_name[NAMEDATALEN + 1];
    char part_name[NAMEDATALEN + 1];
    populate_index_partition_name(index, index_name, part_name);
    constexpr size_t npage_per_report = 10'000ul;
    Timer timer(0, npage_per_report, index_name, part_name);
    HnswVacuumState vacuumstate(index, stats, nparallel, callback, callback_state, metablkno,
                                delete_set, &slock, timer);

    /* Pass 1: Remove heap TIDs */
    timer.set_stage("Remove index point data");
    RemoveHeapTids(vacuumstate);
    timer.report("Remove index point data done");

    /* Pass 2: Repair graph */
    timer.set_nloop(vacuumstate.nblkno);
    timer.reset_step(npage_per_report);
    timer.set_stage("Modify index neighboring layout");
    RepairGraph(vacuumstate);
    timer.report("Modify index neighboring layout done");

    /* Pass 3: Mark as deleted */
    timer.reset_step(npage_per_report);
    timer.set_stage("Recollect index space");
    MarkDeleted(vacuumstate);
    timer.report("Recollect index space done");

    IndexBulkDeleteResult *res = vacuumstate.stats;
    vacuumstate.destroy();
    S_LOCK_FREE(&slock);
    timer.destroy();
    return res;
}

IndexBulkDeleteResult *hnswvacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    if (info->analyze_only) {
        return stats;
    }
    /* stats is NULL if ambulkdelete not called */
    /* OK to return NULL if index not changed */
    if (stats == NULL) {
        return NULL;
    }

    stats->num_pages = RelationGetNumberOfBlocks(info->index);
    return stats;
}
