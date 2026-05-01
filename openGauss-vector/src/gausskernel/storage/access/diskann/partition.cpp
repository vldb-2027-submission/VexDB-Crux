/** 
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT license.
 * Copyright ...
 */

#include <random>

#include "access/diskann/disk_pq.h"
#include "access/diskann/partition.h"
#include "access/diskann/diskann.h"
#include "access/diskann/math_utils.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/annvector/distance/distance.h"
#include "access/annvector/annkmeans.h"
#include "lib/pairingheap.h"

using namespace ann_helper;
using disk_container::DiskVector;
using disk_container::AccessorLockType;

static size_t max_block_size(uint32 dim)
{
    constexpr size_t BLOCK_SIZE = 200'000ul;
    const size_t res = MaxAllocSize / dim / sizeof(float);
    return std::min(res, BLOCK_SIZE);
}

static uint32 div_round_up(uint32 x, uint32 y) { return (x + y - 1u) / y; }

void gen_random_slice(DiskAnnBuildState *buildState, float *&sampled_data, uint32 *slice_size)
{
    uint32 npts = buildState->numPoints;
    uint32 dim = buildState->dimensions;
    double p_val = buildState->samplingRate;

    Vector<float *> sampled_vectors;
    size_t vec_size = dim * sizeof(float);

    p_val = p_val < 1 ? p_val : 1;
    std::random_device rd; /* Will be used to obtain a seed for the random number engine */
    uint32 x = rd();
    std::mt19937 generator((uint32)x); /* Standard mersenne_twister_engine seeded with rd() */
    std::uniform_real_distribution<float> distribution(0, 1);

    for (uint32 i = 0; i < npts; ++i) {
        float rnd_val = distribution(generator);
        if (rnd_val < p_val) {
            float *point = (float *)palloc(vec_size);
            if (buildState->hasLargeData) {
                read_vector(buildState->index, i + 1u, dim, (char *)point);
            } else {
                errno_t rc = memcpy_s(point, vec_size,
                                      buildState->data.data() + (i + 1u) * dim, vec_size);
                securec_check(rc, "\0", "\0");
            }
            sampled_vectors.push_back(point);
        }
    }

    *slice_size = sampled_vectors.size();
    sampled_data = (float *)palloc(*slice_size * vec_size);
    for (uint32 i = 0; i < *slice_size; ++i) {
        errno_t rc = memcpy_s(sampled_data + i * dim, vec_size, sampled_vectors[i], vec_size);
        securec_check(rc, "\0", "\0");
        pfree(sampled_vectors[i]);
    }
    ann_helper::optional_destroy(sampled_vectors);

    /* Normalize sampled data if needed */
    auto preprocessor = ann_helper::get_vector_preprocess_func(buildState->metric);
    if (preprocessor && !buildState->hasLargeData) {
        for (size_t i = 0; i < *slice_size; ++i) {
            preprocessor(sampled_data + i * dim, dim, sampled_data + i * dim);
        }
    }
}

struct CenterDistance {
    int centroidIndex;
    double distance;
};

typedef struct CenterDistancePairingHeapNode
{
	pairingheap_node ph_node;
	CenterDistance *inner;
}			CenterDistancePairingHeapNode;


static inline CenterDistancePairingHeapNode *
CreatePairingHeapNode(CenterDistance * c)
{
	CenterDistancePairingHeapNode *node = (CenterDistancePairingHeapNode * )palloc(sizeof(CenterDistancePairingHeapNode));

	node->inner = c;
	return node;
}

static inline int
CompareClosestCenter(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	if (((const CenterDistancePairingHeapNode *) a)->inner->distance < ((const CenterDistancePairingHeapNode *) b)->inner->distance)
		return 1;

	if (((const CenterDistancePairingHeapNode *) a)->inner->distance > ((const CenterDistancePairingHeapNode *) b)->inner->distance)
		return -1;

	return 0;
}


static void estimate_cluster_sizes(DiskAnnBuildState *buildState, float *test_data_float, uint32 num_test,
                                    FloatVectorArray centers, const uint32 test_dim, Vector<uint32> &cluster_sizes)
{
    const uint32 num_shards = centers->length;
    uint32 *shard_counts = (uint32 *)palloc0(num_shards * sizeof(uint32));
    distance_func dist_func = get_general_distance_func(buildState->metric, test_dim);

    float adaptive_relax = DiskAnnGetSubGraphAdaptiveRelaxation(buildState->index);
    size_t shard_max_vector = DiskAnnGetSubGraphMaxVectorsFactor(buildState->index) * num_test / num_shards;
    for (uint32 i = 0; i < num_test; ++i) {
        MemoryContext tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "shard temporary context",
									  ALLOCSET_DEFAULT_SIZES); 
        MemoryContext oldCtx = MemoryContextSwitchTo(tmpCtx);                                          
        pairingheap *closest_centers = pairingheap_allocate(CompareClosestCenter, NULL);
        for (uint32 j = 0; j < num_shards; ++j) {
            float dist = dist_func(test_data_float + i * test_dim,  FloatVectorArrayGet(centers, j), test_dim);
            CenterDistance *centerdist = (CenterDistance*)palloc(sizeof(CenterDistance));
            centerdist->centroidIndex = j;
            centerdist->distance = dist;
            pairingheap_add(closest_centers, &(CreatePairingHeapNode(centerdist)->ph_node));
        }

        size_t curOLPCnt = 0;
        float curOLPFactor = 0.0;
        float accDist = 0.0;
        float curAVGDist = std::numeric_limits<float>::max();
        int closest_shard_id = -1;

        while (!pairingheap_is_empty(closest_centers) && curOLPCnt < MAX_OVERLAPPING_FACTOR) {
            CenterDistance *centerdist = ((CenterDistancePairingHeapNode *)pairingheap_remove_first(closest_centers))->inner;
            int shard_id = centerdist->centroidIndex;
            float dist = centerdist->distance;

            if (closest_shard_id == -1) {
                closest_shard_id = shard_id;  
            }

            if (dist <= curAVGDist + adaptive_relax * abs(curAVGDist)) {
                curOLPFactor += 1.0;
                accDist += dist;
                curAVGDist = accDist / curOLPFactor;
                
                if (shard_counts[shard_id] < shard_max_vector) {
                    ++curOLPCnt;
                    ++shard_counts[shard_id];
                } else {
                    curAVGDist = std::numeric_limits<float>::max();
                }
            }
        }

        if (curOLPCnt < 1ul) {
            ++shard_counts[closest_shard_id];
        }

        MemoryContextSwitchTo(oldCtx);
	    MemoryContextDelete(tmpCtx);
    }

    cluster_sizes.clear();
    cluster_sizes.reserve(num_shards);
    for (uint32 i = 0; i < num_shards; ++i) {
        cluster_sizes.push_back(shard_counts[i]);
    }

    pfree(shard_counts);
}

/* useful for partitioning large dataset. we first generate only the IDS for
   each shard, and retrieve the actual vectors on demand. */
static void shard_data_into_clusters_only_ids(DiskAnnBuildState *buildState, FloatVectorArray centers)
{
    uint32 dim = buildState->dimensions;
    uint32 num_shards = buildState->numShards;
    float *block_data_float = (float *)palloc(dim * sizeof(float));
    Assert(buildState->hasLargeData);

    float adaptive_relax = DiskAnnGetSubGraphAdaptiveRelaxation(buildState->index);
    size_t shard_max_vector = DiskAnnGetSubGraphMaxVectorsFactor(buildState->index) * buildState->numPoints / num_shards;

    distance_func dist_func = get_general_distance_func(buildState->metric, buildState->dimensions);
    for (size_t i = 1; i <= buildState->numPoints; ++i) {
        vec_read(buildState->index->rd_smgr, i * dim * sizeof(float), dim * sizeof(float), (char *)block_data_float);
        MemoryContext tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
			"shard temporary context", ALLOCSET_DEFAULT_SIZES); 
        MemoryContext oldCtx = MemoryContextSwitchTo(tmpCtx);                                          

        pairingheap *closest_centers = pairingheap_allocate(CompareClosestCenter, NULL);
        for (uint32 j = 0; j < num_shards; ++j) {
            float dist = dist_func(block_data_float, FloatVectorArrayGet(centers, j), dim);
            CenterDistance *centerdist = (CenterDistance*)palloc(sizeof(CenterDistance));
            centerdist->centroidIndex = j;
            centerdist->distance = dist;
            pairingheap_add(closest_centers, &(CreatePairingHeapNode(centerdist)->ph_node));
        }

        size_t curOLPCnt = 0;
        float curOLPFactor = 0.0;
        float accDist = 0.0;
        float curAVGDist = std::numeric_limits<float>::max();
        int closest_shard_id = -1;

        while (!pairingheap_is_empty(closest_centers) && curOLPCnt < MAX_OVERLAPPING_FACTOR) {
            CenterDistance *centerdist = ((CenterDistancePairingHeapNode *)pairingheap_remove_first(closest_centers))->inner;
            int shard_id = centerdist->centroidIndex;
            float dist = centerdist->distance;

            if (closest_shard_id == -1) {
                closest_shard_id = shard_id;  
            }

            if (dist <= curAVGDist + adaptive_relax * abs(curAVGDist)) {
                curOLPFactor += 1.0;
                accDist += dist;
                curAVGDist = accDist / curOLPFactor;
                
                if (buildState->shardIDs[shard_id].size() < shard_max_vector) {
                    ++curOLPCnt;
                    buildState->shardIDs[shard_id].push_back(i - 1ul);
                } else {
                    curAVGDist = std::numeric_limits<float>::max();
                }
            }
        }

        if(curOLPCnt < 1ul) {
             buildState->shardIDs[closest_shard_id].push_back(i - 1ul);
        }

        MemoryContextSwitchTo(oldCtx);
	    MemoryContextDelete(tmpCtx);
    }

    size_t total = 0;
    for(uint32 i = 0; i < num_shards; ++i) {
        elog(NOTICE, "shard index:%u, size:%lu", i, buildState->shardIDs[i].size());
        total += buildState->shardIDs[i].size();
    }

    elog(NOTICE, "numPoints:%lu, sub-graphs total points:%lu", buildState->numPoints, total);
    pfree(block_data_float);
}

void retrieve_shard_data_from_ids(DiskAnnBuildState *buildState, uint32 shardIndex)
{
    uint32 num_points = buildState->numPoints;
    uint32 dim = buildState->dimensions;

    tmpvector<uint32> shard_ids = buildState->shardIDs[shardIndex];
    uint64_t shard_size = shard_ids.size();
    uint32 cur_pos = 0;
    uint32 block_size = max_block_size(dim);

    float *block_data_float = (float *)palloc(block_size * dim * sizeof(float));
    uint32 block_data_offset = dim;
    auto *block_nodes_data = (DiskAnnVamanaNode *)palloc(block_size * sizeof(DiskAnnVamanaNode));
    uint32 block_nodes_offset = 0;
    uint32 num_blocks = div_round_up(num_points, block_size);
    Assert(buildState->hasLargeData);

    DiskVector<DiskAnnVamanaNode> nodes(buildState->index, buildState->nodeMetaBlkNo, false);
    for (uint32 block = 0; block < num_blocks; ++block) {
        uint32 start_id = block * block_size;
        uint32 end_id = std::min((block + 1) * block_size, num_points);
        uint32 cur_blk_size = end_id - start_id;

        vec_read(buildState->index->rd_smgr, block_data_offset * sizeof(float),
                 cur_blk_size * dim * sizeof(float), (char *)block_data_float);
        block_data_offset += cur_blk_size * dim;

        nodes.get_n<AccessorLockType::NoLockRead>(block_nodes_offset, cur_blk_size, block_nodes_data);
        block_nodes_offset += cur_blk_size;

        for (uint32 p = 0; p < cur_blk_size; ++p) {
            uint32 original_point_map_id = start_id + p;
            if (cur_pos == shard_size) {
                break;
            }
            if (original_point_map_id == shard_ids[cur_pos]) {
                ++cur_pos;
                buildState->shardData[shardIndex].push_back_n(block_data_float + p * dim, dim);
                buildState->shardNodes[shardIndex].push_back(block_nodes_data[p]);
            }
        }
        if (cur_pos == shard_size) {
            break;
        }
    }

    Assert(shard_size == buildState->shardData[shardIndex].size() / dim);
    nodes.destroy();

    pfree(block_data_float);
    pfree(block_nodes_data);
}

static void allocate_shard_resources(DiskAnnBuildState *buildState)
{
    uint32 num_shards = buildState->numShards;

    buildState->shardIDs.reserve(num_shards);
    buildState->shardData.reserve(num_shards);
    buildState->shardNodes.reserve(num_shards);
    buildState->shardGraphs.reserve(num_shards);
    buildState->shardMedoids.reserve(num_shards);

    for (uint32 i = 0; i < num_shards; ++i) {
        tmpvector<uint32> ids;
        buildState->shardIDs.push_back(ids);
        tmpvector<float> data;
        buildState->shardData.push_back(data);
        tmpvector<DiskAnnVamanaNode> nodes;
        buildState->shardNodes.push_back(nodes);
        tmpvector<AnnNeighbors> subGraph;
        buildState->shardGraphs.push_back(subGraph);
        buildState->shardMedoids.push_back(0);
    }
}

/**
 * @brief
 * partitions a large base file into many shards using k-means hueristic
 * on a random sample generated using sampling_rate probability. After this, it
 * assignes each base point to the closest k_base nearest centers and creates the shards.
 * The total number of points across all shards will be k_base * num_points.
 */
bool partition_with_ram_budget(DiskAnnBuildState *buildState, uint32 k_base)
{
    uint32 num_shards = buildState->numPoints * k_base / buildState->maxNumPointsInMem + 1;
    bool fit_in_ram = false;

    uint32 train_dim = buildState->dimensions;
    double sampling_rate = buildState->samplingRate;
    uint32 max_degree = DiskAnnGetMaxGraphDegree(buildState->index);
    uint32 graph_degree = 2 * max_degree / 3;

    uint32 num_train;
    float *train_data_float;
    gen_random_slice(buildState, train_data_float, &num_train);

    uint32 num_test;
    float *test_data_float;
    gen_random_slice(buildState, test_data_float, &num_test);

    INIT_TASK_RUNNER();
    if (buildState->numThreads > 1) {
        LAUNCH_CONSUMER(buildState->numThreads);
    }

    AnnKmeansState *kmeanstate = (AnnKmeansState*)palloc0(sizeof(AnnKmeansState));
	setupKmeansState(buildState->metric, buildState->index, kmeanstate, buildState->dimensions, false, false);
    FloatVectorArray centers = nullptr;
    FloatVectorArray samples = (FloatVectorArray)palloc(sizeof(FloatVectorArrayData));
    samples->length = num_train;
    samples->maxlen = num_train;
    samples->dim = train_dim;
    samples->items = train_data_float;
    while (!fit_in_ram) {
        fit_in_ram = true;
        double max_ram_usage = 0;
        centers = FloatVectorArrayInit(num_shards, train_dim);
	    AnnKmeans(kmeanstate, samples, centers, u_sess->attr.attr_memory.maintenance_work_mem, buildState->numThreads);

        /* now pivots are ready. need to stream base points and assign them to closest clusters. */
        Vector<uint32> cluster_sizes;
        estimate_cluster_sizes(buildState, test_data_float, num_test, centers, train_dim, cluster_sizes);

        for (auto p : cluster_sizes) {
            /* to account for the fact that p is the size of the shard over the testing sample. */
            p /= sampling_rate;
            double cur_shard_ram_estimate = estimate_ram_usage(p, train_dim, sizeof(float), graph_degree);

            if (cur_shard_ram_estimate > max_ram_usage) {
                max_ram_usage = cur_shard_ram_estimate;
            }
        }
        ann_helper::optional_destroy(cluster_sizes);

        if (max_ram_usage > buildState->maxWorkMem) {
            fit_in_ram = false;
            num_shards += 2;
            FloatVectorArrayFree(centers);
            if (num_shards > MAX_ANN_SUBGRAPH_COUNT) {
                DESTROY_TASK_RUNNER();
                return false;
            }
        }
    }
    DESTROY_TASK_RUNNER();
    pfree(train_data_float);
    pfree(samples);
    pfree(test_data_float);
    FREE_ANNKEMANSTATE(kmeanstate);

    buildState->numShards = num_shards;
    allocate_shard_resources(buildState);    
    shard_data_into_clusters_only_ids(buildState, centers);

    FloatVectorArrayFree(centers);
    return true;
}

void merge_shards(DiskAnnBuildState *buildState)
{
    size_t max_degree = DiskAnnGetMaxGraphDegree(buildState->index);
    uint32 num_shards = buildState->numShards;
    /* find max node id */
    uint32 nnodes = 0;
    uint32 nelems = 0;
    for (uint32 shard = 0; shard < num_shards; ++shard) {
        for (uint32 idx = 0; idx < buildState->shardIDs[shard].size(); ++idx) {
            nnodes = std::max(nnodes, buildState->shardIDs[shard][idx]);
        }
        nelems += buildState->shardIDs[shard].size();
    }
    ++nnodes;

    /* compute inverse map: node -> shards */
    Vector<Pair<uint32, uint32>> node_shard;
    node_shard.reserve(nelems);
    for (uint32 shard = 0; shard < num_shards; ++shard) {
        for (uint32 idx = 0; idx < buildState->shardIDs[shard].size(); ++idx) {
            uint32 node_id = buildState->shardIDs[shard][idx];
            node_shard.push_back(vtl::make_pair(node_id, shard));
        }
    }

    std::sort(node_shard.begin(), node_shard.end(), [](const auto &left, const auto &right) {
        return left.first < right.first || (left.first == right.first && left.second < right.second);
    });

    std::random_device rng;
    std::mt19937 urng(rng());

    uint32 nnbrs = 0;
    uint32 cur_id = 0;
    uint32 *shard_idx = (uint32 *)palloc0(num_shards * sizeof(uint32));
    for(uint32 i = 0; i < num_shards; i++){
        shard_idx[i] = 1u;
    }
    bool *nhood_set = (bool *)palloc0(nnodes * sizeof(bool));
    Vector<uint32> final_nhood;

    DiskVector<AnnNeighbors> neighbors(buildState->index, buildState->graphMetaBlkNo, false);
    neighbors.push_back({0, 0, });

    for (const auto &id_shard : node_shard) {
        uint32 node_id = id_shard.first;
        uint32 shard_id = id_shard.second;

        if (cur_id < node_id) {
            std::shuffle(final_nhood.begin(), final_nhood.end(), urng);
            nnbrs = (uint32)std::min(final_nhood.size(), max_degree);

            AnnNeighbors nbrs;
            nbrs.num_neighbors = nnbrs;
            for (uint32 i = 0; i < nbrs.num_neighbors; ++i) {
                nbrs.neighbors[i] = final_nhood[i] + 1u;
            }
            neighbors.push_back(nbrs);

            cur_id = node_id;
            nnbrs = 0;
            for (uint32 p : final_nhood) {
                nhood_set[p] = 0;
            }
            final_nhood.clear();
        }

        AnnNeighbors nbrs = buildState->shardGraphs[shard_id].get(shard_idx[shard_id]++);
        Vector<uint32> shard_nhood(nbrs.num_neighbors);
        for (uint32 i = 0; i < nbrs.num_neighbors; ++i) {
            uint32 location = nbrs.neighbors[i];
            location = location == 0 ? buildState->shardMedoids[shard_id] : location - 1u;
            shard_nhood.push_back(location);
        }

        for (uint32 i = 0; i < nbrs.num_neighbors; ++i) {
            uint32 nhood_id = buildState->shardIDs[shard_id][shard_nhood[i]];
            if (!nhood_set[nhood_id]) {
                nhood_set[nhood_id] = true;
                final_nhood.push_back(nhood_id);
            }
        }
        ann_helper::optional_destroy(shard_nhood);
    }
    pfree(shard_idx);
    pfree(nhood_set);
    ann_helper::optional_destroy(node_shard);

    std::shuffle(final_nhood.begin(), final_nhood.end(), urng);
    nnbrs = (uint32)std::min(final_nhood.size(), max_degree);

    AnnNeighbors nbrs;
    nbrs.num_neighbors = nnbrs;
    for (uint32 i = 0; i < nbrs.num_neighbors; ++i) {
        nbrs.neighbors[i] = final_nhood[i];
    }
    neighbors.push_back(nbrs);
    neighbors.destroy();
    ann_helper::optional_destroy(final_nhood);
}
