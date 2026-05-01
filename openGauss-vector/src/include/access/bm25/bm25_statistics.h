/**
 * Copyright ...
 * BM25 statistics
 */

#ifndef BM25_STATISTICS_H
#define BM25_STATISTICS_H

#include "c.h"

namespace bm25 {
struct GlobalStats {
    union {
        uint64 total_length;
        double total_norm;
    };
    uint64 total_distinct;
    uint32 total_doc;
};

struct BM25StatisticsPageData {
    uint32 version;
    uint32 magic;
    uint64 max_doc_id;
    GlobalStats stats[FLEXIBLE_ARRAY_MEMBER];
    void init();
};
using BM25StatisticsPage = BM25StatisticsPageData *;
#define GetBM25StatisticsPage(page) ((BM25StatisticsPage)((char *)(page) + SizeOfPageHeaderData))

struct DocumentStats {
    union {
        float length;
        float norm;
    };
};

struct TokenStats {
    uint32 ndoc;
};

#pragma pack(push, 2)
struct SparseDimStats {
    uint32 ndoc;
    uint16 max_score;
};
#pragma pack(pop)
}; /* namespace bm25 */

#endif /* BM25_STATISTICS_H */
