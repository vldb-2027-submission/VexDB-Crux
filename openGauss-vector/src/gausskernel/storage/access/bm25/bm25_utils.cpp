/**
 * Copyright ...
 */

#include "access/bm25/bm25_utils.h"
#include "access/bm25/bm25_internal.h"
#include "catalog/pg_type.h"
#include "access/bm25/tokenizer/tokenizer.h"
#include "access/bm25/tokenizer/cppjieba/full_segment.h"
#include "access/bm25/tokenizer/cppjieba/string_util.h"

using namespace bm25;
using namespace cppjieba;

static void fill_document(Document &res, Vector<char *> &&words)
{
    UnorderedMap<CharString, size_t> tmp_map;
    for (char *word : words) {
        auto res = tmp_map.emplace(word, 1ul);
        if (!res.second) {
            res.first->second += 1ul;
            pfree(word);
        }
    }
    ann_helper::optional_destroy(words);

    res.tok_size = tmp_map.size();
    res.toks = (char **)palloc(res.tok_size * sizeof(char *));
    res.frequencies = (uint32 *)palloc(res.tok_size * sizeof(uint32));
    size_t i = 0;
    for (const auto &word_s : tmp_map) {
        res.toks[i] = word_s.first.str();
        res.frequencies[i] = word_s.second;
        ++i;
    }
    ann_helper::optional_destroy(tmp_map);
}

Document::Document(Datum d, Oid dtype, void *dict_in)
{
    switch (dtype) {
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
        case NVARCHAR2OID:
        case CLOBOID: {
            auto *packed = pg_detoast_datum_packed((struct varlena *)DatumGetPointer(d));
            uint32 slen = VARSIZE_ANY_EXHDR(packed);
            char *str = (char *)palloc(sizeof(char) * (slen + 1u));
            MemCpy(str, VARDATA_ANY(packed), slen);
            if (packed != (struct varlena*)DatumGetPointer(d)) {
                pfree_ext(packed);
            }
            str[slen] = '\0';
            Vector<char *> words = (*(Jieba *)dict_in).cut_mix(str);
            pfree(str);
            fill_document(*this, std::move(words));
        } break;
        case TEXTARRAYOID:
        case VARCHARARRAYOID:
        case BPCHARARRAYOID: {
            ArrayType *arr = DatumGetArrayTypeP(d);
            const Oid etype = dtype == TEXTARRAYOID ? TEXTOID :
                              dtype == VARCHARARRAYOID ? VARCHAROID : BPCHAROID;
            Datum *elems;
            bool *nulls;
            int nelems;
            deconstruct_array(arr, etype, -1, false, 'i', &elems, &nulls, &nelems);
            Vector<char *> words;
            for (int i = 0; i < nelems; ++i) {
                if (nulls[i]) {
                    continue;
                }
                uint32 slen = VARSIZE_ANY_EXHDR(DatumGetPointer(elems[i]));
                char *str = (char *)palloc(sizeof(char) * (slen + 1u));
                MemCpy(str, VARDATA_ANY(DatumGetPointer(elems[i])), slen);
                str[slen] = '\0';
                if (dtype == BPCHARARRAYOID) {
                    rtrim(str);
                }
                words.push_back(str);
            }
            fill_document(*this, std::move(words));
        } break;
        default:
            ereport(ERROR, (errcode(ERRCODE_INVALID_ATTRIBUTE),
                            errmsg("Invalid data type %u for fulltext index", dtype)));
    }
}

char *bm25::get_cstring(Datum d, Oid type)
{
    auto orig_d = DatumGetPointer(d);
    auto plain_d = pg_detoast_datum_packed((varlena *)orig_d);
    uint32 size = VARSIZE_ANY_EXHDR(plain_d);
    char *res = (char *)palloc(sizeof(char) * (size + 1u));
    errno_t rc = memcpy_s(res, sizeof(char) * (size + 1u), VARDATA_ANY(plain_d), sizeof(char) * size);
    securec_check_c(rc, "\0", "\0");
    res[size] = '\0';
    if ((Pointer)plain_d != orig_d) {
        pfree(plain_d);
    }
    return res;
}

const_string_arr bm25::get_arr_cstring(ArrayType *arr, Oid type, uint32 &len)
{
    Assert(type == TEXTOID || type == VARCHAROID || type == BPCHAROID);
    Datum *elems;
    bool *nulls;
    int nelems;
    deconstruct_array(arr, type, -1, false, 'i', &elems, &nulls, &nelems);
    len = 0;
    if (nelems <= 0) {
        return NULL;
    }
    char **res = (char **)palloc(uint32(nelems) * sizeof(const char *));
    for (uint32 i = 0; i < uint32(nelems); ++i) {
        if (nulls[i]) {
            continue;
        }
        res[len] = get_cstring(elems[i], type);
        ++len;
    }
    pfree(elems);
    pfree(nulls);
    return uint32(nelems) == len ? res : (const_string_arr)repalloc(res, len * sizeof(const char *));
}

void bm25::free_string_arr(const_string_arr arr, uint32 len)
{
    for (uint32 i = 0; i < len; ++i) {
        pfree_ext(arr[i]);
    }
    pfree_ext(arr);
}

bool bm25::bm25_match(const char *, const char *)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("BM25 matching only applies to BM25 index scan")));
    return false;
}

bool bm25::bm25_match_arr(const_string_arr, uint32, const char *)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("BM25 matching only applies to BM25 index scan")));
    return false;
}

bool bm25::bm25_rank_match(const char *, const char *)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("BM25 rank matching only applies to BM25 index scan")));
    return false;
}

bool bm25::bm25_rank_match_arr(const_string_arr, uint32, const_string_arr, uint32)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("BM25 rank matching only applies to BM25 index scan")));
    return false;
}

uint32 bm25::get_bm25_parallel_workers(Relation index)
{
    BM25Options *opts = (BM25Options *)index->rd_options;
    return opts != NULL ? uint32(opts->parallel_workers) : 0;
}

const char *bm25::get_bm25_dictionaries(Relation index)
{
    BM25Options *opts = (BM25Options *)index->rd_options;
    return opts != NULL && opts->dicts_offset > 0 ?
        (const char *)opts + opts->dicts_offset : NULL;
}

const char *bm25::get_bm25_coefficients(Relation index)
{
    BM25Options *opts = (BM25Options *)index->rd_options;
    return opts != NULL && opts->coefficients_offset > 0 ?
        (const char *)opts + opts->coefficients_offset : NULL;
}

const char *bm25::get_bm25_algorithms(Relation index)
{
    BM25Options *opts = (BM25Options *)index->rd_options;
    return opts != NULL && opts->algorithms_offset > 0 ?
        (const char *)opts + opts->algorithms_offset : NULL;
}
