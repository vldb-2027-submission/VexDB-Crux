#ifndef HNSW_H
#define HNSW_H

#include "postgres.h"
#include "fmgr.h"

#include "access/hnsw/hnsw_struct.h"
#include "access/diskann/diskann.h"
#include "access/diskann/vector_bt.h"

/* function */
extern Datum hnswbuild(PG_FUNCTION_ARGS);
extern Datum hnswbuildempty(PG_FUNCTION_ARGS);
extern Datum hnswinsert(PG_FUNCTION_ARGS);
extern Datum hnswbulkdelete(PG_FUNCTION_ARGS);
extern Datum hnswvacuumcleanup(PG_FUNCTION_ARGS);
extern Datum hnswcostestimate(PG_FUNCTION_ARGS);
extern Datum hnswoptions(PG_FUNCTION_ARGS);
extern Datum hnswvalidate(PG_FUNCTION_ARGS);
extern Datum hnswbeginscan(PG_FUNCTION_ARGS);
extern Datum hnswrescan(PG_FUNCTION_ARGS);
extern Datum hnswgettuple(PG_FUNCTION_ARGS);
extern Datum hnswendscan(PG_FUNCTION_ARGS);

/* build */
extern void hnswbuildempty_internal(Relation index);
extern IndexBuildResult *hnswbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo);
extern HnswMetaBlknos createHnswMetaPage(Relation index, ForkNumber forkNum, int m, int dimensions, 
    int efConstruction, Metric metric, QuantizerType qt_type, RaBitQParam *rbq_param,
    DistPrecisionType precision_type = DistPrecisionType::FLOAT, bool needWal = false);
extern BlockNumber BuildHnswIndex(Relation heap, Relation index, IndexInfo *indexInfo,
    ForkNumber forkNum, int m, int efConstruction, QuantizerType qt_type, int parallel_workers,
    int maintenance_work_mem, vector_pair_vector *vec_data = NULL, double *reltuples = NULL,
    double *indtuples = NULL);

/* insert */
extern void HnswInsertAppendPage(Relation index, Buffer *nbuf, Page *npage, Page page);
extern bool hnswinsert_internal(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno, size_t floatvectoriIndex);
extern bool HnswInsertTupleOnDisk(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, bool building, size_t floatVectorIndex, BlockNumber metablkno);
extern void HnswUpdateNeighborsOnDisk(Relation index, Relation heap, distance_func dist_func,
    HnswTuple tuple, BlkOffsetNumEntry *entry, Page metapage, bool checkExisting, bool building,
    QuantizerParam &qt_param);

/* scan */
extern void *create_hnsw_scanopaque(Relation index);
extern void free_hnsw_scanopaque(void *so);
extern IndexScanDesc hnswbeginscan_internal(Relation index, int nkeys, int norderbys);
extern void hnswrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys,
    int norderbys);
extern bool hnswgettuple_internal(IndexScanDesc scan, void *so, BlockNumber metablkno, int ef,
    float *dist_out = NULL);
extern void hnswendscan_internal(IndexScanDesc scan);

/* utils */
extern int HnswGetM(Relation index);
extern int HnswGetEfConstruction(Relation index);
extern int HnswGetBuildParallel(Relation index);
extern QuantizerType HnswGetQuantizerType(Relation index);
extern int64 HnswGetNumCluster(Relation index);

extern FmgrInfo *HnswOptionalProcInfo(Relation rel, uint16 procnum);
extern Buffer HnswNewBuffer(Relation index, ForkNumber forkNum, bool needlock = false);
extern Buffer HnswLoadBufferExtended(Relation index, ForkNumber forkNum, BlockNumber blkNo);
extern void HnswCommitBuffer(Buffer buf);
extern void HnswInitPage(Buffer buf, Page page);

extern VecStorageType GetVecStorageType(QuantizerParam &qt_param, bool for_vector);
extern VecStorageType GetVecStorageType(bool graph_pq, RaBitQParam &rbq_param, bool for_vector);
extern HnswTuple HnswInitTuple(ItemPointer heaptid, bool as_bin_elem, int m,
    size_t floatVectorIndex);
extern void HnswSetNeighborsData(HnswTuple tuple, HnswElement e, int m);
extern void HnswSetElementTuple(HnswTuple tuple, HnswElement element);
extern void HnswSetElementVector(Relation index, size_t idx, size_t vec_size, char *value,
    bool building, bool with_qtcode = false);
extern void HnswSetElementQTCode(Relation index, size_t idx, size_t code_len, char *code,
    bool building, bool with_vector = true);
extern HnswNeighborArray *HnswGetNeighbors(HnswElement element, uint8 lc);
extern void *HnswAlloc(HnswAllocator * allocator, Size size);
extern void *HnswAlignedAlloc(HnswAllocator * allocator, Size size, Size align);

extern HnswDiskCandidate* GetDiskCandiateByTuple(Relation index, Relation heap, BlockNumber blkno,
    OffsetNumber offno, const char* query, uint32 dim, distance_func dist_func, uint8 &level,
    QuantizerParam &qt_param, DistPrecisionType precision_type);
extern void HnswSearchLayerDisk(Relation index, Relation heap, const char *query,
    Vector<HnswDiskCandidate *> &cand, int ef, int m, uint32 dim, uint8 lc, distance_func dist_func,
    BlkOffsetNumEntry *skipEntry, QuantizerParam &qt_param, DistPrecisionType precision_type);
extern HnswElement HnswInitElement(ItemPointer tid, int m, double ml, int maxLevel,
    HnswElement bin_elem, HnswAllocator * alloc, size_t floatVectorIndex);
extern void HnswFindElementNeighbors(HnswElement element, HnswElement entryPoint,
    distance_func dist_func, int m, int efConstruction, uint32 dim);
extern bool HnswFindElementNeighborsonDisk(Relation index, Relation heap, const char *query,
    HnswTuple tup, Buffer meta_buf, const HnswMetaPage metap, BlkOffsetNumEntry *skipEntry,
    bool building, QuantizerParam &qt_param);
extern void HnswUpdateConnection(HnswElement element, HnswCandidate * hc, int lm, uint8 lc, 
    distance_func dist_func, uint32 dim, int m);
extern int HnswUpdateConnectionDisk(Relation index, Relation heap, HnswTuple tuple,
    HnswNeighborData *neighbor, BlkOffsetNumEntry *entry, distance_func dist_func, int lm,
    uint8 lc, Page metapage, bool checkExisting, QuantizerParam &qt_param);
extern distance_func hnsw_get_aligned_distance_func(Relation index, Metric metric, int dim,
    DistPrecisionType type, bool ispq = false);
extern ItemPointerData get_heap_tid(Relation index, ItemPointerData indexTid);

/* vacuum */
extern IndexBulkDeleteResult *hnswbulkdelete_internal(Relation index, IndexBulkDeleteResult *stats,
    int nparallel, IndexBulkDeleteCallback callback, void *callback_state, BlockNumber metablkno,
    IdxSet *delete_set);
extern IndexBulkDeleteResult *hnswvacuumcleanup_internal(IndexVacuumInfo *info,
    IndexBulkDeleteResult *stats);

/* inspect */
extern void *hnsw_inspect(Relation index);

#endif /* HNSW_H */
