#ifndef EUMMD_H
#define EUMMD_H

#include <random>
#include <algorithm>
#include <cmath>
#include <cstddef>

#include <vtl/vector>

#define L2_SQUARED_DIST g_instance.annvec_cxt.l2_squared_distance
#define NEGATIVE_INNER_PRODUCT_DIST g_instance.annvec_cxt.negative_inner_product

struct MMDResult {
    float mmd_value;
    float p_value;
    float bandwidth;
    bool is_different;
    float effect_size;
};

float compute_mmd(float* X, float* Y, size_t n, size_t dim, float bandwidth) {
    float *x_norms = (float *)palloc0(n * sizeof(float));
    float *y_norms = (float *)palloc0(n * sizeof(float));

    for (size_t i = 0; i < n; ++i) {
        x_norms[i] = -NEGATIVE_INNER_PRODUCT_DIST(X + i * dim, X + i * dim, dim);
        y_norms[i] = -NEGATIVE_INNER_PRODUCT_DIST(Y + i * dim, Y + i * dim, dim);
    }
    
    float kxx_sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j) { 
                continue;
            }
            float dot = -NEGATIVE_INNER_PRODUCT_DIST(X + i * dim, X + j * dim, dim);
            float kern_val = dot - 0.5f * x_norms[i] - 0.5f * x_norms[j];
            kxx_sum += std::exp(bandwidth * kern_val);
        }
    }
    float kxx = (n > 1) ? kxx_sum / (n * (n - 1)) : 0.0f;
    
    float kyy_sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j) { 
                continue;
            }
            float dot = -NEGATIVE_INNER_PRODUCT_DIST(Y + i * dim, Y + j * dim, dim);
            float kern_val = dot - 0.5f * y_norms[i] - 0.5f * y_norms[j];
            kyy_sum += std::exp(bandwidth * kern_val);
        }
    }
    float kyy = (n > 1) ? kyy_sum / (n * (n - 1)) : 0.0f;

    float kxy_sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            float dot = -NEGATIVE_INNER_PRODUCT_DIST(X + i * dim, Y + j * dim, dim);
            float kern_val = dot - 0.5f * x_norms[i] - 0.5f * y_norms[j];
            kxy_sum += std::exp(bandwidth * kern_val);
        }
    }
    float kxy = kxy_sum / (n * n);
    
    return kxx + kyy - 2 * kxy;
}

float choose_bandwidth(float *data, size_t n_samples, size_t dim) {
    constexpr size_t MAX_PAIRS = 1000;
    
    if (n_samples < 2) {
        return 1.0f;
    }
    
    size_t max_pairs = (MAX_PAIRS < n_samples * (n_samples - 1) / 2) ? 
                       MAX_PAIRS : n_samples * (n_samples - 1) / 2;
    
    Vector<float> distances;
    distances.reserve(max_pairs);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, n_samples - 1);
    
    for (size_t i = 0; i < max_pairs; ++i) {
        size_t idx1 = dist(gen);
        size_t idx2 = dist(gen);
        if (idx1 == idx2) {
            continue;
        }
        
        float dist_sq = L2_SQUARED_DIST(data + idx1 * dim, data + idx2 * dim, dim);
        if (dist_sq > 0) {
            distances.push_back(std::sqrt(dist_sq));
        }
    }
    
    if (distances.empty()) {
        return 1.0f;
    }
    
    std::sort(distances.begin(), distances.end());
    float median = distances[distances.size() / 2];
    
    return 1.0f / (median * median);
}

MMDResult mmd_test(float *X, float *Y, size_t n, size_t dim, 
                   float bandwidth = -1.0f, int permutations = 100, 
                   float alpha = 0.05f) {
    MMDResult result = {0.0f, 1.0f, 1.0f, false, 0.0f};    
    size_t total = 2 * n;
    
    float* all_data = (float *)palloc(sizeof(float) * total * dim);
    
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < dim; ++j) {
            all_data[i * dim + j] = X[i * dim + j];
        }
    }
    
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < dim; ++j) {
            all_data[(n + i) * dim + j] = Y[i * dim + j];
        }
    }
    
    if (bandwidth <= 0) {
        result.bandwidth = choose_bandwidth(all_data, total, dim);
    } else {
        result.bandwidth = bandwidth;
    }
    
    result.mmd_value = compute_mmd(X, Y, n, dim, result.bandwidth);
    
    Vector<size_t> indices(total, 0);
    for (size_t i = 0; i < total; ++i) {
        indices[i] = i;
    }
    
    float *X_perm = (float *)palloc(sizeof(float) * n * dim);
    float *Y_perm = (float *)palloc(sizeof(float) * n * dim);
    
    std::mt19937 rng;
    std::random_device rd;
    rng.seed(rd());
    
    int count_better = 1;
    
    for (int i = 0; i < permutations; ++i) {
        std::shuffle(indices.begin(), indices.end(), rng);
        for (size_t j = 0; j < n; ++j) {
            size_t src_idx = indices[j];
            float *src_ptr = all_data + src_idx * dim;
            float *dst_ptr = X_perm + j * dim;
            for (size_t k = 0; k < dim; ++k) {
                dst_ptr[k] = src_ptr[k];
            }
        }
        for (size_t j = 0; j < n; ++j) {
            size_t src_idx = indices[n + j];
            float *src_ptr = all_data + src_idx * dim;
            float *dst_ptr = Y_perm + j * dim;
            for (size_t k = 0; k < dim; ++k) {
                dst_ptr[k] = src_ptr[k];
            }
        }
        float perm_mmd = compute_mmd(X_perm, Y_perm, n, dim, result.bandwidth);        
        if (std::abs(perm_mmd) >= std::abs(result.mmd_value)) {
            ++count_better;
        }
    }
    
    result.p_value = static_cast<float>(count_better) / (permutations + 1);
    result.is_different = result.p_value < alpha;
    result.effect_size = result.mmd_value;
    
    pfree(all_data);
    pfree(X_perm);
    pfree(Y_perm);
    
    return result;
}

#endif /* EUMMD_H */
