#include "access/qasp/qasp.h"
#include "pgstat.h"

using namespace ann_helper;
using namespace disk_container;

static void ProfileQASPEdges(Relation index, QASPMetaPage *metaPage) {
    uint32 num_data = metaPage->num_data;

    elog(NOTICE, "========== 验证全新的 QASP 极简图结构 ==========");
    DiskVector<BaseEdges> base_disk(index, metaPage->base_edges_meta_blkno, false);
    DiskVector<OverflowBucket> overflow_disk(index, metaPage->overflow_buckets_meta_blkno, false);
    
    uint64 total_edges = 0;
    uint32 max_edges = 0;
    uint32 overflow_count = 0;
    
    for(uint32 i = 0; i < num_data; i++) {
        BaseEdges base = base_disk.get<AccessorLockType::ReadLock>(i);
        uint32 curr_edges = base.edge_num;
        uint32_t curr_over = base.overflow_offset;
        
        while(curr_over != (uint32_t)-1) {
            overflow_count++;
            OverflowBucket bucket = overflow_disk.get<AccessorLockType::ReadLock>(curr_over);
            curr_edges += bucket.edge_num;
            curr_over = bucket.next_bucket_offset;
        }
        total_edges += curr_edges;
        if(curr_edges > max_edges) max_edges = curr_edges;
    }
    
    elog(NOTICE, "节点总数 (num_data) : %u", num_data);
    elog(NOTICE, "总出边数 (Total)    : %lu (平均 %.2f)", total_edges, (double)total_edges/num_data);
    elog(NOTICE, "最大出度 (Max Edges): %u", max_edges);
    elog(NOTICE, "被分配的溢出桶总数  : %u", overflow_count);
    elog(NOTICE, "================================================");
    
    base_disk.destroy();
    overflow_disk.destroy();
}


struct QASPScanOpaqueData {
    Relation    index;
    ItemPointer tids;
    uint32      searchListSize;
    uint32      dimensions;
    int         currIndex;
    int         lastIndex;
    bool        first;
};
typedef QASPScanOpaqueData *QASPScanOpaque;

void *create_qasp_scanopaque(Relation index)
{
    QASPScanOpaque so = (QASPScanOpaque)palloc(sizeof(QASPScanOpaqueData));
    so->index = index;
    so->first = true;

    so->searchListSize = u_sess->attr.attr_storage.ef_search;

    so->dimensions = VEC_DIM;

    so->tids = NULL;
    so->currIndex = so->lastIndex = 0;

    return so;
}


IndexScanDesc qaspbeginscan_internal(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    scan->opaque = create_qasp_scanopaque(index);
    return scan;
}


void qasprescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    QASPScanOpaque so = (QASPScanOpaque)scan->opaque;
    so->first = true;
    pfree_ext(so->tids);

    if (keys && scan->numberOfKeys > 0) {
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }

    if (orderbys && scan->numberOfOrderBys > 0) {
        memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
    }
}


static void free_qasp_scanopaque(void *in_so)
{
    QASPScanOpaque so = (QASPScanOpaque)in_so;
    pfree_ext(so->tids);
    pfree(so);
}

void qaspendscan_internal(IndexScanDesc scan)
{
    if (scan->opaque) {
        free_qasp_scanopaque(scan->opaque);
        scan->opaque = NULL;
    }
}

typedef struct CentroidDis {
    uint32 centroid_index;
    float distance;
} CentroidsDist;

void SemanticGreedySearch(Relation index, float *query, uint32 ef, CandidateQueue &cq,
    Vector<uint32> &closest_centroids, UnorderedSet<uint32> &visited, QASPMetaPage *metaPage) 
{
    uint32 semantic_activated = closest_centroids.size();
    auto dist_func = get_general_distance_func(Metric::INNER_PRODUCT, metaPage->dimensions);
    
    // 【质变】：全程只需要这 2 个极简文件，再也没有 20 个 semantic_edges 了！
    DiskVector<BaseEdges> base_edges_disk(index, metaPage->base_edges_meta_blkno, false);
    DiskVector<OverflowBucket> overflow_buckets_disk(index, metaPage->overflow_buckets_meta_blkno, false);

    while (cq.has_unexplored()) {
        QueryNeighbor neighbor = cq.pop_unexplored();
        
        // 1. O(1) 一次性获取该节点所有语义的主槽边！
        BaseEdges base = base_edges_disk.get<AccessorLockType::ReadLock>(neighbor.id);
        
        for (uint32 i = 0; i < semantic_activated; i ++) {
            uint32 semantic_id = closest_centroids[i];
            
            // 【无敌的 O(1) 路由】：直接拿到该语义边的起始和结束下标
            uint16_t start = base.start_idx[semantic_id];
            uint16_t end   = base.start_idx[semantic_id + 1];
            
            for (uint16_t k = start; k < end; k++) {
                uint32 data_offset = decode_target(base.edges[k]);
                if (!visited.insert(data_offset).second) {
                    continue;
                }
                VecBuffer buf = vec_read_buffer(index, data_offset, metaPage->dimensions * sizeof(float));
                float *v = (float *)buf.get_vecbuf();
                QueryNeighbor qn(data_offset, dist_func(query, v, metaPage->dimensions));
                buf.release();
                cq.insert(qn);
            }
        }
        
        // 2. 顺藤摸瓜寻找溢出桶（绝大多数节点并不会触发这段逻辑）
        uint32_t curr_overflow = base.overflow_offset;
        while (curr_overflow != (uint32_t)-1) {
            OverflowBucket bucket = overflow_buckets_disk.get<AccessorLockType::ReadLock>(curr_overflow);
            
            for (uint32 i = 0; i < semantic_activated; i++) {
                uint32 semantic_id = closest_centroids[i];
                uint16_t start = bucket.start_idx[semantic_id];
                uint16_t end   = bucket.start_idx[semantic_id + 1];
                
                for (uint16_t k = start; k < end; k++) {
                    uint32 data_offset = decode_target(bucket.edges[k]);
                    if (!visited.insert(data_offset).second) {
                        continue;
                    }
                    VecBuffer buf = vec_read_buffer(index, data_offset, metaPage->dimensions * sizeof(float));
                    float *v = (float *)buf.get_vecbuf();
                    QueryNeighbor qn(data_offset, dist_func(query, v, metaPage->dimensions));
                    buf.release();
                    cq.insert(qn);
                }
            }
            curr_overflow = bucket.next_bucket_offset;
        }
    }
    
    base_edges_disk.destroy();
    overflow_buckets_disk.destroy();
}

Vector<QueryNeighbor> SingleQuerySearch(Relation index, float *query, uint32 ef,
    uint32 num_semantic_activated, Buffer meta_buf, QASPMetaPage *metaPage)
{
    // 1. compute the semantics to be activated.
    const uint32 num_semantic_cluster = metaPage->num_semantic_cluster;
    const uint32 dim = metaPage->dimensions;
    float *centroid_vecs = (float *)palloc0(sizeof(float) * dim * num_semantic_cluster);
    DiskVector<float> centroids(index, metaPage->centers_meta_blkno, false);
    centroids.get_n<AccessorLockType::ReadLock>(0, num_semantic_cluster * dim, centroid_vecs);
    centroids.destroy();
    distance_func dist_func = get_general_distance_func(metaPage->metric, dim);
    CentroidsDist *centroid_dist = (CentroidsDist *)palloc0(sizeof(CentroidsDist) * num_semantic_cluster);
    for (uint32 i = 0; i < num_semantic_cluster; i++) {
        centroid_dist[i].distance = dist_func(query, centroid_vecs + i * dim, dim);
        centroid_dist[i].centroid_index = i;
    }
    std::sort(centroid_dist, centroid_dist + num_semantic_cluster, [](const CentroidsDist &a, const CentroidsDist &b) {
        return a.distance < b.distance;
    });
    pfree(centroid_vecs);
    Vector<uint32> closest_centroids;
    for (uint32 i = 0; i < num_semantic_activated; i++) {
        closest_centroids.push_back(centroid_dist[i].centroid_index);
    }

    pfree(centroid_dist);
    CandidateQueue cq(ef);
    UnorderedSet<uint32> visited;
    visited.insert(metaPage->entry_point);
    VecBuffer buf = vec_read_buffer(index, metaPage->entry_point, dim * sizeof(float));
    float *eps = (float *)buf.get_vecbuf();
    cq.emplace(metaPage->entry_point, dist_func(query, eps, dim));
    buf.release();
    for (uint32 i = 0; i < num_semantic_activated; i++) {
        uint32 x = closest_centroids[i];
        uint32 entry_point_x = metaPage->multi_ep[x];
        visited.insert(entry_point_x);
        VecBuffer buf_x = vec_read_buffer(index, entry_point_x, dim * sizeof(float));
        float *ep_x = (float *)buf_x.get_vecbuf();
        QueryNeighbor qn(entry_point_x, dist_func(query, ep_x, dim));
        buf_x.release();
        cq.insert(qn);
    }

    // 3. do targeted semantic greedy search 
    SemanticGreedySearch(index, query, ef, cq, closest_centroids, visited, metaPage);
    visited.destroy();

    // 4. return with candidates.
    Vector<QueryNeighbor> candidates;
    candidates.clear();
    for (const auto &candidate : cq) {
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), QueryNeighbor::compare_distance);

    closest_centroids.destroy();
    cq.destroy();
    return candidates;
}

void SemanticGreedySearch_repair(Relation index, float *query, uint32 ef, CandidateQueue &cq,
    Vector<uint32> &closest_centroids, UnorderedSet<uint32> &visited, QASPMetaPage *metaPage,
    SearchStats *stats) 
{
    uint32 semantic_activated = closest_centroids.size();
    auto dist_func = get_general_distance_func(Metric::INNER_PRODUCT, metaPage->dimensions);
    
    DiskVector<BaseEdges> base_edges_disk(index, metaPage->base_edges_meta_blkno, false);
    DiskVector<OverflowBucket> overflow_buckets_disk(index, metaPage->overflow_buckets_meta_blkno, false);

    while (cq.has_unexplored()) {
        QueryNeighbor neighbor = cq.pop_unexplored();
        BaseEdges base = base_edges_disk.get<AccessorLockType::ReadLock>(neighbor.id);
        
        for (uint32 i = 0; i < semantic_activated; i ++) {
            uint32 semantic_id = closest_centroids[i];
            uint16_t start = base.start_idx[semantic_id];
            uint16_t end   = base.start_idx[semantic_id + 1];
            
            for (uint16_t k = start; k < end; k++) {
                uint32 data_offset = decode_target(base.edges[k]);
                if (!visited.insert(data_offset).second) {
                    continue;
                }
                VecBuffer buf = vec_read_buffer(index, data_offset, metaPage->dimensions * sizeof(float));
                float *v = (float *)buf.get_vecbuf();
                float dist = dist_func(query, v, metaPage->dimensions);
                QueryNeighbor qn(data_offset, dist);
                buf.release();
                cq.insert(qn);
            }
        }
        
        uint32_t curr_overflow = base.overflow_offset;
        while (curr_overflow != (uint32_t)-1) {
            OverflowBucket bucket = overflow_buckets_disk.get<AccessorLockType::ReadLock>(curr_overflow);
            for (uint32 i = 0; i < semantic_activated; i++) {
                uint32 semantic_id = closest_centroids[i];
                uint16_t start = bucket.start_idx[semantic_id];
                uint16_t end   = bucket.start_idx[semantic_id + 1];
                for (uint16_t k = start; k < end; k++) {
                    uint32 data_offset = decode_target(bucket.edges[k]);
                    if (!visited.insert(data_offset).second) {
                        continue;
                    }
                    VecBuffer buf = vec_read_buffer(index, data_offset, metaPage->dimensions * sizeof(float));
                    float *v = (float *)buf.get_vecbuf();
                    float dist = dist_func(query, v, metaPage->dimensions);
                    QueryNeighbor qn(data_offset, dist);
                    buf.release();
                    cq.insert(qn);
                }
            }
            curr_overflow = bucket.next_bucket_offset;
        }
    }
    
    base_edges_disk.destroy();
    overflow_buckets_disk.destroy();
}

Vector<QueryNeighbor> SingleQuerySearch_repair(Relation index, float *query, uint32 ef, 
    uint32 num_semantic_activated, Buffer meta_buf, QASPMetaPage *metaPage, 
    SearchStats *stats)
{
    const uint32 num_semantic_cluster = metaPage->num_semantic_cluster;
    const uint32 dim = metaPage->dimensions;
    float *centroid_vecs = (float *)palloc0(sizeof(float) * dim * num_semantic_cluster);
    DiskVector<float> centroids(index, metaPage->centers_meta_blkno, false);
    centroids.get_n<AccessorLockType::ReadLock>(0, num_semantic_cluster * dim, centroid_vecs);
    centroids.destroy();
    
    distance_func dist_func = get_general_distance_func(metaPage->metric, dim);
    CentroidsDist *centroid_dist = (CentroidsDist *)palloc0(sizeof(CentroidsDist) * num_semantic_cluster);
    for (uint32 i = 0; i < num_semantic_cluster; i++) {
        centroid_dist[i].distance = dist_func(query, centroid_vecs + i * dim, dim);
        centroid_dist[i].centroid_index = i;
    }
    std::sort(centroid_dist, centroid_dist + num_semantic_cluster, [](const CentroidsDist &a, const CentroidsDist &b) {
        return a.distance < b.distance;
    });
    pfree(centroid_vecs);
    
    Vector<uint32> closest_centroids;
    for (uint32 i = 0; i < num_semantic_activated; i++) {
        closest_centroids.push_back(centroid_dist[i].centroid_index);
    }
    pfree(centroid_dist);

    CandidateQueue cq(ef);
    UnorderedSet<uint32> visited;
    
    visited.insert(metaPage->entry_point);
    VecBuffer buf = vec_read_buffer(index, metaPage->entry_point, dim * sizeof(float));
    float *eps = (float *)buf.get_vecbuf();
    cq.emplace(metaPage->entry_point, dist_func(query, eps, dim));
    buf.release();
    
    for (uint32 i = 0; i < num_semantic_activated; i++) {
        uint32 x = closest_centroids[i];
        uint32 entry_point_x = metaPage->multi_ep[x];
        if (visited.insert(entry_point_x).second) {
            VecBuffer buf_x = vec_read_buffer(index, entry_point_x, dim * sizeof(float));
            float *ep_x = (float *)buf_x.get_vecbuf();
            QueryNeighbor qn(entry_point_x, dist_func(query, ep_x, dim));
            buf_x.release();
            cq.insert(qn);
        }
    }

    SemanticGreedySearch_repair(index, query, ef, cq, closest_centroids, visited, metaPage, stats);
    visited.destroy();

    Vector<QueryNeighbor> candidates;
    candidates.clear();
    for (const auto &candidate : cq) {
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), QueryNeighbor::compare_distance);

    closest_centroids.destroy();
    cq.destroy();
    return candidates;
}

bool qaspgettuple_internal(IndexScanDesc scan, uint32 num_semantic_activated)
{
    QASPScanOpaque so = (QASPScanOpaque)scan->opaque;
    if (so->first) {
        uint32 ef = so->searchListSize;
        /* Count index scan for stats */
        pgstat_count_index_scan(scan->indexRelation);

        /* Safety check */
        if (scan->numberOfOrderBys <= 0 || scan->orderByData->sk_attno != 1) {
            ereport(ERROR, (errcode(ERRCODE_PLPGSQL_ERROR),
                            errmsg("cannot scan diskann index without vector order")));
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

        Buffer buf = AnnLoadBuffer(so->index, QASP_METAPAGE_BLKNO);
        QASPMetaPage *metaPage = QASPPageGetMeta(BufferGetPage(buf));
        Vector<QueryNeighbor> candidates =
            SingleQuerySearch(so->index, query, ef, num_semantic_activated, buf, metaPage);
        
        BlockNumber scan_data_meta_blkno = metaPage->scan_data_meta_blkno;
        UnlockReleaseBuffer(buf);
        if (alloced) {
            pfree(query);
        }
        so->currIndex = 0;
        so->lastIndex = (int)candidates.size();
        so->tids = (ItemPointerData *)palloc(so->lastIndex * sizeof(ItemPointerData));
        ItemPointer temp = so->tids;
        
        DiskVector<ScanData> dp(so->index, scan_data_meta_blkno, false);
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