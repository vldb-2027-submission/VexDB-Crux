/**
 * Copyright ...
 */

#include <cmath>
#include <vtl/definition>
#include "access/bm25/bm25_score.h"
#include "access/annvector/halfutils.h"

using namespace bm25;

float bm25::word_score(const Scorer &scorer, const GlobalStats &stat, const TokenStats &tok_stat)
{
    if (scorer.metric != ScoreMetric::CLASSIC) {
        if (scorer.metric == ScoreMetric::IP || scorer.metric == ScoreMetric::COSINE) {
            return 1;
        }
        return std::log(double(stat.total_doc) / tok_stat.ndoc + 1.0);
    }
    double s = 0.5 + stat.total_doc - tok_stat.ndoc;
    s /= tok_stat.ndoc + 0.5;
    return std::log(s + 1.0);
}

float bm25::doc_score(const Scorer &scorer, const GlobalStats &stat, const DocumentStats &doc_stat,
                      uint32 freq)
{
    switch (scorer.metric) {
        case ScoreMetric::CLASSIC:
            return (freq * (scorer.coef.k + 1.0f)) /
                (freq + scorer.coef.k * (1.0f - scorer.coef.b +
                    (scorer.coef.b * doc_stat.length * stat.total_doc) / stat.total_length));
        case ScoreMetric::TF_IDF:
            return freq;
        case ScoreMetric::LOG_TF_IDF:
            return std::log(freq) + 1.0;
        case ScoreMetric::IP:
        case ScoreMetric::COSINE:
            return half_to_float_unsigned(freq);
    }
    return 0.0; /* keep compiler quiet */
}

class PostDocScorerClassic : public PostDocScorer {
public:
    PostDocScorerClassic(const Scorer &scorer, const GlobalStats &stat)
        : k(scorer.coef.k), bm1(1.0f - scorer.coef.b),
          avg_dl_c(scorer.coef.b * float(stat.total_doc) / float(stat.total_length)) {}

    float doc_score(const DocumentStats &doc_stat, uint32 freq) const override
    {
        return (freq * (k + 1.0f)) /
            (freq + k * (bm1 + doc_stat.length * avg_dl_c));
    }
private:
    const float k;
    const float bm1;
    const float avg_dl_c;
};

class PostDocScorerTFIDF : public PostDocScorer {
public:
    PostDocScorerTFIDF() = default;

    float doc_score(const DocumentStats &doc_stat, uint32 freq) const override
    {
        return float(freq);
    }
};

class PostDocScorerLogTFIDF : public PostDocScorer {
public:
    PostDocScorerLogTFIDF() = default;

    float doc_score(const DocumentStats &doc_stat, uint32 freq) const override
    {
        return std::log(float(freq)) + 1.0f;
    }
};

class PostDocScorerSparseBase : public PostDocScorer {
public:
    PostDocScorerSparseBase() = default;

    float doc_score(const DocumentStats &doc_stat, uint32 freq) const override
        { return half_to_float_unsigned(freq); }
};

PostDocScorer *PostDocScorer::get_doc_scorer(const Scorer &scorer, const GlobalStats &stat)
{
    switch (scorer.metric) {
        case ScoreMetric::CLASSIC:
            return NEW PostDocScorerClassic(scorer, stat);
        case ScoreMetric::TF_IDF:
            return NEW PostDocScorerTFIDF;
        case ScoreMetric::LOG_TF_IDF:
            return NEW PostDocScorerLogTFIDF;
        case ScoreMetric::IP:
        case ScoreMetric::COSINE:
            return NEW PostDocScorerSparseBase;
    }
    return NULL; /* keep compiler quiet */
}
