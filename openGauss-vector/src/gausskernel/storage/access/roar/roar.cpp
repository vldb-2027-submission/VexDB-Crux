#include "access/roar/roar.h"
#include "access/hnsw/hnsw.h"
#include <boost/optional/optional.hpp>

using namespace ann_helper;
using namespace disk_container;

bool roarinsert_internal(Relation index, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno, size_t floatvectoriIndex);

int ROARGetM(Relation index)
{
    ROAROptions *opts = (ROAROptions *) index->rd_options;
    if (opts) {
        return opts->m;
    }
    return Default_M; // 在 roar.h 中建议设为 35 或 64
}

int ROARGetEfConstruction(Relation index)
{
    ROAROptions *opts = (ROAROptions *) index->rd_options;
    if (opts) {
        return opts->efConstruction;
    }
    return Default_efConstruction;
}

int ROARGetBuildParallel(Relation index)
{
    int defa = 15;
    ROAROptions * opts = (ROAROptions *) index->rd_options;
    if (opts) {
        return opts->parallel_workers;
    }
    return defa;
}

int ROARGetNumberGroundTruth(Relation index)
{
    ROAROptions *opts = (ROAROptions *) index->rd_options;
    if (opts) {
        return opts->num_ground_truth;
    }
    return Default_NumberGroundTruth;
}

void free_roar_ctx(ROARBuildState *buildstate)
{
    if (t_thrd.proc->sessMemorySessionid == buildstate->roarCtxcreator &&
        !buildstate->roarCtxfreed) {
        MemoryContextDelete(buildstate->roarCtx);
        buildstate->roarCtxfreed = true;
    }
}

// ==========================================
// 2. 状态初始化 (初始化内存容器)
// ==========================================
static void InitBuildState(ROARBuildState &buildstate, Relation heap, Relation index,
    IndexInfo *indexInfo, ForkNumber forkNum, int m, int efConstruction, int parallel_workers,
    int num_ground_truth, int maintenance_work_mem)
{
    buildstate.heap = heap;
    buildstate.index = index;
    buildstate.indexInfo = indexInfo;
    buildstate.forkNum = forkNum;

    buildstate.m = m;
    buildstate.efConstruction = efConstruction;
    buildstate.parallel_workers = parallel_workers;
    buildstate.maintenance_work_mem = maintenance_work_mem;
    buildstate.num_ground_truth = num_ground_truth;
    buildstate.num_data = 0;
    buildstate.dimensions = TupleDescAttr(index->rd_att, 0)->atttypmod;

    if (TupleDescAttr(index->rd_att, 0)->atttypid == VARBITOID) {
        elog(ERROR, "type not supported for ROAR index");
    }
    if (buildstate.dimensions < 0) {
        elog(ERROR, "column does not have dimensions");
    }
    if (buildstate.efConstruction < 2 * buildstate.m) {
        elog(ERROR, "ef_construction must be greater than or equal to 2 * m");
    }

    buildstate.reltuples = 0;
    buildstate.indtuples = 0;

    buildstate.procinfo = NULL;
    buildstate.metric = Metric::INNER_PRODUCT;
    buildstate.collation = INT4OID;
    buildstate.func_ptr = get_general_distance_func(Metric::INNER_PRODUCT, buildstate.dimensions);

    buildstate.roarCtx = AllocSetContextCreate(g_instance.diskann_cxt.vec_indexer_ctx,
        "ROAR build graph context", ALLOCSET_DEFAULT_SIZES, SHARED_CONTEXT);

    buildstate.roarCtxcreator = t_thrd.proc->sessMemorySessionid;
    buildstate.roarCtxfreed = false; 

    auto old_ctx = MemoryContextSwitchTo(buildstate.roarCtx);
    buildstate.samples = FloatVectorArrayInit(QUERYSIZE, buildstate.dimensions);
    
    // 【纯内存建图接管】初始化内存容器
    buildstate.mem_heaptids.clear();
    buildstate.mem_graph.clear();
    MemoryContextSwitchTo(old_ctx);
    
    create_vec_data(buildstate.index, true);
}

// ==========================================
// 3. 构建回调 (内存写入)
// ==========================================
static uint32 roar_count = 0;

static void BuildCallback(Relation index, HeapTuple hup, Datum *values, const bool *isnull,
    bool tupleIsAlive, void *state) {
    
    BuildCallbackData *data = (BuildCallbackData *)state;
    ROARBuildState &buildstate = data->buildstate;
    
    uint32 current_mark = *(data->heap_mark);
    ++(*data->heap_mark);

    if (current_mark < QUERYSIZE) {
        if (*(data->heap_mark) % 10000 == 0) {
            roar_count++;
            uint32 out_num = roar_count * 10000;
            elog(NOTICE, "Current progress: %u data points processed.", out_num);
        }

        FloatVector *vec = DatumGetFloatVector(values[0]);
        float *v = (float *)vec->x;
        FloatVectorArraySet(buildstate.samples, buildstate.samples->length, v);
        buildstate.samples->length++;
        
    } else {
        if (*(data->heap_mark) % 10000 == 0) {
            roar_count++;
            uint32 out_num = roar_count * 10000;
            elog(NOTICE, "Current progress: %u data points processed.", out_num);
        }

        FloatVector *vec = DatumGetFloatVector(values[0]);
        float *v = (float *)vec->x;
        
        // 1. 将底库向量写入 smgr 磁盘
        vec_write(index->rd_smgr, (buildstate.num_data * buildstate.dimensions) * sizeof(float),
            buildstate.dimensions * sizeof(float), (const char *)(v), false);
            
        // 2. 【纯内存建图接管】直接把 TID 和一个空的邻居表推入内存
        buildstate.mem_heaptids.push_back(hup->t_self);
        buildstate.mem_graph.push_back(Vector<size_t>());
        
        buildstate.num_data++;
    }
}

static int32 *get_nn_answer(ROARBuildState &buildstate)
{
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        elog(ERROR, "Could not open ground truth file: %s", filename);
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *fmap = mmap(NULL, fileSize, PROT_READ, MAP_SHARED, fileno(f), 0);
    if (fmap == MAP_FAILED) {
        fclose(f);
        elog(ERROR, "mmap failed for ground truth file");
    }

    uint32 ndata = *((uint32 *)fmap);
    uint32 nans = *((uint32 *)fmap + 1);
    Assert(buildstate.num_ground_truth <= nans);
    int *data = (int *)((uint32 *)fmap + 2);
    size_t allocSize = (size_t)sizeof(int32) * GTSIZE * buildstate.num_ground_truth;
    int32 *res = (int32 *)palloc0_huge(buildstate.roarCtx, allocSize);

    for (uint32 i = 0; i < GTSIZE; i++) {
        errno_t rc = memcpy_s(&res[i * buildstate.num_ground_truth], 
                              buildstate.num_ground_truth * sizeof(int32), 
                              &data[i * nans], 
                              buildstate.num_ground_truth * sizeof(int32));
        securec_check(rc, "\0", "\0");
    }

    munmap(fmap, fileSize);
    fclose(f);
    return res;
}

Vector<size_t> SemanticPrune(ROARBuildState &buildstate, CandidateQueue &cq, uint32 tgt_id, bool disk, Relation index) {
    if (!RelationIsValid(index) && disk) {
        index = buildstate.index;
    }
    
    uint32 M_pjbp = std::min((uint32)buildstate.m, (uint32)40); 
    
    Vector<QueryNeighbor> pivot_neighbors_candidates;
    UnorderedSet<size_t> pivot_neighbors_candidates_ids;
    
    while (cq.has_unexplored()) {
        QueryNeighbor neighbor = cq.pop_unexplored();
        if (!pivot_neighbors_candidates_ids.insert(neighbor.id).second || neighbor.id == tgt_id) {
            continue;
        }
        pivot_neighbors_candidates.push_back(neighbor);
    }
    pivot_neighbors_candidates_ids.destroy();

    Vector<size_t> result;
    result.reserve(M_pjbp * 2);

    if (pivot_neighbors_candidates.empty()) {
        pivot_neighbors_candidates.destroy();
        return result; 
    }

    std::sort(pivot_neighbors_candidates.begin(), pivot_neighbors_candidates.end());
    
    uint32 start = 0;
    result.push_back(pivot_neighbors_candidates[start].id);

    double alpha = 1.0; 
    for (uint32 pass = 0 ; pass < 2 ; pass++) {
        if (pass == 1) {
            alpha = 1.2; 
        }
        
        start = 0; 
        while (result.size() < M_pjbp && (++start) < pivot_neighbors_candidates.size()) {
            QueryNeighbor &p = pivot_neighbors_candidates[start];
            
            float *v_p = (float*)alloc_vector(buildstate.dimensions * sizeof(float));
            VecBuffer buf_p = vec_read_buffer(index, p.id, buildstate.dimensions * sizeof(float));
            memcpy(v_p, (float*)buf_p.get_vecbuf(), sizeof(float) * buildstate.dimensions);
            buf_p.release();
            
            bool occlude = false;
            for (size_t t = 0; t < result.size(); t++) {
                if (p.id == result[t]) {
                    occlude = true;
                    break;
                }
                
                VecBuffer buf_t = vec_read_buffer(index, result[t], buildstate.dimensions * sizeof(float));
                float *v_t = (float*)buf_t.get_vecbuf();
                float dist = buildstate.func_ptr(v_p, v_t, buildstate.dimensions);
                buf_t.release();
                
                if (alpha * dist < p.distance) {
                    occlude = true;
                    break;
                }
            }
            
            if (!occlude) {
                if (p.id != tgt_id && std::find(result.begin(), result.end(), p.id) == result.end()) {
                    result.push_back(p.id);
                }                    
            }
            free_vector(v_p);
        }
    }

    for (uint32 i = 0; i < pivot_neighbors_candidates.size(); i++) {
        if (result.size() >= M_pjbp) break;
        
        if (pivot_neighbors_candidates[i].id != tgt_id && 
            std::find(result.begin(), result.end(), pivot_neighbors_candidates[i].id) == result.end()) {
            result.push_back(pivot_neighbors_candidates[i].id);
        }
    }
    
    std::sort(result.begin(), result.end());
    pivot_neighbors_candidates.destroy();
    
    return result;
}


// =================================================================
// RoarGraph 核心：纯内存 Neighborhood-Aware Projection
// =================================================================
static void NeighborhoodAwareProjection(ROARBuildState &buildstate, uint32 query_id, int32* gt_array) {
    if (buildstate.num_ground_truth == 0) return;

    uint32 pivot_id = gt_array[0]; 
    CandidateQueue cq(buildstate.num_ground_truth * 2);

    VecBuffer buf_pivot = vec_read_buffer(buildstate.index, pivot_id, buildstate.dimensions * sizeof(float));
    float *v_pivot = (float*)buf_pivot.get_vecbuf();

    // 2. 从【内存图】提取 Pivot 已有的边
    for(size_t ex_id : buildstate.mem_graph[pivot_id]) {
        VecBuffer buf_ex = vec_read_buffer(buildstate.index, ex_id, buildstate.dimensions * sizeof(float));
        float dist = buildstate.func_ptr(v_pivot, (float*)buf_ex.get_vecbuf(), buildstate.dimensions);
        buf_ex.release();
        cq.emplace(ex_id, dist);
    }

    for (uint32 k = 1; k < buildstate.num_ground_truth; k++) {
        uint32 neighbor_id = gt_array[k];
        VecBuffer buf_neighbor = vec_read_buffer(buildstate.index, neighbor_id, buildstate.dimensions * sizeof(float));
        float *v_neighbor = (float*)buf_neighbor.get_vecbuf();
        float dist = buildstate.func_ptr(v_pivot, v_neighbor, buildstate.dimensions);
        buf_neighbor.release();
        cq.emplace(neighbor_id, dist);
    }

    Vector<size_t> pruned_list = SemanticPrune(buildstate, cq, pivot_id, true);
    cq.destroy();

    // 5. 【内存极速修改】更新 Pivot 的前向边
    buildstate.mem_graph[pivot_id].clear();
    for (size_t target : pruned_list) {
        buildstate.mem_graph[pivot_id].push_back(target);
    }
    buf_pivot.release();

    // 6. 尝试为这些 Neighbor 添加反向边
    for (uint32 i = 0; i < pruned_list.size(); i++) {
        uint32 neighbor_id = pruned_list[i];
        Vector<size_t> &reverse_edges = buildstate.mem_graph[neighbor_id];

        if (reverse_edges.size() < (size_t)buildstate.m) {
            if (std::find(reverse_edges.begin(), reverse_edges.end(), pivot_id) == reverse_edges.end()) {
                reverse_edges.push_back(pivot_id);
            }
        } else {
            CandidateQueue cq_rev(buildstate.m * 2);
            VecBuffer buf_n = vec_read_buffer(buildstate.index, neighbor_id, buildstate.dimensions * sizeof(float));
            float *v_neighbor = (float*)buf_n.get_vecbuf();

            // 压入已存在的边
            for (size_t existing_target : reverse_edges) {
                VecBuffer ibuf = vec_read_buffer(buildstate.index, existing_target, buildstate.dimensions * sizeof(float));
                float dist = buildstate.func_ptr(v_neighbor, (float*)ibuf.get_vecbuf(), buildstate.dimensions);
                ibuf.release();
                cq_rev.emplace(existing_target, dist);
            }
            // 压入试图新增的 pivot_id
            VecBuffer ibuf_p = vec_read_buffer(buildstate.index, pivot_id, buildstate.dimensions * sizeof(float));
            float dist_p = buildstate.func_ptr(v_neighbor, (float*)ibuf_p.get_vecbuf(), buildstate.dimensions);
            ibuf_p.release();
            cq_rev.emplace(pivot_id, dist_p);
            buf_n.release();

            Vector<size_t> rev_pruned = SemanticPrune(buildstate, cq_rev, neighbor_id, true);
            cq_rev.destroy();

            // 【内存极速修改】写回修剪后的反向边
            reverse_edges.clear();
            for (size_t edge : rev_pruned) {
                reverse_edges.push_back(edge);
            }
            rev_pruned.destroy();
        }
    }
    pruned_list.destroy();
}

static void Initialization(ROARBuildState &buildstate) {
    Timer timer(QUERYSIZE, 10'000);
    timer.report("  Enter ROAR Initialization (Bipartite Projection).");
    
    int32 *res = get_nn_answer(buildstate);
    timer.report("  Loaded Ground Truths into Memory.");

    for (size_t query_id = 0; query_id < buildstate.samples->length; ++query_id) {
        CHECK_FOR_INTERRUPTS();
        timer.report_loop("  Projecting Bipartite Graph");

        if (query_id >= GTSIZE) continue;

        int32* gt_for_query = &res[query_id * buildstate.num_ground_truth];
        NeighborhoodAwareProjection(buildstate, query_id, gt_for_query);
    }
    
    timer.report("  Neighborhood-Aware Projection Done");
    timer.destroy();
    pfree(res);
}

// =================================================================
// 1. 元数据管理精简
// =================================================================
void ROARInitMeta(Buffer buf, Page page) {
    PageInit(page, BufferGetPageSize(buf), sizeof(ROARPageOpaque));
    ROARPageGetOpaque(page)->page_id = ROAR_META_ID;
}

void CreateMetaPage(ROARBuildState &buildstate) {
    Buffer buf = AnnNewBuffer(buildstate.index, MAIN_FORKNUM);
    Page page = BufferGetPage(buf);
    ROARInitMeta(buf, page);
    ROARMetaPage *metaPage = ROARPageGetMeta(page);

    metaPage->magicNumber = ROAR_MAGIC_NUMBER;
    metaPage->metric = buildstate.metric;
    metaPage->dimensions = buildstate.dimensions;
    metaPage->version = ROAR_VERSION;
    metaPage->num_data = buildstate.num_data;

    buildstate.data_meta_blkno = DiskVector<Data_r>::get_disk_vector(buildstate.index, false);
    buildstate.edges_meta_blkno = DiskVector<Edges_r>::get_disk_vector(buildstate.index, false);

    metaPage->data_meta_blkno = buildstate.data_meta_blkno;
    metaPage->edges_meta_blkno = buildstate.edges_meta_blkno;

    ((PageHeader) page)->pd_lower = ((char *) metaPage + sizeof(ROARMetaPage)) - (char *) page;
    AnnCommitBuffer(buf);
}

void UpdateMetaPage(ROARBuildState &buildstate){
    Buffer buf = AnnLoadBufferExtended(buildstate.index, buildstate.forkNum, ROAR_METAPAGE_BLKNO);
    ROARMetaPage *metaPage = ROARPageGetMeta(BufferGetPage(buf));

    metaPage->entry_point = buildstate.entry_point;
    metaPage->num_data = buildstate.num_data;

    AnnCommitBuffer(buf);
}

// =================================================================
// 2. Connectivity Enhancement (纯内存直接操作)
// =================================================================

static void SearchMemGraph(ROARBuildState &buildstate, Vector<slock_t> &locks,
    Relation index, CandidateQueue &cq, const float *query_vec, uint32 query_id) {
    
    UnorderedSet<size_t> visited;
    visited.insert(buildstate.entry_point);
    VecBuffer buf_ep = vec_read_buffer(index, buildstate.entry_point, buildstate.dimensions * sizeof(float));
    float *ep = (float*)buf_ep.get_vecbuf();
    float dist = buildstate.func_ptr(ep, query_vec, buildstate.dimensions);
    buf_ep.release();
    
    cq.emplace(buildstate.entry_point, dist);
    
    while (cq.has_unexplored()) {
        QueryNeighbor curr = cq.pop_unexplored();
        size_t curr_id = curr.id; 
        
        SpinLockAcquire(&locks[curr_id]);
        Vector<size_t> neighbors = buildstate.mem_graph[curr_id];
        SpinLockRelease(&locks[curr_id]);
        
        if (!neighbors.empty()) {
            for (auto &n_id : neighbors) {
                if (!visited.insert(n_id).second || n_id == query_id) {
                    continue;
                }
                VecBuffer buf_n = vec_read_buffer(index, n_id, buildstate.dimensions * sizeof(float));
                float d = buildstate.func_ptr(query_vec, (float*)buf_n.get_vecbuf(), buildstate.dimensions);
                buf_n.release();
                cq.emplace(n_id, d);
            }
        }
        neighbors.destroy();
    }
    visited.destroy();
}

static void ConnectivityEnhancementHelper(ROARBuildState &buildstate, Vector<slock_t> &locks,
    Relation index, uint32 i) {
    
    VecBuffer buf_i = vec_read_buffer(index, i, buildstate.dimensions * sizeof(float));
    float *v_i = (float*)buf_i.get_vecbuf();

    CandidateQueue cq(buildstate.efConstruction);
    SearchMemGraph(buildstate, locks, index, cq, v_i, i);

    SpinLockAcquire(&locks[i]);
    Vector<size_t> existing_edges = buildstate.mem_graph[i];
    SpinLockRelease(&locks[i]);

    for (size_t n_id : existing_edges) {
        VecBuffer buf_n = vec_read_buffer(index, n_id, buildstate.dimensions * sizeof(float));
        float d = buildstate.func_ptr(v_i, (float*)buf_n.get_vecbuf(), buildstate.dimensions);
        buf_n.release();
        cq.insert(QueryNeighbor(n_id, d));
    }

    Vector<size_t> pruned_list = SemanticPrune(buildstate, cq, i, true, index);
    cq.destroy();

    SpinLockAcquire(&locks[i]);
    buildstate.mem_graph[i].clear();
    for (size_t edge : pruned_list) {
        buildstate.mem_graph[i].push_back(edge);
    }
    SpinLockRelease(&locks[i]);

    for (uint32 rev_id : pruned_list) {
        SpinLockAcquire(&locks[rev_id]);
        if (buildstate.mem_graph[rev_id].size() < (size_t)buildstate.m) {
            if (std::find(buildstate.mem_graph[rev_id].begin(), buildstate.mem_graph[rev_id].end(), i) == buildstate.mem_graph[rev_id].end()) {
                buildstate.mem_graph[rev_id].push_back(i);
            }
            SpinLockRelease(&locks[rev_id]);
        } else {
            Vector<size_t> rev_existing = buildstate.mem_graph[rev_id];
            SpinLockRelease(&locks[rev_id]);

            CandidateQueue cq_rev(buildstate.m * 2);
            VecBuffer buf_rev = vec_read_buffer(index, rev_id, buildstate.dimensions * sizeof(float));
            float *v_rev = (float*)buf_rev.get_vecbuf();

            float dist_i = buildstate.func_ptr(v_rev, v_i, buildstate.dimensions);
            cq_rev.insert(QueryNeighbor(i, dist_i));

            for (size_t ex_id : rev_existing) {
                VecBuffer buf_ex = vec_read_buffer(index, ex_id, buildstate.dimensions * sizeof(float));
                float d = buildstate.func_ptr(v_rev, (float*)buf_ex.get_vecbuf(), buildstate.dimensions);
                buf_ex.release();
                cq_rev.insert(QueryNeighbor(ex_id, d));
            }
            buf_rev.release();

            Vector<size_t> rev_pruned = SemanticPrune(buildstate, cq_rev, rev_id, true, index);
            cq_rev.destroy();

            SpinLockAcquire(&locks[rev_id]);
            buildstate.mem_graph[rev_id].clear();
            for (size_t edge : rev_pruned) {
                buildstate.mem_graph[rev_id].push_back(edge);
            }
            SpinLockRelease(&locks[rev_id]);

            rev_pruned.destroy();
            rev_existing.destroy();
        }
    }
    buf_i.release();
    pruned_list.destroy();
    existing_edges.destroy();
}

static void ConnectivityEnhancement(ROARBuildState &buildstate) {
    Timer timer(buildstate.num_data, 10'000);
    timer.report("  Enter RoarGraph Connectivity Enhancement");

    Vector<slock_t> locks(buildstate.num_data, '\0');
    for (auto &l : locks) {
        SpinLockInit(&l);
    }

    constexpr size_t nparallel = 16;
    INIT_TASK_RUNNER();
    LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(nparallel);

    const auto task = [&](int, int64 start, int64 end) -> void {
        Relation index = buildstate.index;
        MemoryContext old_ctx = CurrentMemoryContext;
        if (IsBgWorkerProcess()) {
            index = index_open(index->rd_id, NoLock);
            old_ctx = MemoryContextSwitchTo(buildstate.roarCtx);
        }
        for (int i = start; i < end; i++) {
            if (uint32(i) == buildstate.entry_point) continue;
            CHECK_FOR_INTERRUPTS();
            ConnectivityEnhancementHelper(buildstate, locks, index, i);
            timer.inc_loop_count_forground_report("  Enhancing Base Nodes");
        }
        if (IsBgWorkerProcess()) {
            MemoryContextSwitchTo(old_ctx);
            index_close(index, NoLock);
        }
    };

    START_TASK_POOL();
    PARALLEL_BATCH_RUN_INIT();
    PARALLEL_BATCH_RUN_TASK_WAIT(buildstate.num_data, int(nparallel), task);
    END_TASK_POOL();
    DESTROY_TASK_RUNNER();

    ConnectivityEnhancementHelper(buildstate, locks, buildstate.index, buildstate.entry_point);

    for (auto &l : locks) {
        SpinLockFree(&l);
    }
    locks.destroy();
    timer.report("  Connectivity Enhancement Complete");
    timer.destroy();
}

static void compute_entry_point(ROARBuildState &buildstate) {
    Timer timer;
    timer.report("  Calculate entry point start");
    uint32 dim = buildstate.dimensions;
    size_t size = buildstate.num_data;

    constexpr size_t block_size = 1024ul;
    float *buf = (float *)palloc0(sizeof(float) * dim * block_size);
    float *center = (float *)palloc0(sizeof(float) * dim);

    double *temp_center = (double *)palloc0(sizeof(double) * dim); 
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        vec_read(buildstate.index->rd_smgr, i * dim * sizeof(float), block * dim * sizeof(float), (char *)buf);
        for (size_t j = 0; j < block; ++j) {
            for (size_t k = 0; k < dim; ++k) {
                temp_center[k] += buf[j * dim + k];
            }
        }
    }
    for (size_t j = 0; j < dim; ++j) {
        center[j] = temp_center[j] / size;
    }
    pfree(temp_center);

    const auto func = get_general_distance_batch_func(buildstate.metric, dim);
    float *distances = (float *)palloc(sizeof(float) * size);
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        vec_read(buildstate.index->rd_smgr, i * dim * sizeof(float), block * dim * sizeof(float), (char *)buf);
        func(center, buf, dim, block, distances + i); 
    }

    pfree(buf);
    pfree(center);

    size_t min_idx = 0;
    float min_dist = distances[0];
    for (size_t i = 1ul; i < size; ++i) {
        if (distances[i] < min_dist) {
            min_idx = i;
            min_dist = distances[i];
        }
    }
    pfree(distances);
    timer.report("  Calculate entry point done");
    timer.destroy();
    
    buildstate.entry_point = min_idx;
}

// =================================================================
// 新增：终极落盘函数 (Flush Graph to Disk)
// =================================================================
static void FlushGraphToDisk(ROARBuildState &buildstate) {
    Timer timer(buildstate.num_data, 10'000);
    timer.report("  Enter Final Graph Compaction and Flush.");

    DiskVector<Data_r> data_points(buildstate.index, buildstate.data_meta_blkno, false);
    DiskVector<Edges_r> edges_disk(buildstate.index, buildstate.edges_meta_blkno, false);

    for (uint32 i = 0; i < buildstate.num_data; i++) {
        timer.report_loop("  Flushing nodes to disk");

        // 1. 获取当前节点内存中的所有邻居
        Vector<size_t> &mem_edges = buildstate.mem_graph[i];
        
        // 2. 构造 Edges_r 并写入磁盘
        Edges_r e;
        e.edge_r_num = std::min((size_t)40, mem_edges.size()); 
        for (uint16 j = 0; j < e.edge_r_num; j++) {
            e.edges_r[j].data_offset = (uint32_t)mem_edges[j];
        }
        uint32_t edge_offset = edges_disk.push_back(e);

        // 3. 构造 Data_r (关联 TID 和边表 Offset) 并写入磁盘
        Data_r d;
        d.heapTid = buildstate.mem_heaptids[i];
        d.edge_r_offset = edge_offset;
        data_points.push_back(d);

        // 释放已落盘的边数组内存
        mem_edges.destroy();
    }
    
    data_points.destroy();
    edges_disk.destroy();
    buildstate.mem_graph.destroy();
    buildstate.mem_heaptids.destroy();

    timer.report("  Final Graph Compaction and Flush Done");
    timer.destroy();
}

// =================================================================
// 3. 构建索引主入口精简
// =================================================================
void BuildROARIndex(Relation heap, Relation index, IndexInfo *indexInfo, ForkNumber forkNum, int m,
    int efConstruction, int parallel_workers, int maintenance_work_mem,
    int num_ground_truth, double *reltuples, double *indtuples) {
    
    ROARBuildState buildstate;
    InitBuildState(buildstate, heap, index, indexInfo, MAIN_FORKNUM, ROARGetM(index), 
        ROARGetEfConstruction(index), ROARGetBuildParallel(index), 
        ROARGetNumberGroundTruth(index), u_sess->attr.attr_memory.maintenance_work_mem);
    
    CreateMetaPage(buildstate);

    auto old_ctx = MemoryContextSwitchTo(buildstate.roarCtx);
    uint32 heap_mark = 0;
    BuildCallbackData data = {buildstate, heap, &heap_mark};
    
    buildstate.reltuples = IndexBuildHeapScan(heap, index, indexInfo, true, BuildCallback, &data, NULL);

    compute_entry_point(buildstate);
    
    // Phase 1: 投影二分图 (基于 Ground Truth 连边)
    Initialization(buildstate);
    
    // Phase 2: 全局连通性增强 (寻找孤岛节点补充边)
    ConnectivityEnhancement(buildstate);
    
    UpdateMetaPage(buildstate);

    // ========================================================
    // 新增：调用一波流刷盘
    FlushGraphToDisk(buildstate);
    // ========================================================

    log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
                      true, RM_HNSW_ID, XLOG_BM25_BUILD_INDEX, NULL, true);
    
    MemoryContextSwitchTo(old_ctx);
    *reltuples = buildstate.reltuples;
    *indtuples = buildstate.num_data;
    buildstate.destroy();
}

IndexBuildResult *roarbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo) {
    double reltuples, indtuples;
    BuildROARIndex(heap, index, indexInfo, MAIN_FORKNUM, ROARGetM(index), 
        ROARGetEfConstruction(index), ROARGetBuildParallel(index), 
        u_sess->attr.attr_memory.maintenance_work_mem,
        ROARGetNumberGroundTruth(index),
        &reltuples, &indtuples);
        
    IndexBuildResult *res = (IndexBuildResult *)palloc0(sizeof(IndexBuildResult));
    res->heap_tuples = reltuples;
    res->index_tuples = indtuples;
    return res;
}