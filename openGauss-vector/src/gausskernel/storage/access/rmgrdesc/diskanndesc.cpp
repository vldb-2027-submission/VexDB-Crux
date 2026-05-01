#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/diskann/diskann.h"

const char* diskann_type_name(uint8 subtype)
{
    uint8 info = subtype & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_DISKANN_BUILD_INDEX:
            return "diskann_build_index";
            break;
        case XLOG_DISKANN_UNLOG_BUILD_INDEX:
            return "diskann_unlog_build_index";
            break;
        case XLOG_DISKANN_SET_ELEM:
            return "diskann_set_elem";
            break;
        case XLOG_DISKANN_EXTEND_NEWPAGES:
            return "diskann_extend_newpages";
            break;
        case XLOG_DISKANN_UPDATE_META_START_NPAGES:
            return "diskann_update_meta_start_npages";
            break;
        case XLOG_DISKANN_UPDATE_META_NITEM:
            return "diskann_update_meta_nitem";
            break;
        case XLOG_DISKANN_ADD_VECTOR:
            return "diskann_add_vector";
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_DATA:
            return "diskann_inplace_filter_add_data";
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM:
            return "diskann_inplace_filter_add_item";
            break;
        case XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM:
            return "diskann_inplace_filter_delete_item";
            break;
        case XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE:
            return "diskann_inplace_filter_multi_delete";
            break;
        case XLOG_DISKANN_INVALIDATE_VECTOR_CACHE:
            return "diskann_invalidate_vector_cache";
            break;
        default:
            return "unknow_type";
            break;
    }   
}

void diskann_desc(StringInfo buf, XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_DISKANN_BUILD_INDEX:
            appendStringInfoString(buf, "diskann build index");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_DISKANN_UNLOG_BUILD_INDEX:
            appendStringInfoString(buf, "diskann unlog build index");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_DISKANN_SET_ELEM:
            appendStringInfoString(buf, "diskann set elem");
            break;
        case XLOG_DISKANN_EXTEND_NEWPAGES:
            appendStringInfoString(buf, "diskann extend newpages");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_DISKANN_UPDATE_META_START_NPAGES:
            appendStringInfoString(buf, "diskann update meta start npages");
            break;
        case XLOG_DISKANN_UPDATE_META_NITEM:
            appendStringInfoString(buf, "diskann update meta nitem");
            break;
        case XLOG_DISKANN_ADD_VECTOR:
            appendStringInfoString(buf, "diskann add vector");
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_DATA:
            appendStringInfoString(buf, "diskann inplace filter add data");
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM:
            appendStringInfoString(buf, "diskann inplace filter add item");
            break;
        case XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM:
            appendStringInfoString(buf, "diskann inplace filter delete item");
            break;
        case XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE:
            appendStringInfoString(buf, "diskann inplace filter multi delete");
            break;
        case XLOG_DISKANN_INVALIDATE_VECTOR_CACHE:
            appendStringInfoString(buf, "diskann invalidate vector cache");
            break;
        default:
            appendStringInfo(buf, "unknown diskann op code %hhu", info);
            break;
    }
}