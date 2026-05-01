/**
 * Copyright ...
 * Diskann hibrid index interface.
 */

#ifndef DISKANN_VECTOR_BPTREE_H
#define DISKANN_VECTOR_BPTREE_H

#include <vtl/disk_container/diskvector.hpp>
#include <vtl/disk_container/freespace.hpp>

#include "access/diskann/vector_bt.h"
#include "access/diskann/diskann_internal.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/hybridann/bplustree/disk_impl.h"
#include "access/annvector/ann_utils.h"

class DiskAnnVectorIndex : public VectorIndex {
    using super = VectorIndex;
    template <typename T> using DiskVector = disk_container::DiskVector<T>;
    using AccessorLockType = disk_container::AccessorLockType;
    using DiskNodeImpl = disk_container::DiskNodeImpl;
public:
    DiskAnnVectorIndex(Relation rel, Relation heap, Buffer meta_buf)
        : super(rel, heap, meta_buf)
    {
        if (_meta->index_meta_blkno != InvalidBlockNumber) {
            _index = NEW DiskANNIndex(_rel, _meta->index_meta_blkno, false, true);
        }
    }

    void create_vector_index(vector_pair_vector &data, int parallel_workers,
                             int maintenance_work_mem, bool need_wal)
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        _meta->index_meta_blkno = create_diskann_meta(_meta->index_magnitude_level, data, need_wal);
        MarkBufferDirty(_meta_buf);
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
        if (need_wal) { /* actually used to skip build_in_memory */
            _index = NEW DiskANNIndex(_rel, _meta->index_meta_blkno, false, true);
        }
    }

    void build(vector_pair_vector &vec_data, int parallel_workers, int maintenance_work_mem)
    {
        create_vector_index(vec_data, parallel_workers, maintenance_work_mem, false);
        if (support_mem_build(vec_data.size(), maintenance_work_mem)) {
            build_in_memory(vec_data, parallel_workers, maintenance_work_mem);
        } else {
            _index = NEW DiskANNIndex(_rel, _meta->index_meta_blkno, false, false);
            batch_insert(vec_data, parallel_workers);
        }
    }

    void split_to(VectorIndex *new_index, vector_pair_vector &right_data, int parallel_workers)
    {
        split_to_impl(new_index, right_data, parallel_workers);
    }

    void insert(size_t idx, ItemPointerData tid)
    {
        _index->insert_point_to(idx);
    }

    void batch_insert(vector_pair_vector &data, int parallel_workers)
    {
        Timer timer;
        const size_t size = data.size();
        timer.report("Start building index");
        _index->set_building();
        if (parallel_workers <= 1) {
            const size_t report_step_size = size > report_threshold ? 10'000 : 1'000;
            Timer build_timer(size, report_step_size);
            for (size_t i = 0; i < size; ++i) {
                _index->insert_point_to(data[i].vid);
                report_progress(i, build_timer);
            }
            build_timer.destroy();
        } else {
            DiskAnnBuildBTreeData build_data =
                {BTreeBuildParamType::BUILD, _meta->index_meta_blkno, data};
            _index->do_parallel_build(&build_data, 0, size - 1ul, parallel_workers);
        }
        _index->insert_point_to(_index->medoid());
        _index->unset_building();
        timer.report("Index built");
        timer.destroy();
    }

    void recycle(size_t index_magnitude_level, size_t size)
    {
        size_t nnode;
        BlockNumber *blknos = get_node_blkno(nnode);
        Vector<BlockNumber> node_blknos(nnode);
        for (size_t i = 0; i < nnode; ++i) {
            node_blknos.push_back(blknos[i]);
        }
        pfree(blknos);

        BlockNumber leftmost_node_blkno = get_leftmost_node();
        DiskNodeImpl leftmost_node(_rel, leftmost_node_blkno);
        leftmost_node.r_lock();
        BlockNumber prev_node_blkno = leftmost_node.prev();
        leftmost_node.r_unlock_destroy();
        BlockNumber next_index_meta_blkno = next();

        /* handle nodes and new None vector index related */
        BlockNumber new_index_meta_blkno = InvalidBlockNumber;
        if (size > 0) {
            new_index_meta_blkno = create_vector_index_metapage(
                _rel, VectorIndexType::None, index_magnitude_level, leftmost_node_blkno, &node_blknos);
            LogManager logmgr(_rel);
            logmgr.log_vecindex_meta_and_node(new_index_meta_blkno);
            logmgr.destroy();
            VectorIndex *new_index = NEW VectorIndex(_rel, _heap, new_index_meta_blkno);
            new_index->update_next_index_meta_blkno(next_index_meta_blkno, true);
            ereport(NOTICE, (errmsg("  Created new %s Index (%d:%d) on magnitude level %lu",
                                    new_index->type_name(), new_index_meta_blkno, InvalidBlockNumber, index_magnitude_level)));
            ann_helper::optional_destroy(*new_index);
            delete new_index;
        }
        for (BlockNumber blkno : node_blknos) {
            DiskNodeImpl node(_rel, blkno);
            node.w_lock();
            node.index_ptr()[index_magnitude_level] = new_index_meta_blkno;
            node.mark_dirty();
            LogManager logmgr(_rel);
            logmgr.log_vecindex_set_index_ptr(node._buf, node._page, index_magnitude_level, new_index_meta_blkno, false);
            logmgr.destroy();
            node.w_unlock_destroy();
        }
        ann_helper::optional_destroy(node_blknos);

        /* handle prev vector index if exists */
        if (BlockNumberIsValid(prev_node_blkno)) {
            DiskNodeImpl prev_node(_rel, prev_node_blkno);
            prev_node.r_lock();
            BlockNumber prev_index_blkno = prev_node.index_ptr()[index_magnitude_level];
            if (BlockNumberIsValid(prev_index_blkno)) {
                VectorIndex *prev_index = NEW VectorIndex(_rel, _heap, prev_index_blkno);
                if (BlockNumberIsValid(new_index_meta_blkno)) {
                    prev_index->update_next_index_meta_blkno(new_index_meta_blkno, true);
                } else {
                    prev_index->update_next_index_meta_blkno(next_index_meta_blkno, true);
                }
                ann_helper::optional_destroy(*prev_index);
                delete prev_index;
            }
            prev_node.r_unlock_destroy();
        }

        /* recycle current vector index and mark it as deleted */
        // remove_all_node_blknos();
        set_leftmost_node_blkno(InvalidBlockNumber);
        update_next_index_meta_blkno(InvalidBlockNumber, true);
        recycle_to_fsm();
        set_deleted(true);
    }

    void vacuum(IndexBulkDeleteCallback callback, const void *callback_state, IdxSet &delete_set, int parallel_workers)
    {
        const size_t index_magnitude_level = _meta->index_magnitude_level;
        const size_t curr_index_magnitude = _index_magnitude->magnitudes()[index_magnitude_level];
        const size_t vacuum_threshold = curr_index_magnitude / 2;
        large_vector<size_t> valid_ids(curr_index_magnitude);

        ereport(NOTICE, (errmsg("  Collecting undeleted points ...")));
        _index->collect_valid_points(delete_set, valid_ids, curr_index_magnitude);
        ereport(NOTICE, (errmsg("  Collected %lu undeleted points with vacuum threshold %lu",
                                valid_ids.size(), vacuum_threshold)));

        size_t size = valid_ids.size();

        if (size >= vacuum_threshold) {
            if (parallel_workers <= 1) {
                /* consolidate valid points and their neighbors */
                _index->consolidate_all_points(delete_set, valid_ids);
            } else {
                DiskAnnVacuumBTreeData vacuum_data =
                    {BTreeBuildParamType::VACUUM, _meta->index_meta_blkno, valid_ids, delete_set};
                _index->do_parallel_build(&vacuum_data, 0, size - 1ul, parallel_workers);
            }
        } else {
            /* recycle current diskann vector index since too few points */
            recycle(index_magnitude_level, size);
            ereport(NOTICE, (errmsg("  Recycled %s Index (%d:%d) on magnitude level %lu",
                                    type_name(), ptr(), index_meta_blkno(), index_magnitude_level)));
        }

        ann_helper::optional_destroy(valid_ids);
    }

    void recycle_to_fsm()
    {
        /* the old index leaves 3 meta pages (vector index meta, vector index node, diskann meta) */
        RecordFreeIndexPage(_rel, index_meta_blkno());
        free_index_meta_pages();
    }

    size_t search(IndexScanDesc scan, float *dist_out, ItemPointerData *iptr, void *param)
    {
        VectorIndexNoneSearchParam *search_param = (VectorIndexNoneSearchParam *)param;
        uint32 list_size = u_sess->attr.attr_storage.ef_search;
        list_size = list_size <= 0 ? DEFAULT_ANN_QUEUE_SIZE : list_size;
        list_size = std::max(search_param->top_k, list_size);
        return _index->search(search_param->query, search_param->top_k, list_size, iptr, dist_out);
    }

    size_t size()
    {
        return _index->size();
    }

    void destroy()
    {
        if (_index != nullptr) {
            _index->destroy();
            delete _index;
        }
        super::destroy();
    }

private:
    DiskANNIndex *_index{nullptr};

    BlockNumber create_diskann_meta(size_t index_magnitude_level, vector_pair_vector &data, bool need_wal)
    {
        Buffer bt_meta_buf = AnnLoadBuffer(_rel, DISKANN_METAPAGE_BLKNO);
        DiskAnnMetaPageV2 *meta_v2 = (DiskAnnMetaPageV2 *)DiskAnnPageGetMeta(BufferGetPage(bt_meta_buf));
        Assert(meta_v2->magicNumber == DISKANN_MAGIC_NUMBER);
        Assert(meta_v2->version == DISKANN_VERSION_TWO);

        LockRelationForExtension(_rel, ExclusiveLock);
        Buffer diskann_meta_buf = ReadBuffer(_rel, P_NEW);
        UnlockRelationForExtension(_rel, ExclusiveLock);
        Page diskann_meta_page = BufferGetPage(diskann_meta_buf);

        DiskAnnInitMeta(diskann_meta_buf, diskann_meta_page);
        DiskAnnMetaPage *meta= (DiskAnnMetaPage *)DiskAnnPageGetMeta(diskann_meta_page);

        meta->magicNumber = DISKANN_MAGIC_NUMBER;
        meta->metric = meta_v2->metric;
        meta->dimensions = _meta->dim = meta_v2->dimensions;
        meta->version = DISKANN_VERSION_ONE;

        meta->numCenters = DISKANN_MAX_NUM_PQ_CENTROIDS;
        meta->numPQChunks = meta->dimensions;

        meta->nodeMetaBlkNo = meta_v2->nodeMetaBlkNo; /* shared global */
        meta->freespaceMetaBlkNo = meta_v2->freespaceMetaBlkNo; /* shared global */
        meta->graphMetaBlkNo = meta_v2->graphMetaBlkNo[index_magnitude_level]; /* shared global on the same magnitude */

        /* calculate medoid for the part of data using global locations for current diskann index */
        size_t medoid = DiskANNIndex::calculate_entry_point(_rel, data, meta->metric, meta->dimensions);
        VecBuffer vec_buf = vec_read_buffer(_rel, medoid, meta->dimensions * sizeof(float));
        meta->medoid = push_back_vector(_rel, meta_v2->dataMetaBlkNo, (float *)vec_buf.get_vecbuf(), meta->dimensions);
        vec_buf.release();

        /* add a node for the new medoid */
        DiskVector<DiskAnnVamanaNode> nodes(_rel, meta->nodeMetaBlkNo, need_wal);
        DiskAnnVamanaNode node = nodes.get<AccessorLockType::NoLockUnsafe>(medoid);
        diskann_node_flag::set_frozen(node.flag);
        nodes.extend(meta->medoid + 1);
        nodes.set<AccessorLockType::WriteLock>(meta->medoid, node);
        nodes.destroy();

        /* we have to add a neighbor for each magnitude to keep their length equal to vectors' */
        AnnNeighbors neighbor = {0, 0, };
        for (size_t i = _index_magnitude->graph_entry_level(); i < _index_magnitude->size(); ++i) {
            DiskVector<AnnNeighbors> neighbors(_rel, meta_v2->graphMetaBlkNo[i], need_wal);
            neighbors.extend(meta->medoid + 1);
            neighbors.set<AccessorLockType::WriteLock>(meta->medoid, neighbor);
            neighbors.destroy();
        }

        meta->attrMetaBlkNo = InvalidBlockNumber;
        meta->pqPivotsMetaBlkNo = InvalidBlockNumber;
        meta->pqCompressedMetaBlkNo = InvalidBlockNumber;

        ((PageHeader) diskann_meta_page)->pd_lower = ((char *) meta + sizeof(DiskAnnMetaPage)) - (char *) diskann_meta_page;

        BlockNumber diskann_meta_blkno = BufferGetBlockNumber(diskann_meta_buf);

        MarkBufferDirty(diskann_meta_buf);
        if (need_wal) {
            XLogBeginInsert();
            START_CRIT_SECTION();
            XLogRegisterBuffer(0, diskann_meta_buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
            XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_ADD_ANN_META);
            PageSetLSN(diskann_meta_page, recptr);
            END_CRIT_SECTION();
        }
        ReleaseBuffer(diskann_meta_buf);

        UnlockReleaseBuffer(bt_meta_buf);

        return diskann_meta_blkno;
    }

    bool support_mem_build(size_t data_size, int maintenance_work_mem)
    {
        uint64 max_work_mem = ((uint64) maintenance_work_mem) * kb_to_bytes * DISKANN_WORK_MEM_PERCENTAGE;
        uint32 max_num_points_in_mem = max_work_mem / (_meta->dim * sizeof(float) + sizeof(DiskAnnVamanaNode) + sizeof(AnnNeighbors));
        return data_size <= max_num_points_in_mem;
    }

    void populate_mem_data(vector_pair_vector &vec_data, size_t medoid, BlockNumber node_meta_blkno,
                           large_vector<float> &data, large_vector<AnnNeighbors> &graph, large_vector<DiskAnnVamanaNode> &nodes)
    {
        size_t data_size = vec_data.size();

        /* set medoid first */
        read_vector(_rel, medoid, _meta->dim, (char *)data.data());
        /* then all data */
        for (size_t i = 0; i < data_size; ++i) {
            read_vector(_rel, vec_data[i].vid, _meta->dim, (char *)(data.data() + (i + 1) * _meta->dim));
        }

        /* then nodes */
        constexpr auto key = disk_container::PlainStore::invalid_key();

        DiskVector<DiskAnnVamanaNode> all_nodes(_rel, node_meta_blkno, false);
        DiskAnnVamanaNode node = all_nodes.get<AccessorLockType::NoLockUnsafe>(medoid);
        all_nodes.destroy();

        DiskAnnVamanaNode medoid_node = {node.heapTid, key, diskann_node_flag::init_flag};
        diskann_node_flag::set_frozen(medoid_node.flag);
        nodes.set(0, medoid_node);

        for (size_t i = 0; i < data_size; ++i) {
            nodes.set(i + 1, {vec_data[i].tid, key, diskann_node_flag::init_flag});
        }

        /* and initial graph */
        AnnNeighbors ngh = {0, 0, };
        for (size_t i = 0; i <= data_size; ++i) {
            graph.set(i, ngh);
        }
    }

    void flush_mem_graph(vector_pair_vector &vec_data, size_t medoid, BlockNumber graph_meta_blkno,
                        large_vector<AnnNeighbors> &graph)
    {
        /* map real vector ids */
        for (size_t i = 0; i < graph.size(); ++i) {
            AnnNeighbors &nbrs = graph[i];
            for (size_t j = 0; j < nbrs.num_neighbors; ++j) {
                AnnNeighbor nbr = nbrs.neighbors[j];
                if (nbr == 0) {
                    nbrs.neighbors[j] = medoid;
                } else {
                    nbrs.neighbors[j] = vec_data[nbr - 1].vid;
                }
            }
        }
        /* flush to disk */
        DiskVector<AnnNeighbors> all_graphs(_rel, graph_meta_blkno, false);
        all_graphs.set<AccessorLockType::WriteLock>(medoid, graph[0]);
        for (size_t i = 0; i < vec_data.size(); ++i) {
            all_graphs.set<AccessorLockType::WriteLock>(vec_data[i].vid, graph[i + 1]);
        }
        all_graphs.destroy();
    }

    void build_in_memory(vector_pair_vector &vec_data, int parallel_workers, int maintenance_work_mem)
    {
        Buffer buf = AnnLoadBuffer(_rel, _meta->index_meta_blkno);
        DiskAnnMetaPage *meta = (DiskAnnMetaPage *)DiskAnnPageGetMeta(BufferGetPage(buf));
        size_t medoid = meta->medoid;
        BlockNumber node_meta_blkno = meta->nodeMetaBlkNo;
        BlockNumber graph_meta_blkno = meta->graphMetaBlkNo;
        UnlockReleaseBuffer(buf);

        size_t data_size = vec_data.size();

        large_vector<float> data;
        large_vector<AnnNeighbors> graph;
        large_vector<DiskAnnVamanaNode> nodes;

        data.resize((data_size + 1) * _meta->dim);
        graph.resize(data_size + 1);
        nodes.resize(data_size + 1);

        populate_mem_data(vec_data, medoid, node_meta_blkno, data, graph, nodes);

        pthread_mutex_t mutex;
        pthread_mutex_init(&mutex, NULL);

        _index = NEW DiskANNIndex(_rel, _meta->index_meta_blkno, data, nodes, graph, &mutex);
        _index->build(data, nodes, graph, &mutex, parallel_workers, _meta->index_meta_blkno);

        pthread_mutex_destroy(&mutex);

        flush_mem_graph(vec_data, medoid, graph_meta_blkno, graph);

        ann_helper::optional_destroy(data);
        ann_helper::optional_destroy(graph);
        ann_helper::optional_destroy(nodes);
    }
};

#endif /* DISKANN_VECTOR_BPTREE_H */
