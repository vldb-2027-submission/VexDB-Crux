#include "postgres.h"
#include "access/qasp/qasp.h"
#include "access/hnsw/hnsw.h"
#include <boost/optional/optional.hpp>
#include "utils/elog.h"
#include <algorithm> 

using namespace ann_helper;
using namespace disk_container;

#define MAX_PHYSICAL_EDGES 40

uint32 GetClosestSemanticCluster(Relation index, float* query, QASPMetaPage *metaPage) {
    const uint32 num_semantic_cluster = metaPage->num_semantic_cluster;
    const uint32 dim = metaPage->dimensions;
    float *centroid_vecs = (float *)palloc0(sizeof(float) * dim * num_semantic_cluster);
    DiskVector<float> centroids(index, metaPage->centers_meta_blkno, false);
    centroids.get_n<AccessorLockType::ReadLock>(0, num_semantic_cluster * dim, centroid_vecs);
    centroids.destroy();

    auto dist_func = get_general_distance_func(metaPage->metric, dim);
    uint32 best_cluster_id = 0;
    float min_dist = FLT_MAX;

    for (uint32 i = 0; i < num_semantic_cluster; i++) {
        float dist = dist_func(query, centroid_vecs + i * dim, dim);
        if (dist < min_dist) {
            min_dist = dist;
            best_cluster_id = i;
        }
    }
    pfree(centroid_vecs);
    return best_cluster_id;
}

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
    while (current_semantic < NUM_SEMANTIC_CLUSTER) {
        current_semantic++;
        start_idx[current_semantic] = edge_num;
    }
}

void SafeAppendEdges(
    Relation index,
    DiskVector<BaseEdges> &base_edges, 
    DiskVector<OverflowBucket> &overflow_buckets,
    uint32 node_id,
    uint32 semantic_id,
    const Vector<uint32> &new_candidates,
    DiskVector<edgeNumReminder> *edge_num_reminders = NULL) {
    
    bool actually_changed = false;
    
    base_edges.apply<AccessorLockType::WriteLock>([&](BaseEdges &base) -> bool {
        
        Vector<uint32_t> all_edges;
        for (int i = 0; i < base.edge_num; i++) {
            all_edges.push_back(base.edges[i]);
        }
        
        uint32_t curr_off = base.overflow_offset;
        Vector<uint32_t> existing_overflows;
        while (curr_off != (uint32_t)-1) {
            existing_overflows.push_back(curr_off);
            OverflowBucket ob = overflow_buckets.get<AccessorLockType::ReadLock>(curr_off);
            for (int i = 0; i < ob.edge_num; i++) {
                all_edges.push_back(ob.edges[i]);
            }
            curr_off = ob.next_bucket_offset;
        }

        Vector<uint32_t> cluster_edges[NUM_SEMANTIC_CLUSTER];
        for (uint32_t e : all_edges) {
            cluster_edges[decode_semantic(e)].push_back(decode_target(e));
        }

        bool changed = false;
        for (uint32 new_id : new_candidates) {
            if (new_id == node_id) continue;
            if (cluster_edges[semantic_id].size() >= MAX_PHYSICAL_EDGES) break; // 保护 MAX_PHYSICAL_EDGES 限制

            bool exists = false;
            for (uint32_t existing_id : cluster_edges[semantic_id]) {
                if (existing_id == new_id) { 
                    exists = true; 
                    break; 
                }
            }

            if (!exists) {
                cluster_edges[semantic_id].push_back(new_id);
                changed = true;
            }
        }

        if (!changed) {
            all_edges.destroy();
            existing_overflows.destroy();
            for(int i=0; i<NUM_SEMANTIC_CLUSTER; i++) cluster_edges[i].destroy();
            return false;
        }

        actually_changed = true;

        all_edges.clear();
        for (uint32 j = 0; j < NUM_SEMANTIC_CLUSTER; j++) {
            for (uint32 target : cluster_edges[j]) {
                all_edges.push_back(encode_edge(j, target));
            }
        }

        base.edge_num = std::min((size_t)BASE_EDGE_CAPACITY, all_edges.size());
        base.overflow_offset = (uint32_t)-1;
        for (size_t k = 0; k < base.edge_num; k++) {
            base.edges[k] = all_edges[k];
        }
        compute_start_idx(base.start_idx, base.edges, base.edge_num);

        if (all_edges.size() > BASE_EDGE_CAPACITY) {
            size_t num_overflow = (all_edges.size() - BASE_EDGE_CAPACITY + OVERFLOW_EDGE_CAPACITY - 1) / OVERFLOW_EDGE_CAPACITY;
            uint32_t first_overflow_offset = (uint32_t)-1;
            uint32_t last_overflow_offset = (uint32_t)-1;
            
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

                uint32_t current_offset;
                if (b < existing_overflows.size()) {
                    current_offset = existing_overflows[b];
                    overflow_buckets.set<AccessorLockType::WriteLock>(current_offset, bucket);
                } else {
                    current_offset = overflow_buckets.push_back(bucket);
                }

                if (b == 0) first_overflow_offset = current_offset;
                else {
                    overflow_buckets.apply<AccessorLockType::WriteLock>([&](OverflowBucket &prev) -> bool {
                        prev.next_bucket_offset = current_offset;
                        return true;
                    })(last_overflow_offset);
                }
                last_overflow_offset = current_offset;
            }
            base.overflow_offset = first_overflow_offset;
        }

        all_edges.destroy();
        existing_overflows.destroy();
        for(int i=0; i<NUM_SEMANTIC_CLUSTER; i++) cluster_edges[i].destroy();
        
        return true;
    })(node_id);

    if (actually_changed && edge_num_reminders != NULL) {
        edgeNumReminder reminder = edge_num_reminders->get<AccessorLockType::ReadLock>(node_id);
        
        uint32_t sem_count = 0;
        BaseEdges base = base_edges.get<AccessorLockType::ReadLock>(node_id);
        sem_count += (base.start_idx[semantic_id + 1] - base.start_idx[semantic_id]);
        
        uint32_t curr_off = base.overflow_offset;
        while (curr_off != (uint32_t)-1) {
            OverflowBucket ob = overflow_buckets.get<AccessorLockType::ReadLock>(curr_off);
            sem_count += (ob.start_idx[semantic_id + 1] - ob.start_idx[semantic_id]);
            curr_off = ob.next_bucket_offset;
        }

        reminder.Initialization_edges_num[semantic_id] = sem_count;
        reminder.Inter_cluster_edges_num[semantic_id] = 0;
        edge_num_reminders->set<AccessorLockType::WriteLock>(node_id, reminder);
    }
}

void approxSingleQueryRepair(float* ood_query, QASPMetaPage *metaPage, Buffer meta_buf, Relation index, uint32 num_ground_truth, uint32 repair_ef) {
    uint32 semantic_id = GetClosestSemanticCluster(index, ood_query, metaPage);
    
    SearchStats stats;
    Vector<QueryNeighbor> repaired_candidates = SingleQuerySearch_repair(index, ood_query, repair_ef, metaPage->num_semantic_cluster, meta_buf, metaPage, &stats);
    if (repaired_candidates.size() == 0) { repaired_candidates.destroy(); return; }

    DiskVector<BaseEdges> base_edges(index, metaPage->base_edges_meta_blkno, false);
    DiskVector<OverflowBucket> overflow_buckets(index, metaPage->overflow_buckets_meta_blkno, false);
    DiskVector<edgeNumReminder> edge_num_reminders(index, metaPage->edgeNumReminder_meta_blkno, false);

    uint32 pivot_num = 5;
    if (repaired_candidates.size() < pivot_num) pivot_num = repaired_candidates.size();

    for (uint32 k = 0; k < pivot_num; k++) {
        uint32 pivot_id = repaired_candidates[k].id;
        Vector<uint32> new_neighbors;
        for (uint32 i = 0; i < repaired_candidates.size(); i++) {
            if (repaired_candidates[i].id != pivot_id) new_neighbors.push_back(repaired_candidates[i].id);
        }
        
        SafeAppendEdges(index, base_edges, overflow_buckets, pivot_id, semantic_id, new_neighbors, &edge_num_reminders);
        
        Vector<uint32> reverse_candidate;
        reverse_candidate.push_back(pivot_id);
        for (uint32 neighbor_id : new_neighbors) {
            SafeAppendEdges(index, base_edges, overflow_buckets, neighbor_id, semantic_id, reverse_candidate, &edge_num_reminders);
        }
        new_neighbors.destroy();
        reverse_candidate.destroy();
    }
    
    repaired_candidates.destroy(); 
    base_edges.destroy(); 
    overflow_buckets.destroy(); 
    edge_num_reminders.destroy();
}

void repairSingleWithGT(Relation index, QASPMetaPage *metaPage, uint32 pivot_id, int32* logical_neighbors, uint32 num_neighbors) {
    if (num_neighbors == 0 || logical_neighbors == NULL) return;

    DiskVector<uint32_t> logical_to_physical(index, metaPage->logical_to_physical_meta_blkno, false);
    
    Vector<uint32> physical_neighbors;
    for (uint32 i = 0; i < num_neighbors; i++) {
        uint32 phys_id = logical_to_physical.get<AccessorLockType::ReadLock>((uint32)logical_neighbors[i]);
        physical_neighbors.push_back(phys_id);
    }
    logical_to_physical.destroy();
    uint32 top1_neighbor = physical_neighbors[0];
    
    VecBuffer buf = vec_read_buffer(index, top1_neighbor, metaPage->dimensions * sizeof(float)); 
    uint32 semantic_id = GetClosestSemanticCluster(index, (float*)buf.get_vecbuf(), metaPage);
    buf.release();

    DiskVector<BaseEdges> base_edges(index, metaPage->base_edges_meta_blkno, false);
    DiskVector<OverflowBucket> overflow_buckets(index, metaPage->overflow_buckets_meta_blkno, false);
    DiskVector<edgeNumReminder> edge_num_reminders(index, metaPage->edgeNumReminder_meta_blkno, false);

    uint32 pivot_num = 5;
    if (num_neighbors < pivot_num) pivot_num = num_neighbors;

    for (uint32 k = 0; k < pivot_num; k++) {
        uint32 current_pivot = physical_neighbors[k];
        Vector<uint32> new_neighbors;
        for (uint32 i = 0; i < num_neighbors; i++) {
            if (physical_neighbors[i] != current_pivot) {
                new_neighbors.push_back(physical_neighbors[i]);
            }
        }
        
        SafeAppendEdges(index, base_edges, overflow_buckets, current_pivot, semantic_id, new_neighbors, &edge_num_reminders);
        
        Vector<uint32> reverse_candidate;
        reverse_candidate.push_back(current_pivot);
        for (uint32 neighbor_id : new_neighbors) {
            SafeAppendEdges(index, base_edges, overflow_buckets, neighbor_id, semantic_id, reverse_candidate, &edge_num_reminders);
        }
        new_neighbors.destroy();
        reverse_candidate.destroy();
    }
    
    physical_neighbors.destroy();
    base_edges.destroy(); 
    overflow_buckets.destroy(); 
    edge_num_reminders.destroy();
}