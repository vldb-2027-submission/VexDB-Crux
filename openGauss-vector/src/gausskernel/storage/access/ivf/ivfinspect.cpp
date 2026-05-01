/**
 * Copyright ...
 */

#include "access/annvector/ivf.h"
#include "access/bm25/index_inspect.h"

void *ivfflat_inspect(Relation index)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("index_inspect does not support ivfflat")));
    IndexInspectResult *res = NULL;
    return res;
}

void *ivfpq_inspect(Relation index)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("index_inspect does not support ivfpq")));
    IndexInspectResult *res = NULL;
    return res;
}
