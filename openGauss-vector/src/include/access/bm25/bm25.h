/**
 * Copyright ...
 * BM25 index access method definitions.
 */

#ifndef BM25_H
#define BM25_H

#include "fmgr.h"
#include "access/xlog_basic.h"
#include "lib/stringinfo.h"
#include "storage/buf/buf.h"
#include "storage/buf/bufpage.h"

#define BM25_META_BLKNO 0
#define BM25_STATISTICS_BLKNO 1u
#define BM25_VERSION 1u
#define BM25_STATS_VERSION 1u
#define BM25_INVERTED_LIST_VERSION 1u
#define BM25_META_MAGIC 0x1a1a11aau
#define BM25_STATS_MAGIC 0x1a1a11acu
#define BM25_INVERTED_LIST_MAGIC 0x0abcabc0u

#define BM25_STATS false
#define OUTPUT_BM25_LOG false
#define BM25_MAX_NATTR INDEX_MAX_KEYS

/* options has type List * which should only be passed by ts_search module */
extern void bm25_dict_init(void *bm25_cxt);
extern void *get_bm25_dict(Oid dict_id, void *options);
extern Datum djieba_init(PG_FUNCTION_ARGS);
extern Datum djieba_lexize(PG_FUNCTION_ARGS);

extern Datum bm25_match_text(PG_FUNCTION_ARGS);
extern Datum bm25_match_varchar(PG_FUNCTION_ARGS);
extern Datum bm25_match_char(PG_FUNCTION_ARGS);
extern Datum bm25_match_textarr(PG_FUNCTION_ARGS);
extern Datum bm25_match_varchararr(PG_FUNCTION_ARGS);
extern Datum bm25_match_chararr(PG_FUNCTION_ARGS);
extern Datum bm25_rank_match_text(PG_FUNCTION_ARGS);
extern Datum bm25_rank_match_varchar(PG_FUNCTION_ARGS);
extern Datum bm25_rank_match_char(PG_FUNCTION_ARGS);
extern Datum bm25_rank_match_textarr(PG_FUNCTION_ARGS);
extern Datum bm25_rank_match_varchararr(PG_FUNCTION_ARGS);
extern Datum bm25_rank_match_chararr(PG_FUNCTION_ARGS);
extern Datum bm25_score(PG_FUNCTION_ARGS);
extern Datum bm25_tokenize(PG_FUNCTION_ARGS);
extern Datum bm25_query_tokenize(PG_FUNCTION_ARGS);
extern Datum bm25_search_highlight(PG_FUNCTION_ARGS);

extern Datum bm25build(PG_FUNCTION_ARGS);
extern Datum bm25buildempty(PG_FUNCTION_ARGS);
extern Datum bm25beginscan(PG_FUNCTION_ARGS);
extern Datum bm25rescan(PG_FUNCTION_ARGS);
extern Datum bm25gettuple(PG_FUNCTION_ARGS);
extern Datum bm25endscan(PG_FUNCTION_ARGS);
extern Datum bm25costestimate(PG_FUNCTION_ARGS);
extern Datum bm25insert(PG_FUNCTION_ARGS);
extern Datum bm25bulkdelete(PG_FUNCTION_ARGS);
extern Datum bm25vacuumcleanup(PG_FUNCTION_ARGS);
extern Datum bm25options(PG_FUNCTION_ARGS);

#define XLOG_BM25_BUILD_INDEX 0x00
#define XLOG_BM25_INIT_PAGE 0x10
#define XLOG_BM25_APPEND_PAGES 0x20
#define XLOG_BM25_INSERT_ENTRY 0x30
#define XLOG_BM25_INSERT_INPLACE_ENTRY 0x40
#define XLOG_BM25_INSERT_STATS 0x50
#define XLOG_BM25_UPDATE_INVERT_LIST 0x60
#define XLOG_BM25_ADD_DATA 0x70

struct xl_bm25_global_stats {
    AttrNumber attrno;
    uint64 length;
    uint64 tok_size;
};

struct xl_bm25_insert_entry {
    uint32 offset;
    size_t entry_size;
    uint16 nentry;
    xl_bm25_insert_entry() = default;
    xl_bm25_insert_entry(uint32 offset, size_t entry_size, uint16 nentry)
        : offset(offset), entry_size(entry_size), nentry(nentry) {}
};

struct xl_bm25_insert_inplace_entry {
    size_t bit_offset;
    size_t bit_size;
    size_t offset;
    uint32 entry_size;
    uint16 nentry;
    xl_bm25_insert_inplace_entry(size_t bit_offset, size_t bit_size, size_t offset,
                                 uint32 entry_size, uint16 nentry)
        : bit_offset(bit_offset),
          bit_size(bit_size),
          offset(offset),
          entry_size(entry_size),
          nentry(nentry) {}
};

struct xl_bm25_insert_stats {
    uint64 max_doc_id;
    uint32 offset;
    size_t data_size;
};

struct xl_bm25_add_data {
    uint32 offset;
    size_t data_size;
    xl_bm25_add_data() = default;
    xl_bm25_add_data(uint32 offset, size_t data_size) : offset(offset), data_size(data_size) {}
};

struct xl_bm25_update_invert_list {
    uint32 offset;
    size_t entry_size;
    uint16 nentry;
};

extern void bm25_redo(XLogReaderState *record);
extern void bm25_desc(StringInfo buf, XLogReaderState *record);
extern const char* bm25_type_name(uint8 subtype);
extern void Bm25XLogInitPage(Buffer buffer, Page page);
extern void Bm25XLogAppendPage(Buffer buffer, Page page);
extern void Bm25XLogInsertEntry(Buffer buffer, Page page, const xl_bm25_insert_entry &xl_rec);
extern void Bm25XLogInsertInplaceEntry(Buffer buffer, Page page, const xl_bm25_insert_inplace_entry &xl_rec);
extern void Bm25XLogInsertStats(Buffer buffer, Page page, const xl_bm25_insert_stats &xl_rec);
extern void Bm25XLogAddData(Buffer buffer, Page page, const xl_bm25_add_data &xl_rec);
extern void Bm25XLogUpdateInvertList(Buffer buffer, Page page, const xl_bm25_update_invert_list &xl_rec);

#endif /* BM25_H */
