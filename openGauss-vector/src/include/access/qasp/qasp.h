#ifndef QASP_H
#define QASP_H

#include "postgres.h"
#include "access/genam.h"
#include <vtl/disk_container/diskvector.hpp>
#include "access/qasp/qasp_types.h"
#include <vtl/vector>
#include <unordered_map>

#include "access/annvector/distance/distance.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/module/timer.h"
#include "access/annvector/annkmeans.h"
#include "access/index_backend/taskpool.h"
#include "access/diskann/diskann_internal.h"
#include "access/annvector/store/vector_smgr.h"

#define QASP_MAGIC_NUMBER                0x51415350
#define QASP_VERSION                     1
#define NUM_SEMANTIC_CLUSTER             20
#define QASP_PAGE_ID                     0xFFA0u
#define QASP_META_ID                     0xFFA1u
#define QASP_METAPAGE_BLKNO              0

#define QASPPageGetOpaque(page)  ((QASPPageOpaque *) PageGetSpecialPointer(page))
#define QASPPageGetMeta(page)    ((QASPMetaPage *) PageGetContents(page))

// Note: The training size and data size of laion-10M is 10004480, not 10000000

#define VEC_DIM 200
#define QUERYSIZE 1000000
#define GTSIZE 1000000
constexpr char *filename = "/home/fanlu/workspace/simple_exp/data_repair_formal/t2i-10M/gt.t2b.A.bin";

#define Default_M 17
#define Act_cluster_num 4
#define Default_Cluster_num 20
#define Default_efConstruction 500
#define Default_NumberGroundTruth 100

extern IndexBuildResult *qaspbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo);
extern bool qaspinsert_internal(Relation index, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno, size_t floatvectoriIndex);
extern IndexScanDesc qaspbeginscan_internal(Relation index, int nkeys, int norderbys);
extern void qasprescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern void qaspendscan_internal(IndexScanDesc scan);
extern bool qaspgettuple_internal(IndexScanDesc scan, uint32 num_semantic_activated);

typedef struct QASPPageOpaque {
    uint16      unused;
    uint16      page_id;
} QASPPageOpaque;

typedef struct QASPMetaPage {
	uint32          magicNumber;
    Metric          metric;
    uint32          dimensions;
    uint32          version;
	uint32          num_semantic_cluster;
	size_t 			num_data;
	uint32          entry_point;
	uint32			multi_ep[NUM_SEMANTIC_CLUSTER];
	BlockNumber centers_meta_blkno;
	BlockNumber scan_data_meta_blkno;
    BlockNumber base_edges_meta_blkno;
    BlockNumber overflow_buckets_meta_blkno;
    BlockNumber edgeNumReminder_meta_blkno;
	BlockNumber logical_to_physical_meta_blkno;

	BlockNumber query_vec_meta_blkno;          
    BlockNumber upper_index_edges_meta_blkno;

} QASPMetaPage;

struct SearchStats {
    uint32 total_hops;          // 总步数 (Plateau 分母)
    uint32 visited_count;       // 总访问节点数 (Efficiency 分母)
    uint32 last_update_step;    // 最优距离最后一次更新的步数 (用于计算 Plateau)
    float  best_distance;       // 当前搜索到的最优距离 (用于判断更新)
};

/* QASP index options */
typedef struct QASPOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			m;				
	int			efConstruction; 
	int 		parallel_workers;
	int 		num_semantic_cluster;
	int 		num_ground_truth;
}			QASPOptions;

struct QASPBuildState : public BaseObject
{
	/* Info */
	Relation	heap;
	Relation	index;
	IndexInfo  *indexInfo;
	ForkNumber	forkNum;
	BlockNumber query_meta_blkno;
	BlockNumber upper_index_edges_meta_blkno;
	BlockNumber centers_meta_blkno;
	BlockNumber query_vec_meta_blkno;
	BlockNumber query_data_neighborhood_meta_blkno;
	BlockNumber edgeNumReminder_meta_blkno;
    BlockNumber scan_data_meta_blkno;
    BlockNumber base_edges_meta_blkno;
    BlockNumber overflow_buckets_meta_blkno;
	BlockNumber logical_to_physical_meta_blkno;

	/* Settings */
	int			dimensions;
	int			m;
	int			efConstruction;
	int 		parallel_workers;
	int 		maintenance_work_mem;
	size_t 		ncluster;
	size_t 		num_ground_truth;
	size_t 		num_data;
	uint32 	entry_point;
	Vector<Vector<uint32>> query_per_cluster;
	Vector<uint32> query_cluster_map;
	Vector<uint32> multi_entry;

	/* Statistics */
	double		indtuples;
	double		reltuples;

	/* Variables */
	FloatVectorArray samples;
	FloatVectorArray centers;
    Vector<BitSet<20ul>> mem_clusters;
    Vector<ItemPointerData> mem_heaptids;
    Vector<Edges*> mem_graph;

	std::vector<std::vector<uint32_t>> mem_query_graph;

	std::vector<std::unordered_map<uint32_t, uint32_t>> global_edge_heat;

	/* Support functions */
	FmgrInfo   *procinfo;
	Metric     metric;
	Oid			collation;
	ann_helper::distance_func func_ptr;

	/* Memory */
	MemoryContext qaspCtx;
	ThreadId      qaspCtxcreator;
	bool          qaspCtxfreed;
	char	indexName[NAMEDATALEN + 1];
	char	partIndexName[NAMEDATALEN + 1];

	void destroy() {
		ann_helper::optional_destroy(query_per_cluster);
		ann_helper::optional_destroy(query_cluster_map);
		ann_helper::optional_destroy(multi_entry);

		global_edge_heat.clear();
		std::vector<std::unordered_map<uint32_t, uint32_t>>().swap(global_edge_heat);

		mem_query_graph.clear();
		std::vector<std::vector<uint32_t>>().swap(mem_query_graph);

		if (!qaspCtxfreed && t_thrd.proc->sessMemorySessionid == qaspCtxcreator) {
			MemoryContextDelete(qaspCtx);
			qaspCtxfreed = true;
		}
	}
};

struct BuildCallbackData {
    QASPBuildState &buildstate;
    Relation heap;
	uint32 *heap_mark;
};

void buildVamanaOnDisk(QASPBuildState &buildstate);
Vector<QueryNeighbor> SingleQuerySearch(Relation index, float *query, uint32 ef, uint32 num_semantic_activated, Buffer meta_buf, QASPMetaPage *metaPage);
Vector<size_t> SemanticPrune(QASPBuildState &buildstate, CandidateQueue &cq, uint32 tgt_id, bool disk, Relation index = InvalidRelation);
int QASPGetM(Relation index);
void approxSingleQueryRepair(float* ood_query, QASPMetaPage *metaPage, Buffer meta_buf, Relation index, uint32 num_ground_truth, uint32 repair_ef);
Vector<QueryNeighbor> SingleQuerySearch_repair(Relation index, float *query, uint32 ef, uint32 num_semantic_activated, Buffer meta_buf, QASPMetaPage *metaPage, SearchStats *stats);
void repairSingleWithGT(Relation index, QASPMetaPage *metaPage, uint32 pivot_id, int32* neighbors, uint32 num_neighbors);

#endif /* EXP_H */