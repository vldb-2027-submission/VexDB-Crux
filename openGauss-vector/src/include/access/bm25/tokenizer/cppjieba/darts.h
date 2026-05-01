/**
 * Copyright ...
 */

#ifndef DARTS_H_
#define DARTS_H_

#include <vtl/vector>
#include <vtl/bitvector>

#include "utils/palloc.h"

namespace Darts {
namespace Details {
class DoubleArrayUnit : public BaseObject {
public:
    DoubleArrayUnit() = default;

    bool has_leaf() const { return ((unit_ >> 8) & 1) == 1; }
    int value() const { return static_cast<int>(unit_ & ((1U << 31) - 1)); }
    uint32 label() const { return unit_ & ((1U << 31) | 0xFF); }
    uint32 offset() const { return (unit_ >> 10) << ((unit_ & (1U << 9)) >> 6); }

private:
    uint32 unit_;
};
}  /* namespace Details */

template <typename, typename, typename T, typename>
class DoubleArrayImpl {
public:
    struct result_pair_type {
        T value;
        size_t length;
    };

    DoubleArrayImpl() = default;
    virtual ~DoubleArrayImpl() {}
    void destroy() { clear(); }

    void set_result(T *result, T value, size_t) const {
        *result = value;
    }

    void set_result(result_pair_type *result, T value, size_t length) const {
        result->value = value;
        result->length = length;
    }

    void set_array(const void *ptr, size_t size = 0) {
        clear();
        array_ = static_cast<const unit_type *>(ptr);
        size_ = size;
    }

    const void *array() const {
        return array_;
    }

    void clear() {
        size_ = 0;
        array_ = NULL;
        if (buf_) {
            delete[] buf_;
            buf_ = NULL;
        }
    }

    size_t unit_size() const { return sizeof(unit_type); }
    size_t size() const { return size_; }
    size_t total_size() const { return unit_size() * size(); }
    size_t nonzero_size() const { return size(); }
    void build(size_t num_keys, const char * const *keys,
        const size_t *lengths = NULL, const T *values = NULL);

    template <class U>
    void exactMatchSearch(const char *key, U &result,
        size_t length = 0, size_t node_pos = 0) const {
        result = exactMatchSearch<U>(key, length, node_pos);
    }

    template <class U>
    inline U exactMatchSearch(const char *key, size_t length = 0,
        size_t node_pos = 0) const;

    template <class U>
    inline size_t commonPrefixSearch(const char *key, U *results,
        size_t max_num_results, size_t length = 0,
        size_t node_pos = 0) const;

    inline T traverse(const char *key, size_t &node_pos,
        size_t &key_pos, size_t length = 0) const;

private:
    typedef Details::DoubleArrayUnit unit_type;

    size_t size_{0};
    const unit_type *array_{NULL};
    unit_type *buf_{NULL};

    /* Disallows copy and assignment. */
    DoubleArrayImpl(const DoubleArrayImpl &);
    DoubleArrayImpl &operator=(const DoubleArrayImpl &);
};

typedef DoubleArrayImpl<void, void, int, void> DoubleArray;

template <typename A, typename B, typename T, typename C>
template <typename U>
inline U DoubleArrayImpl<A, B, T, C>::exactMatchSearch(const char *key,
    size_t length, size_t node_pos) const {
    U result;
    set_result(&result, static_cast<T>(-1), 0);

    unit_type unit = array_[node_pos];
    if (length != 0) {
        for (size_t i = 0; i < length; ++i) {
            node_pos ^= unit.offset() ^ static_cast<uint8>(key[i]);
            unit = array_[node_pos];
            if (unit.label() != static_cast<uint8>(key[i])) {
                return result;
            }
        }
    } else {
        for ( ; key[length] != '\0'; ++length) {
          node_pos ^= unit.offset() ^ static_cast<uint8>(key[length]);
          unit = array_[node_pos];
          if (unit.label() != static_cast<uint8>(key[length])) {
              return result;
          }
        }
    }

    if (!unit.has_leaf()) {
        return result;
    }
    unit = array_[node_pos ^ unit.offset()];
    set_result(&result, static_cast<T>(unit.value()), length);
    return result;
}

template <typename A, typename B, typename T, typename C>
template <typename U>
inline size_t DoubleArrayImpl<A, B, T, C>::commonPrefixSearch(
    const char *key, U *results, size_t max_num_results,
    size_t length, size_t node_pos) const {
    size_t num_results = 0;

    unit_type unit = array_[node_pos];
    node_pos ^= unit.offset();
    if (length != 0) {
        for (size_t i = 0; i < length; ++i) {
            node_pos ^= static_cast<uint8>(key[i]);
            unit = array_[node_pos];
            if (unit.label() != static_cast<uint8>(key[i])) {
                return num_results;
            }

            node_pos ^= unit.offset();
            if (unit.has_leaf()) {
                if (num_results < max_num_results) {
                    set_result(&results[num_results], static_cast<T>(array_[node_pos].value()), i + 1);
                }
                ++num_results;
            }
        }
    } else {
        for ( ; key[length] != '\0'; ++length) {
            node_pos ^= static_cast<uint8>(key[length]);
            unit = array_[node_pos];
            if (unit.label() != static_cast<uint8>(key[length])) {
                return num_results;
            }

            node_pos ^= unit.offset();
            if (unit.has_leaf()) {
                if (num_results < max_num_results) {
                    set_result(&results[num_results], static_cast<T>(array_[node_pos].value()), length + 1);
                }
                ++num_results;
            }
        }
    }

    return num_results;
}

template <typename A, typename B, typename T, typename C>
inline T
DoubleArrayImpl<A, B, T, C>::traverse(const char *key,
    size_t &node_pos, size_t &key_pos, size_t length) const {
    uint32 id = static_cast<uint32>(node_pos);
    unit_type unit = array_[id];

    if (length != 0) {
        for ( ; key_pos < length; ++key_pos) {
            id ^= unit.offset() ^ static_cast<uint8>(key[key_pos]);
            unit = array_[id];
            if (unit.label() != static_cast<uint8>(key[key_pos])) {
                return static_cast<T>(-2);
            }
            node_pos = id;
        }
    } else {
        for ( ; key[key_pos] != '\0'; ++key_pos) {
            id ^= unit.offset() ^ static_cast<uint8>(key[key_pos]);
            unit = array_[id];
            if (unit.label() != static_cast<uint8>(key[key_pos])) {
                return static_cast<T>(-2);
            }
            node_pos = id;
        }
    }

    if (!unit.has_leaf()) {
        return static_cast<T>(-1);
    }
    unit = array_[id ^ unit.offset()];
    return static_cast<T>(unit.value());
}

namespace Details {
template <typename T>
class Keyset {
public:
    Keyset(size_t num_keys, const char * const *keys, const size_t *lengths, const T *values)
        : num_keys_(num_keys), keys_(keys), lengths_(lengths), values_(values) {}

    size_t num_keys() const { return num_keys_; }
    const char *keys(size_t id) const { return keys_[id]; }
    uint8 keys(size_t key_id, size_t char_id) const {
        if (has_lengths() && char_id >= lengths_[key_id]) {
            return '\0';
        }
        return keys_[key_id][char_id];
    }

    bool has_lengths() const { return lengths_ != NULL; }
    size_t lengths(size_t id) const {
        if (has_lengths()) {
            return lengths_[id];
        }
        size_t length = 0;
        while (keys_[id][length] != '\0') {
            ++length;
        }
        return length;
    }

    bool has_values() const { return values_ != NULL; }
    const int values(size_t id) const {
        if (has_values()) {
            return static_cast<int>(values_[id]);
        }
        return static_cast<int>(id);
    }

private:
    size_t num_keys_;
    const char * const * keys_;
    const size_t *lengths_;
    const T *values_;

    // Disallows copy and assignment.
    Keyset(const Keyset &);
    Keyset &operator=(const Keyset &);
};

/* Node of Directed Acyclic Word Graph (DAWG). */
class DawgNode {
public:
    DawgNode() = default;

    void set_child(uint32 child) { child_ = child; }
    void set_sibling(uint32 sibling) { sibling_ = sibling; }
    void set_value(int value) { child_ = value; }
    void set_label(uint8 label) { label_ = label; }
    void set_is_state(bool is_state) { is_state_ = is_state; }
    void set_has_sibling(bool has_sibling) { has_sibling_ = has_sibling; }

    uint32 child() const { return child_; }
    uint32 sibling() const { return sibling_; }
    int value() const { return static_cast<int>(child_); }
    uint8 label() const { return label_; }
    bool is_state() const { return is_state_; }
    bool has_sibling() const { return has_sibling_; }

    uint32 unit() const {
        if (label_ == '\0') {
            return (child_ << 1) | (has_sibling_ ? 1 : 0);
        }
        return (child_ << 2) | (is_state_ ? 2 : 0) | (has_sibling_ ? 1 : 0);
    }

private:
    uint32 child_{0};
    uint32 sibling_{0};
    uint8 label_{'\0'};
    bool is_state_{false};
    bool has_sibling_{false};

    /* Copyable. */
};

/* Fixed unit of Directed Acyclic Word Graph (DAWG). */
class DawgUnit {
public:
    explicit DawgUnit(uint32 unit = 0) : unit_(unit) {}
    DawgUnit(const DawgUnit &unit) : unit_(unit.unit_) {}

    DawgUnit &operator=(uint32 unit) {
        unit_ = unit;
        return *this;
    }

    uint32 unit() const { return unit_; }
    uint32 child() const { return unit_ >> 2; }
    bool has_sibling() const { return (unit_ & 1) == 1; }
    int value() const { return static_cast<int>(unit_ >> 1); }
    bool is_state() const { return (unit_ & 2) == 2; }

private:
    uint32 unit_;
    /* Copyable. */
};

/* Directed Acyclic Word Graph (DAWG) builder. */
class DawgBuilder {
public:
    DawgBuilder() = default;
    void destroy() { clear(); }

    uint32 root() const { return 0; }
    uint32 child(uint32 id) const { return units_[id].child(); }
    uint32 sibling(uint32 id) const { return units_[id].has_sibling() ? (id + 1) : 0; }
    int value(uint32 id) const { return units_[id].value(); }
    bool is_leaf(uint32 id) const { return label(id) == '\0'; }
    uint8 label(uint32 id) const { return labels_[id]; }
    bool is_intersection(uint32 id) const { return is_intersections_[id]; }
    uint32 intersection_id(uint32 id) const { return is_intersections_.rank(id) - 1; }
    size_t num_intersections() const { return is_intersections_.num_ones(); }
    size_t size() const { return units_.size(); }

    void init();
    void finish();
    void insert(const char *key, size_t length, int value);
    void clear();

private:
    enum { INITIAL_TABLE_SIZE = 1 << 10 };

    Vector<DawgNode> nodes_;
    Vector<DawgUnit> units_;
    Vector<uint8> labels_;
    BitVector<> is_intersections_;
    Vector<uint32> table_;
    Vector<uint32> node_stack_;
    Vector<uint32> recycle_bin_;
    size_t num_states_{0};

    // Disallows copy and assignment.
    DawgBuilder(const DawgBuilder &);
    DawgBuilder &operator=(const DawgBuilder &);

    void flush(uint32 id);

    void expand_table();

    uint32 find_unit(uint32 id, uint32 *hash_id) const;
    uint32 find_node(uint32 node_id, uint32 *hash_id) const;

    bool are_equal(uint32 node_id, uint32 unit_id) const;

    uint32 hash_unit(uint32 id) const;
    uint32 hash_node(uint32 id) const;

    uint32 append_node();
    uint32 append_unit();

    void free_node(uint32 id) { recycle_bin_.push_back(id); }

    static uint32 hash(uint32 key) {
        key = ~key + (key << 15);   /* key = (key << 15) - key - 1; */
        key = key ^ (key >> 12);
        key = key + (key << 2);
        key = key ^ (key >> 4);
        key = key * 2057;           /* key = (key + (key << 3)) + (key << 11); */
        key = key ^ (key >> 16);
        return key;
    }
};

inline void DawgBuilder::init() {
    table_.resize(INITIAL_TABLE_SIZE, 0);

    append_node();
    append_unit();

    num_states_ = 1ul;

    nodes_[0].set_label(0xFF);
    node_stack_.push_back(0);
}

inline void DawgBuilder::finish() {
    flush(0);

    units_[0] = nodes_[0].unit();
    labels_[0] = nodes_[0].label();

    nodes_.clear();
    table_.clear();
    node_stack_.clear();
    recycle_bin_.clear();
}

inline void DawgBuilder::insert(const char *key, size_t length, int value) {
    uint32 id = 0;
    size_t key_pos = 0;

    for (; key_pos <= length; ++key_pos) {
        uint32 child_id = nodes_[id].child();
        if (child_id == 0) {
            break;
        }

        uint8 key_label = static_cast<uint8>(key[key_pos]);
        uint8 unit_label = nodes_[child_id].label();
        if (key_label > unit_label) {
            nodes_[child_id].set_has_sibling(true);
            flush(child_id);
            break;
        }
        id = child_id;
    }

    if (key_pos > length) {
      return;
    }

    for (; key_pos <= length; ++key_pos) {
        uint8 key_label = static_cast<uint8>((key_pos < length) ? key[key_pos] : '\0');
        uint32 child_id = append_node();

        if (nodes_[id].child() == 0) {
            nodes_[child_id].set_is_state(true);
        }
        nodes_[child_id].set_sibling(nodes_[id].child());
        nodes_[child_id].set_label(key_label);
        nodes_[id].set_child(child_id);
        node_stack_.push_back(child_id);

        id = child_id;
    }
    nodes_[id].set_value(value);
}

inline void DawgBuilder::clear() {
    nodes_.clear();
    units_.clear();
    labels_.clear();
    is_intersections_.clear();
    table_.clear();
    node_stack_.clear();
    recycle_bin_.clear();
    num_states_ = 0;
}

inline void DawgBuilder::flush(uint32 id) {
    while (node_stack_.back() != id) {
        uint32 node_id = node_stack_.back();
        node_stack_.pop_back();

        if (num_states_ >= table_.size() - (table_.size() >> 2)) {
            expand_table();
        }

        uint32 num_siblings = 0;
        for (uint32 i = node_id; i != 0; i = nodes_[i].sibling()) {
            ++num_siblings;
        }

        uint32 hash_id;
        uint32 match_id = find_node(node_id, &hash_id);
        if (match_id != 0) {
            is_intersections_.set(match_id, true);
        } else {
            uint32 unit_id = 0;
            for (uint32 i = 0; i < num_siblings; ++i) {
                unit_id = append_unit();
            }
            for (uint32 i = node_id; i != 0; i = nodes_[i].sibling()) {
                units_[unit_id] = nodes_[i].unit();
                labels_[unit_id] = nodes_[i].label();
                --unit_id;
            }
            match_id = unit_id + 1;
            table_[hash_id] = match_id;
            ++num_states_;
        }

        for (uint32 i = node_id, next; i != 0; i = next) {
            next = nodes_[i].sibling();
            free_node(i);
        }

        nodes_[node_stack_.back()].set_child(match_id);
    }
    node_stack_.pop_back();
}

inline void DawgBuilder::expand_table() {
    size_t table_size = table_.size() << 1;
    table_.clear();
    table_.resize(table_size, 0);

    for (size_t i = 1; i < units_.size(); ++i) {
        uint32 id = static_cast<uint32>(i);
        if (labels_[id] == '\0' || units_[id].is_state()) {
            uint32 hash_id;
            find_unit(id, &hash_id);
            table_[hash_id] = id;
        }
    }
}

inline uint32 DawgBuilder::find_unit(uint32 id, uint32 *hash_id) const {
    *hash_id = hash_unit(id) % table_.size();
    for ( ; ; *hash_id = (*hash_id + 1) % table_.size()) {
        uint32 unit_id = table_[*hash_id];
        if (unit_id == 0) {
            break;
        }
        /* There must not be the same unit. */
    }
    return 0;
}

inline uint32 DawgBuilder::find_node(uint32 node_id, uint32 *hash_id) const {
    *hash_id = hash_node(node_id) % table_.size();
    for (;; *hash_id = (*hash_id + 1) % table_.size()) {
        uint32 unit_id = table_[*hash_id];
        if (unit_id == 0) {
            break;
        }

        if (are_equal(node_id, unit_id)) {
            return unit_id;
        }
    }
    return 0;
}

inline bool DawgBuilder::are_equal(uint32 node_id, uint32 unit_id) const {
    for (uint32 i = nodes_[node_id].sibling(); i != 0; i = nodes_[i].sibling()) {
        if (units_[unit_id].has_sibling() == false) {
            return false;
        }
        ++unit_id;
    }
    if (units_[unit_id].has_sibling() == true) {
        return false;
    }

    for (uint32 i = node_id; i != 0; i = nodes_[i].sibling(), --unit_id) {
        if (nodes_[i].unit() != units_[unit_id].unit() || nodes_[i].label() != labels_[unit_id]) {
            return false;
        }
    }
    return true;
}

inline uint32 DawgBuilder::hash_unit(uint32 id) const {
    uint32 hash_value = 0;
    for (; id != 0; ++id) {
        uint32 unit = units_[id].unit();
        uint8 label = labels_[id];
        hash_value ^= hash((label << 24) ^ unit);

        if (units_[id].has_sibling() == false) {
            break;
        }
    }
    return hash_value;
}

inline uint32 DawgBuilder::hash_node(uint32 id) const {
    uint32 hash_value = 0;
    for (; id != 0; id = nodes_[id].sibling()) {
        uint32 unit = nodes_[id].unit();
        uint8 label = nodes_[id].label();
        hash_value ^= hash((label << 24) ^ unit);
    }
    return hash_value;
  }

inline uint32 DawgBuilder::append_unit() {
    is_intersections_.append();
    units_.resize(units_.size() + 1ul);
    labels_.resize(labels_.size() + 1ul);

    return static_cast<uint32>(is_intersections_.size() - 1ul);
}

inline uint32 DawgBuilder::append_node() {
    uint32 id;
    if (recycle_bin_.empty()) {
        id = static_cast<uint32>(nodes_.size());
        nodes_.resize(nodes_.size() + 1ul);
    } else {
        id = recycle_bin_.back();
        nodes_[id] = DawgNode();
        recycle_bin_.pop_back();
    }
    return id;
}

/* Unit of double-array builder. */
class DoubleArrayBuilderUnit {
public:
    DoubleArrayBuilderUnit() = default;
    void set_has_leaf(bool has_leaf) {
        if (has_leaf) {
            unit_ |= 1U << 8;
        } else {
            unit_ &= ~(1U << 8);
        }
    }

    void set_value(int value) { unit_ = value | (1U << 31); }

    void set_label(uint8 label) { unit_ = (unit_ & ~0xFFU) | label; }

    void set_offset(uint32 offset) {
        unit_ &= (1U << 31) | (1U << 8) | 0xFF;
        if (offset < 1U << 21) {
            unit_ |= (offset << 10);
        } else {
            unit_ |= (offset << 2) | (1U << 9);
        }
    }

private:
    uint32 unit_{0};
};

/* Extra unit of double-array builder. */
class DoubleArrayBuilderExtraUnit : public BaseObject {
public:
    DoubleArrayBuilderExtraUnit() = default;
    void set_prev(uint32 prev) { prev_ = prev; }
    void set_next(uint32 next) { next_ = next; }
    void set_is_fixed(bool is_fixed) { is_fixed_ = is_fixed; }
    void set_is_used(bool is_used) { is_used_ = is_used; }

    uint32 prev() const { return prev_; }
    uint32 next() const { return next_; }
    bool is_fixed() const { return is_fixed_; }
    bool is_used() const { return is_used_; }

private:
    uint32 prev_{0};
    uint32 next_{0};
    bool is_fixed_{false};
    bool is_used_{false};
};

/* DAWG -> double-array converter. */
class DoubleArrayBuilder {
public:
    DoubleArrayBuilder() = default;
    template <typename T>
    void build(const Keyset<T> &keyset);
    void copy(size_t *size_ptr, DoubleArrayUnit **buf_ptr) const;

    void clear();
    void destroy();

private:
    enum { BLOCK_SIZE = 256 };
    enum { NUM_EXTRA_BLOCKS = 16 };
    enum { NUM_EXTRAS = BLOCK_SIZE * NUM_EXTRA_BLOCKS };

    enum { UPPER_MASK = 0xFF << 21 };
    enum { LOWER_MASK = 0xFF };

    typedef DoubleArrayBuilderUnit unit_type;
    typedef DoubleArrayBuilderExtraUnit extra_type;

    Vector<unit_type> units_;
    Vector<extra_type> extras_;
    Vector<uint8> labels_;
    Vector<uint32> table_;
    uint32 extras_head_{0};

    /* Disallows copy and assignment. */
    DoubleArrayBuilder(const DoubleArrayBuilder &);
    DoubleArrayBuilder &operator=(const DoubleArrayBuilder &);

    size_t num_blocks() const {
      return units_.size() / BLOCK_SIZE;
    }

    const extra_type &extras(uint32 id) const {
      return extras_[id % NUM_EXTRAS];
    }
    extra_type &extras(uint32 id) {
      return extras_[id % NUM_EXTRAS];
    }

    template <typename T>
    void build_dawg(const Keyset<T> &keyset, DawgBuilder *dawg_builder);
    void build_from_dawg(const DawgBuilder &dawg);
    void build_from_dawg(const DawgBuilder &dawg, uint32 dawg_id, uint32 dic_id);
    uint32 arrange_from_dawg(const DawgBuilder &dawg, uint32 dawg_id, uint32 dic_id);

    template <typename T>
    void build_from_keyset(const Keyset<T> &keyset);
    template <typename T>
    void build_from_keyset(const Keyset<T> &keyset, size_t begin, size_t end, size_t depth, uint32 dic_id);
    template <typename T>
    uint32 arrange_from_keyset(const Keyset<T> &keyset, size_t begin, size_t end, size_t depth, uint32 dic_id);

    uint32 find_valid_offset(uint32 id) const;
    bool is_valid_offset(uint32 id, uint32 offset) const;

    void reserve_id(uint32 id);
    void expand_units();

    void fix_all_blocks();
    void fix_block(uint32 block_id);
};

template <typename T>
void DoubleArrayBuilder::build(const Keyset<T> &keyset) {
    if (keyset.has_values()) {
        Details::DawgBuilder dawg_builder;
        build_dawg(keyset, &dawg_builder);
        build_from_dawg(dawg_builder);
        dawg_builder.clear();
    } else {
        build_from_keyset(keyset);
    }
}

inline void DoubleArrayBuilder::copy(size_t *size_ptr, DoubleArrayUnit **buf_ptr) const {
    if (size_ptr != NULL) {
        *size_ptr = units_.size();
    }
    if (buf_ptr != NULL) {
        *buf_ptr = NEW DoubleArrayUnit[units_.size()];
        unit_type *units = reinterpret_cast<unit_type *>(*buf_ptr);
        for (size_t i = 0; i < units_.size(); ++i) {
            units[i] = units_[i];
        }
    }
}

inline void DoubleArrayBuilder::clear() {
    units_.clear();
    extras_.clear();
    labels_.clear();
    table_.clear();
    extras_head_ = 0;
}

inline void DoubleArrayBuilder::destroy() {
    ann_helper::optional_destroy(units_);
    ann_helper::optional_destroy(extras_);
    ann_helper::optional_destroy(labels_);
    ann_helper::optional_destroy(table_);
}

template <typename T>
void DoubleArrayBuilder::build_dawg(const Keyset<T> &keyset, DawgBuilder *dawg_builder) {
    dawg_builder->init();
    for (size_t i = 0; i < keyset.num_keys(); ++i) {
        dawg_builder->insert(keyset.keys(i), keyset.lengths(i), keyset.values(i));
    }
    dawg_builder->finish();
}

inline void DoubleArrayBuilder::build_from_dawg(const DawgBuilder &dawg) {
    size_t num_units = 1;
    while (num_units < dawg.size()) {
        num_units <<= 1;
    }
    units_.reserve(num_units);

    table_.resize(dawg.num_intersections());
    for (size_t i = 0; i < dawg.num_intersections(); ++i) {
        table_[i] = 0;
    }

    extras_.resize(NUM_EXTRAS);

    reserve_id(0);
    extras(0).set_is_used(true);
    units_[0].set_offset(1);
    units_[0].set_label('\0');

    if (dawg.child(dawg.root()) != 0) {
        build_from_dawg(dawg, dawg.root(), 0);
    }

    fix_all_blocks();

    extras_.clear();
    labels_.clear();
    table_.clear();
}

inline void DoubleArrayBuilder::build_from_dawg(const DawgBuilder &dawg, uint32 dawg_id, uint32 dic_id) {
    uint32 dawg_child_id = dawg.child(dawg_id);
    if (dawg.is_intersection(dawg_child_id)) {
        uint32 intersection_id = dawg.intersection_id(dawg_child_id);
        uint32 offset = table_[intersection_id];
        if (offset != 0) {
          offset ^= dic_id;
          if (!(offset & UPPER_MASK) || !(offset & LOWER_MASK)) {
              if (dawg.is_leaf(dawg_child_id)) {
                  units_[dic_id].set_has_leaf(true);
              }
              units_[dic_id].set_offset(offset);
              return;
          }
        }
    }

    uint32 offset = arrange_from_dawg(dawg, dawg_id, dic_id);
    if (dawg.is_intersection(dawg_child_id)) {
        table_[dawg.intersection_id(dawg_child_id)] = offset;
    }

    do {
        uint8 child_label = dawg.label(dawg_child_id);
        uint32 dic_child_id = offset ^ child_label;
        if (child_label != '\0') {
            build_from_dawg(dawg, dawg_child_id, dic_child_id);
        }
        dawg_child_id = dawg.sibling(dawg_child_id);
    } 
    while (dawg_child_id != 0);
}

inline uint32 DoubleArrayBuilder::arrange_from_dawg(const DawgBuilder &dawg, uint32 dawg_id, uint32 dic_id) {
    labels_.resize(0);

    uint32 dawg_child_id = dawg.child(dawg_id);
    while (dawg_child_id != 0) {
        labels_.resize(labels_.size() + 1, dawg.label(dawg_child_id));
        dawg_child_id = dawg.sibling(dawg_child_id);
    }

    uint32 offset = find_valid_offset(dic_id);
    units_[dic_id].set_offset(dic_id ^ offset);

    dawg_child_id = dawg.child(dawg_id);
    for (size_t i = 0; i < labels_.size(); ++i) {
        uint32 dic_child_id = offset ^ labels_[i];
        reserve_id(dic_child_id);

        if (dawg.is_leaf(dawg_child_id)) {
            units_[dic_id].set_has_leaf(true);
            units_[dic_child_id].set_value(dawg.value(dawg_child_id));
        } else {
            units_[dic_child_id].set_label(labels_[i]);
        }

        dawg_child_id = dawg.sibling(dawg_child_id);
    }
    extras(offset).set_is_used(true);

    return offset;
}

template <typename T>
void DoubleArrayBuilder::build_from_keyset(const Keyset<T> &keyset) {
    size_t num_units = 1;
    while (num_units < keyset.num_keys()) {
        num_units <<= 1;
    }
    units_.reserve(num_units);

    extras_.resize(NUM_EXTRAS);

    reserve_id(0);
    extras(0).set_is_used(true);
    units_[0].set_offset(1);
    units_[0].set_label('\0');

    if (keyset.num_keys() > 0) {
        build_from_keyset(keyset, 0, keyset.num_keys(), 0, 0);
    }

    fix_all_blocks();

    extras_.clear();
    labels_.clear();
}

template <typename T>
void DoubleArrayBuilder::build_from_keyset(
    const Keyset<T> &keyset, size_t begin, 
    size_t end, size_t depth, uint32 dic_id) {
    uint32 offset = arrange_from_keyset(keyset, begin, end, depth, dic_id);

    while (begin < end) {
        if (keyset.keys(begin, depth) != '\0') {
            break;
        }
        ++begin;
    }
    if (begin == end) {
        return;
    }

    size_t last_begin = begin;
    uint8 last_label = keyset.keys(begin, depth);
    while (++begin < end) {
        uint8 label = keyset.keys(begin, depth);
        if (label != last_label) {
            build_from_keyset(keyset, last_begin, begin,
                depth + 1, offset ^ last_label);
            last_begin = begin;
            last_label = keyset.keys(begin, depth);
        }
    }
    build_from_keyset(keyset, last_begin, end, depth + 1, offset ^ last_label);
}

template <typename T>
uint32 DoubleArrayBuilder::arrange_from_keyset(const Keyset<T> &keyset,
    size_t begin, size_t end, size_t depth, uint32 dic_id) {
    labels_.resize(0);

    int value = -1;
    for (size_t i = begin; i < end; ++i) {
        uint8 label = keyset.keys(i, depth);
        if (label == '\0') {
            if (value == -1) {
                value = keyset.values(i);
            }
        }

        if (labels_.empty()) {
            labels_.resize(labels_.size() + 1, label);
        } else if (label != labels_[labels_.size() - 1]) {
            labels_.resize(labels_.size() + 1, label);
        }
    }

    uint32 offset = find_valid_offset(dic_id);
    units_[dic_id].set_offset(dic_id ^ offset);

    for (size_t i = 0; i < labels_.size(); ++i) {
        uint32 dic_child_id = offset ^ labels_[i];
        reserve_id(dic_child_id);
        if (labels_[i] == '\0') {
            units_[dic_id].set_has_leaf(true);
            units_[dic_child_id].set_value(value);
        } else {
            units_[dic_child_id].set_label(labels_[i]);
        }
    }
    extras(offset).set_is_used(true);

    return offset;
}

inline uint32 DoubleArrayBuilder::find_valid_offset(uint32 id) const {
    if (extras_head_ >= units_.size()) {
        return units_.size() | (id & LOWER_MASK);
    }

    uint32 unfixed_id = extras_head_;
    do {
        uint32 offset = unfixed_id ^ labels_[0];
        if (is_valid_offset(id, offset)) {
            return offset;
        }
        unfixed_id = extras(unfixed_id).next();
    } 
    while (unfixed_id != extras_head_);

    return units_.size() | (id & LOWER_MASK);
}

inline bool DoubleArrayBuilder::is_valid_offset(uint32 id, uint32 offset) const {
    if (extras(offset).is_used()) {
        return false;
    }

    uint32 rel_offset = id ^ offset;
    if ((rel_offset & LOWER_MASK) && (rel_offset & UPPER_MASK)) {
        return false;
    }

    for (size_t i = 1; i < labels_.size(); ++i) {
        if (extras(offset ^ labels_[i]).is_fixed()) {
            return false;
        }
    }

    return true;
}

inline void DoubleArrayBuilder::reserve_id(uint32 id) {
    if (id >= units_.size()) {
        expand_units();
    }

    if (id == extras_head_) {
        extras_head_ = extras(id).next();
        if (extras_head_ == id) {
            extras_head_ = units_.size();
        }
    }
    extras(extras(id).prev()).set_next(extras(id).next());
    extras(extras(id).next()).set_prev(extras(id).prev());
    extras(id).set_is_fixed(true);
}

inline void DoubleArrayBuilder::expand_units() {
    uint32 src_num_units = units_.size();
    uint32 src_num_blocks = num_blocks();

    uint32 dest_num_units = src_num_units + BLOCK_SIZE;
    uint32 dest_num_blocks = src_num_blocks + 1;

    if (dest_num_blocks > NUM_EXTRA_BLOCKS) {
        fix_block(src_num_blocks - NUM_EXTRA_BLOCKS);
    }

    units_.resize(dest_num_units);

    if (dest_num_blocks > NUM_EXTRA_BLOCKS) {
        for (size_t id = src_num_units; id < dest_num_units; ++id) {
            extras(id).set_is_used(false);
            extras(id).set_is_fixed(false);
        }
    }

    for (uint32 i = src_num_units + 1; i < dest_num_units; ++i) {
        extras(i - 1).set_next(i);
        extras(i).set_prev(i - 1);
    }

    extras(src_num_units).set_prev(dest_num_units - 1);
    extras(dest_num_units - 1).set_next(src_num_units);

    extras(src_num_units).set_prev(extras(extras_head_).prev());
    extras(dest_num_units - 1).set_next(extras_head_);

    extras(extras(extras_head_).prev()).set_next(src_num_units);
    extras(extras_head_).set_prev(dest_num_units - 1);
}

inline void DoubleArrayBuilder::fix_all_blocks() {
    uint32 begin = 0;
    if (num_blocks() > NUM_EXTRA_BLOCKS) {
        begin = num_blocks() - NUM_EXTRA_BLOCKS;
    }
    uint32 end = num_blocks();

    for (uint32 block_id = begin; block_id != end; ++block_id) {
        fix_block(block_id);
    }
}

inline void DoubleArrayBuilder::fix_block(uint32 block_id) {
    uint32 begin = block_id * BLOCK_SIZE;
    uint32 end = begin + BLOCK_SIZE;

    uint32 unused_offset = 0;
    for (uint32 offset = begin; offset != end; ++offset) {
        if (!extras(offset).is_used()) {
            unused_offset = offset;
            break;
        }
    }

    for (uint32 id = begin; id != end; ++id) {
        if (!extras(id).is_fixed()) {
            reserve_id(id);
            units_[id].set_label(static_cast<uint8>(id ^ unused_offset));
        }
    }
}

}  /* namespace Details */

template <typename A, typename B, typename T, typename C>
void DoubleArrayImpl<A, B, T, C>::build(size_t num_keys, const char * const *keys,
                                        const size_t *lengths, const T *values) {
    Details::Keyset<T> keyset(num_keys, keys, lengths, values);
    Details::DoubleArrayBuilder builder;
    builder.build(keyset);

    size_t size = 0;
    unit_type *buf = NULL;
    builder.copy(&size, &buf);
    builder.destroy();
    
    clear();
    size_ = size;
    array_ = buf;
    buf_ = buf;
}

}  /* namespace Darts */
#endif  /* DARTS_H_ */
