/**
 * Copyright ...
 */

#ifndef BM25_TOKENIZER_TOKEN_H
#define BM25_TOKENIZER_TOKEN_H

#include <string.h>

#include "c.h"

namespace bm25_tokenizer {
constexpr size_t max_token_length = 4000ul; /* less than 4096 to make sure at least 2 item per page */
constexpr double default_token_weight = 3.0;

inline int cmp_token(const char *a, const char *b) { return strcmp(a, b); }
inline uint32 hash_token(const char *str)
{
    /* One-byte-at-a-time hash based on Murmur's mix
     * Source: https://github.com/aappleby/smhasher/blob/master/src/Hashes.cpp */
    constexpr uint32 hseed = 0x12345678u;
    uint32 h = hseed;
    for (; *str; ++str) {
        h ^= *str;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
    }
    return h;
}
} /* namespace bm25_tokenizer */

#endif /* BM25_TOKENIZER_TOKEN_H */
