#include <math.h>
#include <memory>

#include "postgres.h"
#include "access/tableam.h"
#include "storage/buf/bufmgr.h"
#include "lib/pairingheap.h"
#include "utils/rel.h"
#include "utils/datum.h"
#include "utils/hashutils.h"

#include "access/hnsw/hnsw_xlog.h"
#include "access/annvector/floatvector.h"
#include "access/diskann/diskann_cache.h"
#include "access/annvector/store/buffer_manager.h"
#include "access/rabitq/estimator.h"
#include "access/annvector/ann_utils.h"

using namespace ann_helper;
using namespace rabitq;

int HnswGetM(Relation index)
{
    HnswOptions *opts = (HnswOptions *)index->rd_options;
    return opts ? opts->m : HNSW_DEFAULT_M;
}

int HnswGetEfConstruction(Relation index)
{
    HnswOptions *opts = (HnswOptions *)index->rd_options;
    return opts ? opts->efConstruction : HNSW_DEFAULT_EF_CONSTRUCTION;
}

int HnswGetBuildParallel(Relation index)
{
    HnswOptions *opts = (HnswOptions *)index->rd_options;
    return opts ? opts->parallel_workers : 0;
}

QuantizerType HnswGetQuantizerType(Relation index)
{
    HnswOptions *opts = (HnswOptions *)index->rd_options;
    return opts != NULL && opts->qt_type_offset > 0 ?
        extract_qt((const char *)opts + opts->qt_type_offset) :
        (QuantizerType)HNSW_DEFAULT_QUANTIZER_TYPE;
}

int64 HnswGetNumCluster(Relation index)
{
    if (index->rd_am->ambuild != F_HNSWBUILD || !index->rd_options) {
        return 0;
    }
    return ((HnswOptions *)index->rd_options)->num_cluster;
}

bool HnswUseCluster(Relation index)
{
    return HnswGetNumCluster(index) > 0;
}

FmgrInfo *HnswOptionalProcInfo(Relation rel, uint16 procnum)
{
    if (!OidIsValid(index_getprocid(rel, 1, procnum)))
        return NULL;

    return index_getprocinfo(rel, 1, procnum);
}

Buffer HnswNewBuffer(Relation index, ForkNumber forkNum, bool needlock)
{
    if (needlock) {
        LockRelationForExtension(index, ExclusiveLock);
    }
    Buffer buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
    if (needlock) {
        UnlockRelationForExtension(index, ExclusiveLock);
    }
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

Buffer HnswLoadBufferExtended(Relation index, ForkNumber forkNum, BlockNumber blkNo)
{
    Buffer buf = ReadBufferExtended(index, forkNum, blkNo, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

void HnswCommitBuffer(Buffer buf)
{
    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);
}

void HnswInitPage(Buffer buf, Page page)
{
    PageInit(page, BufferGetPageSize(buf), sizeof(HnswPageOpaqueData));
    HnswPageGetOpaque(page)->nextblkno = InvalidBlockNumber;
    HnswPageGetOpaque(page)->page_id = HNSW_PAGE_ID;
}

static HnswNeighborArray *HnswInitNeighborArray(int lm, HnswAllocator *allocator)
{
    HnswNeighborArray *a = (HnswNeighborArray *)HnswAlloc(allocator, HNSW_NEIGHBOR_ARRAY_SIZE(lm));
    a->length = 0;
    a->closerSet = false;
    return a;
}

static void HnswInitNeighbors(HnswElement element, int m, HnswAllocator *allocator)
{
    int level = element->level;
    HnswNeighborArray **neighborList =
        (HnswNeighborArray **)HnswAlloc(allocator, sizeof(HnswNeighborArray *) * (level + 1));
    element->neighbors = neighborList;
    for (int lc = 0; lc <= level; ++lc) {
        neighborList[lc] = HnswInitNeighborArray(HnswGetLayerM(m, lc), allocator);
    }
}

void *HnswAlloc(HnswAllocator *allocator, Size size)
{
    Assert(allocator);
    return (*(allocator)->alloc)(size, (allocator)->state);
}

void *HnswAlignedAlloc(HnswAllocator * allocator, Size size, Size align)
{
    Assert(allocator);
    return (*(allocator)->align_alloc)(size, align, (allocator)->state);
}

HnswElement HnswInitElement(ItemPointer heaptid, int m, double ml, int maxLevel,
    HnswElement bin_elem, HnswAllocator *allocator, size_t floatvectorIndex)
{
    HnswElement element;
    int level;
    if (bin_elem) {
        element = bin_elem;
        level = 0;
        element->floatVectorIndex = floatvectorIndex;
    } else {
        element = (HnswElement)HnswAlignedAlloc(allocator, sizeof(HnswElementData), HnswElementDataAlignment);
        level = (int)(-log(RandomDouble()) * ml);
        if (level > maxLevel) {
            level = maxLevel;
        }
        element->floatVectorIndex = floatvectorIndex;
    }
    element->level = level;

    element->init();
    element->set_ntids(1u);
    element->heaptids[0] = *heaptid;

    if (!bin_elem || !element->neighbors) {
        HnswInitNeighbors(element, m, allocator);
    } else {
        element->neighbors[0]->length = 0;
        element->neighbors[0]->closerSet = false;
    }

    element->value = NULL;
    return element;
}

HnswTuple HnswInitTuple(ItemPointer heaptid, bool as_bin_elem, int m, size_t floatvectorInndex)
{
    uint8 level;
    if (as_bin_elem) {
        level = 0;
    } else {
        double ml = HnswGetMl(m);
        int max_level = HnswGetMaxLevel(m);
        level = (uint8)(-log(RandomDouble()) * ml);
        /* Cap level */
        if (level > max_level) {
            level = max_level;
        }
    }

    /* Prepare element tuple */
    HnswTuple tuple = (HnswTuple)palloc0(HNSW_TUPLE_SIZE(level, m));
    tuple->init();
    tuple->heaptids[0] = *heaptid;
    tuple->set_ntids(1u);
    tuple->level = level;
    tuple->floatVectorIndex = as_bin_elem ? NonCenterVectorIndex : floatvectorInndex;
    for (int i = 0; i < (level + 2) * m; ++i) {
        ItemPointerSetInvalid(&tuple->neighbors[i].indexTid);
        tuple->neighbors[i].floatVectorIndex = InvalidVectorIndex;
    }
    return tuple;
}

/*
 * Set element tuple, except for neighbor info
 */
void HnswSetElementTuple(HnswTuple tuple, HnswElement element)
{
    tuple->level = element->level;
    tuple->flag = element->flag;
    uint8 ntid = element->ntids();
    for (uint8 i = 0; i < ntid; ++i) {
        tuple->heaptids[i] = element->heaptids[i];
    }
    tuple->set_ntids(ntid);
}

void HnswSetElementVector(Relation index, size_t idx, size_t vec_size, char *value, bool building, bool with_qtcode)
{
    write_vector(index, idx, vec_size, value, with_qtcode);
    if (RelationNeedsWAL(index) && !building) {
        with_qtcode ? 
            HnswXLogAddVector(index, value, idx * vec_size, vec_size, VecStorageType::VecWithCode) :
            HnswXLogAddVector(index, value, idx * vec_size, vec_size, VecStorageType::PureVec);
    }
}

void HnswSetElementQTCode(Relation index, size_t idx, size_t code_len, char *code, bool building, bool with_vector)
{
    write_qtcode(index, idx, code_len, code, with_vector);
    if (RelationNeedsWAL(index) && !building) {
        with_vector ?
            HnswXLogAddVector(index, (char *)code, idx * code_len, code_len, VecStorageType::CodeWithVec) :
            HnswXLogAddVector(index, (char *)code, idx * code_len, code_len, VecStorageType::PureCode);
    }
}

void HnswXLogBuildAddVector(Relation heap, Relation index, size_t vec_size, ForkNumber forkNum, BlockNumber metablkno,
    RaBitQParam &rbq_param)
{
    if (!heap || !RelationNeedsWAL(index)) {
        return;
    }

    Buffer metaBuf = ReadBufferExtended(index, forkNum, metablkno, RBM_NORMAL, NULL);
    LockBuffer(metaBuf, BUFFER_LOCK_SHARE);
    Page metaPage = BufferGetPage(metaBuf);
    HnswMetaPage metap = HnswPageGetMeta(metaPage);
    size_t num_vectors  = metap->num_vectors;
    QuantizerType qt_type = metap->quantizer_metainfo.get_type();
    UnlockReleaseBuffer(metaBuf);

    if (!num_vectors) {
        return;
    }

    constexpr size_t wal_block_size = 25'600'000ul;
    const size_t waL_vec_num = wal_block_size / vec_size;

    if (qt_type == QuantizerType::PQ) {
        /* wal for PQ */
        const HnswPQMetaInfo &pq_metainfo = metap->quantizer_metainfo.get_pq_metainfo();
        const size_t code_len = pq_metainfo.code_size();
        const size_t block_size_pq = wal_block_size / code_len;
        char *value_pq = (char *)palloc(block_size_pq * code_len);
        for (size_t i = 0; i < num_vectors; i += block_size_pq) {
            size_t block = std::min(num_vectors - i, block_size_pq);
            vec_read(index->rd_smgr, i * code_len, block * code_len, value_pq, VecStorageType::PureCode);
            HnswXLogAddVector(index, value_pq, i * code_len, block * code_len, VecStorageType::PureCode);
        }
        pfree(value_pq);
    } else if (qt_type == QuantizerType::RABITQ) {
        /* wal for vectors with RaBitQ if kept */
        if (rbq_param.rbq_meta.keep_vecs) {
            char *value = (char *)palloc(waL_vec_num * vec_size);
            for (size_t i = 0; i < num_vectors; i += waL_vec_num) {
                size_t block = std::min(num_vectors - i, waL_vec_num);
                vec_read(index->rd_smgr, i * vec_size, block * vec_size, value, VecStorageType::VecWithCode);
                HnswXLogAddVector(index, value, i * vec_size, block * vec_size, VecStorageType::VecWithCode);
            }
            pfree(value);
        }
        /* wal for RaBitQ codes */
        const size_t code_len = rbq_param.rbq_meta.quant_size;
        VecStorageType vec_storage_type = rbq_param.rbq_meta.keep_vecs ? VecStorageType::CodeWithVec : VecStorageType::PureCode;
        const size_t block_size_rbq = wal_block_size / code_len;
        char *value_rbq = (char *)palloc(block_size_rbq * code_len);
        for (size_t i = 0; i < num_vectors; i += block_size_rbq) {
            size_t block = std::min(num_vectors - i, block_size_rbq);
            vec_read(index->rd_smgr, i * code_len, block * code_len, value_rbq, vec_storage_type);
            HnswXLogAddVector(index, value_rbq, i * code_len, block * code_len, vec_storage_type);
        }
        pfree(value_rbq);
    } else {
        char *value = (char *)palloc(waL_vec_num * vec_size);
        /* wal for vectors without quantization */
        for (size_t i = 0; i < num_vectors; i += waL_vec_num) {
            size_t block = std::min(num_vectors - i, waL_vec_num);
            vec_read(index->rd_smgr, i * vec_size, block * vec_size, value, VecStorageType::PureVec);
            HnswXLogAddVector(index, value, i * vec_size, block * vec_size, VecStorageType::PureVec);
        }
        pfree(value);
    }
}

/*
 * Write neighborsdata
 */
void HnswSetNeighborsData(HnswTuple tuple, HnswElement e, int m)
{
    int idx = 0;
    for (int lc = e->level; lc >= 0; --lc) {
        HnswNeighborArray *neighbors = HnswGetNeighbors(e, lc);
        int lm = HnswGetLayerM(m, lc);
        for (int i = 0; i < lm; ++i) {
            HnswNeighbor neighbor = &tuple->neighbors[idx++];
            if (i < neighbors->length) {
                HnswCandidate *hc = &neighbors->items[i];
                HnswElement hce = (HnswElement)hc->element;
                ItemPointerSet(&neighbor->indexTid, hce->blkno, hce->offno);
                neighbor->floatVectorIndex = hce->floatVectorIndex;
            } else {
                ItemPointerSetInvalid(&neighbor->indexTid);
                neighbor->floatVectorIndex = InvalidVectorIndex;
            }
        }
    }
}

HnswCandidate * HnswEntryCandidate(HnswElement entryPoint, const char *query,
    distance_func dist_func, uint32 dim)
{
    HnswCandidate *hc = (HnswCandidate *)palloc(sizeof(HnswCandidate));
    hc->element = entryPoint;
    hc->distance = dist_func(query, hc->element->value, dim);
    return hc;
}

static int CompareNearestCandidates(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
    const HnswPairingHeapNode *aa = (const HnswPairingHeapNode *)a;
    const HnswPairingHeapNode *bb = (const HnswPairingHeapNode *)b;
    if (aa->inner->distance < bb->inner->distance) {
        return 1;
    }
    if (aa->inner->distance > bb->inner->distance) {
        return -1;
    }
    return 0;
}

static int CompareNearestDiskCandidates(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
    const HnswDiskPairingHeapNode *aa = (const HnswDiskPairingHeapNode *)a;
    const HnswDiskPairingHeapNode *bb = (const HnswDiskPairingHeapNode *)b;
    if (aa->inner->distance < bb->inner->distance) {
        return 1;
    }
    if (aa->inner->distance > bb->inner->distance) {
        return -1;
    }
    return 0;
}

static int CompareFurthestCandidates(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
    return -CompareNearestCandidates(a, b, arg);
}

static int CompareFurthestDiskCandidates(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
    return -CompareNearestDiskCandidates(a, b, arg);
}

static HnswPairingHeapNode *CreatePairingHeapNode(HnswCandidate *c)
{
    HnswPairingHeapNode *node = (HnswPairingHeapNode *)palloc(sizeof(HnswPairingHeapNode));
    node->inner = c;
    return node;
}

static HnswDiskPairingHeapNode *CreatePairingHeapNode(HnswDiskCandidate *c)
{
    HnswDiskPairingHeapNode *node = (HnswDiskPairingHeapNode *)palloc(sizeof(HnswDiskPairingHeapNode));
    node->inner = c;
    return node;
}

/*
 * Algorithm 2 from paper
 */
static void HnswSearchLayer(const char *query, distance_func dist_func,
    Vector<HnswCandidate *> &cand, int ef, int m, uint32 dim, uint8 lc)
{
    pairingheap *C = pairingheap_allocate(CompareNearestCandidates, NULL);
    pairingheap *W = pairingheap_allocate(CompareFurthestCandidates, NULL);
    Size neighborhoodSize = HNSW_NEIGHBOR_ARRAY_SIZE(HnswGetLayerM(m, lc));
    HnswNeighborArray *neighborhoodData = (HnswNeighborArray *)palloc(neighborhoodSize);
    UnorderedSet<size_t> v(ef * m * 2);
    /* Add entry points to v, C, and W */
    for (HnswCandidate *hc : cand) {
        v.insert((size_t)hc->element);
        pairingheap_add(C, &(CreatePairingHeapNode(hc)->ph_node));
        pairingheap_add(W, &(CreatePairingHeapNode(hc)->ph_node));
    }
    int wlen = (int)cand.size();
    cand.clear();

    while (!pairingheap_is_empty(C)) {
        HnswCandidate *c = ((HnswPairingHeapNode *) pairingheap_remove_first(C))->inner;
        HnswCandidate *f = ((HnswPairingHeapNode *) pairingheap_first(W))->inner;
        if (c->distance > f->distance) {
            break;
        }

        HnswElement cElement = c->element;
        HnswNeighborArray *neighborhood = HnswGetNeighbors(cElement, lc);
        /* Copy neighborhood to local memory if needed */
        LWLockAcquire(&cElement->lock, LW_SHARED);
        memcpy(neighborhoodData, neighborhood, neighborhoodSize);
        LWLockRelease(&cElement->lock);
        neighborhood = neighborhoodData;

        for (int i = 0; i < neighborhood->length; i++) {
            HnswCandidate *e = &neighborhood->items[i];
            if (!v.insert((size_t)e->element).second) {
                continue;
            }
            HnswElement eElement = e->element;
            f = ((HnswPairingHeapNode *)pairingheap_first(W))->inner;
            float eDistance = dist_func(query, eElement->value, dim);
            if (eDistance >= f->distance && wlen >= ef) {
                continue;
            }
            HnswCandidate *ec = (HnswCandidate *)palloc(sizeof(HnswCandidate));
            ec->element = eElement;
            ec->distance = eDistance;

            pairingheap_add(C, &(CreatePairingHeapNode(ec)->ph_node));
            pairingheap_add(W, &(CreatePairingHeapNode(ec)->ph_node));
            ++wlen;
            /* No need to decrement wlen */
            if (wlen > ef) {
                pairingheap_remove_first(W);
            }
        }
    }
    optional_destroy(v);

    /* Add each element of W to w */
    while (!pairingheap_is_empty(W)) {
        HnswCandidate *hc = ((HnswPairingHeapNode *)pairingheap_remove_first(W))->inner;
        cand.push_back(hc);
    }
}

VecStorageType GetVecStorageType(QuantizerParam &qt_param, bool for_vector)
{
    switch (qt_param.get_type()) {
        case QuantizerType::PQ: {
            return VecStorageType::PureCode;
        } break;
        case QuantizerType::RABITQ: {
            RaBitQParam &rbq_param = qt_param.get_rabitq_param();
            if (rbq_param.rbq_meta.keep_vecs) {
                return for_vector ? VecStorageType::VecWithCode : VecStorageType::CodeWithVec;
            } else {
                return VecStorageType::PureCode;
            }
        } break;
        default: {
            return VecStorageType::PureVec;
        } break;
    }
    return VecStorageType::PureVec;
}

VecStorageType GetVecStorageType(bool graph_pq, RaBitQParam &rbq_param, bool for_vector)
{
    if (graph_pq) {
        return for_vector ? VecStorageType::PureVec : VecStorageType::PureCode;
    }
    if (rbq_param.rbq_meta.enabled) {
        if (rbq_param.rbq_meta.keep_vecs) {
            return for_vector ? VecStorageType::VecWithCode : VecStorageType::CodeWithVec;
        } else {
            return VecStorageType::PureCode;
        }
    }
    return VecStorageType::PureVec;
}

static void get_bin_dist(RaBitQEstimator *estimator, char *quant_data, RaBitQParam &rbq_param,
    EstimateRecord &est)
{
    uint16 cluster_id = *((uint16 *)quant_data);
    char *bin_data = quant_data + rbq_param.cid_size;
    estimator->get_bin_dist(cluster_id, bin_data, est);
}

static void get_full_dist(RaBitQEstimator *estimator, char *quant_data, RaBitQParam &rbq_param,
    EstimateRecord &est)
{
    uint16 cluster_id = *((uint16 *)quant_data);
    char *bin_data = quant_data + rbq_param.cid_size;
    char *ext_data = bin_data + rbq_param.bin_size;
    estimator->get_full_dist(cluster_id, bin_data, ext_data, est);
}

HnswDiskCandidate *GetDiskCandiateByTuple(Relation index, Relation heap, BlockNumber blkno, OffsetNumber offno,
    const char *query, uint32 dim, distance_func dist_func, uint8 &level, QuantizerParam &qt_param,
    DistPrecisionType precision_type)
{
    HnswDiskCandidate* hc = (HnswDiskCandidate *)palloc(sizeof(HnswDiskCandidate));
    Buffer buf = ReadBuffer(index, blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buf);
    HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
    
    ItemPointerSet(&hc->indexTid, blkno, offno);
    
    VecStorageType vst = GetVecStorageType(qt_param, true);
    VecStorageType cst = GetVecStorageType(qt_param, false);
    size_t idx = tuple->floatVectorIndex;
    BulkBuffer *bulkbuf = qt_param.bulkbuf;
    const size_t vec_size = dim * VEC_ELEM_SIZE(precision_type);

    if (qt_param.get_type() == QuantizerType::PQ) {
        char *value = alloc_vector(vec_size);
        if (fetch_vec_from_heap(index, heap, tuple->heaptids[0], value, dim, precision_type)) {
            hc->distance = dist_func(query, value, dim);
            free_vector(value);
        } else {
            PQParam &pq_param = qt_param.get_pq_param();
            free_vector(value);
            if (bulkbuf) {
                uint8 *code = (uint8 *)bulkbuf->get(idx);
                hc->distance = pq_param.pq->distance_to_code(code, pq_param.dist_table);
            } else {
                VecBuffer pq_buf = vec_read_quant(index, idx, pq_param.code_len, VecStorageType::PureCode);
                hc->distance = pq_param.pq->distance_to_code((uint8 *)pq_buf.get_vecbuf(), pq_param.dist_table);
                pq_buf.release();
            }
        }
    } else if (qt_param.get_type() == QuantizerType::RABITQ) {
        RaBitQParam &rbq_param = qt_param.get_rabitq_param();
        if (rbq_param.applied) {
            EstimateRecord est;
            RaBitQEstimator *estimator = reinterpret_cast<RaBitQEstimator *>(rbq_param.estimator);
            if (bulkbuf) {
                char *quant_data = bulkbuf->get(idx);
                get_full_dist(estimator, quant_data, rbq_param, est);
            } else {
                int quant_size = rbq_param.rbq_meta.quant_size;
                VecBuffer buf = vec_read_quant(index, idx, quant_size, cst);
                get_full_dist(estimator, buf.get_vecbuf(), rbq_param, est);
                buf.release();
            }
            hc->distance = est.est_dist;
        } else if (rbq_param.rbq_meta.keep_vecs) {
            VecBuffer vec_buf = vec_read_buffer(index, idx, vec_size, vst);
            hc->distance = dist_func(query, vec_buf.get_vecbuf(), dim);
            vec_buf.release();
        } else {
            char *value = alloc_vector(vec_size);
            if (fetch_vec_from_heap(index, heap, tuple->heaptids[0], value, dim, precision_type)) {
                hc->distance = dist_func(query, value, dim);
                free_vector(value);
            } else if (rbq_param.estimator) {
                free_vector(value);
                EstimateRecord est;
                RaBitQEstimator *estimator = reinterpret_cast<RaBitQEstimator *>(rbq_param.estimator);
                if (bulkbuf) {
                    char *quant_data = bulkbuf->get(idx);
                    get_full_dist(estimator, quant_data, rbq_param, est);
                } else {
                    int quant_size = rbq_param.rbq_meta.quant_size;
                    VecBuffer buf = vec_read_quant(index, idx, quant_size, cst);
                    get_full_dist(estimator, buf.get_vecbuf(), rbq_param, est);
                    buf.release();
                }
                hc->distance = est.est_dist;
            } else {
                elog(PANIC, "could not calculate distance for heaptid (%u:%u) using heap vector or rabitq codes",
                    ItemPointerGetBlockNumber(&tuple->heaptids[0]),
                    ItemPointerGetOffsetNumber(&tuple->heaptids[0]));
            }
        }
    } else {
        if (bulkbuf) {
            char *value = bulkbuf->get(idx);
            hc->distance = dist_func(query, value, dim);
        } else {
            VecBuffer vec_buf = vec_read_buffer(index, idx, vec_size, vst);
            char *value = vec_buf.get_vecbuf();
            hc->distance = dist_func(query, value, dim);
            vec_buf.release();
        }
    }

    hc->floatVectorIndex = tuple->floatVectorIndex;

    level = tuple->level;

    UnlockReleaseBuffer(buf);
    return hc;
}

void HnswSearchLayerDisk(Relation index, Relation heap, const char *query,
    Vector<HnswDiskCandidate *> &cand, int ef, int m, uint32 dim, uint8 lc, distance_func dist_func,
    BlkOffsetNumEntry *skipEntry, QuantizerParam &qt_param, DistPrecisionType precision_type)
{
    pairingheap *C = pairingheap_allocate(CompareNearestDiskCandidates, NULL);
    pairingheap *R = pairingheap_allocate(CompareFurthestDiskCandidates, NULL);

    /*
     * Do not count elements being deleted towards ef when vacuuming. It
     * would be ideal to do this for inserts as well, but this could
     * affect insert performance.
     */
    int Rlen = !skipEntry ? cand.size() : 0;
    UnorderedSet<ItemPointerData> v(ef * m * 2);

    /* Add entry points to v, C, and R */
    for (HnswDiskCandidate *hc : cand) {
        v.insert(hc->indexTid);
        pairingheap_add(C, &(CreatePairingHeapNode(hc)->ph_node));
        pairingheap_add(R, &(CreatePairingHeapNode(hc)->ph_node));
    }
    cand.clear();

    auto run_search = [&](auto &&get_distance, auto &&get_refined_distance) -> void {
        static_assert(IS_INVOCABLE_R(decltype(get_distance), float, size_t, ItemPointerData, bool),
            "get_distance must receive 'size_t', 'ItemPointerData' and 'bool', and return 'float'.");
        static_assert(IS_INVOCABLE_R(decltype(get_refined_distance), void, size_t, bool, float*),
            "get_refined_distance must receive 'size_t', 'bool' and 'float*', and return 'void'.");
        while (!pairingheap_is_empty(C)) {
            HnswDiskCandidate *c = ((HnswDiskPairingHeapNode *) pairingheap_remove_first(C))->inner;
            HnswDiskCandidate *f = ((HnswDiskPairingHeapNode *) pairingheap_first(R))->inner;
    
            if (c->distance > f->distance) {
                break;
            }

            Buffer buf = ReadBuffer(index, ItemPointerGetBlockNumberNoCheck(&c->indexTid));
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            Page page = BufferGetPage(buf);
            HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, c->indexTid.ip_posid));

            if (unlikely(lc > tuple->level || tuple->is_deleted())) {
                UnlockReleaseBuffer(buf);
                continue;
            }

            const bool tuple_has_tid = !tuple->empty();
            uint16 start = m * (tuple->level - lc);
            uint16 end = start + HnswGetLayerM(m, lc);
            for (uint16 i = start; i < end; ++i) {
                if (!ItemPointerIsValid(&tuple->neighbors[i].indexTid)) {
                    break;
                }
                if (!v.insert(tuple->neighbors[i].indexTid).second) {
                    continue;
                }
    
                bool alwaysAdd = Rlen < ef;
                f = ((HnswDiskPairingHeapNode *)pairingheap_first(R))->inner;

                float distance = get_distance(tuple->neighbors[i].floatVectorIndex, tuple->neighbors[i].indexTid, alwaysAdd);

                if (distance >= f->distance && !alwaysAdd) {
                    continue;
                }

                get_refined_distance(tuple->neighbors[i].floatVectorIndex, alwaysAdd, &distance);

                HnswDiskCandidate *ec = (HnswDiskCandidate *)palloc(sizeof(HnswDiskCandidate));
                ec->indexTid = tuple->neighbors[i].indexTid;
                ec->distance = distance;
                ec->floatVectorIndex = tuple->neighbors[i].floatVectorIndex;

                pairingheap_add(C, &(CreatePairingHeapNode(ec)->ph_node));
                pairingheap_add(R, &(CreatePairingHeapNode(ec)->ph_node));

                /*
                 * Do not count elements being deleted towards ef when vacuuming. It
                 * would be ideal to do this for inserts as well, but this could
                 * affect insert performance.
                 */
                if (!skipEntry || tuple_has_tid) {
                    ++Rlen;
                    /* No need to decrement wlen */
                    if (Rlen > ef) {
                        pairingheap_remove_first(R);
                    }
                }
            }
            UnlockReleaseBuffer(buf);
        }

        /* Add each element of W to w */
        while (!pairingheap_is_empty(R)) {
            HnswDiskCandidate *hc = ((HnswDiskPairingHeapNode *)pairingheap_remove_first(R))->inner;
            cand.push_back(hc);
        }
    };

    /* do quantizer setting */
    VecStorageType vst = GetVecStorageType(qt_param, true);
    VecStorageType cst = GetVecStorageType(qt_param, false);
    BulkBuffer *bulkbuf = qt_param.bulkbuf;
    auto empty_searching_refine = [](size_t idx, bool alwaysAdd, float *distance) -> void {};
    const size_t vec_size = dim * VEC_ELEM_SIZE(precision_type);
    if (qt_param.get_type() == QuantizerType::PQ) {
        /* do pq setting */
        constexpr float k_factor = 1.25;
        ef *= k_factor;
        PQParam &pq_param = qt_param.get_pq_param();
        ProductQuantizer pq = *pq_param.pq;
        uint32 code_len = pq_param.code_len;
        float flag = pq_param.flag;

        float *query_float = (float *)query;
        float *half2float = NULL;
        if (precision_type == DistPrecisionType::HALF) {
            half2float = alloc_floatvector(dim);
            halfs_to_floats((half *)query, half2float, dim);
            query_float = half2float;
        }
        float *dist_table = pq_param.dist_table;
        pq.compute_distance_table(query_float, dist_table);

        if (bulkbuf) {
            auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                uint8 *code = (uint8 *)bulkbuf->get(idx);
                float distance = flag * pq.distance_to_code(code, dist_table);
                return distance;
            };
            run_search(get_distance, empty_searching_refine);
        } else {
            auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                VecBuffer pq_buf = vec_read_quant(index, idx, code_len, VecStorageType::PureCode);
                float distance = flag * pq.distance_to_code((uint8 *)pq_buf.get_vecbuf(), dist_table);
                pq_buf.release();
                return distance;
            };
            run_search(get_distance, empty_searching_refine);
        }

        if (half2float) {
            free_vector(half2float);
        }

        /* refine */
        if (ef > 1) {
            char *value = alloc_vector(vec_size);
            for (HnswDiskCandidate *hc : cand) {
                if (fetch_vec_from_heap(index, heap, get_heap_tid(index, hc->indexTid), value, dim, precision_type)) {
                    hc->distance = dist_func(query, value, dim);
                } else {
                    hc->distance = FLT_MAX;
                }
            }
            free_vector(value);
            std::sort(cand.begin(), cand.end(),
                [](const HnswDiskCandidate *a, const HnswDiskCandidate *b){
                    return a->distance > b->distance;
                });
        }
    } else if (qt_param.get_type() == QuantizerType::RABITQ) {
        /* for scan or insert to estimate distance based on one vector */
        RaBitQParam &rbq_param = qt_param.get_rabitq_param();
        RaBitQEstimator *estimator = reinterpret_cast<RaBitQEstimator *>(rbq_param.estimator);
        if (rbq_param.applied) {
            if (bulkbuf) {
                auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                    float distance;
                    EstimateRecord est;
                    char *quant_data = bulkbuf->get(idx);
                    if (alwaysAdd) {
                        get_full_dist(estimator, quant_data, rbq_param, est);
                        distance = est.est_dist;
                    } else {
                        get_bin_dist(estimator, quant_data, rbq_param, est);
                        distance = est.low_dist;
                    }
                    return distance;
                };
                auto searching_refine = [&](size_t idx, bool alwaysAdd, float *distance) -> void {
                    if (!alwaysAdd) {
                        EstimateRecord est;
                        char *quant_data = bulkbuf->get(idx);
                        /* reranking with fully estimated distance */
                        get_full_dist(estimator, quant_data, rbq_param, est);
                        *distance = est.est_dist;
                    }
                };
                run_search(get_distance, searching_refine);
            } else {
                auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                    float distance;
                    EstimateRecord est;
                    int quant_size = rbq_param.rbq_meta.quant_size;
                    VecBuffer buf = vec_read_quant(index, idx, quant_size, cst);
                    char *quant_data = buf.get_vecbuf();
                    if (alwaysAdd) {
                        get_full_dist(estimator, quant_data, rbq_param, est);
                        distance = est.est_dist;
                    } else {
                        get_bin_dist(estimator, quant_data, rbq_param, est);
                        distance = est.low_dist;
                    }
                    buf.release();
                    return distance;
                };
                auto searching_refine = [&](size_t idx, bool alwaysAdd, float *distance) -> void {
                    if (!alwaysAdd) {
                        EstimateRecord est;
                        int quant_size = rbq_param.rbq_meta.quant_size;
                        VecBuffer buf = vec_read_quant(index, idx, quant_size, cst);
                        /* reranking with fully estimated distance */
                        get_full_dist(estimator, buf.get_vecbuf(), rbq_param, est);
                        *distance = est.est_dist;
                        buf.release();
                    }
                };
                run_search(get_distance, searching_refine);
            }
        } else if (rbq_param.rbq_meta.keep_vecs) {
            auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                VecBuffer vec_buf = vec_read_buffer(index, idx, dim * VEC_ELEM_SIZE(precision_type), vst);
                float distance = dist_func(query, vec_buf.get_vecbuf(), dim);
                vec_buf.release();
                return distance;
            };
            run_search(get_distance, empty_searching_refine);
        } else {
            auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                char *value = alloc_vector(vec_size);
                float distance = FLT_MAX; /* skipped if no vector fetched from heap */
                if (fetch_vec_from_heap(index, heap, get_heap_tid(index, indexTid), value, dim, precision_type)) {
                    distance = dist_func(query, value, dim);
                }
                free_vector(value);
                return distance;
            };
            run_search(get_distance, empty_searching_refine);
        }
    } else {
        if (bulkbuf) {
            auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                char *value = bulkbuf->get(idx);
                float distance = dist_func(query, value, dim);
                return distance;
            };
            run_search(get_distance, empty_searching_refine);
        } else {
            auto get_distance = [&](size_t idx, ItemPointerData indexTid, bool alwaysAdd) -> float {
                VecBuffer vec_buf = vec_read_buffer(index, idx, dim * VEC_ELEM_SIZE(precision_type), vst);
                char *value = vec_buf.get_vecbuf();
                float distance = dist_func(query, value, dim);
                vec_buf.release();
                return distance;
            };
            run_search(get_distance, empty_searching_refine);
        }
    }
    optional_destroy(v);
}

/*
 * Check if an element is closer to q than any element from R
 */
static bool CheckElementCloser(HnswCandidate * e, Vector<HnswCandidate *> &r,
    distance_func dist_func, uint32 dim)
{
    HnswElement eElement = (HnswElement)e->element;
    for (HnswCandidate *ri : r) {
        HnswElement riElement = ri->element;
        float distance = dist_func(eElement->value, riElement->value, dim);
        if (distance <= e->distance) {
            return false;
        }
    }
    return true;
}

/*
 * Check if an element is closer to q than any element from R
 */
static bool CheckDiskCandidateCloser(HnswDiskCandidate* e, Vector<HnswDiskCandidate *> &r,
    distance_func dist_func, uint32 dim)
{
    for (HnswDiskCandidate *ri : r) {
        float distance = dist_func(e->value, ri->value, dim);
        if (distance <= e->distance) {
            return false;
        }
    }
    return true;
}

/*
 * Algorithm 4 from paper
 */
static Vector<HnswCandidate *> SelectNeighbors(Vector<HnswCandidate *> &c, int lm, uint8 lc, 
    distance_func dist_func, HnswElement e2, HnswCandidate *newCandidate, HnswCandidate **pruned,
    bool sortCandidates, uint32 dim)
{
    int clength = (int)c.size();
    if (clength <= lm) {
        return c;
    }
    /* Ensure order of candidates is deterministic for closer caching */
    if (sortCandidates) {
        pg_qsort(c.begin(), c.size(), sizeof(HnswCandidate *),
            [](const void *in_a, const void *in_b) -> int {
                const HnswCandidate *a = *(HnswCandidate **)in_a;
                const HnswCandidate *b = *(HnswCandidate **)in_b;
                if (a->distance != b->distance) {
                    return a->distance > b->distance ? -1 : 1;
                }
                return b->element - a->element;
            });
    }

    int wdlen = 0;
    int wdoff = 0;
    int cindex = clength - 1;
    bool removedAny = false;
    HnswNeighborArray *neighbors = HnswGetNeighbors(e2, lc);
    bool mustCalculate = !neighbors->closerSet;
    Vector<HnswCandidate *> r(lm);
    Vector<HnswCandidate *> added(lm);
    Vector<HnswCandidate *> wd(lm);
    while (cindex >= 0 && (int)r.size() < lm) {
        /* Assumes w is already ordered desc */
        HnswCandidate *e = c[cindex--];

        /* Use previous state of r and wd to skip work when possible */
        if (mustCalculate) {
            e->closer = CheckElementCloser(e, r, dist_func, dim);
        } else if (!added.empty()) {
            /*
             * If the current candidate was closer, we only need to compare it
             * with the other candidates that we have added.
             */
            if (e->closer) {
                e->closer = CheckElementCloser(e, added, dist_func, dim);
                if (!e->closer) {
                    removedAny = true;
                }
            } else {
                /*
                 * If we have removed any candidates from closer, a candidate
                 * that was not closer earlier might now be.
                 */
                if (removedAny) {
                    e->closer = CheckElementCloser(e, r, dist_func, dim);
                    if (e->closer) {
                        added.push_back(e);
                    }
                }
            }
        } else if (e == newCandidate) {
            e->closer = CheckElementCloser(e, r, dist_func, dim);
            if (e->closer) {
                added.push_back(e);
            }
        }

        if (e->closer) {
            r.push_back(e);
        } else {
            wd.push_back(e);
            ++wdlen;
        }
    }

    /* Cached value can only be used in future if sorted deterministically */
    neighbors->closerSet = sortCandidates;

    /* Keep pruned connections */
    while (wdoff < wdlen && (int)r.size() < lm) {
        r.push_back(wd[wdoff++]);
    }

    /* Return pruned for update connections */
    if (pruned != NULL) {
        if (wdoff < wdlen) {
            *pruned = wd[wdoff];
        } else {
            *pruned = cindex >= 0 ? c[0] : NULL;
        }
    }
    optional_destroy(wd);
    optional_destroy(added);
    return r;
}

/*
 * Algorithm 4 from paper
 */
static inline Vector<HnswDiskCandidate *> SelectNeighborsonDisk(Vector<HnswDiskCandidate *> &c,
    int lm, int lc, distance_func dist_func, int *pruned, bool sortCandidates, uint32 dim)
{
    int clength = (int)c.size();
    if (clength <= lm) {
        return c;
    }
    /* Ensure order of candidates is deterministic for closer caching */
    if (sortCandidates) {
        pg_qsort(c.begin(), c.size(), sizeof(HnswDiskCandidate *),
            [](const void *in_a, const void *in_b) -> int {
                const HnswDiskCandidate *a = *(HnswDiskCandidate **)in_a;
                const HnswDiskCandidate *b = *(HnswDiskCandidate **)in_b;
                if (a->distance != b->distance) {
                    return a->distance > b->distance ? -1 : 1;
                }
                return b->value - a->value;
            });
    }

    int wdlen = 0;
    int wdoff = 0;
    int cindex = clength - 1;
    Vector<HnswDiskCandidate *> r(lm);
    Vector<HnswDiskCandidate *> wd(lm);
    while (cindex >= 0 && (int)r.size() < lm) {
        /* Assumes w is already ordered desc */
        HnswDiskCandidate *e = c[cindex--];

        if (CheckDiskCandidateCloser(e, r, dist_func, dim)) {
            r.push_back(e);
        } else {
            wd.push_back(e);
            ++wdlen;
        }
    }

    /* Keep pruned connections */
    while (wdoff < wdlen && (int)r.size() < lm) {
        r.push_back(wd[wdoff++]);
    }

    /* Return pruned for update connections */
    if (pruned != NULL) {
        if (wdoff < wdlen) {
            *pruned = wd[wdoff]->neighborindex;
        } else {
            *pruned = cindex >= 0 ? c[0]->neighborindex : -1;
        }
    }
    optional_destroy(wd);
    return r;
}

static void AddConnections(HnswElement element, Vector<HnswCandidate *> &neighbors, int lc)
{
    HnswNeighborArray *a = HnswGetNeighbors(element, lc);
    for (const HnswCandidate *nb : neighbors) {
        a->items[a->length] = *nb;
        a->items[a->length].neighborIndex = a->length;
        ++a->length;
    }
}

static void AddTupleConnections(HnswTuple tuple, Vector<HnswDiskCandidate *> &neighbors, int lc, int m)
{
    const size_t start = m * ((int)tuple->level - lc);
    const size_t lm = HnswGetLayerM(m, lc);
    const size_t len = neighbors.size();
    size_t i = 0;
    for (; i < len  && i < lm; ++i) {
        tuple->neighbors[start + i].floatVectorIndex = neighbors[i]->floatVectorIndex;
        tuple->neighbors[start + i].indexTid = neighbors[i]->indexTid;
    }

    /* Invaild the left neighbors*/
    for (; i < lm; ++i) {
        ItemPointerSetInvalid(&tuple->neighbors[start + i].indexTid);
        tuple->neighbors[start + i].floatVectorIndex = InvalidVectorIndex;
    }
}

/*
 * Update connections
 */
void HnswUpdateConnection(HnswElement element, HnswCandidate *hc, int lm, uint8 lc,
    distance_func dist_func, uint32 dim, int m)
{
    HnswElement hce = (HnswElement)hc->element;
    HnswNeighborArray *currentNeighbors = HnswGetNeighbors(hce, lc);
    HnswCandidate hc2;

    hc2.element = element;
    hc2.distance = hc->distance;
    hc2.neighborIndex = currentNeighbors->length;
    

    if (currentNeighbors->length < lm) {
        currentNeighbors->items[currentNeighbors->length] = hc2;
        pg_memory_barrier();
        currentNeighbors->length++;
    } else {
        /* Shrink connections */
        HnswCandidate *pruned = NULL;
        Vector<HnswCandidate *> c(currentNeighbors->length + 1);
        for (int i = 0; i < currentNeighbors->length; i++) {
            c.push_back(&currentNeighbors->items[i]);
        }
        c.push_back(&hc2);

        Vector<HnswCandidate *> neighbors =
            SelectNeighbors(c, lm, lc, dist_func, hce, &hc2, &pruned, true, dim);
        optional_destroy(neighbors);
        optional_destroy(c);
        /* Should not happen */
        if (!pruned) {
            return;
        }

        if(pruned->neighborIndex < lm) {
			hc2.neighborIndex = pruned->neighborIndex;
            currentNeighbors->items[pruned->neighborIndex] = hc2;
        }
    }
}

int HnswUpdateConnectionDisk(Relation index, Relation heap, HnswTuple tuple,
    HnswNeighborData *neighbor, BlkOffsetNumEntry *entry, distance_func dist_func, int lm,
    uint8 lc, Page metapage, bool checkExisting, QuantizerParam &qt_param)
{
    HnswMetaPage metap = HnswPageGetMeta(metapage);
    const int dim = metap->dimensions;
    const int m = metap->m;
    const DistPrecisionType precision_type = metap->precision_type;

    Buffer buf = ReadBuffer(index, ItemPointerGetBlockNumber(&neighbor->indexTid));
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buf);
    ItemId itemid = PageGetItemId(page, ItemPointerGetOffsetNumber(&neighbor->indexTid));
    HnswTuple ntuple = (HnswTuple) PageGetItem(page, itemid);

    if (unlikely(lc > ntuple->level || ntuple->is_deleted())) {
        UnlockReleaseBuffer(buf);
        return -1;
    }
    int start = m * ((int)ntuple->level - lc);

    int count = 0;
    for (int i = start; i < start + lm; ++i) {
        if (!ItemPointerIsValid(&ntuple->neighbors[i].indexTid)) {
            break;
        }
        if (checkExisting &&
            ItemPointerGetBlockNumber(&ntuple->neighbors[i].indexTid) == entry->blkno &&
            ItemPointerGetOffsetNumber(&ntuple->neighbors[i].indexTid) == entry->offno) {
            /* connction already existes*/
            UnlockReleaseBuffer(buf);
            return -1;
        }
        ++count;
    }

    if (count < lm) {
        UnlockReleaseBuffer(buf);
        return start + count;
    }

    /* Shrink connections */
    Vector<size_t> vectorindexes(lm);
    Vector<ItemPointerData> indexTids(lm);

    /* Load elements on insert */
    for (int i = start; i < start + lm; ++i) {
        if (!ItemPointerIsValid(&ntuple->neighbors[i].indexTid)) {
            break;
        }

        Buffer nbuf = ReadBuffer(index, ItemPointerGetBlockNumber(&ntuple->neighbors[i].indexTid));
        LockBuffer(nbuf, BUFFER_LOCK_SHARE);
        Page npage = BufferGetPage(nbuf);
        ItemId nitemid = PageGetItemId(npage, ItemPointerGetOffsetNumber(&ntuple->neighbors[i].indexTid));
        HnswTuple localtuple = (HnswTuple) PageGetItem(npage, nitemid);

        /* Prune element if being deleted */
        if (localtuple->ntids() == 0) {
            UnlockReleaseBuffer(nbuf);
            UnlockReleaseBuffer(buf);
            vectorindexes.destroy();
            indexTids.destroy();
            return i;
        }

        vectorindexes.emplace_back(ntuple->neighbors[i].floatVectorIndex);
        indexTids.emplace_back(ntuple->neighbors[i].indexTid);
        UnlockReleaseBuffer(nbuf);
    }

    size_t floatvectorIndex = ntuple->floatVectorIndex;
    ItemPointerData heapTid = ntuple->heaptids[0];
    UnlockReleaseBuffer(buf);

    VecStorageType st = GetVecStorageType(qt_param, true);
    BulkBuffer *bulkbuf = qt_param.bulkbuf;
    const size_t vec_size = dim * VEC_ELEM_SIZE(precision_type);
    char *query = NULL;
    VecBuffer vec_buf;
    if (st == VecStorageType::PureCode) {
        query = alloc_vector(vec_size);
        if (!fetch_vec_from_heap(index, heap, heapTid, query, dim, precision_type)) {
            free_vector(query);
            vectorindexes.destroy();
            indexTids.destroy();
            return -1; /* skipped if no vector fetched from heap */
        }
    } else {
        if (bulkbuf) {
            query = bulkbuf->get(floatvectorIndex);
        } else {
            vec_buf = vec_read_buffer(index, floatvectorIndex, vec_size, st);
            query = vec_buf.get_vecbuf();
        }
    }

    /* Add candidates */
    Vector<HnswDiskCandidate *> c(lm + 1);
    for (size_t i = 0; i < vectorindexes.size(); ++i) {
        HnswDiskCandidate *cand = (HnswDiskCandidate *)palloc(sizeof(HnswDiskCandidate));
        char *value = st != VecStorageType::PureCode && bulkbuf ? NULL : alloc_vector(vec_size);
        if (st == VecStorageType::PureCode) {
            if (!fetch_vec_from_heap(index, heap, get_heap_tid(index, indexTids[i]), value, dim, precision_type)) {
                free_vector(value);
                continue;
            }
        } else {
            if (bulkbuf) {
                value = bulkbuf->get(vectorindexes[i]);
            } else {
                VecBuffer local_vec_buf = vec_read_buffer(index, vectorindexes[i], vec_size, st);
                error_t rc = memcpy_s(value, vec_size, local_vec_buf.get_vecbuf(), vec_size);
                securec_check(rc, "\0", "\0");
                local_vec_buf.release();
            }
        }
        cand->distance = dist_func(query, value, dim);
        cand->value = value;
        cand->neighborindex = i + start;
        c.emplace_back(cand);
    }

    bool found = true;
    char *value = st != VecStorageType::PureCode && bulkbuf ? NULL : alloc_vector(vec_size);
    HnswDiskCandidate *hc2 = (HnswDiskCandidate *)palloc(sizeof(HnswDiskCandidate));
    if (st == VecStorageType::PureCode) {
        if (!fetch_vec_from_heap(index, heap, tuple->heaptids[0], value, dim, precision_type)) {
            free_vector(value);
            found = false;
        }
    } else {
        if (bulkbuf) {
            value = bulkbuf->get(tuple->floatVectorIndex);
        } else {
            VecBuffer new_vec_buf = vec_read_buffer(index, tuple->floatVectorIndex, vec_size, st);
            error_t rc = memcpy_s(value, vec_size, new_vec_buf.get_vecbuf(), vec_size);
            securec_check(rc, "\0", "\0");
            new_vec_buf.release();
        }
    }
    if (found) {
        hc2->value = value;
        hc2->neighborindex = start + lm;
        hc2->distance = dist_func(query, value, dim);
        c.emplace_back(hc2);
    }

    if (st == VecStorageType::PureCode) {
        free_vector(query);
    } else if (!bulkbuf) {
        vec_buf.release();
    }

    int prunedIndex = -1;
    Vector<HnswDiskCandidate *> neighbors = SelectNeighborsonDisk(c, lm, lc, dist_func, &prunedIndex, true, dim);
    optional_destroy(neighbors);

    if (bulkbuf && st != VecStorageType::PureCode) {
        for (HnswDiskCandidate *hc : c) {
            pfree(hc);
        }
    } else {
        for(HnswDiskCandidate *hc : c) {
            free_vector(hc->value);
            pfree(hc);
        }
    }
    c.destroy();

    vectorindexes.destroy();
    indexTids.destroy();

    if (prunedIndex < start + lm && prunedIndex >= start) {
        return prunedIndex;
    }
    return -1;
}

static void RemoveElements(Relation index, Vector<HnswDiskCandidate *> &w,
    BlkOffsetNumEntry *skipEntry)
{
    Assert(skipEntry);
    w.erase_if([index, skipEntry](const HnswDiskCandidate *hc) -> bool {
        /* Skip self for vacuuming update */
        const BlockNumber nb_blkno = ItemPointerGetBlockNumber(&hc->indexTid);
        const OffsetNumber nb_offset = ItemPointerGetOffsetNumber(&hc->indexTid);
        if (nb_blkno == skipEntry->blkno && nb_offset == skipEntry->offno) {
            return true;
        }
        bool skip = true;
        Buffer buf = ReadBuffer(index, nb_blkno);
        Page page = BufferGetPage(buf);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        ItemId itemid = PageGetItemId(page, nb_offset);
        if (ItemIdIsValid(itemid)) {
            HnswTuple tup = (HnswTuple)PageGetItem(page, itemid);
            skip = tup->empty();
        }
        UnlockReleaseBuffer(buf);
        return skip;
    }, false);
}

/*
 * Algorithm 1 from paper
 */
void HnswFindElementNeighbors(HnswElement element, HnswElement entryPoint, distance_func dist_func,
                              int m, int efConstruction, uint32 dim)
{
    /* No neighbors if no entry point */
    if (!entryPoint) {
        return;
    }

    /* Get entry point and level */
    const char *query = element->value;
    Vector<HnswCandidate *> ep;
    ep.push_back(HnswEntryCandidate(entryPoint, query, dist_func, dim));
    uint8 level = element->level;
    uint8 entryLevel = entryPoint->level;
    /* 1st phase: greedy search to insert level */
    for (uint8 lc = entryLevel; lc >= level + 1u; --lc) {
        HnswSearchLayer(query, dist_func, ep, 1, m, dim, lc);
    }
    if (level > entryLevel) {
        level = entryLevel;
    }

    /* 2nd phase */
    for (uint8 ilc = 0; ilc <= level; ++ilc) {
        const uint8 lc = level - ilc;
        HnswSearchLayer(query, dist_func, ep, efConstruction, m, dim, lc);

        /*
         * Candidates are sorted, but not deterministically. Could set
         * sortCandidates to true for in-memory builds to enable closer
         * caching, but there does not seem to be a difference in performance.
         */
        Vector<HnswCandidate *> neighbors =
            SelectNeighbors(ep, HnswGetLayerM(m, lc), lc, dist_func, element, NULL, NULL, false, dim);
        AddConnections(element, neighbors, lc);
        optional_destroy(neighbors);
    }
    optional_destroy(ep);
}

/*
 * Add a heap TID to an existing tuple
 */
static bool
AddDuplicateOnDisk(Relation index, HnswMetaPage metap, HnswTuple tupleInsert, const char *query,
    HnswDiskCandidate* dup, bool building)
{
    Buffer buf = ReadBuffer(index, ItemPointerGetBlockNumber(&dup->indexTid));
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buf);

    /* Find space */
    const OffsetNumber offno = ItemPointerGetOffsetNumber(&dup->indexTid);
    ItemId itemid = PageGetItemId(page, offno);
    HnswTuple tuple = (HnswTuple) PageGetItem(page, itemid);
    if (tuple->empty()) {
        /* it's empty and searchable
         * only if the index is under vacuum and it will be mark deleted later
         */
        UnlockReleaseBuffer(buf);
        return false;
    }

    bool overwriten = false;
    bool res = false;

    if (metap->cluster.cluster_pq) {
        // ProductQuantizer pq;
        // pq.set_basic_values(RelationGetDescr(index)->attrs[0].atttypmod, pq_metainfo.m, pq_metainfo.nbits());
        // pq.set_fvec_L2sqr_ny_nearest_func();
        // hnsw_read_pq_center(index, pq);
        // res = tuple->insert_tid_pq(index, building, query, tupleInsert->heaptids[0], pq, overwriten);
        // pq.free_resourses();
    } else {
        res = tuple->insert_tid(index, building, tupleInsert->heaptids[0], overwriten);
    }

    if (overwriten) {   /* Commit */
        MarkBufferDirty(buf);
        if (!building) {
            HnswXLogUpdateHeaptid(index, tuple->get_heaptids(), tuple->ntids(), offno, buf, page);
        }
    }
    UnlockReleaseBuffer(buf);

    return res;
}

/*
 * Find duplicate element
 */
static bool FindDuplicateOnDisk(Relation index, HnswMetaPage metap, HnswTuple tup, const char *query,
    const Vector<HnswDiskCandidate *> &neighbors, bool building, uint32 dim)
{
    for (HnswDiskCandidate *hc : neighbors) {
        size_t old_idx = tup->floatVectorIndex;

        if (memcmp(query, hc->value, VEC_ELEM_SIZE(metap->precision_type) * dim) == 0) {
            tup->floatVectorIndex = InvalidVectorIndex;
        } else if (tup->floatVectorIndex != NonCenterVectorIndex) {
            /* Exit early since ordered by distance */
            return false;
        }

        if (AddDuplicateOnDisk(index, metap, tup, query, hc, building)) {
            return true;
        }
        tup->floatVectorIndex = old_idx;
    }

    return false;
}

/*
 * Algorithm 1 from paper
 */
bool HnswFindElementNeighborsonDisk(Relation index, Relation heap, const char *query, HnswTuple tup,
    Buffer meta_buf, const HnswMetaPage metap, BlkOffsetNumEntry *skipEntry, bool building,
    QuantizerParam &qt_param)
{
    Assert(RelationIsValid(index));
    LockBuffer(meta_buf, BUFFER_LOCK_SHARE);
    const BlockNumber entry_blkno = metap->entryBlkno;
    const OffsetNumber entry_offno =  metap->entryOffno;
    LockBuffer(meta_buf, BUFFER_LOCK_UNLOCK);
    /* No neighbors if no entry point */
    if (!BlockNumberIsValid(entry_blkno)) {
        return false;
    }

    const uint32 dim = metap->dimensions;
    const uint16 m = metap->m;
    uint16 efConstruction = metap->efConstruction;
    bool addasDuplicate = false;
    uint8 level = tup->level;
    DistPrecisionType precision_type = metap->precision_type;
    uint8 entryLevel;
    const distance_func dist_func = hnsw_get_aligned_distance_func(index, metap->metric,
        metap->dimensions, precision_type, qt_param.get_type() == QuantizerType::PQ);
    /* Get entry point and level */
    HnswDiskCandidate *hc = GetDiskCandiateByTuple(index, heap, entry_blkno, entry_offno,
        query, dim, dist_func, entryLevel, qt_param, precision_type);
    Vector<HnswDiskCandidate *> ep(1ul, hc);

    /* 1st phase: greedy search to insert level */
    for (uint8 lc = entryLevel; lc >= level + 1u; --lc) {
        HnswSearchLayerDisk(index, heap, query, ep, 1, m, dim, lc, dist_func, skipEntry, qt_param, precision_type);
    }
    if (level > entryLevel) {
        level = entryLevel;
    }

    /* Add one for existing element */
    if (skipEntry) {
        ++efConstruction;
    }

    VecStorageType st = GetVecStorageType(qt_param, true);

    /* 2nd phase */
    for (int lc = level; lc >= 0; --lc) {
        HnswSearchLayerDisk(index, heap, query, ep, efConstruction, m, dim, lc, dist_func, skipEntry, qt_param, precision_type);

        /* Elements being deleted or skipped can help with search */
        /* but should be removed before selecting neighbors */
        Vector<HnswDiskCandidate *> lw(ep);
        if (skipEntry) {
            RemoveElements(index, lw, skipEntry);
        }

        /*
         * Candidates are sorted, but not deterministically. Could set
         * sortCandidates to true for in-memory builds to enable closer
         * caching, but there does not seem to be a difference in performance.
         */
        const size_t vec_size = dim * VEC_ELEM_SIZE(precision_type);
        const size_t aligned_vec_size = get_aligned_vec_size(vec_size);
        char *vectors = alloc_vector(aligned_vec_size * lw.size());
        char *temp_vec = vectors;
        for (HnswDiskCandidate *lwh : lw) {
            if (st == VecStorageType::PureCode) {
                char *vector = alloc_vector(vec_size);
                if (fetch_vec_from_heap(index, heap, get_heap_tid(index, lwh->indexTid), vector, dim, precision_type)) {
                    error_t rc = memcpy_s(temp_vec, vec_size, vector, vec_size);
                    securec_check(rc, "\0", "\0");
                }
                free_vector(vector);
            } else {
                if (st == VecStorageType::PureVec && qt_param.bulkbuf) {
                    char *vector = qt_param.bulkbuf->get(lwh->floatVectorIndex);
                    error_t rc = memcpy_s(temp_vec, vec_size, vector, vec_size);
                    securec_check(rc, "\0", "\0");
                } else {
                    VecBuffer local_vec_buf = vec_read_buffer(index, lwh->floatVectorIndex, vec_size, st);
                    error_t rc = memcpy_s(temp_vec, vec_size, local_vec_buf.get_vecbuf(), vec_size);
                    securec_check(rc, "\0", "\0");
                    local_vec_buf.release();
                }
            }
            lwh->value = temp_vec;
            temp_vec += aligned_vec_size;
        }

        int lm = HnswGetLayerM(m, lc);
        auto neighbors = SelectNeighborsonDisk(lw, lm, lc, dist_func, NULL, false, dim);
        if (skipEntry == NULL && lc == 0 &&
            FindDuplicateOnDisk(index, metap, tup, query, neighbors, building, dim)) {
            addasDuplicate = true;
        } else {
            AddTupleConnections(tup, neighbors, lc, m);
        }

        free_vector(vectors);
        optional_destroy(neighbors);
        optional_destroy(lw);
    }
    optional_destroy(ep);

    return addasDuplicate;
}

using namespace disk_container;
using namespace ann_helper;

uint32 hnsw_get_vector(float **vecs, Relation heap, Relation index, ItemPointer tids, uint32 ndata,
                       uint32 dim)
{
    uint32 res = 0;
    RelationIncrementReferenceCount(index);
    IndexScanDesc scan = RelationGetIndexScan(index, 0, 0);
    scan->heapRelation = heap;
    scan->xs_snapshot = u_sess->utils_cxt.CurrentSnapshot;
    scan->xs_heapfetch = tableam_scan_index_fetch_begin(heap);
    const int2 vec_attr = index->rd_index->indkey.values[0];
    /* there is risk if use expression to create index, because index->rd_index->indkey.values is always zero, should fix this in future*/
    Assert(vec_attr != 0);
    for (uint32 i = 0; i < ndata; ++i) {
        scan->xs_ctup.t_self = tids[i];
        HeapTuple htuple = (HeapTuple)IndexFetchTuple(scan);
        if (!htuple) {
            continue;
        }
        bool is_null;
        Datum d = tableam_tops_tuple_getattr(htuple, vec_attr, heap->rd_att, &is_null);
        Assert(!is_null);
        FloatVector *vd = DatumGetFloatVector(d);
        Assert(vd->dim == int32(dim));
        errno_t rc = memcpy_s(vecs[res], dim * sizeof(float), vd->x, dim * sizeof(float));
        securec_check(rc, "\0", "\0");
        if ((Pointer)vd != DatumGetPointer(d)) {
            pfree(vd);
        }
        ++res;
    }
    if (scan->xs_heapfetch) {
        tableam_scan_index_fetch_end(scan->xs_heapfetch);
    }
    if (BufferIsValid(scan->xs_cbuf)) {
        ReleaseBuffer(scan->xs_cbuf);
    }
    RelationDecrementReferenceCount(index);
    IndexScanEnd(scan);
    return res;
}




distance_func hnsw_get_aligned_distance_func(Relation index, Metric metric, int dim,
    DistPrecisionType type, bool ispq)
{
    if (ispq && metric == Metric::INNER_PRODUCT && HnswOptionalProcInfo(index, HNSW_NORM_PROC)) {
        metric = Metric::L2;
    }
    return type == DistPrecisionType::FLOAT ?
            get_aligned_distance_func(metric, dim) :
            get_aligned_half_distance_func(metric, dim);
}

HnswNeighborArray *HnswGetNeighbors(HnswElement element, uint8 lc)
{
    HnswNeighborArray **neighborList = (HnswNeighborArray **)element->neighbors;
    Assert(element->level >= lc);
    return (HnswNeighborArray *)neighborList[lc];
}
