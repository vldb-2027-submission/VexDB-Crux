/**
 * Copyright ...
 */

#include "access/bm25/tokenizer/cppjieba/dsnowball.h"
#include "utils/palloc.h"
#include "utils/builtins.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "tsearch/ts_cache.h"

static int clear_snowball_stoplist(void *dict_snowball)
{
    StopList &s = *(StopList *)((char *)dict_snowball + sizeof(void *));
    int res = s.len;
    s.len = 0;
    return res;
}

static void recover_snowball_stoplist(void *dict_snowball, int length)
{
    ((StopList *)((char *)dict_snowball + sizeof(void *)))->len = length;
}

static Oid get_snowball_oid()
{
    List *dict_name = stringToQualifiedNameList("english_stem");
    Oid snowball_oid = get_ts_dict_oid(dict_name, false);
    list_free_deep(dict_name);
    return snowball_oid;
}

void *call_dsnowball_lexize(const char *word)
{
    static Oid snowball_dict_id = get_snowball_oid();
    DictSubState dstate = {false, false, NULL};
    TSDictionaryCacheEntry *snowball_entry = lookup_ts_dictionary_cache(snowball_dict_id);
    void *snowball_dict = snowball_entry->dictData;
    int orig_len = clear_snowball_stoplist(snowball_dict);

    text *in = cstring_to_text(word);
    void *res = DatumGetPointer(FunctionCall4(&snowball_entry->lexize,
        PointerGetDatum(snowball_dict),
        PointerGetDatum(VARDATA(in)),
        Int32GetDatum(VARSIZE(in) - VARHDRSZ),
        PointerGetDatum(&dstate)));
    pfree(in);
    recover_snowball_stoplist(snowball_dict, orig_len);
    return res;
}
