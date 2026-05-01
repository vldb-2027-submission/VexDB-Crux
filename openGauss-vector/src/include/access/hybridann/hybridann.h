/**
 * Copyright ...
 * Hybridann index interface.
 */

#ifndef HYBRIDANN_H
#define HYBRIDANN_H

#include <vtl/vector>
#include <vtl/array>

#include "access/genam.h"
#include "nodes/nodes.h"
#include "nodes/relation.h"
#include "utils/tuplesort.h"

#define HYBRIDANN_DISTANCE_PROC               1
#define HYBRIDANN_WORK_MEM_PERCENTAGE         0.9

#define HYBRIDANN_META_ID                     0xFFA1u
#define HYBRIDANN_PAGE_ID                     0xFFA0u
#define HYBRIDANN_MAGIC_NUMBER                0x54414E4E
#define HYBRID_VERSION_ONE                    1
#define HYBRIDANN_METAPAGE_BLKNO              0

constexpr size_t max_index_magnitude_size = 10ul;
constexpr Array<size_t, 5ul> default_index_magnitudes =
    {20'000ul, 100'000ul, 500'000ul, 2'500'000ul, 12'500'000ul};

/* note: opaque has the same format with diskvector opaque, change on the struct
   will affect how pagehack parse it, but all else things should be fine */
typedef struct HybridAnnPageOpaque {
    uint16      unused;
    uint16      page_id;
} HybridAnnPageOpaque;

typedef struct HybridAnnMetaPage {
    uint32          magicNumber;
    Metric          metric;
    uint32          dimensions;
    uint32          version;
    uint32          m;
    uint32          ef_construction;
    BlockNumber     freespaceMetaBlkNo;
    BlockNumber     BTMetaBlkNo;
    BlockNumber     dataMetaBlkNo;
    size_t          sizeIndexMagnitudes;
    size_t          indexMagnitudes[max_index_magnitude_size];
    int64           graphMagnitudeThreshold;
}   HybridAnnMetaPage;

#define HybridAnnPageGetOpaque(page)  ((HybridAnnPageOpaque *) PageGetSpecialPointer(page))
#define HybridAnnPageGetMeta(page)    ((HybridAnnMetaPage *) PageGetContents(page))

typedef struct HybridAnnOptions {
    int32       vl_len_;                // varlena header (do not touch directly!)
    uint32      parallel_workers;       // the number of parallel threads
    int32       fillfactor;
    uint32      m;
    uint32      ef_construction;
    int         vec_index_magnitudes_offset;
    int64       graph_magnitude_threshold;
}   HybridAnnOptions;

typedef struct HybridAnnDataMetaPage {
    size_t         num_vectors;
} HybridAnnDataMetaPage;
typedef HybridAnnDataMetaPage *HybridAnnDataMeta;

typedef struct HybridAnnBuildState {
    Relation heap;
    Relation index;
    IndexInfo *indexInfo;
    ForkNumber forkNum;

    double relTuples;
    double idxTuples;

    FmgrInfo *procInfo;
    Oid collation;

    /* Basic Info */
    Metric metric;
    uint32 dimensions;
    uint32 m;
    uint32 ef_construction;
    uint32 numThreads;

    /* Data Processing */
    size_t numPoints;
    size_t num_inserted;
    uint64 maxWorkMem;
    uint32 maxNumPointsInMem;

    Vector<float, HUGE_ALLOCATOR<float>> data;

    Tuplesortstate *sortState;
    TupleDesc	tupdesc;
    BlockNumber btreeMetaBlkNo;

    Vector<size_t> indexMagnitudes;
} HybridAnnBuildState;

extern IndexBuildResult *hybridannbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo);
extern void hybridannbuildempty_internal(Relation index);
extern bytea *hybridannoptions_internal(Datum reloptions, bool validate);
extern IndexScanDesc hybridannbeginscan_internal(Relation index, int nkeys, int norderbys);
extern void hybridannrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern bool hybridanngettuple_internal(IndexScanDesc scan, ScanDirection dir);
extern void hybridannendscan_internal(IndexScanDesc scan);
extern void hybridanncostestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity, double *indexCorrelation);
extern bool hybridanninsert_internal(Relation index, Datum *values, const bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique);
extern IndexBulkDeleteResult *hybridannbulkdelete_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *hybridannvacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

extern Datum hybridannbuild(PG_FUNCTION_ARGS);
extern Datum hybridannbuildempty(PG_FUNCTION_ARGS);
extern Datum hybridannbeginscan(PG_FUNCTION_ARGS);
extern Datum hybridannrescan(PG_FUNCTION_ARGS);
extern Datum hybridanngettuple(PG_FUNCTION_ARGS);
extern Datum hybridannendscan(PG_FUNCTION_ARGS);
extern Datum hybridanncostestimate(PG_FUNCTION_ARGS);
extern Datum hybridanninsert(PG_FUNCTION_ARGS);
extern Datum hybridannbulkdelete(PG_FUNCTION_ARGS);
extern Datum hybridannvacuumcleanup(PG_FUNCTION_ARGS);
extern Datum hybridannoptions(PG_FUNCTION_ARGS);


extern uint32 hybridAnnGetNumParallel(Relation index);
extern const char *hybridAnnGetVecIndexMagnitudes(Relation index);
extern int64 hybridAnnGetGraphMagnitudeThreshold(Relation index);
extern uint32 hybridannGetM(Relation index);
extern uint32 hybridannGetEfConstruction(Relation index);
extern int32 HybridAnnGetFillFactor(Relation index);
extern FmgrInfo *hybridAnnOptionalProcInfo(Relation rel, uint16 procnum);
extern void HybridAnnInitPage(Buffer buf, Page page);
extern void hybridAnnInitMeta(Buffer buf, Page page);
extern BlockNumber hybridAnnGetFreespaceblkno(Relation index);

#endif /* HYBRIDANN_H */
