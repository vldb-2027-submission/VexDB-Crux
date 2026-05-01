/**
 * Copyright ...
 */

#include <new>  /* inplace new */

#include "utils/builtins.h"
#include "utils/palloc.h"
#include "catalog/namespace.h"
#include "access/bm25/bm25_parameters.h"
#include "access/bm25/bm25.h"
#include "access/bm25/tokenizer/cppjieba/string_util.h"
#include "access/bm25/tokenizer/tokenizer.h"
#include "catalog/pg_opfamily.h"

using namespace bm25;
using namespace bm25_tokenizer;

constexpr char ATTR_SEP = '#';
constexpr char COEF_SEP = ':';
constexpr char COEF_EQUAL = '=';
constexpr char *attr_sep = "#";
constexpr char *coef_sep = ":";
constexpr char *coef_equal = "=";
constexpr char *DEFAULT_BM25_DICTIONARY = "default";
constexpr char *DEFAULT_BM25_METRIC = "default";

constexpr const char *bm25::metric_str(ScoreMetric metric)
{
    switch (metric) {
        case ScoreMetric::CLASSIC:
            return "bm25";
        case ScoreMetric::TF_IDF:
            return "tf_idf";
        case ScoreMetric::LOG_TF_IDF:
            return "log_tf_idf";
        default:
            break;
    }
    return NULL; /* keep compiler quiet */
}

static uint32 check_nsep(const char *value, char sep)
{
    const char *cur = value;
    uint32 nsep = 0;
    while (cur) {
        cur = strchr(cur, sep);
        if (cur) {
            ++cur;
            ++nsep;
        }
    }
    if (BM25_MAX_NATTR <= nsep) {
        ereport(ERROR, (errmsg("the number of columns used for indexing "
                               "exceeds the upper limit of %d", BM25_MAX_NATTR)));
    }
    return nsep;
}

static void extract_metric(const char *alg_val, Scorer *scorers, uint32 nscorer)
{
    if (!alg_val) {
        return;
    }
    char *alg_val_tmp = pstrdup(alg_val);
    char *alg_vals[BM25_MAX_NATTR];
    uint32 nassigned_algs = split(alg_vals, alg_val_tmp, attr_sep, BM25_MAX_NATTR);
    if (nscorer < nassigned_algs) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("the number of algorithms exceeds the number of indexed columns")));
    }
    for (uint32 i = 0; i < nassigned_algs; ++i) {
        if (pg_strcasecmp(alg_vals[i], metric_str(ScoreMetric::CLASSIC)) == 0) {
            scorers[i].metric = ScoreMetric::CLASSIC;
        } else if (pg_strcasecmp(alg_vals[i], metric_str(ScoreMetric::TF_IDF)) == 0) {
            scorers[i].metric = ScoreMetric::TF_IDF;
        } else if (pg_strcasecmp(alg_vals[i], metric_str(ScoreMetric::LOG_TF_IDF)) == 0) {
            scorers[i].metric = ScoreMetric::LOG_TF_IDF;
        } else if (pg_strcasecmp(alg_vals[i], DEFAULT_BM25_METRIC) != 0) {
            ereport(ERROR,
                (errmsg("Algorithm \"%s\" is not supported", alg_vals[i])));
        }
    }
    pfree(alg_val_tmp);
}

static void extract_coefficients(const char *coef_val, Scorer *scorers, uint32 nscorer)
{
    if (!coef_val) {
        return;
    }
    char *coef_val_tmp = pstrdup(coef_val);
    char *coef_vals[BM25_MAX_NATTR];
    uint32 nassigned_coefs = split(coef_vals, coef_val_tmp, attr_sep, BM25_MAX_NATTR);
    if (nscorer < nassigned_coefs) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("the number of coefficients exceeds the number of indexed columns")));
    }
    constexpr size_t bm25_coef_args_num = 2ul;
    char *args[bm25_coef_args_num];
    for (uint32 i = 0; i < nassigned_coefs; ++i) {
        size_t nsep = check_nsep(coef_vals[i], COEF_EQUAL);
        if (nsep > bm25_coef_args_num) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("the number of coefficients exceeds possible parameter number of %lu",
                        bm25_coef_args_num)));
        }
        const size_t nargs = split(args, coef_vals[i], coef_sep, bm25_coef_args_num);
        bool b_set = false;
        bool k_set = false;
        ScoreCoefficient &coef = scorers[i].coef;
        for (size_t j = 0; j < nargs; ++j) {
            constexpr size_t two = 2ul;
            char *args_c[two];
            size_t n = split(args_c, args[j], coef_equal, two);
            if (n != two) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("Incorrect parameter format for \'%s\', "
                            "expected <parameter>%c<value>", args[j], COEF_EQUAL)));
            }
            if (strcmp(args_c[0], "b") == 0) {
                if (b_set) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("Duplicate parameter \'b\' in coefficient \"%s\"", coef_vals[i])));
                }
                b_set = true;
                char *ptr;
                coef.b = strtof(args_c[1], &ptr);
                if (ptr != args_c[1] + strlen(args_c[1])) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Incorrect parameter format for \'b\', "
                               "expected a float but got %s", args_c[1])));
                } else if (coef.b < 0 || coef.b > 1) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Incorrect parameter value for \'b\', "
                               "expected range [0, 1] but got %f", coef.b)));
                }
            } else if (strcmp(args_c[0], "k") == 0) {
                if (k_set) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("Duplicate parameter \'k\' in coefficient \"%s\"", coef_vals[i])));
                }
                k_set = true;
                char *ptr;
                coef.k = strtof(args_c[1], &ptr);
                if (ptr != args_c[1] + strlen(args_c[1])) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Incorrect parameter format for \'k\', "
                               "expected float, got %s", args_c[1])));
                } else if (coef.k < 1 || coef.k > 2) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Incorrect parameter value for \'k\', "
                               "expected range [1, 2] but got %f", coef.k)));
                }
            } else {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Incorrect parameter name for \'%s\', "
                               "expected \'b\' or \'k\'", args_c[0])));
            }
        }
    }
    pfree(coef_val_tmp);
}

void bm25::extract_scorer(const char *alg_val, const char *coef_val, Scorer *scorers,
                          uint32 nscorer, Relation index)
{
    for (uint32 i = 0; i < nscorer; ++i) {
        Scorer &s = scorers[i];
        if (index->rd_opfamily[i] == SPARSEVEC_IP_FULLTEXT_FAM_OID) {
            s.metric = ScoreMetric::IP;
        } else if (index->rd_opfamily[i] == SPARSEVEC_COSINE_FULLTEXT_FAM_OID) {
            s.metric = ScoreMetric::COSINE;
        } else {
            s.metric = ScoreMetric::CLASSIC;
        }
        new (&s.coef) ScoreCoefficient();
    }
    extract_metric(alg_val, scorers, nscorer);
    extract_coefficients(coef_val, scorers, nscorer);
}

void bm25::extract_dict(const char *dict_val, Oid *dict_ids, uint32 ndict)
{
    if (!dict_val) {
        if (dict_ids) {
            for (uint32 i = 0; i < ndict; ++i) {
                dict_ids[i] = default_jieba_dict_id;
            }
        }
        return;
    }

    char *dict_val_tmp = pstrdup(dict_val);
    char *dict_names[BM25_MAX_NATTR];
    uint32 nassigned_dicts = split(dict_names, dict_val_tmp, attr_sep, BM25_MAX_NATTR);
    if (ndict < nassigned_dicts) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("the number of dictionaries exceeds the number of indexed columns")));
    }
    uint32 i = 0;
    for (; i < nassigned_dicts; ++i) {
        List *name = stringToQualifiedNameList(dict_names[i]);
        Oid dict_oid = get_ts_dict_oid(name, false);
        if (dict_ids) {
            dict_ids[i] = dict_oid;
        }
        list_free_ext(name);
    }
    if (dict_ids) {
        for (; i < ndict; ++i) {
            dict_ids[i] = default_jieba_dict_id;
        }
    }
    pfree(dict_val_tmp);
}

void validate_dicts(const char *value)
{
    extract_dict(value, NULL, BM25_MAX_NATTR);
}

void validate_algorithms(const char *value)
{
    const uint32 nsep = check_nsep(value, ATTR_SEP);
    Scorer scorers[nsep + 1u];
    extract_metric(value, scorers, nsep + 1u);
}

void validate_coefficients(const char *value)
{
    const uint32 nsep = check_nsep(value, ATTR_SEP);
    Scorer scorers[nsep + 1u];
    extract_coefficients(value, scorers, nsep + 1u);
}
