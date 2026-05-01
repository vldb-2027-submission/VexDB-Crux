#ifndef PG_EMBEDDING_PROVIDER_H
#define PG_EMBEDDING_PROVIDER_H

#include "catalog/genbki.h"

#define EmbProvRelationId  3632 
#define EmbProvRelation_Rowtype_Id 3633

CATALOG(pg_embedding_provider,3632) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
    NameData    service_name;
    NameData    api_provider;
#ifdef CATALOG_VARLEN         /* variable-length fields start here */    
    text        endpoint;
    text        apikey;
#endif    
} FormData_pg_embedding_provider;

typedef FormData_pg_embedding_provider *Form_pg_embedding_provider;

#define Natts_pg_embedding_provider                 4
#define Anum_pg_embedding_provider_service_name     1
#define Anum_pg_embedding_provider_api_provider     2
#define Anum_pg_embedding_provider_endpoint         3
#define Anum_pg_embedding_provider_apikey           4

#endif