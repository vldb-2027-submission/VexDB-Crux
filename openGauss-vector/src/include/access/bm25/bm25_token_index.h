/**
 * Copyright ...
 * BM25 token index.
 * Require c++14 or greater for advanced lambda usage.
 */

#ifndef BM25_TOKEN_INDEX_H
#define BM25_TOKEN_INDEX_H

#include <vtl/disk_container/disk_hashtable.hpp>

#include "access/bm25/bm25.h"
#include "access/bm25/bm25_struct.h"
#include "access/bm25/bm25_utils.h"
#include "access/bm25/tokenizer/token.h"
#include "access/annvector/module/leak_checker.h"
#include "access/genam.h"
#include "access/hash.h"
#include "storage/buf/block.h"
#include "storage/buf/buf.h"
#include "storage/buf/bufmgr.h"

namespace bm25 {
struct TokenIndexMeta {
    struct {
        BlockNumber short_store_blkno;
        BlockNumber mid_store_blkno;
        BlockNumber long_store_blkno;
        BlockNumber full_store_blkno;
    } attr_meta[BM25_MAX_NATTR];
};

template <class TokenType>
struct TokenHasher {
    uint32 operator()(const TokenType &s) const noexcept { return bm25_tokenizer::hash_token(s); }
};
struct DimHasher {
    uint32 operator()(const uint32 &s) const noexcept { return DatumGetUInt32(hash_uint32(s)); }
};
template <class TokenType>
struct TokenComparator {
    bool operator()(const TokenType &s1, const TokenType &s2) const noexcept
        { return bm25_tokenizer::cmp_token(s1, s2) == 0; }
};

class TokenIndex : public ann_helper::LeakChecker {
public:
    using short_store = disk_container::DiskHashTable<ShortToken, TokenIndexEntry,
        TokenHasher<ShortToken>, TokenComparator<ShortToken>, 128ul>;
    using mid_store = disk_container::DiskHashTable<MidToken, TokenIndexEntry,
        TokenHasher<MidToken>, TokenComparator<MidToken>, 64ul>;
    using long_store = disk_container::DiskHashTable<LongToken, TokenIndexEntry,
        TokenHasher<LongToken>, TokenComparator<LongToken>, 32ul>;
    using full_store = disk_container::DiskHashTable<FullToken, TokenIndexEntry,
        TokenHasher<FullToken>, TokenComparator<FullToken>, 16ul>;
    using dim_store = disk_container::DiskHashTable<uint32, SparseDimIndexEntry,
        DimHasher, std::equal_to<uint32>, 128ul>;

    static void create_token_index(Relation rel, uint32 sparse_bits, TokenIndexMeta &meta, bool need_wal);
    TokenIndex(Relation rel, TokenIndexMeta &meta, Buffer buf, bool need_wal)
        : _rel(rel), _meta(meta), _buf(buf), _need_wal(need_wal && RelationNeedsWAL(rel)) {}
    void destroy() { ann_helper::LeakChecker::destroy(); }

    bool get(const Token &token, TokenIndexEntry &out);
    bool get(const SparseElement &sd, SparseDimIndexEntry &out);
    void insert(const Token &token, uint64 doc_id, uint32 freq, const void *il_meta);
    void insert(const SparseElement &sd, uint64 doc_id, const void *il_meta);
    void vacuum(doc_id_track &id_track, IndexBulkDeleteResult *stats, const void *il_meta);
    void verify(const void *meta);

    Relation get_relation() { return _rel; }
    bool need_wal() const { return _need_wal; }

private:
    Relation _rel;
    TokenIndexMeta &_meta;
    Buffer _buf;
    bool _need_wal;

    template <typename F> bool helper(const Token &token, F &&f)
    {
        bool res = false;
        const size_t len = strlen(token.tok) + 1ul;
        if (len <= short_word_len) {
            short_store store(_rel, _meta.attr_meta[token.attrno - 1].short_store_blkno, _need_wal);
            ShortToken tok(token.tok);
            res = std::forward<F>(f)(tok, store);
            store.destroy();
        } else if (len <= mid_word_len) {
            mid_store store(_rel, _meta.attr_meta[token.attrno - 1].mid_store_blkno, _need_wal);
            MidToken tok(token.tok);
            res = std::forward<F>(f)(tok, store);
            store.destroy();
        } else if (len <= long_word_len) {
            long_store store(_rel, _meta.attr_meta[token.attrno - 1].long_store_blkno, _need_wal);
            LongToken tok(token.tok);
            res = std::forward<F>(f)(tok, store);
            store.destroy();
        } else {
            full_store store(_rel, _meta.attr_meta[token.attrno - 1].full_store_blkno, _need_wal);
            FullToken *tok = NEW FullToken(token.tok);
            res = std::forward<F>(f)(*tok, store);
            store.destroy();
            pfree(tok);
        }
        return res;
    }

    template <typename F> bool helper2(const SparseElement &sd, F &&f)
    {
        const BlockNumber store_blkno = sd.v > 0 ?
            _meta.attr_meta[sd.attrno - 1].short_store_blkno :
            _meta.attr_meta[sd.attrno - 1].mid_store_blkno;
        dim_store store(_rel, store_blkno, _need_wal);
        bool res = std::forward<F>(f)(sd.dim, store);
        store.destroy();
        return res;
    }
};
}; /* namespace bm25 */

#endif /* BM25_TOKEN_INDEX_H */
