/**
 * Copyright ...
 */

#include <vtl/hashtable>

#include "access/bm25/tokenizer/tokenizer.h"
#include "access/bm25/bm25.h"
#include "access/bm25/bm25_utils.h"
#include "utils/memutils.h"
#include "tsearch/ts_cache.h"
#include "access/bm25/tokenizer/cppjieba/dsnowball.h"
#include "access/bm25/tokenizer/cppjieba/jieba.h"
#include "utils/resowner.h"
#include "utils/fmgroids.h"
#include "catalog/indexing.h"
#include "catalog/pg_ts_dict.h"
#include "access/bm25/bm25_utils.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"
#include "utils/acl.h"

using namespace bm25;
using namespace bm25_tokenizer;
using namespace cppjieba;

#define TokenizerBuffer ((TokenizerResource *)(g_instance.bm25_cxt.buffer))
constexpr size_t max_using_dict_num = 16ul; /* TD: replace it with GUC */

Oid get_dict_oid(const char *dict_name)
{
    List *dict_name_list = stringToQualifiedNameList(dict_name);
    Oid dict_oid = get_ts_dict_oid(dict_name_list, false);
    list_free_ext(dict_name_list);
    return dict_oid;
}

Oid get_dict_template(const char *dict_name)
{
    Oid dict_id = get_dict_oid(dict_name);
    HeapTuple tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dict_id));
    /* should not happen */
    if (!HeapTupleIsValid(tup)) {
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search dictionary %u", dict_id)));
    }
    bool isNull = false;
    Datum template_id = SysCacheGetAttr(TSDICTOID, tup, Anum_pg_ts_dict_dicttemplate, &isNull);
    Oid res = DatumGetObjectId(template_id);
    ReleaseSysCache(tup);
    return res;
}

text *get_dict_initoption(Oid dict_id)
{
    HeapTuple tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dict_id));
    /* should not happen */
    if (!HeapTupleIsValid(tup)) {
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search dictionary %u", dict_id)));
    }
    bool isNull = false;
    Datum initoption = SysCacheGetAttr(TSDICTOID, tup, Anum_pg_ts_dict_dictinitoption, &isNull);
    text *res = DatumGetTextPCopy(initoption);
    ReleaseSysCache(tup);
    return res;
}

text *get_dict_initoption(const char *dict_name)
{
    Oid dict_id = get_dict_oid(dict_name);
    return get_dict_initoption(dict_id);
}

char *get_initoption_setting(text *initoption, const char *key)
{
    ListCell *l = NULL;
    List *options = deserialize_deflist(PointerGetDatum(initoption));
    char *setting = NULL;
    foreach (l, options) {
        DefElem *defel = (DefElem *)lfirst(l);
        if (pg_strcasecmp(key, defel->defname) == 0) {
            setting = defGetString(defel);
            break;
        }
    }
    return setting;
}

char **get_ts_content(Oid dict_id, const char type, uint32 &total_len)
{
    if (dict_id == default_jieba_dict_id) {
        total_len = 0;
        return NULL;
    }
    Relation content_rel = heap_open(TSContentRelationId, RowExclusiveLock);
    ScanKeyData scanKeys[2];
    ScanKeyInit(&scanKeys[0],
                Anum_pg_ts_content_dictoid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(dict_id));
    ScanKeyInit(&scanKeys[1],
                Anum_pg_ts_content_type,
                BTEqualStrategyNumber,
                F_CHAREQ,
                CharGetDatum(type));

    SysScanDesc scan = systable_beginscan(content_rel, TSContentOidTypeIndexId, true, NULL, 2, scanKeys);
    HeapTuple htup = systable_getnext(scan);
    if (!HeapTupleIsValid(htup)) {
        ereport(ERROR, (errcode(ERRCODE_FETCH_DATA_FAILED),
            errmsg("Failed to load dictionary %u content", dict_id), errdetail("N/A"),
            errcause("Possible snapshot un-sync."), erraction("Please reconnect and try again.")));
    }
    bool isnull = false;
    Datum dtm_content = heap_getattr(htup, Anum_pg_ts_content_content, content_rel->rd_att, &isnull);
    ArrayType *content_array = DatumGetArrayTypeP(dtm_content);
    char **arr_cstring = get_arr_cstring(content_array, TEXTOID, total_len);
    if ((Pointer)content_array != DatumGetPointer(dtm_content)) {
        pfree(content_array);
    }
    systable_endscan(scan);
    heap_close(content_rel, RowExclusiveLock);
    return arr_cstring;
}

char **get_ts_content(const char *dict_name, const char type, uint32 &total_len)
{
    Oid dict_id = get_dict_oid(dict_name);
    return get_ts_content(dict_id, type, total_len);
}

void free_arraycstr(ArrayCStr &a)
{
    if (a.count == 0) {
        return;
    }
    for (int32 i = 0; i < a.count; ++i) {
        pfree(a.values[i]);
    }
    pfree(a.values);
    a.count = 0;
}

static ArrayCStr merge_arraycstr(ArrayCStr &a1, ArrayCStr &a2)
{
    ArrayCStr res;
    UnorderedSet<CharString> tmp_s;
    for (int i = 0; i < a1.count; ++i) {
        tmp_s.emplace(a1.values[i]);
    }
    for (int i = 0; i < a2.count; ++i) {
        tmp_s.emplace(a2.values[i]);
    }
    res.count = tmp_s.size();
    res.values = (char **)palloc(sizeof(char *) * res.count);

    int32 i = 0;
    for (CharString cs : tmp_s) {
        res.values[i++] = pstrdup(cs.c_str());
    }
    ann_helper::optional_destroy(tmp_s);
    return res;
}

static ArrayCStr anti_merge_arraycstr(ArrayCStr &a1, ArrayCStr &a2)
{
    /* a1 - a2 */
    ArrayCStr res;
    UnorderedSet<CharString> tmp_a1;
    UnorderedSet<CharString> tmp_a2;
    for (int i = 0; i < a1.count; ++i) {
        tmp_a1.emplace(a1.values[i]);
    }
    for (int i = 0; i < a2.count; ++i) {
        tmp_a2.emplace(a2.values[i]);
    }
    for (CharString cs : tmp_a2) {
        if (tmp_a1.find(cs) == tmp_a1.end()) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("array does not contain the word \"%s\"", cs.c_str())));
        }
        tmp_a1.erase(cs);
    }
    res.count = tmp_a1.size();
    res.values = (char **)palloc(sizeof(char *) * res.count);
    int32 i = 0;
    for (CharString cs : tmp_a1) {
        res.values[i++] = pstrdup(cs.c_str());
    }
    ann_helper::optional_destroy(tmp_a1);
    ann_helper::optional_destroy(tmp_a2);
    return res;
}

void create_ts_content(const Oid dict_id, const char type, ArrayCStr &input_arr)
{
    check_input(input_arr);

    Relation content_rel = heap_open(TSContentRelationId, RowExclusiveLock);
    Datum ts_content_values[Natts_pg_ts_content];
    bool content_nulls[Natts_pg_ts_content] = {false, false, false};
    ts_content_values[Anum_pg_ts_content_dictoid - 1] = dict_id;
    ts_content_values[Anum_pg_ts_content_type - 1] = type;
    char **words = input_arr.values;
    int32 len = input_arr.count;
    Datum *dtm_words = (Datum *)palloc(sizeof(Datum) * len);
    for (int32 i = 0; i < len; ++i) {
        if (words[i] != NULL) {
            dtm_words[i] = CStringGetTextDatum(words[i]);
        } else {
            dtm_words[i] = CStringGetTextDatum("");
        }
    }
    ArrayType *arr_words = construct_array(dtm_words, len, TEXTOID, -1, false, 'i');
    ts_content_values[Anum_pg_ts_content_content - 1] = PointerGetDatum(arr_words);
    HeapTuple tup_content = heap_form_tuple(content_rel->rd_att, ts_content_values, content_nulls);
    simple_heap_insert(content_rel, tup_content);
    CatalogUpdateIndexes(content_rel, tup_content);
    heap_close(content_rel, RowExclusiveLock);
    pfree(dtm_words);
    pfree(arr_words);
}

void delete_ts_content(const Oid dict_id, const char type)
{
    Relation content_rel = heap_open(TSContentRelationId, RowExclusiveLock);
    ScanKeyData scanKeys[2];
    ScanKeyInit(&scanKeys[0],
                Anum_pg_ts_content_dictoid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(dict_id));
    ScanKeyInit(&scanKeys[1],
                Anum_pg_ts_content_type,
                BTEqualStrategyNumber,
                F_CHAREQ,
                CharGetDatum(type));

    SysScanDesc scan = systable_beginscan(content_rel, TSContentOidTypeIndexId, true, NULL, 2, scanKeys);
    HeapTuple htup = systable_getnext(scan);
    if (HeapTupleIsValid(htup)) {
        simple_heap_delete(content_rel, &htup->t_self);
    }

    systable_endscan(scan);
    heap_close(content_rel, RowExclusiveLock);
}

void clear_ts_content(const Oid dict_id, const char type)
{
    Relation content_rel = heap_open(TSContentRelationId, RowExclusiveLock);
    ScanKeyData scanKeys[2];
    ScanKeyInit(&scanKeys[0],
                Anum_pg_ts_content_dictoid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(dict_id));
    ScanKeyInit(&scanKeys[1],
                Anum_pg_ts_content_type,
                BTEqualStrategyNumber,
                F_CHAREQ,
                CharGetDatum(type));

    SysScanDesc scan = systable_beginscan(content_rel, TSContentOidTypeIndexId, true, NULL, 2, scanKeys);
    HeapTuple htup = systable_getnext(scan);
    if (!HeapTupleIsValid(htup)) {
        ereport(ERROR, (errcode(ERRCODE_FETCH_DATA_FAILED),
            errmsg("Cache lookup failed for dictionary %u", dict_id), errdetail("N/A"),
            errcause("System error."), erraction("Contact engineer to support.")));
    }

    Datum *dtm_words_empty = (Datum *)palloc(0);
    ArrayType *empty_arr_words = construct_array(dtm_words_empty, 0, TEXTOID, -1, false, 'i');
    Datum update_values[Natts_pg_ts_content];
    bool nulls[Natts_pg_ts_content] = {false, false, false};
    bool repl[Natts_pg_ts_content] = {false, false, true};
    update_values[Anum_pg_ts_content_content - 1] = PointerGetDatum(empty_arr_words);
    HeapTuple new_tup = heap_modify_tuple(htup, RelationGetDescr(content_rel), update_values, nulls, repl);
    simple_heap_update(content_rel, &htup->t_self, new_tup);

    systable_endscan(scan);
    heap_close(content_rel, RowExclusiveLock);
}

void update_ts_content(const Oid dict_id, const char type, ArrayCStr &input_arr, bool is_delete)
{
    Relation content_rel = heap_open(TSContentRelationId, RowExclusiveLock);
    ScanKeyData scanKeys[2];
    ScanKeyInit(&scanKeys[0],
                Anum_pg_ts_content_dictoid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(dict_id));
    ScanKeyInit(&scanKeys[1],
                Anum_pg_ts_content_type,
                BTEqualStrategyNumber,
                F_CHAREQ,
                CharGetDatum(type));

    SysScanDesc scan = systable_beginscan(content_rel, TSContentOidTypeIndexId, true, NULL, 2, scanKeys);
    HeapTuple htup = systable_getnext(scan);

    if (!HeapTupleIsValid(htup)) {
        ereport(ERROR, (errcode(ERRCODE_FETCH_DATA_FAILED),
            errmsg("Cache lookup failed for dictionary %u", dict_id), errdetail("N/A"),
            errcause("System error."), erraction("Contact engineer to support.")));
    }
    uint32 content_len = 0;
    bool isnull = false;
    Datum dtm_content = heap_getattr(htup, Anum_pg_ts_content_content, content_rel->rd_att, &isnull);
    ArrayType *content_array = DatumGetArrayTypeP(dtm_content);
    char **content_words = get_arr_cstring(content_array, TEXTOID, content_len);

    ArrayCStr a2;
    a2.count = content_len;
    a2.values = content_words;
    ArrayCStr arr = is_delete ? anti_merge_arraycstr(a2, input_arr) : merge_arraycstr(input_arr, a2);
    char **words = arr.values;
    int32 len = arr.count;
    Datum *dtm_words = (Datum *)palloc(sizeof(Datum) * len);
    for (int32 i = 0; i < len; ++i) {
        if (words[i] != NULL) {
            dtm_words[i] = CStringGetTextDatum(words[i]);
        } else {
            dtm_words[i] = CStringGetTextDatum("");
        }
    }
    ArrayType *arr_words = construct_array(dtm_words, len, TEXTOID, -1, false, 'i');

    Datum update_values[Natts_pg_ts_content];
    bool nulls[Natts_pg_ts_content] = {false, false, false};
    bool repl[Natts_pg_ts_content] = {false, false, true};
    update_values[Anum_pg_ts_content_content - 1] = PointerGetDatum(arr_words);
    HeapTuple new_tup = heap_modify_tuple(htup, RelationGetDescr(content_rel), update_values, nulls, repl);
    simple_heap_update(content_rel, &htup->t_self, new_tup);

    systable_endscan(scan);
    heap_close(content_rel, RowExclusiveLock);
    free_arraycstr(a2);
    free_arraycstr(arr);
    pfree(dtm_words);
    pfree(arr_words);
}

static void ownercheck(Oid dict_id)
{
    if (!pg_ts_dict_ownercheck(dict_id, GetUserId())) {
        HeapTuple tp = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dict_id));
        Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
        char *dict_name = pstrdup(NameStr(reltup->relname));
        ReleaseSysCache(tp);
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY, dict_name);
    }
}

static void defaultdict_check(Oid dict_id)
{
    if (dict_id == default_jieba_dict_id) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("cannot edit default dictionary `cn_tokenizer`")));
    }
}

static void typecheck(Oid dict_id)
{
    text *initoption = get_dict_initoption(dict_id);
    char *setting = get_initoption_setting(initoption, "userdict");
    if (!setting) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("cannot add/delete keyword to dictionary which use delimiter")));
    }
}

Datum vexjieba_add_stopwords(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(0);
    defaultdict_check(dict_id);
    ownercheck(dict_id);
    ArrayType *input_stopwords_arr = PG_GETARG_ARRAYTYPE_P(1);
    uint32 input_len = 0;
    ArrayCStr arr;
    char **input_words = get_arr_cstring(input_stopwords_arr, TEXTOID, input_len);
    arr.count = input_len;
    arr.values = input_words;
    update_ts_content(dict_id, 's', arr);
    free_arraycstr(arr);
    PG_RETURN_BOOL(true);
}

Datum vexjieba_add_userdict(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(0);
    defaultdict_check(dict_id);
    ownercheck(dict_id);
    typecheck(dict_id);
    ArrayType *input_userdict_arr = PG_GETARG_ARRAYTYPE_P(1);
    uint32 input_len = 0;
    ArrayCStr arr;
    char **input_words = get_arr_cstring(input_userdict_arr, TEXTOID, input_len);
    arr.count = input_len;
    arr.values = input_words;
    check_input(arr);
    update_ts_content(dict_id, 'u', arr);
    free_arraycstr(arr);
    PG_RETURN_BOOL(true);
}

Datum vexjieba_delete_stopwords(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(0);
    defaultdict_check(dict_id);
    ownercheck(dict_id);
    clear_ts_content(dict_id, 's');
    PG_RETURN_BOOL(true);
}

Datum vexjieba_delete_stopwords_specified(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(0);
    defaultdict_check(dict_id);
    ownercheck(dict_id);
    ArrayType *delete_stopwords_arr = PG_GETARG_ARRAYTYPE_P(1);
    uint32 input_len = 0;
    ArrayCStr arr;
    char **input_words = get_arr_cstring(delete_stopwords_arr, TEXTOID, input_len);
    if (input_len == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("words to be deleted cannot be empty")));
    }
    arr.count = input_len;
    arr.values = input_words;
    update_ts_content(dict_id, 's', arr, true);
    free_arraycstr(arr);
    PG_RETURN_BOOL(true);
}

Datum vexjieba_delete_userdict(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(0);
    defaultdict_check(dict_id);
    ownercheck(dict_id);
    typecheck(dict_id);
    clear_ts_content(dict_id, 'u');
    PG_RETURN_BOOL(true);
}

Datum vexjieba_delete_userdict_specified(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(0);
    defaultdict_check(dict_id);
    ownercheck(dict_id);
    typecheck(dict_id);
    ArrayType *delete_userdict_arr = PG_GETARG_ARRAYTYPE_P(1);
    uint32 input_len = 0;
    ArrayCStr arr;
    char **input_words = get_arr_cstring(delete_userdict_arr, TEXTOID, input_len);
    if (input_len == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("words to be deleted cannot be empty")));
    }
    arr.count = input_len;
    arr.values = input_words;
    update_ts_content(dict_id, 'u', arr, true);
    free_arraycstr(arr);
    PG_RETURN_BOOL(true);
}

Datum vexjieba_reload(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(0);
    defaultdict_check(dict_id);
    ownercheck(dict_id);

    constexpr uint32 spins_per_delay = 40u;
    constexpr uint32 max_spins = 80u * spins_per_delay;
    constexpr long min_delay_usec = 500l;
    constexpr long max_delay_usec = 100'000l;
    uint32 spins = 0; /* we start with wait */
    long delay = min_delay_usec;
retry:
    LWLockAcquire(BM25DictBufferLock, LW_EXCLUSIVE);
    for (size_t i = 0; i < max_using_dict_num; ++i) {
        TokenizerResource &t = TokenizerBuffer[i];
        if (t.dict_id == dict_id) {
            if (t.threads_num > 0) {
                LWLockRelease(BM25DictBufferLock);
                if (spins > max_spins) {
                    ereport(ERROR, (errmsg("someone else is using %u, reload later", dict_id)));
                }
                if (spins % spins_per_delay == 0) {
                    CHECK_FOR_INTERRUPTS();
                    pg_usleep(delay);
                    delay += (delay * ((double) random() / (double) MAX_RANDOM_VALUE) + 0.5);
                    if (delay > max_delay_usec) {
                        delay = max_delay_usec;
                    }
                }
                SPIN_DELAY();
                ++spins;
                goto retry;
            } else {
                t.reset();
            }
        }
    }
    LWLockRelease(BM25DictBufferLock);
    PG_RETURN_BOOL(true);
}

void bm25_dict_init(void *in_bm25_cxt)
{
    auto *bm25_cxt = (knl_g_bm25_context *)in_bm25_cxt;
    auto old_ctx = MemoryContextSwitchTo(bm25_cxt->dict_ctx);
    bm25_cxt->buffer = palloc(sizeof(TokenizerResource) * max_using_dict_num);
    for (size_t i = 0; i < max_using_dict_num; ++i) {
        new (TokenizerBuffer + i) TokenizerResource;
    }
    MemoryContextSwitchTo(old_ctx);
}

void *bm25_tokenizer::get_dict_resource(Oid dict_id)
{
    LWLockAcquire(BM25DictBufferLock, LW_SHARED);
    void *res = NULL;
    for (size_t i = 0; i < max_using_dict_num; ++i) {
        TokenizerResource &t = TokenizerBuffer[i];
        if (t.dict_id == dict_id) {
            ++t.threads_num;
            ResourceOwnerEnlargeBm25Dicts(t_thrd.utils_cxt.CurrentResourceOwner);
            ResourceOwnerRememberBm25Dict(t_thrd.utils_cxt.CurrentResourceOwner, &t);
            res = t.tokenizer;
            break;
        }
    }
    LWLockRelease(BM25DictBufferLock);
    return res;
}

void bm25_tokenizer::release_dict_resource(void *j)
{
    LWLockAcquire(BM25DictBufferLock, LW_SHARED);
    for (size_t i = 0; i < max_using_dict_num; ++i) {
        TokenizerResource &t = TokenizerBuffer[i];
        if (t.tokenizer == j) {
            Assert(t.threads_num > 0);
            ResourceOwnerForgetBm25Dict(t_thrd.utils_cxt.CurrentResourceOwner, &t);
            --t.threads_num;
            LWLockRelease(BM25DictBufferLock);
            return;
        }
    }
    ereport(PANIC, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Failed to release dictionary cache")));
}

void *bm25_tokenizer::replace_dict(Oid dict_id, void *options)
{
    void *res = NULL;
    size_t free_slot = max_using_dict_num;
    size_t existing_slot = max_using_dict_num;
    int randint = rand() % max_using_dict_num;
    for (size_t idx = randint; idx < randint + max_using_dict_num; ++idx) {
        size_t i = idx % max_using_dict_num;
        /* find free slot or dict_id already exists */
        TokenizerResource &t = TokenizerBuffer[i];
        if (t.dict_id == dict_id) {
            existing_slot = i;
            break;
        }
        if (free_slot == max_using_dict_num && t.threads_num == 0) {
            free_slot = i;
        }
    }
    if (existing_slot < max_using_dict_num) {
        TokenizerResource &t = TokenizerBuffer[existing_slot];
        ++t.threads_num;
        ResourceOwnerEnlargeBm25Dicts(t_thrd.utils_cxt.CurrentResourceOwner);
        ResourceOwnerRememberBm25Dict(t_thrd.utils_cxt.CurrentResourceOwner, &t);
        res = &t;
    } else if (free_slot < max_using_dict_num) {
        TokenizerResource &t = TokenizerBuffer[free_slot];
        /* release old dict(delete ctx) */
        t.reset();

        /* create new dict */
        HeapTuple tp = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dict_id));
        Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);

        t.ctx = AllocSetContextCreate(g_instance.bm25_cxt.dict_ctx,
            NameStr(reltup->relname), ALLOCSET_DEFAULT_SIZES, SHARED_CONTEXT);
        ReleaseSysCache(tp);
        auto old_ctx = MemoryContextSwitchTo(t.ctx);
        bool first_init = false;
        Jieba *j = (Jieba *)DatumGetPointer(DirectFunctionCall3(
                &djieba_init, PointerGetDatum(options), BoolGetDatum(first_init), ObjectIdGetDatum(dict_id)));
        MemoryContextSwitchTo(old_ctx);

        t.init(j, dict_id);
        ResourceOwnerEnlargeBm25Dicts(t_thrd.utils_cxt.CurrentResourceOwner);
        ResourceOwnerRememberBm25Dict(t_thrd.utils_cxt.CurrentResourceOwner, &t);
        res = &t;
    }
    return res;
}

void *get_bm25_dict(Oid dict_id, void *options)
{
    Assert(OidIsValid(dict_id));
    void *res = NULL;
    constexpr uint32 spins_per_delay = 40u;
    constexpr uint32 max_spins = 80u * spins_per_delay;
    constexpr long min_delay_usec = 500l;
    constexpr long max_delay_usec = 100'000l;
    uint32 spins = 0; /* we start with wait */
    long delay = min_delay_usec;
retry:
    void *dict_resource = get_dict_resource(dict_id);
    if (!dict_resource) {
        LWLockAcquire(BM25DictBufferLock, LW_EXCLUSIVE);
        TokenizerResource *t = (TokenizerResource *)replace_dict(dict_id, options);
        if (!t) {
            LWLockRelease(BM25DictBufferLock);
            if (spins > max_spins) {
                ereport(ERROR, (errmsg("wait dict resource too long, exit")));
            }
            if (spins % spins_per_delay == 0) {
                CHECK_FOR_INTERRUPTS();
                pg_usleep(delay);
                delay += (delay * ((double) random() / (double) MAX_RANDOM_VALUE) + 0.5);
                if (delay > max_delay_usec) {
                    delay = max_delay_usec;
                }
            }
            SPIN_DELAY();
            ++spins;
            goto retry;
        }
        res = t->tokenizer;
        LWLockRelease(BM25DictBufferLock);
    } else {
        res = dict_resource;
    }
    return res;
}

void bm25_tokenizer::release_bm25_dict(TokenizerResource *dict)
{
    ResourceOwnerForgetBm25Dict(t_thrd.utils_cxt.CurrentResourceOwner, dict);
    --dict->threads_num;
}

void *bm25_tokenizer::get_jieba(Oid dict_id)
{
    TSDictionaryCacheEntry *entry = lookup_ts_dictionary_cache(dict_id);
    return entry->dictData;
}
