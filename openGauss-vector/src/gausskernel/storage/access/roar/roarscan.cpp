#include "access/roar/roar.h"
#include "pgstat.h"

using namespace ann_helper;
using namespace disk_container;

// =================================================================
// 1. 图结构 Profiler 适配为单图
// =================================================================
static void ProfileROAREdges(Relation index, ROARMetaPage *metaPage) {
    uint32 num_data = metaPage->num_data;

    elog(NOTICE, "========== 开始扫描并统计 ROAR 单图物理边分布 ==========");
    
    // 打开 DataPoints 和 唯一的 Semantic Edges_r
    DiskVector<Data_r> dp(index, metaPage->data_meta_blkno, false);
    DiskVector<Edges_r> sedges(index, metaPage->edges_meta_blkno, false);

    // 分配内存用于统计
    uint32 *total_edges = (uint32 *)palloc0(sizeof(uint32) * num_data);
    uint64 total_edges_sum = 0;

    for (uint32 i = 0; i < num_data; i++) {
        Data_r data = dp.get<AccessorLockType::ReadLock>(i);
        uint32 current_edges = 0;
        
        if (data.edge_r_offset != (uint32_t)-1) {
            Edges_r e = sedges.get<AccessorLockType::ReadLock>(data.edge_r_offset);
            current_edges = e.edge_r_num;
        }
        
        total_edges[i] = current_edges;
        total_edges_sum += current_edges;
        
        if (i % 500000 == 0 && i > 0) {
            elog(NOTICE, "  -> 已扫描 %u / %u 个节点...", i, num_data);
        }
    }

    // 统计指标计算
    std::sort(total_edges, total_edges + num_data);
    uint32 max_edges = total_edges[num_data - 1];
    uint32 top_1_percent = total_edges[(uint32)(num_data * 0.99)];
    double avg_edges = (double)total_edges_sum / num_data;

    uint32 count_over_30 = 0;
    uint32 count_over_50 = 0;
    uint32 count_over_80 = 0;
    for (uint32 i = 0; i < num_data; i++) {
        if (total_edges[i] > 30) count_over_30++;
        if (total_edges[i] > 50) count_over_50++;
        if (total_edges[i] > 80) count_over_80++;
    }

    // 打印最终战报
    elog(NOTICE, "========== ROAR 物理图结构统计报告 ==========");
    elog(NOTICE, "节点总数 (num_data)      : %u", num_data);
    elog(NOTICE, "平均出边数 (Avg Edges_r)   : %.2f", avg_edges);
    elog(NOTICE, "最大出边数 (Max Edges_r)   : %u", max_edges);
    elog(NOTICE, "前 1%% 节点出边数 (Top1%%): %u", top_1_percent);
    elog(NOTICE, "超过  30 条边的节点数量  : %u (%.2f%%)", count_over_30, (double)count_over_30 / num_data * 100.0);
    elog(NOTICE, "超过  50 条边的节点数量  : %u (%.2f%%)", count_over_50, (double)count_over_50 / num_data * 100.0);
    elog(NOTICE, "超过  80 条边的节点数量  : %u (%.2f%%)", count_over_80, (double)count_over_80 / num_data * 100.0);
    elog(NOTICE, "=============================================");

    pfree(total_edges);
    dp.destroy();
    sedges.destroy();
}

// =================================================================
// 2. Scan Opaque 基础定义
// =================================================================
struct ROARScanOpaqueData {
    Relation    index;
    ItemPointer tids;
    uint32      searchListSize;
    uint32      dimensions;
    int         currIndex;
    int         lastIndex;
    bool        first;
};
typedef ROARScanOpaqueData *ROARScanOpaque;

void *create_roar_scanopaque(Relation index)
{
    ROARScanOpaque so = (ROARScanOpaque)palloc(sizeof(ROARScanOpaqueData));
    so->index = index;
    so->first = true;
    so->searchListSize = u_sess->attr.attr_storage.ef_search;
    so->dimensions = VEC_DIM;
    so->tids = NULL;
    so->currIndex = so->lastIndex = 0;
    return so;
}

IndexScanDesc roarbeginscan_internal(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    scan->opaque = create_roar_scanopaque(index);
    return scan;
}

void roarrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    ROARScanOpaque so = (ROARScanOpaque)scan->opaque;
    so->first = true;
    pfree_ext(so->tids);

    if (keys && scan->numberOfKeys > 0) {
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }

    if (orderbys && scan->numberOfOrderBys > 0) {
        memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
    }
}

static void free_roar_scanopaque(void *in_so)
{
    ROARScanOpaque so = (ROARScanOpaque)in_so;
    pfree_ext(so->tids);
    pfree(so);
}

void roarendscan_internal(IndexScanDesc scan)
{
    if (scan->opaque) {
        free_roar_scanopaque(scan->opaque);
        scan->opaque = NULL;
    }
}

// =================================================================
// 3. 核心单图搜索逻辑 (Pure Greedy Beam Search)
// =================================================================
Vector<QueryNeighbor> SingleQuerySearch(Relation index, float *query, uint32 ef, Buffer meta_buf, ROARMetaPage *metaPage)
{
    const uint32 dim = metaPage->dimensions;
    distance_func dist_func = get_general_distance_func(metaPage->metric, dim);
    
    CandidateQueue cq(ef);
    UnorderedSet<uint32> visited;
    
    // 1. 获取图的唯一入口点
    uint32 ep = metaPage->entry_point;
    visited.insert(ep);
    
    VecBuffer buf_ep = vec_read_buffer(index, ep, dim * sizeof(float));
    float *ep_vec = (float *)buf_ep.get_vecbuf();
    cq.emplace(ep, dist_func(query, ep_vec, dim));
    buf_ep.release();

    disk_container::DiskVector<Data_r> data_points(index, metaPage->data_meta_blkno, false);
    disk_container::DiskVector<Edges_r> edges_disk(index, metaPage->edges_meta_blkno, false);

    // 2. 在单图上进行标准贪心搜索寻路
    while (cq.has_unexplored()) {
        QueryNeighbor curr = cq.pop_unexplored();
        
        Data_r data_curr = data_points.get<AccessorLockType::ReadLock>(curr.id);
        if (data_curr.edge_r_offset == (uint32_t)-1) {
            continue; // 如果该节点孤立，跳过
        }
        
        Edges_r curr_edges = edges_disk.get<AccessorLockType::ReadLock>(data_curr.edge_r_offset);
        
        for (uint32 i = 0; i < curr_edges.edge_r_num; i++) {
            uint32 neighbor_id = curr_edges.edges_r[i].data_offset;
            if (!visited.insert(neighbor_id).second) {
                continue; // 已访问过
            }
            
            VecBuffer buf_n = vec_read_buffer(index, neighbor_id, dim * sizeof(float));
            float *n_vec = (float *)buf_n.get_vecbuf();
            float dist = dist_func(query, n_vec, dim);
            buf_n.release();
            
            cq.emplace(neighbor_id, dist);
        }
    }
    
    data_points.destroy();
    edges_disk.destroy();
    visited.destroy();

    // 3. 收集结果
    Vector<QueryNeighbor> candidates;
    candidates.clear();
    for (const auto &candidate : cq) {
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), QueryNeighbor::compare_distance);
    cq.destroy();

    return candidates;
}

// =================================================================
// 4. 精简后的扫描执行包装器
// =================================================================
// 移除了 num_semantic_activated 参数
bool roargettuple_internal(IndexScanDesc scan)
{
    ROARScanOpaque so = (ROARScanOpaque)scan->opaque;
    if (so->first) {
        uint32 ef = so->searchListSize;
        pgstat_count_index_scan(scan->indexRelation);

        if (scan->numberOfOrderBys <= 0 || scan->orderByData->sk_attno != 1) {
            ereport(ERROR, (errcode(ERRCODE_PLPGSQL_ERROR),
                            errmsg("cannot scan ROAR index without vector order")));
        }
        if (scan->orderByData->sk_flags & SK_ISNULL) {
            return false;
        }

        Datum value = scan->orderByData->sk_argument;
        FloatVector *vector = DatumGetFloatVector(value);
        if (uint16(vector->dim) != so->dimensions) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Incorrect dimension of query vector")));
        }
        float *query = vector->x;

        if (scan->with_limit && so->searchListSize < (uint32)scan->limit_count + 1) {
            so->searchListSize = (uint32)scan->limit_count + 1;
        }

        bool alloced = false;
        if (!is_aligned(query)) {
            float *temp = (float *)palloc0(so->dimensions * sizeof(float));
            errno_t rc = memcpy_s(temp, sizeof(float) * so->dimensions, query, sizeof(float) * so->dimensions);
            securec_check(rc, "\0", "\0");
            query = temp;
            alloced = true;
        }

        Buffer buf = AnnLoadBuffer(so->index, ROAR_METAPAGE_BLKNO);
        ROARMetaPage *metaPage = ROARPageGetMeta(BufferGetPage(buf));
        
        // 调用重写后的单图搜索
        Vector<QueryNeighbor> candidates = SingleQuerySearch(so->index, query, ef, buf, metaPage);

        // ==== 一次性图结构探测代码 ====
        static bool is_profiled = false;
        if (!is_profiled) {
            ProfileROAREdges(so->index, metaPage);
            is_profiled = true;
        }
        // ==========================
        
        BlockNumber data_meta_blkno = metaPage->data_meta_blkno;
        UnlockReleaseBuffer(buf);
        
        if (alloced) {
            pfree(query);
        }
        
        so->currIndex = 0;
        so->lastIndex = (int)candidates.size();
        so->tids = (ItemPointerData *)palloc(so->lastIndex * sizeof(ItemPointerData));
        ItemPointer temp = so->tids;
        
        DiskVector<Data_r> dp(so->index, data_meta_blkno, false);
        for (const auto &c : candidates) {
            *temp = dp.get<AccessorLockType::ReadLock>(c.id).heapTid;
            ++temp;
        }
        
        dp.destroy();
        candidates.destroy();
        so->first = false;
    }

    if (so->currIndex < so->lastIndex) {
        scan->xs_recheck = false;
        scan->xs_ctup.t_self = so->tids[so->currIndex];
        ++so->currIndex;
        return true;
    }

    if (so->lastIndex >= 0 && !RelationIsPartition(scan->indexRelation)) {
        ereport(WARNING, (errmsg("Run out of search list with potentially unfetched data. "
                                 "Set larger ef_search value to fetch potential data.")));
    }

    return false;
}
