/**
 * Copyright ...
 * Diskann index interface.
 */

#ifndef DISKANN_H
#define DISKANN_H

#include <vtl/vector>
#include <vtl/disk_container/plain_store.hpp>

#include "access/genam.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/xlogreader.h"
#include "catalog/pg_index.h"
#include "lib/stringinfo.h"
#include "storage/buf/bufmgr.h"
#include "utils/tuplesort.h"
#include "access/relscan.h"
#include "nodes/execnodes.h"
#include "knl/knl_instance.h"
#include "access/annvector/floatvector.h"
#include "access/annvector/distance/distance.h"

#define DISKANN_MAX_DIM                     FLOATVECTOR_MAX_DIM
#define DISKANN_DISTANCE_PROC               1
#define DISKANN_VERSION_ONE                 1
#define DISKANN_VERSION_TWO                 2
#define DISKANN_MAGIC_NUMBER                0x44414E4E
#define DISKANN_PAGE_ID                     0xFFA0u
#define DISKANN_META_ID                     0xFFA1u
#define DISKANN_METAPAGE_BLKNO              0
#define DISKANN_NUM_PQ_BITS                 8u
#define DISKANN_MAX_NUM_PQ_CENTROIDS        (1u << DISKANN_NUM_PQ_BITS)
#define DISKANN_MIN_NUM_PQ_CENTROIDS        8u
#define DISKANN_NUM_KMEANS_REPS_PQ          12
#define DISKANN_MAX_PQ_TRAINING_SET_SIZE    256000
#define DISKANN_WORK_MEM_PERCENTAGE         0.9

#define DiskAnnPageGetOpaque(page)  ((DiskAnnPageOpaque *) PageGetSpecialPointer(page))
#define DiskAnnPageGetMeta(page)    ((DiskAnnMetaPageBase *) PageGetContents(page))

#define DISKANN_PRINT_NEIGHBORS_PER_POINT false

constexpr size_t kb_to_bytes = 1024ul;
constexpr size_t MAX_ANN_GRAPH_DEGREE = 200ul;
constexpr size_t DEFAULT_ANN_QUEUE_SIZE = 100ul;
constexpr size_t MAX_ANN_SUBGRAPH_COUNT = 10ul;
constexpr size_t MAX_OVERLAPPING_FACTOR = 2;


namespace diskann_node_flag {
constexpr uint8 VALID_FLAG = 0x01u;
constexpr uint8 EXISTING_FLAG = 0x02u;
constexpr uint8 FROZEN_FLAG = 0x04u;
constexpr static uint8 init_flag = VALID_FLAG | EXISTING_FLAG;
inline bool is_valid(uint8 flag) { return flag & VALID_FLAG; }
inline bool is_existing(uint8 flag) { return flag & EXISTING_FLAG; }
inline bool is_frozen(uint8 flag) { return flag & FROZEN_FLAG; }
inline bool visible(uint8 flag) { return flag == (VALID_FLAG | EXISTING_FLAG); }
inline void set_valid(uint8 &flag) { flag |= VALID_FLAG; }
inline void set_existing(uint8 &flag) { flag |= (VALID_FLAG | EXISTING_FLAG); }
inline void set_frozen(uint8 &flag) { flag |= (VALID_FLAG | FROZEN_FLAG); }
inline void unset_valid(uint8 &flag) { flag = 0x00u; }
inline void unset_existing(uint8 &flag) { flag &= ~EXISTING_FLAG; }
inline void unset_frozen(uint8 &flag) { flag &= ~FROZEN_FLAG; }
} /* namespace diskann_node_flag */

typedef struct DiskAnnOptions {
    int32       vl_len_;                // varlena header (do not touch directly!)
    uint32      parallel_workers;       // the number of parallel threads
    bool        enable_quantization;    // generate PQ data or not
    bool        enable_subgraph;        // allow to construct using subgraph
    double      occlusion_factor;       // occlusion factor
    int32       fillfactor;
    uint32      m;
    uint32      ef_construction;
    int         vec_index_magnitudes_offset;
    double      adaptive_relaxation;
    double      subgraph_max_vectors_factor;
}   DiskAnnOptions;

/* note: opaque has the same format with diskvector opaque, change on the struct
   will affect how pagehack parse it, but all else things should be fine */
typedef struct DiskAnnPageOpaque {
    uint16      unused;
    uint16      page_id;
} DiskAnnPageOpaque;

typedef struct DiskAnnMetaPageBase {
    uint32          magicNumber;
    Metric          metric;
    uint32          dimensions;
    uint32          version;
    BlockNumber     nodeMetaBlkNo;
    BlockNumber     freespaceMetaBlkNo;
}   DiskAnnMetaPageBase;

typedef struct DiskAnnMetaPage : public DiskAnnMetaPageBase {
    uint32          numCenters;
    uint32          numPQChunks;
    uint32          medoid;
    BlockNumber     graphMetaBlkNo;
    BlockNumber     attrMetaBlkNo;
    BlockNumber     pqPivotsMetaBlkNo;
    BlockNumber     pqCompressedMetaBlkNo;
}   DiskAnnMetaPage;

struct DiskAnnVamanaNode {
    ItemPointerData heapTid;
    disk_container::PlainStore::key attr_ptr;
    uint8           flag;
    DiskAnnVamanaNode() = default;
    DiskAnnVamanaNode(ItemPointerData tid, disk_container::PlainStore::key ptr, uint8 flag)
        : heapTid(tid), attr_ptr(ptr), flag(flag) {}
};
static_assert(!ann_helper::constructor_need_ctx<DiskAnnVamanaNode>, "compiler error on DiskAnnVamanaNode");
struct VectorPair {
    size_t vid;
    ItemPointerData tid;
    VectorPair() = default;
    VectorPair(size_t vid, ItemPointerData tid) : vid(vid), tid(tid) {}
};
static_assert(!ann_helper::constructor_need_ctx<VectorPair>, "compiler error on VectorPair");
using vector_pair_vector = Vector<VectorPair, HUGE_ALLOCATOR<VectorPair>>;

using AnnNeighbor = uint32;
struct AnnNeighbors {
    SAFE_CONSTRUCTOR_DECL();
    uint32 version{0};  /* multi-version for neighbor update */
    uint32 num_neighbors;
    AnnNeighbor neighbors[MAX_ANN_GRAPH_DEGREE];
    bool operator==(const AnnNeighbors &rhs) const {
        if (num_neighbors != rhs.num_neighbors) {
            return false;
        }
        for (uint32 i = 0; i < num_neighbors; i++) {
            if (neighbors[i] != rhs.neighbors[i]) {
                return false;
            }
        }
        return true;
    }
    bool operator!=(const AnnNeighbors &rhs) const { return !(*this == rhs); }
};
static_assert(!ann_helper::constructor_need_ctx<AnnNeighbors>, "compiler error on AnnNeighbors");

extern void DiskAnnInitPage(Buffer buf, Page page);
extern void DiskAnnInitMeta(Buffer buf, Page page);

extern FmgrInfo *DiskAnnOptionalProcInfo(Relation rel, uint16 procnum);
extern bool DiskAnnUseBTree(Relation index);
extern float DiskAnnGetOcclusionFactor(Relation index);
extern uint32 DiskAnnGetMaxGraphDegree(Relation index);
extern uint32 DiskAnnGetBuildListSize(Relation index);
extern uint32 DiskAnnGetNumParallel(Relation index);
extern float DiskAnnGetSubGraphAdaptiveRelaxation(Relation index);
extern float DiskAnnGetSubGraphMaxVectorsFactor(Relation index);
extern size_t push_back_vector(Relation rel, BlockNumber data_meta, const float *point, uint32 dim);
extern void *diskann_inspect(Relation index);
extern void *hybridann_inspect(Relation index);

extern IndexBuildResult *diskannbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo);
extern void diskannbuildempty_internal(Relation index);
extern bytea *diskannoptions_internal(Datum reloptions, bool validate);
extern IndexScanDesc diskannbeginscan_internal(Relation index, int nkeys, int norderbys);
extern void diskannrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern bool diskanngettuple_internal(IndexScanDesc scan, ScanDirection dir);
extern void diskannendscan_internal(IndexScanDesc scan);
extern void diskanncostestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity, double *indexCorrelation);
extern bool diskanninsert_internal(Relation index, Datum *values, const bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique);
extern IndexBulkDeleteResult *diskannbulkdelete_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *diskannvacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

extern Datum diskannbuild(PG_FUNCTION_ARGS);
extern Datum diskannbuildempty(PG_FUNCTION_ARGS);
extern Datum diskannbeginscan(PG_FUNCTION_ARGS);
extern Datum diskannrescan(PG_FUNCTION_ARGS);
extern Datum diskanngettuple(PG_FUNCTION_ARGS);
extern Datum diskannendscan(PG_FUNCTION_ARGS);
extern Datum diskanncostestimate(PG_FUNCTION_ARGS);
extern Datum diskanninsert(PG_FUNCTION_ARGS);
extern Datum diskannbulkdelete(PG_FUNCTION_ARGS);
extern Datum diskannvacuumcleanup(PG_FUNCTION_ARGS);
extern Datum diskannoptions(PG_FUNCTION_ARGS);

/*
 * WAL record definitions for diskann's WAL operations
 */
#define XLOG_DISKANN_BUILD_INDEX		0x00
#define XLOG_DISKANN_UNLOG_BUILD_INDEX 	0x10
#define XLOG_DISKANN_SET_ELEM			0x20
#define XLOG_DISKANN_EXTEND_NEWPAGES	0x30
#define XLOG_DISKANN_UPDATE_META_START_NPAGES	0x40
#define XLOG_DISKANN_UPDATE_META_NITEM	0x50
#define XLOG_DISKANN_ADD_VECTOR			0x60
#define XLOG_DISKANN_INPLACE_FILTER_ADD_DATA 0x70
#define XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM 0x80
#define XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM 0x90
#define XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE 0xA0
#define XLOG_DISKANN_INVALIDATE_VECTOR_CACHE 0xB0

extern void diskann_redo(XLogReaderState *record);
extern void diskann_desc(StringInfo buf, XLogReaderState *record);
extern const char* diskann_type_name(uint8 subtype);
#endif /* DISKANN_H */
