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
    HYBRID_FULL_IMAGE_PAGE_BLOCK_NUM = 0,
} XLogHybridFullImagePageEnum;

typedef enum {
    HYBRID_INSERT_LEFT_BLOCK_NUM = 0,
    HYBRID_INSERT_CHILD_BLOCK_NUM,
} XLogHybridInsertEnum;

typedef enum {
    HYBRID_SPLIT_ORIGH_BLOCK_NUM = 0,
    HYBRID_SPLIT_RNODE_BLOCK_NUM,
    HYBRID_SPLIT_RIGHT_BLOCK_NUM,
    HYBRID_SPLIT_CHILD_BLOCK_NUM,
} XLogHybridSplitEnum;

typedef enum {
    HYBRID_SPLITROOT_NEWROOT_BLOCK_NUM = 0,
    HYBRID_SPLITROOT_LEFT_BLOCK_NUM,
    HYBRID_SPLITROOT_META_BLOCK_NUM,
} XLogHybridSplitRootEnum;

typedef enum {
    HYBRID_VACUUM_BLOCK_NUM = 0,
} XLogHybridVacuumEnum;

typedef enum {
    HYBRID_MARK_HALF_DEAD_TOPPARENT_BLOCK_NUM = 0,
    HYBRID_MARK_HALF_DEAD_NODE_BLOCK_NUM,
} XLogHybridMarkHalfDeadEnum;

typedef enum {
    HYBRID_UNLINK_HALF_DEAD_RNODE_BLOCK_NUM = 0,
    HYBRID_UNLINK_HALF_DEAD_NODE_BLOCK_NUM,
    HYBRID_UNLINK_HALF_DEAD_LNODE_BLOCK_NUM,
    HYBRID_UNLINK_HALF_DEAD_LEAFNODE_BLOCK_NUM,
} XLogHybridUnlinkHalfDeadEnum;

static XLogRecParseState *HybridXlogFullImagePageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HYBRID_FULL_IMAGE_PAGE_BLOCK_NUM, recordstatehead);
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

static XLogRecParseState *HybridXlogAddVectorParseBlock(XLogReaderState *record, uint32 *blocknum)
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
    recordstatehead->blockparse.extra_rec.blockvectorec.mainData = XLogRecGetData(record);
    recordstatehead->blockparse.extra_rec.blockvectorec.datalen = sizeof(xl_ann_add_vector) + xl_rec->nbytes;
    recordstatehead->isFullSync = record->isFullSync;
    return recordstatehead;
}

static XLogRecParseState *HybridXlogInsertDataParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HYBRID_INSERT_LEFT_BLOCK_NUM, recordstatehead);
    *blocknum = 1;

    if (XLogRecHasBlockRef(record, HYBRID_INSERT_CHILD_BLOCK_NUM)) {
        XLogRecParseState *blockstate = NULL;
        XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
        XLogRecSetBlockDataState(record, HYBRID_INSERT_CHILD_BLOCK_NUM, blockstate);
        ++(*blocknum);
    }

    return recordstatehead;
}

static XLogRecParseState *HybridXlogSplitParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HYBRID_SPLIT_ORIGH_BLOCK_NUM, recordstatehead);

    XLogRecParseState *blockstate = NULL;
    XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
    XLogRecSetBlockDataState(record, HYBRID_SPLIT_RNODE_BLOCK_NUM, blockstate);

    *blocknum = 2;

    xl_hybrid_split *xl_rec = (xl_hybrid_split *)XLogRecGetData(record);

    if (xl_rec->is_right_most) {
        XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
        XLogRecSetBlockDataState(record, HYBRID_SPLIT_RIGHT_BLOCK_NUM, blockstate);
        ++(*blocknum);
    }

    if (!xl_rec->isleaf) {
        XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
        XLogRecSetBlockDataState(record, HYBRID_SPLIT_CHILD_BLOCK_NUM, blockstate);
        ++(*blocknum);
    }

    return recordstatehead;
}

static XLogRecParseState *HybridXlogSplitRootParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HYBRID_SPLITROOT_NEWROOT_BLOCK_NUM, recordstatehead);

    XLogRecParseState *blockstate = NULL;
    XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
    XLogRecSetBlockDataState(record, HYBRID_SPLITROOT_LEFT_BLOCK_NUM, blockstate);

    XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
    XLogRecSetBlockDataState(record, HYBRID_SPLITROOT_META_BLOCK_NUM, blockstate);

    *blocknum = 3;
    return recordstatehead;
}

static XLogRecParseState *HybridXlogVacuumParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HYBRID_VACUUM_BLOCK_NUM, recordstatehead);

    *blocknum = 1;
    return recordstatehead;
}

static XLogRecParseState *HybridXlogMarkPageHalfDeadParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HYBRID_MARK_HALF_DEAD_TOPPARENT_BLOCK_NUM, recordstatehead);

    XLogRecParseState *blockstate = NULL;
    XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
    XLogRecSetBlockDataState(record, HYBRID_MARK_HALF_DEAD_NODE_BLOCK_NUM, blockstate);

    *blocknum = 2;
    return recordstatehead;
}

static XLogRecParseState *HybridXlogUnlinkHalfDeadPageParseBlock(XLogReaderState *record, uint32 *blocknum)
{
    XLogRecParseState *recordstatehead = NULL;
    XLogParseBufferAllocListFunc(record, &recordstatehead, NULL);
    XLogRecSetBlockDataState(record, HYBRID_UNLINK_HALF_DEAD_RNODE_BLOCK_NUM, recordstatehead);

    XLogRecParseState *blockstate = NULL;
    XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
    XLogRecSetBlockDataState(record, HYBRID_UNLINK_HALF_DEAD_NODE_BLOCK_NUM, blockstate);

    *blocknum = 2;

    if (XLogRecHasBlockRef(record, HYBRID_UNLINK_HALF_DEAD_LNODE_BLOCK_NUM)) {
        XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
        XLogRecSetBlockDataState(record, HYBRID_UNLINK_HALF_DEAD_LNODE_BLOCK_NUM, blockstate);
        ++(*blocknum);
    }

    xl_hybrid_unlink_harfhead_page *xldata = (xl_hybrid_unlink_harfhead_page *)XLogRecGetData(record);

    if (xldata->target != xldata->leafblkno) {
        XLogParseBufferAllocListFunc(record, &blockstate, recordstatehead);
        XLogRecSetBlockDataState(record, HYBRID_UNLINK_HALF_DEAD_LEAFNODE_BLOCK_NUM, blockstate);
        ++(*blocknum);
    }

    return recordstatehead;
}

XLogRecParseState *HybridRedoParseToBlock(XLogReaderState *record, uint32 *blocknum)
{
    *blocknum = 0;
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    XLogRecParseState *recordblockstate = NULL;

    switch (info) {
        case XLOG_HYBRID_BUILD_INDEX:
        case XLOG_HYBRID_UNLOG_BUILD_INDEX:
            recordblockstate = HybridXlogFullImagePageParseBlock(record, blocknum);
            break;
        case XLOG_HYBRID_ADD_VECTOR:
            recordblockstate = HybridXlogAddVectorParseBlock(record, blocknum);
            break;
        case XLOG_HYBRID_BT_INSERT_DATA:
            recordblockstate = HybridXlogInsertDataParseBlock(record, blocknum);
            break;
        case XLOG_HYBRID_BT_SPLIT:
            recordblockstate = HybridXlogSplitParseBlock(record, blocknum);
            break;
        case XLOG_HYBRID_BT_SPLIT_ROOT:
            recordblockstate = HybridXlogSplitRootParseBlock(record, blocknum);
            break;
        case XLOG_HYBRID_BT_VACUUM:
            recordblockstate = HybridXlogVacuumParseBlock(record, blocknum);
            break;
        case XLOG_HYBRID_BT_MARK_PAGE_HALFDEAD:
            recordblockstate = HybridXlogMarkPageHalfDeadParseBlock(record, blocknum);
            break;
        case XLOG_HYBRID_BT_UNLINK_HALFDEAD_PAGE:
            recordblockstate = HybridXlogUnlinkHalfDeadPageParseBlock(record, blocknum);
            break;
        default:
            elog(PANIC, "redo_hybridxlog: unknown record type %u", info);
    }
    return recordblockstate;
}

void HybridRedoFullImagePageBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action != BLK_RESTORED) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("HybridRedoFullImagePageBlock did not contain a full-page image of %u page",
                               XLogBlockDataGetBlockId(datadecode))));
    }
    MakeRedoBufferDirty(bufferinfo);
}

void HybridRedoInsertDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    if (XLogBlockDataGetBlockId(datadecode) == HYBRID_INSERT_LEFT_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            OffsetNumber *newitemoff = (OffsetNumber *)XLogBlockDataGetMainData(datadecode, NULL);
            char *data = XLogBlockDataGetBlockData(datadecode, NULL);
            DiskNodeImpl lnode(bufferinfo->buf);
            lnode.insert(*newitemoff, (BTTupleData *)data);
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            DiskNodeImpl cnode(bufferinfo->buf);
            cnode.set_incomplete_split(false);
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    }
}

void HybridRedoSplitBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;

    if (XLogBlockDataGetBlockId(datadecode) == HYBRID_SPLIT_ORIGH_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            char *data = XLogBlockDataGetBlockData(datadecode, NULL);
            errno_t rc = memcpy_s(bufferinfo->pageinfo.page, BLCKSZ, data, BLCKSZ);
            securec_check(rc, "\0", "\0");
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else if (XLogBlockDataGetBlockId(datadecode) == HYBRID_SPLIT_RNODE_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action != BLK_RESTORED) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("HybridRedoSplitBlock did not contain a full-page image of %u page",
                                   XLogBlockDataGetBlockId(datadecode))));
        }
    } else if (XLogBlockDataGetBlockId(datadecode) == HYBRID_SPLIT_RIGHT_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            BlockNumber *blkno = (BlockNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
            DiskNodeImpl right_node(bufferinfo->buf);
            right_node.prev() = *blkno;
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            DiskNodeImpl child_node(bufferinfo->buf);
            child_node.set_incomplete_split(false);
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    }
}

void HybridRedoSplitRootBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    if (XLogBlockDataGetBlockId(datadecode) == HYBRID_SPLITROOT_NEWROOT_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action != BLK_RESTORED) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("HybridRedoSplitRootBlock did not contain a full-page image of %u page",
                                   XLogBlockDataGetBlockId(datadecode))));
        }
    } else if (XLogBlockDataGetBlockId(datadecode) == HYBRID_SPLITROOT_LEFT_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            DiskNodeImpl l_node(bufferinfo->buf);
            l_node.set_root(false);
            l_node.set_incomplete_split(false);
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            BlockNumber *blkno = (BlockNumber *)XLogBlockDataGetBlockData(datadecode, NULL);
            DiskBTMeta metapage = (DiskBTMeta) bufferinfo->pageinfo.page;
            metapage->root_ptr = *blkno;
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    }
}

void HybridRedoVacuumBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
    if (action == BLK_NEEDS_REDO) {
        int *num_deletable = (int *)XLogBlockDataGetMainData(datadecode, NULL);
        if (*num_deletable > 0) {
            char *deletable = XLogBlockDataGetBlockData(datadecode, NULL);
            PageIndexMultiDelete(bufferinfo->pageinfo.page, (OffsetNumber *)deletable, *num_deletable);
        }
        DiskNodeImpl node(bufferinfo->buf);
        node.cycle_id() = 0;
        node.set_has_garbage(false);
        PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
        MakeRedoBufferDirty(bufferinfo);
    }
}

void HybridRedoMarkPageHalfDeadBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    if (XLogBlockDataGetBlockId(datadecode) == HYBRID_MARK_HALF_DEAD_TOPPARENT_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action== BLK_NEEDS_REDO) {
            xl_hybrid_mark_page_harfhead *xl_rec = (xl_hybrid_mark_page_harfhead *)XLogBlockDataGetBlockData(datadecode, NULL);
            DiskNodeImpl topparent(bufferinfo->buf);
            topparent.get_data(xl_rec->topoff)->set_ptr(xl_rec->rightsib);
            topparent.delete_tuple(OffsetNumberNext(xl_rec->topoff));
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            char *tupledata = XLogBlockDataGetBlockData(datadecode, NULL);
            DiskNodeImpl node(bufferinfo->buf);
            node.set_half_dead(true);
            node.delete_tuple(P_HIKEY);
            node.insert(P_HIKEY, (BTTupleData *)tupledata);
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    }
}

 void HybridRedoUnlinkHalfDeadPageBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    XLogBlockDataParse *datadecode = blockdatarec;
    xl_hybrid_unlink_harfhead_page *xl_rec = (xl_hybrid_unlink_harfhead_page *)XLogBlockDataGetMainData(datadecode, NULL);

    if (XLogBlockDataGetBlockId(datadecode) == HYBRID_UNLINK_HALF_DEAD_RNODE_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            DiskNodeImpl rnode(bufferinfo->buf);
            rnode.prev() = xl_rec->leftsib;
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else if (XLogBlockDataGetBlockId(datadecode) == HYBRID_UNLINK_HALF_DEAD_NODE_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            DiskNodeImpl node(bufferinfo->buf);
            node.set_half_dead(false);
            node.set_deleted(true);
            node.set_xact(xl_rec->xid);
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else if (XLogBlockDataGetBlockId(datadecode) == HYBRID_UNLINK_HALF_DEAD_LNODE_BLOCK_NUM) {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            DiskNodeImpl lnode(bufferinfo->buf);
            lnode.next() = xl_rec->rightsib;
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    } else {
        XLogRedoAction action = XLogCheckBlockDataRedoAction(datadecode, bufferinfo);
        if (action == BLK_NEEDS_REDO) {
            DiskNodeImpl leafnode(bufferinfo->buf);
            leafnode.get_data(P_HIKEY)->set_ptr(xl_rec->nextchild);
            PageSetLSN(bufferinfo->pageinfo.page, bufferinfo->lsn);
            MakeRedoBufferDirty(bufferinfo);
        }
    }
}

void HybridRedoDataBlock(XLogBlockHead *blockhead, XLogBlockDataParse *blockdatarec, RedoBufferInfo *bufferinfo)
{
    uint8 info = XLogBlockHeadGetInfo(blockhead) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_HYBRID_BUILD_INDEX:
        case XLOG_HYBRID_UNLOG_BUILD_INDEX:
            HybridRedoFullImagePageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HYBRID_BT_INSERT_DATA:
            HybridRedoInsertDataBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HYBRID_BT_SPLIT:
            HybridRedoSplitBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HYBRID_BT_SPLIT_ROOT:
            HybridRedoSplitRootBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HYBRID_BT_VACUUM:
            HybridRedoVacuumBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HYBRID_BT_MARK_PAGE_HALFDEAD:
            HybridRedoMarkPageHalfDeadBlock(blockhead, blockdatarec, bufferinfo);
            break;
        case XLOG_HYBRID_BT_UNLINK_HALFDEAD_PAGE:
            HybridRedoUnlinkHalfDeadPageBlock(blockhead, blockdatarec, bufferinfo);
            break;
        default:
            ereport(PANIC, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("HybridRedoDataBlock: unknown op code %u", info)));
    }
}