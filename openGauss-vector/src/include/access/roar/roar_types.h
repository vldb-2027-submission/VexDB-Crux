#ifndef ROAR_TYPES_H
#define ROAR_TYPES_H

#include "postgres.h"
#include "access/genam.h"
#include <vtl/vector>

struct Data_r {
    ItemPointerData heapTid;
    uint32_t edge_r_offset; 
};

struct Edge_r {
    uint32_t data_offset;
};

struct Edges_r {
    Edge_r edges_r[80ul];
    uint16 edge_r_num;
};

#endif