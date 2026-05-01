/**
 * Copyright ...
 * BM25 scorer.
 */

#ifndef BM25_SCORE_H
#define BM25_SCORE_H

#include "access/bm25/bm25_statistics.h"
#include "utils/palloc.h"

namespace bm25 {
enum class ScoreMetric {
    CLASSIC = 0,
    TF_IDF = 1,
    LOG_TF_IDF = 2,
    IP = 3,
    COSINE = 4,
};

struct ScoreCoefficient {
    static constexpr float DEFAULT_B = 0.75f;
    static constexpr float DEFAULT_K = 1.2f;
    float k{DEFAULT_K};
    float b{DEFAULT_B};
};

struct Scorer {
    ScoreMetric metric;
    ScoreCoefficient coef;
};

/* require further research on scoring functions to finalize scoring interface */
float word_score(const Scorer &scorer, const GlobalStats &stat, const TokenStats &tok_stat);
float doc_score(const Scorer &scorer, const GlobalStats &stat, const DocumentStats &doc_stat,
                uint32 freq);

class PostDocScorer : public BaseObject {
public:
    static PostDocScorer *get_doc_scorer(const Scorer &scorer, const GlobalStats &stat);
    virtual ~PostDocScorer() = default;
    virtual float doc_score(const DocumentStats &doc_stat, uint32 freq) const = 0;
};

} /* namespace bm25 */

#endif /* BM25_SCORE_H */
