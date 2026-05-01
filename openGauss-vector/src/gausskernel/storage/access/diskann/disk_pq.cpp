/**
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT license.
 * 
 * Copyright ...
 */

#include <algorithm>    /* min_element */
#include <vtl/disk_container/diskvector.hpp>

#include "access/diskann/disk_pq.h"
#include "access/diskann/partition.h"
#include "access/diskann/math_utils.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/module/timer.h"
#include "access/annvector/distance/distance.h"
#include "access/annvector/ann_utils.h"

using namespace disk_container;

static size_t count_distinct(const float *const passed_train_data, size_t num_train,
                             uint32 chunk_size, uint32 dim, uint32 offset)
{
    UnorderedSet<size_t> distinct(num_train);
    uint32 *hash_vals = (uint32 *)palloc(sizeof(uint32) * num_train);
    for (size_t i = 0; i < num_train; ++i) {
        hash_vals[i] = tag_hash(passed_train_data + i * dim + offset * chunk_size,
                                chunk_size * sizeof(float));
    }
    size_t out = 0;
    for (size_t i = 0; i < num_train; ++i) {
        if (distinct.insert(hash_vals[i]).second) {
            ++out;
        }
    }
    pfree(hash_vals);
    ann_helper::optional_destroy(distinct);
    return out;
}

size_t min_ndistinct_pivots(const float *const passed_train_data, size_t num_train, uint32 nchunk,
                            uint32 dim)
{
    Assert(nchunk > 0 && dim > 0);
    Assert(dim % nchunk == 0);
    uint32 chunk_size = dim / nchunk;
    size_t *min_ndistincts = (size_t *)palloc(sizeof(size_t) * nchunk);
    const auto task = [&](size_t offset) {
        min_ndistincts[offset] = count_distinct(passed_train_data, num_train, chunk_size, dim, offset);
    };
    ann_helper::Timer timer(nchunk, 10);
#if !defined(__aarch64__)
    INIT_TASK_RUNNER();
    START_TASK_POOL();
    for (uint32 offset = 0; offset < nchunk; ++offset) {
        RUN_TASK(task, offset);
        timer.report_loop("Verifying quantinization parameters");
    }
    WAIT_AND_END_TASK_POOL();
#else
    for (uint32 offset = 0; offset < nchunk; ++offset) {
        task(offset);
        timer.report_loop("Verifying quantinization parameters");
    }
#endif
    timer.report("Quantinization parameters verified");
    timer.destroy();
    size_t res = *std::min_element(min_ndistincts, min_ndistincts + nchunk);
    pfree(min_ndistincts);
    return res;
}

void generate_pq_pivots(DiskAnnBuildState *build_state, const float *const passed_train_data,
                        size_t num_train, uint32 max_k_means_reps)
{
    uint32 dim = build_state->dimensions;
    uint32 num_centers = build_state->numCenters;
    uint32 num_pq_chunks = build_state->numPQChunks;

    float *train_data = (float *)palloc(num_train * dim * sizeof(float));
    errno_t rc = memcpy_s(train_data, num_train * dim * sizeof(float),
        passed_train_data, num_train * dim * sizeof(float));
    securec_check(rc, "\0", "\0");

    build_state->pqPivots = (float *)palloc(num_centers * dim * sizeof(float));
    uint32 *chunk_offsets = (uint32 *)palloc((num_pq_chunks + 1u) * sizeof(uint32));
    const uint32 step_size = dim / num_pq_chunks;
    uint32 cur_offset = 0;
    for (uint32 b = 0; b < num_pq_chunks; ++b) {
        chunk_offsets[b] = cur_offset;
        cur_offset += step_size;
    }
    chunk_offsets[num_pq_chunks] = dim;
    build_state->pqChunkOffsets = chunk_offsets;

    char indexName[NAMEDATALEN + 1];
	char partIndexName[NAMEDATALEN + 1];
    populate_index_partition_name(build_state->index, indexName, partIndexName);
    ann_helper::Timer timer(num_pq_chunks, 10ul, indexName, partIndexName);
    timer.set_stage("PQ Train");

    const auto task = [&](size_t i) {
        size_t cur_chunk_size = chunk_offsets[i + 1ul] - chunk_offsets[i];
        if (cur_chunk_size == 0) {
            return;
        }
        float *cur_pivot_data = (float *)palloc(num_centers * cur_chunk_size * sizeof(float));
        float *cur_data = (float *)palloc(num_train * cur_chunk_size * sizeof(float));
        uint32 *closest_center = (uint32 *)palloc(num_train * sizeof(uint32));

        for (size_t j = 0; j < num_train; ++j) {
            for (size_t k = 0; k < cur_chunk_size; ++k) {
                cur_data[j * cur_chunk_size + k] = train_data[j * dim + chunk_offsets[i] + k];
            }
        }

        kmeanspp_selecting_pivots(cur_data, num_train, cur_chunk_size, cur_pivot_data, num_centers);
        run_lloyds(cur_data, num_train, cur_chunk_size, cur_pivot_data, num_centers,
                   max_k_means_reps, closest_center);

        for (uint32 j = 0; j < num_centers; ++j) {
            errno_t rc = memcpy_s(build_state->pqPivots + j * dim + chunk_offsets[i],
                cur_chunk_size * sizeof(float), cur_pivot_data + j * cur_chunk_size,
                cur_chunk_size * sizeof(float));
            securec_check(rc, "\0", "\0");
        }

        pfree(cur_pivot_data);
        pfree(cur_data);
        pfree(closest_center);
        timer.report_loop("Generating quantinization pivots");
    };

#if !defined(__aarch64__)
    INIT_TASK_RUNNER();
    START_TASK_POOL();

    for (size_t i = 0; i < num_pq_chunks; ++i) {
        RUN_TASK(task, i);
    }
    WAIT_AND_END_TASK_POOL();
#else
    for (size_t i = 0; i < num_pq_chunks; ++i) {
        task(i);
    }
#endif
    timer.report("Quantinization pivots generated");
    timer.destroy();
    pfree(train_data);

    /* PQ Pivots */
    build_state->pqPivotsMetaBlkNo = DiskVector<float>::get_disk_vector(build_state->index, false);
    DiskVector<float> pqPivotsVec(build_state->index, build_state->pqPivotsMetaBlkNo, false);
    pqPivotsVec.push_back_n(build_state->pqPivots, num_centers * dim);
    pqPivotsVec.destroy();
}

void generate_pq_data_from_pivots(DiskAnnBuildState *build_state)
{
    uint32 num_pq_chunks = build_state->numPQChunks;
    size_t num_points = build_state->numPoints;
    build_state->pqCompressedMetaBlkNo = DiskVector<uint8>::get_disk_vector(build_state->index, false);
    DiskVector<uint8> pqCompressedVec(build_state->index, build_state->pqCompressedMetaBlkNo, false);
    /* the first point is kept as start point (medoid) */
    pqCompressedVec.push_back_n(std::forward<uint8>(0u), num_pq_chunks);
    const size_t dim = build_state->dimensions;

    const auto task = [build_state, num_pq_chunks, dim, &pqCompressedVec](
            uint32 block_data_offset, size_t offset, size_t cur_blk_size, size_t block) {
        const uint32 num_centers = build_state->numCenters;
        const uint32 *chunk_offsets = build_state->pqChunkOffsets;
        const float *full_pivot_data = build_state->pqPivots;
        uint8 *block_compressed_base = (uint8 *)palloc(cur_blk_size * num_pq_chunks * sizeof(uint8));
        float *block_data_float = (float *)palloc(cur_blk_size * dim * sizeof(float));

        if (build_state->hasLargeData) {
            vec_read(build_state->index->rd_smgr, block_data_offset * sizeof(float),
                     cur_blk_size * dim * sizeof(float), (char *)block_data_float);
        } else {
            errno_t rc = memcpy_s(block_data_float, cur_blk_size * dim * sizeof(float),
                build_state->data.data() + block_data_offset, cur_blk_size * dim * sizeof(float));
            securec_check(rc, "\0", "\0");
        }

        /* Normalize original data if needed */
        auto preprocessor = ann_helper::get_vector_preprocess_func(build_state->metric);
        if (preprocessor) {
            for (size_t i = 0; i < cur_blk_size; i++) {
                preprocessor(block_data_float + i * dim, dim, block_data_float + i * dim);
            }
        }

        const auto task_internal = [&](size_t i) {
            const size_t cur_chunk_size = chunk_offsets[i + 1] - chunk_offsets[i];
            if (cur_chunk_size == 0) {
                return;
            }

            float *cur_pivot_data = (float *)palloc(num_centers * cur_chunk_size * sizeof(float));
            float *cur_data = (float *)palloc(cur_blk_size * cur_chunk_size * sizeof(float));
            uint32 *closest_center = (uint32 *)palloc(cur_blk_size * sizeof(uint32));
            for (size_t j = 0; j < cur_blk_size; ++j) {
                for (size_t k = 0; k < cur_chunk_size; ++k) {
                    cur_data[j * cur_chunk_size + k] = block_data_float[j * dim + chunk_offsets[i] + k];
                }
            }
            for (size_t j = 0; j < num_centers; ++j) {
                errno_t rc = memcpy_s(cur_pivot_data + j * cur_chunk_size,
                    cur_chunk_size * sizeof(float), full_pivot_data + j * dim + chunk_offsets[i],
                    cur_chunk_size * sizeof(float));
                securec_check(rc, "\0", "\0");
            }

            compute_closest_centers(cur_data, cur_blk_size, cur_chunk_size, cur_pivot_data,
                                    num_centers, 1ul, closest_center);
            for (size_t j = 0; j < cur_blk_size; ++j) {
                block_compressed_base[j * num_pq_chunks + i] = uint8(closest_center[j]);
            }

            pfree(cur_pivot_data);
            pfree(cur_data);
            pfree(closest_center);
        };
        ++block;
        ann_helper::Timer timer(num_pq_chunks, 10);
#if !defined(__aarch64__)
        START_TASK_POOL();
        for (size_t i = 0; i < num_pq_chunks; ++i) {
            RUN_TASK(task_internal, i);
            timer.report_loop("Quantinization task %lu", block);
        }
        WAIT_AND_END_TASK_POOL();
#else
        for (size_t i = 0; i < num_pq_chunks; ++i) {
            task_internal(i);
            timer.report_loop("Quantinization task %lu", block);
        }
#endif
        timer.report_loop("Quantinization task %lu done", block);
        timer.destroy();
        pfree(block_data_float);
        pqCompressedVec.push_back_n(block_compressed_base, cur_blk_size * num_pq_chunks);
        pfree(block_compressed_base);
    };

#if !defined(__aarch64__)
    INIT_TASK_RUNNER();
#endif
    size_t offset = num_pq_chunks;
    constexpr size_t max_block_size = 100'000lu;
    const size_t block_size = std::min(max_block_size, MaxAllocSize / dim / sizeof(float));
    size_t num_blocks = (num_points + block_size - 1) / block_size;
    uint32 block_data_offset = dim;
    ann_helper::Timer timer(num_blocks, 1);
    for (size_t block = 0; block < num_blocks; ++block) {
        const size_t start_id = block * block_size;
        const size_t cur_blk_size = start_id + block_size > num_points ?
            num_points - start_id :
            block_size;
        timer.report_loop("Launched quantinization task");
        task(block_data_offset, offset, cur_blk_size, block);
        offset += cur_blk_size * num_pq_chunks;
        block_data_offset += cur_blk_size * dim;
    }
    /* copy medoid as the first point */
    timer.report("Quantinization done");
    timer.destroy();
    pqCompressedVec.destroy();
}
