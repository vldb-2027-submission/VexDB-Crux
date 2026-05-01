/**
 * Copyright ...
 * BM25 index parameters.
 */

#ifndef BM25_PARAMETERS_H
#define BM25_PARAMETERS_H

#include "access/bm25/bm25_score.h"

namespace bm25 {
constexpr const char *metric_str(ScoreMetric metric);
constexpr size_t metric_strlen(ScoreMetric metric);
void extract_scorer(const char *alg_val, const char *coef_val, Scorer *scorers, uint32 nscorer,
                    Relation index);
void extract_dict(const char *alg_val, Oid *dict_ids, uint32 ndict);
} /* namespace bm25 */

extern void validate_dicts(const char *value);
extern void validate_algorithms(const char *value);
extern void validate_coefficients(const char *value);

#endif /* BM25_PARAMETERS_H */
