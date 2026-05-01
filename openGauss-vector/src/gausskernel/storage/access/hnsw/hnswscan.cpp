#include <algorithm>    /* sort, nth_element */

#include <vtl/hashtable>
#include <vtl/optional>

#include "postgres.h"
#include "access/relscan.h"
#include "access/hnsw/hnsw.h"
#include "pgstat.h"
#include "storage/buf/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/fmgroids.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/store/buffer_manager.h"

using namespace ann_helper;

struct HnswScanOpaqueData {
    bool first;
    bool has_more_data;
    Optional<UnorderedSet<ItemPointerData>> returned;
    int tid_offset;
    int tid_count;
    Vector<HnswDiskCandidate *> candidates;
    ItemPointer heaptids;
    float *dists;
    MemoryContext tmpCtx;
};
typedef HnswScanOpaqueData *HnswScanOpaque;

/*
 * Algorithm 5 from paper
 */
static void GetScanItems(Relation index, Relation heap, char *query, int ef,
    Vector<HnswDiskCandidate *> &ep, Buffer meta_buf, HnswMetaPage metap, QuantizerParam &qt_param)
{
    const int m = metap->m;
    const uint32 dim = metap->dimensions;
    const DistPrecisionType precision_type = metap->precision_type;
    distance_func dist_func = hnsw_get_aligned_distance_func(index, metap->metric, dim,
        precision_type, qt_param.get_type() == QuantizerType::PQ);
    if (ep.empty()) {
        LockBuffer(meta_buf, BUFFER_LOCK_SHARE);

        const BlockNumber entry_blkno = metap->entryBlkno;
        const OffsetNumber entry_offno =  metap->entryOffno;
        LockBuffer(meta_buf, BUFFER_LOCK_UNLOCK);
        if (!BlockNumberIsValid(entry_blkno)) {
            return;
        }
        uint8 entryLevel;
        HnswDiskCandidate* hc = GetDiskCandiateByTuple(index, heap, entry_blkno, entry_offno,
                                                       query, dim, dist_func, entryLevel, qt_param, precision_type);
        ep.push_back(hc);
        for (uint8 lc = entryLevel; lc >= 1u; --lc) {
            HnswSearchLayerDisk(index, heap, query, ep, 1, m, dim, lc, dist_func, NULL, qt_param, precision_type);
        }
    }
    HnswSearchLayerDisk(index, heap, query, ep, ef, m, dim, 0, dist_func, NULL, qt_param, precision_type);
}

static Datum GetScanValue(IndexScanDesc scan)
{
    Datum value;
    if (scan->orderByData->sk_flags & SK_ISNULL) {
        value = PointerGetDatum(NULL);
    } else {
        value = scan->orderByData->sk_argument;

        /* Value should not be compressed or toasted */
        Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
        Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));
    }

    return value;
}

void *create_hnsw_scanopaque(Relation index)
{
    HnswScanOpaque so = (HnswScanOpaque)palloc(sizeof(HnswScanOpaqueData));
    so->first = true;
    new (&so->returned) decltype(so->returned); /* no memory usage here */
    so->tid_offset = 0;
    so->tid_count = 0;
    so->heaptids = NULL;
    so->dists = NULL;
    so->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
        "Hnsw scan temporary context", ALLOCSET_DEFAULT_SIZES);
    return so;
}

IndexScanDesc hnswbeginscan_internal(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    scan->opaque = create_hnsw_scanopaque(index);
    return scan;
}

void hnswrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    HnswScanOpaque so = (HnswScanOpaque)scan->opaque;
    so->first = true;
    optional_destroy(so->returned);
    so->tid_offset = 0;
    so->tid_count = 0;
    MemoryContextReset(so->tmpCtx);

    if (keys && scan->numberOfKeys > 0) {
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }
    if (orderbys && scan->numberOfOrderBys > 0) {
        memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
    }
}

bool hnswgettuple_internal(IndexScanDesc scan, void *in_so, BlockNumber metablkno, int ef, float *dist_out)
{
    Relation index = scan->indexRelation;
    Relation heap = scan->heapRelation;
    HnswScanOpaque so = (HnswScanOpaque)in_so;
    if (so->first) {
        /* Count index scan for stats */
        pgstat_count_index_scan(index);

        if (scan->orderByData == NULL) {
            elog(ERROR, "cannot scan hnsw index without order");
        }
        if (scan->orderByData->sk_flags & SK_ISNULL) {
            return false;
        }
        /* Requires MVCC-compliant snapshot as not able to maintain a pin */
        /* https://www.postgresql.org/docs/current/index-locking.html */
        if (!IsMVCCSnapshot(scan->xs_snapshot)) {
            elog(ERROR, "non-MVCC snapshots are not supported with hnsw");
        }

retry:
        MemoryContext oldCtx = MemoryContextSwitchTo(so->tmpCtx);
        Buffer meta_buf = ReadBuffer(index, metablkno);
        HnswMetaPage metap = HnswPageGetMeta(BufferGetPage(meta_buf));
        uint32 dim = metap->dimensions;
        bool cluster_pq = metap->cluster.cluster_pq;
        QuantizerMetaInfo qt_metainfo = metap->quantizer_metainfo;

        Datum value = GetScanValue(scan);
        Pointer vec_p = NULL;
        char *v = DatumGetVector(value, metap->precision_type, &vec_p);
        if (uint16(((FloatVector *)vec_p)->dim) != dim) {
            elog(ERROR, "incorrect dimension of query vector");
        }
        const bool use_cluster = HnswUseCluster(index);

        char *query = v;
        bool alloced = false;
        const size_t vec_size = dim * VEC_ELEM_SIZE(metap->precision_type);
        if (!is_aligned(query)) {
            char *temp = alloc_vector(vec_size);
            errno_t rc = memcpy_s(temp, vec_size, query, vec_size);
            securec_check(rc, "\0", "\0");
            query = temp;
            alloced = true;
        }

        if (use_cluster) {
            ef = u_sess->attr.attr_storage.ef_search;
        }
        size_t refine_ndata = 0;
        if (cluster_pq) {
            if (!scan->with_limit) {
                cluster_pq = false;
            } else {
                refine_ndata =
                    size_t(scan->limit_count) * u_sess->attr.attr_storage.ivfpq_refine_k_factor;
            }
        }

        if (so->dists && so->heaptids) {
            if (!so->returned.has_value()) {
                so->returned.emplace(so->tid_count);
            }
            for (int i = 0; i < so->tid_count; ++i) {
                so->returned->insert(so->heaptids[i]);
            }
            pfree(so->dists);
            pfree(so->heaptids);
        } else {
            new (&so->candidates) decltype(so->candidates)(ef);
        }
        
        QuantizerParam qt_param;
        qt_param.set_type(qt_metainfo.get_type(), qt_metainfo.get_setting_type());
        qt_param.set_resource(index, metap, query);
        GetScanItems(index, heap, query, ef, so->candidates, meta_buf, metap, qt_param);
        qt_param.release_resource();

        int len = (int)so->candidates.size();
        const auto gather_res = [&](auto &&gather_func) {
            for (int i = len - 1; i >= 0; --i) {
                HnswDiskCandidate *hc = so->candidates[i];
                Buffer buf = ReadBuffer(index, ItemPointerGetBlockNumber(&hc->indexTid));
                LockBuffer(buf, BUFFER_LOCK_SHARE);
                Page page = BufferGetPage(buf);
                HnswTuple tuple = (HnswTuple)PageGetItem(page,
                    PageGetItemId(page, ItemPointerGetOffsetNumber(&hc->indexTid)));
                gather_func(tuple, hc->distance);
                UnlockReleaseBuffer(buf);
            }
        };
        Vector<float> dists;
        if (use_cluster) {  /* not maintained */
            Vector<ItemPointerData> tids;
            disk_container::PlainStore ps(index, HNSW_PS_BLKNO, false);
            Metric order_metric =
                scan->orderByData->sk_func.fn_oid == F_COSINE_DISTANCE ? COSINE : metap->metric;
            distance_func_batch2 batch2_func =
                get_aligned_distance_batch_func2(order_metric, dim);
            if (!cluster_pq) {
                Vector<float *> vec_holder;
                gather_res([&](HnswTuple tuple, float distance) {
                    tuple->get_tid_dists(scan->heapRelation, index, dists, tids,
                        (float *)query, vec_holder, dim, ps, batch2_func);
                });
                if (!vec_holder.empty()) {
                    free_vector(vec_holder[0]);
                }
                optional_destroy(vec_holder);
            }
            ps.destroy();
            struct VecPair { ItemPointerData tid; float dist; };
            /* TD: use range to sort tids directly */
            const size_t res_size = tids.size();
            Vector<VecPair> temp(res_size);
            for (size_t i = 0; i < res_size; ++i) {
                temp.push_back({tids[i], dists[i]});
            }

            if (!cluster_pq) {
                optional_destroy(tids);
                optional_destroy(dists);
                std::sort(temp.begin(), temp.end(), [](const VecPair &a, const VecPair &b) {
                    return a.dist < b.dist;
                });
                so->tid_count = res_size;
                so->heaptids = (ItemPointer)palloc(sizeof(ItemPointerData) * res_size);
                for (size_t i = 0; i < res_size; ++i) {
                    so->heaptids[i] = temp[i].tid;
                }
            } else {
                std::nth_element(temp.begin(), temp.at(refine_ndata), temp.end(),
                    [](const VecPair &a, const VecPair &b) {
                    return a.dist < b.dist;
                });
                size_t nsize = std::min(res_size, refine_ndata);
                for (uint32 i = 0; i < nsize; ++i) {
                    tids[i] = temp[i].tid;
                }
                float **vecs = (float **)palloc(sizeof(float *) * nsize);
                uint32 adim = get_aligned_dim(dim);
                float *vec_buf = alloc_floatvector(adim, nsize);
                for (size_t i = 0; i < nsize; ++i) {
                    vecs[i] = vec_buf + i * adim;
                }
                nsize = hnsw_get_vector(vecs, scan->heapRelation, index, tids.data(), nsize, dim);
                batch2_func((float *)query, (void *const *)vecs, dim, nsize, dists.data());
                pfree(vecs);
                free_vector(vec_buf);
                temp.clear();
                for (size_t i = 0; i < nsize; ++i) {
                    temp.push_back({tids[i], dists[i]});
                }
                optional_destroy(tids);
                optional_destroy(dists);
                std::sort(temp.begin(), temp.end(), [](const VecPair &a, const VecPair &b) {
                    return a.dist < b.dist;
                });
                so->heaptids = (ItemPointer)palloc(sizeof(ItemPointerData) * nsize);
                for (size_t i = 0; i < nsize; ++i) {
                    so->heaptids[i] = temp[i].tid;
                }
                so->tid_count = nsize;
            }
            optional_destroy(temp);
        } else {
            Vector<ItemPointerData> res;
            gather_res([&](HnswTuple tuple, float distance) {
                uint8 ntid = tuple->ntids();
                res.push_back(tuple->heaptids, tuple->heaptids + ntid);
                for (uint32 j = 0; j < ntid; ++j) {
                    dists.push_back(distance);
                }
            });
            so->heaptids = res.data();
            so->tid_count = res.size();
            Assert(res.size() == dists.size());
            so->dists = dists.data();
        }

        ReleaseBuffer(meta_buf);

        if (alloced) {
            free_vector(query);
        }

        if (vec_p != DatumGetPointer(value)) {
            pfree(vec_p);
        }

        so->first = false;
        so->has_more_data = len >= ef;
        MemoryContextSwitchTo(oldCtx);
    }

retry2:
    if (so->tid_offset >= so->tid_count) {
        if (!so->has_more_data || RelationIsPartition(index)) {
            return false;
        }
        ef = std::max(so->candidates.size() * 2ul, 25ul);
        so->tid_offset = 0;
        goto retry;
    }
    int offset = so->tid_offset++;
    if (so->returned.has_value() && so->returned->contains(so->heaptids[offset])) {
        goto retry2;
    }
    scan->xs_ctup.t_self = so->heaptids[offset];
    if (dist_out) {
        *dist_out = so->dists[offset];
    }
    return true;
}

void free_hnsw_scanopaque(void *in_so)
{
    HnswScanOpaque so = (HnswScanOpaque)in_so;
    MemoryContextDelete(so->tmpCtx);
    pfree(so);
}

void hnswendscan_internal(IndexScanDesc scan)
{
    HnswScanOpaque so = (HnswScanOpaque)scan->opaque;
    free_hnsw_scanopaque(so);
    scan->opaque = NULL;
}
