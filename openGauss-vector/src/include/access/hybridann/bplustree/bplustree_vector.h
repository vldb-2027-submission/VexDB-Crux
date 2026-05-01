/**
 * Copyright ...
 * B+ tree Vector Index implementation.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_VECTOR_H
#define DISKANN_CONTAINER_BPLUSTREE_VECTOR_H

#include "access/hybridann/bplustree/bplustree.h"
#include "access/index_backend/taskpool.h"
#include "postmaster/bgworker.h"
#include "catalog/pg_partition_fn.h"
#include "access/annvector/ann_utils.h"

namespace disk_container {
class VectorBPlusTree : virtual public BPlusTreeImpl {
    using super = BPlusTreeImpl;
public:
    using super::BPlusTreeImpl; /* inherit constructor */

    void buildVectorIndexes()
    {
        const size_t index_magnitude_size = _ctx.index_magnitude()->size();
        const auto set_none_index = [&](NodeImpl &node, size_t start_slot) {
            Vector<BlockNumber> nodes;
            nodes.emplace_back(node.ptr());
            for (size_t i = start_slot; i < index_magnitude_size; ++i) {
                BlockNumber index_meta_blkno = create_vector_index_metapage(
                    _ctx.get_index(), VectorIndexType::None, i, node.ptr(), &nodes);
                node.set_index_ptr(i, index_meta_blkno);
            }
            ann_helper::optional_destroy(nodes);
        };

        Vector<BlockNumber> path;
        iterate_to_leaf_leftmost(path);
        if (path.size() <= 1ul) {
            Assert(!path.empty()); /* there should be at least a root node */
            NodeImpl node = _ctx.get_node(path.front());
            ann_helper::optional_destroy(path);
            /* no need to lock node as we are building it */
            set_none_index(node, 0);
            node.mark_dirty();
            node.destroy();
            return;
        }

        Vector<size_t> &index_magnitudes = _ctx.index_magnitude()->magnitudes();
        size_t node_tuple_capacities[path.size()];
        size_t node_tuple_counts[path.size()];
        for (int i = path.size() - 1 ; i >= 0; --i) {
            BlockNumber blkno = path[i];
            NodeImpl node = _ctx.get_node(blkno);
            node.r_lock();
            size_t capacity = 0;
            size_t count = 0;
            if (node.is_leaf()) {
                capacity = (size_t)((node.size() - p_firstdatakey(node) + 1) / node.get_page_used_ratio());
                count = node.size() - p_firstdatakey(node) + 1;
            } else {
                size_t lower_cap = node_tuple_capacities[i + 1];
                size_t lower_count = node_tuple_counts[i + 1];
                capacity = (size_t)(((node.size() - p_firstdatakey(node) + 1) * lower_cap) /
                    node.get_page_used_ratio());
                count = (node.size() - p_firstdatakey(node) + 1) * lower_count;
            }

            node_tuple_capacities[i] = capacity;
            node_tuple_counts[i] = count;
            node.r_unlock_destroy();
        }

        const auto create_vec_idx_meta = [](BlockNumber *from, BlockNumber *to,
            Relation index, Relation heap, VectorIndexType indexType, size_t index_magnitude_level,
            BlockNumber prev_meta_blkno) -> BlockNumber
        {
            Vector<BlockNumber> nodes(from, to);
            BlockNumber index_meta_blkno = create_vector_index_metapage(
                index, indexType, index_magnitude_level, *from, &nodes);
            ann_helper::optional_destroy(nodes);

            if (BlockNumberIsValid(prev_meta_blkno)) {
                VectorIndex *vectorIndex = VectorIndexFactory::create(index, heap, prev_meta_blkno);
                vectorIndex->update_next_index_meta_blkno(index_meta_blkno, false);
                delete_vector_index(vectorIndex);
            }
            return index_meta_blkno;
        };

        const auto calc_vector_index_type = [this, &index_magnitudes](size_t total_size, size_t level, size_t &nindex)
                -> VectorIndexType {
            nindex = total_size / index_magnitudes[level];
            if (total_size % index_magnitudes[level] >= index_magnitudes[level] / 2) {
                nindex += 1;
            }
            if (nindex == 0) {
                nindex = 1;
                return VectorIndexType::None;
            }
            return get_vector_index_type(level, _ctx.index_magnitude()->graph_entry_level());
        };

        const auto get_min_category = [&index_magnitudes](size_t capacity) -> size_t {
            for (size_t ic = 0; ic < index_magnitudes.size(); ++ic) {
                if (capacity <= 2 * index_magnitudes[ic]) {
                    return ic;
                }
            }
            /* an invalid index */
            return index_magnitudes.size();
        };

        Vector<BlockNumber> vector_index_blknos;
        size_t upper_cat = 0;
        for (uint16 i = 0; i < path.size(); ++i) {
            BlockNumber blkno = path[i];
            size_t min_cat = get_min_category(node_tuple_capacities[i]);
            if (min_cat >= index_magnitude_size) { 
                upper_cat = min_cat;
                continue;
            }
            if (i == 0) {
                /* root page case */
                Vector<BlockNumber> nodes(1);
                nodes.emplace_back(blkno);
                NodeImpl node = _ctx.get_node(blkno);
                node.w_lock();
                size_t totalSize = (node.size() - p_firstdatakey(node) + 1) * node_tuple_counts[i + 1];
                size_t unused;
                VectorIndexType idx_type = calc_vector_index_type(totalSize, min_cat, unused);
                BlockNumber index_meta_blkno = create_vector_index_metapage(
                    _ctx.get_index(), idx_type, min_cat, blkno, &nodes);
                ann_helper::optional_destroy(nodes);
                vector_index_blknos.push_back(index_meta_blkno);
                node.set_index_ptr(min_cat, index_meta_blkno);
                set_none_index(node, min_cat + 1);
                node.mark_dirty();
                node.w_unlock_destroy();
                upper_cat = min_cat;
                continue;
            }
            Vector<BlockNumber> blknos;
            size_t totalSize = 0;
            while (BlockNumberIsValid(blkno)) {
                blknos.emplace_back(blkno);
                NodeImpl node = _ctx.get_node(blkno);
                size_t node_size = node.size() - p_firstdatakey(node) + 1;
                totalSize += node.is_leaf() ? node_size : node_size * node_tuple_counts[i + 1];
                node.r_lock();
                blkno = node.next();
                node.r_unlock_destroy();
            }

            for (size_t ic = min_cat; ic < upper_cat; ++ic) {
                size_t idx_num;
                VectorIndexType indexType = calc_vector_index_type(totalSize, ic, idx_num);
                idx_num = std::min(idx_num, blknos.size());
                size_t avg_num = blknos.size() / idx_num;
                size_t rem = blknos.size() % idx_num;
                size_t start = 0;
                size_t end = avg_num;
                BlockNumber prev_meta_blkno = InvalidBlockNumber;
                for (size_t i = 0; i < idx_num; ++i) {
                    if (rem > 0 &&  i >= idx_num - rem) {
                        ++end;
                        --rem;
                    }
                    auto from =  blknos.begin() + start;
                    auto to = blknos.begin() + end;
                    BlockNumber index_meta_blkno = create_vec_idx_meta(
                        from, to, _ctx.get_index(), _ctx.get_heap(), indexType, ic, prev_meta_blkno);
                    vector_index_blknos.push_back(index_meta_blkno);
                    prev_meta_blkno = index_meta_blkno;
                    while (start < end) {
                        NodeImpl node = _ctx.get_node(blknos[start]);
                        node.w_lock();
                        node.set_index_ptr(ic, index_meta_blkno);
                        node.mark_dirty();
                        node.w_unlock_destroy();
                        ++start;
                    }

                    start = end;
                    end += avg_num;
                }
                Assert(rem == 0);
            }
            upper_cat = min_cat;
            ann_helper::optional_destroy(blknos);
        }

        _build_vector_indexes(vector_index_blknos);

        ann_helper::optional_destroy(path);
        ann_helper::optional_destroy(vector_index_blknos);
    }

    void vacuumVectorIndexes(IndexBulkDeleteCallback callback, void *callback_state)
    {
        IdxSet delete_set;
        UnorderedSet<BlockNumber> ivf_blknos, graph_blknos;

        _collect_vector_indexes(ivf_blknos, graph_blknos);

        if (ivf_blknos.empty() && graph_blknos.empty()) {
            ann_helper::optional_destroy(ivf_blknos);
            ann_helper::optional_destroy(graph_blknos);
            return;
        }

        int parallel_workers = hybridAnnGetNumParallel(_ctx.get_index());
        const auto vacuum = [this, callback, callback_state, &delete_set, &parallel_workers](UnorderedSet<BlockNumber> &blknos) -> void {
            /* inter-parallel */
            int min_parallel_workers = std::min((size_t)parallel_workers, blknos.size() - 1 );
            _vacuum_inter_parallel(callback, callback_state, min_parallel_workers, delete_set, blknos);
        };

        ereport(NOTICE, (errmsg("VACUUM: %lu IVF vector indexes, %lu Graph vector indexes",
                                ivf_blknos.size(), graph_blknos.size())));
        
        vacuum(ivf_blknos);
        /*ivf doesn't/no need handle delete_set */
        Assert(delete_set.size() == 0);
        vacuum(graph_blknos);
        recycle_to_freespace(_ctx.get_index(), hybridAnnGetFreespaceblkno(_ctx.get_index()), delete_set);
        
        ann_helper::optional_destroy(ivf_blknos);
        ann_helper::optional_destroy(graph_blknos);
        ann_helper::optional_destroy(delete_set);
    }

private:
    void _build_vector_indexes(Vector<BlockNumber> &vector_index_blknos)
    {
        size_t size = vector_index_blknos.size();
        ereport(NOTICE, (errmsg("Start to build %lu internal vector indexes", size)));
        TupleDesc tuple_desc = CreateTupleDescCopy(_ctx.get_tupDesc());
        char indexName[NAMEDATALEN + 1];
        char partIndexName[NAMEDATALEN + 1];
        populate_index_partition_name(_ctx.get_index(), indexName, partIndexName);
        ann_helper::Timer timer(size, 1, indexName, partIndexName);
        for (size_t i = 0; i < size; ++i) {
            _build_vector_index(i + 1, size, vector_index_blknos[i], tuple_desc, &timer);
        }
        pfree(tuple_desc);
        timer.report("Build Vector Index finished");
        timer.destroy();
        ereport(NOTICE, (errmsg("Done to build %lu internal vector indexes", size)));
    }

    void _build_vector_index(size_t nth, size_t total, BlockNumber blkno, TupleDesc &tuple_desc, Timer *timer)
    {
        VectorIndex *index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), blkno);
        BlockNumber leftmostnode = index->get_leftmost_node();
        DiskNodeImpl node(_ctx.get_index(), leftmostnode);
        node.r_lock();
        uint16 btLevel = node.level();
        node.r_unlock_destroy();

        char buffer[100];
        sprintf(buffer, "Build %s Vector Index", index->type_name());
        timer->set_stage(buffer);

        ereport(NOTICE, (errmsg("[%lu/%lu] Start to build %s vector index (%d:%d) "
                                "on magnitude level %lu and btree level %u",
                                nth, total, index->type_name(), blkno, index->index_meta_blkno(),
                                index->index_magnitude_level(), btLevel)));

        size_t nblkno;
        BlockNumber *blknos = index->get_node_blkno(nblkno);

        vector_pair_vector data;
        for (size_t i = 0; i < nblkno; ++i) {
            collect_vector_info(_ctx.get_index(), blknos[i], data, tuple_desc, false, NULL);
        }

        index->build(data, hybridAnnGetNumParallel(_ctx.get_index()), u_sess->attr.attr_memory.maintenance_work_mem);

        ereport(NOTICE, (errmsg("[%lu/%lu] Done to build %s vector index (%d:%d) "
                                "on magnitude level %lu and btree level %u",
                                nth, total, index->type_name(), blkno, index->index_meta_blkno(),
                                index->index_magnitude_level(), btLevel)));
        timer->report_loop("Build Vector Index");                       

        pfree(blknos);
        ann_helper::optional_destroy(data);
        delete_vector_index(index);
    }

    void _split_vector_index_root(NodeImpl &new_root, NodeImpl &lnode)
    {
        if (new_root.level() == 1) {
            new_root.internal_opaque()->init_vec_meta();
        } else {
            new_root.set_index_ptrs(lnode, _ctx.index_magnitude()->size());
        }
        for (size_t i = 0; i < _ctx.index_magnitude()->size(); ++i) {
            if (new_root.index_ptr()[i] == InvalidBlockNumber) {
                continue;
            }
            IndexerBuildTaskParam params;
            params.root_blkno = new_root.ptr();
            add_vector_index_task(_ctx.get_index(), new_root.index_ptr()[i], i, _ctx.get_tupDesc(), IndexerTaskType::IndexerTaskTypeBuild, &params);
        }
    }

    void _split_vector_index_node(NodeImpl &pnode)
    {
        for (size_t i = 0; i < _ctx.index_magnitude()->size(); ++i) {
            if (pnode.index_ptr()[i] == InvalidBlockNumber) {
                continue;
            }

            VectorIndex *index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), pnode.index_ptr()[i]);
            index->r_lock();
            const VectorIndexType itype = index->type();
            index->r_unlock();
            delete_vector_index(index);
            if (itype == VectorIndexType::None) {
                IndexerBuildTaskParam params;
                params.root_blkno = InvalidBlockNumber;
                add_vector_index_task(_ctx.get_index(), pnode.index_ptr()[i], i, _ctx.get_tupDesc(), IndexerTaskType::IndexerTaskTypeBuild, &params);
            } else {
                IndexerSplitTaskParam params;
                add_vector_index_task(_ctx.get_index(), pnode.index_ptr()[i], i, _ctx.get_tupDesc(), IndexerTaskType::IndexerTaskTypeSplit, &params);
            }
        }
    }

    void _merge_vector_index_node(NodeImpl &pnode)
    {
        for (size_t i = 0; i < _ctx.index_magnitude()->size(); ++i) {
            if (pnode.index_ptr()[i] == InvalidBlockNumber) {
                continue;
            }

            VectorIndex *index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), pnode.index_ptr()[i]);
            index->r_lock();
            const VectorIndexType itype = index->type();
            index->r_unlock();
            delete_vector_index(index);
            if (itype == VectorIndexType::None) {
                continue;
            }

            IndexerMergeTaskParam params;
            add_vector_index_task(_ctx.get_index(), pnode.index_ptr()[i], i, _ctx.get_tupDesc(), IndexerTaskType::IndexerTaskTypeMerge, &params);
        }
    }

    void _recycle_vector_index(const BlockNumber indexBlkno, const size_t magnitude_level)
    {
        IndexerRecycleTaskParam params;
        add_vector_index_task(_ctx.get_index(), indexBlkno, magnitude_level, _ctx.get_tupDesc(),
                              IndexerTaskType::IndexerTaskTypeRecycle, &params);
    }

    void _insert_node_to_vector_index(NodeImpl &node)
    {
        _insert_to_vector_index(node, NULL, [this, &node](VectorIndex *index) {
            index->insert_node_blkno(node.ptr());
        });
    }

    void _insert_data_to_vector_index(Data *data, PathStack &path)
    {
        while (!path.empty()) { /* may require get parent instead of for loop */
            SearchEntry last_entry = path.back();
            path.pop_back();
            NodeImpl node = _ctx.get_node(last_entry.ptr);
            node.r_lock();
            _insert_to_vector_index(node, data, [this, &data](VectorIndex *index) {
                size_t vec_id = get_vector_id(data->tuple(), _ctx.get_tupDesc());
                index->insert(vec_id, data->t_tid);
            });
            node.r_unlock_destroy();
        }
    }

    template <class F>
    void _insert_to_vector_index(NodeImpl &node, Data *data, F &&handle)
    {
        const auto skipInsert = [this](Data *data, VectorIndex *index) {
            bool skip = false;
            index->r_lock();
            BlockNumber blkno = index->split_scanned_blkno();
            if(blkno != 0) {
                if (BlockNumberIsValid(blkno)) {
                    auto scan_key = _ctx.get_scan_key(data);
                    DiskNodeImpl node(_ctx.get_index(), blkno);
                    node.r_lock();
                    Assert(node.is_leaf());
                    Data *firstData = node.get_data(p_firstdatakey(node));
                    skip = (scan_key > *firstData);
                    node.r_unlock();
                    node.destroy();
                    ann_helper::optional_destroy(scan_key);
                } else {
                    skip = true;
                }
            }
            index->r_unlock();
            return skip;
        };

        for (size_t i = 0; i < _ctx.index_magnitude()->size(); ++i) {
            if (!BlockNumberIsValid(node.index_ptr()[i])) {
                continue;
            }

            if (BlockNumberIsValid(node.new_index_ptr()[i])) {
                VectorIndex *new_index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), node.new_index_ptr()[i]);
                if (data && skipInsert(data, new_index)) {
                    delete_vector_index(new_index);
                    continue;
                }
                handle(new_index);
                delete_vector_index(new_index);
                continue;
            }

            /* handle with old index */
            VectorIndex *index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), node.index_ptr()[i]);
            handle(index);
            delete_vector_index(index);
        }
    };

    void recycle_to_freespace(Relation rel, BlockNumber freespace_blkno, IdxSet &delete_set)
    {
        constexpr size_t counter_step = 2'000ul;
        size_t *deleted_idx = (size_t *)palloc(sizeof(size_t) * counter_step);
        FreeSpace<size_t> free_space(rel, freespace_blkno);
        auto it = delete_set.cbegin();
        size_t counter = 0;
        while (it != delete_set.cend()) {
            counter = 0;
            for (; it != delete_set.cend(); ++it) {
                size_t idx = *it;
                deleted_idx[counter] = idx;
                ++counter;
                if (counter >= counter_step) {
                    free_space.insert(deleted_idx, counter);
                    break;
                }
            }
        }

        if (counter > 0) {
            free_space.insert(deleted_idx, counter);
        }
        pfree(deleted_idx);
        free_space.destroy();
    }

    void _vacuum_inter_parallel(IndexBulkDeleteCallback callback, void *callback_state, int parallel_workers,
                                IdxSet &delete_set, UnorderedSet<BlockNumber> &index_blknos)
    {
        IdxSet delete_sets[index_blknos.size()];
        size_t total = index_blknos.size();
        const auto task = [&](size_t nth, BlockNumber blkno) {
            Oid base_relid;
            Oid part_id = InvalidOid;
            Relation target;
            Relation parent;
            Partition part;

            Relation rel = _ctx.get_index();

            if (IsBgWorkerProcess()) {
                if (RelationIsPartition(rel)) {
                    base_relid = GetBaseRelOidOfParition(rel);
                    part_id = RelationGetRelid(rel);
                } else {
                    base_relid = RelationGetRelid(rel);
                }

                if (part_id == InvalidOid) {
                    target = index_open(base_relid, NoLock);
                } else {
                    parent = index_open(base_relid, NoLock);
                    part = partitionOpen(parent, part_id, NoLock);
                    target = partitionGetRelation(parent, part);
                }
                RelationOpenSmgr(target);
            } else {
                target = rel;
            }

            VectorIndex *index = VectorIndexFactory::create(target, _ctx.get_heap(), blkno);
            ereport(NOTICE, (errmsg("[%lu/%lu] Start to vacuum %s vector index (%d:%d) "
                                    "on magnitude level %lu",
                                    nth, total, index->type_name(), blkno, index->index_meta_blkno(),
                                    index->index_magnitude_level())));
            index->vacuum(callback, callback_state, delete_sets[nth - 1], 0);
            ereport(NOTICE, (errmsg("[%lu/%lu] Done to vacuum %s vector index (%d:%d) "
                                    "on magnitude level %lu",
                                    nth, total, index->type_name(), blkno, index->index_meta_blkno(),
                                    index->index_magnitude_level())));
            delete_vector_index(index);

            if (IsBgWorkerProcess()) {
                index_close(target, NoLock);
                if (part_id != InvalidOid) {
                    partitionClose(parent, part, NoLock);
                    index_close(parent, NoLock);
                }
            }
        };

        INIT_TASK_RUNNER();
        LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(parallel_workers);

        START_TASK_POOL();
        ann_helper::Timer timer(index_blknos.size(), 10);
        size_t nth = 1ul;
        for (const auto& blkno : index_blknos) {
            RUN_TASK(task, nth, blkno);
            timer.report_loop("Vacuuming Vector Index");
            ++nth;
        }
        WAIT_AND_END_TASK_POOL();
        timer.report("Done to vacuum");
        timer.destroy();

        DESTROY_TASK_RUNNER();

        size_t total_size = 0;
        for (auto &ds : delete_sets) {
            total_size += ds.size();
        }
        delete_set.reserve(total_size + 10ul);
        for (auto &ds : delete_sets) {
            ds.ctraverse([&delete_set](const size_t &idx) {
                delete_set.insert(idx);
            });
            ann_helper::optional_destroy(ds);
        }
    }

    void _collect_vector_indexes(UnorderedSet<BlockNumber> &ivf_blknos, UnorderedSet<BlockNumber> &graph_blknos)
    {
        Vector<BlockNumber> path;
        iterate_to_leaf_leftmost(path);

        if (path.empty()) {
            ann_helper::optional_destroy(path);
            return;
        }

        const auto collect = [this, &ivf_blknos, &graph_blknos](BlockNumber index_blkno) -> void {
            if (ivf_blknos.contains(index_blkno) || graph_blknos.contains(index_blkno)) {
                return;
            }
            VectorIndex *index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), index_blkno);
            switch (index->type()) {
                case VectorIndexType::GRAPH:
                    graph_blknos.insert(index_blkno);
                    break;
                case VectorIndexType::IVF:
                    ivf_blknos.insert(index_blkno);
                    break;
                default:
                    break;
            }
            delete_vector_index(index);
        };

        NodeImpl node;
        BlockNumber node_blkno;
        while (!path.empty()) {
            node_blkno = path.back();
            path.pop_back();
            while (BlockNumberIsValid(node_blkno)) {
                node = _ctx.get_node(node_blkno);
                node.r_lock();
                for (size_t i = 0; i < _ctx.index_magnitude()->size(); ++i) {
                    BlockNumber blkno = node.index_ptr()[i];
                    if (BlockNumberIsValid(blkno)) {
                        collect(blkno);
                    }
                    BlockNumber newblkno = node.new_index_ptr()[i];
                    if (BlockNumberIsValid(newblkno)) {
                        collect(newblkno);
                    }
                }
                node_blkno = node.next();
                node.r_unlock_destroy();
            }
        }

        ann_helper::optional_destroy(path);
    }
};
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_VECTOR_H */
