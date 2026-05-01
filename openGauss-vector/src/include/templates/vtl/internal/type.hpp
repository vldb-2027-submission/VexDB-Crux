/**
 * Copyright ...
 * Type utilities.
 */

#ifndef ANN_HELPER_TYPE_H
#define ANN_HELPER_TYPE_H

#include <boost/preprocessor/seq.hpp>
#include <boost/preprocessor/repeat.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <vtl/disk_container/disk_hashtable_dependency.hpp>

#include "c.h"
#include "access/diskann/diskann.h"
#include "access/bm25/bm25_struct.h"
#include "access/qasp/qasp_types.h"
#include "access/roar/roar_types.h"

namespace ann_helper {
namespace internal {
template<uint16 N> struct marker_id { static uint16 const value = N; };
template<typename T> struct marker_type { typedef T type; };
template<typename T, uint16 N>
struct register_id : marker_id<N>, marker_type<T> {
private:
    friend marker_type<T> marked_id(marker_id<N>) {
        return marker_type<T>();
    }
};
template <uint16 nattr>
using bm25_doc_store_table_plain_entry =
    disk_container::PlainEntryWithoutHash<uint64, bm25::DocInfo<nattr>>;
template <uint16 nattr>
using bm25_doc_store_table_global_entry =
    disk_container::PlainEntryWithoutHash<uint64, bm25::DocOidInfo<nattr>>;
using bm25_ss_plain_entry =
    disk_container::PlainEntryWithoutHash<bm25::ShortToken, bm25::TokenIndexEntry>;
using bm25_ms_plain_entry =
    disk_container::PlainEntryWithoutHash<bm25::MidToken, bm25::TokenIndexEntry>;
using bm25_ls_plain_entry =
    disk_container::PlainEntryWithoutHash<bm25::LongToken, bm25::TokenIndexEntry>;
using bm25_fs_plain_entry =
    disk_container::PlainEntryWithHash<bm25::FullToken, bm25::TokenIndexEntry>;
using bm25_test_plain_entry =
    disk_container::PlainEntryWithoutHash<uint32, uint32>;
} /* namespace internal */

struct UnknownType {};
#define TYPES (UnknownType)(float)(uint8)(uint32)(size_t)(DiskAnnVamanaNode)(AnnNeighbors)(Data_r)(Edge_r)(Edges_r)  \
              (ScanData)(BaseEdges)(OverflowBucket)(Edges)(QueryPoints)(Neighborhood)(QuerySubIndexNeighbors)(edgeNumReminder) \
              (char)(disk_container::HashEntry)(bm25::FixedInvertedList<4u>)                \
              (bm25::FixedInvertedList<32u>)(bm25::FixedInvertedList<162u>)                 \
              (internal::bm25_doc_store_table_plain_entry<1u>)                              \
              (internal::bm25_doc_store_table_plain_entry<2u>)                              \
              (internal::bm25_doc_store_table_plain_entry<3u>)                              \
              (internal::bm25_doc_store_table_plain_entry<4u>)                              \
              (internal::bm25_doc_store_table_plain_entry<5u>)                              \
              (internal::bm25_doc_store_table_plain_entry<6u>)                              \
              (internal::bm25_doc_store_table_plain_entry<7u>)                              \
              (internal::bm25_doc_store_table_plain_entry<8u>)                              \
              (internal::bm25_doc_store_table_plain_entry<9u>)                              \
              (internal::bm25_doc_store_table_plain_entry<10u>)                             \
              (internal::bm25_doc_store_table_plain_entry<11u>)                             \
              (internal::bm25_doc_store_table_plain_entry<12u>)                             \
              (internal::bm25_doc_store_table_plain_entry<13u>)                             \
              (internal::bm25_doc_store_table_plain_entry<14u>)                             \
              (internal::bm25_doc_store_table_plain_entry<15u>)                             \
              (internal::bm25_doc_store_table_plain_entry<16u>)                             \
              (internal::bm25_doc_store_table_plain_entry<17u>)                             \
              (internal::bm25_doc_store_table_plain_entry<18u>)                             \
              (internal::bm25_doc_store_table_plain_entry<19u>)                             \
              (internal::bm25_doc_store_table_plain_entry<20u>)                             \
              (internal::bm25_doc_store_table_plain_entry<21u>)                             \
              (internal::bm25_doc_store_table_plain_entry<22u>)                             \
              (internal::bm25_doc_store_table_plain_entry<23u>)                             \
              (internal::bm25_doc_store_table_plain_entry<24u>)                             \
              (internal::bm25_doc_store_table_plain_entry<25u>)                             \
              (internal::bm25_doc_store_table_plain_entry<26u>)                             \
              (internal::bm25_doc_store_table_plain_entry<27u>)                             \
              (internal::bm25_doc_store_table_plain_entry<28u>)                             \
              (internal::bm25_doc_store_table_plain_entry<29u>)                             \
              (internal::bm25_doc_store_table_plain_entry<30u>)                             \
              (internal::bm25_doc_store_table_plain_entry<31u>)                             \
              (internal::bm25_doc_store_table_plain_entry<32u>)                             \
              (internal::bm25_doc_store_table_global_entry<1u>)                             \
              (internal::bm25_doc_store_table_global_entry<2u>)                             \
              (internal::bm25_doc_store_table_global_entry<3u>)                             \
              (internal::bm25_doc_store_table_global_entry<4u>)                             \
              (internal::bm25_doc_store_table_global_entry<5u>)                             \
              (internal::bm25_doc_store_table_global_entry<6u>)                             \
              (internal::bm25_doc_store_table_global_entry<7u>)                             \
              (internal::bm25_doc_store_table_global_entry<8u>)                             \
              (internal::bm25_doc_store_table_global_entry<9u>)                             \
              (internal::bm25_doc_store_table_global_entry<10u>)                            \
              (internal::bm25_doc_store_table_global_entry<11u>)                            \
              (internal::bm25_doc_store_table_global_entry<12u>)                            \
              (internal::bm25_doc_store_table_global_entry<13u>)                            \
              (internal::bm25_doc_store_table_global_entry<14u>)                            \
              (internal::bm25_doc_store_table_global_entry<15u>)                            \
              (internal::bm25_doc_store_table_global_entry<16u>)                            \
              (internal::bm25_doc_store_table_global_entry<17u>)                            \
              (internal::bm25_doc_store_table_global_entry<18u>)                            \
              (internal::bm25_doc_store_table_global_entry<19u>)                            \
              (internal::bm25_doc_store_table_global_entry<20u>)                            \
              (internal::bm25_doc_store_table_global_entry<21u>)                            \
              (internal::bm25_doc_store_table_global_entry<22u>)                            \
              (internal::bm25_doc_store_table_global_entry<23u>)                            \
              (internal::bm25_doc_store_table_global_entry<24u>)                            \
              (internal::bm25_doc_store_table_global_entry<25u>)                            \
              (internal::bm25_doc_store_table_global_entry<26u>)                            \
              (internal::bm25_doc_store_table_global_entry<27u>)                            \
              (internal::bm25_doc_store_table_global_entry<28u>)                            \
              (internal::bm25_doc_store_table_global_entry<29u>)                            \
              (internal::bm25_doc_store_table_global_entry<30u>)                            \
              (internal::bm25_doc_store_table_global_entry<31u>)                            \
              (internal::bm25_doc_store_table_global_entry<32u>)                            \
              (internal::bm25_ss_plain_entry)(internal::bm25_ms_plain_entry)                \
              (internal::bm25_ls_plain_entry)(internal::bm25_fs_plain_entry)                \
              (internal::bm25_test_plain_entry)

/**
 * @brief Get the type id of the given type.
 * e.g. GET_TYPE_ID(float) returns 1, and GET_TYPE(1) returns float,
 *      float can be deduced as template but 1 has to be constant number.
 * add type:
 *  add type to above TYPES sequence, format: (type1)(type2)(type3)...
 * implementation:
 *  declare `template<> struct TypeIdMap<float> : internal::register_id<float, 1> {};`
 *  using macro BOOST_PP_SEQ_FOR_EACH_I with SET_TYPE to generate the code,
 *  GET_TYPE_ID expands to TypeIdMap<type>::value which is a template specialization.
 *  GET_TYPE expands to BOOST_PP_SEQ_ELEM(id, TYPES) which is a sequence element,
 *      that's why it requires constant integral input.
 */
#define NUM_TYPES BOOST_PP_SEQ_SIZE(TYPES)
template<typename T> struct TypeIdMap : internal::register_id<T, __UINT16_MAX__> {};
#define SET_TYPE(r, data, i, elem) template<> struct TypeIdMap<elem> : internal::register_id<elem, i> {};
BOOST_PP_SEQ_FOR_EACH_I(SET_TYPE, ~, TYPES)
#undef SET_TYPE
#define GET_TYPE_ID(type) TypeIdMap<type>::value
#define GET_TYPE(id) BOOST_PP_SEQ_ELEM(id, TYPES)
} /* namespace ann_helper */

#endif /* ANN_HELPER_TYPE_H */
