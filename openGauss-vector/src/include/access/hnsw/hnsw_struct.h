#ifndef HNSW_STRUCT_H
#define HNSW_STRUCT_H

#include "postgres.h"
#include "storage/buf/block.h"
#include "storage/off.h"
#include "storage/item/itemptr.h"
#include "storage/lock/lwlock.h"
#include "lib/pairingheap.h"

#include "access/hnsw/hnsw_param.h"
#include "access/hnsw/hnsw_cluster.h"
#include "access/hnsw/hnsw_quantizer.h"
#include "access/annvector/distance/distance.h"

struct BlkOffsetNumEntry {
    BlockNumber blkno;
    OffsetNumber offno;
    BlkOffsetNumEntry(BlockNumber blkno_, OffsetNumber offno_): blkno(blkno_), offno(offno_) {}
    BlkOffsetNumEntry(): blkno(InvalidBlockNumber), offno(InvalidOffsetNumber) {}
};

struct HnswEntryPointBase {
    uint8 level;
    OffsetNumber offno;
    BlockNumber blkno;
    HnswEntryPointBase() = default;
    HnswEntryPointBase(uint8 level, const BlkOffsetNumEntry &entry)
        : level(level), offno(entry.offno), blkno(entry.blkno) {}
    HnswEntryPointBase(uint8 level, OffsetNumber offno, BlockNumber blkno)
        : level(level), offno(offno), blkno(blkno) {}
};

struct HnswEntryPoint : public HnswEntryPointBase {
    size_t floatVectorIndex;
};

struct HnswNeighborArray;

class HnswElementData : public HnswTids<HnswElementData> {
public:
    uint8 level;
    uint8 flag;
    OffsetNumber offno;
    BlockNumber blkno;
    size_t floatVectorIndex;
    ItemPointerData heaptids[HNSW_HEAPTIDS];
    LWLock lock;
    HnswElementData *next;
    HnswNeighborArray **neighbors;
    char *value;

    const uint8 &get_flag() const { return flag; }
    uint8 &get_flag() { return flag; }
    const ItemPointerData *get_heaptids() const { return heaptids; }
    ItemPointerData *get_heaptids() { return heaptids; }
    uint8 ntids() const { return (flag >> 2) & 0x0f; }
    void set_ntids(uint8 n) { flag = (flag & 0xc3) | (n << 2); }
};
using HnswElement = HnswElementData *;

struct HnswCandidate {
    bool closer;
    uint16 neighborIndex;
    float distance;
    HnswElement element;
};

struct HnswNeighborArray {
    int length;
    bool closerSet;
    HnswCandidate items[FLEXIBLE_ARRAY_MEMBER];
};

struct HnswPairingHeapNode {
    pairingheap_node ph_node;
    HnswCandidate *inner;
};

struct HnswDiskCandidate {
    uint16 neighborindex;
    ItemPointerData indexTid;
    float distance;
    char *value;
    size_t floatVectorIndex;
};

struct HnswDiskPairingHeapNode {
    pairingheap_node ph_node;
    HnswDiskCandidate *inner;
};

/* HNSW index options */
struct HnswOptions {
    int32 vl_len_;        /* varlena header (do not touch directly!) */
    int m;                /* number of connections */
    int efConstruction;   /* size of dynamic candidate list */
    int qt_type_offset;
    int parallel_workers;
    int64 num_cluster;
    bool rabitq_keep_vecs;
};

struct HnswAllocator {
    void *(*alloc)(Size size, void *state);
    void *(*align_alloc)(Size size, Size align, void *state);
    void *state;
};

struct HnswMetaPageData {
    uint32 magicNumber;
    uint32 version;
    uint32 dimensions;
    uint16 m;
    uint16 efConstruction;
    BlockNumber entryBlkno;
    OffsetNumber entryOffno;
    int16 entryLevel;
    BlockNumber insertPage;
    size_t num_vectors;
    Metric metric;
    BlockNumber head_blkno;
    QuantizerMetaInfo quantizer_metainfo;
    Cluster cluster;
    DistPrecisionType precision_type;
};
using HnswMetaPage = HnswMetaPageData *;


struct HnswPageOpaqueData {
    BlockNumber nextblkno;
    uint16 unused;
    uint16 page_id;        /* for identification of HNSW indexes */
};
using HnswPageOpaque = HnswPageOpaqueData *;

struct HnswNeighborData {
    ItemPointerData indexTid;
    size_t floatVectorIndex;
};
using HnswNeighbor = HnswNeighborData *;

struct HnswTupleData : public HnswTids<HnswTupleData> {
	uint8 level;
	uint8 flag;
	uint16 heapCount;
	ItemPointerData heaptids[HNSW_HEAPTIDS];
	size_t floatVectorIndex;
	HnswNeighborData neighbors[FLEXIBLE_ARRAY_MEMBER];

	const uint8 &get_flag() const { return flag; }
	uint8 &get_flag() { return flag; }
	const ItemPointerData *get_heaptids() const { return heaptids; }
	ItemPointerData *get_heaptids() { return heaptids; }
	/* internal helper func, use actual_ntids to get actual number of elements */
	uint8 ntids() const { return heapCount; }
	/* make sure you know what is going on when you call this func */
	void set_ntids(uint8 n) { heapCount = n; }
};
using HnswTuple = HnswTupleData *;

struct HnswMetaBlknos {
    BlockNumber metablkno;
    BlockNumber headblkno;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
constexpr size_t HnswTupleDataSize = offsetof(HnswTupleData, neighbors);
#pragma GCC diagnostic pop

#define HNSW_TUPLE_SIZE(level, m) MAXALIGN(HnswTupleDataSize + ((level) + 2) * (m) * sizeof(HnswNeighborData))

#endif /* HNSW_STRUCT_H */
