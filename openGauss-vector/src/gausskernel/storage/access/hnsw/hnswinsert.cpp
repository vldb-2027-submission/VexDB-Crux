#include <math.h>

#include "postgres.h"
#include "storage/buf/bufmgr.h"
#include "storage/buf/bufpage.h"
#include "storage/lmgr.h" 
#include "utils/memutils.h"
#include "utils/datum.h"

#include "access/hnsw/hnsw.h"
#include "access/hnsw/hnsw_xlog.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/store/buffer_manager.h"
#include "access/index_backend/index_backend.h"
#include "access/rabitq/rabitq.h"

using namespace ann_helper;
using namespace rabitq;

static bool HnswFreeOffset(Relation index, Buffer buf, Page page, Size tupleSize,
    OffsetNumber *freeOffno, BlockNumber *newInsertPage)
{
    OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);
    for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
        ItemId itemid = PageGetItemId(page, offno);
        HnswTuple tuple = (HnswTuple) PageGetItem(page, itemid);

        if (tuple->is_deleted()) {
            BlockNumber elementPage = BufferGetBlockNumber(buf);
            /* Ensure aligned for space check */
            Assert(tupleSize == MAXALIGN(tupleSize));

            /*
             * Calculate free space individually since tuples are overwritten
             * individually (in separate calls to PageIndexTupleOverwrite)
             */
            Size pageFree = ItemIdGetLength(itemid) + PageGetExactFreeSpace(page);

            /* Check for space */
            if (pageFree >= tupleSize) {
                *freeOffno = offno;
                *newInsertPage = elementPage;
                return true;
            }
        }
    }
    return false;
}

void HnswInsertAppendPage(Relation index, Buffer *nbuf, Page *npage, Page page)
{
    /* Add a new page */
    *nbuf = HnswNewBuffer(index, MAIN_FORKNUM, true);
    /* Init new page */
    *npage = BufferGetPage(*nbuf);
    HnswInitPage(*nbuf, *npage);

    /* Update previous buffer */
    if (page) {
        HnswPageGetOpaque(page)->nextblkno = BufferGetBlockNumber(*nbuf);
    }
}

static BlkOffsetNumEntry AddElementOnDisk(Relation index, HnswTuple tuple, char *value,
    Buffer metabuf, Page metapage, bool building, QuantizerParam &qt_param)
{
    HnswMetaPage metap = HnswPageGetMeta(metapage);
    Buffer buf;
    Page page;
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    BlockNumber currentPage = metap->insertPage;
    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

    OffsetNumber freeOffno = InvalidOffsetNumber;
    BlockNumber newInsertPage = InvalidBlockNumber;
    Size tupleSize = HNSW_TUPLE_SIZE(tuple->level, metap->m);

    /* Find a page to insert the tuples */
    for (;;) {
        /*
         * 1. freeSpace > tupleSize,  PageAddItem
         * 2. freeSpace < tupleSize, HnswFreeOffset return true ---> page_index_tuple_overwrite.
         * 3. freeSpace < tupleSize, HnswFreeOffset return false, goto nextPage and return 1 and 2.
         * 4. if nextPage is invalid, then append a new page, and PageAddItem.
         */
        if (BlockNumberIsValid(currentPage)) {
            buf = ReadBuffer(index, currentPage);
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            page = BufferGetPage(buf);
        } else {
            HnswInsertAppendPage(index, &buf, &page, NULL);
            currentPage = BufferGetBlockNumber(buf);
            MarkBufferDirty(buf);
            if (!building) {
                HnswXLogAppendPage(index, buf, page);
            }
        }

        /* 1. freeSpace > tupleSize, break, PageAddItem */
        if (PageGetFreeSpace(page) >= tupleSize) {
            newInsertPage = currentPage;
            break;
        }

        /* 2. freeSpace < tupleSize, HnswFreeOffset return true, break, page_index_tuple_overwrite */
        if (HnswFreeOffset(index, buf, page, tupleSize, &freeOffno, &newInsertPage)) {
            break;
        }

        currentPage = HnswPageGetOpaque(page)->nextblkno;

        /* go to nextPage, if valid, continue; else append new page */
        if (BlockNumberIsValid(currentPage)) {
            /* Move to next page */
            UnlockReleaseBuffer(buf);
        } else {
            Buffer newbuf;
            Page newpage;
            HnswInsertAppendPage(index, &newbuf, &newpage, page);

            /* Commit */
            MarkBufferDirty(newbuf);
            MarkBufferDirty(buf);
            if (!building) {
                HnswXLogUpdateNextBlkno(index, buf, HnswPageGetOpaque(page)->nextblkno);
                HnswXLogAppendPage(index, newbuf, newpage);
            }
            /* Unlock previous buffer */
            UnlockReleaseBuffer(buf);

            /* Prepare new buffer */
            buf = newbuf;
            page = newpage;
            newInsertPage = BufferGetBlockNumber(buf);
            break;
        }
    }

    const size_t vec_size = metap->dimensions * VEC_ELEM_SIZE(metap->precision_type);
    OffsetNumber insertOffno = InvalidOffsetNumber;
    /* Add element tuple*/
    if (OffsetNumberIsValid(freeOffno)) {
        ItemId itemid = PageGetItemId(page, freeOffno);
        HnswTuple freetup = (HnswTuple)PageGetItem(page, itemid);
        if (!isHybridIndex(index)) {
            tuple->floatVectorIndex = freetup->floatVectorIndex;
            if (qt_param.get_type() == QuantizerType::PQ) {
                PQParam &pq_param = qt_param.get_pq_param();
                HnswSetElementQTCode(index, tuple->floatVectorIndex, pq_param.code_len, pq_param.code, building, false);
            } else if (qt_param.get_type() == QuantizerType::RABITQ) {
                RaBitQParam &rbq_param = qt_param.get_rabitq_param();
                if (rbq_param.rbq_meta.keep_vecs) {
                    HnswSetElementVector(index, tuple->floatVectorIndex, vec_size, value, building, true);
                }
                HnswSetElementQTCode(index, tuple->floatVectorIndex, rbq_param.rbq_meta.quant_size, rbq_param.quant_data,
                    building, rbq_param.rbq_meta.keep_vecs);
            } else {
                HnswSetElementVector(index, tuple->floatVectorIndex, vec_size, value, building);
            }
            if (qt_param.get_setting_type() != QuantizerType::NONE && !building) {
                LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
                ++metap->quantizer_metainfo.num_new_data;
                MarkBufferDirty(metabuf);
                HnswXLogUpdateMetaNumNewData(index, metabuf, metapage, metap->quantizer_metainfo.num_new_data);
                LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
            }
        }

        if (!page_index_tuple_overwrite(page, freeOffno, (Item)tuple, tupleSize)) {
            elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
        }
        insertOffno = freeOffno;
    } else {
        if (!isHybridIndex(index)) {
            LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
            size_t floatVectorIndex = metap->num_vectors++;
            MarkBufferDirty(metabuf);
            if (!building) {
                HnswXLogUpdateMetaNumVector(index, metabuf, metapage, metap->num_vectors);
                if (qt_param.get_setting_type() != QuantizerType::NONE) {
                    ++metap->quantizer_metainfo.num_new_data;
                    MarkBufferDirty(metabuf);
                    HnswXLogUpdateMetaNumNewData(index, metabuf, metapage, metap->quantizer_metainfo.num_new_data);
                }
            }
            LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
            tuple->floatVectorIndex = floatVectorIndex;
            if (qt_param.get_type() == QuantizerType::PQ) {
                PQParam &pq_param = qt_param.get_pq_param();
                HnswSetElementQTCode(index, floatVectorIndex, pq_param.code_len, pq_param.code, building, false);
            } else if (qt_param.get_type() == QuantizerType::RABITQ) {
                RaBitQParam &rbq_param = qt_param.get_rabitq_param();
                if (rbq_param.rbq_meta.keep_vecs) {
                    HnswSetElementVector(index, floatVectorIndex, vec_size, value, building, true);
                }
                HnswSetElementQTCode(index, floatVectorIndex, rbq_param.rbq_meta.quant_size, rbq_param.quant_data,
                    building, rbq_param.rbq_meta.keep_vecs);
            } else {
                HnswSetElementVector(index, floatVectorIndex, vec_size, value, building);
            }
        }
        insertOffno = PageAddItem(page, (Item) tuple, tupleSize, InvalidOffsetNumber, false, false);
        if (insertOffno == InvalidOffsetNumber) {
            elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
        }
    }
    BlkOffsetNumEntry entry = {BufferGetBlockNumber(buf), insertOffno};

    /* Commit */
    MarkBufferDirty(buf);
    if (!building) {
        HnswXLogAddElement(index, tupleSize, freeOffno, insertOffno, (Item)tuple, buf, page);
    }
    UnlockReleaseBuffer(buf);

    /* update bulkbuffer */
    BulkBuffer *bulkbuf = qt_param.bulkbuf;
    if (bulkbuf) {
        QuantizerType qt_type = qt_param.get_type();
        if (qt_type == QuantizerType::PQ) {
            bulkbuf->update(tuple->floatVectorIndex, qt_param.get_pq_param().code);
        } else if (qt_type == QuantizerType::RABITQ) {
            bulkbuf->update(tuple->floatVectorIndex, qt_param.get_rabitq_param().quant_data);
        } else if (qt_type == QuantizerType::NONE) {
            bulkbuf->update(tuple->floatVectorIndex, value);
        }
    }

    /* if is updating qtcode, record */
    auto qt_update_mgr = (QtUpdateMgr *)g_instance.annvec_cxt.qt_update_mgr;
    QtUpdatingBuffer *qtbuf= qt_update_mgr->find_record(index->rd_id);
    if (qtbuf) {
        auto ctx = MemoryContextSwitchTo(g_instance.diskann_cxt.vec_indexer_ctx);
        float *vec = (float *)palloc(vec_size);
        errno_t rc = memcpy_s(vec, vec_size, value, vec_size);
        securec_check_c(rc, "\0", "\0");
        SpinLockAcquire(&qtbuf->lock);
        qtbuf->record_vec.emplace_back(tuple->floatVectorIndex, vec);
        SpinLockRelease(&qtbuf->lock);
        MemoryContextSwitchTo(ctx);
    }

    /* Update the insert page */
    if (BlockNumberIsValid(newInsertPage) && newInsertPage != metap->insertPage) {
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        if (newInsertPage != metap->insertPage) {
            metap->insertPage = newInsertPage;
            MarkBufferDirty(metabuf);
            if (!building) {
                HnswXLogUpdateMetaInsertPage(index, metabuf, metapage, newInsertPage);
            }
        }
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }
    return entry;
}

void HnswUpdateNeighborsOnDisk(Relation index, Relation heap, distance_func dist_func,
    HnswTuple tuple, BlkOffsetNumEntry *entry, Page metapage, bool checkExisting, bool building,
    QuantizerParam &qt_param)
{
    HnswMetaPage metap = HnswPageGetMeta(metapage);
    const int m = metap->m; 
    for (uint8 ilc = 0; ilc <= tuple->level; ++ilc) {
        const uint8 lc = tuple->level - ilc;
        int start = m * ilc;
        int lm = HnswGetLayerM(m, lc);
        for (int i = start; i < start + lm; ++i) {
            if (!ItemPointerIsValid(&(tuple->neighbors[i].indexTid))) {
                break;
            }

            /*
             * Could improve performance for vacuuming by checking neighbors
             * against list of elements being deleted to find index. It's
             * important to exclude already deleted elements for this since
             * they can be replaced at any time.
             */

            /* Select neighbors */
            int idx = HnswUpdateConnectionDisk(index, heap, tuple, &tuple->neighbors[i], entry,
                dist_func, lm, lc, metapage, checkExisting, qt_param);
            /* New element was not selected as a neighbor or connection alreay existed */
            if (idx < 0) {
                continue;
            }

            /* Register page */
            Buffer buf = ReadBuffer(index, ItemPointerGetBlockNumber(&tuple->neighbors[i].indexTid));
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            Page page = BufferGetPage(buf);
            OffsetNumber offno = ItemPointerGetOffsetNumber(&tuple->neighbors[i].indexTid);
            ItemId itemid = PageGetItemId(page, offno);
            HnswTuple ntuple = (HnswTuple)PageGetItem(page, itemid);
            ItemPointer indextid = &ntuple->neighbors[idx].indexTid;
            /* Update neighbor on the buffer */
            ItemPointerSet(indextid, entry->blkno, entry->offno);
            ntuple->neighbors[idx].floatVectorIndex = tuple->floatVectorIndex;

            /* Commit */
            MarkBufferDirty(buf);
            if (!building) {
                HnswXLogWriteNeighbor(index, &ntuple->neighbors[idx], idx, offno, buf, page);
            }
            UnlockReleaseBuffer(buf);
        }
    }
}

static void UpdateGraphOnDisk(Relation index, Relation heap, HnswTuple tuple, char *value,
    Buffer metabuf, Page metapage, bool building, QuantizerParam &qt_param)
{
    /* Add element */
    BlkOffsetNumEntry entry = AddElementOnDisk(index, tuple, value, metabuf, metapage, building, qt_param);

    HnswMetaPage metap = HnswPageGetMeta(metapage);
    const distance_func dist_func = hnsw_get_aligned_distance_func(index, metap->metric,
        metap->dimensions, metap->precision_type);
    /* Update neighbors */
    HnswUpdateNeighborsOnDisk(index, heap, dist_func, tuple, &entry, metapage, false, building, qt_param);
    /* Update entry point if needed */
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    if (!BlockNumberIsValid(metap->entryBlkno) || tuple->level > metap->entryLevel) {
        metap->entryBlkno = entry.blkno;
        metap->entryOffno = entry.offno;
        metap->entryLevel = tuple->level;
        MarkBufferDirty(metabuf);
        if (!building) {
            HnswXLogUpdateMetaEntry(index, metabuf, metapage, {tuple->level, entry});
        }
    }
    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
}

bool HnswInsertTupleOnDisk(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, bool building, size_t floatvectorIndex, BlockNumber metablkno)
{
    Buffer metabuf = ReadBuffer(index, metablkno);
    Page metapage = BufferGetPage(metabuf);
    HnswMetaPage metap = HnswPageGetMeta(metapage);
    const DistPrecisionType precision_type = metap->precision_type;
    const uint32 dim = metap->dimensions;

    Pointer vec_p;
    char *v = DatumGetVector(values[0], precision_type, &vec_p);

    char *value = v;
    const size_t vec_size = dim * VEC_ELEM_SIZE(precision_type);
    FmgrInfo *normprocinfo = HnswOptionalProcInfo(index, HNSW_NORM_PROC);
    if (normprocinfo != NULL) {
        char *temp = alloc_vector(vec_size);
        auto func = get_vector_preprocess_func(Metric::FAST_COSINE, precision_type);
        func(value, dim, temp);
        value = temp;
    }

    if (!is_aligned(value)) {
        char *temp = alloc_vector(vec_size);
        memcpy(temp, value, vec_size);
        value = temp;
    }

    const uint16 m = metap->m;
    const size_t num_vectors = metap->num_vectors;
    const QuantizerMetaInfo &qt_metainfo = metap->quantizer_metainfo;
    const uint32 num_new_data = qt_metainfo.num_new_data;
    QuantizerParam qt_param;

    qt_param.set_type(qt_metainfo.get_type(), qt_metainfo.get_setting_type());
    QuantizerType setting_type = qt_metainfo.get_setting_type();
    QtUpdateMgr *qt_update_mgr = (QtUpdateMgr *)g_instance.annvec_cxt.qt_update_mgr;
    TimeRing *timering = NULL;
    if (!building && setting_type != QuantizerType::NONE) {
        timering = qt_update_mgr->find_timering(index->rd_id);
        if (timering) {
            timering->visit();
        } else {
            timering = qt_update_mgr->insert_timgring(index->rd_id);
        }
    }

    bool condition_numvectors = num_vectors >= HNSW_MIN_QT_SAMPLES_SIZE;
    bool condition_enbaling = (qt_metainfo.get_type() != qt_metainfo.get_setting_type());
    bool condition_numnewdata = num_new_data > (num_vectors - num_new_data) * 0.3;
    bool need_update = condition_numvectors && (condition_enbaling || condition_numnewdata);
    if (!building && need_update) {
        if (!qt_update_mgr->contain_updating(index->rd_id)) {
            /* launch a backgroud thread */
            float freq_10min = timering->get_all() / 10;
            float factor = (float)num_new_data / num_vectors;
            if (freq_10min / (freq_10min + 100) > factor) {
                goto exit_update;
            }
            bool success = qt_update_mgr->insert_updating(index->rd_id);
            if (!success) {
                goto exit_update;
            }
            QuantizerUpdateParam param;
            param.qt_type = setting_type;
            param.enable = !condition_enbaling;
            param.metablkno = metablkno;
            param.freq_10min = freq_10min;
            param.force = false;
            add_quantizer_update_task(index, &param);
            QT_UPDATE_LOG("index \"%s\" insert or update freq in the last 10 minutes = %f,"
                            " num_new_data: %u, num_vectors: %lu,"
                            " factor: num_new_data/num_vectors = %f,"
                            " freq / (freq + 100) < factor, add quantizer update task", 
                            RelationGetRelationName(index), freq_10min, num_new_data, num_vectors, factor);
        }
    }
exit_update:

    /* Create an tuple */
    const size_t ncluster = (size_t)HnswGetNumCluster(index);
    const bool as_bin = ncluster > 0 && metap->num_vectors > ncluster;
    HnswTuple tuple = HnswInitTuple(heap_tid, as_bin, m, floatvectorIndex);

    /* wait vacuum to operate atomically */
    LockPage(index, metablkno, ShareLock);
    LOCKMODE lmode = ShareLock;
    if (!BlockNumberIsValid(metap->entryBlkno) || tuple->level > metap->entryLevel) {
        UnlockPage(index, metablkno, ShareLock);
        lmode = ExclusiveLock;
        LockPage(index, metablkno, ExclusiveLock);
    }
    qt_param.set_resource(index, metap, value, building);

    /* Find neighbors for element */
    if(!HnswFindElementNeighborsonDisk(index, heap, value, tuple, metabuf, metap, NULL, building, qt_param)) {
        float *qt_value = (float *)value;
        float *half2float = NULL;
        if (qt_param.get_type() != QuantizerType::NONE && precision_type == DistPrecisionType::HALF) {
            half2float = alloc_floatvector(dim);
            halfs_to_floats((half *)value, half2float, dim);
            qt_value = half2float;
        }
        if (qt_param.get_type() == QuantizerType::PQ) {
            PQParam &pq_param = qt_param.get_pq_param();
            pq_param.code = (char *)palloc(sizeof(char) * pq_param.code_len);
            pq_param.pq->compute_code(qt_value, (uint8 *)pq_param.code);
            UpdateGraphOnDisk(index, heap, tuple, value, metabuf, metapage, building, qt_param);
        } else if (qt_param.get_type() == QuantizerType::RABITQ) {
            RaBitQParam &rbq_param = qt_param.get_rabitq_param();
            rbq_param.quant_data = (char *)palloc0(rbq_param.rbq_meta.quant_size);
            QuantizeRaBitQ(rbq_param, qt_value, rbq_param.quant_data);
            UpdateGraphOnDisk(index, heap, tuple, value, metabuf, metapage, building, qt_param);
            pfree(rbq_param.quant_data);
        } else {
            UpdateGraphOnDisk(index, heap, tuple, value, metabuf, metapage, building, qt_param);
        }
        if (half2float) {
            free_vector(half2float);
        }
    }
    UnlockPage(index, metablkno, lmode);
    ReleaseBuffer(metabuf);

    qt_param.release_resource();

    if (value != v) {
        free_vector(value);
    }

    if (vec_p != DatumGetPointer(values[0])) {
        pfree(vec_p);
    }

    return true;
}

static bool HnswInsertTuple(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno, size_t floatvectoriIndex)
{
    bool res = HnswInsertTupleOnDisk(index, heap, values, isnull, heap_tid, false, floatvectoriIndex, metablkno);
    return res;
}

bool hnswinsert_internal(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno, size_t floatvectoriIndex)
{
    if (isnull[0]) {
        return false;
    }

    MemoryContext insertCtx = AllocSetContextCreate(CurrentMemoryContext,
        "Hnsw insert temporary context", ALLOCSET_DEFAULT_SIZES);
    MemoryContext oldCtx = MemoryContextSwitchTo(insertCtx);

    bool res = HnswInsertTuple(index, heap, values, isnull, heap_tid, metablkno, floatvectoriIndex);

    MemoryContextSwitchTo(oldCtx);
    MemoryContextDelete(insertCtx);
    return res;
}
