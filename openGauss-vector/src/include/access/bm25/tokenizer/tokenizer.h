/**
 * Copyright ...
 */

#ifndef BM25_TOKENIZER_TOKENIZER_H
#define BM25_TOKENIZER_TOKENIZER_H

#include <atomic>

#include "c.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "fmgr/fmgr_comp.h"

extern Datum vexjieba_add_stopwords(PG_FUNCTION_ARGS);
extern Datum vexjieba_add_userdict(PG_FUNCTION_ARGS);
extern Datum vexjieba_delete_stopwords(PG_FUNCTION_ARGS);
extern Datum vexjieba_delete_stopwords_specified(PG_FUNCTION_ARGS);
extern Datum vexjieba_delete_userdict(PG_FUNCTION_ARGS);
extern Datum vexjieba_delete_userdict_specified(PG_FUNCTION_ARGS);
extern Datum vexjieba_reload(PG_FUNCTION_ARGS);

extern Oid get_dict_template(const char *dict_name);
extern text *get_dict_initoption(const char *dict_name);
extern text *get_dict_initoption(Oid dict_id);
extern char *get_initoption_setting(text *initoption, const char *key);
extern void free_arraycstr(ArrayCStr &a);

extern char **get_ts_content(const char *dict_name, const char type, uint32 &total_len);
extern char **get_ts_content(const Oid dict_id, const char type, uint32 &total_len);
extern void create_ts_content(const Oid dict_id, const char type, ArrayCStr &arr);
extern void delete_ts_content(const Oid dict_id, const char type);
extern void update_ts_content(const Oid dict_id, const char type, ArrayCStr &arr, bool is_delete = false);
extern Oid get_dict_oid(const char *dict_name);

struct TokenizerResource {
    void *tokenizer{NULL};
    Oid dict_id{InvalidOid};
    std::atomic<size_t> threads_num{0};
    MemoryContext ctx{NULL};

    void reset()
    {
        tokenizer = NULL;
        dict_id = InvalidOid;
        threads_num = 0;
        if (ctx) {
            MemoryContextDelete(ctx);
            ctx = NULL;
        }
    }
    void init(void *j, Oid d)
    {
        Assert(threads_num == 0);
        tokenizer = j;
        dict_id = d;
        threads_num = 1ul;
    }
};

namespace bm25_tokenizer {
constexpr Oid default_jieba_dict_id = 3885u;

void *get_dict_resource(Oid dict_id);
void release_dict_resource(void *j);
void *replace_dict(Oid dict_id, void *options);
void release_bm25_dict(TokenizerResource *dict);
void *get_jieba(Oid dict_id = default_jieba_dict_id);
} /* namespace bm25_tokenizer */
#endif /* BM25_TOKENIZER_TOKENIZER_H */
