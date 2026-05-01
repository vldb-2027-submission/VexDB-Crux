/**
 * Copyright ...
 */

#include <vtl/hashtable>
#include <vtl/vector>

#include "postgres.h"
#include "knl/knl_variable.h"
#include "commands/defrem.h"
#include "utils/builtins.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_public.h"
#include "catalog/namespace.h"
#include "access/bm25/tokenizer/cppjieba/jieba.h"
#include "access/bm25/tokenizer/cppjieba/dict_trie.h"
#include "access/bm25/tokenizer/cppjieba/string_util.h"
#include "knl/knl_instance.h"
#include "access/bm25/tokenizer/cppjieba/dsnowball.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_template.h"
#include "access/bm25/bm25_utils.h"
#include "access/bm25/tokenizer/tokenizer.h"

using namespace cppjieba;
using namespace bm25;

#define DJIEBA_ARGSNUM 4
#define DJIEBA_STOPWORDS 0
#define DJIEBA_USERDICT 1
#define DJIEBA_SENSITIVE 2
#define DJIEBA_DELIMITER 3

Datum djieba_init(PG_FUNCTION_ARGS)
{
    List *dictoptions = (List *)PG_GETARG_POINTER(0);
    bool first_init = PG_GETARG_BOOL(1);
    Oid dict_id = PG_GETARG_OID(2);
    ListCell *l = NULL;
    Jieba *j = (Jieba *)palloc(sizeof(Jieba));

    const char *default_dict_path = 
        get_tsearch_config_filename("jieba_dict", NULL, FILE_POSTFIX_JIEBA, false);
    const char *default_hmm_path = 
        get_tsearch_config_filename("hmm_model", NULL, FILE_POSTFIX_JIEBA, false);
    const char *default_stopwords_path = 
        get_tsearch_config_filename("jieba_stopwords", NULL, FILE_POSTFIX_STOP, false);

    /* 4 params in dictoptions */
    const char *args[DJIEBA_ARGSNUM] = {"default", "default", "true", NULL};
    bool argsloaded[DJIEBA_ARGSNUM] = {false, false, false, false};
    const char *paths[DJIEBA_ARGSNUM] = {NULL, NULL, NULL, NULL};

    TemplContentInfos *info = (TemplContentInfos *)palloc(sizeof(TemplContentInfos));
    info->delimiter = NULL;
    info->sensitive = true;
    info->case_sensitive = "true";
    info->templ_stopwords_setting = "default";
    info->templ_userdict_setting = "default";
    char **templ_stopwords = NULL;
    uint32 templ_stopwords_len = 0;
    char **templ_userdict = NULL;
    uint32 templ_userdict_len = 0;

    foreach (l, dictoptions) {
        DefElem *defel = (DefElem*)lfirst(l);
        if (pg_strcasecmp("StopWords", defel->defname) == 0) {
            if (argsloaded[DJIEBA_STOPWORDS]){
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), 
                                errmsg("multiple StopWords parameters")));
            }
            args[DJIEBA_STOPWORDS] = defGetString(defel);
            argsloaded[DJIEBA_STOPWORDS] = true;

            if (pg_strcasecmp("default", args[DJIEBA_STOPWORDS]) == 0) {
                paths[DJIEBA_STOPWORDS] = default_stopwords_path;
                info->templ_stopwords_setting = "default";
            } else if (pg_strcasecmp("empty", args[DJIEBA_STOPWORDS]) == 0) {
                info->templ_stopwords_setting = "empty";
            } else {
                const char *templ_dict_name = args[DJIEBA_STOPWORDS];
                Oid template_id = get_dict_template(templ_dict_name);
                if (template_id != TSTemplateJiebaId) {
                    ereport(ERROR,
                        (errmsg("cannot create vex_jieba dictionary with stopwords template \"%s\"," 
                                " which is not a vex_jieba dictionary", templ_dict_name)));
                }
                text *initoption = get_dict_initoption(templ_dict_name);
                char *templ_setting = get_initoption_setting(initoption, "stopwords");
                info->templ_stopwords_setting = templ_setting;
                if (pg_strcasecmp("default", templ_setting) == 0) {
                    paths[DJIEBA_STOPWORDS] = default_stopwords_path;
                }
                templ_stopwords = get_ts_content(templ_dict_name, 's', templ_stopwords_len);
            }
        } else if (pg_strcasecmp("UserDict", defel->defname) == 0) {
            if (argsloaded[DJIEBA_USERDICT]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("multiple UserDict parameters")));
            }
            if (argsloaded[DJIEBA_DELIMITER]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("Only one of UserDict or Delimiter can be used")));
            }
            args[DJIEBA_USERDICT] = defGetString(defel);
            argsloaded[DJIEBA_USERDICT] = true;

            if (pg_strcasecmp("default", args[DJIEBA_USERDICT]) == 0) {
                paths[DJIEBA_USERDICT] = default_dict_path;
                info->templ_userdict_setting = "default";
            } else if (pg_strcasecmp("empty", args[DJIEBA_USERDICT]) == 0) {
                info->templ_userdict_setting = "empty";
            } else {
                const char *templ_dict_name = args[DJIEBA_USERDICT];
                text *initoption = get_dict_initoption(templ_dict_name);
                Oid template_id = get_dict_template(templ_dict_name);
                if (template_id != TSTemplateJiebaId) {
                    ereport(ERROR,
                        (errmsg("cannot create vex_jieba dictionary with userdict template \"%s\"," 
                                " which is not a vex_jieba dictionary", templ_dict_name)));
                }
                char *templ_setting = get_initoption_setting(initoption, "userdict");
                if (templ_setting == NULL) {
                    ereport(ERROR,
                        (errmsg("cannot create vex_jieba dictionary with userdict template \"%s\"," 
                                " which use delimiter", templ_dict_name)));
                }
                info->templ_userdict_setting = templ_setting;
                if (pg_strcasecmp("default", templ_setting) == 0) {
                    paths[DJIEBA_USERDICT] = default_dict_path;
                }
                templ_userdict = get_ts_content(templ_dict_name, 'u', templ_userdict_len);
            }
        } else if (pg_strcasecmp("KeywordCaseSensitive", defel->defname) == 0) {
            if (argsloaded[DJIEBA_SENSITIVE]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("multiple KeywordCaseSensitive parameters")));
            }
            if (argsloaded[DJIEBA_DELIMITER]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("KeywordCaseSensitive can only used with Userdict")));
            }
            args[DJIEBA_SENSITIVE] = defGetString(defel);
            argsloaded[DJIEBA_SENSITIVE] = true;
            
            bool res = false;
            if (parse_bool(args[DJIEBA_SENSITIVE], &res)) {
                info->sensitive = res;
                info->case_sensitive = res ? "true" : "false";
            } else {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("unrecognized KeywordCaseSensitive parameter: \"%s\"," 
                                       " must be boolean value", args[DJIEBA_SENSITIVE])));
            }
        } else if (pg_strcasecmp("Delimiter", defel->defname) == 0) {
            if (argsloaded[DJIEBA_DELIMITER]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("multiple Delimiter parameters")));
            }
            if (argsloaded[DJIEBA_USERDICT]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("Only one of UserDict or Delimiter can be used")));
            }
            if (argsloaded[DJIEBA_SENSITIVE]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("KeywordCaseSensitive can only used with Userdict")));
            }
            args[DJIEBA_DELIMITER] = defGetString(defel);
            argsloaded[DJIEBA_DELIMITER] = true;
            info->delimiter = args[DJIEBA_DELIMITER];

            if (!(strlen(args[DJIEBA_DELIMITER]) > 0 && strlen(args[DJIEBA_DELIMITER]) < 20)) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Delimiter length must be greater than 0 and less than 20")));
            }
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unrecognized jiaba parameter: \"%s\"", defel->defname)));
        }
    }
    /*
    * Need to load dictionary for two cases:
    * 1.Use dictionary
    * 2.Create/alter dictionary query from user directly
    */
   if (first_init) {
        info->templ_stopwords.values = templ_stopwords;
        info->templ_stopwords.count = (int32)templ_stopwords_len;
        info->templ_userdict.values = templ_userdict;
        info->templ_userdict.count = (int32)templ_userdict_len;
        TemplContentInfos *copy = info->deep_copy();
        new (j)
            Jieba(
                args[DJIEBA_DELIMITER],
                paths[DJIEBA_USERDICT],
                default_hmm_path,
                paths[DJIEBA_STOPWORDS],
                info->sensitive,
                copy->templ_userdict,
                copy->templ_stopwords
            );

        PG_RETURN_POINTER((List *)info);
    } else {
        uint32 this_stopwords_len;
        char **this_stopwords = get_ts_content(dict_id, 's', this_stopwords_len);
        ArrayCStr stopwords_arr = {(int32)this_stopwords_len, this_stopwords};
        converse_encoding_arraycstr(stopwords_arr);

        ArrayCStr userdict_arr;
        if (info->delimiter == NULL) {
            uint32 this_userdict_len;
            char **this_userdict = get_ts_content(dict_id, 'u', this_userdict_len);
            userdict_arr = {(int32)this_userdict_len, this_userdict};
            converse_encoding_arraycstr(userdict_arr);
        }

        new (j)
            Jieba(
                args[DJIEBA_DELIMITER],
                paths[DJIEBA_USERDICT],
                default_hmm_path,
                paths[DJIEBA_STOPWORDS],
                info->sensitive,
                userdict_arr,
                stopwords_arr
            );
        /* need to free memory when not first time init */
        free_arraycstr(stopwords_arr);
        if (templ_stopwords) {
            for (uint32 i = 0; i < templ_stopwords_len; ++i) {
                pfree(templ_stopwords[i]);
            }
            pfree(templ_stopwords);
        }
        if (info->delimiter == NULL) {
            free_arraycstr(userdict_arr);
            if (templ_userdict) {
                for (uint32 i = 0; i < templ_userdict_len; ++i) {
                    pfree(templ_userdict[i]);
                }
                pfree(templ_userdict);
            }
        }
        pfree(info);
        list_free_deep(dictoptions);

        PG_RETURN_POINTER(j);
    }
}

Datum djieba_lexize(PG_FUNCTION_ARGS)
{
    ereport(ERROR, (
        errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("vexjieba cannot be used externally as text search dictionary, "
               "it only supports internal usage from FULLTEXT index"),
        errhint("Please do not use vexjieba dictionary for text configuration, ts_lexize"
                "TSVector, and other non-BM25 use case")));
                
    Jieba *j = (Jieba *)PG_GETARG_POINTER(0);
    char *in = (char *)PG_GETARG_POINTER(1);
    int32 len = PG_GETARG_INT32(2);
    TSLexeme *res = NULL;

    char *txt = (char*)palloc(sizeof(char) * (len + 1));
    errno_t rc = memcpy_s(txt, len + 1, in, len + 1);
    securec_check_c(rc, "\0", "\0");
    txt[len] = '\0';

    if (*txt == '\0' || j->is_stopword(txt)) {
        /* reject as jieba stopword */
        pfree_ext(txt);
        res = (TSLexeme *)palloc0(sizeof(TSLexeme) * 2ul);
        PG_RETURN_POINTER(res);
    }

    res = (TSLexeme *)call_dsnowball_lexize(txt);
    pfree(txt);
    PG_RETURN_POINTER(res);
}
