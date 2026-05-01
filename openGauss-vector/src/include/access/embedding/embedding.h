#ifndef EMBEDDING_H
#define EMBEDDING_H
#include "fmgr.h"

extern Datum text_dense_embedding(PG_FUNCTION_ARGS);
extern Datum add_embedding_provider(PG_FUNCTION_ARGS);
extern Datum delete_embedding_provider(PG_FUNCTION_ARGS);

#endif