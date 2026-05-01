/**
 * Copyright ...
 * Diskann internal algorithm and data structure.
 */

#ifndef DISKANN_INTERNAL_H
#define DISKANN_INTERNAL_H

#include <vtl/vector>
#include <vtl/bitvector>
#include <vtl/hashtable>
#include <vtl/btree>
#include <vtl/disk_container/diskvector.hpp>
#include <vtl/disk_container/tempfilevector.hpp>

#include "access/genam.h"
#include "access/diskann/diskann.h"
#include "access/annvector/module/timer.h"
#include "access/diskann/storage_interface/storage_interface.h"

#define VERIFY_VISITED false
using ann_helper::Timer;
template <typename T> using tmpvector = disk_container::TempFileVector<T, false>;
template <typename T> using large_vector = Vector<T, HUGE_ALLOCATOR<T>>;

typedef struct DiskAnnBuildState {
    Relation heap;
    Relation index;
    IndexInfo *indexInfo;
    ForkNumber forkNum;

    double relTuples;
    double idxTuples;

    FmgrInfo *procInfo;
    Oid collation;

    /* Basic Info */
    Metric metric;
    uint32 dimensions;
    uint32 numThreads;

    /* Data Processing */
    size_t numPoints;
    size_t num_inserted;
    size_t required_size;
    bool hasLargeData;
    bool use_inplace_attr_filter;

    uint64 maxWorkMem;
    uint32 maxNumPointsInMem;

    large_vector<float> data;
    large_vector<AnnNeighbors> graph;
    large_vector<DiskAnnVamanaNode> nodes;

    /* DiskANN Index Storage */
    BlockNumber nodeMetaBlkNo;
    BlockNumber graphMetaBlkNo;
    BlockNumber attrMetaBlkNo;

    /* Sub-Graph Build and Merge */
    uint32 numShards;
    Vector<tmpvector<uint32>> shardIDs;
    Vector<tmpvector<float>> shardData;
    Vector<tmpvector<DiskAnnVamanaNode>> shardNodes;
    Vector<tmpvector<AnnNeighbors>> shardGraphs;
    Vector<uint32> shardMedoids;

    /* PQ Storage and Cache */
    bool isPQEnabled;
    bool enableSubGraph;

    double samplingRate;
    uint32 numCenters;
    uint32 numPQChunks;

    BlockNumber pqPivotsMetaBlkNo;
    BlockNumber pqCompressedMetaBlkNo;

    float *pqPivots;
    uint32 *pqChunkOffsets;
} DiskAnnBuildState;

struct QueryNeighbor {
    uint32 id;
    float distance;
    QueryNeighbor() {}
    QueryNeighbor(uint32 id, float distance) : id{id}, distance{distance} {}
    QueryNeighbor(size_t id, float distance) : id{uint32(id)}, distance{distance} {}
    bool operator<(const QueryNeighbor &other) const { return distance < other.distance || (distance == other.distance && id < other.id); }
    bool operator==(const QueryNeighbor &other) const { return (id == other.id); }
    static bool compare_id(const QueryNeighbor &a, const QueryNeighbor &b) { return a.id < b.id; }
    static bool compare_distance(const QueryNeighbor &a, const QueryNeighbor &b) { return a.distance < b.distance; }
};

class CandidateQueue {
public:
    using bptree = Set<QueryNeighbor>;
    using iterator = bptree::iterator;
    using const_iterator = bptree::const_iterator;

    explicit CandidateQueue(size_t capacity) : _capacity(capacity) { Assert(_capacity >= 4); }
    void destroy() { ann_helper::optional_destroy(_left); ann_helper::optional_destroy(_right); }
    bool insert(const QueryNeighbor &n);
    bool insert(QueryNeighbor &&n) { return insert(n); }
    bool emplace(const unsigned id, const float distance) { return insert(QueryNeighbor(id, distance)); }
    bool emplace(const size_t id, const float distance) { return insert(QueryNeighbor(id, distance)); }
    bool can_insert(const float distance) const { return _size < _capacity || distance < _max_distance; }
    QueryNeighbor pop_unexplored();
    bool has_unexplored() const { return !_right.empty(); }
    float top_dist() const { return _right.cbegin()->distance; }
    uint32 top_id() const { return _right.cbegin()->id; }
    bool full() const { return _size >= _capacity; }
    iterator begin() { return _left.begin(); }
    iterator end() { return _left.end(); }
    const_iterator cbegin() const { return _left.cbegin(); }
    const_iterator cend() const { return _left.cend(); }
private:
    bptree _left{};
    bptree _right{};
    size_t _size{0};
    size_t _capacity;
    size_t _cur{0};
    float _max_distance{-FLT_MAX};

    void pop();
};

struct QueryEdgeNeighbor {
    size_t data_offset;
    size_t edge_offset;
    size_t semantic_id;
    float distance;

    QueryEdgeNeighbor() = default;

    QueryEdgeNeighbor(size_t data_off,
                      size_t edge_off,
                      size_t sem_id,
                      float dist)
        : data_offset(data_off),
          edge_offset(edge_off),
          semantic_id(sem_id),
          distance(dist)
    {}

    bool operator<(const QueryEdgeNeighbor &other) const {
        if (distance != other.distance)
            return distance < other.distance;
        if (data_offset != other.data_offset)
            return data_offset < other.data_offset;
        if (semantic_id != other.semantic_id)
            return semantic_id < other.semantic_id;
        return edge_offset < other.edge_offset;
    }

    bool operator==(const QueryEdgeNeighbor &other) const {
        return distance == other.distance &&
               data_offset == other.data_offset &&
               semantic_id == other.semantic_id &&
               edge_offset == other.edge_offset;
    }

    static bool compare_id(const QueryEdgeNeighbor &a, const QueryEdgeNeighbor &b) {
        return a.data_offset < b.data_offset;
    }

    // ----- 单独按 distance 比较 -----
    static bool compare_distance(const QueryEdgeNeighbor &a, const QueryEdgeNeighbor &b) {
        return a.distance < b.distance;
    }
};


class CandidateEdgeQueue {
    public:
        using bptree = Set<QueryEdgeNeighbor>;
        using iterator = bptree::iterator;
        using const_iterator = bptree::const_iterator;
    
        explicit CandidateEdgeQueue(size_t capacity) : _capacity(capacity) { Assert(_capacity >= 4); }
        void destroy() { ann_helper::optional_destroy(_left); ann_helper::optional_destroy(_right); }
        bool insert(const QueryEdgeNeighbor &n);
        bool insert(QueryEdgeNeighbor &&n) { return insert(n); }
        // bool emplace(const unsigned id, const float distance) { return insert(QueryEdgeNeighbor(id, distance)); }
        // bool emplace(const size_t id, const float distance) { return insert(QueryEdgeNeighbor(id, distance)); }
        bool can_insert(const float distance) const { return _size < _capacity || distance < _max_distance; }
        QueryEdgeNeighbor pop_unexplored();
        bool has_unexplored() const { return !_right.empty(); }
        float top_dist() const { return _right.cbegin()->distance; }
        size_t top_id() const { return _right.cbegin()->data_offset; }
        size_t top_semantic_id() const { return _right.cbegin()->semantic_id; }
        size_t top_edge_offset() const { return _right.cbegin()->edge_offset; }
        bool full() const { return _size >= _capacity; }
        iterator begin() { return _left.begin(); }
        iterator end() { return _left.end(); }
        const_iterator cbegin() const { return _left.cbegin(); }
        const_iterator cend() const { return _left.cend(); }
    private:
        bptree _left{};
        bptree _right{};
        size_t _size{0};
        size_t _capacity;
        size_t _cur{0};
        float _max_distance{-FLT_MAX};
    
        void pop();
    };

struct PruneContent {
    constexpr static size_t default_capacity = 5'000ul;
    bool query_alloced;
    const float *query;
    const size_t l_size;
    Vector<QueryNeighbor> expanded_nodes{};
    Vector<QueryNeighbor> pruned_list{};
    CandidateQueue candidates;
    CandidateQueue assistances;
    UnorderedSet<size_t> visited{default_capacity};
#if VERIFY_VISITED
    BitVector<> verify_visited{default_capacity};
#endif /* VERIFY_VISITED */

    PruneContent(const float *q, size_t l, size_t dim, double selectivity = 0.25);
    ~PruneContent() {}
    void destroy();
};

using IdxSet = UnorderedSet<size_t, impl::DefaultHasher<size_t>, std::equal_to<size_t>,
                            HUGE_ALLOCATOR<size_t>>;

struct DiskAnnBuildMemData {
    large_vector<float> &data;
    large_vector<DiskAnnVamanaNode> &node;
    large_vector<AnnNeighbors> &graph;
    pthread_mutex_t *mutex;
    uint32 dim;
};

enum BTreeBuildParamType : uint8 { BUILD, VACUUM, MEM_BUILD };

struct DiskAnnBTreeData {
    BTreeBuildParamType type;
    BlockNumber index_meta_blkno;
};

struct DiskAnnBuildBTreeData : public DiskAnnBTreeData {
    large_vector<VectorPair> &data;
};

struct DiskAnnVacuumBTreeData : public DiskAnnBTreeData {
    large_vector<size_t> &data;
    IdxSet &delete_set;
};

struct DiskAnnBuildMemBTreeData : public DiskAnnBTreeData, public DiskAnnBuildMemData {
};

constexpr float ALPHA = 1.2f;
constexpr size_t report_threshold = 500'000;

/* TD: cache progress to global */
inline void report_progress(size_t i, Timer &timer)
{
    timer._nloop_count = i - 1ul;
    timer.report_loop("  Building index");
}

inline void report_progress_vacuum(size_t i, Timer &timer)
{
    timer._nloop_count = i - 1ul;
    timer.report_loop("  Consolidating index");
}

class DiskANNIndex : public BaseObject {
public:
    struct VacuumReport {
        size_t num_point_remained{0};
        size_t num_page_remained{0};
        size_t num_point_deleted{0};
        size_t num_page_freed{0};
    };

    DiskANNIndex(Relation rel, BlockNumber meta_blkno, bool use_pq = false, bool isWal = true);
    explicit DiskANNIndex(Relation rel, bool use_pq = false, bool isWal = true)
        : DiskANNIndex(rel, DISKANN_METAPAGE_BLKNO, use_pq, isWal) {}

    DiskANNIndex(Relation rel, BlockNumber meta_blkno, large_vector<float> &data,
        large_vector<DiskAnnVamanaNode> &node, large_vector<AnnNeighbors> &graph, pthread_mutex_t *mutex);
    explicit DiskANNIndex(Relation rel, large_vector<float> &data, large_vector<DiskAnnVamanaNode> &node,
        large_vector<AnnNeighbors> &graph, pthread_mutex_t *mutex)
        : DiskANNIndex(rel, DISKANN_METAPAGE_BLKNO, data, node, graph, mutex) {}

    ~DiskANNIndex() {}
    void destroy();

    /* Build index on disk for large data */
    void build(size_t size, size_t num_parallel = 1ul);

    /* Build index in memory for one-shot or sub-graph */
    void build(large_vector<float> &data, large_vector<DiskAnnVamanaNode> &node,
               large_vector<AnnNeighbors> &graph, pthread_mutex_t *mutex,
               size_t num_parallel = 1ul, BlockNumber meta_blkno = DISKANN_METAPAGE_BLKNO);

    /* Flush index to disk for memory build */
    void flush(large_vector<float> &&data, large_vector<DiskAnnVamanaNode> &&node,
               large_vector<AnnNeighbors> &&graph, uint32 medoid);

    /* Flush index to disk for sub-graph build */
    void flush(size_t data_size);

    size_t calculate_entry_point(size_t size);
    static size_t calculate_entry_point(large_vector<float> &data, Metric metric, uint32 dim);
    static size_t calculate_entry_point(tmpvector<float> &data, Metric metric, uint32 dim);
    static size_t calculate_entry_point(Relation rel, vector_pair_vector &data, Metric metric, uint32 dim);

    void do_parallel_build(void *build_params, size_t start, size_t total_size, size_t num_parallel);

    size_t search(const float *query, const size_t K, const size_t L, ItemPointerData *tids, float *dists_out, uint32 num_scan_key, ScanKey scanKey);
    size_t search(const float *query, const size_t K, const size_t L, ItemPointerData *tids, float *dists_out)
        { return search(query, K, L, tids, dists_out, 0, nullptr); }

    void insert_point(float *point, const ItemPointerData &tid, Datum *values, const bool *isnull);
    void insert_point_to(const float *point, const size_t location);
    void insert_point_to(size_t location);

    template <class F1, class F2>
    static void get_deleted_point_idx(IndexBulkDeleteCallback callback, void *callback_state, IdxSet &delete_set, size_t total_size, F1 &&get_node, F2 &&set_node);
    static void get_deleted_point_idx(Relation rel, BlockNumber node_blkno, IndexBulkDeleteCallback callback, void *callback_state, IdxSet &delete_set);
    void get_deleted_point_idx(IndexBulkDeleteCallback callback, void *callback_state, IdxSet &delete_set, size_t &total_size);

    template <class F>
    static VacuumReport retrieve_deleted_slots(Relation rel, size_t upto, IdxSet &delete_set, BlockNumber freespace_blkno, F &&node_handler);
    static void retrieve_deleted_slots(Relation rel, BlockNumber node_blkno, BlockNumber freespace_blkno, IdxSet &delete_set);
    VacuumReport retrieve_deleted_slots(size_t upto, IdxSet &delete_set);

    template <class F>
    void process_deleted_neighbors(size_t loc, F &&delete_filter);
    void consolidate_all_points(size_t upto, IdxSet &delete_set);
    void collect_valid_points(IdxSet &delete_set, large_vector<size_t> &valid_ids, const size_t default_capacity);
    void consolidate_all_points(IdxSet &delete_set, large_vector<size_t> &valid_ids);

    uint32 dim() const { return _meta.dimensions; }
    Metric metric() const { return _meta.metric; }
    size_t medoid() const { return _meta.medoid; }
    void report_to(PerfUsage &usage) { _storage->perf_report_to(usage); }
    void set_building() { _is_building = true; }
    void unset_building() { _is_building = false; }
    size_t size() { return _storage->size(); }

protected:
    DiskANNIndex(const DiskANNIndex &) = delete;
    DiskANNIndex &operator=(const DiskANNIndex &) = delete;

    void init_disk_storage(size_t size);

    Vector<size_t> get_init_ids();
    template <bool search_invocation, class F>
    void iterate_to_fixed_point(PruneContent &content, const Vector<size_t> &init_ids, F &&filter);
    void search_for_point_and_prune(PruneContent &content, size_t location);
    void prune_neighbors(const size_t location, PruneContent &content);

    /* Prunes candidates in @pool to a shorter list @result
       @pool must be sorted before calling */
    void occlude_list(const size_t location, Vector<QueryNeighbor> &pool, Vector<QueryNeighbor> &result);
    /* add reverse links from all the visited nodes to node n. */
    void inter_insert(size_t n, PruneContent &content);

    size_t reserve_location(const float *point, const DiskAnnVamanaNode &node);
    void load_meta(BlockNumber meta_blkno);

private:
    constexpr static uint32 max_candidate_size = MAX_ANN_GRAPH_DEGREE * 1.8;

    Relation _rel;
    DiskAnnMetaPage _meta;
    StorageInterface *_storage;
    float _alpha{ALPHA};
    uint32 _max_degree{MAX_ANN_GRAPH_DEGREE};
    uint32 _list_size{DEFAULT_ANN_QUEUE_SIZE};
    bool _is_building{false};
    bool _use_pq{false};
    bool _isWal;

    /* hopefully this prevents dummy_filter to be convertible with function pointers */
    struct DummyFilter {
        template <typename ...Args>
        constexpr bool operator()(Args &&...) const { return true; }
    };
    /**
     * `constexpr` to make it initialized at compile time,
     *  it does not have to be compile time, but it has to be declared like this to
     *  be initialized in header file rather than mannualy make it in every constructor
     * `inline` to resolve linker errors brought by `static`
     */
    NO_UNIQUE_ADDRESS constexpr inline static const DummyFilter dummy_filter = {};
};

#endif /* DISKANN_INTERNAL_H */
