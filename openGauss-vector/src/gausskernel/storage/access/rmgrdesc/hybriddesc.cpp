#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/annvector/xlog/log_manager.h"

const char* hybrid_type_name(uint8 subtype)
{
    uint8 info = subtype & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_HYBRID_BUILD_INDEX:
            return "hybrid_build_index";
            break;
        case XLOG_HYBRID_UNLOG_BUILD_INDEX:
            return "hybrid_unlog_build_index";
            break;
        case XLOG_HYBRID_ADD_VECTOR:
            return "hybrid_add_vector";
            break;
        case XLOG_HYBRID_BT_INSERT_DATA:
            return "hybrid_bt_insert_data";
            break;
        case XLOG_HYBRID_BT_SPLIT:
            return "hybrid_bt_split";
            break;
        case XLOG_HYBRID_BT_SPLIT_ROOT:
            return "hybrid_bt_split_root";
            break;
        case XLOG_HYBRID_BT_VACUUM:
            return "hybrid_bt_vacuum";
            break;
        case XLOG_HYBRID_BT_MARK_PAGE_HALFDEAD:
            return "hybrid_bt_mark_page_halfdead";
            break;
        case XLOG_HYBRID_BT_UNLINK_HALFDEAD_PAGE:
            return "hybrid_bt_unlink_halfdead_page";
            break;
        default:
            return "unknow_type";
            break;
    }
}

void hybrid_desc(StringInfo buf, XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_HYBRID_BUILD_INDEX:
            appendStringInfoString(buf, "hybrid build index");
            break;
        case XLOG_HYBRID_UNLOG_BUILD_INDEX:
            appendStringInfoString(buf, "hybrid unlog build index");
            break;
        case XLOG_HYBRID_ADD_VECTOR:
            appendStringInfoString(buf, "hybrid add vector");
            break;
        case XLOG_HYBRID_BT_INSERT_DATA:
            appendStringInfoString(buf, "hybrid bt insert data");
            break;
        case XLOG_HYBRID_BT_SPLIT:
            appendStringInfoString(buf, "hybrid bt split");
            break;
        case XLOG_HYBRID_BT_SPLIT_ROOT:
            appendStringInfoString(buf, "hybrid bt split root");
            break;
        case XLOG_HYBRID_BT_VACUUM:
            appendStringInfoString(buf, "hybrid bt vacuum");
            break;
        case XLOG_HYBRID_BT_MARK_PAGE_HALFDEAD:
            appendStringInfoString(buf, "hybrid bt mark page halfdead");
            break;
        case XLOG_HYBRID_BT_UNLINK_HALFDEAD_PAGE:
            appendStringInfoString(buf, "hybrid bt unlink halfdead page");
            break;
        default:
            appendStringInfo(buf, "unknown hybrid op code %hhu", info);
            break;
    }
}