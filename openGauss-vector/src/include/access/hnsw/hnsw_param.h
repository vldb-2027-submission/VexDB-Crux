#ifndef HNSW_PARAM_H
#define HNSW_PARAM_H

#define HNSW_MAX_DIM FLOATVECTOR_MAX_DIM
#define HNSW_MAX_NNZ 1000

/* Support functions */
#define HNSW_DISTANCE_PROC 1
#define HNSW_NORM_PROC 2
#define HNSW_TYPE_INFO_PROC 3

#define HNSW_VERSION 1
#define HNSW_MAGIC_NUMBER 0xA953A953
#define HNSW_PAGE_ID 0xFF90

/* Preserved page numbers */
#define HNSW_METAPAGE_BLKNO 0
#define HNSW_HEAD_BLKNO 1 /* first element page */
#define HNSW_PS_BLKNO 2
#define HNSW_PQ_BLKNO(index) (HnswUseCluster(index) ? 3 : 2)
/* pq and rabitq cannot coexist in the same index */
#define HNSW_RABITQ_BLKNO(index) HNSW_PQ_BLKNO(index)

/* HNSW parameters */
#define HNSW_DEFAULT_M 16
#define HNSW_MIN_M 2
#define HNSW_MAX_M 100
#define HNSW_DEFAULT_EF_CONSTRUCTION 64
#define HNSW_MIN_EF_CONSTRUCTION 4
#define HNSW_MAX_EF_CONSTRUCTION 1000
#define HNSW_DEFAULT_EF_SEARCH 40
#define HNSW_MIN_EF_SEARCH 1
#define HNSW_MAX_EF_SEARCH 1000
#define HNSW_DEFAULT_QUANTIZER_TYPE 0

/* Tuple types */
#define HNSW_ELEMENT_TUPLE_TYPE 1
#define HNSW_NEIGHBOR_TUPLE_TYPE 2

/* Make graph robust against non-HOT updates */
#define HNSW_HEAPTIDS 10

#define HNSW_UPDATE_ENTRY_GREATER 1
#define HNSW_UPDATE_ENTRY_ALWAYS 2

/* Build phases */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 */
#define PROGRESS_HNSW_PHASE_LOAD 2

#define HNSW_MAX_SIZE (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
                       MAXALIGN(sizeof(HnswPageOpaqueData)) - sizeof(ItemIdData))
#define HNSW_TUPLE_ALLOC_SIZE BLCKSZ
#define HNSW_NEIGHBOR_ARRAY_SIZE(lm) (offsetof(HnswNeighborArray, items) + sizeof(HnswCandidate) * (lm))
#define HnswPageGetOpaque(page) ((HnswPageOpaque) PageGetSpecialPointer(page))
#define HnswPageGetMeta(page) ((HnswMetaPageData *) PageGetContents(page))

#define RandomDouble() (((double) random()) / MAX_RANDOM_VALUE)
#define SeedRandom(seed) srandom(seed)

#define HnswIsElementTuple(tup) ((tup)->type == HNSW_ELEMENT_TUPLE_TYPE)
#define HnswIsNeighborTuple(tup) ((tup)->type == HNSW_NEIGHBOR_TUPLE_TYPE)

#define list_delete_last(list) list_truncate(list, list_length(list) - 1)
#define list_sort(list, cmp) ((list) = list_qsort(list, cmp))

#define HnswIsElementTuple(tup) ((tup)->type == HNSW_ELEMENT_TUPLE_TYPE)
#define HnswIsNeighborTuple(tup) ((tup)->type == HNSW_NEIGHBOR_TUPLE_TYPE)

/* 2 * M connections for ground layer */
#define HnswGetLayerM(m, layer) (layer == 0 ? (m) * 2 : (m))

/* Optimal ML from paper */
#define HnswGetMl(m) (1 / log(m))

/* Ensure fits on page and in uint8 */
#define HnswGetMaxLevel(m) \
    Min(((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(HnswPageOpaqueData)) - \
          HnswTupleDataSize - sizeof(ItemIdData)) / (sizeof(HnswNeighborData)) / (m)) - 2, 255)

#define relptr_offset(rp) ((rp).relptr_off - 1)
#define HnswPtrIsNull(hp) (hp == NULL)
#define HnswPtrEqual(hp1, hp2)  (hp1 == hp2)

#define InvalidVectorIndex size_t(-1)
#define NonCenterVectorIndex size_t(-2)

static_assert(HNSW_HEAPTIDS <= 15 && HNSW_HEAPTIDS > 0, "wrong HNSW_HEAPTIDS");

#define HnswElementDataAlignment (sizeof(LWLock::state))


#endif /* HNSW_PARAM_H */
