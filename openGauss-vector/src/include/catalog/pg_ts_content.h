#ifndef PG_TS_CONTENT_H
#define PG_TS_CONTENT_H

#include "catalog/genbki.h"

#define TSContentRelationId    3627
#define TSContentRelation_Rowtype_Id 3630

CATALOG(pg_ts_content,3627) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
    Oid         dictoid;      /* dict oid */
    char        type;         /* type of content, could be stopwords or userdicts */
#ifdef CATALOG_VARLEN         /* variable-length fields start here */
    text        content[1];   /* stopwords or userdicts */
#endif
} FormData_pg_ts_content;

typedef FormData_pg_ts_content *Form_pg_ts_content;

#define Natts_pg_ts_content                 3
#define Anum_pg_ts_content_dictoid          1
#define Anum_pg_ts_content_type             2
#define Anum_pg_ts_content_content          3

#endif