/**
 * Copyright ...
 */

#ifndef CONTAINER_H
#define CONTAINER_H

#include "c.h"

#define USE_INTERNAL_MEMORY true
#define CONTAINER_USE_STL false
#define CONTAINER_USE_STL_VECTOR (CONTAINER_USE_STL || false)
#define CONTAINER_USE_STL_TREE (CONTAINER_USE_STL || false)
#define CONTAINER_USE_STL_HASH (CONTAINER_USE_STL || false)
#define CONTAINER_USE_STL_PAIR (CONTAINER_USE_STL || false)
#define CONTAINER_USE_STL_OPTIONAL (__cplusplus >= 201703L && (CONTAINER_USE_STL || false))
#define CONTAINER_USE_STL_VARIANT (__cplusplus >= 201703L && (CONTAINER_USE_STL || false))
#define CONTAINER_USE_STL_TUPLE (CONTAINER_USE_STL || false)
#define VERIFY_DATA false
#define BTREE_VERIFY_DATA false

#if USE_INTERNAL_MEMORY
#include "utils/palloc.h"
#define NEW New(CurrentMemoryContext)
#else
#define NEW new
#define New(cxt) new
#endif /* USE_INTERNAL_MEMORY */

struct EmptyObject {};

namespace container::internal {
struct safe_constructor {
    static constexpr bool construct_with_alloc = false;
};
struct unsafe_constructor {
    static constexpr bool construct_with_alloc = true;
};
} /* namespace container::internal */

#if USE_INTERNAL_MEMORY
#define SAFE_CONSTRUCTOR public container::internal::safe_constructor
#define UNSAFE_CONSTRUCTOR public container::internal::unsafe_constructor
#define SAFE_CONSTRUCTOR_DECL() static constexpr bool construct_with_alloc = false
#define UNSAFE_CONSTRUCTOR_DECL() static constexpr bool construct_with_alloc = true
#else
#define SAFE_CONSTRUCTOR public EmptyObject
#define UNSAFE_CONSTRUCTOR public EmptyObject
#define SAFE_CONSTRUCTOR_DECL()
#define UNSAFE_CONSTRUCTOR_DECL()
#endif

#endif /* CONTAINER_H */
