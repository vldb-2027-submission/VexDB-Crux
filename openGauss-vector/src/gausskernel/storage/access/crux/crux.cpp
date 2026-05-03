#include "access/crux/crux.h"
#include "access/hnsw/hnsw.h"
#include <boost/optional/optional.hpp>
#include <queue>

using namespace ann_helper;
using namespace disk_container;

bool cruxinsert_internal(Relation index, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno, size_t floatvectoriIndex);

int CRUXGetM(Relation index)
{
    CRUXOptions *opts = (CRUXOptions *) index->rd_options;
    if (opts) {
        return opts->m;
    }
    return Default_M;
}

int CRUXGetEfConstruction(Relation index)
{
    CRUXOptions *opts = (CRUXOptions *) index->rd_options;
    if (opts) {
        return opts->efConstruction;
    }
    return Default_efConstruction;
}

int CRUXGetBuildParallel(Relation index)
{
    int defa = 10;
    CRUXOptions * opts = (CRUXOptions *) index->rd_options;
    if (opts) {
        return opts->parallel_workers;
    }
    return defa;
}

int CRUXGetNumberCluster(Relation index)
{
    CRUXOptions *opts = (CRUXOptions *) index->rd_options;
    if (opts) {
        return opts->num_semantic_cluster;
    }
    return Default_Cluster_num;
}

int CRUXGetNumberGroundTruth(Relation index)
{
    CRUXOptions *opts = (CRUXOptions *) index->rd_options;
    if (opts) {
        return opts->num_ground_truth;
    }
    return Default_NumberGroundTruth;
}

void free_crux_ctx(CRUXBuildState *buildstate)
{
    if (t_thrd.proc->sessMemorySessionid == buildstate->cruxCtxcreator &&
        !buildstate->cruxCtxfreed) {
        MemoryContextDelete(buildstate->cruxCtx);
        buildstate->cruxCtxfreed = true;
    }
}

static void InitBuildState(CRUXBuildState &buildstate, Relation heap, Relation index,
    IndexInfo *indexInfo, ForkNumber forkNum, int m, int efConstruction, int parallel_workers,
    int num_semantic_cluster, int num_ground_truth, int maintenance_work_mem)
{
    buildstate.heap = heap;
    buildstate.index = index;
    buildstate.indexInfo = indexInfo;
    buildstate.forkNum = forkNum;

    buildstate.m = m;
    buildstate.efConstruction = efConstruction;
    buildstate.parallel_workers = parallel_workers;
    buildstate.maintenance_work_mem = maintenance_work_mem;
    buildstate.ncluster = num_semantic_cluster;
    buildstate.num_ground_truth = num_ground_truth;
    buildstate.num_data = 0;
    buildstate.dimensions = TupleDescAttr(index->rd_att, 0)->atttypmod;

    /* Disallow varbit since require fixed dimensions */
    if (TupleDescAttr(index->rd_att, 0)->atttypid == VARBITOID) {
        elog(ERROR, "type not supported for crux index");
    }

    /* Require column to have dimensions to be indexed */
    if (buildstate.dimensions < 0) {
        elog(ERROR, "column does not have dimensions");
    }

    if (buildstate.efConstruction < 2 * buildstate.m) {
        elog(ERROR, "ef_construction must be greater than or equal to 2 * m");
    }

    buildstate.reltuples = 0;
    buildstate.indtuples = 0;

    /* Get support functions */
    buildstate.procinfo = NULL;
    buildstate.metric = Metric::INNER_PRODUCT;
    buildstate.collation = INT4OID;
    buildstate.func_ptr = get_general_distance_func(Metric::INNER_PRODUCT, buildstate.dimensions);

    buildstate.cruxCtx = AllocSetContextCreate(g_instance.diskann_cxt.vec_indexer_ctx,
        "CRUX build graph context", ALLOCSET_DEFAULT_SIZES, SHARED_CONTEXT);

    buildstate.cruxCtxcreator = t_thrd.proc->sessMemorySessionid;
    buildstate.cruxCtxfreed = false; 

    auto old_ctx = MemoryContextSwitchTo(buildstate.cruxCtx);
    buildstate.centers = FloatVectorArrayInit(buildstate.ncluster, buildstate.dimensions);
    buildstate.samples = FloatVectorArrayInit(QUERYSIZE * 2, buildstate.dimensions);

    buildstate.mem_clusters.clear();
    buildstate.mem_heaptids.clear();
    buildstate.mem_graph.clear();

    MemoryContextSwitchTo(old_ctx);
    create_vec_data(buildstate.index, true);
}

static uint32 count = 0;

static void BuildCallback(Relation index, HeapTuple hup, Datum *values, const bool *isnull,
    bool tupleIsAlive, void *state) {
    BuildCallbackData *data = (BuildCallbackData *)state;
    CRUXBuildState &buildstate = data->buildstate;
    ++(*data->heap_mark);
    if (*(data->heap_mark) < QUERYSIZE) {
        
        if (*(data->heap_mark) %10000 == 0) {
            count ++;
            uint32 out_num = count * 10000;
            elog(NOTICE, "Current progress: %u data points processed.", count);
        }
        FloatVector *vec = DatumGetFloatVector(values[0]);
        float *v = (float *)vec->x;
        FloatVectorArraySet(buildstate.samples, buildstate.samples->length, v);
        buildstate.samples->length++;
    } else if (*(data->heap_mark) == QUERYSIZE) {
        FloatVector *vec = DatumGetFloatVector(values[0]);
        float *v = (float *)vec->x;
        FloatVectorArraySet(buildstate.samples, buildstate.samples->length, v);
        buildstate.samples->length++;
        AnnKmeansState *kmeanstate = (AnnKmeansState*)palloc0(sizeof(AnnKmeansState));
        elog(NOTICE, "Enter AnnKmeans.");
        setupKmeansState(buildstate.metric, index, kmeanstate, buildstate.dimensions, false, false);
        INIT_TASK_RUNNER();
        int parallelWorkers = buildstate.parallel_workers;
        if (parallelWorkers > 0) {
            LAUNCH_CONSUMER(parallelWorkers);
        }
        AnnKmeans(kmeanstate, buildstate.samples, buildstate.centers, buildstate.maintenance_work_mem, parallelWorkers);
        FREE_ANNKEMANSTATE(kmeanstate);
        DESTROY_TASK_RUNNER();
        elog(NOTICE, "Finish AnnKmeans.");
        DiskVector<float> centers(index, buildstate.centers_meta_blkno, false);
        for (uint32 i = 0; i < buildstate.ncluster; i++) {
            float *vec_center = FloatVectorArrayGet(buildstate.centers, i);
            centers.push_back_n(vec_center, buildstate.dimensions);
        }
        centers.destroy();
        // build query_cluster_map
        buildstate.query_per_cluster.resize(buildstate.ncluster);
        // DiskVector<QueryPoints> query_points(index, buildstate.query_meta_blkno, false);
        auto func = get_general_distance_func(Metric::INNER_PRODUCT, buildstate.dimensions);        
        for (int i = 0; i < buildstate.samples->length; i++) {
            float min_dist = FLT_MAX;
            uint32 min_cluster = 0;
            for (uint32 j = 0 ; j < buildstate.ncluster; j++) {
                float dist = func(FloatVectorArrayGet(buildstate.samples, i),
                    FloatVectorArrayGet(buildstate.centers, j), buildstate.dimensions);
                if (dist < min_dist) {
                    min_dist = dist;
                    min_cluster = j;
                }
            }
            // query_points.push_back(QueryPoints{(uint16)min_cluster});
            buildstate.query_cluster_map.push_back(min_cluster);
            buildstate.query_per_cluster[min_cluster].push_back(i);
        }
        // query_points.destroy();
        // compute the centroid of query cluster.
        for (uint32 i = 0; i < buildstate.ncluster; i++) {
            float * query_cluster_centroid = FloatVectorArrayGet(buildstate.centers, i);
            float min_dist = FLT_MAX;
            uint32 min_query_idx = 0;
            for (const auto &idx : buildstate.query_per_cluster[i]) {
                float *query_one = FloatVectorArrayGet(buildstate.samples, idx);
                float dist = func(query_cluster_centroid, query_one, buildstate.dimensions);
                if (dist < min_dist) {
                    min_dist = dist;
                    min_query_idx = idx;
                } 
            }
            buildstate.multi_entry.push_back(min_query_idx);
        }
    } else {
        if (*(data->heap_mark) % 10000 == 0) {
            count ++;
            uint32 out_num = count * 10000;
            elog(NOTICE, "Current progress: %u data points processed.", out_num);
        }
        FloatVector *vec = DatumGetFloatVector(values[0]);
        float *v = (float *)vec->x;
        vec_write(index->rd_smgr, (buildstate.num_data * buildstate.dimensions) * sizeof(float),
            buildstate.dimensions * sizeof(float), (const char *)(v), false);        
        // Data d{BitSet<20ul>(), hup->t_self};
        // // d.vec_offset = buildstate.num_data;
        // for (uint32 i = 0; i < buildstate.ncluster; i++) {
        //     // d.edge_offset[i] = (size_t)-1;
        //     d.edge_offset[i] = (uint32_t)-1;
        // }
        // DiskVector<Data> data_points(index, buildstate.data_meta_blkno, false);
        // data_points.push_back(d);
        // data_points.destroy();

        // 【纯内存建图接管区】
        buildstate.mem_clusters.push_back(BitSet<20ul>());
        buildstate.mem_heaptids.push_back(hup->t_self);
        
        // 在 CRUX 专用的内存上下文中，为当前节点一次性分配 20 个簇的定长边表数组，并清零
        Edges *node_edges = (Edges *)MemoryContextAllocZero(buildstate.cruxCtx, sizeof(Edges) * buildstate.ncluster);
        buildstate.mem_graph.push_back(node_edges);


        buildstate.num_data++;
    }
}

// ========================================================================
// 【新增】Step 3: 提取 Cross-Query KNN Edges (长程边跨模态连接)
// ========================================================================
static void CrossQueryKNNEdges(CRUXBuildState &buildstate, int32 *res) {
    Timer timer(QUERYSIZE, 10000);
    timer.report("  Enter Cross-query KNN Edges construction...");

    // 1. 读取步骤二中落盘的 Query Graph
    DiskVector<QuerySubIndexNeighbors> query_edges(buildstate.index, buildstate.upper_index_edges_meta_blkno, false);
    
    // 准备细粒度自旋锁，保护 mem_graph 的并发写入
    std::vector<slock_t> locks(buildstate.num_data);
    for(auto &l : locks) SpinLockInit(&l);

    // 2. 多线程遍历每个 query 及其邻居
    INIT_TASK_RUNNER();
    LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(buildstate.parallel_workers);
    const auto task = [&](int, int64 start, int64 end) -> void {
        for (int64 q = start; q < end; q++) {
            // p 是 q 的 pivot (即最近的数据点, KNN_1)
            uint32 p = res[q * buildstate.num_ground_truth]; 
            uint32 c_i = buildstate.query_cluster_map[q];    

            // 获取 Query Graph 中该 q 的所有邻居
            QuerySubIndexNeighbors q_neighbors = query_edges.get<AccessorLockType::ReadLock>(q);

            for (size_t edge_idx = 0; edge_idx < 30; edge_idx++) {
                uint32 q_j = q_neighbors.data_offset_subindex[edge_idx];
                if (q_j == (uint32)-1) break; // 遇到 -1 说明邻居表结束
                
                uint32 c_j = buildstate.query_cluster_map[q_j];
                uint32 p_j = res[q_j * buildstate.num_ground_truth]; // p_j 是 q_j 的 pivot

                // ----------------------------------------------------
                // 3. argmin 逻辑：正向寻找 u 
                // 目标：Find data vector u in KNN_k(q_j) that is nearest to query q
                // ----------------------------------------------------
                float min_dist_u = FLT_MAX;
                uint32 best_u = p_j; // 默认 fallback 到 p_j
                for (uint32 k = 0; k < buildstate.num_ground_truth; k++) {
                    uint32 candidate_u = res[q_j * buildstate.num_ground_truth + k];
                    VecBuffer buf_u = vec_read_buffer(buildstate.index, candidate_u, buildstate.dimensions * sizeof(float));
                    float dist = buildstate.func_ptr(FloatVectorArrayGet(buildstate.samples, q), (float*)buf_u.get_vecbuf(), buildstate.dimensions);
                    buf_u.release();
                    if (dist < min_dist_u) {
                        min_dist_u = dist;
                        best_u = candidate_u;
                    }
                }

                // ----------------------------------------------------
                // 3. argmin 逻辑：反向寻找 v
                // 目标：Find data vector v in KNN_k(q) that is nearest to query q_j
                // ----------------------------------------------------
                float min_dist_v = FLT_MAX;
                uint32 best_v = p; // 默认 fallback 到 p
                for (uint32 k = 0; k < buildstate.num_ground_truth; k++) {
                    uint32 candidate_v = res[q * buildstate.num_ground_truth + k];
                    VecBuffer buf_v = vec_read_buffer(buildstate.index, candidate_v, buildstate.dimensions * sizeof(float));
                    float dist = buildstate.func_ptr(FloatVectorArrayGet(buildstate.samples, q_j), (float*)buf_v.get_vecbuf(), buildstate.dimensions);
                    buf_v.release();
                    if (dist < min_dist_v) {
                        min_dist_v = dist;
                        best_v = candidate_v;
                    }
                }

                // ----------------------------------------------------
                // 4. 并发写入 mem_graph 并增加 GLO 热度
                // ----------------------------------------------------
                
                // 写入正向边 (p -> best_u)，归属 cluster c_i
                SpinLockAcquire(&locks[p]);
                Edges &edges_i = buildstate.mem_graph[p][c_i];
                if (edges_i.edge_num < 40) { // 判断容量限制
                    bool exists = false;
                    for (uint16 k = 0; k < edges_i.edge_num; k++) {
                        if (edges_i.edges[k].data_offset == best_u) { exists = true; break; }
                    }
                    if (!exists && best_u != p) {
                        edges_i.edges[edges_i.edge_num].data_offset = best_u;
                        edges_i.edge_num++;
                        // 增加 GLO 的热度权重 (权重设为 100，强调它是关键的跨模态桥梁)
                        buildstate.global_edge_heat[p][best_u] += 100; 
                    }
                }
                SpinLockRelease(&locks[p]);

                // 写入反向边 (p_j -> best_v)，归属 cluster c_j
                SpinLockAcquire(&locks[p_j]);
                Edges &edges_j = buildstate.mem_graph[p_j][c_j];
                if (edges_j.edge_num < 40) { // 判断容量限制
                    bool exists = false;
                    for (uint16 k = 0; k < edges_j.edge_num; k++) {
                        if (edges_j.edges[k].data_offset == best_v) { exists = true; break; }
                    }
                    if (!exists && best_v != p_j) {
                        edges_j.edges[edges_j.edge_num].data_offset = best_v;
                        edges_j.edge_num++;
                        // 增加 GLO 的热度权重
                        buildstate.global_edge_heat[p_j][best_v] += 100; 
                    }
                }
                SpinLockRelease(&locks[p_j]);
            }
            timer.inc_loop_count_forground_report("  Constructing Cross-Query Edges");
        }
    };
    START_TASK_POOL();
    PARALLEL_BATCH_RUN_INIT();
    PARALLEL_BATCH_RUN_TASK_WAIT(QUERYSIZE, buildstate.parallel_workers + 1, task);
    END_TASK_POOL();
    DESTROY_TASK_RUNNER();

    query_edges.destroy();
    for(auto &l : locks) SpinLockFree(&l);
    timer.report("  Cross-query KNN Edges Done");
    timer.destroy();
}

// ========================================================================
// 【新增】Step 2: 构建训练集的 Query Graph (内存 Vamana 并写入 DiskVector)
// ========================================================================
static void buildQueryGraph(CRUXBuildState &buildstate) {
    Timer timer(QUERYSIZE, 10000);
    timer.report("  Enter Build Query Graph...");

    uint32 num_queries = QUERYSIZE;
    std::vector<std::vector<uint32_t>> query_graph(num_queries);
    std::vector<slock_t> locks(num_queries);
    for(auto &l : locks) SpinLockInit(&l);

    // 1. 保存所有 Training Queries 到 DiskVector
    DiskVector<float> query_vecs(buildstate.index, buildstate.query_vec_meta_blkno, false);
    for (uint32 i = 0; i < num_queries; i++) {
        query_vecs.push_back_n(FloatVectorArrayGet(buildstate.samples, i), buildstate.dimensions);
    }
    query_vecs.destroy();
    timer.report("  Flushed Query Vectors to Disk");

    // 2. 多线程构建 Query Vamana 图
    uint32 query_L = 40; // 启发式搜索窗口 (L)
    uint32 query_R = 15; // 启发式最大出度 (R)
    uint32 ep = 0;       // 入口点默认选第0个query

    INIT_TASK_RUNNER();
    // 使用传入的 parallel_workers 控制并发度
    LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(buildstate.parallel_workers);
    const auto task = [&](int, int64 start, int64 end) -> void {
        for (int64 i = start; i < end; i++) {
            if (i == ep) continue;
            float* v_i = FloatVectorArrayGet(buildstate.samples, i);
            CandidateQueue cq(query_L);
            cq.emplace(ep, buildstate.func_ptr(v_i, FloatVectorArrayGet(buildstate.samples, ep), buildstate.dimensions));
            UnorderedSet<uint32> visited;
            visited.insert(ep);

            // 阶段 A: Greedy Search 寻找邻居候选集
            while (cq.has_unexplored()) {
                QueryNeighbor curr = cq.pop_unexplored();
                SpinLockAcquire(&locks[curr.id]);
                auto neighbors = query_graph[curr.id];
                SpinLockRelease(&locks[curr.id]);

                for (uint32 n_id : neighbors) {
                    if (visited.insert(n_id).second) {
                        float d = buildstate.func_ptr(v_i, FloatVectorArrayGet(buildstate.samples, n_id), buildstate.dimensions);
                        cq.emplace(n_id, d);
                    }
                }
            }
            visited.destroy();

            // 阶段 B: Robust Prune (基于三角不等式修剪冗余边)
            CandidateQueue cq_prune(query_L);
            for (const auto& x : cq) cq_prune.insert(x);

            std::vector<uint32> result;
            while (cq_prune.has_unexplored() && result.size() < query_R) {
                QueryNeighbor p = cq_prune.pop_unexplored();
                bool occlude = false;
                for (uint32 t : result) {
                    float dist = buildstate.func_ptr(FloatVectorArrayGet(buildstate.samples, p.id), FloatVectorArrayGet(buildstate.samples, t), buildstate.dimensions);
                    if (dist < p.distance) { occlude = true; break; }
                }
                if (!occlude) result.push_back(p.id);
            }

            SpinLockAcquire(&locks[i]);
            query_graph[i] = result;
            SpinLockRelease(&locks[i]);

            // 阶段 C: 添加反向边 (保持双向连通性，阈值放宽到 R*2)
            for (uint32 rev_id : result) {
                SpinLockAcquire(&locks[rev_id]);
                if (query_graph[rev_id].size() < query_R * 2) {
                    if (std::find(query_graph[rev_id].begin(), query_graph[rev_id].end(), i) == query_graph[rev_id].end()) {
                        query_graph[rev_id].push_back(i);
                    }
                }
                SpinLockRelease(&locks[rev_id]);
            }
            timer.inc_loop_count_forground_report("  Building Query Graph");
        }
    };
    START_TASK_POOL();
    PARALLEL_BATCH_RUN_INIT();
    PARALLEL_BATCH_RUN_TASK_WAIT(num_queries, buildstate.parallel_workers + 1, task);
    END_TASK_POOL();
    DESTROY_TASK_RUNNER();

    // 3. 将建好的 Query Graph 序列化并写入 DiskVector
    DiskVector<QuerySubIndexNeighbors> query_edges(buildstate.index, buildstate.upper_index_edges_meta_blkno, false);
    for (uint32 i = 0; i < num_queries; i++) {
        QuerySubIndexNeighbors q_neighbors;
        // 初始化为空 (-1)
        memset(q_neighbors.data_offset_subindex, -1, sizeof(q_neighbors.data_offset_subindex));
        // 最多存 30 条边（防越界），一般 prunning 后是 15
        for (size_t j = 0; j < std::min((size_t)30, query_graph[i].size()); j++) {
            q_neighbors.data_offset_subindex[j] = query_graph[i][j];
        }
        query_edges.push_back(q_neighbors);
    }
    query_edges.destroy();
    
    for(auto &l : locks) SpinLockFree(&l);
    timer.report("  Build Query Graph Done");
    timer.destroy();
}

Vector<size_t> SemanticPrune(CRUXBuildState &buildstate, CandidateQueue &cq, uint32 tgt_id, bool disk, Relation index) {
    if (!RelationIsValid(index) && disk) {
        index = buildstate.index;
    }
    uint32 M_pjbp = buildstate.m;
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

    std::sort(pivot_neighbors_candidates.begin(), pivot_neighbors_candidates.end());
    
    if (pivot_neighbors_candidates.empty()) {
        pivot_neighbors_candidates.destroy();
        return Vector<size_t>(); 
    }

    Vector<size_t> result;
    result.reserve(M_pjbp * 2);
    uint32 start = 0;
    result.push_back(pivot_neighbors_candidates[start].id);

    double alpha = 1;
    for (uint32 i = 0 ; i < 2 ; i++) {
        start = 0;
        while (result.size() < M_pjbp && (++start) < pivot_neighbors_candidates.size()) {
            QueryNeighbor &p = pivot_neighbors_candidates[start];
            float *v_p = NULL;
            if (disk) {
                v_p = (float*)alloc_vector(buildstate.dimensions * sizeof(float));
                VecBuffer buf = vec_read_buffer(index, p.id, buildstate.dimensions * sizeof(float));
                memcpy(v_p, (float*)buf.get_vecbuf(), sizeof(float) * buildstate.dimensions);
                buf.release();
            } else {
                v_p = FloatVectorArrayGet(buildstate.samples, p.id);
            }
            bool occlude = false;
            for (size_t t = 0; t < result.size(); t++) {
                if (p.id == result[t]) {
                    occlude = true;
                    break;
                }
                float dist;
                if (disk) {
                    VecBuffer buf_r = vec_read_buffer(index, result[t], buildstate.dimensions * sizeof(float));
                    float *v_t = (float *)buf_r.get_vecbuf();
                    dist = buildstate.func_ptr(v_p, v_t, buildstate.dimensions);
                    buf_r.release();
                } else {
                    float *v_t = FloatVectorArrayGet(buildstate.samples, result[t]);
                    dist = buildstate.func_ptr(v_p, v_t, buildstate.dimensions);
                }
                if (alpha * dist < p.distance) {
                    occlude = true;
                    break;
                }
            }
            if (!occlude) {
                if (p.id != tgt_id) {
                    if (std::find(result.begin(), result.end(), p.id) == result.end()) {
                        result.push_back(p.id);
                    }                    
                }
            }
            if (disk) {
                free_vector(v_p);
            }
        }
    }

    for (uint32 i = 0; i < pivot_neighbors_candidates.size(); i++) {
        if (result.size() < size_t(buildstate.m) &&
            std::find(result.begin(), result.end(), pivot_neighbors_candidates[i].id) == result.end()) {
            if (pivot_neighbors_candidates[i].id != tgt_id) {
                result.push_back(pivot_neighbors_candidates[i].id);
            }
        }
        if (result.size() >= size_t(buildstate.m))
            break;
    }
    std::sort(result.begin(), result.end());
    pivot_neighbors_candidates.destroy();
    return result;
}

static void SingleQueryProjection(CRUXBuildState &buildstate, CandidateQueue &cq, uint32 query_id, uint32 semantic_id, QueryNeighbor& pivot) {
    Timer timer;
    CandidateQueue candidate_projection(buildstate.num_ground_truth);
    VecBuffer buf = vec_read_buffer(buildstate.index, pivot.id, buildstate.dimensions * sizeof(float));
    float *v_pivot = (float*)buf.get_vecbuf();
    
    // 计算 distance(pivot, GT)，并直接在内存中打上 semantic 标签
    while (cq.has_unexplored()) {
        QueryNeighbor neighbor = cq.pop_unexplored();
        buildstate.mem_clusters[neighbor.id].set(semantic_id); // 【内存极速修改】
        VecBuffer buf_n = vec_read_buffer(buildstate.index, neighbor.id, buildstate.dimensions * sizeof(float));
        float *v = (float*)buf_n.get_vecbuf();
        float dist = buildstate.func_ptr(v_pivot, v, buildstate.dimensions);
        buf_n.release();
        QueryNeighbor qn(neighbor.id, dist);
        candidate_projection.insert(qn);
    }
    buf.release();
    
    Vector<size_t> pruned_list = SemanticPrune(buildstate, candidate_projection, pivot.id, true);
    candidate_projection.destroy();

    // 【内存极速写入】：正向边直接挂载
    Edges &pivot_edges = buildstate.mem_graph[pivot.id][semantic_id];
    pivot_edges.edge_num = pruned_list.size();
    Assert(pruned_list.size() < 30);
    for (uint32 i = 0; i < pruned_list.size(); i++) {
        uint32 target_id = (uint32_t)pruned_list[i];
        pivot_edges.edges[i].data_offset = target_id;
        
        // ========================================================
        // 【GLO Step 2 - 新增】：收集 Star 阶段正向边热度 (权重 100)
        buildstate.global_edge_heat[pivot.id][target_id] += 100;
        // ========================================================
    }

    // 【内存极速写入】：处理反向修剪逻辑
    for (uint32 i = 0 ; i < pruned_list.size(); i++) {
        uint32 neighbor_id = pruned_list[i];
        Edges &reverse_edges = buildstate.mem_graph[neighbor_id][semantic_id];
        
        // 如果邻居的出边还没满，直接无脑塞入反向边
        if (reverse_edges.edge_num < buildstate.m) {
            reverse_edges.edges[reverse_edges.edge_num].data_offset = pivot.id;
            reverse_edges.edge_num++;
            
            // ========================================================
            // 【GLO Step 2 - 新增】：收集 Star 阶段直接反向边热度 (权重 100)
            buildstate.global_edge_heat[neighbor_id][pivot.id] += 100;
            // ========================================================
            continue;
        }
        
        // 满了，触发 Vamana Prune (逻辑完全保留)
        CandidateQueue cq_rev(buildstate.m * 2);
        VecBuffer buf_n = vec_read_buffer(buildstate.index, neighbor_id, buildstate.dimensions * sizeof(float));
        float *v_neighbor = (float*)buf_n.get_vecbuf();

        for (uint32_t j = 0 ; j < reverse_edges.edge_num; j ++) {
            uint32 data_offset = reverse_edges.edges[j].data_offset;
            VecBuffer ibuf = vec_read_buffer(buildstate.index, data_offset, buildstate.dimensions * sizeof(float));
            float *v = (float*)ibuf.get_vecbuf();
            QueryNeighbor qn(data_offset, buildstate.func_ptr(v_neighbor, v, buildstate.dimensions));
            ibuf.release();
            cq_rev.insert(qn);
        }

        VecBuffer buf_pivot = vec_read_buffer(buildstate.index, pivot.id, buildstate.dimensions * sizeof(float));
        float *v_pivot = (float*)buf_pivot.get_vecbuf();
        QueryNeighbor qn_p(pivot.id, buildstate.func_ptr(v_neighbor, v_pivot, buildstate.dimensions));
        buf_pivot.release();
        buf_n.release();
        cq_rev.insert(qn_p);

        Vector<size_t> rev_pruned = SemanticPrune(buildstate, cq_rev, neighbor_id, true);
        cq_rev.destroy();
        
        // 写回修剪后的反向边
        reverse_edges.edge_num = rev_pruned.size();
        for (uint32 j = 0; j < reverse_edges.edge_num; j++) {
            uint32 target_id = (uint32_t)rev_pruned[j];
            reverse_edges.edges[j].data_offset = target_id;
            
            // ========================================================
            // 【GLO Step 2 - 新增】：收集 Star 阶段修剪后反向边热度 (权重 100)
            buildstate.global_edge_heat[neighbor_id][target_id] += 100;
            // ========================================================
        }
        rev_pruned.destroy();
    }
    pruned_list.destroy();
}

static int32 *get_nn_answer(CRUXBuildState &buildstate)
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
    int32 *res = (int32 *)palloc0_huge(buildstate.cruxCtx, allocSize);

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

static void Initialization(CRUXBuildState &buildstate) {
    Timer timer(QUERYSIZE, 10'000);
    timer.report("  enter Initialization.");
    int32 *res = get_nn_answer(buildstate);
    timer.report("  get nn answer done.");
    UnorderedSet<uint32> visited;
    for (uint32 i = 0; i < buildstate.query_per_cluster.size(); i++) {
        for (uint32 j = 0; j < buildstate.query_per_cluster[i].size(); j++) {
            CHECK_FOR_INTERRUPTS();
            timer.report_loop("  Build Projection Graph");
            size_t query_id = buildstate.query_per_cluster[i][j];
            if (query_id >= GTSIZE)
                continue;
            CandidateQueue cq(buildstate.num_ground_truth);
            const float *query = FloatVectorArrayGet(buildstate.samples, query_id);
            for (uint32 k = 0; k < buildstate.num_ground_truth; k++) {
                uint32 neighbor_id = res[query_id * buildstate.num_ground_truth + k];
                VecBuffer buf = vec_read_buffer(buildstate.index, neighbor_id, buildstate.dimensions * sizeof(float));
                float dist = buildstate.func_ptr(query, (float*)buf.get_vecbuf(), buildstate.dimensions);
                buf.release();
                cq.emplace(neighbor_id, dist);
            }
            for (uint32 k = 0; k < buildstate.multi_entry.size(); k++) {
                if (query_id == buildstate.multi_entry[k]) {
                    buildstate.multi_entry[k] = cq.top_id();
                    break;
                }
            }
            Vector<QueryNeighbor> temp_pivots;
            uint32 pivot_num = 5;
            for (uint32 k = 0 ; k < pivot_num; k ++) {
                if (cq.has_unexplored()) {
                    QueryNeighbor pivot = cq.pop_unexplored();
                    temp_pivots.push_back(pivot);
                }
            }
            for (uint32 k = 0 ; k < temp_pivots.size(); k ++) {
                cq.insert(temp_pivots[k]);
            }
            for (uint32 k = 0; k < temp_pivots.size(); k ++) {
                SingleQueryProjection(buildstate, cq, query_id, i, temp_pivots[k]);
            }
            temp_pivots.destroy();
            cq.destroy();
        }
    }
    visited.destroy();
    // DESTROY_TASK_RUNNER();
    timer.report("  Build Projection Graph Done");
    timer.destroy();

    // ==========================================================
    // 【新增 Step 4.1】：在完成传统的单点 Projection 后，启动 Cross-Query KNN Edges 机制
    CrossQueryKNNEdges(buildstate, res);
    // ==========================================================

    pfree(res);
}

// greedy search on the projection graph.
static void SearchProjectionGraphInternal(CRUXBuildState &buildstate, Vector<slock_t> &locks,
    const Vector<Vector<size_t>> &Projection_all_semantic, Relation index, CandidateQueue &cq,
    const float *query_vec, uint32 query_id) { // 【修改点：去掉了 batchIndex 和 thread_heats】
    
    UnorderedSet<size_t> visited;
    visited.insert(buildstate.entry_point);
    VecBuffer buf_ep = vec_read_buffer(index, buildstate.entry_point, buildstate.dimensions * sizeof(float));
    float *ep = (float*)buf_ep.get_vecbuf();
    float dist = buildstate.func_ptr(ep, query_vec, buildstate.dimensions);
    buf_ep.release();
    cq.emplace(buildstate.entry_point, dist);
    
    while (cq.has_unexplored()) {
        QueryNeighbor neighbor = cq.pop_unexplored();
        size_t curr_id = neighbor.id; 
        
        SpinLockAcquire(&locks[curr_id]);
        if (!Projection_all_semantic[curr_id].empty()) {
            for (auto &next_id : Projection_all_semantic[curr_id]) {
                if (!visited.insert(next_id).second || next_id == query_id) {
                    continue;
                }
                
                // ========================================================
                // 【绝妙修改：由于最外层持有 locks[curr_id]，这里绝对并发安全！】
                buildstate.global_edge_heat[curr_id][next_id] += 1;
                // ========================================================

                VecBuffer buf = vec_read_buffer(index, next_id, buildstate.dimensions * sizeof(float));
                float dist = buildstate.func_ptr(query_vec, (float*)buf.get_vecbuf(), buildstate.dimensions);
                buf.release();
                cq.emplace(next_id, dist);
            }
        }
        SpinLockRelease(&locks[curr_id]);
    }
    visited.destroy();
}

static void CE_partition(CRUXBuildState &buildstate, Vector<size_t> &pruned_list, uint32 query_id) {
    for (uint32 i = 0; i < pruned_list.size(); i++) {    
        uint32 target_id = (uint32)pruned_list[i];
        BitSet<20ul> &pruned_cluster = buildstate.mem_clusters[target_id];
        
        bool flag_skip = true; // 恢复 flag_skip
        
        for (uint32 j = 0; j < buildstate.ncluster; j++) {
            if (!pruned_cluster.get(j)) {
                continue;
            }
            
            Edges &edges_semantic = buildstate.mem_graph[query_id][j];
            bool flag = false;
            
            for (uint32 k = 0; k < edges_semantic.edge_num; k++) {
                if (edges_semantic.edges[k].data_offset == target_id || edges_semantic.edge_num == 40ul) {
                    flag = true;
                    flag_skip = false; // 找到归属并尝试处理，取消 fallback
                    break;
                }
            }
            if (flag) {
                continue;
            }
            
            flag_skip = false; // 成功加边，取消 fallback
            edges_semantic.edges[edges_semantic.edge_num].data_offset = target_id;
            edges_semantic.edge_num++;
        }
        if (flag_skip) {
            for (uint32 j = 0; j < buildstate.ncluster; j++) {
                Edges &edges_semantic = buildstate.mem_graph[query_id][j];
                bool flag = false;
                for (uint32 k = 0; k < edges_semantic.edge_num; k++) {
                    if (edges_semantic.edges[k].data_offset == target_id || edges_semantic.edge_num >= 40ul) {
                        flag = true;
                        break;
                    }
                }
                if (flag) {
                    continue;
                }
                edges_semantic.edges[edges_semantic.edge_num].data_offset = target_id;
                edges_semantic.edge_num++;
            }
        }
    }
}

// search + Robust Prune.
static Vector<size_t> search_neighbors(CRUXBuildState &buildstate, const float *v, Relation index,
    const Vector<Vector<size_t>> &Projection_all_semantic, Vector<slock_t> &locks, uint32 i) { // 【修改点：去掉额外参数】
    
    CandidateQueue cq(buildstate.efConstruction);
    // 【修改点：去掉额外参数】
    SearchProjectionGraphInternal(buildstate, locks, Projection_all_semantic, index, cq, v, i);
    CandidateQueue cq_prune(buildstate.efConstruction);
    for (const auto &x : cq) {
        if (x.id == i) {
            continue;
        }
        cq_prune.insert(x);
    }
    cq.destroy();
    Vector<size_t> pruned_list = SemanticPrune(buildstate, cq_prune, i, true, index);
    cq_prune.destroy();
    return pruned_list;
}

// add reverse for projection_all_semantic.
static void set_up_proj_semantic(CRUXBuildState &buildstate, const float *v, Vector<slock_t> &locks,
    Vector<Vector<size_t>> &Projection_all_semantic, Relation index, Vector<size_t> &pruned_list,
    uint32 i) {
    SpinLockAcquire(&locks[i]);
    Projection_all_semantic[i].push_back(pruned_list.cbegin(), pruned_list.cend());
    SpinLockRelease(&locks[i]);

    for (uint32 reverse_neighbor : pruned_list) {
        SpinLockAcquire(&locks[reverse_neighbor]);
        if (Projection_all_semantic[reverse_neighbor].size() < size_t(buildstate.m) * 2) {
            Projection_all_semantic[reverse_neighbor].push_back(i);
            SpinLockRelease(&locks[reverse_neighbor]);
            continue;
        }

        CandidateQueue cq_reverse_prune(buildstate.efConstruction);
        VecBuffer buf_r = vec_read_buffer(index, reverse_neighbor, buildstate.dimensions * sizeof(float));
        float* reverse_query = (float*)buf_r.get_vecbuf();
        cq_reverse_prune.emplace(i, buildstate.func_ptr(reverse_query, v, buildstate.dimensions));
        
        for (auto x: Projection_all_semantic[reverse_neighbor]) {
            VecBuffer buf_x = vec_read_buffer(index, x, buildstate.dimensions * sizeof(float));
            float dist = buildstate.func_ptr(reverse_query, (float*)buf_x.get_vecbuf(), buildstate.dimensions);
            buf_x.release();
            
            QueryNeighbor qn_neighbor(x, dist);
            cq_reverse_prune.insert(qn_neighbor);
        }
        Projection_all_semantic[reverse_neighbor] = SemanticPrune(buildstate, cq_reverse_prune, reverse_neighbor, true, index);
        SpinLockRelease(&locks[reverse_neighbor]);
        buf_r.release();
        cq_reverse_prune.destroy();
    }
}

static void InterClusterEnhancementHelper(CRUXBuildState &buildstate, Vector<slock_t> &locks,
    Vector<Vector<size_t>> &Projection_all_semantic, Relation index, uint32 i) { // 【修改点：去掉额外参数】
    
    VecBuffer buf_v = vec_read_buffer(index, i, buildstate.dimensions * sizeof(float));
    float *v = (float*)buf_v.get_vecbuf();
    // 【修改点：去掉额外参数】
    Vector<size_t> pruned_list = search_neighbors(buildstate, v, index, Projection_all_semantic, locks, i);
    set_up_proj_semantic(buildstate, v, locks, Projection_all_semantic, index, pruned_list, i);
    
    buf_v.release();
    pruned_list.destroy();
}

static void InterClusterEnhancement(CRUXBuildState &buildstate) {
    Vector<Vector<size_t>> Projection_all_semantic;
    Vector<Vector<size_t>> Projections_only;
    Projection_all_semantic.resize(buildstate.num_data);
    Projections_only.resize(buildstate.num_data);
    
    for (uint32 j = 0; j < buildstate.num_data; j++) {
        for (uint32 i = 0; i < buildstate.ncluster; i ++) {
            Edges &edges = buildstate.mem_graph[j][i];
            for (uint32 k = 0; k < edges.edge_num; k++) {
                Projection_all_semantic[j].push_back(edges.edges[k].data_offset);
                Projections_only[j].push_back(edges.edges[k].data_offset);
            }
        }
    }

    Vector<slock_t> locks(buildstate.num_data, '\0');
    for (auto &l : locks) {
        SpinLockInit(&l);
    }
    constexpr size_t nparallel = 10;
    // int totalParaWorkers = nparallel + 1;
    
    // ========================================================
    // 【GLO Step 3 - 新增】：初始化无锁多线程草稿本数组
    // std::vector<std::unordered_map<uint64_t, uint32_t>> thread_heats(totalParaWorkers);
    // ========================================================

    INIT_TASK_RUNNER();
    LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(nparallel);
    Timer timer(buildstate.num_data, 50'000);
    const auto task = [&](int, int64 start, int64 end) -> void {
        Relation index = buildstate.index;
        MemoryContext old_ctx = CurrentMemoryContext;
        if (IsBgWorkerProcess()) {
            index = index_open(index->rd_id, NoLock);
            old_ctx = MemoryContextSwitchTo(buildstate.cruxCtx);
        }
        for (int i = start; i < end; i++) {
            if (uint32(i) == buildstate.entry_point) {
                continue;
            }
            CHECK_FOR_INTERRUPTS();
            // 【修改点：去掉参数】
            InterClusterEnhancementHelper(buildstate, locks, Projection_all_semantic, index, i);
            timer.inc_loop_count_forground_report("  Inter Cluster Enhancement");
        }
        if (IsBgWorkerProcess()) {
            MemoryContextSwitchTo(old_ctx);
            index_close(index, NoLock);
        }
    };
    
    START_TASK_POOL();
    PARALLEL_BATCH_RUN_INIT();
    PARALLEL_BATCH_RUN_TASK_WAIT(buildstate.num_data, int(nparallel + 1), task);
    END_TASK_POOL();
    DESTROY_TASK_RUNNER();
    
    // 处理落单的 entry_point（单线程环境，直接塞给第 0 个草稿本没问题）
    // InterClusterEnhancementHelper(buildstate, locks, Projection_all_semantic, buildstate.index, buildstate.entry_point, 0, thread_heats);
    InterClusterEnhancementHelper(buildstate, locks, Projection_all_semantic, buildstate.index, buildstate.entry_point);

    for (auto &l : locks) {
        SpinLockFree(&l);
    }
    locks.destroy();
    timer.report("  Inter Cluster Enhancement Done");
    timer.reset_step(50'000);

    Vector<size_t> InterClusterIncremental;
    
    for (uint32 i = 0; i < buildstate.num_data; i++) {
        timer.report_loop("  CE Partition");
        InterClusterIncremental.clear();
        for (size_t pas : Projection_all_semantic[i]) {
            if (std::find(Projections_only[i].cbegin(), Projections_only[i].cend(), pas) ==
                Projections_only[i].cend()) {
                InterClusterIncremental.push_back(pas);
            }
        }
        CE_partition(buildstate, InterClusterIncremental, i);
    }
    
    timer.report("  CE Partition Done");
    timer.destroy();
    InterClusterIncremental.destroy();
    Projection_all_semantic.destroy();
    Projections_only.destroy();
}


static void compute_entry_point(CRUXBuildState &buildstate) {
    Timer timer;
    timer.report("  Calculate entry point start");
    uint32 dim = buildstate.dimensions;
    size_t size = buildstate.num_data;

    constexpr size_t block_size = 1024ul;
    float *buf = (float *)palloc0(sizeof(float) * dim * block_size);
    float *center = (float *)palloc0(sizeof(float) *dim);

    double *temp_center = (double *)palloc0(sizeof(double) * dim);
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        // vec_read(buildstate.index->rd_smgr, (i + 1ul) * dim * sizeof(float), block * dim * sizeof(float), (char *)buf);
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

    /* compute all to one distance */
    const auto func = get_general_distance_batch_func(buildstate.metric, dim);
    float *distances = (float *)palloc(sizeof(float) * size);
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        // vec_read(buildstate.index->rd_smgr, (i + 1ul) * dim * sizeof(float), block * dim * sizeof(float), (char *)buf);
        vec_read(buildstate.index->rd_smgr, i * dim * sizeof(float), block * dim * sizeof(float), (char *)buf);
        func(center, buf, dim, block, distances + i);
    }

    pfree(buf);
    pfree(center);

    /* find imin */
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

void CRUXInitMeta(Buffer buf, Page page) {
    PageInit(page, BufferGetPageSize(buf), sizeof(CRUXPageOpaque));
    CRUXPageGetOpaque(page)->page_id = CRUX_META_ID;
}

void CreateMetaPage(CRUXBuildState &buildstate) {
    Buffer buf = AnnNewBuffer(buildstate.index, MAIN_FORKNUM);
    Page page = BufferGetPage(buf);
    CRUXInitMeta(buf, page);
    CRUXMetaPage *metaPage = CRUXPageGetMeta(page);

    metaPage->magicNumber = CRUX_MAGIC_NUMBER;
    metaPage->metric = buildstate.metric;
    metaPage->dimensions = buildstate.dimensions;
    metaPage->version = CRUX_VERSION;
    metaPage->num_semantic_cluster = buildstate.ncluster;
    metaPage->num_data = buildstate.num_data;

    // centers 依然需要在初始阶段落盘
    buildstate.centers_meta_blkno = DiskVector<float>::get_disk_vector(buildstate.index, false);
    metaPage->centers_meta_blkno = buildstate.centers_meta_blkno;

    buildstate.scan_data_meta_blkno = DiskVector<ScanData>::get_disk_vector(buildstate.index, false);
    metaPage->scan_data_meta_blkno = buildstate.scan_data_meta_blkno;

    buildstate.base_edges_meta_blkno = DiskVector<BaseEdges>::get_disk_vector(buildstate.index, false);
    metaPage->base_edges_meta_blkno = buildstate.base_edges_meta_blkno;

    buildstate.overflow_buckets_meta_blkno = DiskVector<OverflowBucket>::get_disk_vector(buildstate.index, false);
    metaPage->overflow_buckets_meta_blkno = buildstate.overflow_buckets_meta_blkno;

    // 【Step 1 恢复：防遗忘元数据落盘申请】
    buildstate.edgeNumReminder_meta_blkno = DiskVector<edgeNumReminder>::get_disk_vector(buildstate.index, false);
    metaPage->edgeNumReminder_meta_blkno = buildstate.edgeNumReminder_meta_blkno;
    // （注意：庞大的 data_meta 和 20 个 edges_meta 的创建逻辑已被我们彻底抹除！）

    // 【GLO 适配：申请 Logical -> Physical 映射表的物理文件】
    buildstate.logical_to_physical_meta_blkno = DiskVector<uint32_t>::get_disk_vector(buildstate.index, false);
    metaPage->logical_to_physical_meta_blkno = buildstate.logical_to_physical_meta_blkno;

    buildstate.query_vec_meta_blkno = DiskVector<float>::get_disk_vector(buildstate.index, false);
    metaPage->query_vec_meta_blkno = buildstate.query_vec_meta_blkno;

    buildstate.upper_index_edges_meta_blkno = DiskVector<QuerySubIndexNeighbors>::get_disk_vector(buildstate.index, false);
    metaPage->upper_index_edges_meta_blkno = buildstate.upper_index_edges_meta_blkno;

    ((PageHeader) page)->pd_lower = ((char *) metaPage + sizeof(CRUXMetaPage)) - (char *) page;
    AnnCommitBuffer(buf);
}

void UpdateMetaPage(CRUXBuildState &buildstate){
    Buffer buf = AnnLoadBufferExtended(buildstate.index, buildstate.forkNum, CRUX_METAPAGE_BLKNO);
    CRUXMetaPage *metaPage = CRUXPageGetMeta(BufferGetPage(buf));

    metaPage->entry_point = buildstate.entry_point;
    metaPage->num_data = buildstate.num_data;
    for (uint32 i = 0; i < buildstate.ncluster; i++) {
        metaPage->multi_ep[i] = buildstate.multi_entry[i];
    }
    AnnCommitBuffer(buf);
}

// 【Step 1 恢复：基于 A2 极速内存图的防遗忘游标快照重写】
void update_edges_num(CRUXBuildState &buildstate, bool Initialization_done) {
    DiskVector<edgeNumReminder> edgeNumReminders(buildstate.index, buildstate.edgeNumReminder_meta_blkno, false);
    for (uint32 i = 0; i < buildstate.num_data; i++) {
        edgeNumReminder reminder;
        if (!Initialization_done) {
            memset(&reminder, 0, sizeof(edgeNumReminder));
            for (uint32 j = 0; j < buildstate.ncluster; j++) {
                reminder.Initialization_edges_num[j] = buildstate.mem_graph[i][j].edge_num;
                reminder.Inter_cluster_edges_num[j] = 0;
            }
            edgeNumReminders.push_back(reminder);
        } else {
            reminder = edgeNumReminders.get<AccessorLockType::ReadLock>(i);
            for (uint32 j = 0; j < buildstate.ncluster; j++) {
                uint32 cur_edges = buildstate.mem_graph[i][j].edge_num;
                uint32 init_edges = reminder.Initialization_edges_num[j];
                if (cur_edges >= init_edges) {
                    reminder.Inter_cluster_edges_num[j] = cur_edges - init_edges;
                } else {
                    reminder.Inter_cluster_edges_num[j] = 0;
                }
            }
            edgeNumReminders.set<AccessorLockType::WriteLock>(i, reminder);
        }
    }
    edgeNumReminders.destroy();
}

// 辅助函数：根据排好序的 edges 数组，生成 O(1) 路由微目录
static void compute_start_idx(uint16_t *start_idx, const uint32_t *edges, uint16_t edge_num) {
    uint32_t current_semantic = 0;
    start_idx[0] = 0;
    for (uint16_t idx = 0; idx < edge_num; ++idx) {
        uint32_t sem = decode_semantic(edges[idx]);
        while (current_semantic < sem) {
            current_semantic++;
            start_idx[current_semantic] = idx;
        }
    }
    // 将剩余未激活的 semantic 边界全部指向末尾
    while (current_semantic < NUM_SEMANTIC_CLUSTER) {
        current_semantic++;
        start_idx[current_semantic] = edge_num;
    }
}

// ========================================================================
// 【GLO Step 4 - 核心】：全局最优图布局计算 (Priority-Weighted BFS)
// ========================================================================
static void ComputeGlobalLayout(CRUXBuildState &buildstate, 
                                std::vector<uint32_t> &mem_to_disk, 
                                std::vector<uint32_t> &disk_to_mem) {
    uint32_t num_data = buildstate.num_data;
    mem_to_disk.assign(num_data, (uint32_t)-1);
    disk_to_mem.assign(num_data, (uint32_t)-1);
    
    // 优先队列：<热度(Weight), 内存节点ID(mem_id)>
    // C++ 的 priority_queue 默认是大顶堆，热度最高的节点会优先出队
    std::priority_queue<std::pair<uint32_t, uint32_t>> pq;
    uint32_t next_disk_id = 0;
    
    // 外层循环：兜底防止图中存在没有任何入边的“孤岛节点”
    for (uint32_t start_i = 0; start_i < num_data; start_i++) {
        // 第一优先级从全局 Entry Point 开始搜索
        uint32_t root = (start_i == 0) ? buildstate.entry_point : start_i;
        
        if (mem_to_disk[root] != (uint32_t)-1) continue;
        
        // 赋予起点极高优先级
        pq.push({99999999, root}); 
        
        while(!pq.empty()) {
            auto curr = pq.top();
            pq.pop();
            uint32_t u = curr.second;
            
            // 如果已经被分配过 ID，直接跳过
            if (mem_to_disk[u] != (uint32_t)-1) continue;
            
            // 【分配物理落盘 ID】
            mem_to_disk[u] = next_disk_id;
            disk_to_mem[next_disk_id] = u;
            next_disk_id++;
            
            // 遍历该节点的所有出边，将邻居按热度压入优先队列
            for (const auto& edge : buildstate.global_edge_heat[u]) {
                uint32_t v = edge.first;
                uint32_t heat = edge.second;
                
                // 只将尚未分配新 ID 的邻居入队
                if (mem_to_disk[v] == (uint32_t)-1) {
                    pq.push({heat, v});
                }
            }
        }
    }

    // 【极其关键】：翻译系统入口点元数据
    buildstate.entry_point = mem_to_disk[buildstate.entry_point];
    for (uint32 i = 0; i < buildstate.ncluster; i++) {
        buildstate.multi_entry[i] = mem_to_disk[buildstate.multi_entry[i]];
    }
    
    // 立即更新到磁盘 MetaPage，覆盖掉旧的内存 ID 映射
    UpdateMetaPage(buildstate);
    
    elog(NOTICE, "Global Layout Optimization (GLO) completed successfully.");
}

// 终极压缩落盘引擎
// 终极压缩落盘引擎 (带 GLO 物理地址翻译)
static void FlushGraphToDisk(CRUXBuildState &buildstate) {
    Timer timer(buildstate.num_data, 10'000);
    timer.report("  Enter Final Graph Compaction and Flush.");
    
    // ========================================================
    // 1. 调用 GLO 布局计算，获取双向映射表
    std::vector<uint32_t> mem_to_disk;
    std::vector<uint32_t> disk_to_mem;
    ComputeGlobalLayout(buildstate, mem_to_disk, disk_to_mem);

    // 【全新加入：持久化 Logical -> Physical 映射表】
    timer.report("  Saving Logical-to-Physical ID mapping...");
    DiskVector<uint32_t> logical_to_physical(buildstate.index, buildstate.logical_to_physical_meta_blkno, false);
    for (uint32 i = 0; i < buildstate.num_data; i++) {
        logical_to_physical.push_back(mem_to_disk[i]);
    }
    logical_to_physical.destroy();
    // ========================================================

    timer.report("  Fast in-memory reordering vector payloads...");
    size_t vec_bytes = buildstate.dimensions * sizeof(float);
    size_t total_bytes = (size_t)buildstate.num_data * vec_bytes;
    
    // 一次性顺序读入所有原始向量 (10M 数据约 8GB)
    float *old_vecs = (float*)palloc_huge(CurrentMemoryContext, total_bytes);
    vec_read(buildstate.index->rd_smgr, 0, total_bytes, (char*)old_vecs);
    
    // 在内存中一次性完成超高速洗牌
    float *new_vecs = (float*)palloc_huge(CurrentMemoryContext, total_bytes);
    for (uint32 i = 0; i < buildstate.num_data; i++) {
        uint32 new_id = mem_to_disk[i];
        
        // 每次拷贝单个向量的长度，安全规避 memcpy_s 的 2GB 安全上限
        errno_t rc = memcpy_s((char*)new_vecs + (size_t)new_id * vec_bytes, 
                              vec_bytes, 
                              (char*)old_vecs + (size_t)i * vec_bytes, 
                              vec_bytes);
        securec_check(rc, "\0", "\0");
    }
    
    // 一次性顺序写回重排后的向量
    vec_write(buildstate.index->rd_smgr, 0, total_bytes, (char*)new_vecs, false);
    
    pfree(old_vecs);
    pfree(new_vecs);
    timer.report("  Vector payloads reordering done.");
    // ========================================================

    // ========================================================
    // 【GLO 附带修复】：将 edgeNumReminder 也进行物理洗牌对齐
    // ========================================================
    timer.report("  Fast in-memory reordering edgeNumReminders...");
    DiskVector<edgeNumReminder> reminders_disk(buildstate.index, buildstate.edgeNumReminder_meta_blkno, false);
    
    // 1. 一次性读入所有旧的 reminder (约 1.5GB)
    edgeNumReminder *all_rems = (edgeNumReminder *)palloc_huge(CurrentMemoryContext, buildstate.num_data * sizeof(edgeNumReminder));
    for (uint32 i = 0; i < buildstate.num_data; i++) {
        all_rems[i] = reminders_disk.get<AccessorLockType::ReadLock>(i);
    }
    
    // 2. 按照 GLO 的 disk_id 顺序，重新写回磁盘
    for (uint32 disk_id = 0; disk_id < buildstate.num_data; disk_id++) {
        uint32 mem_id = disk_to_mem[disk_id];
        reminders_disk.set<AccessorLockType::WriteLock>(disk_id, all_rems[mem_id]);
    }
    
    pfree(all_rems);
    reminders_disk.destroy();
    timer.report("  edgeNumReminders reordering done.");
    // ========================================================

    DiskVector<ScanData> scan_data(buildstate.index, buildstate.scan_data_meta_blkno, false);
    DiskVector<BaseEdges> base_edges(buildstate.index, buildstate.base_edges_meta_blkno, false);
    DiskVector<OverflowBucket> overflow_buckets(buildstate.index, buildstate.overflow_buckets_meta_blkno, false);
    
    Vector<uint32_t> all_edges;
    
    // ========================================================
    // 2. 主循环：严格按照新的物理落盘顺序 (disk_id) 进行遍历
    // ========================================================
    for (uint32 disk_id = 0; disk_id < buildstate.num_data; disk_id++) {
        timer.report_loop("  Flushing nodes to disk");
        
        // 找到该 disk_id 对应的真实内存数据
        uint32 mem_id = disk_to_mem[disk_id]; 
        
        ScanData sd;
        sd.cluster = buildstate.mem_clusters[mem_id];
        sd.heapTid = buildstate.mem_heaptids[mem_id];
        scan_data.push_back(sd);
        
        all_edges.clear();
        for (uint32 j = 0; j < buildstate.ncluster; j++) {
            Edges &edges = buildstate.mem_graph[mem_id][j];
            for (uint16_t k = 0; k < edges.edge_num; k++) {
                
                // 【核心翻译】：把目标节点的内存 ID 翻译为新的落盘 ID
                uint32_t target_mem = edges.edges[k].data_offset;
                uint32_t target_disk = mem_to_disk[target_mem];
                
                // 将新的 disk_id 压入边表
                all_edges.push_back(encode_edge(j, target_disk));
            }
        }
        
        // 3. 高 5 位是 semantic_id，排序后自动按 semantic 分组
        std::sort(all_edges.begin(), all_edges.end());
        
        // 4. 封装并写入 BaseEdges
        BaseEdges base;
        base.edge_num = std::min((size_t)BASE_EDGE_CAPACITY, all_edges.size());
        base.overflow_offset = (uint32_t)-1;
        for (size_t k = 0; k < base.edge_num; k++) {
            base.edges[k] = all_edges[k];
        }
        compute_start_idx(base.start_idx, base.edges, base.edge_num);
        
        // 5. 级联处理 OverflowBuckets
        if (all_edges.size() > BASE_EDGE_CAPACITY) {
            uint32_t first_overflow_offset = (uint32_t)-1;
            uint32_t last_overflow_offset = (uint32_t)-1;
            
            size_t num_overflow = (all_edges.size() - BASE_EDGE_CAPACITY + OVERFLOW_EDGE_CAPACITY - 1) / OVERFLOW_EDGE_CAPACITY;
            
            for (size_t b = 0; b < num_overflow; b++) {
                OverflowBucket bucket;
                size_t start_e = BASE_EDGE_CAPACITY + b * OVERFLOW_EDGE_CAPACITY;
                size_t count = std::min((size_t)OVERFLOW_EDGE_CAPACITY, all_edges.size() - start_e);
                
                bucket.edge_num = count;
                bucket.next_bucket_offset = (uint32_t)-1;
                for (size_t k = 0; k < count; k++) {
                    bucket.edges[k] = all_edges[start_e + k];
                }
                compute_start_idx(bucket.start_idx, bucket.edges, bucket.edge_num);
                
                uint32_t current_offset = overflow_buckets.push_back(bucket);
                
                if (b == 0) {
                    first_overflow_offset = current_offset;
                } else {
                    overflow_buckets.apply<AccessorLockType::WriteLock>([&](OverflowBucket &prev) -> bool {
                        prev.next_bucket_offset = current_offset;
                        return true;
                    })(last_overflow_offset);
                }
                last_overflow_offset = current_offset;
            }
            base.overflow_offset = first_overflow_offset;
        }
        
        base_edges.push_back(base);
    }
    
    // 6. 收尾清理
    all_edges.destroy();
    scan_data.destroy();
    base_edges.destroy();
    overflow_buckets.destroy();
    
    buildstate.mem_clusters.destroy();
    buildstate.mem_heaptids.destroy();
    buildstate.mem_graph.destroy();
    
    timer.report("  Final Graph Compaction and Flush Done");
    timer.destroy();
}

void BuildCRUXIndex(Relation heap, Relation index, IndexInfo *indexInfo, ForkNumber forkNum, int m,
    int efConstruction, int parallel_workers, int maintenance_work_mem, int num_semantic_cluster,
    int num_ground_truth, double *reltuples, double *indtuples) {
    CRUXBuildState buildstate;
    InitBuildState(buildstate, heap, index, indexInfo, MAIN_FORKNUM, CRUXGetM(index), 
        CRUXGetEfConstruction(index), CRUXGetBuildParallel(index), CRUXGetNumberCluster(index), 
        CRUXGetNumberGroundTruth(index), u_sess->attr.attr_memory.maintenance_work_mem);
    CreateMetaPage(buildstate);

    auto old_ctx = MemoryContextSwitchTo(buildstate.cruxCtx);
    uint32 heap_mark = 0;
    BuildCallbackData data = {buildstate, heap, &heap_mark};
    buildstate.reltuples = IndexBuildHeapScan(heap, index, indexInfo, true, BuildCallback, &data, NULL);

    // ==========================================================
    if (buildstate.num_data == 0) {
        UpdateMetaPage(buildstate);
        MemoryContextSwitchTo(old_ctx);
        *reltuples = 0;
        *indtuples = 0;
        buildstate.destroy();
        return;
    }
    // ==========================================================

    buildstate.global_edge_heat.clear();
    buildstate.global_edge_heat.resize(buildstate.num_data);

    compute_entry_point(buildstate);

    buildstate.parallel_workers = parallel_workers; // 确保线程数已配置
    buildQueryGraph(buildstate);

    Initialization(buildstate);
    update_edges_num(buildstate, false);
    UpdateMetaPage(buildstate);

    InterClusterEnhancement(buildstate);
    update_edges_num(buildstate, true);

    // ========================================================
    FlushGraphToDisk(buildstate);
    // ========================================================

    log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
                      true, RM_HNSW_ID, XLOG_BM25_BUILD_INDEX, NULL, true);
    MemoryContextSwitchTo(old_ctx);
    *reltuples = buildstate.reltuples;
    *indtuples = buildstate.num_data;
    buildstate.destroy();
}

IndexBuildResult *cruxbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo) {
    double reltuples, indtuples;
    BuildCRUXIndex(heap, index, indexInfo, MAIN_FORKNUM, CRUXGetM(index), 
        CRUXGetEfConstruction(index), CRUXGetBuildParallel(index), u_sess->attr.attr_memory.maintenance_work_mem, 
        CRUXGetNumberCluster(index), CRUXGetNumberGroundTruth(index), 
        &reltuples, &indtuples);
    IndexBuildResult *res = (IndexBuildResult *)palloc0(sizeof(IndexBuildResult));
    res->heap_tuples = reltuples;
    res->index_tuples = indtuples;
    return res;
}
