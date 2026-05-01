#ifndef IVF_H
#define IVF_H

#include <vtl/hashtable>

#include "postgres.h"
#include "access/reloptions.h"
#include "nodes/execnodes.h"
#include "port.h"				/* for strtof() and random() */
#include "lib/pairingheap.h"
#include "utils/tuplesort.h"
#include "access/annvector/floatvector.h"
#include "access/annvector/pq.h"
#include "access/diskann/vector_bt.h"
#include "access/annvector/module/timer.h"

using namespace ann_helper;

#ifdef IVF_BENCH
#include "portability/instr_time.h"
#endif

#define IVF_MAX_DIM 2000

/* Support functions */
#define IVF_DISTANCE_PROC 1
#define IVF_NORM_PROC 2
#define IVF_KMEANS_DISTANCE_PROC 3
#define IVF_KMEANS_NORM_PROC 4
#define IVF_PQ_KMEANS_DISTANCE_PROC 5

#define IVFFLAT_VERSION_OLD	1
#define IVFPQ_VERSION_OLD	1

#define IVFFLAT_VERSION_NEW	2
#define IVFPQ_VERSION_NEW	3
#define IVF_MAGIC_NUMBER 0x14FF1A7
#define IVF_PAGE_ID	0xFF84

/* Preserved page numbers */
#define IVF_METAPAGE_BLKNO	0
#define IVF_HEAD_BLKNO		1	/* first list page */

#define IVF_DEFAULT_LISTS	100
#define IVF_MAX_LISTS		32768

#define IVFPQ_DEFAULT_NUM_SUBQUANTIZERS         8
#define IVFPQ_DEFAULT_NBITS     8  

#define IVF_LIST_SIZE(_dim)	(offsetof(IvfListData, center) + FLOATVECTOR_SIZE(_dim))

#define IvfPageGetOpaque(page)	((IvfPageOpaque) PageGetSpecialPointer(page))
#define IvfflatPageGetMeta(page)	((IvfflatMetaPageData *) PageGetContents(page))
#define IvfPQPageGetMeta(page)	((IvfPQMetaPageData *) PageGetContents(page))
#define IvfPageGetMeta(page)	((IvfMetaPageData *) PageGetContents(page))

#ifdef IVF_BENCH
#define IvfBench(name, code) \
	do { \
		instr_time	start; \
		instr_time	duration; \
		INSTR_TIME_SET_CURRENT(start); \
		(code); \
		INSTR_TIME_SET_CURRENT(duration); \
		INSTR_TIME_SUBTRACT(duration, start); \
		elog(INFO, "%s: %.3f ms", name, INSTR_TIME_GET_MILLISEC(duration)); \
	} while (0)
#else
#define IvfBench(name, code) (code)
#endif

#define RandomDouble() (((double) random()) / MAX_RANDOM_VALUE)
#define RandomInt() random()

/* Variables */
// extern int	ivfflat_probes;

/* Exported functions */
extern "C" void _PG_init(void);

typedef struct IvfShared
{
	/* Immutable state */
	Oid			heaprelid;
	Oid			indexrelid;
	Oid 		heappartid;
	Oid 		indexpartid;
	bool		isconcurrent;
	int			scantuplesortstates;

	/* Mutex for mutable state */
	slock_t		mutex;

	/* Mutable state */
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;
	bool		ispq;
	int 		maintenanceWorkMem;

#ifdef IVFFLAT_KMEANS_DEBUG
	double		inertia;
#endif
	Sharedsort *sharedsort;
	char       *ivfcenters;
	Timer *timer;

    /*
     * This variable-sized field must come last.
     */
    ParallelHeapScanDescData heapdesc;
}			IvfShared;


typedef struct IvfLeader
{
	int			nparticipanttuplesorts;
	IvfShared   *ivfshared;
}			IvfLeader;

typedef struct IvfSpool
{
	Tuplesortstate *sortstate;
	Relation	heap;
	Relation	index;
}			IvfSpool;

typedef struct ListInfo
{
	BlockNumber blkno;
	OffsetNumber offno;
}			ListInfo;

/* IVFFlat index options */
typedef struct IvfOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int 		storage_type;
	int			lists;			/* number of lists */
	int 		secondary_lists;
	int 		compress;
	bool        enable_toast;
	int			parallel_workers;
}			IvfOptions;

typedef IvfOptions IvfflatOptions;

typedef struct IvfPQOptions : IvfOptions
{
	int         num_subquantizers;     //number of subquantizers
    int         nbits; //number of bits per quantization index
	bool        enable_performance_mode;
	bool		by_residual;
}			IvfPQOptions;

enum IvfVacuumDelMethod {
	DELETE_PHYSICAL = 0,
	DELETE_MARK_UNUSED = 1,
};

typedef struct IvfprocInfos 

{
	/* Support functions */
	distance_func   procinfo;
	distance_func   kmeansprocinfo;
	distance_func   normprocinfo;
	distance_func   kmeansnormprocinfo;
	distance_func   pqkmeansprocinfo;
} 			IvfprocInfos;

typedef struct IvfBuildState
{
	/* Info */
	Relation	heap;
	Relation	index;
	IndexInfo  *indexInfo;

	/* Settings */
	int			dimensions;
	int			lists;

	/* Statistics */
	double		indtuples;
	double		reltuples;

	/* Support functions */
	distance_func   procinfo;
	distance_func   kmeansprocinfo;
	distance_func   normprocinfo;
	distance_func   kmeansnormprocinfo;
	distance_func   pqkmeansprocinfo;

	/* Variables */
	FloatVectorArray samples;
	FloatVectorArray centers;
	ListInfo   *listInfo;
	FloatVector	   *normvec;

#ifdef IVF_KMEANS_DEBUG
	double		inertia;
	double	   *listSums;
	int		   *listCounts;
#endif

	/* Sorting */
	Tuplesortstate *sortstate;
	TupleDesc	tupdesc;
	TupleDesc   vectorTupdesc;
	TupleTableSlot *slot;

	/* Memory */
	MemoryContext tmpCtx;
	bool ispq;
	Metric metric;
	
	/* Parallel builds */
	IvfLeader *ivfleader;
	bool skipkmeansnormsample;

	vector_pair_vector *vectorIds;
	bool enableToast;
	int  parallelworkers;
	int  maintenanceWorkMem;
	bool vecBufMode;
	size_t curVecId; /*pay attention to parallel build case*/
	char	indexName[NAMEDATALEN + 1];
	char	partIndexName[NAMEDATALEN + 1];

}			IvfBuildState;

typedef struct IvfMetaPageData
{
	uint32		magicNumber;
	uint32		version;
	uint16		dimensions;
	uint16		lists;
}			IvfMetaPageData;

typedef IvfMetaPageData * IvfMetaPage;

typedef struct IvfflatMetaPageData : IvfMetaPageData
{
	BlockNumber listFirstPage;
	bool		enableToast;
	bool		vecBufMode;
}			IvfflatMetaPageData;
typedef IvfflatMetaPageData * IvfflatMetaPage;

typedef struct IvfPQMetaPageData : IvfMetaPageData
{
	bool by_residual;
	size_t num_subquantizers;
	size_t nbits;
	BlockNumber pq_codebook_start_page;
	BlockNumber listFirstPage;
	bool perfEnabled;
	bool enableToast;
}			IvfPQMetaPageData;

typedef IvfPQMetaPageData * IvfPQMetaPage;

typedef struct IvfPageOpaqueData
{
	BlockNumber nextblkno;
	uint16		unused;
	uint16		page_id;		/* for identification of IVFFlat indexes */
}			IvfPageOpaqueData;

typedef IvfPageOpaqueData * IvfPageOpaque;


typedef struct IvfListUpdateData
{
	BlockNumber startPage;
	BlockNumber insertPage;
	BlockNumber secondaryStartPage;
	BlockNumber secondaryInsertPage;
}			IvfListUpdateData;

typedef IvfListUpdateData * IvfListUpdate;

typedef struct IvfListData
{
	BlockNumber startPage;
	BlockNumber insertPage;
	BlockNumber secondaryStartPage;
	BlockNumber secondaryInsertPage;
	FloatVector		center;
}			IvfListData;

typedef IvfListData * IvfList;

typedef struct IvfInsertTupleCompositeData
{
	Item tuple;
	Item secondaryTuple;
}   IvfInsertTupleCompositeData;	

typedef IvfInsertTupleCompositeData * IvfInsertTupleComposite;

typedef struct IvfScanList
{
	pairingheap_node ph_node;
	BlockNumber startPage;
	double		distance;
	FloatVector	*center;
}			IvfScanList;

typedef struct IvfPQBuildState : IvfBuildState
{
    size_t num_subquantizers;     //number of subquantizers
    size_t nbits; //number of bits per quantization index
	ProductQuantizer *pq;
	bool by_residual;
	bool perfEnabled;
} IvfPQBuildState;


typedef struct IvfflatVecBufTupleData
{
    ItemPointerData htid;
	size_t 			vectorId;
}  IvfflatVecBufTupleData;

typedef IvfflatVecBufTupleData * IvfflatVecBufTuple;

typedef struct IvfPQIndexTupleBaseData
{
    ItemPointerData htid;
}  IvfPQIndexTupleBaseData;

typedef IvfPQIndexTupleBaseData * IvfPQIndexTupleBase;

typedef struct IvfPQIndexTupleData : IvfPQIndexTupleBaseData
{
	uint8_t codes[FLEXIBLE_ARRAY_MEMBER];
}          IvfPQIndexTupleData;

typedef IvfPQIndexTupleData * IvfPQIndexTuple;


typedef struct IvfPQIndexTuplePerfData : IvfPQIndexTupleBaseData
{
	ItemPointerData itid;
	uint8_t codes[FLEXIBLE_ARRAY_MEMBER];
}          IvfPQIndexTuplePerfData;

typedef IvfPQIndexTuplePerfData * IvfPQIndexTuplePerf;


typedef struct IvfInsertState
{
	/* Support functions */
	distance_func   procinfo;
	distance_func   normprocinfo;
}          IvfInsertState;

#define IVFPQINDEXTUPLEDATA_SIZE(code_size)		(offsetof(IvfPQIndexTupleData, codes) + sizeof(uint8_t)*(code_size))
#define IVFPQINDEXTUPLEPERFDATA_SIZE(code_size) (offsetof(IvfPQIndexTuplePerfData, codes) + sizeof(uint8_t)*(code_size))

extern IvfPQIndexTupleBase InitIvfPQIndexTuple(size_t code_size, bool perfMode);
extern size_t GetIvfPQTupleSize(size_t code_size, bool perfMode);
extern void IvfPQComputeCodes(float *vec, float *center, ProductQuantizer *pq, bool by_residual, IvfPQIndexTupleBase indexTuple, bool perfMode);
extern void PopulatePQCodeBookFromPages(Relation index, BlockNumber blockno, ProductQuantizer *pq);
/* Methods */
extern FmgrInfo   *IvfOptionalProcInfo(Relation rel, uint16 procnum);

extern IvfMetaPage IvfGetMetaPageData(Relation index, BlockNumber metablkno, bool ispq);
extern int IvfGetlists(Relation index, BlockNumber metablkno);
extern uint32 IvfGetMetaVersion(Relation index, BlockNumber metablkno);
extern void		IvfUpdateList(Relation index, ListInfo listInfo, IvfListUpdate listUpdateInfo, ForkNumber forkNum, bool isWal);
extern void     InvalidListUpdateData(IvfListUpdate listUpdate);
extern void		IvftCommitBuffer(Buffer buf);
extern void		IvfAppendPage(Relation index, Buffer *buf, Page *page, ForkNumber forkNum);
extern Buffer		IvfNewBuffer(Relation index, ForkNumber forkNum);
extern void		IvfInitPage(Buffer buf, Page page);
extern void		IvfInitRegisterPage(Relation index, Buffer *buf, Page *page);


/* the below functions should only use in build index stage to fetch the parameter, for scan and insert etc should get from metapage*/
extern int	  IvfGetLists(Relation index);
extern bool   IvfGetEnableToast(Relation index);
extern size_t IvfPQGetNumSubquantizers(Relation index);
extern size_t IvfPQGetNbits(Relation index);
extern bool   IvfPQPerformanceModeEnabled(Relation index);
extern bool   IvfPQByResidual(Relation index);
extern int    IvfGetParallelWorkers(Relation index);

/* end */

/*forward compatibility functions*/
extern BlockNumber IvfGetListFirstPage(IvfMetaPage meta, bool ispq); 
extern bool IvfMetaGetEnableToast(IvfMetaPage meta, Relation index);
extern bool IvfMetaGetPQPerfMode(IvfMetaPage meta, Relation index);

/* Index access methods */
extern IndexBuildResult *ivfflatbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo);
extern void		ivfflatbuildempty_internal(Relation index);
extern bool		ivfinsert_internal(Relation index, IvfInsertState *state, Datum *values, const bool *isnull, ItemPointer heap_tid, 
									bool ispq, BlockNumber metablkno = IVF_METAPAGE_BLKNO, size_t vectorId = 0);
extern IvfInsertState *createIvfInsertState(Relation index, Metric metric, bool ispq, BlockNumber metablkno = IVF_METAPAGE_BLKNO);
extern IndexBulkDeleteResult *ivfbulkdelete_internal(Relation index, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, 
														const void *callback_state, bool ispq, BlockNumber metablkno = IVF_METAPAGE_BLKNO);
extern IndexBulkDeleteResult *ivfvacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
extern IndexScanDesc ivfbeginscan_internal(Relation index, int nkeys, int norderbys, bool ispq, BlockNumber metablkno = IVF_METAPAGE_BLKNO);
extern void		ivfrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern bool		ivfgettuple_internal(IndexScanDesc scan, void *so_in, float *dist_out = NULL);
extern void *ivfcreatescanopaque(Relation index, Metric metric, bool ispq, BlockNumber metablkno, int probes_in, TupleDesc tupDesc);
extern void		ivfendscan_internal(void *so);


extern BlockNumber IvfflatBuildIndex(Relation heap, Relation index, IndexInfo *indexInfo, IvfBuildState * buildstate,
	ForkNumber forkNum, vector_pair_vector *vectorIds = NULL, int parallelWorkers = 0, int maintenanceWorkMem = 0);

extern BlockNumber IvfflatComputeCenters(Relation heap, Relation index, IndexInfo *indexInfo, IvfBuildState * buildstate,
	ForkNumber forkNum, vector_pair_vector *vectorIds = NULL, int parallelWorkers = 0, int maintenanceWorkMem = 0);						 

extern Datum ivfflathandler(PG_FUNCTION_ARGS);
extern Datum ivfflatbuild(PG_FUNCTION_ARGS);
extern Datum ivfflatbuildempty(PG_FUNCTION_ARGS);
extern Datum ivfflatinsert(PG_FUNCTION_ARGS);
extern Datum ivfflatbulkdelete(PG_FUNCTION_ARGS);
extern Datum ivfflatvacuumcleanup(PG_FUNCTION_ARGS);
extern Datum ivfflatcostestimate(PG_FUNCTION_ARGS);
extern Datum ivfflatoptions(PG_FUNCTION_ARGS);
extern Datum ivfflatvalidate(PG_FUNCTION_ARGS);
extern Datum ivfflatbeginscan(PG_FUNCTION_ARGS);
extern Datum ivfflatrescan(PG_FUNCTION_ARGS);
extern Datum ivfflatgettuple(PG_FUNCTION_ARGS);
extern Datum ivfflatendscan(PG_FUNCTION_ARGS);

extern Datum ivfpqbuild(PG_FUNCTION_ARGS);
extern Datum ivfpqbuildempty(PG_FUNCTION_ARGS);
extern Datum ivfpqinsert(PG_FUNCTION_ARGS);
extern Datum ivfpqbulkdelete(PG_FUNCTION_ARGS);
extern Datum ivfpqvacuumcleanup(PG_FUNCTION_ARGS);
extern Datum ivfpqcostestimate(PG_FUNCTION_ARGS);
extern Datum ivfpqoptions(PG_FUNCTION_ARGS);
extern Datum ivfpqvalidate(PG_FUNCTION_ARGS);
extern Datum ivfpqbeginscan(PG_FUNCTION_ARGS);
extern Datum ivfpqrescan(PG_FUNCTION_ARGS);
extern Datum ivfpqgettuple(PG_FUNCTION_ARGS);
extern Datum ivfpqendscan(PG_FUNCTION_ARGS);

extern Metric getIvfMetricType(Relation index, bool ispq);
extern IvfprocInfos *getIvfprocInfo(Metric metric, bool ispq, int dimensions);

extern void *ivfflat_inspect(Relation index);
extern void *ivfpq_inspect(Relation index);
extern void ivf_recycle_to_fsm(Relation index, BlockNumber metablkno);
extern void ivf_collect_index_page_blknos(Relation index, BlockNumber metablkno, Vector<BlockNumber> &indexblknos);

/* Index access methods */
extern IndexBuildResult *ivfpqbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo);
extern void		ivfpqbuildempty_internal(Relation index);
/*
 * WAL record definitions for ivf's WAL operations
 */
#define XLOG_IVF_UNLOG_BUILD_INDEX	0x00
#define XLOG_IVF_BUILD_INDEX		0x10
#define XLOG_IVF_EXTEND_NEWPAGES	0x20
#define XLOG_IVF_INSERT_INDEX		0x30
#define XLOG_IVF_DELETE_INDEX		0x40
#define XLOG_IVF_UPDATE_LIST		0x50
#define XLOG_IVF_UPDATE_PAGE_NEXTBLKNO	0x60

extern void ivf_redo(XLogReaderState *record);
extern void ivf_desc(StringInfo buf, XLogReaderState *record);
extern const char* ivf_type_name(uint8 subtype);
extern void IvfXLogNextBlkNo(Relation index, Buffer buffer, BlockNumber blkno);
extern void IvfXLogInsert(Size itemsz, OffsetNumber offsetNumber, OffsetNumber reuseOffno, Item itup, Buffer buf, Page page);
extern void IvfXLogUpdateList(OffsetNumber offsetNumber, IvfList list, Buffer buf, Page page);
extern void IvfXLogDelete(int ndeletable, OffsetNumber *deletable, Buffer buf, Page page, uint8_t flag);

typedef struct xl_ivf_insert {
	OffsetNumber offsetNumber;
	OffsetNumber reuseOffno;
	Size itemsz;
} xl_ivf_insert;

typedef struct xl_ivf_vacuum {
	uint8_t flag;
	int ndeletable;
} xl_ivf_vacuum;
#endif /* IVF_H */
