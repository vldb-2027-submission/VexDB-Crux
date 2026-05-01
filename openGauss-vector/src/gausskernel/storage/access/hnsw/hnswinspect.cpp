/**
 * Copyright ...
 */

#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#include <vtl/vector>
#include <vtl/hashtable>
#include <vtl/holder>

#include "access/hnsw/hnsw.h"
#include "access/bm25/index_inspect.h"

extern int64 calculate_relation_size(RelFileNode* rfn, BackendId backend, ForkNumber forknum);

void *hnsw_inspect(Relation index)
{
    Buffer buf = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
    Page page = BufferGetPage(buf);
    auto meta = HnswPageGetMeta(page);
    bool cluster_pq = meta->cluster.cluster_pq;
    ReleaseBuffer(buf);
    const int m = HnswGetM(index);
    const int m2 = m * 2;
    const size_t slot_size = HNSW_TUPLE_SIZE(0, m);
    size_t avail_slot = 0;
    size_t total_size_used = 0;
    size_t total_points = 0;
    size_t total_tids = 0;
    size_t total_neighbors = 0;
    double mean = 0;    /* used for neighbor# std */
    double s2 = 0;      /* used for neighbor# std */
    Vector<uint32> neighbor_count(uint32(m2 + 1), 0ul);    /* neighbor# percentiles */
    Vector<uint32> elem_count;
    Vector<size_t> level_counter;
    BlockNumber nblocks = 0;
    BlockNumber blkno = HNSW_HEAD_BLKNO;
    Holder<disk_container::PlainStore> ps;
    if (HnswUseCluster(index)) {
        ps.emplace(index, HNSW_PS_BLKNO, false);
    }
    do {
        CHECK_FOR_INTERRUPTS();
        ++nblocks;
        buf = ReadBuffer(index, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        if (HnswPageGetOpaque(page)->page_id != HNSW_PAGE_ID) {
            UnlockReleaseBuffer(buf);
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                errmsg("Found incorrect HNSW page opaque page id"),
                errhint("Index may be corrupted, please rebuild the index")));
        }
        avail_slot += PageGetFreeSpace(page) / slot_size;
        OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);
        OffsetNumber offno;
        for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
            HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, offno));
            /* points, tids and size */
            ++total_points;
            if (tuple->is_deleted()) {
                ++avail_slot;
            } else {
                total_size_used += HNSW_TUPLE_SIZE(tuple->level, m);
                uint32 ntid;
                if (!cluster_pq) {
                    ntid = tuple->actual_ntids(index, ps);
                    if (tuple->is_extended()) {
                        total_size_used += ntid * sizeof(ItemPointerData);
                    }
                } else {
                    size_t used_size;
                    ntid = tuple->actual_ntids_pq(index, 128u, ps, used_size);
                    total_size_used += used_size;
                }
                total_tids += ntid;
                elem_count.push_back(ntid);
            }

            /* levels */
            if (level_counter.size() <= tuple->level) {
                level_counter.resize(tuple->level + 1, 0);
            }
            ++level_counter[tuple->level];

            /* neighbors */
            int i;
            for (i = 0; i < m2; ++i) {
                if (!ItemPointerIsValid(&tuple->neighbors[i].indexTid)) {
                    break;
                }
            }

            double delta = (double)i - mean;
            mean += delta / total_points;
            double delta_new = (double)i - mean;
            s2 += delta * delta_new;

            total_neighbors += size_t(i);
            ++neighbor_count[i];

            int start_idx = m2;
            for (uint8 l = 1u; l < tuple->level; ++l) {
                for (i = 0; i < m; ++i) {
                    if (!ItemPointerIsValid(&tuple->neighbors[start_idx + i].indexTid)) {
                        break;
                    }
                }
                start_idx += m;
                total_neighbors += size_t(i);
            }
        }
        blkno = HnswPageGetOpaque(page)->nextblkno;
        UnlockReleaseBuffer(buf);
    } while (BlockNumberIsValid(blkno));
    if (HnswUseCluster(index)) {
        ps->destroy();
    }

    IndexInspectResult *res = NEW IndexInspectResult;
    const size_t vector_size = smgrexists(index->rd_smgr, VECTOR_FORKNUM) ?
        calculate_relation_size(&(index->rd_node), index->rd_backend, VECTOR_FORKNUM) : 0;
    size_t total_size = size_t(RelationGetNumberOfBlocksInFork(index, MAIN_FORKNUM)) * BLCKSZ;
    total_size += vector_size + (smgrexists(index->rd_smgr, FSM_FORKNUM) ?
        calculate_relation_size(&(index->rd_node), index->rd_backend, FSM_FORKNUM) : 0);
    total_size_used += vector_size;
    res->append_attr("Used Space");
    res->fill_content(total_size);
    res->append_attr("Required Space");
    res->fill_content(total_size_used);
    res->append_attr("Space Utilization Rate");
    res->fill_content("%f%%", double(total_size_used) / total_size * 100);
    res->append_attr("Available Slot#");
    res->fill_content("%lu", avail_slot);
    res->append_attr("Total Points");
    res->fill_content("%lu", total_points);
    res->append_attr("Total Elements");
    res->fill_content("%lu", total_tids);
    res->append_attr("Total Neighbors");
    res->fill_content("%lu", total_neighbors);
    for (size_t i = 0; i < level_counter.size(); ++i) {
        if (level_counter[i] == 0) {
            continue;
        }
        res->append_attr("Number of elements reaching level %lu", i);
        res->fill_content("%lu", level_counter[i]);
    }
    constexpr size_t n_percentile[] = {1ul, 5ul, 10ul, 25ul, 50ul, 75ul, 90ul, 95ul, 99ul};
    constexpr size_t n_report = sizeof(n_percentile) / sizeof(n_percentile[0]);
    if (total_points > 0) {
        res->append_attr("Average Number of Bottom Neighbors");
        res->fill_content("%.2f", mean);
        res->append_attr("Standard Deviation of Bottom Neighbors");
        res->fill_content("%.2f", sqrt(s2 / total_points));
        size_t start_idx = 0;
        size_t count = 0;
        for (int i = 0; i <= m2; ++i) {
            count += neighbor_count[i];
            for (size_t j = start_idx; j < n_report; ++j) {
                if (count < (total_points * n_percentile[j] / 100ul)) {
                    break;
                }
                res->append_attr("Percentile %lu%% of Bottom Neighbors Count", n_percentile[j]);
                res->fill_content("%d", i);
                ++start_idx;
            }
            if (start_idx >= n_report) {
                break;
            }
        }
    }

    if (HnswUseCluster(index)) {
        const size_t ncenter = elem_count.size();
        size_t total_elem = 0;
        for (size_t count : elem_count) {
            total_elem += count;
        }
        res->append_attr("Average Elements per Center");
        const double avg_elem = double(total_elem) / ncenter;
        res->fill_content("%.2f", avg_elem);
        res->append_attr("Standard Deviation for Elements per Center");
        s2 = 0.0;
        for (size_t count : elem_count) {
            double delta = count - avg_elem;
            s2 += delta * delta;
        }
        s2 /= ncenter;
        res->fill_content("%.2f", sqrt(s2));
        std::sort(elem_count.begin(), elem_count.end());
        for (size_t i = 0; i < n_report; ++i) {
            size_t idx = ncenter * n_percentile[i] / 100ul;
            res->append_attr("Percentile %lu%% of Element Count", n_percentile[i]);
            res->fill_content("%d", elem_count[idx]);
        }
    }

    ann_helper::optional_destroy(neighbor_count);
    ann_helper::optional_destroy(elem_count);
    ann_helper::optional_destroy(level_counter);
    return res;
}
