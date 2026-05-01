 /*
 * redo_bm25xlog.cpp
 *    parse bm25 xlog
 *
 * IDENTIFICATION
 *
 * src/gausskernel/storage/access/redo/redo_bm25xlog.cpp
 *
 */

#include <vtl/bitvector>
#include <vtl/disk_container/disk_hashtable_dependency.hpp>

#include "postgres.h"
#include "knl/knl_variable.h"
#include "utils/memutils.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "access/xlogproc.h"
#include "access/bm25/bm25.h"
#include "access/bm25/bm25_inverted_list.h"
#include "access/bm25/bm25_statistics.h"

using namespace bm25;
using namespace disk_container;
typedef enum {
    BM25_FULL_IMAGE_PAGE_BLOCK_NUM = 0,
} XLogBm25FullImagePageEnum;

typedef enum {
    BM25_INIT_PAGE_BLOCK_NUM = 0,
} XLogBm25ReuseFreePageEnum;

typedef enum {
    BM25_INSERT_ENTRY_BLOCK_NUM = 0,
} XLogBm25InsertEntryEnum;

typedef enum {
    BM25_INSERT_INPLACE_ENTRY_BLOCK_NUM = 0,
} XLogBm25InsertInplaceEntryEnum;

typedef enum {
    BM25_INSERT_STATS_BLOCK_NUM = 0,
} XLogBm25InsertStatsEnum;

typedef enum {
    BM25_UPDATE_INVERT_LIST_BLOCK_NUM = 0,
} XLogBm25UpdateInvertListEnum;

typedef enum {
    BM25_ADD_DATA_BLOCK_NUM = 0,
} XLogBm25AddDataEnum;

static XLogRecParseState *Bm25XlogFullImagePageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, BM25_FULL_IMAGE_PAGE_BLOCK_NUM, recordstatehead);
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

static XLogRecParseState *Bm25XlogInitPageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, BM25_INIT_PAGE_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *Bm25XlogInsertEntryParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, BM25_INSERT_ENTRY_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *Bm25XlogInsetInplaceEntryParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, BM25_INSERT_INPLACE_ENTRY_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *Bm25XlogInsertStatsParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, BM25_INSERT_STATS_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *Bm25XlogUpdateInvertListParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, BM25_UPDATE_INVERT_LIST_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    return recordstatehead;
}

static XLogRecParseState *Bm25XlogAddDataParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, BM25_ADD_DATA_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    return recordstatehead;
}

XLogRecParseState *Bm25RedoParseToBlock(XLogReaderState *record, uint32 *blocknum)
{
    *blocknum = 0;
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    XLogRecParseState *recordblockstate = NULL;

    switch (info) {
        case XLOG_BM25_BUILD_INDEX:
        case XLOG_BM25_APPEND_PAGES:
            recordblockstate = Bm25XlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_BM25_INIT_PAGE:
            recordblockstate = Bm25XlogInitPageParseBlock(record, blocknum);
            break;
        case XLOG_BM25_INSERT_ENTRY:
            recordblockstate = Bm25XlogInsertEntryParseBlock(record, blocknum);
            break;
        case XLOG_BM25_INSERT_INPLACE_ENTRY:
            recordblockstate = Bm25XlogInsetInplaceEntryParseBlock(record, blocknum);
            break;
        case XLOG_BM25_INSERT_STATS:
            recordblockstate = Bm25XlogInsertStatsParseBlock(record, blocknum);
            break;
        case XLOG_BM25_UPDATE_INVERT_LIST:
            recordblockstate = Bm25XlogUpdateInvertListParseBlock(record, blocknum);
            break;
        case XLOG_BM25_ADD_DATA:
            recordblockstate = Bm25XlogAddDataParseBlock(record, blocknum);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Bm25RedoParseToBlock: unknown op code %u", info)));
    }

    return recordblockstate;
}

void Bm25RedoFullImagePageBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action != BLK_RESTORED) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("Bm25RedoFullImagePageBlock did not contain a full-page image of %u page",
                               XLogBlockDataGetBlockId(datadecode))));
    }
    MakeRedoBufferDirty(bufferinfo);
}

void Bm25RedoInitPageBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        errno_t rc = memcpy_s(page, BLCKSZ, data, BLCKSZ);
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void Bm25RedoInsertEntryBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        xl_bm25_insert_entry *xl_rec = (xl_bm25_insert_entry *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->entry_size, data, xl_rec->entry_size);
        securec_check(rc, "\0", "\0");

        InvertedListPage il_page = GetInvertedListPage(page);
        il_page->nentry = xl_rec->nentry;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void Bm25RedoInsertInplaceEntryBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        xl_bm25_insert_inplace_entry *xl_rec = (xl_bm25_insert_inplace_entry *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);

        errno_t rc = memcpy_s((char *)page + xl_rec->bit_offset, xl_rec->bit_size, data, xl_rec->bit_size);
        securec_check(rc, "\0", "\0");
        rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->entry_size, (char *)(data + xl_rec->bit_size), xl_rec->entry_size);
        securec_check(rc, "\0", "\0");

        uint16 *nentry = (uint16 *)(page + MAXALIGN(SizeOfPageHeaderData) + sizeof(uint32) + sizeof(uint16));
        *nentry = xl_rec->nentry;

        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void Bm25RedoInsertStatsBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        BM25StatisticsPage bm_page = ((BM25StatisticsPage)((char *)(page) + SizeOfPageHeaderData));
        xl_bm25_insert_stats *xl_rec = (xl_bm25_insert_stats *)XLogBlockDataGetMainData(datadecode, NULL);
        bm_page->max_doc_id = xl_rec->max_doc_id;
        if (xl_rec->data_size > 0) {
            char *data = XLogBlockDataGetBlockData(datadecode, NULL);
            errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->data_size, data, xl_rec->data_size);
            securec_check(rc, "\0", "\0");
        }
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void Bm25RedoUpdateInvertListBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        xl_bm25_update_invert_list *xl_rec = (xl_bm25_update_invert_list *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);

        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->entry_size, data, xl_rec->entry_size);
        securec_check(rc, "\0", "\0");

        InvertedListPage il_page = GetInvertedListPage(page);
        il_page->nentry = xl_rec->nentry;

        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void Bm25RedoAddDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);

    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        xl_bm25_add_data *xl_rec = (xl_bm25_add_data *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->data_size, data, xl_rec->data_size);
        securec_check(rc, "\0", "\0");

        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void Bm25RedoDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    uint8 info = XLogBlockHeadGetInfo(blockhead) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_BM25_BUILD_INDEX:
        case XLOG_BM25_APPEND_PAGES:
            Bm25RedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_BM25_INIT_PAGE:
            Bm25RedoInitPageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_BM25_INSERT_ENTRY:
            Bm25RedoInsertEntryBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_BM25_INSERT_INPLACE_ENTRY:
            Bm25RedoInsertInplaceEntryBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_BM25_INSERT_STATS:
            Bm25RedoInsertStatsBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_BM25_UPDATE_INVERT_LIST:
            Bm25RedoUpdateInvertListBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_BM25_ADD_DATA:
            Bm25RedoAddDataBlock(blockhead, blockdatarec, bufferinfo);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Bm25RedoDataBlock: unknown op code %u", info)));
    }
}