#ifndef HNSW_QUANTIZER_H
#define HNSW_QUANTIZER_H

#include "postgres.h"
#include "storage/smgr/fd.h"

#include "access/annvector/pq.h"
#include "access/annvector/quantizer.h"
#include "access/annvector/store/buffer_manager.h"
#include "access/annvector/timering.h"

#define HNSW_RABITQ_NUM_CLUSTERS 16
#define HNSW_RABITQ_EX_BITS 8
#define HNSW_MIN_QT_SAMPLES_SIZE 10000

struct HnswPQMetaInfo {
    bool graph_pq; /* false if data is not enough to calculate pq */
    uint16 m;
    uint16 k;
    void init(uint32 dim) {
        graph_pq = false;
        if (dim % 4u == 0) {
            m = dim / 4u;
        } else if (dim % 3u == 0) {
            m = dim / 3u;
        } else if (dim % 5u == 0) {
            m = dim / 5u;
        } else if (dim % 2u == 0) {
            m = dim / 2u;
        } else {
            m = dim;
        }
        k = 256u;
    }
    uint32 code_size() const { return m * (k <= 256 ? 1 : 2); }
    uint16 nbits() const { Assert(k > 0); Assert(31 - __builtin_clz(k) == 8); return 31 - __builtin_clz(k); }
};

struct PQParam {
    HnswPQMetaInfo pq_metainfo;
    char *code;
    uint32 code_len;
    ProductQuantizer *pq;
    float *dist_table;
    float flag;
};

struct RaBitQMeta {
    bool enabled; /* RaBitQ is enabled for current index */
    bool keep_vecs; /* whether keeping original vectors in vector buffers or not */
    int quant_size; /* cid_size + bin_size + ext_size */
    double query_rescaling_factor; /* pre-computed factor for query only */
};

struct RaBitQParam {
    RaBitQMeta rbq_meta;
    bool applied; /* RaBitQ is applied for distance estimation */
    int dim; /* dimension */
    int padded_dim; /* padded dimentions by 64 */
    Metric metric; /* metric */
    size_t num_vectors; /* current number of all stored vectors */
    int cid_size; /* the size of the index of the closest cluster for one vector */
    int bin_size; /* the size of bin_data for one vector */
    int ext_size; /* the size of ext_data for one vector */
    char *quant_data; /* quantized data (cluster_id + bin_data + ext_data) */
    void *quantizer; /* used for vector quantization */
    void *estimator; /* used for vector distance estimation */
    char *get_quant_data(size_t idx) { return quant_data + idx * rbq_meta.quant_size; }
};

/* quantizer meta info, write to disk */
struct QuantizerMetaInfo {
    union {
        HnswPQMetaInfo pq_metainfo;
        RaBitQMeta rbq_meta;
    } metainfo;
    QuantizerType quantizer_type;
    uint32 num_new_data;
    uint8 centroids_version;
    uint8 code_version;
    TimestampTz last_update_time;

    QuantizerType get_type() const {
        int retry = 0;
        while (centroids_version != code_version) {
            ereport(WARNING,
                (errmsg("Index version does not match, it may be updating. "
                        "Wait for five seconds, up to a maximum of twenty seconds.")));
            ++retry;
            pg_usleep(5 * 100'0000);
            if (retry == 4) {
                ereport(ERROR, (errmsg("index may be broken, need check and rebuild")));
            } 
        }
        if (quantizer_type == QuantizerType::PQ) {
            return get_pq_metainfo().graph_pq ? QuantizerType::PQ : QuantizerType::NONE;
        } else if (quantizer_type == QuantizerType::RABITQ) {
            return get_rabitq_meta().enabled ? QuantizerType::RABITQ : QuantizerType::NONE;
        } else {
            return QuantizerType::NONE;
        }
    }
    QuantizerType get_setting_type() const { return quantizer_type; }
    bool has_quant() const { return get_type() != QuantizerType::NONE; }
    void set_type(QuantizerType qt_type) { quantizer_type = qt_type; }
    const HnswPQMetaInfo &get_pq_metainfo() const { return metainfo.pq_metainfo; }
    HnswPQMetaInfo &get_pq_metainfo() { return metainfo.pq_metainfo; }
    const RaBitQMeta &get_rabitq_meta() const { return metainfo.rbq_meta; }
    RaBitQMeta &get_rabitq_meta() { return metainfo.rbq_meta; }
};

/* quantizer running param, used when build, search or vacuum */
struct QuantizerParam {
    QuantizerType quantizer_type;
    QuantizerType setting_quantizer_type;
    BulkBuffer *bulkbuf;
    union {
        PQParam pq_param;
        RaBitQParam rbq_param;
    } param;

    void set_bulkbuf(BulkBuffer *buf) { bulkbuf = buf; }
    void release_bulkbuf() {
        if (bulkbuf) {
            bulkbuf->release();
            bulkbuf = NULL;
        }
    }
    QuantizerType get_setting_type() { return setting_quantizer_type; }
    QuantizerType get_type() { return quantizer_type; }
    void set_type(const QuantizerType qt_type, const QuantizerType setting_qt_type) {
        quantizer_type = qt_type;
        setting_quantizer_type = setting_qt_type;
    }
    const PQParam &get_pq_param() const { return param.pq_param; }
    PQParam &get_pq_param() { return param.pq_param; }
    const RaBitQParam &get_rabitq_param() const { return param.rbq_param; }
    RaBitQParam &get_rabitq_param() { return param.rbq_param; }
    
    void set_resource(Relation index, void *metap, char *query = NULL, bool building = false, bool set_bulk = true);
    void release_resource();
};

/* quantizer updating struct */
struct Slot {
    size_t floatVectorIdx;
    float *value;
    Slot(size_t idx, float *v) : floatVectorIdx(idx), value(v) {}
};
struct QtUpdatingBuffer {
    slock_t lock;
    Vector<Slot> record_vec;
    QtUpdatingBuffer() { SpinLockInit(&lock); }
};
struct QtTmpFile {
    Oid oid;
    uint16 idx;
    QtTmpFile(Oid o, uint16 i) : oid(o), idx(i) {}
};
struct QtUpdateMgr {
    UnorderedMap<Oid, QtUpdatingBuffer *> qt_record_map;
    slock_t qt_record_maplock;
    UnorderedSet<Oid> qt_updating_set;
    slock_t qt_updating_setlock;
    UnorderedMap<Oid, TimeRing *> qt_timering_map;
    slock_t qt_timering_maplock;

    bool insert_updating(Oid oid);
    void erase_updating(Oid oid);
    bool contain_updating(Oid oid);
    void insert_record(Oid oid);
    Vector<Slot> *erase_record(Oid oid);
    QtUpdatingBuffer *find_record(Oid oid);
    TimeRing *insert_timgring(Oid oid);
    TimeRing *find_timering(Oid oid);
    void erase_timering(Oid oid);
};

#define IS_GRAPH_INDEX(index) ((index)->rd_am->ambuild == F_HNSWBUILD)

#define OUTPUT_QTUPDATE_LOG true
#if OUTPUT_QTUPDATE_LOG
#define QT_UPDATE_LOG(fmt, ...) ereport(LOG, (errcode(ERRCODE_LOG), errmsg(fmt, ##__VA_ARGS__)))
#else
#define QT_UPDATE_LOG(fmt, ...)
#endif /* OUTPUT_TASKPOOL_LOG */

/* pq */
extern void	hnsw_read_pq_center(Relation index, ProductQuantizer &pq);
extern void store_centroids(Relation index, const float *center, size_t write_size,
    bool building, bool enabling, bool updating);

/* rabitq */
extern bool HnswGetRaBitQKeepVecs(Relation index);
extern void QuantizeRaBitQ(RaBitQParam &rbq_param, float *vec, char *quant_data);
extern void read_rabitq_data(Relation index, size_t rabitq_data_size, char *rabitq_data);

/* general */
extern bool fetch_vec_from_heap(Relation index, Relation heap, ItemPointerData htid, char *vec,
    uint32 dim, DistPrecisionType precision_type);
extern ItemPointerData get_heap_tid(Relation index, ItemPointerData indexTid);
FloatVectorArray quantizer_sample_data(Relation heap, Relation index, size_t dimensions,
    bool need_norm, DistPrecisionType precision_type, int parallel_workers, size_t k);
ProductQuantizer *do_kmeans(Relation index, FloatVectorArray samples, size_t dimensions,
    size_t m, size_t k, Metric metric, bool need_norm, int parallel_workers);

/* quantizer update */
extern void add_quantizer_update_task(Relation index, void *params);
extern void qt_update_init(knl_g_annvec_context *annvec_cxt);
extern void prepare_quantizer_update_task(TimeRing *timering);
Datum index_qtupdate(PG_FUNCTION_ARGS);

#endif /* HNSW_QUANTIZER_H */
