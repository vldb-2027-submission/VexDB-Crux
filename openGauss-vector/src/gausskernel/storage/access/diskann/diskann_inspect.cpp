/**
 * Copyright ...
 */

#include <numeric>

#include <vtl/bitvector>
#include <vtl/disk_container/diskvector.hpp>
#include <vtl/disk_container/freespace.hpp>
#include "access/diskann/diskann.h"
#include "access/bm25/index_inspect.h"

using namespace disk_container;

extern int64 calculate_relation_size(RelFileNode* rfn, BackendId backend, ForkNumber forknum);

void *diskann_inspect(Relation index)
{
    IndexInspectResult *res = NEW IndexInspectResult;
    Buffer buf = ReadBuffer(index, DISKANN_METAPAGE_BLKNO);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    DiskAnnMetaPageBase *bmeta = DiskAnnPageGetMeta(BufferGetPage(buf));
    if (bmeta->magicNumber != DISKANN_MAGIC_NUMBER || bmeta->version != DISKANN_VERSION_ONE) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("diskann index meta page is corrupted")));
    }
    const uint32 dim = bmeta->dimensions;
    DiskAnnMetaPage *meta = (DiskAnnMetaPage *)bmeta;
    DiskVector<AnnNeighbors> graph(index, meta->graphMetaBlkNo, false);
    res->append_attr("Used Space");
    size_t total_space = size_t(RelationGetNumberOfBlocksInFork(index, MAIN_FORKNUM)) * BLCKSZ;
    total_space +=
        (smgrexists(index->rd_smgr, FSM_FORKNUM) ?
            calculate_relation_size(&(index->rd_node), index->rd_backend, FSM_FORKNUM) : 0) +
        (smgrexists(index->rd_smgr, VECTOR_FORKNUM) ?
            calculate_relation_size(&(index->rd_node), index->rd_backend, VECTOR_FORKNUM) : 0);
    res->fill_content(total_space);
    const size_t num_point = graph.size();
    size_t reserved_size = graph.capacity() - num_point;
    FreeSpace<size_t> fspace(index, meta->freespaceMetaBlkNo, false);
    reserved_size += fspace.size();
    fspace.destroy();
    if (num_point == 0) {
        graph.destroy();
        UnlockReleaseBuffer(buf);
        res->append_attr("Required Space");
        res->fill_content(0ul);
        res->append_attr("Space Utilization Rate");
        res->fill_content("0%%");
        res->append_attr("Total Points");
        res->fill_content("0");
        res->append_attr("Reserved Slot");
        res->fill_content("%lu", reserved_size);
        res->append_attr("Total Neighbors");
        res->fill_content("0");
        return res;
    }
    DiskVector<DiskAnnVamanaNode> node(index, meta->nodeMetaBlkNo, false);
    UnlockReleaseBuffer(buf);

    constexpr size_t node_per_page = DiskVector<DiskAnnVamanaNode>::n_data_per_block();
    size_t node_offset = 0;
    size_t graph_offset = 0;
    size_t nnode_valid = 0;
    size_t status_length = 0;
    BitSet<node_per_page> node_status;
    const auto set_node_status = [&]() {
        node.template visit<AccessorLockType::ReadLock>(
            [&](const DiskAnnVamanaNode *nodes, size_t nitems) {
                node_status.reset();
                for (size_t i = 0; i < nitems; ++i) {
                    if (!diskann_node_flag::is_valid(nodes[i].flag)) {
                        continue;
                    }
                    ++nnode_valid;
                    node_status.set(i);
                }
                node_offset += nitems;
                status_length = nitems;
            })(node_offset, std::min(num_point - node_offset, node_per_page));
    };
    const auto node_valid = [&]() -> bool {
        return node_status.get(status_length - (node_offset - graph_offset));
    };

    Vector<size_t> neighbor_count(MAX_ANN_GRAPH_DEGREE + 1, 0);
    graph.template visit<AccessorLockType::ReadLock>(
        [&](const AnnNeighbors *neighbors, size_t nneighbors) {
            for (size_t i = 0; i < nneighbors; ++i) {
                if (graph_offset >= node_offset) {
                    set_node_status();
                }
                if (node_valid()) {
                    ++neighbor_count[neighbors[i].num_neighbors];
                }
                ++graph_offset;
            }
        })(0, num_point);    /* we assume 0 is always valid */
    graph.destroy();
    node.destroy();

    size_t required_size = (sizeof(float) * dim + sizeof(AnnNeighbors) +
                            sizeof(DiskAnnVamanaNode)) * nnode_valid;
    res->append_attr("Required Space");
    res->fill_content(required_size);
    res->append_attr("Space Utilization Rate");
    res->fill_content("%f%%", double(required_size) / total_space * 100);
    res->append_attr("Total Points");
    res->fill_content("%lu", nnode_valid);
    res->append_attr("Reserved Slot");
    res->fill_content("%lu", reserved_size);
    res->append_attr("Total Neighbors");
    size_t total_neighbors = 0;
    for (size_t i = 0; i <= MAX_ANN_GRAPH_DEGREE; ++i) {
        total_neighbors += neighbor_count[i] * i;
    }
    res->fill_content("%lu", total_neighbors);
    res->append_attr("Average Number of Neighbors");
    const double mean = double(total_neighbors) / nnode_valid;
    res->fill_content("%.2f", mean);
    res->append_attr("Standard Deviation for Number of Neighbors");
    double s2 = 0.0;
    for (size_t i = 0; i <= MAX_ANN_GRAPH_DEGREE; ++i) {
        if (neighbor_count[i] > 0) {
            double delta = i - mean;
            s2 += delta * delta * neighbor_count[i];
        }
    }
    s2 /= nnode_valid;
    res->fill_content("%.2f", sqrt(s2));
    constexpr size_t n_percentile[] = {1ul, 5ul, 10ul, 25ul, 50ul, 75ul, 90ul, 95ul, 99ul};
    constexpr size_t n_report = sizeof(n_percentile) / sizeof(n_percentile[0]);
    size_t start_idx = 0;
    size_t count = 0;
    for (size_t i = 0; i <= MAX_ANN_GRAPH_DEGREE; ++i) {
        count += neighbor_count[i];
        for (size_t j = start_idx; j < n_report; ++j) {
            if (count < (nnode_valid * n_percentile[j] / 100ul)) {
                break;
            }
            res->append_attr("Percentile %lu%% of Neighbors Count", n_percentile[j]);
            res->fill_content("%d", i);
            ++start_idx;
        }
        if (start_idx >= n_report) {
            break;
        }
    }
    ann_helper::optional_destroy(neighbor_count);
    return res;
}

void *hybridann_inspect(Relation index)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("index_inspect does not support hybridann")));
    IndexInspectResult *res = NULL;
    return res;
}
