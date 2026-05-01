/**
 * Copyright ...
 */

#include <random>

#include <vtl/vector>
#include <vtl/hashtable>

#include "miscadmin.h"
#include "utils/palloc.h"
#include "access/diskann/math_utils.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/distance/distance.h"
#include "access/annvector/distance/cblas_interface.h"

static float calc_distance(const float *vec_1, const float *vec_2, size_t dim)
{
    float dist = 0;
    for (size_t j = 0; j < dim; ++j) {
        dist += (vec_1[j] - vec_2[j]) * (vec_1[j] - vec_2[j]);
    }
    return dist;
}

static void compute_vecs_l2sq(float *vecs_l2sq, float *data, const size_t num_points, const size_t dim)
{
    for (size_t i = 0; i < num_points; ++i) {
        vecs_l2sq[i] = snrm_sqr(dim, (data + (i * dim)), 1);
    }
}

struct PivotContainer {
    size_t piv_id;
    float dist;
    PivotContainer(size_t piv_id, float dist) : piv_id(piv_id), dist(dist) {}
    bool operator<(const PivotContainer &rhs) const { return dist < rhs.dist; }
};

/**
 * calculate k closest centers to data of num_points * dim (row major)
 * centers is num_centers * dim (row major)
 * data_l2sq has pre-computed squared norms of data
 * centers_l2sq has pre-computed squared norms of centers
 * pre-allocated center_index will contain id of nearest center
 * pre-allocated dist_matrix shound be num_points * num_centers and contain squared distances
 */
static void compute_closest_centers_in_block(const float *const data, const size_t num_points,
    const size_t dim, const float *const centers, const size_t num_centers, const float *const docs_l2sq,
    const float *const centers_l2sq, uint32_t *center_index, float *const dist_matrix, size_t k)
{
    Assert(k <= num_centers && k > 0);
    ann_helper::pairwise_distance(Metric::L2, data, centers, docs_l2sq, centers_l2sq,
        dim, num_points, num_centers, dist_matrix);

    if (k == 1) {
        const auto task = [num_centers, dist_matrix, center_index](size_t i) {
            center_index[i] = cblas_isamin(num_centers, dist_matrix + (i * num_centers), 1);
        };
        for (size_t i = 0; i < num_points; ++i) {
            task(i);
        }
    } else {
        const auto task = [num_centers, dist_matrix, center_index, k](size_t i) {
            float *current = dist_matrix + (i * num_centers);
            PivotContainer *pivot_container = (PivotContainer *)palloc(sizeof(PivotContainer) * num_centers);
            for (size_t j = 0; j < num_centers; ++j) {
                new (pivot_container + j) PivotContainer(j, current[j]);
            }
            std::nth_element(pivot_container, pivot_container + k, pivot_container + num_centers);
            for (size_t j = 0; j < k; ++j) {
                center_index[i * k + j] = pivot_container[j].piv_id;
            }
            pfree(pivot_container);
        };
        for (size_t i = 0; i < num_points; ++i) {
            task(i);
        }
    }
}

/**
 * @brief
 * Given data in num_points * new_dim row major
 * Pivots stored in full_pivot_data as num_centers * new_dim row major
 * Calculate the k closest pivot for each point and store it in vector
 * closest_centers_ivf (row major, num_points*k) (which needs to be allocated
 * outside).
 * Additionally, if inverted index is not null (and pre-allocated), it
 * will return inverted index for each center, assuming each of the inverted
 * indices is an empty vector.
 * Additionally, if pts_norms_squared is not null, then it will assume that
 * point norms are pre-computed and use those values.
 */
void compute_closest_centers(float *data, size_t num_points, size_t dim, float *pivot_data, size_t num_centers,
                             size_t k, uint32_t *closest_centers_ivf, float *pts_norms_squared)
{
    Assert (k <= num_centers);
    float *pivs_norms_squared = NULL;
    bool pts_norms_squared_alloced = false;

    if (!pts_norms_squared) {
        pts_norms_squared = (float *)palloc(sizeof(float) * num_points);
        compute_vecs_l2sq(pts_norms_squared, data, num_points, dim);
        pts_norms_squared_alloced = true;
    }
    pivs_norms_squared = (float *)palloc(sizeof(float) * num_centers);
    compute_vecs_l2sq(pivs_norms_squared, pivot_data, num_centers, dim);

    const size_t PAR_BLOCK_SIZE = 10 * 1024lu * 1024lu / sizeof(float) / num_centers - 1lu;
    float *distance_matrix;
    if (num_points <= PAR_BLOCK_SIZE) {
        distance_matrix = (float *)palloc(sizeof(float) * num_centers * num_points);
        compute_closest_centers_in_block(data, num_points, dim, pivot_data, num_centers,
            pts_norms_squared, pivs_norms_squared, closest_centers_ivf, distance_matrix, k);
    } else {
        distance_matrix = (float *)palloc(sizeof(float) * num_centers * PAR_BLOCK_SIZE);
        const size_t N_BLOCKS =
            (num_points % PAR_BLOCK_SIZE) == 0 ? (num_points / PAR_BLOCK_SIZE) : (num_points / PAR_BLOCK_SIZE) + 1;
        const auto task = [data, num_points, dim, pivot_data, num_centers, pts_norms_squared, pivs_norms_squared,
                           closest_centers_ivf, distance_matrix, k, PAR_BLOCK_SIZE](size_t cur_blk) {
            size_t num_pts_blk = std::min(PAR_BLOCK_SIZE, num_points - cur_blk * PAR_BLOCK_SIZE);
            float *data_cur_blk = data + cur_blk * PAR_BLOCK_SIZE * dim;
            float *pts_norms_blk = pts_norms_squared + cur_blk * PAR_BLOCK_SIZE;
            uint32_t *closest_centers = closest_centers_ivf + cur_blk * PAR_BLOCK_SIZE * k;
            compute_closest_centers_in_block(data_cur_blk, num_pts_blk, dim, pivot_data, num_centers,
                pts_norms_blk, pivs_norms_squared, closest_centers, distance_matrix, k);
        };
#if !defined(__aarch64__)
        INIT_TASK_RUNNER();
        START_TASK_POOL();
        for (size_t i = 0; i < N_BLOCKS; ++i) {
            RUN_TASK(task, i);
        }
        WAIT_AND_END_TASK_POOL();
#else
        for (size_t i = 0; i < N_BLOCKS; ++i) {
            task(i);
        }
#endif
    }

    pfree(distance_matrix);
    pfree_ext(pivs_norms_squared);
    if (pts_norms_squared_alloced) {
        pfree(pts_norms_squared);
    }
}

/**
 * Run Lloyds one iteration
 * Given data in row major num_points * dim, centers in row major num_centers * dim,
 * and squared lengths of data points, output the closest center to each data point and update centers.
 */ 
static float lloyds_iter(float *data, size_t num_points, size_t dim, float *centers, size_t num_centers, float *docs_l2sq,
                         uint32_t *&closest_center)
{
    compute_closest_centers(data, num_points, dim, centers, num_centers, 1, closest_center, docs_l2sq);
    errno_t rc = memset_s(closest_center, sizeof(uint32_t) * num_points, 0, sizeof(uint32_t) * num_points);
    securec_check(rc, "\0", "\0");

    const auto task1 = [data, centers, dim, num_points, num_centers, closest_center](size_t c) {
        float *center = centers + (size_t)c * (size_t)dim;
        double *cluster_sum = (double *)palloc0(sizeof(double) * dim);
        size_t counter = 0;
        for (size_t i = 0; i < num_points; ++i) {
            if (closest_center[i] != c) {
                continue;
            }
            for (size_t j = 0; j < dim; ++j) {
                cluster_sum[j] += data[i * dim + j];
            }
            ++counter;
        }
        if (counter > 0) {
            for (size_t i = 0; i < dim; ++i) {
                center[i] = float(cluster_sum[i] / counter);
            }
        }
        pfree(cluster_sum);
    };

#if !defined(__aarch64__)
    INIT_TASK_RUNNER();
    START_TASK_POOL();
    for (size_t c = 0; c < num_centers; ++c) {
        RUN_TASK(task1, c);
    }
    WAIT_TASK();
#else
    for (size_t c = 0; c < num_centers; ++c) {
        task1(c);
    }
#endif

    constexpr size_t CHUNK_SIZE = 2 * 8192;
    size_t nchunks = num_points / CHUNK_SIZE + (num_points % CHUNK_SIZE == 0 ? 0 : 1);
    float *residuals = (float *)palloc0(sizeof(float) * nchunks);
    const auto task2 = [data, centers, dim, num_points, closest_center, residuals](size_t chunk) {
        for (size_t d = chunk * CHUNK_SIZE; d < num_points && d < (chunk + 1) * CHUNK_SIZE; ++d) {
            residuals[chunk] += calc_distance(data + (d * dim), centers + closest_center[d] * dim, dim);
        }
    };

#if !defined(__aarch64__)
    for (size_t chunk = 0; chunk < nchunks; ++chunk) {
        RUN_TASK(task2, chunk);
    }
    WAIT_AND_END_TASK_POOL();
#else
    for (size_t chunk = 0; chunk < nchunks; ++chunk) {
        task2(chunk);
    }
#endif

    float residual = 0.0;
    for (size_t chunk = 0; chunk < nchunks; ++chunk) {
        residual += residuals[chunk];
    }
    pfree(residuals);
    return residual;
}

/**
 * Run Lloyds until max_reps or stopping criterion
 * Final centers are output in centers as row major num_centers * dim
 */
float run_lloyds(float *data, size_t num_points, size_t dim, float *centers, const size_t num_centers,
                 const size_t max_reps, uint32_t *closest_center)
{
    bool ret_closest_center = true;
    if (closest_center == NULL) {
        closest_center = (uint32_t *)palloc(sizeof(uint32_t) * num_points);
        ret_closest_center = false;
    }

    float *docs_l2sq = (float *)palloc(sizeof(float) * num_points);
    compute_vecs_l2sq(docs_l2sq, data, num_points, dim);

    constexpr float stop_epsilon = 0.00001;
    float residual = FLT_MAX;
    float old_residual = FLT_MAX;
    for (size_t i = 0; i < max_reps; ++i) {
        CHECK_FOR_INTERRUPTS();
        residual = lloyds_iter(data, num_points, dim, centers, num_centers, docs_l2sq, closest_center);
        if (((i != 0) && ((old_residual - residual) / residual) < stop_epsilon) || (residual < FLT_EPSILON)) {
            break;
        }
        old_residual = residual;
    }
    pfree(docs_l2sq);
    if (!ret_closest_center) {
        pfree(closest_center);
    }
    return residual;
}

static float *deduplicate(float *data, size_t num_points, size_t dim, size_t &num_unique)
{
    UnorderedSet<size_t> distinct(num_points);
    uint32 *hash_vals = (uint32 *)palloc(sizeof(uint32) * num_points);
    const auto task1 = [data, dim, hash_vals](size_t start, size_t end) {
        for (size_t i = start; i < end; ++i) {
            hash_vals[i] = tag_hash(data + i * dim, dim * sizeof(float));
        }
    };

    constexpr size_t CHUNK_SIZE = 1024lu;
#if !defined(__aarch64__)
    INIT_TASK_RUNNER();
    START_TASK_POOL();
    for (size_t i = 0; i < num_points; i += CHUNK_SIZE) {
        size_t end = std::min(i + CHUNK_SIZE, num_points);
        RUN_TASK(task1, i, end);
    }
    WAIT_AND_END_TASK_POOL();
#else
    for (size_t i = 0; i < num_points; i += CHUNK_SIZE) {
        size_t end = std::min(i + CHUNK_SIZE, num_points);
        task1(i, end);
    }
#endif
    num_unique = 0;
    float *out = (float *)palloc(sizeof(float) * num_points * dim);
    for (size_t i = 0; i < num_points; ++i) {
        if (distinct.insert(hash_vals[i]).second) {
            for (size_t j = 0; j < dim; ++j) {
                out[num_unique * dim + j] = data[i * dim + j];
            }
            ++num_unique;
        }
    }
    pfree(hash_vals);
    ann_helper::optional_destroy(distinct);
    return (float *)repalloc(out, sizeof(float) * num_unique * dim);
}

/* select randomly num_centers points as pivots */
static void selecting_pivots(float *data, size_t num_points, size_t dim, float *pivot_data, size_t num_centers)
{
    Vector<size_t> picked;
    std::random_device rd;
    auto x = rd();
    std::mt19937 generator(x);
    std::uniform_int_distribution<size_t> distribution(0, num_points - 1);

    size_t tmp_pivot;
    for (size_t j = 0; j < num_centers; ++j) {
        tmp_pivot = distribution(generator);
        if (std::find(picked.cbegin(), picked.cend(), tmp_pivot) != picked.cend()) {
            --j;
            continue;
        }
        picked.push_back(tmp_pivot);
        for (size_t k = 0; k < dim; ++k) {
            pivot_data[j * dim + k] = data[tmp_pivot * dim + k];
        }
    }

    ann_helper::optional_destroy(picked);
}

/* kmeans++ init algorithm, assume num_centers is small, i.e. less than 1k */
void kmeanspp_selecting_pivots(float *data, size_t num_points, size_t dim, float *pivot_data, size_t num_centers)
{
    size_t ndistinct;
    float *distinct_data = deduplicate(data, num_points, dim, ndistinct);
    if (ndistinct > 1 << 23) {
        selecting_pivots(distinct_data, ndistinct, dim, pivot_data, num_centers);
        pfree(distinct_data);
        return;
    }
    if (ndistinct <= num_centers) {
        errno_t rc = memcpy_s(pivot_data, num_centers * dim * sizeof(float),
            distinct_data, ndistinct * dim * sizeof(float));
        securec_check(rc, "\0", "\0");
        for (size_t i = ndistinct; i < num_centers; ++i) {
            for (size_t j = 0; j < dim; ++j) {
                pivot_data[i * dim + j] += distinct_data[j];
            }
        }
        pfree(distinct_data);
        return;
    }
    if (ndistinct < num_centers * 3) {
        errno_t rc = memcpy_s(pivot_data, num_centers * dim * sizeof(float),
            distinct_data, num_centers * dim * sizeof(float));
        securec_check(rc, "\0", "\0");
        pfree(distinct_data);
        return;
    }

    Vector<size_t> picked;
    std::random_device rd;
    auto x = rd();
    std::mt19937 generator(x);
    std::uniform_real_distribution<> distribution(0, 1);
    std::uniform_int_distribution<size_t> int_dist(0, ndistinct - 1);

    size_t init_id = int_dist(generator);
    size_t num_picked = 1;
    picked.push_back(init_id);
    errno_t rc = memcpy_s(pivot_data, num_centers * dim * sizeof(float),
        distinct_data + init_id * dim, dim * sizeof(float));
    securec_check(rc, "\0", "\0");

    auto func = ann_helper::get_general_distance_batch_func(Metric::L2, dim);
    float *dist = (float *)palloc(ndistinct * sizeof(float));
    auto calc_dist_all = [distinct_data, dim, ndistinct, dist, func](size_t pivot) {
        const float *pivot_data = distinct_data + pivot * dim;
        /* TD: parallelize this */
        func(pivot_data, distinct_data, dim, ndistinct, dist);
    };
    calc_dist_all(init_id);

    constexpr size_t max_reloop_count = 10'000;
    size_t reloop_count = 0;
    size_t tmp_pivot;
    while (num_picked < num_centers) {
        double sum = 0;
        for (size_t i = 0; i < ndistinct; ++i) {
            sum += dist[i];
        }
reloop:
        CHECK_FOR_INTERRUPTS();
        double dart_val = distribution(generator) * sum;
        double prefix_sum = 0;
        for (tmp_pivot = 0; tmp_pivot < ndistinct; ++tmp_pivot) {
            if (dart_val >= prefix_sum && dart_val < prefix_sum + dist[tmp_pivot]) {
                break;
            }
            prefix_sum += dist[tmp_pivot];
        }

        if (std::find(picked.cbegin(), picked.cend(), tmp_pivot) != picked.cend()) {
            if (sum == 0) {
                // do {
                //     tmp_pivot = int_dist(generator);
                // } while (std::find(picked.cbegin(), picked.cend(), tmp_pivot) != picked.cend());
            } else if (reloop_count < max_reloop_count) {
                ++reloop_count;
                goto reloop;
            } else {
                selecting_pivots(distinct_data, ndistinct, dim, pivot_data, num_centers);
                ann_helper::optional_destroy(picked);
                pfree(distinct_data);
                pfree(dist);
                return;
            }
        }
        picked.push_back(tmp_pivot);
        for (size_t j = 0; j < dim; ++j) {
            pivot_data[num_picked * dim + j] = distinct_data[tmp_pivot * dim + j];
        }
        ++num_picked;
        calc_dist_all(tmp_pivot);
    }
    ann_helper::optional_destroy(picked);
    pfree(distinct_data);
    pfree(dist);
}
