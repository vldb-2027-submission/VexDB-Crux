#include "curl/curl.h"
#include "cjson/cJSON.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "postgres.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "catalog/indexing.h"
#include "utils/fmgroids.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "fmgr/fmgr_comp.h"
#include "access/embedding/embedding.h"
#include "access/annvector/floatvector.h"
#include "catalog/pg_embedding_provider.h"

typedef struct {
    char* buffer;
    size_t size;
} ApiResponse;

static void init_api_response(ApiResponse* resp) {
    resp->buffer = (char *)palloc(1);
    resp->size = 0;
}

class EmbedRequestBase {
    public:
        explicit EmbedRequestBase() {}
        virtual ~EmbedRequestBase() {}
        virtual char* serialize() const = 0;
        virtual void parse_response(const ApiResponse* resp, FloatVector **vector_out, MemoryContext out_ctx) = 0;
        virtual void parse_failed_response(const ApiResponse* resp, const int http_code) = 0;
        virtual void destroy() {}
};

class OllamaEmbedRequest : public EmbedRequestBase {
public:
    OllamaEmbedRequest(char* model, char* input)
        : EmbedRequestBase(), _model(model), _input(input) {}
    
    char* serialize() const override {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", _model);
        cJSON_AddStringToObject(root, "input", _input);
        char* json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return json_str;
    }

    void parse_response(const ApiResponse* resp, FloatVector **vector_out, MemoryContext out_ctx) override {
        if (!resp->buffer || resp->size == 0) {
            elog(ERROR, "Embedding response data is empty");
            return;
        }
        cJSON* root = cJSON_Parse(resp->buffer);
        if (!root) {
            elog(ERROR, "Failed to decode embedding response json data: %s ", cJSON_GetErrorPtr());
            return;
        }

        cJSON* embeddings = cJSON_GetObjectItem(root, "embeddings");
        if (cJSON_IsArray(embeddings) && cJSON_GetArraySize(embeddings) > 0) {
            cJSON* first_embedding = cJSON_GetArrayItem(embeddings, 0);
            if (cJSON_IsArray(first_embedding)) {
                int dim = cJSON_GetArraySize(first_embedding);
                MemoryContext old_ctx = MemoryContextSwitchTo(out_ctx);
                *vector_out = InitFloatVector(dim);
                MemoryContextSwitchTo(old_ctx);
                cJSON* item = nullptr;
                int i = 0;
                cJSON_ArrayForEach(item, first_embedding) {
                    (*vector_out)->x[i++] = item->valuedouble;
                }
            } else {
                elog(ERROR, "Embedding response's embeddings attribute is not array");
            }
        } else {
            elog(ERROR, "There is no embeddings attribute in Embedding response data");
        }
        cJSON_Delete(root);
    }

    void parse_failed_response(const ApiResponse* resp, const int http_code)
    {
        if (!resp->buffer || resp->size == 0) {
            elog(ERROR, "Embedding request faild, http code: %d, response data is empty", http_code);
        }

        cJSON* err_json = cJSON_Parse(resp->buffer);
        if (err_json) {
            cJSON* err_msg = cJSON_GetObjectItem(err_json, "error");
            if (cJSON_IsString(err_msg)) {
                elog(ERROR, "Embedding request faild, http code: %d, error detail: %s", http_code, err_msg->valuestring);
            }
            cJSON_Delete(err_json);
        }
        elog(ERROR, "Embedding request faild, http code: %d, response buffer: %s", http_code, resp->buffer);
    }

private:
    char* _model;
    char* _input;
};

class HttpClient {
public:
    HttpClient() : _curl(nullptr), _headers(nullptr) 
    {
        _headers = curl_slist_append(_headers, "Content-Type: application/json");
        _headers = curl_slist_append(_headers, "Accept: application/json");
        _curl = curl_easy_init();
        if (!_curl) {
            elog(ERROR, "Faild to initialize curl.");
        }
    }

    void destroy() {
        if (_headers) curl_slist_free_all(_headers);
        if (_curl) curl_easy_cleanup(_curl);
    }

    int send_post(const char* url, const char* post_data, ApiResponse* resp) {
        if (!post_data) {
            elog(ERROR, "POST body is empty");
            return -1;
        }

        curl_easy_setopt(_curl, CURLOPT_URL, url);
        curl_easy_setopt(_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, _headers);

        curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, response_callback);
        curl_easy_setopt(_curl, CURLOPT_WRITEDATA, resp);

        // 执行请求
        CURLcode res = curl_easy_perform(_curl);
        long http_code = 0;
        if (res != CURLE_OK) {
            elog(ERROR, "Failed to send Embedding request: %s", curl_easy_strerror(res));
            http_code = -1;
        } else {
            curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &http_code);
        }
        return (int)(http_code);
    }

private:
    static size_t response_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
        size_t realsize = size * nmemb;
        ApiResponse* resp = static_cast<ApiResponse*>(userdata);
        char* new_buffer = (char *)repalloc(resp->buffer, resp->size + realsize + 1);
        if (!new_buffer) {
            elog(ERROR, "Failed to allocate %zu bytes for embedding response callback", resp->size + realsize + 1);
            return 0;
        }

        resp->buffer = new_buffer;
        memcpy(&(resp->buffer[resp->size]), ptr, realsize);
        resp->size += realsize;
        resp->buffer[resp->size] = '\0';

        return realsize;
    }

    CURL* _curl;
    struct curl_slist* _headers;
};



char *get_service_endpoint(char *service_name)
{
    Relation provider_rel = heap_open(EmbProvRelationId, RowExclusiveLock);
    ScanKeyData scanKeys[1];
    ScanKeyInit(&scanKeys[0],
                Anum_pg_embedding_provider_service_name,
                BTEqualStrategyNumber,
                F_NAMEEQ,
                CStringGetDatum(service_name));

    SysScanDesc scan = systable_beginscan(provider_rel, EMBPROVSERNAMEIndexId, true, NULL, 1, scanKeys);
    HeapTuple htup = systable_getnext(scan);
    if (!HeapTupleIsValid(htup)) {
        ereport(ERROR, (errcode(ERRCODE_FETCH_DATA_FAILED),
            errmsg("Failed to load embedding provider for %s", service_name), errdetail("N/A"),
            errcause("Possible snapshot un-sync."), erraction("Please reconnect and try again.")));
    }
    bool isnull = false;
    Datum endpoint = heap_getattr(htup, Anum_pg_embedding_provider_endpoint, provider_rel->rd_att, &isnull);
    char *endpoint_str = text_to_cstring(DatumGetTextP(endpoint));
    systable_endscan(scan);
    heap_close(provider_rel, RowExclusiveLock);
    return endpoint_str;
}


Datum text_dense_embedding(PG_FUNCTION_ARGS)
{
    char *service_name = PG_GETARG_CSTRING(0);
    char *model = PG_GETARG_CSTRING(1);
    text *text_p = PG_GETARG_TEXT_P(2);

    MemoryContext tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
        "Embedding temporary context", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old_ctx = MemoryContextSwitchTo(tmpCtx);

    char *text = text_to_cstring(text_p);
    if (strlen(service_name) == 0) {
        ereport(ERROR, (errmsg("service name is empty!")));
    }
    if (strlen(model) == 0) {
        ereport(ERROR, (errmsg("model name is empty!")));
    }

    char *endpoint = get_service_endpoint(service_name);
    size_t endpoint_len = strlen(endpoint);
    const char *suffix = "/embed";
    size_t suffix_len = strlen(suffix);
    char* post_endpoint = (char *)palloc(endpoint_len + suffix_len + 1);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    strncpy(post_endpoint, endpoint, endpoint_len);
#pragma GCC diagnostic pop
    strcpy(post_endpoint + endpoint_len, suffix);

    OllamaEmbedRequest ollama_req(model, text);
    char* ollama_post_data = ollama_req.serialize();
    ApiResponse ollama_resp;
    init_api_response(&ollama_resp);
    HttpClient http_client;
    int ollama_http_code = http_client.send_post(post_endpoint, ollama_post_data, &ollama_resp);

    FloatVector *vector;
    if (ollama_http_code == 200) {
        ollama_req.parse_response(&ollama_resp, &vector, old_ctx);
    } else {
        ollama_req.parse_failed_response(&ollama_resp, ollama_http_code);
    }

    ollama_req.destroy();
    http_client.destroy();

    MemoryContextDelete(tmpCtx);
    MemoryContextSwitchTo(old_ctx);

    PG_RETURN_POINTER(vector);
}

Datum add_embedding_provider(PG_FUNCTION_ARGS)
{
    char *service_name = PG_GETARG_CSTRING(0);
    char *endpoint = PG_GETARG_CSTRING(1);
    char *apikey = PG_GETARG_CSTRING(2);
    char *api_provider = PG_GETARG_CSTRING(3);

    if (!service_name) {
        elog(ERROR, "service_name is NULL");
    }

    if (!endpoint) {
        elog(ERROR, "endpoint is NULL");
    }

    if (!api_provider) {
        elog(ERROR, "api_provider is NULL");
    }

    if (strcmp(api_provider, "ollama") != 0) {
        elog(ERROR, "api_provider should be ollama");
    }

    size_t len = strlen(endpoint);
    if (len > 0 && endpoint[len - 1] == '/') {
        endpoint[len - 1] = '\0';
    }
    Datum provider_values[Natts_pg_embedding_provider];
    bool provider_nulls[Natts_pg_embedding_provider] = {false, false, false, false};
    provider_values[Anum_pg_embedding_provider_service_name - 1] =  DirectFunctionCall1(namein, CStringGetDatum(service_name));
    provider_values[Anum_pg_embedding_provider_api_provider - 1] = DirectFunctionCall1(namein, CStringGetDatum(api_provider));
    provider_values[Anum_pg_embedding_provider_endpoint - 1] = PointerGetDatum(cstring_to_text(endpoint));
    if (apikey) {
        provider_values[Anum_pg_embedding_provider_apikey - 1] = PointerGetDatum(cstring_to_text(apikey));
    } else {
        provider_nulls[Anum_pg_embedding_provider_apikey - 1] = true;
    }

    Relation provider_rel = heap_open(EmbProvRelationId, RowExclusiveLock);
    HeapTuple tup = heap_form_tuple(provider_rel->rd_att, provider_values, provider_nulls);
    simple_heap_insert(provider_rel, tup);
    CatalogUpdateIndexes(provider_rel, tup);
    heap_close(provider_rel, RowExclusiveLock);
    PG_RETURN_BOOL(true);
}
Datum delete_embedding_provider(PG_FUNCTION_ARGS)
{
    char *service_name = PG_GETARG_CSTRING(0);
    Relation provider_rel = heap_open(EmbProvRelationId, RowExclusiveLock);
    ScanKeyData scanKeys[1];
    ScanKeyInit(&scanKeys[0],
                Anum_pg_embedding_provider_service_name,
                BTEqualStrategyNumber,
                F_NAMEEQ,
                CStringGetDatum(service_name));

    SysScanDesc scan = systable_beginscan(provider_rel, EMBPROVSERNAMEIndexId, true, NULL, 1, scanKeys);
    HeapTuple htup = systable_getnext(scan);
    bool del = false;
    if (HeapTupleIsValid(htup)) {
        simple_heap_delete(provider_rel, &htup->t_self);
        del = true;
    }
    systable_endscan(scan);
    heap_close(provider_rel, RowExclusiveLock);
    PG_RETURN_BOOL(del);
}