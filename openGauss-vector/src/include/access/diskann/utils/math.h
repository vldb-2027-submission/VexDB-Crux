/**
 * Copyright ...
 * Math utilities.
 */

#ifndef DISKANN_UTILS_MATH_H
#define DISKANN_UTILS_MATH_H

#include <numeric>
#include <math.h>

namespace ann_helper {
struct ChiSquare {
    static double pvalue(double x, size_t df)
    {
        if (x < 0 || df < 1) {
            return 0.0;
        }
        double K = ((double)df) * 0.5;
        x *= 0.5;
        if (df == 2) {
            return exp(-1.0 * x);
        }

        double res = igf(K, x);
        if (isnan(res) || isinf(res) || res <= EPS_THRESHOLD) {
            return EPS;
        }
        return 1.0 - res / approx_gamma(K);
    }
private:
    constexpr static double EPS = 1e-14;
    constexpr static double EPS_THRESHOLD = 1e-8;

    static double igf(double s, double z)
    {
        if (z < 0.0) {
            return 0.0;
        }
        const double sc = (1.0 / s) * pow(z, s) * exp(-z);
        double sum = 1.0;
        double nom = 1.0;
        double denom = 1.0;
        constexpr size_t MAX_ITER = 100ul;
        for(size_t i = 0; i < MAX_ITER; ++i) {
            nom *= z;
            ++s;
            denom *= s;
            const double term = nom / denom;
            if (term <= EPS) {
                break;
            }
            sum += (nom / denom);
        }
        return sum * sc;
    }
    static inline double approx_gamma(double z)
    {
        constexpr double inv_e = 0.36787944117144232159552377016147;
        constexpr double two_pi = 6.283185307179586476925286766559;
        double d = 1.0 / (10.0 * z);
        d = 1.0 / ((12 * z) - d);
        d = (d + z) * inv_e;
        d = pow(d, z);
        return d * sqrt(two_pi / z);
    }
};

struct KolmogorovSmirnov {
    template <typename T>
    static double cat_stats(const T *data, size_t ncat)
    {
        double sum = static_cast<double>(std::accumulate(data, data + ncat, 0));
        T cur = 0;
        double res = 0;
        for (size_t i = 1; i < ncat; ++i) {
            cur += data[i - 1];
            const double diff = std::abs(cur / sum - double(i) / ncat);
            if (diff > res) {
                res = diff;
            }
        }
        return res;
    }

    static double pvalue(double d, size_t n)
    {
        const double z = d * std::sqrt(n);
        const double z2 = z * z;

        double p = 0.5 - std::exp(-2 * z2);
        if (isnan(p) || p < 1e-10) {
            return 0.0;
        }
        constexpr size_t max_iter = 100;
        constexpr double EPS = 1e-14;
        for (size_t i = 2; i < max_iter; i += 2) {
            const auto step = std::exp(-2 * i * i * z2) - std::exp(-2 * (i + 1) * (i + 1) * z2);
            if (isnan(step) || step < EPS) {
                break;
            }
            p += step;
        }
        return p * 2;
    }
};
} /* namespace ann_helper */
#endif /* DISKANN_UTILS_MATH_H */
