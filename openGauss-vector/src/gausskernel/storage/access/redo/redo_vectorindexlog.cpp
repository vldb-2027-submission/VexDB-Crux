#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "access/xlogproc.h"
#include "utils/memutils.h"
#include "access/annvector/xlog/log_manager.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/hybridann/bplustree/disk_impl.h"

using namespace disk_container;

typedef enum {
    VEC_PREPARE_META_BLOCK_NUM = 0,
    VEC_PREPARE_NODE_BLOCK_NUM,
} XLogVecPrepareMetaEnum;

typedef enum {
    VEC_SPLIT_SCAN_BLOCK_NUM = 0,
} XLogVecSplitScanEnum;

typedef enum {
    VEC_UPDATE_NEXT_META_BLOCK_NUM = 0,
} XLogVecUpdateNextMetaEnum;

typedef enum {
    VEC_SET_LEFTMOST_NODE_BLOCK_NUM = 0,
} XLogVecSetLeftMostNodeEnum;

typedef enum {
    VEC_INSERT_NODE_BLOCK_NUM = 0,
} XLogVecInsertNodeEnum;

typedef enum {
    VEC_REMOVE_NODE_BLOCK_NUM = 0,
} XLogVecRemoveNodeEnum;

typedef enum {
    VEC_UPDATE_META_BLOCK_NUM = 0,
} XLogVecUpdateMetaEnum;

typedef enum {
    VEC_START_OPERATION_BLOCK_NUM = 0,
} XLogVecStartOperationEnum;

typedef enum {
    VEC_FINISH_OPERATION_BLOCK_NUM = 0,
} XLogVecFinishOperationEnum;

typedef enum {
    VEC_SET_INDEX_PTR_BLOCK_NUM = 0,
} XLogVecSetIndexPtrEnum;

typedef enum {
    VEC_SET_NEW_INDEX_PTR_BLOCK_NUM = 0,
} XLogVecSetNewIndexPtrEnum;

typedef enum {
    VEC_SET_DELETED_BLOCK_NUM = 0,
} XLogVecSetDeletedEnum;

typedef enum {
    VEC_SET_TEMP_UNLINK_BLOCK_NUM = 0,
} XLogVecSetTempUnlinkEnum;

typedef enum {
    VEC_ADD_ANN_META_BLOCK_NUM = 0,
} XLogVecAddAnnMetaEnum;

typedef enum {
    VEC_MODIFY_META_BLOCK_NUM = 0,
} XLogVecModifyMetaEnum;


static XLogRecParseState *VecXLogPrepareMetaParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_PREPARE_META_BLOCK_NUM, recordstatehead);

    XLogRecParseState *blockstate = NULL;
    XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
    XLogRecSetBlockDataState(record, VEC_PREPARE_NODE_BLOCK_NUM, blockstate);

    *blocknum = 2;
    return recordstatehead;
}

static XLogRecParseState *VecXLogSplitScanBlknoParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_SPLIT_SCAN_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogUpdateNextMetaBlknoParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_UPDATE_NEXT_META_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogSetLeftMostNodeBlknoParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_SET_LEFTMOST_NODE_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogInsertNodeParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_INSERT_NODE_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogRemoveNodeParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_REMOVE_NODE_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState  *VecXLogStartOperationParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_START_OPERATION_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogFinishOperationParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_FINISH_OPERATION_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogSetIndexPtrParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_SET_INDEX_PTR_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogSetDeletedPtrParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_SET_DELETED_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogSetTempUnlinkPtrParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_SET_TEMP_UNLINK_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogAddAnnMetaPtrParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_ADD_ANN_META_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *VecXLogModifyMetaPtrParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, VEC_MODIFY_META_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

XLogRecParseState *VectorIndexRedoParseToBlock(XLogReaderState *record, uint32 *blocknum)
{
    *blocknum = 0;
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    XLogRecParseState *recordblockstate = NULL;

    switch (info) {
        case XLOG_VECTORINDEX_META_AND_NODE:
            recordblockstate = VecXLogPrepareMetaParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_SPLIT_SCAN_BLKNO:
            recordblockstate = VecXLogSplitScanBlknoParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_UPDATE_NEXT_META_BLKNO:
            recordblockstate = VecXLogUpdateNextMetaBlknoParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_SET_LEFTMOST_NODE_BLKNO:
            recordblockstate = VecXLogSetLeftMostNodeBlknoParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_INSERT_NODE:
            recordblockstate = VecXLogInsertNodeParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_REMOVE_NODE:
            recordblockstate = VecXLogRemoveNodeParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_START_OPERATION:
            recordblockstate = VecXLogStartOperationParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_FINISH_OPERATION:
            recordblockstate = VecXLogFinishOperationParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_SET_INDEX_PTR:
            recordblockstate = VecXLogSetIndexPtrParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_SET_DELETED:
            recordblockstate = VecXLogSetDeletedPtrParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_SET_TEMP_UNLINK:
            recordblockstate = VecXLogSetTempUnlinkPtrParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_ADD_ANN_META:
            recordblockstate = VecXLogAddAnnMetaPtrParseBlock(record, blocknum);
            break;
        case XLOG_VECTORINDEX_MODIFY_META:
            recordblockstate = VecXLogModifyMetaPtrParseBlock(record, blocknum);
            break;
        default:
            elog(PANIC, "redo_vectorindexlog: unknown record type %u", info);
    }
    return recordblockstate;
}


void VecRedoPrepareMetaBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;

    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action != BLK_RESTORED) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("VecRedoPrepareMetaBlock did not contain a full-page image of %u page",
                               XLogBlockDataGetBlockId(datadecode))));
    }
    MakeRedoBufferDirty(bufferinfo);
}


void VecRedoSplitScanBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        BlockNumber *blkno = (BlockNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->split_scanned_blkno = *blkno;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}


void VecRedoUpdateNextMetaBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        BlockNumber *blkno = (BlockNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->next_idx_blkno = *blkno;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VecRedoSetLeftmostNodeBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        BlockNumber *blkno = (BlockNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->leftmost_node_blkno = *blkno;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
} 
 
void VecRedoInsertNodeBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        Size size;
        BlockNumber *blkno = (BlockNumber *)XLogBlockDataGetBlockData(datadecode, &size);
        Page page = bufferinfo->pageinfo.page;
        VectorIndexNode node = reinterpret_cast<VectorIndexNode>(PageGetContents(page));
        if (size != sizeof(BlockNumber)) {
            node->nodes[nodepage_internal::max_nnode_per_page] = *blkno;
        } else {
            node->insert(InvalidRelation, *blkno, bufferinfo->buf);
        }
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VecRedoRemoveNodeBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        Page page = bufferinfo->pageinfo.page;
        VectorIndexNode node = reinterpret_cast<VectorIndexNode>(PageGetContents(page));
        BlockNumber *blkno = (BlockNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
        node->remove(InvalidRelation, *blkno);
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VecRedoStartOperationBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        bool *idle = (bool *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->idle = *idle;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VecRedoFinishOperationBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    VecRedoStartOperationBlock(blockhead, blockdatarec, bufferinfo);
}

void VecRedoSetIndexPtrBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        bool *indexPtrIsNew = (bool *)XLogBlockDataGetMainData(datadecode, NULL);
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        size_t *level = (size_t *)data;
        BlockNumber *blkno = (BlockNumber *)(data + sizeof(size_t));
        Page page = bufferinfo->pageinfo.page;
        DiskBTInternalOpaque opaque = reinterpret_cast<DiskBTInternalOpaque>(PageGetSpecialPointer(page));
        if (*indexPtrIsNew) {
            opaque->new_vec_index_meta_blkno[*level] = *blkno;
        } else {
            opaque->vec_index_meta_blkno[*level] = *blkno;
        }
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VecRedoSetDeletePtrBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        bool *deleted = (bool *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->deleted = *deleted;
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VecRedoSetTempUnlinkPtrBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        bool *isUnlink = (bool *)XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        DiskNodeImpl node(bufferinfo->buf);
        node.set_temporarily_unlinkable(*isUnlink);
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VecRedoAddAnnMetaPtrBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action != BLK_RESTORED) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("VecRedoAddAnnMetaPtrBlock did not contain a full-page image of %u page",
                               XLogBlockDataGetBlockId(datadecode))));
    }
    MakeRedoBufferDirty(bufferinfo);
}

void VecRedoModifyMetaPtrBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        char *data = XLogBlockDataGetBlockData(datadecode, NULL);
        Page page = bufferinfo->pageinfo.page;
        VectorIndexType *type = (VectorIndexType *)XLogBlockDataGetMainData(datadecode, NULL);
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->index_meta_blkno = *(BlockNumber *)data;
        if (*type == VectorIndexType::GRAPH) {
            uint32 *dim = (uint32 *)(data + sizeof(BlockNumber));
            meta->dim = *dim;
        }
        PageSetLSN(page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void VectorIndexRedoDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    uint8 info = XLogBlockHeadGetInfo(blockhead) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_VECTORINDEX_META_AND_NODE:
            VecRedoPrepareMetaBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_SPLIT_SCAN_BLKNO:
            VecRedoSplitScanBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_UPDATE_NEXT_META_BLKNO:
            VecRedoUpdateNextMetaBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_SET_LEFTMOST_NODE_BLKNO:
            VecRedoSetLeftmostNodeBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_INSERT_NODE:
            VecRedoInsertNodeBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_REMOVE_NODE:
            VecRedoRemoveNodeBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_START_OPERATION:
            VecRedoStartOperationBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_FINISH_OPERATION:
            VecRedoFinishOperationBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_SET_INDEX_PTR:
            VecRedoSetIndexPtrBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_SET_DELETED:
            VecRedoSetDeletePtrBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_SET_TEMP_UNLINK:
            VecRedoSetTempUnlinkPtrBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_ADD_ANN_META:
            VecRedoAddAnnMetaPtrBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_VECTORINDEX_MODIFY_META:
            VecRedoModifyMetaPtrBlock(blockhead, blockdatarec, bufferinfo);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("VectorIndexRedoDataBlock: unknown op code %u", info)));
    }
}