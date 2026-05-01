 /*
 * redo_diskannxlog.cpp
 *    parse diskann xlog
 *
 * IDENTIFICATION
 *
 * src/gausskernel/storage/access/redo/redo_diskannxlog.cpp
 *
 */

#include <vtl/disk_container/diskvector.hpp>

#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "access/xlogproc.h"
#include "utils/memutils.h"
#include "access/diskann/diskann.h"
#include "access/annvector/store/vector_smgr.h"

using namespace disk_container;

typedef enum {
    DISKANN_FULL_IMAGE_PAGE_BLOCK_NUM = 0,
} XLogDiskannFullImagePageEnum;

typedef enum {
    DISKANN_SET_ELEM_BLOCK_NUM = 0,
} XLogDiskannSetElemEnum;

typedef enum {
    DISKANN_UPDATE_META_START_NPAGES_BLOCK_NUM = 0,
} XLogDiskannUpdateMetaStartNpagesEnum;

typedef enum {
    DISKANN_INPLACE_FILTER_ADD_DATA_BLOCK_NUM = 0,
} XLogDiskannInplaceFilterAddDataEnum;

typedef enum {
    DISKANN_INPLACE_FILTER_ADD_ITEM_BLOCK_NUM = 0,
} XLogDiskannInplaceFilterAddItemEnum;

typedef enum {
    DISKANN_INPLACE_FILTER_DELETE_ITEM_BLOCK_NUM = 0,
} XLogDiskannInplaceFilterDeleteItemEnum;

typedef enum {
    DISKANN_INPLACE_FILTER_MULTI_DELETE_BLOCK_NUM = 0,
} XLogDiskannInplaceFilterMultiDeleteEnum;

typedef enum {
    DISKANN_UPDATE_META_NITEM_BLOCK_NUM = 0,
} XLogDiskannUpdateMetaNitemEnum;

static XLogRecParseState *DiskannXlogFullImagePageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_FULL_IMAGE_PAGE_BLOCK_NUM, recordstatehead);
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

static XLogRecParseState *DiskannXlogSetElemParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_SET_ELEM_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *DiskannUpdateMetaStartNpagesParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_UPDATE_META_START_NPAGES_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *DiskInplaceFilterAddDataParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_INPLACE_FILTER_ADD_DATA_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *DiskInplaceFilterAddItemParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_INPLACE_FILTER_ADD_ITEM_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *DiskInplaceFilterDeleteItemParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_INPLACE_FILTER_DELETE_ITEM_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *DiskInplaceFilterMultiDeleteParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_INPLACE_FILTER_MULTI_DELETE_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *DiskannUpdateMetaNitemParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, DISKANN_UPDATE_META_NITEM_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *DiskannAddVectorParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    (*blocknum)++;

    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    if (recordstatehead == NULL) {
        return NULL;
    }

    RelFileNode tmp_node;
    xl_ann_add_vector *xl_rec = (xl_ann_add_vector *)XLogRecGetData(record);
    RelFileNodeCopy(tmp_node, xl_rec->r_Node, XLogRecGetBucketId(record));
    RelFileNodeForkNum filenode = RelFileNodeForkNumFill(&tmp_node, InvalidBackendId, VECTOR_FORKNUM, InvalidBlockNumber);
    XLogRecSetBlockCommonState(record, BLOCK_DATA_VECTOR_TYPE, filenode, recordstatehead);
    recordstatehead->blockparse.extra_rec.blockvectorec.mainData = (char *)xl_rec;
    recordstatehead->blockparse.extra_rec.blockvectorec.datalen = sizeof(xl_ann_add_vector) + xl_rec->nbytes;
    recordstatehead->isFullSync = record->isFullSync;
    return recordstatehead;
}

static XLogRecParseState *DiskannInvalidateVectorCache(XLogReaderState *record, uint32 *blocknum)
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
    RelFileNodeForkNum filenode = RelFileNodeForkNumFill(&tmp_node, InvalidBackendId, VECTOR_FORKNUM, InvalidBlockNumber);

    XLogRecSetBlockCommonState(record, BLOCK_DATA_VECTOR_TYPE, filenode, recordstatehead);
    recordstatehead->blockparse.extra_rec.blockvectorec.mainData = (char *)xl_rec;
    recordstatehead->blockparse.extra_rec.blockvectorec.datalen = sizeof(xl_invalidate_vector_cache);
    recordstatehead->isFullSync = record->isFullSync;
    return recordstatehead;
}

XLogRecParseState *DiskannRedoParseToBlock(XLogReaderState *record, uint32 *blocknum)
{
    *blocknum = 0;
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    XLogRecParseState *recordblockstate = NULL;

    switch (info) {
        case XLOG_DISKANN_BUILD_INDEX:
            recordblockstate = DiskannXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_UNLOG_BUILD_INDEX:
            recordblockstate = DiskannXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_SET_ELEM:
            recordblockstate = DiskannXlogSetElemParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_EXTEND_NEWPAGES:
            recordblockstate = DiskannXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_UPDATE_META_START_NPAGES:
            recordblockstate = DiskannUpdateMetaStartNpagesParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_UPDATE_META_NITEM:
            recordblockstate = DiskannUpdateMetaNitemParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_ADD_VECTOR:
            recordblockstate = DiskannAddVectorParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_DATA:
            recordblockstate = DiskInplaceFilterAddDataParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM:
            recordblockstate = DiskInplaceFilterAddItemParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM:
            recordblockstate = DiskInplaceFilterDeleteItemParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE:
            recordblockstate = DiskInplaceFilterMultiDeleteParseBlock(record, blocknum);
            break;
        case XLOG_DISKANN_INVALIDATE_VECTOR_CACHE:
            recordblockstate = DiskannInvalidateVectorCache(record, blocknum);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("DiskannRedoParseToBlock: unknown op code %u", info)));
    }

    return recordblockstate;
}

void DiskannRedoFullImagePageBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action != BLK_RESTORED) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("DiskannRedoFullImagePageBlock did not contain a full-page image of %u page",
                               XLogBlockDataGetBlockId(datadecode))));
    }
    MakeRedoBufferDirty(bufferinfo);
}

void DiskannRedoSetElemBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        xl_diskann_add_elem *xl_rec = (xl_diskann_add_elem *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->elem_size, data, xl_rec->elem_size);
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void DiskannRedoUpdateMetaStartNpagesBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        xl_diskann_update_start_npages *xl_rec = (xl_diskann_update_start_npages *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        DiskVectorMetaPage meta = reinterpret_cast<DiskVectorMetaPage>(PageGetContents(page));
        meta->npage = xl_rec->num_pages;
        meta->item_start_pages[xl_rec->num_pages - 1] = xl_rec->start_blkno;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void DiskannRedoInplaceFilterAddDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        xl_diskann_inplace_filter_add_data *xl_rec = (xl_diskann_inplace_filter_add_data *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->data_size, data, xl_rec->data_size);
        securec_check(rc, "\0", "\0");

        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void DiskannRedoInplaceFilterAddItemBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        xl_diskann_inplace_filter_add_item *xl_rec = (xl_diskann_inplace_filter_add_item *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        if (xl_rec->isOverWrite) {
            if(!page_index_tuple_overwrite(page, xl_rec->offset, (Item)data, xl_rec->size)) {
                ereport(PANIC, (errmsg("diskann inplace filter add item redo : failed to overwrite item")));
            }
        } else {
            if (PageAddItem(page, (Item)data, xl_rec->size, xl_rec->offset, false, false) == InvalidOffsetNumber) {
                ereport(PANIC, (errmsg("diskann inplace filter add item redo : failed to add item")));
            }
        }
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void DiskannRedoInplaceFilterDeleteItemBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        OffsetNumber *offset = (OffsetNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
        PageIndexTupleDelete(page, *offset);
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void DIskannRedoInplaceFilterMultiDeleteBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        size_t *n = (size_t *)XLogBlockDataGetMainData(datadecode, NULL);
        OffsetNumber *offsets = (OffsetNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
        PageIndexMultiDelete(page, offsets, *n);
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void DiskannRedoUpdateMetaNitemBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        size_t *nitem = (size_t *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        DiskVectorMetaPage meta = reinterpret_cast<DiskVectorMetaPage>(PageGetContents(page));
        meta->nitem = *nitem;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void DiskannRedoDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    uint8 info = XLogBlockHeadGetInfo(blockhead) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_DISKANN_BUILD_INDEX:
            DiskannRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_UNLOG_BUILD_INDEX:
            DiskannRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_SET_ELEM:
            DiskannRedoSetElemBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_EXTEND_NEWPAGES:
            DiskannRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_UPDATE_META_START_NPAGES:
            DiskannRedoUpdateMetaStartNpagesBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_DATA:
            DiskannRedoInplaceFilterAddDataBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM:
            DiskannRedoInplaceFilterAddItemBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM:
            DiskannRedoInplaceFilterDeleteItemBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE:
            DIskannRedoInplaceFilterMultiDeleteBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_DISKANN_UPDATE_META_NITEM:
            DiskannRedoUpdateMetaNitemBlock(blockhead, blockdatarec, bufferinfo);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("DiskannRedoDataBlock: unknown op code %u", info)));
    }
}