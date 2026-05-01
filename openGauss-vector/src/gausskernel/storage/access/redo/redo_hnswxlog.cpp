 /*
 * redo_hnswxlog.cpp
 *    parse hnsw xlog
 *
 * IDENTIFICATION
 *
 * src/gausskernel/storage/access/redo/redo_hnswxlog.cpp
 *
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "access/xlogproc.h"
#include "utils/memutils.h"
#include "storage/smgr/fd.h"
#include "access/hnsw/hnsw_xlog.h"
#include "access/annvector/store/vector_smgr.h"

typedef enum {
    HNSW_FULL_IMAGE_PAGE_BLOCK_NUM = 0,
} XLogHnswFullImagePageEnum;

static XLogRecParseState *HnswXlogFullImagePageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HNSW_FULL_IMAGE_PAGE_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    int maxBlockId = XLogRecMaxBlockId(record);
    for (int blockId = 0; blockId < maxBlockId; blockId++) {
        XLogRecParseState *blockstate = NULL;
        XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
        XLogRecSetBlockDataState(record, blockId + 1, blockstate);
        ++(*blocknum);
    }

    return recordstatehead;
}

static XLogRecParseState *HnswXlogSinglePageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, 0, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    ++(*blocknum);
    return recordstatehead;
}

static XLogRecParseState *HnswXlogAddVectorParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    (*blocknum)++;

    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    if (recordstatehead == NULL) {
        return NULL;
    }

    RelFileNode tmp_node;
    xl_hnsw_add_vec *xl_rec = (xl_hnsw_add_vec *)XLogRecGetData(record);
    RelFileNodeCopy(tmp_node, xl_rec->r_Node, XLogRecGetBucketId(record));
    RelFileNodeForkNum filenode =
        RelFileNodeForkNumFill(&tmp_node, InvalidBackendId, VECTOR_FORKNUM, InvalidBlockNumber);
    XLogRecSetBlockCommonState(record, BLOCK_DATA_HNSW_VECTOR_TYPE, filenode, recordstatehead);
    recordstatehead->blockparse.extra_rec.blockvectorec.mainData = XLogRecGetData(record);
    recordstatehead->blockparse.extra_rec.blockvectorec.datalen =
        sizeof(xl_hnsw_add_vec) + ((xl_hnsw_add_vec *)XLogRecGetData(record))->nbytes;
    recordstatehead->isFullSync = record->isFullSync;
    return recordstatehead;
}

static XLogRecParseState *HnswXlogInvalidateVectorCacheParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    (*blocknum)++;

    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    if (recordstatehead == NULL) {
        return NULL;
    }

    RelFileNode tmp_node;
    xl_invalidate_vector_cache *xl_rec = (xl_invalidate_vector_cache *)XLogRecGetData(record);
    RelFileNodeCopy(tmp_node, xl_rec->r_Node, XLogRecGetBucketId(record));
    RelFileNodeForkNum filenode =
        RelFileNodeForkNumFill(&tmp_node, InvalidBackendId, VECTOR_FORKNUM, InvalidBlockNumber);
    XLogRecSetBlockCommonState(record, BLOCK_DATA_HNSW_VECTOR_TYPE, filenode, recordstatehead);
    recordstatehead->blockparse.extra_rec.blockvectorec.mainData = XLogRecGetData(record);
    recordstatehead->blockparse.extra_rec.blockvectorec.datalen = sizeof(xl_invalidate_vector_cache);
    recordstatehead->isFullSync = record->isFullSync;
    return recordstatehead;
}

XLogRecParseState *HnswRedoParseToBlock(XLogReaderState *record, uint32 *blocknum)
{
    *blocknum = 0;
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    XLogRecParseState *recordblockstate = NULL;

    switch (info) {
        case XLOG_HNSW_UNLOG_BUILD_INDEX:
            recordblockstate = HnswXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_HNSW_BUILD_INDEX:
            recordblockstate = HnswXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_HNSW_ADD_HEAP_TID:
        case XLOG_HNSW_ADD_ELEMENT:
        case XLOG_HNSW_META:
        case XLOG_HNSW_UPDATE_NEIGHBORPAGE:
        case XLOG_HNSW_APPEND_PAGE:
        case XLOG_HNSW_UPDATE_NEXTBLKNO:
        case XLOG_HNSW_REMOVE_HEAP_TID:
        case XLOG_HNSW_REPAIR_GRAPH_ELEMENT:
        case XLOG_HNSW_MARK_DELETE:
        case XLOG_HNSW_WRITE_NEIGHBOR:
        case XLOG_HNSW_TUPLE:
        case XLOG_HNSW_UPDATE_QT:
            recordblockstate = HnswXlogSinglePageParseBlock(record, blocknum);
            break;
        case XLOG_HNSW_ADD_VECTOR:
            recordblockstate = HnswXlogAddVectorParseBlock(record, blocknum);
            break;
        case XLOG_HNSW_INVALIDATE_VECTOR_CACHE:
            recordblockstate = HnswXlogInvalidateVectorCacheParseBlock(record, blocknum);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("HnswRedoParseToBlock: unknown op code %u", info)));
    }

    return recordblockstate;
}

static void HnswRedoFullImagePageBlock(XLogBlockHead *blockhead, XLogBlockDataParse *datadecode,
                                       RedoBufferInfo *bufferinfo)
{
    if (XLogCheckBlockDataRedoAction(datadecode, bufferinfo) != BLK_RESTORED) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
            errmsg("HnswRedoFullImagePageBlock did not contain a full-page image of %u page",
                   XLogBlockDataGetBlockId(datadecode))));
    }
    MakeRedoBufferDirty(bufferinfo);
}

static void HnswRedoFullTupleBlock(XLogBlockHead *blockhead, XLogBlockDataParse *datadecode,
                                   RedoBufferInfo *bufferinfo)
{
    if (XLogCheckBlockDataRedoAction(datadecode, bufferinfo) != BLK_NEEDS_REDO) {
        return;
    }

    char *tuple = XLogBlockDataGetBlockData(datadecode, NULL);
    xl_hnsw_overwrite_tuple *xl_rec =
        (xl_hnsw_overwrite_tuple *)XLogBlockDataGetMainData(datadecode, NULL);
    Page page = bufferinfo->pageinfo.page;
    if (!page_index_tuple_overwrite(page, xl_rec->offno, (Item)tuple, xl_rec->tupSize)) {
        ereport(ERROR, (errmsg("hnsw redo failed to overwrite a tuple")));
    }
    PageSetLSN(page, bufferinfo->lsn);
    MakeRedoBufferDirty(bufferinfo);
}

static void HnswRedoAddElementBlock(XLogBlockHead *blockhead, XLogBlockDataParse *datadecode,
                                    RedoBufferInfo *bufferinfo)
{
    if (XLogCheckBlockDataRedoAction(datadecode, bufferinfo) != BLK_NEEDS_REDO) {
        return;
    }

    Page page = bufferinfo->pageinfo.page;
    xl_hnsw_add_element *xl_rec = (xl_hnsw_add_element *)XLogBlockDataGetMainData(datadecode, NULL);
    char *tuple = XLogBlockDataGetBlockData(datadecode, NULL);

    if (OffsetNumberIsValid(xl_rec->freeOffno)) {
        if(!page_index_tuple_overwrite(page, xl_rec->offno, (Item) tuple, xl_rec->tupSize)) {
            ereport(PANIC, (errmsg("hnsw index insert redo : failed to add element")));
        }
    } else {
        if (PageAddItem(page, (Item) tuple, xl_rec->tupSize, xl_rec->offno, false, false)  == InvalidOffsetNumber) {
            ereport(PANIC, (errmsg("hnsw index insert redo : failed to add element")));
        }
    }
    PageSetLSN(page, bufferinfo->lsn);
    MakeRedoBufferDirty(bufferinfo);
}

static void HnswRedoUpdateMetaBlock(XLogBlockHead *blockhead, XLogBlockDataParse *datadecode,
                                    RedoBufferInfo *bufferinfo)
{
    if (XLogCheckBlockDataRedoAction(datadecode, bufferinfo) != BLK_NEEDS_REDO) {
        return;
    }

    HNSW_META_XLOG_TYPE type = *(HNSW_META_XLOG_TYPE *)XLogBlockDataGetMainData(datadecode, NULL);
    Page page = bufferinfo->pageinfo.page;
    HnswMetaPage metap = HnswPageGetMeta(page);
    switch (type) {
        case HNSW_META_XLOG_TYPE::UPDATE_NUM_VECTOR:
            metap->num_vectors = *(size_t *)XLogBlockDataGetBlockData(datadecode, NULL);
            break;
        case HNSW_META_XLOG_TYPE::UPDATE_INSERT_PAGE:
            metap->insertPage = *(BlockNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
            break;
        case HNSW_META_XLOG_TYPE::UPDATE_ENTRY_POINT: {
            auto *entry = (HnswEntryPointBase *)XLogBlockDataGetBlockData(datadecode, NULL);
            metap->entryLevel = entry->level;
            metap->entryOffno = entry->offno;
            metap->entryBlkno = entry->blkno;
        } break;
        case HNSW_META_XLOG_TYPE::UPDATE_ALL:
        case HNSW_META_XLOG_TYPE::UPDATE_ALL2: {
            Size datalen;
            char *data = XLogBlockDataGetBlockData(datadecode, &datalen);
            errno_t rc = memcpy_s(metap, datalen, data, datalen);
            securec_check(rc, "\0", "\0");
        } break;
    }
    PageSetLSN(page, bufferinfo->lsn);
    MakeRedoBufferDirty(bufferinfo);
}

static void HnswRedoWriteNeighborBlock(XLogBlockHead *blockhead, XLogBlockDataParse *datadecode,
                                       RedoBufferInfo *bufferinfo)
{
    if (XLogCheckBlockDataRedoAction(datadecode, bufferinfo) != BLK_NEEDS_REDO) {
        return;
    }

    xl_hnsw_write_neighbor *xl_rec = (xl_hnsw_write_neighbor *)XLogBlockDataGetMainData(datadecode, NULL);
    Page page = bufferinfo->pageinfo.page;
    HnswTuple ntuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, xl_rec->offno));
    ntuple->neighbors[xl_rec->idx] = xl_rec->data;
    PageSetLSN(page, bufferinfo->lsn);
    MakeRedoBufferDirty(bufferinfo);
}

static void HnswRedoUpdateNextblknoBlock(XLogBlockHead *blockhead, XLogBlockDataParse *datadecode,
                                         RedoBufferInfo *bufferinfo)
{
    if (XLogCheckBlockDataRedoAction(datadecode, bufferinfo) != BLK_NEEDS_REDO) {
        return;
    }

    Size datalen;
    char *data = XLogBlockDataGetBlockData(datadecode, &datalen);
    Page page = bufferinfo->pageinfo.page;
    errno_t rc = memcpy_s(PageGetSpecialPointer(page), datalen, data, datalen);
    securec_check(rc, "\0", "\0");
    PageSetLSN(page, bufferinfo->lsn);
    MakeRedoBufferDirty(bufferinfo);
}

static void HnswRedoTupleBlock(XLogBlockHead *blockhead, XLogBlockDataParse *datadecode,
                               RedoBufferInfo *bufferinfo)
{
    if (XLogCheckBlockDataRedoAction(datadecode, bufferinfo) != BLK_NEEDS_REDO) {
        return;
    }

    xl_hnsw_tuple_base *base = (xl_hnsw_tuple_base *)XLogBlockDataGetMainData(datadecode, NULL);
    Page page = bufferinfo->pageinfo.page;
    switch (base->type) {
        case xl_hnsw_tuple_base::Types::NEIGHBORS: {
            Size datalen;
            HnswNeighbor neighbors = (HnswNeighbor)XLogBlockDataGetBlockData(datadecode, &datalen);
            OffsetNumber offno = ((xl_hnsw_tuple_neighbors *)base)->offno;
            HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
            errno_t rc = memcpy_s(tuple->neighbors, datalen, neighbors, datalen);
            securec_check(rc, "\0", "\0");
        } break;
        case xl_hnsw_tuple_base::Types::HEAP_TIDS: {
            Size datalen;
            ItemPointer heaptids = (ItemPointer)XLogBlockDataGetBlockData(datadecode, &datalen);
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
            OffsetNumber offno = *(OffsetNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
            HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
            tuple->set_deleted();
        } break;
        default:
            ereport(PANIC, (errmsg("hnsw_redo: unknown tuple op code %u", base->type)));
    }
    PageSetLSN(page, bufferinfo->lsn);
    MakeRedoBufferDirty(bufferinfo);
}

void HnswRedoQtBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    char *main_data = XLogBlockDataGetMainData(datadecode, NULL);
    HNSW_UPDATE_QT_TYPE type = *(HNSW_UPDATE_QT_TYPE *)main_data;
    main_data += sizeof(HNSW_UPDATE_QT_TYPE);
    if (type == HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS) {
        HnswRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
    } else {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            Page page = bufferinfo->pageinfo.page;
            HnswMetaPage metap = HnswPageGetMeta(page);
            QuantizerMetaInfo &qt_metainfo = metap->quantizer_metainfo;
            switch (type) {
                case HNSW_UPDATE_QT_TYPE::WRITE_NEWCODE:
                    HnswRedoWriteNewCode(main_data);
                    break;
                case HNSW_UPDATE_QT_TYPE::BACKUP_SWAPCMD:
                    HnswRedoSwapVecFileAndInvalidBufferCmd(main_data);
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_CODE_VERSION:
                    qt_metainfo.code_version = *(uint8 *)XLogBlockDataGetBlockData(datadecode, NULL);
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_NUM_NEW_DATA:
                    qt_metainfo.num_new_data = *(uint32 *)XLogBlockDataGetBlockData(datadecode, NULL);
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS_VERSION:
                    qt_metainfo.centroids_version = *(uint8 *)XLogBlockDataGetBlockData(datadecode, NULL);
                    break;
                case HNSW_UPDATE_QT_TYPE::SET_VALID:
                    qt_metainfo.get_pq_metainfo().graph_pq = *(bool *)XLogBlockDataGetBlockData(datadecode, NULL);
                    break;
                case HNSW_UPDATE_QT_TYPE::UPDATE_CENTROIDS:
                    /* keep compiler happy */
                    break;
            }
            PageSetLSN(page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    }
}

void HnswRedoDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec,
                       RedoBufferInfo *bufferinfo)
{
    uint8 info = XLogBlockHeadGetInfo(blockhead) & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_HNSW_UNLOG_BUILD_INDEX:
            HnswRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_BUILD_INDEX:
            HnswRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_ADD_ELEMENT:
            HnswRedoAddElementBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_META:
            HnswRedoUpdateMetaBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_UPDATE_NEIGHBORPAGE:
            HnswRedoFullTupleBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_APPEND_PAGE:
            HnswRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_UPDATE_NEXTBLKNO:
            HnswRedoUpdateNextblknoBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_REMOVE_HEAP_TID:
            HnswRedoFullTupleBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_REPAIR_GRAPH_ELEMENT:
            HnswRedoFullTupleBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_MARK_DELETE:
            HnswRedoFullTupleBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_WRITE_NEIGHBOR:
            HnswRedoWriteNeighborBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_TUPLE:
            HnswRedoTupleBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HNSW_UPDATE_QT:
            HnswRedoQtBlock(blockhead, blockdatarec, bufferinfo);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("HnswRedoDataBlock: unknown op code %u", info)));
    }
}
