/**
 * Copyright ...
 * BM25 index utility.
 */

#ifndef BM25_UTILS_H
#define BM25_UTILS_H

#include <vtl/vector>
#include <vtl/hashtable>

#include "c.h"
#include "utils/array.h"
#include "utils/relcache.h"
#include "access/attnum.h"
#include "storage/item/itemptr.h"
#include "access/annvector/sparsevector.h"

namespace bm25 {
using const_string_arr = char **;
struct Token {
    AttrNumber attrno;
    char *tok;
};
struct SparseElement {
    AttrNumber attrno;
    uint32 dim;
    uint16 v;
};

struct Document {
    uint32 tok_size;
    uint32 *frequencies;
    char **toks;

    Document(Datum d, Oid dtype, void *dict);
    void destroy()
    {
        pfree_ext(frequencies);
        if (toks) {
            for (uint32 i = 0; i < tok_size; ++i) {
                if (toks[i]) {
                    pfree(toks[i]);
                }
            }
            pfree(toks);
            toks = NULL;
        }
        tok_size = 0;
    }
};
struct Documents {
    struct ColumnData {
        AttrNumber attrno;
        union {
            Document doc;
            SparseVector *vec;  /* borrowed outside */
        };

        ColumnData(AttrNumber attrno, Datum d, Oid dtype, void *dict)
            : attrno(attrno),
              doc(d, dtype, dict) {}
        ColumnData(AttrNumber attrno, SparseVector *vec)
            : attrno(attrno),
              vec(vec) {}
    };
    ItemPointerData tid;
    Vector<ColumnData> docs;
    Oid part_id;
    Documents(const ItemPointerData &tid) : tid(tid) {}
    void destroy() { ann_helper::optional_destroy(docs); }
};
struct SparseVecs {
    struct SparseVec { AttrNumber attrno; SparseVector *vec; };
    ItemPointerData tid;
    Vector<SparseVec> vecs;
    Oid part_id;
    SparseVecs(const ItemPointerData &tid) : tid(tid) {}
    void destroy() { ann_helper::optional_destroy(vecs); }
};

typedef UnorderedSet<uint64, impl::DefaultHasher<uint64>,
                     std::equal_to<uint64>, CONTEXT_ALLOCATOR<uint64>> doc_id_track;

char *get_cstring(Datum d, Oid type);
const_string_arr get_arr_cstring(ArrayType *arr, Oid type, uint32 &len);
void free_string_arr(const_string_arr arr, uint32 len);
bool bm25_match(const char *, const char *);
bool bm25_rank_match(const char *, const char *);
bool bm25_match_arr(const_string_arr, uint32, const char *);
bool bm25_rank_match_arr(const_string_arr, uint32, const_string_arr, uint32);

uint32 get_bm25_parallel_workers(Relation index);
const char *get_bm25_dictionaries(Relation index);
const char *get_bm25_algorithms(Relation index);
const char *get_bm25_coefficients(Relation index);
}; /* namespace bm25 */

#endif /* BM25_UTILS_H */
