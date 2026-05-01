/**
 * Copyright ...
 * BM25 struct holder.
 */

#ifndef BM25_STRUCT_H
#define BM25_STRUCT_H

#include <type_traits>  /* forward */
#include <algorithm>    /* min */

#include "access/bm25/bm25.h"
#include "access/bm25/bm25_statistics.h"
#include "access/bm25/tokenizer/token.h"
#include "storage/item/itemptr.h"
#include "utils/relcache.h"

namespace bm25 {
#if !BM25_STATS
class QueryStats {
public:
    static void record_query() {}
    static void record_load() {}
    static void record_doc() {}
    static void record_qskip() {}
    static void record_skip() {}
    static void record_lskip() {}
    static void report() {}
    static void destroy() {}
};
#else
class QueryStats {
public:
    void record_query() { ++query_count; }
    void record_load() { ++load_count; }
    void record_doc() { ++doc_count; }
    void record_qskip() { ++qskip_count; }
    void record_skip() { ++skip_count; }
    void record_lskip() { ++lskip_count; }
    void report() const
    {
        if (query_count == 0 && load_count == 0 && doc_count == 0 &&
            qskip_count == 0 && skip_count == 0 && lskip_count == 0) {
            return;
        }
        ereport(NOTICE, (errcode(ERRCODE_LOG),
            errmsg("BM25 statistics: query_count %zu, load_count %zu, doc_count %zu, "
                   "qskip_count %zu, skip_count %zu, lskip_count %zu",
                   query_count, load_count, doc_count, qskip_count, skip_count, lskip_count)));
    }
    static void destroy() {}
private:
    size_t query_count{0};
    size_t load_count{0};
    size_t doc_count{0};
    size_t skip_count{0};
    size_t qskip_count{0};
    size_t lskip_count{0};
};
#endif /* BM25_STATS */

template <uint16 nattr>
struct DocInfo {
    ItemPointerData tid;
    DocumentStats stats[nattr];
    static constexpr Oid get_part_id() { return InvalidOid; }
};

template <uint16 nattr>
struct DocOidInfo : public DocInfo<nattr> {
    Oid part_id;
    Oid get_part_id() const { return part_id; }
};

struct InvertedListEntry {
    uint64 doc_id;
    uint16 freq;
    InvertedListEntry() = default;
    InvertedListEntry(uint64 in_doc_id, uint32 in_freq)
        : doc_id(in_doc_id),
          freq(std::min(0xffffu, in_freq)) {}
    InvertedListEntry(const InvertedListEntry &) = default;
    InvertedListEntry(InvertedListEntry &&) = default;
    InvertedListEntry &operator=(const InvertedListEntry &) = default;
    InvertedListEntry &operator=(InvertedListEntry &&) = default;
} __attribute__ ((packed));

template <uint16 N>
struct FixedInvertedList {
    static constexpr uint16 n = N;
    InvertedListEntry entries[N];
    uint8 version;
} __attribute__ ((packed));

struct TokenIndexEntry {
    BlockNumber start_blkno;
    BlockNumber insert_blkno;
    TokenStats stats;
    TokenIndexEntry() = default;
    TokenIndexEntry(Relation rel, const void *il_meta, uint64 doc_id, uint16 freq, bool need_wal);
    TokenIndexEntry &operator=(const TokenIndexEntry &) = default;
};

#pragma pack(push, 2)
struct SparseDimIndexEntry {
    BlockNumber start_blkno;
    BlockNumber insert_blkno;
    SparseDimStats stats;
    SparseDimIndexEntry() = default;
    SparseDimIndexEntry(Relation rel, const void *il_meta, uint64 doc_id, uint16 score, bool need_wal);
    SparseDimIndexEntry &operator=(const SparseDimIndexEntry &) = default;
};
#pragma pack(pop)

constexpr size_t short_word_len = 7ul;
constexpr size_t mid_word_len = 13ul;
constexpr size_t long_word_len = 49ul;

struct ShortToken {
    char tok[short_word_len];
    ShortToken(const char *s);
    operator const char *() const { return tok; }
    operator char *() { return tok; }
};
struct MidToken {
    char tok[mid_word_len];
    MidToken(const char *s);
    operator const char *() const { return tok; }
    operator char *() { return tok; }
};
struct LongToken {
    char tok[long_word_len];
    LongToken(const char *s);
    operator const char *() const { return tok; }
    operator char *() { return tok; }
};
struct FullToken : public BaseObject {
    char tok[bm25_tokenizer::max_token_length];
    FullToken(const char *s);
    operator const char *() const { return tok; }
    operator char *() { return tok; }
};
}; /* namespace bm25 */
#endif /* BM25_STRUCT_H */
