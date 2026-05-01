/*
 * redo_ivfxlog.cpp
 *    parse ivf xlog
 *
 * IDENTIFICATION
 *
 * src/gausskernel/storage/access/redo/redo_ivfxlog.cpp
 *
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "access/xlogproc.h"
#include "utils/memutils.h"
#include "access/annvector/ivf.h"

typedef enum {
    IVF_FULL_IMAGE_PAGE_BLOCK_NUM = 0,
} XLogIvfFullImagePageEnum;

typedef enum {
    IVF_INSERT_BLOCK_NUM = 0,
} XLogIvfInsertEnum;

typedef enum {
    IVF_DELETE_BLOCK_NUM = 0,
} XLogIvfDeleteEnum;

typedef enum {
    IVF_UPDATE_LIST_BLOCK_NUM = 0,
} XLogIvfUpdateListEnum;

typedef enum {
    IVF_UPDATE_NEXTBLKNO_BLOCK_NUM = 0,
} XLogIvfUpdateNextblknoEnum;

static XLogRecParseState *IvfXlogFullImagePageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, IVF_FULL_IMAGE_PAGE_BLOCK_NUM, recordstatehead);
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

static XLogRecParseState *IvfXlogInsertParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, IVF_INSERT_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *IvfXlogDeleteParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, IVF_DELETE_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *IvfXlogUpdateListParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, IVF_UPDATE_LIST_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *IvfXlogUpdatePageNextBlknoParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, IVF_UPDATE_NEXTBLKNO_BLOCK_NUM, recordstatehead, BLOCK_DATA_MAIN_DATA_TYPE, true);
    *blocknum = 1;

    return recordstatehead;
}

XLogRecParseState *IvfRedoParseToBlock(XLogReaderState *record, uint32 *blocknum)
{
    *blocknum = 0;
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    XLogRecParseState *recordblockstate = NULL;

    switch (info) {
        case XLOG_IVF_UNLOG_BUILD_INDEX:
            recordblockstate = IvfXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_IVF_BUILD_INDEX:
            recordblockstate = IvfXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_IVF_EXTEND_NEWPAGES:
            recordblockstate = IvfXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_IVF_INSERT_INDEX:
            recordblockstate = IvfXlogInsertParseBlock(record, blocknum);
            break;
        case XLOG_IVF_DELETE_INDEX:
            recordblockstate = IvfXlogDeleteParseBlock(record, blocknum);
            break;
        case XLOG_IVF_UPDATE_LIST:
            recordblockstate = IvfXlogUpdateListParseBlock(record, blocknum);
            break;
        case XLOG_IVF_UPDATE_PAGE_NEXTBLKNO:
            recordblockstate = IvfXlogUpdatePageNextBlknoParseBlock(record, blocknum);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("IvfRedoParseToBlock: unknown op code %u", info)));
    }

    return recordblockstate;
}

void IvfRedoFullPageBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action != BLK_RESTORED) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("IvfRedoFullPageBlock did not contain a full-page image of %u page",
                               XLogBlockDataGetBlockId(datadecode))));
    }
    MakeRedoBufferDirty(bufferinfo);
}

void IvfRedoInsertBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    Page page = bufferinfo->pageinfo.page;

    if (action == BLK_NEEDS_REDO) {
        xl_ivf_insert *xl_rec = (xl_ivf_insert *)XLogBlockDataGetMainData(datadecode, NULL);
        char *itup = XLogBlockDataGetBlockData(datadecode, NULL);
        if (OffsetNumberIsValid(xl_rec->reuseOffno)) {
            if (!page_index_tuple_overwrite(page, xl_rec->reuseOffno, (Item) itup, xl_rec->itemsz)) {
                ereport(PANIC, (errmsg("ivf index insert redo : failed to overwrite itup")));
            }
        } else {
            if (PageAddItem(page, (Item) itup, xl_rec->itemsz, xl_rec->offsetNumber, false, false) == InvalidOffsetNumber) {
                ereport(PANIC, (errmsg("ivf index insert redo : failed to add itup")));
            }
        }
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void IvfRedoDeleteBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    Page page = bufferinfo->pageinfo.page;

    if (action == BLK_NEEDS_REDO) {
        xl_ivf_vacuum *xl_rec = (xl_ivf_vacuum *)XLogBlockDataGetMainData(datadecode, NULL);
        OffsetNumber *deletable = (OffsetNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
        if (xl_rec->flag == DELETE_PHYSICAL) {
            PageIndexMultiDelete(page, deletable, xl_rec->ndeletable);
        } else if (xl_rec->flag == DELETE_MARK_UNUSED) {
            PageIndexMultiSetUnused(page, deletable, xl_rec->ndeletable);
        } else {
            elog(ERROR, "unexpected ivf redo delete index method:%u", xl_rec->flag);
        }
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void IvfRedoUpdateListBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    Page page = bufferinfo->pageinfo.page;

    if (action == BLK_NEEDS_REDO) {
        OffsetNumber *offsetNumber = (OffsetNumber *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        IvfList list = (IvfList)PageGetItem(page, PageGetItemId(page, *offsetNumber));
        errno_t rc = memcpy_s((char*)&list->startPage, 4 * sizeof(BlockNumber), data, 4 * sizeof(BlockNumber));
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void IvfRedoUpdatePageNextblknoBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec,
                                     RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Size datalen;
        char *data = XLogBlockDataGetBlockData(datadecode, &datalen);
        Page page = bufferinfo->pageinfo.page;
        errno_t rc = memcpy_s(PageGetSpecialPointer(page), datalen, data, datalen);
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void IvfRedoDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    uint8 info = XLogBlockHeadGetInfo(blockhead) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_IVF_UNLOG_BUILD_INDEX:
            IvfRedoFullPageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_IVF_BUILD_INDEX:
            IvfRedoFullPageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_IVF_EXTEND_NEWPAGES:
            IvfRedoFullPageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_IVF_INSERT_INDEX:
            IvfRedoInsertBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_IVF_DELETE_INDEX:
            IvfRedoDeleteBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_IVF_UPDATE_LIST:
            IvfRedoUpdateListBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_IVF_UPDATE_PAGE_NEXTBLKNO:
            IvfRedoUpdatePageNextblknoBlock(blockhead, blockdatarec, bufferinfo);
            break;
        default:
            ereport(PANIC,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("IvfRedoDataBlock: unknown op code %u", info)));
    }
}