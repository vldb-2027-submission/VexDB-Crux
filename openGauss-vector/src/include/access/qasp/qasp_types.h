#ifndef QASP_TYPES_H
#define QASP_TYPES_H

#include "postgres.h"
#include "access/genam.h"
#include <vtl/vector>
#include <vtl/bitvector>

#define NUM_SEMANTIC_CLUSTER 20
#define BASE_EDGE_CAPACITY 160
#define OVERFLOW_EDGE_CAPACITY 64

inline uint32_t encode_edge(uint32_t semantic_id, uint32_t target_id) {
    return (semantic_id << 27) | target_id;
}
inline uint32_t decode_semantic(uint32_t edge_info) { return edge_info >> 27; }
inline uint32_t decode_target(uint32_t edge_info) { return edge_info & 0x7FFFFFF; }

struct ScanData {
    BitSet<20ul> cluster;       
    ItemPointerData heapTid;    
};

struct BaseEdges {
    uint16_t start_idx[NUM_SEMANTIC_CLUSTER + 1]; 
    uint16_t edge_num;                           
    uint32_t overflow_offset;                    
    uint32_t edges[BASE_EDGE_CAPACITY];          
};

struct OverflowBucket {
    uint16_t start_idx[NUM_SEMANTIC_CLUSTER + 1]; 
    uint16_t edge_num;                           
    uint32_t next_bucket_offset;                 
    uint32_t edges[OVERFLOW_EDGE_CAPACITY];      
};

struct Edge {
    uint32_t data_offset;
};

struct Edges {
    Edge edges[40ul];
    uint16 edge_num;
};

struct QuerySubIndexNeighbors {
    uint32_t data_offset_subindex[30ul];
};

struct QueryPoints {
    uint16 cluster;
};

struct Neighborhood {
    uint32_t neighbors[30ul];
    uint16 neighborhood_size;
};

struct edgeNumReminder {
    uint32 Initialization_edges_num[20ul];
    uint32 Inter_cluster_edges_num[20ul];
};

#endif