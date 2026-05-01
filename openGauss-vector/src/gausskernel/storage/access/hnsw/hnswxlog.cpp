/* -------------------------------------------------------------------------
 *
 * hnswxlog.cpp
 *  WAL replay logic for hnsw index.
 *
 * IDENTIFICATION
 *  src/gausskernel/storage/access/nbtree/hnswxlog.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/xlogproc.h"
#include "access/xlogreader.h"
#include "storage/copydir.h"

#include "access/hnsw/hnsw.h"
#include "access/hnsw/hnsw_xlog.h"
#include "storage/buf/bufpage.h"
#include "storage/buf/bufmgr.h"
#include "access/annvector/store/vector_smgr.h"

constexpr int filename_len = 50;

static void HnswRedoBuildIndex(XLogReaderState *record)
{
    for (uint8 block_id = 0; block_id <= XLogRecMaxBlockId(record); ++block_id) {
        RedoBufferInfo buffer;
        if (XLogReadBufferForRedo(record, block_id, &buffer) != BLK_RESTORED) {
            ereport(ERROR,
                (errmsg("unexpected XLogReadBufferForRedo result when restoring backup block")));
        }
        if (BufferIsValid(buffer.buf)) {
            UnlockReleaseBuffer(buffer.buf);
        }
    }
}

static void HnswRedoOverWriteTuple(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_hnsw_overwrite_tuple *xl_rec = (xl_hnsw_overwrite_tuple *)XLogRecGetData(record);
        char *tuple = XLogRecGetBlockData(record, 0, NULL);
        if (!page_index_tuple_overwrite(page, xl_rec->offno, (Item)tuple, xl_rec->tupSize)) {
            ereport(ERROR, (errmsg("hnsw redo failed to add index tuple")));
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void HnswRedoTuple(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        xl_hnsw_tuple_base *base = (xl_hnsw_tuple_base *)XLogRecGetData(record);
        Page page = buffer.pageinfo.page;
        switch (base->type) {
            case xl_hnsw_tuple_base::Types::NEIGHBORS: {
                Size datalen;
                HnswNeighbor neighbors = (HnswNeighbor)XLogRecGetBlockData(record, 0, &datalen);
                OffsetNumber offno = ((xl_hnsw_tuple_neighbors *)base)->offno;
                HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
                errno_t rc = memcpy_s(tuple->neighbors, datalen, neighbors, datalen);
                securec_check(rc, "\0", "\0");
            } break;
            case xl_hnsw_tuple_base::Types::HEAP_TIDS: {
                Size datalen;
                ItemPointer heaptids = (ItemPointer)XLogRecGetBlockData(record, 0, &datalen);
                OffsetNumber offno = ((xl_hnsw_tuple_heaptids *)base)->offno;
                HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
                if (datalen > 0) {
                    errno_t rc = memcpy_s(tuple->heaptids, datalen, heaptids, datalen);
                    securec_check(rc, "\0", "\0");
                }
                uint8 ntid = (uint8)(datalen / sizeof(ItemPointerData));
                tuple->set_ntids(ntid);
            } break;
            case xl_hnsw_tuple_base::Types::MARK_DELETE: {
                OffsetNumber offno = *(OffsetNumber *)XLogRecGetBlockData(record, 0, NULL);
                HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
                tuple->set_deleted();
            } break;
            default:
                ereport(PANIC, (errmsg("hnsw_redo: unknown tuple op code %u", base->type)));
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void HnswRedoAddElement(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_hnsw_add_element *xl_rec = (xl_hnsw_add_element *)XLogRecGetData(record);
        char *tuple = XLogRecGetBlockData(record, 0, NULL);
        if (OffsetNumberIsValid(xl_rec->freeOffno)) {
            if (!page_index_tuple_overwrite(page, xl_rec->offno, (Item) tuple, xl_rec->tupSize)) {
                ereport(PANIC, (errmsg("hnsw index insert redo : failed to overwrite element")));
            }
        } else {
            if (PageAddItem(page, (Item) tuple, xl_rec->tupSize, xl_rec->offno, false, false) == InvalidOffsetNumber) {
                ereport(PANIC, (errmsg("hnsw index insert redo : failed to add element")));
            }
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void HnswRedoUpdateMeta(XLogReaderState *record)
{
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        HNSW_META_XLOG_TYPE type = *(HNSW_META_XLOG_TYPE *)XLogRecGetData(record);
        Page page = buffer.pageinfo.page;
        HnswMetaPage metap = HnswPageGetMeta(page);
        switch (type) {
            case HNSW_META_XLOG_TYPE::UPDATE_NUM_VECTOR:
                metap->num_vectors = *(size_t *)XLogRecGetBlockData(record, 0, NULL);
                break;
            case HNSW_META_XLOG_TYPE::UPDATE_INSERT_PAGE:
                metap->insertPage = *(BlockNumber *)XLogRecGetBlockData(record, 0, NULL);
                break;
            case HNSW_META_XLOG_TYPE::UPDATE_ENTRY_POINT: {
                auto *entry = (HnswEntryPointBase *)XLogRecGetBlockData(record, 0, NULL);
                metap->entryLevel = entry->level;
                metap->entryOffno = entry->offno;
                metap->entryBlkno = entry->blkno;
            } break;
            case HNSW_META_XLOG_TYPE::UPDATE_ALL:
            case HNSW_META_XLOG_TYPE::UPDATE_ALL2: {
                Size datalen;
                char *data = XLogRecGetBlockData(record, 0, &datalen);
                errno_t rc = memcpy_s(metap, datalen, data, datalen);
                securec_check(rc, "\0", "\0");
            } break;
        }
        PageSetLSN(page, record->EndRecPtr);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void HnswRedoWriteNeighbor(XLogReaderState *record)
{
    RedoBufferInfo buffer;
    XLogRecPtr lsn = record->EndRecPtr;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        auto *xl_rec = (xl_hnsw_write_neighbor *)XLogRecGetBlockData(record, 0, NULL);
        HnswTuple ntuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, xl_rec->offno));
        ntuple->neighbors[xl_rec->idx] = xl_rec->data;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void HnswRedoAppendPage(XLogReaderState *record)
{
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) != BLK_RESTORED) {
        ereport(ERROR,
            (errmsg("unexpected XLogReadBufferForRedo result when restoring backup block")));
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void HnswRedoUpdatePageNextBlkno(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        size_t datalen;
        char *blockDelta = XLogRecGetBlockData(record, 0, &datalen);
        Page page = buffer.pageinfo.page;
        errno_t rc = memcpy_s(PageGetSpecialPointer(page), datalen, blockDelta, datalen);
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void HnswRedoAddVector(XLogReaderState *record)
{
    char *data = XLogRecGetData(record);
    xl_hnsw_add_vec *xl_rec = (xl_hnsw_add_vec *)data;
    char *vec = (char *)(data + sizeof(xl_hnsw_add_vec));
    RelFileNode tmp_node;
    RelFileNodeCopy(tmp_node, xl_rec->r_Node, XLogRecGetBucketId(record));

    SMgrRelation smgr = smgropen(tmp_node, InvalidBackendId);
    /* If vector file does not exist, skip it. */
    if (!smgr->md_fd[VECTOR_FORKNUM] && !smgrexists(smgr, VECTOR_FORKNUM)) {
        return;
    }
    vec_write(smgr, xl_rec->offset, xl_rec->nbytes, vec, false, xl_rec->vec_storage_type);
}

static void HnswRedoInvalidateVectorCache(XLogReaderState *record)
{
    xl_invalidate_vector_cache *xl_rec = (xl_invalidate_vector_cache *)XLogRecGetData(record);
    vec_invalidate_buffer_cache(xl_rec->relNode, xl_rec->loc, xl_rec->elem_size);
}

void HnswRedoSwapVecFileAndInvalidBufferCmd(char *s)
{
    xl_hnsw_swapcmd xl_rec = *(xl_hnsw_swapcmd *)s;
    Oid index_id = xl_rec.index_id;
    uint32 file_idx = xl_rec.file_idx;
    int len_old = xl_rec.len_old;
    Oid rel_node = xl_rec.rel_node;
    size_t invalid_size = xl_rec.invalid_size;
    QuantizerType qt_type = xl_rec.qt_type;
    s += sizeof(xl_hnsw_swapcmd);
    FileName old_file_path = s;
    old_file_path[len_old] = '\0';

    FileName new_file_path = (FileName)palloc(filename_len * sizeof(char));
    errno_t rc = sprintf_s(new_file_path, filename_len, "pg_xlog/%u_vec.%u", index_id, file_idx);
    securec_check_ss(rc, "\0", "\0");

    if (!file_exists(new_file_path)) {
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("File \"%s\" does not exist", new_file_path)));
        return;
    }
    if (durable_rename(new_file_path, old_file_path, ERROR)) {
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("could not rename file \"%s\" to \"%s\": %m", new_file_path, old_file_path)));
    }
    QT_UPDATE_LOG("replace \"%s\" to \"%s\" successfully", new_file_path, old_file_path);

    /* invalid buffer */
    vec_invalidate_buffer_cache(rel_node, invalid_size);
    if (qt_type == QuantizerType::PQ) {
        diskann_clear_pq_cache(index_id);
    } else {
        rabitq::clear_rabitq_cache(index_id);
    }
    QT_UPDATE_LOG("invalid vector buffer and successfully");
}

void HnswRedoWriteNewCode(char *s)
{
    xl_hnsw_new_qtcode xl_rec = *(xl_hnsw_new_qtcode *)s;
    Oid index_id = xl_rec.index_id;
    uint32 file_idx = xl_rec.file_idx;
    int nbytes = xl_rec.nbytes;
    s += sizeof(xl_hnsw_new_qtcode);
    char *code = s;

    FileName file_name = (FileName)palloc(filename_len * sizeof(char));
    errno_t rc = sprintf_s(file_name, filename_len, "pg_xlog/%u_vec.%u", index_id, file_idx);
    securec_check_ss(rc, "\0", "\0");
    File fd = PathNameOpenFile(file_name, O_RDWR | O_CREAT | O_DSYNC | PG_BINARY, 0644);
    FilePWrite(fd, code, nbytes, 0);
    FileClose(fd);
    QT_UPDATE_LOG("write temporary quantizer code file \"%s\" successfully", file_name);
    pfree(file_name);
}

static void HnswRedoUpdateQt(XLogReaderState *record)
{
    RedoBufferInfo buffer;
    char *main_data = XLogRecGetData(record);
    HNSW_UPDATE_QT_TYPE type = *(HNSW_UPDATE_QT_TYPE *)main_data;
    main_data += sizeof(HNSW_UPDATE_QT_TYPE);
    if (type == HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS) {
        if (XLogReadBufferForRedo(record, 0, &buffer) != BLK_RESTORED) {
            ereport(ERROR,
                (errmsg("unexpected XLogReadBufferForRedo result when restoring backup block")));
        }
        QT_UPDATE_LOG("replace centroids page successfully");
    } else {
        if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
            Page page = buffer.pageinfo.page;
            HnswMetaPage metap = HnswPageGetMeta(page);
            QuantizerMetaInfo& qt_metainfo = metap->quantizer_metainfo;
            switch (type) {
                case HNSW_UPDATE_QT_TYPE::WRITE_NEWCODE:
                    HnswRedoWriteNewCode(main_data);
                    break;
                case HNSW_UPDATE_QT_TYPE::BACKUP_SWAPCMD:
                    HnswRedoSwapVecFileAndInvalidBufferCmd(main_data);
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_CODE_VERSION:
                    qt_metainfo.code_version = *(uint8 *)XLogRecGetBlockData(record, 0, NULL);
                    QT_UPDATE_LOG("set code version to %hhu", qt_metainfo.code_version);
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_NUM_NEW_DATA:
                    qt_metainfo.num_new_data = *(uint32 *)XLogRecGetBlockData(record, 0, NULL);
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS_VERSION:
                    qt_metainfo.centroids_version = *(uint8 *)XLogRecGetBlockData(record, 0, NULL);
                    QT_UPDATE_LOG("set centroids version to %hhu", qt_metainfo.centroids_version);
                    break;
                case HNSW_UPDATE_QT_TYPE::SET_VALID:
                    qt_metainfo.get_pq_metainfo().graph_pq = true;
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS:
                    break;
            }
            PageSetLSN(page, record->EndRecPtr);
            MarkBufferDirty(buffer.buf);
        }
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void hnsw_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_HNSW_UNLOG_BUILD_INDEX:
            HnswRedoBuildIndex(record);
            break;
        case XLOG_HNSW_BUILD_INDEX:
            HnswRedoBuildIndex(record);
            break;
        case XLOG_HNSW_ADD_HEAP_TID:
            HnswRedoOverWriteTuple(record);
            break;
        case XLOG_HNSW_ADD_ELEMENT:
            HnswRedoAddElement(record);
            break;
        case XLOG_HNSW_META:
            HnswRedoUpdateMeta(record);
            break;
        case XLOG_HNSW_UPDATE_NEIGHBORPAGE:
            HnswRedoOverWriteTuple(record);
            break;
        case XLOG_HNSW_APPEND_PAGE:
            HnswRedoAppendPage(record);
            break;
        case XLOG_HNSW_UPDATE_NEXTBLKNO:
            HnswRedoUpdatePageNextBlkno(record);
            break;
        case XLOG_HNSW_REMOVE_HEAP_TID:
            HnswRedoOverWriteTuple(record);
            break;
        case XLOG_HNSW_REPAIR_GRAPH_ELEMENT:
            HnswRedoOverWriteTuple(record);
            break;
        case XLOG_HNSW_MARK_DELETE:
            HnswRedoOverWriteTuple(record);
            break;
        case XLOG_HNSW_ADD_VECTOR:
            HnswRedoAddVector(record);
            break;
        case XLOG_HNSW_INVALIDATE_VECTOR_CACHE:
            HnswRedoInvalidateVectorCache(record);
            break;
        case XLOG_HNSW_WRITE_NEIGHBOR:
            HnswRedoWriteNeighbor(record);
            break;
        case XLOG_HNSW_TUPLE:
            HnswRedoTuple(record);
            break;
        case XLOG_HNSW_UPDATE_QT:
            HnswRedoUpdateQt(record);
            break;
        default:
            ereport(PANIC, (errmsg("hnsw_redo: unknown op code %u", info)));
    }
}

void HnswXLogAppendPage(Relation index, Buffer buffer, Page page)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    MarkBufferDirty(buffer);
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
    XLogRecPtr recptr = XLogInsert(RM_HNSW_ID, XLOG_HNSW_APPEND_PAGE);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void HnswXLogAddVector(Relation index, char *value, off_t offset, int nbytes, VecStorageType vec_storage_type)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_add_vec xl_rec;
    RelFileNodeRelCopy(xl_rec.r_Node, index->rd_node);
    xl_rec.offset = offset;
    xl_rec.nbytes = nbytes;
    xl_rec.vec_storage_type = vec_storage_type;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_hnsw_add_vec));
    XLogRegisterData(value, nbytes);
    XLogInsert(RM_HNSW_ID, XLOG_HNSW_ADD_VECTOR);
    END_CRIT_SECTION();
}

void HnswXLogUpdateNextBlkno(Relation index, Buffer buffer, BlockNumber blkno)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    Page page = BufferGetPage(buffer);
    MarkBufferDirty(buffer);
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&blkno, sizeof(BlockNumber));
    XLogRecPtr recptr = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_NEXTBLKNO);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void HnswXLogMarkDelete(Relation index, OffsetNumber offno, Buffer buf, Page page)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_tuple_mark_delete xl;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl, sizeof(xl_hnsw_tuple_mark_delete));
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&offno, sizeof(OffsetNumber));
    XLogRecPtr recptr = XLogInsert(RM_HNSW_ID, XLOG_HNSW_TUPLE);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void HnswXLogWriteNeighbors(Relation index, HnswNeighbor neighbor, uint32 n, OffsetNumber offno, Buffer buf, Page page)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_tuple_neighbors xl(offno);
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl, sizeof(xl_hnsw_tuple_neighbors));
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)neighbor, sizeof(HnswNeighborData) * n);
    XLogRecPtr recptr = XLogInsert(RM_HNSW_ID, XLOG_HNSW_TUPLE);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
    
}

void HnswXLogWriteNeighbor(Relation index, HnswNeighbor neighbor, int idx, OffsetNumber offno,
    Buffer buf, Page page)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_write_neighbor xl_rec;
    xl_rec.data = *neighbor;
    xl_rec.idx = (uint16)idx;
    xl_rec.offno = offno;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&xl_rec, sizeof(xl_hnsw_write_neighbor));
    XLogRecPtr recptr = XLogInsert(RM_HNSW_ID, XLOG_HNSW_WRITE_NEIGHBOR);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
    
}

void HnswXLogUpdateHeaptid(Relation index, ItemPointer heaptids, uint8 ntid, OffsetNumber offno,
    Buffer buf, Page page)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_tuple_heaptids xl(offno);
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl, sizeof(xl_hnsw_tuple_heaptids));
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)heaptids, sizeof(ItemPointerData) * ntid);
    XLogRecPtr recptr = XLogInsert(RM_HNSW_ID, XLOG_HNSW_TUPLE);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void HnswXLogAddElement(Relation index, Size tupSize, OffsetNumber freeOffno, OffsetNumber offno,
    Item tuple, Buffer buf, Page page)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_add_element xl_rec;
    xl_rec.tupSize = tupSize;
    xl_rec.freeOffno = freeOffno;
    xl_rec.offno = offno;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_hnsw_add_element));
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, tuple, tupSize);
    XLogRecPtr recptr = XLogInsert(RM_HNSW_ID, XLOG_HNSW_ADD_ELEMENT);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void HnswXLogUpdateMetaNumVector(Relation index, Buffer buf, Page page, size_t num_vector)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_META_XLOG_TYPE type = HNSW_META_XLOG_TYPE::UPDATE_NUM_VECTOR;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(type));
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&num_vector, sizeof(size_t));
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_META);
    PageSetLSN(page, lsn);
    END_CRIT_SECTION();
}

void HnswXLogUpdateMetaInsertPage(Relation index, Buffer buf, Page page, BlockNumber insert_blkno)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_META_XLOG_TYPE type = HNSW_META_XLOG_TYPE::UPDATE_INSERT_PAGE;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(type));
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&insert_blkno, sizeof(BlockNumber));
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_META);
    PageSetLSN(page, lsn);
    END_CRIT_SECTION();
}

void HnswXLogUpdateMetaEntry(Relation index, Buffer buf, Page page, HnswEntryPointBase entry)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_META_XLOG_TYPE type = HNSW_META_XLOG_TYPE::UPDATE_ENTRY_POINT;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(type));
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&entry, sizeof(HnswEntryPointBase));
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_META);
    PageSetLSN(page, lsn);
    END_CRIT_SECTION();
}

void HnswXLogUpdateMetaNumNewData(Relation index, Buffer metabuf, Page metapage, uint32 num_new_data)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_UPDATE_QT_TYPE type = HNSW_UPDATE_QT_TYPE::UPDATE_NUM_NEW_DATA;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(HNSW_UPDATE_QT_TYPE));
    XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&num_new_data, sizeof(uint32));
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_QT);
    PageSetLSN(metapage, lsn);
    END_CRIT_SECTION();
}

void HnswXLogUpdateCodeVersion(Relation index, Buffer metabuf, Page metapage, uint8 code_version)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_UPDATE_QT_TYPE type = HNSW_UPDATE_QT_TYPE::UPDATE_CODE_VERSION;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(HNSW_UPDATE_QT_TYPE));
    XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&code_version, sizeof(uint8));
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_QT);
    PageSetLSN(metapage, lsn);
    XLogWaitFlush(lsn);
    END_CRIT_SECTION();
}

void HnswXLogUpdateCentroidsVersion(Relation index, Buffer metabuf, Page metapage, uint8 centroids_version)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_UPDATE_QT_TYPE type = HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS_VERSION;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(HNSW_UPDATE_QT_TYPE));
    XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *)&centroids_version, sizeof(uint8));
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_QT);
    PageSetLSN(metapage, lsn);
    XLogWaitFlush(lsn);
    END_CRIT_SECTION();
}

void HnswXLogWriteCentroids(Relation index, Buffer metabuf, Page metapage)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_UPDATE_QT_TYPE type = HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(HNSW_UPDATE_QT_TYPE));
    XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_QT);
    PageSetLSN(metapage, lsn);
    XLogWaitFlush(lsn);
    END_CRIT_SECTION();
}

void HnswXlogWriteNewQtCode(Relation index, Page metapage, Buffer metabuf, uint32 file_idx, int nbytes, char *code)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_new_qtcode xl_rec;
    xl_rec.index_id = index->rd_id;
    xl_rec.file_idx = file_idx;
    xl_rec.nbytes = nbytes;

    constexpr HNSW_UPDATE_QT_TYPE type = HNSW_UPDATE_QT_TYPE::WRITE_NEWCODE;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(HNSW_UPDATE_QT_TYPE));
    XLogRegisterData((char *)&xl_rec, sizeof(xl_hnsw_new_qtcode));
    XLogRegisterData(code, nbytes);
    XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_QT);
    PageSetLSN(metapage, lsn);
    XLogWaitFlush(lsn);
    END_CRIT_SECTION();
}

void HnswXLogBackupSwapAndInvalidBufferCmd(Relation index, Page metapage, Buffer metabuf,
    uint32 file_idx, char *old_file_path, size_t invalid_size, QuantizerType qt_type)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    xl_hnsw_swapcmd xl_rec;
    xl_rec.index_id = index->rd_id;
    xl_rec.file_idx = file_idx;
    size_t len_old = strlen(old_file_path);
    xl_rec.len_old = len_old;
    xl_rec.rel_node = index->rd_smgr->smgr_rnode.node.relNode;
    xl_rec.invalid_size = invalid_size;
    xl_rec.qt_type = qt_type;

    constexpr HNSW_UPDATE_QT_TYPE type = HNSW_UPDATE_QT_TYPE::BACKUP_SWAPCMD;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(HNSW_UPDATE_QT_TYPE));
    XLogRegisterData((char *)&xl_rec, sizeof(xl_hnsw_swapcmd));
    XLogRegisterData(old_file_path, len_old);
    XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_QT);
    PageSetLSN(metapage, lsn);
    XLogWaitFlush(lsn);
    END_CRIT_SECTION();
}

void HnswXLogSetQTValid(Relation index, Page metapage, Buffer metabuf, bool valid)
{
    if (!RelationNeedsWAL(index)) {
        return;
    }

    constexpr HNSW_UPDATE_QT_TYPE type = HNSW_UPDATE_QT_TYPE::SET_VALID;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&type, sizeof(HNSW_UPDATE_QT_TYPE));
    XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *)&valid, sizeof(bool));
    XLogRecPtr lsn = XLogInsert(RM_HNSW_ID, XLOG_HNSW_UPDATE_QT);
    PageSetLSN(metapage, lsn);
    XLogWaitFlush(lsn);
    END_CRIT_SECTION();
}
