#ifndef ROAR_H
#define ROAR_H

#include "postgres.h"
#include "access/genam.h"
#include <vtl/disk_container/diskvector.hpp>
#include <vtl/vector>
#include "access/roar/roar_types.h"
#include "access/annvector/distance/distance.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/module/timer.h"
#include "access/index_backend/taskpool.h"
#include "access/diskann/diskann_internal.h"
#include "access/annvector/store/vector_smgr.h"

#define ROAR_MAGIC_NUMBER                0x524F4152 
#define ROAR_VERSION                     1
#define ROAR_PAGE_ID                     0xFFA0u
#define ROAR_META_ID                     0xFFA1u
#define ROAR_METAPAGE_BLKNO              0

#define ROARPageGetOpaque(page)  ((ROARPageOpaque *) PageGetSpecialPointer(page))
#define ROARPageGetMeta(page)    ((ROARMetaPage *) PageGetContents(page))

// mkmi here is wrong, this can't update the QUERYSIZE and GTSIZE in running, and we should use mdsd.
// mdsd is three times slower than mdsr

#define VEC_DIM 200
#define QUERYSIZE 10000000
#define GTSIZE 10000000
constexpr char *filename = "/home/fanlu/workspace/simple_exp/t2i-10M/gt.t2b.bin";

#define Default_M 35
#define Default_efConstruction 500
#define Default_NumberGroundTruth 100

extern IndexBuildResult *roarbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo);
extern bool roarinsert_internal(Relation index, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno, size_t floatvectoriIndex);
extern IndexScanDesc roarbeginscan_internal(Relation index, int nkeys, int norderbys);
extern void roarrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern void roarendscan_internal(IndexScanDesc scan);
extern bool roargettuple_internal(IndexScanDesc scan); 

typedef struct ROARPageOpaque {
    uint16      unused;
    uint16      page_id;
} ROARPageOpaque;

typedef struct ROARMetaPage {
    uint32          magicNumber;
    Metric          metric;
    uint32          dimensions;
    uint32          version;
    size_t          num_data;
    uint32          entry_point;
    BlockNumber     data_meta_blkno;
    BlockNumber     edges_meta_blkno;
} ROARMetaPage;

typedef struct ROAROptions
{
    int32       vl_len_;        
    int         m;              
    int         efConstruction; 
    int         parallel_workers;
    int         num_ground_truth;
}           ROAROptions;

struct ROARBuildState : public BaseObject
{
    Relation    heap;
    Relation    index;
    IndexInfo  *indexInfo;
    ForkNumber  forkNum;
    BlockNumber data_meta_blkno;
    BlockNumber edges_meta_blkno; 

    int         dimensions;
    int         m;
    int         efConstruction;
    int         parallel_workers;
    int         maintenance_work_mem;
    size_t      num_ground_truth;
    size_t      num_data;
    uint32      entry_point;

    double      indtuples;
    double      reltuples;

    FloatVectorArray samples; 

    Vector<ItemPointerData> mem_heaptids;
    Vector<Vector<size_t>> mem_graph;

    FmgrInfo   *procinfo;
    Metric     metric;
    Oid         collation;
    ann_helper::distance_func func_ptr;

    MemoryContext roarCtx;
    ThreadId      roarCtxcreator;
    bool          roarCtxfreed;
    char    indexName[NAMEDATALEN + 1];
    char    partIndexName[NAMEDATALEN + 1];

    void destroy() {
        if (!roarCtxfreed && t_thrd.proc->sessMemorySessionid == roarCtxcreator) {
            MemoryContextDelete(roarCtx);
            roarCtxfreed = true;
        }
    }
};

struct BuildCallbackData {
    ROARBuildState &buildstate;
    Relation heap;
    uint32 *heap_mark;
};

void buildVamanaOnDisk(ROARBuildState &buildstate);
Vector<QueryNeighbor> SingleQuerySearch(Relation index, float *query, uint32 ef, Buffer meta_buf, ROARMetaPage *metaPage);
Vector<size_t> SemanticPrune(ROARBuildState &buildstate, CandidateQueue &cq, uint32 tgt_id, bool disk, Relation index = InvalidRelation);
int ROARGetM(Relation index);

#endif /* ROAR_H */
