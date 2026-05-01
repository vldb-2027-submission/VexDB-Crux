/**
 * Copyright ...
 */

#include <vtl/disk_container/diskvector.hpp>
#include <vtl/disk_container/freespace.hpp>

#include "access/bm25/bm25_internal.h"
#include "access/bm25/bm25_parameters.h"
#include "access/bm25/tokenizer/tokenizer.h"
#include "access/annvector/halfutils.h"
#include "catalog/dependency.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_partition_fn.h"
#include "utils/fmgroids.h"

using namespace bm25;
using namespace disk_container;
using namespace ann_helper;

void record_dependency(Relation index, ObjectAddress *dict_addr, int ndict)
{
    if (ndict <= 0) {
        return;
    }

    ObjectAddress self;
    if (RelationIsPartition(index)) {
        ObjectAddressSet(self, RelationRelationId, GetBaseRelOidOfParition(index));
    } else {
        ObjectAddressSet(self, RelationRelationId, RelationGetRelid(index));
    }
    Relation dep_rel = heap_open(DependRelationId, RowExclusiveLock);
    ScanKeyData key[2];
    ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(self.classId));
    ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(self.objectId));
    SysScanDesc scan = systable_beginscan(dep_rel, DependDependerIndexId, true, NULL, 2, key);
    bool dependency_already_recorded = false;
    HeapTuple tup;
    while (HeapTupleIsValid((tup = systable_getnext(scan)))) {
        Form_pg_depend depform = (Form_pg_depend)GETSTRUCT(tup);
        if (depform->refclassid == dict_addr->classId && depform->refobjid == dict_addr->objectId) {
            dependency_already_recorded = true;
            break;
        }
    }
    systable_endscan(scan);
    heap_close(dep_rel, RowExclusiveLock);
    if (!dependency_already_recorded) {
        return;
    }
    recordMultipleDependencies(&self, dict_addr, ndict, DEPENDENCY_NORMAL);
}

void BM25MetaPageData::init(Relation index)
{
    magic = BM25_META_MAGIC;
    version = BM25_VERSION;
    nattr = uint32(RelationGetDescr(index)->natts);
    if (RelationIsGlobalIndex(index)) {
        --nattr;
    }
    if (nattr > BM25_MAX_NATTR || nattr == 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("the number of indexed columns exceeds the maximum number %d",
                               BM25_MAX_NATTR)));
    }
    extract_dict(get_bm25_dictionaries(index), dict_ids, nattr);
    for (uint32 i = 0; i < nattr; ++i) {
        const Oid aid = RelationGetDescr(index)->attrs[i].atttypid;
        if (aid == TEXTARRAYOID || aid == VARCHARARRAYOID || aid == BPCHARARRAYOID) {
            dict_ids[i] = InvalidOid;
        } else if (aid == SPARSEVECTOROID) {
            dict_ids[i] = InvalidOid;
            is_sparse_vector |= 1 << i;
        }
    }
    UnorderedSet<Oid> dict_set(nattr);
    for (uint32 i = 0; i < nattr; ++i) {
        Oid dict_id = dict_ids[i];
        if (OidIsValid(dict_id) && dict_id != bm25_tokenizer::default_jieba_dict_id) {
            dict_set.insert(dict_id);
        }
    }
    int ndict = dict_set.size();
    ObjectAddress dict_addr[ndict];
    uint32 offset = 0;
    for (Oid dict_id : dict_set) {
        ObjectAddressSet(dict_addr[offset], TSDictionaryRelationId, dict_id);
        ++offset;
    }
    optional_destroy(dict_set);
    record_dependency(index, dict_addr, ndict);
    extract_scorer(get_bm25_algorithms(index), get_bm25_coefficients(index), scorer, nattr, index);
    TokenIndex::create_token_index(index, is_sparse_vector, tok_idx_meta, false);
    DocumentStore::create_doc_store(index, doc_store_meta, nattr, false);
    il_meta.init(index);
}

void BM25StatisticsPageData::init()
{
    magic = BM25_STATS_MAGIC;
    version = BM25_STATS_VERSION;
    max_doc_id = 0;
    for (size_t i = 0; i < BM25_MAX_NATTR; ++i) {
        stats[i].total_length = 0;
        stats[i].total_distinct = 0;
        stats[i].total_doc = 0;
    }
}

BM25Store::BM25Store(Relation index, bool need_wal)
        : _meta_buf(ReadBuffer(index, BM25_META_BLKNO)),
          _meta(GetBM25MetaPage(BufferGetPage(_meta_buf))),
          _tok_index(index, _meta->tok_idx_meta, _meta_buf, need_wal),
          _doc_store(index, _meta->doc_store_meta, get_nattr(), _tok_index.need_wal()),
          _sparse_vec_bits(_meta->is_sparse_vector) {}

void BM25Store::destroy()
{
    ann_helper::LeakChecker::destroy();
    _tok_index.destroy();
    _doc_store.destroy();
    if (BufferIsValid(_meta_buf)) {
        ReleaseBuffer(_meta_buf);
        _meta_buf = InvalidBuffer;
    }
}

static void get_all_global_stats(Relation index, uint32 nattr, GlobalStats *stats)
{
    Buffer buf = ReadBuffer(index, BM25_STATISTICS_BLKNO);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    BM25StatisticsPage page = GetBM25StatisticsPage(BufferGetPage(buf));
    for (uint32 i = 0; i < nattr; ++i) {
        stats[i] = page->stats[i];
    }
    UnlockReleaseBuffer(buf);
}

uint64 BM25Store::insert_global_stats(Relation index, Documents &docs, const DocumentStats *stats,
                                      bool need_wal)
{
    bool empty = true;
    for (const auto &cd : docs.docs) {
        if (is_sparse_vector(cd.attrno)) {
            if (cd.vec->nnz > 0) {
                empty = false;
                break;
            }
            continue;
        }
        if (cd.doc.tok_size > 0) {
            empty = false;
            break;
        }
    }
    if (empty) {
        return 0;
    }

    Buffer buf = ReadBuffer(index, BM25_STATISTICS_BLKNO);
    BM25StatisticsPage page = GetBM25StatisticsPage(BufferGetPage(buf));
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    uint64 res = ++page->max_doc_id;
    AttrNumber max_writen_attrno = InvalidAttrNumber;
    AttrNumber min_writen_attrno = INT16_MAX;
    for (const auto &cd : docs.docs) {
        Assert(AttributeNumberIsValid(cd.attrno));
        GlobalStats &stat = page->stats[cd.attrno - 1];
        if (is_sparse_vector(cd.attrno)) {
            stat.total_norm += stats[cd.attrno - 1].norm;
            stat.total_distinct += cd.vec->nnz;
        } else {
            stat.total_length += stats[cd.attrno - 1].length;
            stat.total_distinct += cd.doc.tok_size;
        }
        ++stat.total_doc;
        if (max_writen_attrno < cd.attrno) {
            max_writen_attrno = cd.attrno;
        }
        if (min_writen_attrno > cd.attrno) {
            Assert(cd.attrno > 0);
            min_writen_attrno = cd.attrno;
        }
    }
    MarkBufferDirty(buf);
    if (need_wal) {
        if (!AttributeNumberIsValid(max_writen_attrno) || max_writen_attrno < min_writen_attrno) {
            Bm25XLogInsertStats(buf, BufferGetPage(buf), {res, 0, 0});
        }
        Bm25XLogInsertStats(buf, BufferGetPage(buf), {
            res,
            uint32(MAXALIGN(SizeOfPageHeaderData) + offsetof(BM25StatisticsPageData, stats) +
                (min_writen_attrno - 1) * sizeof(GlobalStats)),
            sizeof(GlobalStats) * (max_writen_attrno + 1 - min_writen_attrno)
        });
    }
    UnlockReleaseBuffer(buf);
    return res;
}

void BM25Store::insert(Documents &docs)
{
    DocumentStats stats[get_nattr()];
    for (const auto &cd : docs.docs) {
        if (is_sparse_vector(cd.attrno)) {
            stats[cd.attrno - 1].norm = sparsevector_norm(cd.vec);
            continue;
        }
        float length = 0;
        for (uint32 i = 0; i < cd.doc.tok_size; ++i) {
            length += cd.doc.frequencies[i];
        }
        stats[cd.attrno - 1].length = length;
    }
    uint64 doc_id = insert_global_stats(_tok_index.get_relation(), docs, stats,
                                        _tok_index.need_wal());
    if (doc_id == 0) {
        return;
    }
    _doc_store.store(doc_id, stats, docs.tid, docs.part_id);
    union {
        Token tok;
        SparseElement se;
    };
    for (const auto &cd : docs.docs) {
        if (is_sparse_vector(cd.attrno)) {
            se.attrno = cd.attrno;
            const SparseVector *sv = cd.vec;
            const uint16 *v = SPARSEVEC_VALUES(sv);
            for (uint32 i = 0; i < sv->nnz; ++i) {
                se.dim = sv->indices[i];
                se.v = v[i];
                _tok_index.insert(se, doc_id, &_meta->il_meta);
            }
            continue;
        }
        tok.attrno = cd.attrno;
        for (uint32 i = 0; i < cd.doc.tok_size; ++i) {
            tok.tok = cd.doc.toks[i];
            _tok_index.insert(tok, doc_id, cd.doc.frequencies[i], &_meta->il_meta);
        }
    }
}

void BM25Store::vacuum(IndexBulkDeleteCallback callback, void *callback_state,
                       IndexBulkDeleteResult *stats)
{
    double removed_length[get_nattr()] = {0.0};
    uint32 removed_doc[get_nattr()] = {0};
    doc_id_track id_track =
        _doc_store.vacuum(callback, callback_state, removed_length, removed_doc, stats);
    _tok_index.vacuum(id_track, stats, _meta);
    IndexFreeSpaceMapVacuum(_tok_index.get_relation());
    Buffer buf = ReadBuffer(_tok_index.get_relation(), BM25_STATISTICS_BLKNO);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    BM25StatisticsPage page = GetBM25StatisticsPage(BufferGetPage(buf));
    const uint16 nattr = get_nattr();
    bool modified = false;
    for (uint16 i = 0; i < nattr; ++i) {
        if (removed_doc[i] == 0) {
            continue;
        }
        page->stats[i].total_doc -= removed_doc[i];
        double removed_ratio = 1.0 - removed_length[i] / page->stats[i].total_length;
        if (!is_sparse_vector(i + 1)) {
            page->stats[i].total_length -= removed_length[i];
        } else {
            page->stats[i].total_norm -= removed_length[i];
        }
        /* TD: mark it estimated or make it accurate */
        page->stats[i].total_distinct *= removed_ratio;
        modified = true;
    }
    if (modified) {
        MarkBufferDirty(buf);
        if (_tok_index.need_wal()) {
            Bm25XLogAddData(buf, BufferGetPage(buf), {
                MAXALIGN(SizeOfPageHeaderData) + offsetof(BM25StatisticsPageData, stats),
                sizeof(GlobalStats) * nattr
            });
        }
    }
    UnlockReleaseBuffer(buf);
    ann_helper::optional_destroy(id_track);
}

template <uint16 l>
static void short_inverted_list_inspect_helper(IndexInspectResult &res, Relation index,
                                               BlockNumber vec_blkno, BlockNumber fs_blkno)
{
    using IL_TYPE = FixedInvertedList<il_threshold_levels[l]>;
    DiskVector<IL_TYPE> vec(index, vec_blkno, false);
    FreeSpace<uint32> fs(index, fs_blkno, false);
    res.append_attr("Short Inverted List Type %u Used Size", l);
    res.fill_content((vec.get_nblocks() + fs.get_nblocks()) * BLCKSZ);
    res.append_attr("Short Inverted List Type %u Required Size", l);
    res.fill_content(vec.size() * sizeof(IL_TYPE));
    res.append_attr("Short Inverted List Type %u Number of Entries", l);
    res.fill_content("%lu", vec.size() - fs.size());
    res.append_attr("Short Inverted List Type %u Reserved Number of Entries", l);
    res.fill_content("%lu", vec.capacity() - vec.size() + fs.size());
    vec.destroy();
    fs.destroy();
}

void BM25Store::inspect(IndexInspectResult &res)
{
    constexpr uint32 N_TOKEN_TYPE = 4u;
    constexpr uint32 N_DIM_TYPE = 2u;
    const uint32 nattr = _meta->nattr;
    Relation index = _tok_index.get_relation();
    auto doc_stats = _doc_store.get_stats();
    Vector<DiskHashTableStats> tok_stats;
    for (uint32 i = 0; i < nattr; ++i) {
        if (!is_sparse_vector(i + 1)) {
            TokenIndex::short_store s(index, _meta->tok_idx_meta.attr_meta[i].short_store_blkno, false);
            tok_stats.push_back(s.get_stats());
            s.destroy();
            TokenIndex::mid_store m(index, _meta->tok_idx_meta.attr_meta[i].mid_store_blkno, false);
            tok_stats.push_back(m.get_stats());
            m.destroy();
            TokenIndex::long_store l(index, _meta->tok_idx_meta.attr_meta[i].long_store_blkno, false);
            tok_stats.push_back(l.get_stats());
            l.destroy();
            TokenIndex::full_store f(index, _meta->tok_idx_meta.attr_meta[i].full_store_blkno, false);
            tok_stats.push_back(f.get_stats());
            f.destroy();
        } else {
            TokenIndex::dim_store s(index, _meta->tok_idx_meta.attr_meta[i].short_store_blkno, false);
            tok_stats.push_back(s.get_stats());
            s.destroy();
            TokenIndex::dim_store m(index, _meta->tok_idx_meta.attr_meta[i].mid_store_blkno, false);
            tok_stats.push_back(m.get_stats());
            m.destroy();
            tok_stats.emplace_back();
            tok_stats.emplace_back();
        }
    }

    const size_t total_blk = RelationGetNumberOfBlocksInFork(index, MAIN_FORKNUM);
    const size_t total_size = total_blk * BLCKSZ;
    const size_t doc_store_blk = doc_stats.nblock;
    const size_t doc_store_size = doc_stats.nentry * _doc_store.base_data_size();
    size_t token_blk = 0;
    size_t token_size = 0;
    size_t ntoken = 0;
    size_t ndim = 0;
    size_t ndoc = 0;
    size_t nvec = 0;
    size_t il_size = 0;
    GlobalStats gs[nattr];

    for (uint32 i = 0; i < nattr; ++i) {
        if (!is_sparse_vector(i + 1)) {
            for (uint32 j = 0; j < N_TOKEN_TYPE; ++j) {
                token_blk += tok_stats[i * N_TOKEN_TYPE + j].nblock;
                token_size += tok_stats[i * N_TOKEN_TYPE + j].nentry * (
                    j == 0  ? sizeof(TokenIndex::short_store::kv_base) : (
                    j == 1u ? sizeof(TokenIndex::mid_store::kv_base) : (
                    j == 2u ? sizeof(TokenIndex::long_store::kv_base) :
                            sizeof(TokenIndex::full_store::kv_base)
                )));
                ntoken += tok_stats[i * N_TOKEN_TYPE + j].nentry;
            }
        } else {
            for (uint32 j = 0; j < N_DIM_TYPE; ++j) {
                token_blk += tok_stats[i * N_TOKEN_TYPE + j].nblock;
                token_size +=
                    tok_stats[i * N_TOKEN_TYPE + j].nentry * sizeof(TokenIndex::dim_store::kv_base);
                ndim += tok_stats[i * N_TOKEN_TYPE + j].nentry;
            }
        }
    }

    get_all_global_stats(index, nattr, gs);
    for (uint32 i = 0; i < nattr; ++i) {
        il_size += gs[i].total_distinct * sizeof(InvertedListEntry);
        if (!is_sparse_vector(i + 1)) {
            ndoc += gs[i].total_doc;
        } else {
            nvec += gs[i].total_doc;
        }
    }

    const size_t required_size = doc_store_size + token_size + il_size;
    res.append_attr("Used Space");
    res.fill_content(total_size);
    res.append_attr("Required Space");
    res.fill_content(required_size);
    res.append_attr("Space Utilization Rate");
    res.fill_content("%f%%", double(required_size) / total_size * 100);
    if (ndoc > 0) {
        res.append_attr("Number of documents");
        res.fill_content("%lu", ndoc);
    }
    if (nvec > 0) {
        res.append_attr("Number of sparse vectors");
        res.fill_content("%lu", nvec);
    }
    if (ntoken > 0) {
        res.append_attr("Number of distinct tokens");
        res.fill_content("%lu", ntoken);
    }
    if (ndim > 0) {
        res.append_attr("Number of distinct dimensions in sparse vectors");
        res.fill_content("%lu", ndim);
    }
    for (uint32 i = 0; i < nattr; ++i) {
        const char *attname = NameStr(index->rd_att->attrs[i].attname);
        if (!is_sparse_vector(i + 1)) {
            res.append_attr("  Attribute %s: average doc length", attname);
            res.fill_content("%.2f", double(gs[i].total_length) / gs[i].total_doc);
            res.append_attr("  Attribute %s: number of short-length tokens", attname);
            res.fill_content("%lu", tok_stats[i * N_TOKEN_TYPE].nentry);
            res.append_attr("  Attribute %s: number of mid-length tokens", attname);
            res.fill_content("%lu", tok_stats[i * N_TOKEN_TYPE + 1u].nentry);
            res.append_attr("  Attribute %s: number of long-length tokens", attname);
            res.fill_content("%lu", tok_stats[i * N_TOKEN_TYPE + 2u].nentry);
            res.append_attr("  Attribute %s: number of full tokens", attname);
            res.fill_content("%lu", tok_stats[i * N_TOKEN_TYPE + 3u].nentry);
        } else {
            res.append_attr("  Attribute %s: average norm", attname);
            res.fill_content("%.2f", double(gs[i].total_norm) / gs[i].total_doc);
            res.append_attr("  Attribute %s: number of positive values", attname);
            res.fill_content("%lu", tok_stats[i * N_TOKEN_TYPE].nentry);
            res.append_attr("  Attribute %s: number of negative values", attname);
            res.fill_content("%lu", tok_stats[i * N_TOKEN_TYPE + 1u].nentry);
        }
    }

    res.append_attr("Document Store Used Size");
    res.fill_content(doc_store_blk * BLCKSZ);
    res.append_attr("Document Store Required Size");
    res.fill_content(doc_store_size);
    res.append_attr("Token/Dim Store Used Size");
    res.fill_content(token_blk * BLCKSZ);
    res.append_attr("Token/Dim Store Required Size");
    res.fill_content(token_size);

    static_assert(n_il_level == 3u, "n_il_level must be 3");
    short_inverted_list_inspect_helper<0>(res, index,
        _meta->il_meta.vec_blknos[0], _meta->il_meta.free_space_blknos[0]);
    short_inverted_list_inspect_helper<1>(res, index,
        _meta->il_meta.vec_blknos[1], _meta->il_meta.free_space_blknos[1]);
    short_inverted_list_inspect_helper<2>(res, index,
        _meta->il_meta.vec_blknos[2], _meta->il_meta.free_space_blknos[2]);
    res.append_attr("Inverted List Used Size Total");
    res.fill_content((total_blk - token_blk - doc_store_blk - 2ul) * BLCKSZ);
    res.append_attr("Inverted List Required Size Total");
    res.fill_content(il_size);

    doc_stats.destroy();
    tok_stats.destroy();

    // _tok_index.verify(_meta);
}

bool BM25Store::get_doc_info(uint64 doc_id, DocumentStats *stats, ItemPointer tid, Oid &part_id)
    { return _doc_store.get(doc_id, stats, tid, part_id); }

static float get_max_contribution(const Scorer &scorer, const GlobalStats &gstat, float wscore)
{
    constexpr uint16 max_freq = 16u;
    DocumentStats ds = {float(gstat.total_length) / gstat.total_doc};
    return doc_score(scorer, gstat, ds, max_freq) * wscore;
}

/**
 * Use result.size() == query.tok_size to verify whether the query has a full cover,
 * we don't have a partial match yet so there is no way a token corresponds to multiple lists.
 */
Vector<QueryContextEntry> BM25Store::generate_context(const QueryToken &query,
    GlobalStats *global_stats, bool need_score, const QueryGroupParam &param)
{
    BM25LOG("Converting query to entry");
    const size_t s = query.toks.size();
    Vector<QueryContextEntry> res(s);
    Relation index = _tok_index.get_relation();
    float wscore = 0;
    float max_wscore = 0;
    if (query.is_text) {
        Token tok;
        tok.attrno = query.attrno;
        TokenIndexEntry entry;
        Optional<Vector<uint16>> freqs;
        if (param.norm) {
            freqs.emplace(s);
        }
        for (uint32 i = 0; i < s; ++i) {
            tok.tok = query.toks[i].tok;
            if (!_tok_index.get(tok, entry)) {
                BM25LOG("No corresponding list for token \"%s\"(%hd)", tok.tok, tok.attrno);
                continue;
            }
            if (param.norm) {
                freqs->push_back(query.frequencies[i]);
            }
            if (need_score) {
                wscore = param.boost * query.frequencies[i] *
                    word_score(_meta->scorer[tok.attrno - 1], global_stats[tok.attrno - 1],
                               entry.stats);
                max_wscore = get_max_contribution(_meta->scorer[tok.attrno - 1],
                    global_stats[tok.attrno - 1], wscore);
            }
            res.emplace_back(wscore, max_wscore, tok.attrno, entry.stats.ndoc, index,
                             entry.start_blkno, entry.insert_blkno, &_meta->il_meta);
            BM25LOG("Token \"%s\"(%hd) converts to list %u:%u "
                    "with length %u score %f (boost %f) max_contribution %f",
                    tok.tok, tok.attrno, entry.start_blkno, entry.insert_blkno,
                    entry.stats.ndoc, wscore, param.boost, max_wscore);
        }
        if (param.norm) {
            uint32 best_doc_length = 0;
            for (uint16 i : query.frequencies) {
                best_doc_length += i;
            }
            const DocumentStats ds{(float)best_doc_length};
            const auto &scorer = _meta->scorer[query.attrno - 1];
            const auto &gs = global_stats[query.attrno - 1];
            float max_score = 0;
            for (size_t i = 0; i < res.size(); ++i) {
                max_score += res[i].wscore * doc_score(scorer, gs, ds, freqs.value()[i]);
            }
            max_score /= param.boost;
            if (s > res.size()) {
                constexpr TokenStats ts{1u};
                max_score += (s - res.size()) * word_score(scorer, gs, ts) *
                    doc_score(scorer, gs, ds, 1u);
            }
            freqs->destroy();
            for (auto &qe : res) {
                qe.wscore /= max_score;
                qe.max_wscore /= max_score;
            }
            BM25LOG("Entry normalized by factor 1/%f", max_score);
        }
    } else {
        Assert(need_score);
        Assert(is_sparse_vector(query.attrno));
        SparseElement se;
        se.attrno = query.attrno;
        SparseDimIndexEntry entry;
        for (uint32 i = 0; i < s; ++i) {
            se.dim = query.toks[i].dim;
            se.v = query.frequencies[i];
            if (!_tok_index.get(se, entry)) {
                BM25LOG("No corresponding list for token %u(%f)(%hd)",
                        se.dim, half_to_float_unsigned(se.v), se.attrno);
                continue;
            }
            wscore = param.boost * half_to_float_unsigned(se.v);
            max_wscore = wscore * half_to_float_unsigned(entry.stats.max_score);
            res.emplace_back(wscore, max_wscore, se.attrno, entry.stats.ndoc, index,
                             entry.start_blkno, entry.insert_blkno, &_meta->il_meta);
            BM25LOG("SEdim %u(%f)(%hd) converts to list %u:%u "
                    "with length %u score %f (boost %f) max_contribution %f",
                    se.dim, half_to_float_unsigned(se.v), se.attrno, entry.start_blkno,
                    entry.insert_blkno, entry.stats.ndoc, wscore, param.boost, max_wscore);
        }
    }
    return res;
}

void BM25Store::preprocess_query_context(QueryContext &query_context, QueryGroup *parsed_top_filter,
    QueryGroup *parsed_queries, uint16 nquery)
{
    const uint32 nattr = query_context.nattr = get_nattr();
    query_context.global_stats = (GlobalStats *)palloc(sizeof(GlobalStats) * nattr);
    get_all_global_stats(_tok_index.get_relation(), nattr, query_context.global_stats);
    query_context.scorers = (PostDocScorer **)palloc(sizeof(PostDocScorer *) * nattr);
    for (uint32 i = 0; i < nattr; ++i) {
        query_context.scorers[i] =
            PostDocScorer::get_doc_scorer(_meta->scorer[i], query_context.global_stats[i]);
    }

    size_t idx = 0;
    if (parsed_top_filter) {
        Vector<QueryGroup *> track;
        track.push_back(parsed_top_filter);
        while (idx < track.size()) {
            QueryGroup *cur = track[idx];
            cur->nchild_group = cur->child_group.size();
            if (cur->nchild_group > 0) {
                uint16 *ccur = cur->child_group_idx =
                    (uint16 *)palloc(sizeof(uint16) * cur->nchild_group);
                for (auto &i : cur->child_group) {
                    *ccur = track.size();
                    ++ccur;
                    track.push_back(&i);
                }
            } else {
                Assert(cur->child_group_idx == NULL);
            }
            cur->idx = idx;
            ++idx;
            query_context.group_base.push_back(std::move(*cur));
        }
        ann_helper::optional_destroy(track);
        idx = 0;

        for (auto &group : query_context.group_base) {
            bool full_cover = true;
            if (group.query_tokens.toks.empty()) {
                group.query_tokens.index = uint16(-1);
            } else {
                query_context.entries.push_back(generate_context(group.query_tokens,
                    query_context.global_stats, group.get_need_score(), group.param));
                group.query_tokens.index = idx++;
                if (query_context.entries.back().size() != group.query_tokens.toks.size()) {
                    full_cover = false;
                }
            }
            group.set_full_cover(full_cover);
            ann_helper::optional_destroy(group.child_group);
        }
        query_context.results.resize(query_context.group_base.size());
    }

    for (uint16 i = 0; i < nquery; ++i) {
        auto &pq = parsed_queries[i];
        bool full_cover = true;
        if (pq.query_tokens.toks.empty()) {
            pq.query_tokens.index = uint16(-1);
        } else {
            query_context.entries.push_back(
                generate_context(pq.query_tokens, query_context.global_stats, true, pq.param));
            pq.query_tokens.index = idx++;
            if (query_context.entries.back().size() != pq.query_tokens.toks.size()) {
                full_cover = false;
            }
        }
        pq.set_full_cover(full_cover);
        ann_helper::optional_destroy(pq.child_group);
        query_context.query_base.push_back(std::move(pq));
    }
    if (parsed_queries) {
        pfree(parsed_queries);
    }
}
