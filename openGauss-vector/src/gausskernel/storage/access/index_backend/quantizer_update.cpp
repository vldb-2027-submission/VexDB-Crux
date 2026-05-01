#include "catalog/pg_partition_fn.h"
#include "storage/copydir.h"

#include "access/index_backend/index_backend.h"
#include "access/hnsw/hnsw_quantizer.h"
#include "access/hnsw/hnsw_param.h"
#include "access/hnsw/hnsw_struct.h"
#include "access/hnsw/hnsw_xlog.h"
#include "access/annvector/eummd.h"
#include "access/rabitq/rabitq.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/annkmeans.h"

using namespace rabitq;

void add_quantizer_update_task(Relation index, void *params)
{
    auto old_ctx = MemoryContextSwitchTo(IndexerCtx);
    IndexerTask *task = (IndexerTask *)palloc(sizeof(IndexerTask));

    task->dbid = t_thrd.proc->databaseId;
    if (RelationIsPartition(index)) {
        task->rd_id = GetBaseRelOidOfParition(index);
        task->part_id = RelationGetRelid(index);
        task->parent_heapoid = index->rd_index->indrelid;
        task->part_heapoid = index->rd_partHeapOid;
    } else {
        task->rd_id = RelationGetRelid(index);
        task->part_id = InvalidOid;
        task->parent_heapoid = InvalidOid;
        task->part_heapoid = InvalidOid;
    }

    task->type = IndexerTaskType::QuantizerUpdate;
    task->delay_timer.start_time = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), -1);
    task->delay_timer.cur_delay = 100;
    task->delay_timer.max_delay = 3000;

    IndexerBaseTaskParam *base_params = (IndexerBaseTaskParam *)params;
    base_params->parallel_workers = 0;
    base_params->maintenance_work_mem = u_sess->attr.attr_memory.maintenance_work_mem;
    base_params->tuple_desc = NULL;
    base_params->parent_id = task->rd_id;

    task->params = NEW QuantizerUpdateParam(*(QuantizerUpdateParam *)params);
    task->trx_cxt.txnId = GetCurrentTransactionIdIfAny();
    task->trx_cxt.snapshot = CopySnapshotByCurrentMcxt(GetActiveSnapshot());
    stream_save_txn_context(&task->trx_cxt);

    insert_task(task);
    if (LauncherLatch) {
        SetLatch(LauncherLatch);
    }
    MemoryContextSwitchTo(old_ctx);
}

static void set_buffile(BufFile *file, int file_no, off_t idx, char *code, size_t code_len)
{
    if (BufFileSeek(file, file_no, idx, SEEK_SET) != 0) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
             errmsg("Invalid access idx: %lu", idx)));
    }
    if (BufFileWrite(file, code, code_len) != code_len) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
             errmsg("Write failed")));
    }
}

/* must equal to vector_smgr setting, 1GB */
constexpr char *max_one_file_size_kb = "1048576";
constexpr int max_one_file_size = 1024 * 1024 * 1024;
/* must bigger than `max_one_file_size_kb` */
constexpr char *maintenance_work_mem_limit = "2097152";

static void xlog_write_new_vecfiles(BufFile *tmpfile, Relation index, Page metapage, Buffer metabuf)
{
    int tmp_files_num = BufFileGetNums(tmpfile);
    File *tmpfile_files = BufFileGetFiles(tmpfile);
    BufFileFlush(tmpfile);
    char *buf = NULL;
    for (int i = 0; i < tmp_files_num; ++i) {
        File new_file = tmpfile_files[i];
        buf = (char *)palloc_huge(CurrentMemoryContext, max_one_file_size);
        int readbyte = FilePRead(new_file, buf, max_one_file_size, 0);
        MarkBufferDirty(metabuf);
        HnswXlogWriteNewQtCode(index, metapage, metabuf, i, readbyte, buf);
        pfree(buf);
    }
}

static void swap_vecfiles_and_invalid_buffer(BufFile *tmpfile, Relation index, Page metapage,
    Buffer metabuf, size_t invalid_size, QuantizerType qt_type)
{
    RelationOpenSmgr(index);
    int tmp_files_num = BufFileGetNums(tmpfile);
    File *tmpfile_files = BufFileGetFiles(tmpfile);
    for (int i = 0; i < tmp_files_num; ++i) {
        File new_file = tmpfile_files[i];
        char *old_file_path = _mdfd_segpath(index->rd_smgr, VECTOR_FORKNUM, i);
        char *new_file_path = FilePathName(new_file);
        if (durable_rename(new_file_path, old_file_path, ERROR)) {
            ereport(ERROR,
                (errcode_for_file_access(),
                    errmsg("could not rename file \"%s\" to \"%s\": %m", new_file_path, old_file_path)));
        }
        MarkBufferDirty(metabuf);
        HnswXLogBackupSwapAndInvalidBufferCmd(index, metapage, metabuf, i, old_file_path, invalid_size, qt_type);
    }
}

bool handle_quantizer_update_task(Relation index, Relation heap, QuantizerUpdateParam *params)
{
    auto old_ctx = MemoryContextSwitchTo(IndexerCtx);
    QT_UPDATE_LOG("index \"%s\" start quantizer update task", RelationGetRelationName(index));
    QtUpdateMgr *qt_update_mgr = (QtUpdateMgr *)g_instance.annvec_cxt.qt_update_mgr;
    Buffer metabuf = ReadBuffer(index, params->metablkno);
	Page metapage = BufferGetPage(metabuf);
	HnswMetaPage metap = HnswPageGetMeta(metapage);
    QuantizerMetaInfo &qt_metainfo = metap->quantizer_metainfo;
    size_t dim = metap->dimensions;
    Metric metric = metap->metric;
    DistPrecisionType precision_type = metap->precision_type;
    size_t orig_vec_size = dim * VEC_ELEM_SIZE(precision_type);
    bool need_norm = HnswOptionalProcInfo(index, HNSW_NORM_PROC) != NULL;
    size_t code_len = 0;
    bool need_retrain = true;
    size_t scan_num = 0;

    auto run_update = [&](auto &&compute_code, bool force, float *new_centroids, size_t centroids_size) -> void {
        /* scan index and record update data */
        QT_UPDATE_LOG("index \"%s\" quantizer code need retrain, start updating", RelationGetRelationName(index));
        SetConfigOption("temp_file_limit", max_one_file_size_kb, PGC_SUSET, PGC_S_OVERRIDE);
        BufFile *tmpfile = BufFileCreateTemp(false);
        int _file_no = 0;
        off_t _file_offset = 0;
        BufFileTell(tmpfile, &_file_no, &_file_offset);
        /* collect all update/insert data from other thread */
        qt_update_mgr->insert_record(index->rd_id);
        TimestampTz start_scan_time = GetCurrentTimestamp();
        /* scan index */
        BlockNumber blkno = HNSW_HEAD_BLKNO;
        while (BlockNumberIsValid(blkno)) {
            CHECK_FOR_INTERRUPTS();
            Buffer buf = ReadBuffer(index, blkno);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            Page page = BufferGetPage(buf);
            if (HnswPageGetOpaque(page)->page_id != HNSW_PAGE_ID) {
                UnlockReleaseBuffer(buf);
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg("Found incorrect HNSW page opaque page id"),
                    errhint("Index may be corrupted, please rebuild the index")));
            }
            OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);
            for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
                HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
                if (tuple->is_deleted()) {
                    continue;
                }
                uint32 ntid = tuple->ntids();
                ItemPointerData *heaptids = tuple->get_heaptids();
                char *vec = alloc_vector(orig_vec_size);
                char *code = (char *)palloc0(code_len);
                for (uint32 i = 0; i < ntid; ++i) {
                    ItemPointerData htid = heaptids[i];
                    if (fetch_vec_from_heap(index, heap, htid, vec, dim, precision_type)) {
                        if (precision_type == DistPrecisionType::HALF) {
                            float *half2float = alloc_floatvector(dim);
                            halfs_to_floats((half *)vec, half2float, dim);
                            compute_code(half2float, code);
                            free_vector(half2float);
                        } else {
                            compute_code((float *)vec, code);
                        }
                        break;
                    }
                }
                set_buffile(tmpfile, _file_no, code_len * tuple->floatVectorIndex, code, code_len);
                pfree(code);
                free_vector(vec);
                ++scan_num;
            }
            blkno = HnswPageGetOpaque(page)->nextblkno;
            UnlockReleaseBuffer(buf);
        }
        
        /* start blocking all index operation from here */
        LockRelation(index, AccessExclusiveLock);
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

        /* handle colleted data */
        TimestampTz end_scan_time = GetCurrentTimestamp(); /* get end time here, since wait lock for a long time */
        Vector<Slot> *record_vec = qt_update_mgr->erase_record(index->rd_id);
        long secs = 0;
        int microsecs = 0;
        TimestampDifference(start_scan_time, end_scan_time, &secs, &microsecs);
        float freq = 60 * record_vec->size() / (secs + 0.000001);
        if (freq > params->freq_10min * 2 && !force) {
            QT_UPDATE_LOG("index \"%s\" update or insert freq: %f, abnormally increased during "
                          "quantizer code update period, abandon this round of update", 
                          RelationGetRelationName(index), freq);
            record_vec->destroy();
            LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
            UnlockRelation(index, AccessExclusiveLock);
            return;
        }
        QT_UPDATE_LOG("index \"%s\" scan_num: %lu, update or insert record: %lu",
                      RelationGetRelationName(index), scan_num, record_vec->size());
        for (auto s : *record_vec) {
            size_t idx = s.floatVectorIdx;
            float *vec = s.value;
            char *code = (char *)palloc0(code_len);
            compute_code(vec, code);
            set_buffile(tmpfile, _file_no, idx * code_len, code, code_len);
            pfree(code);
            pfree(vec);
        }
        record_vec->destroy();

        /* update code_version */
        ++qt_metainfo.code_version;
        MarkBufferDirty(metabuf);
        HnswXLogUpdateCodeVersion(index, metabuf, metapage, qt_metainfo.code_version);
        /* wal new vec file */
        xlog_write_new_vecfiles(tmpfile, index, metapage, metabuf);
        /* swap local file and send swapcmd to standby; invalid buffer */
        if (params->enable) {
            swap_vecfiles_and_invalid_buffer(tmpfile, index, metapage, metabuf, code_len, params->qt_type);
            vec_invalidate_buffer_cache(index->rd_smgr->smgr_rnode.node.relNode, code_len);
        } else {
            swap_vecfiles_and_invalid_buffer(tmpfile, index, metapage, metabuf, orig_vec_size, params->qt_type);
            vec_invalidate_buffer_cache(index->rd_smgr->smgr_rnode.node.relNode, orig_vec_size);
        }
        bool do_release = BULKBUF_MGR->auto_index_release(index);
        if (params->qt_type == QuantizerType::PQ) {
            diskann_clear_pq_cache(index->rd_id);
        } else {
            clear_rabitq_cache(index->rd_id);
        }
        /* update centroids */
        store_centroids(index, new_centroids, centroids_size, false, !params->enable, params->enable);
        /* update num_new_data*/
        qt_metainfo.num_new_data = 0;
        MarkBufferDirty(metabuf);
        HnswXLogUpdateMetaNumNewData(index, metabuf, metapage, 0);
        /* set quantizer valid flag */
        HnswPQMetaInfo &metainfo = qt_metainfo.get_pq_metainfo(); /* graph_pq and rabitq enabled has same offset */
        metainfo.graph_pq = true;
        MarkBufferDirty(metabuf);
        HnswXLogSetQTValid(index, metapage, metabuf, metainfo.graph_pq);
        /* update centroids_version */
        ++qt_metainfo.centroids_version;
        MarkBufferDirty(metabuf);
        HnswXLogUpdateCentroidsVersion(index, metabuf, metapage, qt_metainfo.centroids_version);
        /* index can be used from here */
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
        UnlockRelation(index, AccessExclusiveLock);
        /* auto load bulk buffer */
        if (do_release) {
            if (params->parent_id == index->rd_id) {
                LockRelation(index, ShareRowExclusiveLock);
                BULKBUF_MGR->index_load(index, InvalidOid);
                UnlockRelation(index, ShareRowExclusiveLock);
            } else {
                Relation parent_index = index_open(params->parent_id, ShareRowExclusiveLock);
                BULKBUF_MGR->index_load(parent_index, index->rd_id);
                index_close(parent_index, ShareRowExclusiveLock);
            }
        }
    };

    /* calculate current center */
    SetConfigOption("maintenance_work_mem", maintenance_work_mem_limit, PGC_USERSET, PGC_S_OVERRIDE);
    QT_UPDATE_LOG("index \"%s\" calculate current center", RelationGetRelationName(index));
    ProductQuantizer *pq = NULL;
    FloatVectorArray samples = quantizer_sample_data(heap, index, dim, need_norm, precision_type, 0, MAX_SAMPLE_VECTOR_NUM);
    if (params->qt_type == QuantizerType::PQ) {
        HnswPQMetaInfo &pq_metainfo = qt_metainfo.get_pq_metainfo();
        size_t m = pq_metainfo.m;
        size_t k = pq_metainfo.k;
        pq = do_kmeans(index, samples, dim, m, k, metric, need_norm, 2);
        if (params->enable) {
            /* read store center */ 
            ProductQuantizer *store_pq = (ProductQuantizer *)palloc(sizeof(ProductQuantizer));
            store_pq->set_basic_values(dim, m, pq_metainfo.nbits());
            hnsw_read_pq_center(index, *store_pq);
            MMDResult mmd_res = mmd_test(pq->centroids, store_pq->centroids, k, dim);
            need_retrain = mmd_res.is_different || params->force;
            if (need_retrain) {
                QT_UPDATE_LOG("index \"%s\" mmd value is %f, p_value is %f, need retrain",
                    RelationGetRelationName(index), mmd_res.mmd_value, mmd_res.p_value);
            } else {
                QT_UPDATE_LOG("index \"%s\" mmd value is %f, p_value is %f, don't need retrain",
                    RelationGetRelationName(index), mmd_res.mmd_value, mmd_res.p_value);
            }
            store_pq->free_resourses();
            pfree(store_pq);
        }
        if (need_retrain) {
            /* setting pq */
            code_len = pq_metainfo.code_size();
            pq->set_fvec_L2sqr_ny_nearest_func();
            FmgrInfo *normprocinfo = HnswOptionalProcInfo(index, HNSW_NORM_PROC);
            if (metap->metric == Metric::INNER_PRODUCT && normprocinfo == NULL) {
                pq->set_fvec_ny_distance_func(Metric::INNER_PRODUCT);
            } else {
                pq->set_fvec_ny_distance_func(Metric::L2);
            }
            /* set compute_code and runs*/
            auto compute_code = [&](float *vec, char *code) -> void {
                pq->compute_code(vec, (uint8 *)code);
            };
            run_update(compute_code, params->force, pq->centroids, dim * k * sizeof(float));
        } else {
            qt_metainfo.num_new_data = 0;
            MarkBufferDirty(metabuf);
            HnswXLogUpdateMetaNumNewData(index, metabuf, metapage, 0);
        }
    } else {
        int padded_dim = RABITQ_PADDED_DIM(dim);
        RaBitQuantizer *quantizer = NEW RaBitQuantizer(dim, padded_dim, metric);
        size_t random_matrix_size = quantizer->get_random_matrix_size();
        size_t centroids_size = HNSW_RABITQ_NUM_CLUSTERS * dim * sizeof(float);
        size_t rotated_centroids_size = HNSW_RABITQ_NUM_CLUSTERS * padded_dim * sizeof(float);
        size_t total_size = random_matrix_size + centroids_size + rotated_centroids_size;
        char *rabitq_data = (char *)palloc0(total_size);

        pq = do_kmeans(index, samples, dim, dim, HNSW_RABITQ_NUM_CLUSTERS, metric, need_norm, 2);
        if (params->enable) {
            /* read store center */
            read_rabitq_data(index, total_size, rabitq_data);
            float *store_centroids = (float *)(rabitq_data + random_matrix_size);
            MMDResult mmd_res = mmd_test(pq->centroids, store_centroids, HNSW_RABITQ_NUM_CLUSTERS, dim);
            need_retrain = mmd_res.is_different || params->force;
            if (need_retrain) {
                QT_UPDATE_LOG("index \"%s\" mmd value is %f, p_value is %f, need retrain",
                    RelationGetRelationName(index), mmd_res.mmd_value, mmd_res.p_value);
            } else {
                QT_UPDATE_LOG("index \"%s\" mmd value is %f, p_value is %f, don't need retrain",
                    RelationGetRelationName(index), mmd_res.mmd_value, mmd_res.p_value);
            }
        }
        if (need_retrain) {
            /* setting rabitq */
            int cid_size = sizeof(uint16);
            int bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
            int ext_size = RABITQ_EXT_DATA_SIZE(padded_dim);
            int quant_size = cid_size + bin_size + ext_size;
            code_len = quant_size;
            /* set newly generated centroids */
            errno_t rc = memcpy_s(quantizer->get_centroids(), centroids_size, pq->centroids, centroids_size);
            securec_check(rc, "\0", "\0");
            /* generate new random matrix and rotated centroids */
            quantizer->train();
            /* assembly rabitq data */
            char *centroids = rabitq_data + random_matrix_size;
            char *rotated_centroids = centroids + centroids_size;
            rc = memcpy_s(rabitq_data, random_matrix_size, quantizer->get_random_matrix(), random_matrix_size);
            securec_check_c(rc, "\0", "\0");
            rc = memcpy_s(centroids, centroids_size, quantizer->get_centroids(), centroids_size);
            securec_check_c(rc, "\0", "\0");
            rc = memcpy_s(rotated_centroids, rotated_centroids_size, quantizer->get_rotated_centroids(), rotated_centroids_size);
            securec_check_c(rc, "\0", "\0");
            /* set compute_code and run */
            auto compute_code = [&](float *vec, char *code) -> void {
                char *bin_data = code + cid_size;
                char *ext_data = bin_data + bin_size;
                int cluster_id = quantizer->quantize(vec, bin_data, ext_data);
                errno_t rc = memcpy_s(code, cid_size, &cluster_id, cid_size);
                securec_check(rc, "\0", "\0");
            };
            run_update(compute_code, params->force, (float *)rabitq_data, total_size);
        } else {
            qt_metainfo.num_new_data = 0;
            MarkBufferDirty(metabuf);
            HnswXLogUpdateMetaNumNewData(index, metabuf, metapage, 0);
        }
        quantizer->destroy();
        delete quantizer;
        pfree(rabitq_data);
    }

    FloatVectorArrayFree(samples);
    pq->free_resourses();
    pfree(pq);
    
    MemoryContextSwitchTo(old_ctx);
    qt_update_mgr->erase_updating(index->rd_id);
    QT_UPDATE_LOG("index \"%s\" quantizer code update finish", RelationGetRelationName(index));
    return true;
}