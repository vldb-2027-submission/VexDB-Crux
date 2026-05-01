#ifndef HNSW_CLUSTER_H
#define HNSW_CLUSTER_H

#include <vtl/vector>
#include <vtl/disk_container/plain_store.hpp>

#include "postgres.h"
#include "access/annvector/pq.h"
#include "access/hnsw/hnsw_param.h"

using namespace ann_helper;
using ann_helper::distance_func;

extern bool HnswUseCluster(Relation index);
extern uint32 hnsw_get_vector(float **vecs, Relation heap, Relation index, ItemPointer tids, uint32 ndata, uint32 dim);

struct Cluster {
    uint32 num_cluster;
    bool cluster_pq;
};

template <class Impl>
struct HnswTids {
	const uint8 &get_flag() const { return static_cast<const Impl &>(*this).get_flag(); }
	uint8 &get_flag() { return static_cast<Impl &>(*this).get_flag(); }
	const ItemPointerData *get_heaptids() const
		{ return static_cast<const Impl &>(*this).get_heaptids(); }
	ItemPointerData *get_heaptids() { return static_cast<Impl &>(*this).get_heaptids(); }
	/* internal helper func, use actual_ntids to get actual number of elements */
	uint8 ntids() const { return static_cast<const Impl &>(*this).ntids(); }
	/* make sure you know what is going on when you call this func */
	void set_ntids(uint8 n) { static_cast<Impl &>(*this).set_ntids(n); }

	void init() { get_flag() = 0; }
	bool is_deleted() const { return get_flag() & 0x01; }
	void set_deleted() { get_flag() |= 0x01; }
	bool is_extended() const { return get_flag() & 0x40; }
	void set_extended() { get_flag() |= 0x40; }
	bool is_double_extended() const { return get_flag() & 0x80; }
	void set_double_extended() { get_flag() |= 0x80; }
	void unset_double_extended() { get_flag() &= 0x7f; }

private:
	using PlainStore = disk_container::PlainStore;
	/**
	 * struct layout:
	 * 	uint8[] pq;
	 *  tid[] tids;
	 *  char[] paddings;	// make sure the next struct is aligned
	 */
	struct TidPQHandler {
		uint32 pq_len;
		Size size;
		char *buf;

		size_t pq_size() const { return pq_len * sizeof(uint8); }
		uint32 ndata() const { return size / (pq_size() + sizeof(ItemPointerData)); }
		uint8 *get_pq() { return (uint8 *)buf; }
		ItemPointer get_tid() { return (ItemPointer)(buf + pq_size() * ndata()); }
		void *try_insert(const ItemPointerData &tid, uint8 *&new_pq_code_ptr, size_t &new_size)
		{
			const size_t item_size = pq_size() + sizeof(ItemPointerData);
			new_size = (1u + ndata()) * item_size;
			size_t mod = new_size % vector_aligned_size;
			if (mod != 0 && vector_aligned_size - mod < item_size) {
				if (new_size + vector_aligned_size - mod < PlainStore::max_size) {
					new_size += vector_aligned_size - mod;
				}
			}
			if (new_size >= PlainStore::max_size) {
				return NULL;
			}
			char *temp = (char *)palloc(new_size);
			const size_t pqs = pq_size() * ndata();
			new_pq_code_ptr = (uint8 *)(temp + pqs);
			memcpy(temp, buf, pqs);
			memcpy(temp + pqs + pq_size(), buf + pqs, sizeof(ItemPointerData) * ndata());
			*(ItemPointer)(temp + pqs + pq_size() + sizeof(ItemPointerData) * ndata()) = tid;
			return temp;
		}
		static uint32 fullsize(uint32 pq_len)
		{
			return (PlainStore::max_size - 1ul) / (pq_len * sizeof(uint8) + sizeof(ItemPointerData));
		}
	};
	static constexpr uint32 tid_page_cap = PlainStore::max_size / sizeof(ItemPointerData);
	static void expand_vec_buf(Vector<float *> &vec_holder, uint32 dim, uint32 ndata)
	{
		if (vec_holder.size() < ndata) {
			if (!vec_holder.empty()) {
				free_vector(vec_holder[0]);
			}
			size_t adim = get_aligned_dim(dim);
			uint32 target_len = ndata * 1.5;
			float *vec_buf = alloc_floatvector(adim * target_len);
			vec_holder.clear();
			for (uint32 i = 0; i < target_len; ++i) {
				vec_holder.push_back(vec_buf + i * adim);
			}
		}
	}

public:
	bool insert_tid(Relation index, bool is_building, const ItemPointerData &tid, bool &overwriten)
	{
		if (is_deleted()) {
			return false;
		}
		uint8 nentry = ntids();
		if (!is_extended() && nentry < HNSW_HEAPTIDS) {
			get_heaptids()[nentry] = tid;
			set_ntids(nentry + 1u);
			overwriten = true;
			return true;
		}
		if (!HnswUseCluster(index)) {
			return false;
		}
		bool res = true;
		PlainStore ps(index, HNSW_PS_BLKNO, !is_building);
		if (!is_extended()) {
			set_extended();
			set_ntids(1u);
			ItemPointerData temp_tids[HNSW_HEAPTIDS + 1];
			for (int i = 0; i < HNSW_HEAPTIDS; ++i) {
				temp_tids[i] = get_heaptids()[i];
			}
			temp_tids[HNSW_HEAPTIDS] = tid;
			get_heaptids()[0] = ps.put(temp_tids, sizeof(temp_tids));
			overwriten = true;
		} else {
			Assert(nentry > 0);
			auto key = get_heaptids()[nentry - 1u];
			size_t old_size, new_size;
			void *data;
			ps.get(key, [&](const void *in_data, Size size) {
				old_size = size;
				new_size = old_size + sizeof(ItemPointerData);
				if (new_size >= PlainStore::max_size) {
					return;
				}
				data = palloc(new_size);
				errno_t rc = memcpy_s(data, new_size, in_data, size);
				securec_check(rc, "\0", "\0");
			});
			if (new_size >= PlainStore::max_size) {
				if (nentry < HNSW_HEAPTIDS) {
					get_heaptids()[nentry] = ps.put(&tid, sizeof(ItemPointerData));
					set_ntids(nentry + 1u);
					overwriten = true;
				} else {
					res = false;
				}
			} else {
				*((ItemPointerData *)((char *)data + old_size)) = tid;
				get_heaptids()[nentry - 1u] = ps.set(key, data, new_size);
				overwriten = ItemPointerEqualsNoCheck(&key, get_heaptids() + nentry - 1);
				pfree(data);
			}
		}
		ps.destroy();
		return res;
	}
	bool insert_tid_pq(Relation index, bool is_building, const float *x, const ItemPointerData &tid,
		ProductQuantizer &pq, bool &overwriten)
	{
		if (is_deleted()) {
			return false;
		}
		uint8 nentry = ntids();
		PlainStore ps(index, HNSW_PS_BLKNO, !is_building);
		void *new_data;
		uint8 *new_pq_code;
		size_t new_size;
		const auto get_new_data = [&](const void *in_data, Size size) {
			TidPQHandler handler = {uint32(pq.code_size), size, (char *)in_data};
			new_data = handler.try_insert(tid, new_pq_code, new_size);
		};
		if (!is_double_extended()) {
			ps.get(get_heaptids()[nentry - 1u], get_new_data);
			if (!new_data) {
				const Size s = sizeof(uint8) * pq.code_size + sizeof(ItemPointerData);
				new_data = palloc(s);
				pq.compute_code(x, (uint8 *)new_data);
				*(ItemPointer)((char *)new_data + sizeof(uint8) * pq.code_size) = tid;
				auto k = ps.put(new_data, s);
				if (nentry < HNSW_HEAPTIDS) {
					get_heaptids()[nentry] = k;
					set_ntids(nentry + 1u);
				} else {
					ItemPointerData temp_tids[HNSW_HEAPTIDS + 1];
					memcpy(temp_tids, get_heaptids(), sizeof(ItemPointerData[HNSW_HEAPTIDS]));
					temp_tids[HNSW_HEAPTIDS] = k;
					set_double_extended();
					set_ntids(1u);
					get_heaptids()[0] = ps.put(temp_tids, sizeof(temp_tids));
				}
				overwriten = true;
			} else {
				pq.compute_code(x, (uint8 *)new_data);
				auto k = ps.set(get_heaptids()[nentry - 1u], new_data, new_size);
				overwriten = !ItemPointerEqualsNoCheck(&k, &get_heaptids()[nentry - 1u]);
				if (overwriten) {
					get_heaptids()[nentry - 1] = k;
				}
			}
			pfree(new_data);
		} else {
			uint32 nptr;
			ps.get(get_heaptids()[nentry - 1u], [&](const void *in_data, Size size) {
				ItemPointer p = (ItemPointer)in_data;
				nptr = size / sizeof(ItemPointerData);
				if (nptr > 0) {
					ps.get(p[nptr - 1u], get_new_data);
				} else {
					new_data = NULL;
				}
			});
			if (!new_data) {
				const Size s = sizeof(uint8) * pq.code_size + sizeof(ItemPointerData);
				new_data = palloc(s);
				pq.compute_code(x, (uint8 *)new_data);
				*(ItemPointer)((char *)new_data + sizeof(uint8) * pq.code_size) = tid;
				auto k = ps.put(new_data, s);
				if (nptr < tid_page_cap) {
					++nptr;
					const Size ptr_size = sizeof(ItemPointerData) * nptr;
					ItemPointer ptr = (ItemPointer)palloc(ptr_size);
					ps.get(get_heaptids()[nentry - 1u], [&](const void *in_data, Size size) {
						memcpy(ptr, in_data, ptr_size);
					});
					ptr[nptr - 1u] = k;
					k = ps.put(ptr, ptr_size);
					overwriten = !ItemPointerEqualsNoCheck(&k, get_heaptids() + nentry - 1u);
					if (overwriten) {
						get_heaptids()[nentry - 1u] = k;
					}
				} else if (nentry >= HNSW_HEAPTIDS) {
					ps.destroy();
					return false;
				} else {
					get_heaptids()[nentry] = ps.put(&k, sizeof(ItemPointerData));
					set_ntids(nentry + 1u);
					overwriten = true;
				}
			} else {
				pq.compute_code(x, new_pq_code);
				ps.set(get_heaptids()[nentry - 1u], [&](void *in_data, Size size) -> bool {
					ItemPointer p = (ItemPointer)in_data;
					uint32 n = size / sizeof(ItemPointerData);
					auto k = ps.set(p[n - 1u], new_data, new_size);
					bool writen = !ItemPointerEqualsNoCheck(&k, &p[n - 1u]);
					if (writen) {
						p[n - 1u] = k;
					}
					return writen;
				});
				overwriten = false;
			}
		}
		ps.destroy();
		return true;
	}
	uint32 get_tids(Vector<ItemPointerData> &tids, disk_container::PlainStore &ps) const
	{
		uint32 res;
		if (!is_extended()) {
			res = ntids();
			tids.push_back(get_heaptids(), get_heaptids() + res);
			return res;
		}
		uint8 nkey = ntids();
		res = 0;
		for (uint8 i = 0; i < nkey; ++i) {
			ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
				const ItemPointer data = (const ItemPointer)in_data;
				uint32 ndata = size / sizeof(ItemPointerData);
				tids.push_back(data, data + ndata);
				res += ndata;
			});
		}
		return res;
	}
	bool empty() const { return ntids() == 0; }
	void get_tid_dists(Relation heap, Relation index, Vector<float> &dists,
					   Vector<ItemPointerData> &tids, float *query, Vector<float*> &vec_holder,
					   uint32 dim, disk_container::PlainStore &ps,
					   ann_helper::distance_func_batch2 batch2_func) const
	{
		const size_t s = dists.size();
		Assert(s == tids.size());
		uint32 n = get_tids(tids, ps);
		expand_vec_buf(vec_holder, dim, n);
		dists.resize(s + n);
		n = hnsw_get_vector(vec_holder.data(), heap, index, tids.at(s), n, dim);
		batch2_func(query, (void *const *)vec_holder.data(), dim, n, dists.at(s));
	}
	void get_tid_dists_pq(Relation index, Vector<float> &dists, Vector<ItemPointerData> &tids,
						  ProductQuantizer &pq, const float *dist_table,
						  disk_container::PlainStore &ps) const
	{
		const uint8 ntid = ntids();
		const uint32 pq_len = pq.code_size;
		const auto iter = [&](const void *in_data, Size size) {
			TidPQHandler handler = {pq_len, size, (char *)in_data};
			uint32 ndata = handler.ndata();
			uint32 j = 0;
			const uint8 *codes = handler.get_pq();
			const size_t old_ds = dists.size();
			Assert(old_ds == tids.size());
			dists.resize(old_ds + ndata);
			float *cur_dist = dists.at(old_ds);
			for (; j + 4u < ndata; j += 4u, codes += 4u * pq_len, cur_dist += 4) {
				pq.distance_to_four_code(dist_table, codes, codes + pq_len, codes + pq_len * 2u,
					codes + pq_len * 3u, cur_dist[0], cur_dist[1], cur_dist[2], cur_dist[3]);
			}
			for (; j < ndata; ++j, codes += pq_len, ++cur_dist) {
				*cur_dist = pq.distance_to_code(codes, dist_table);
			}
			const ItemPointer tid = handler.get_tid();
			tids.push_back(tid, tid + ndata);
		};
		if (!is_double_extended()) {
			for (uint8 i = 0; i < ntid; ++i) {
				ps.get(get_heaptids()[i], iter);
			}
		} else {
			for (uint8 i = 0; i < ntid; ++i) {
				ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
					uint32 n = size / sizeof(ItemPointerData);
					for (uint32 i = 0; i < n; ++i) {
						ps.get(((ItemPointer)in_data)[i], iter);
					}
				});
			}
		}
	}
	void transform_to_pq(Relation heap, Relation index, ProductQuantizer &pq,
		Vector<ItemPointerData> &tids, Vector<float*> &vec_holder, disk_container::PlainStore &ps)
	{
		tids.clear();
		uint32 n = get_tids(tids, ps);
		uint8 ntid = ntids();
		expand_vec_buf(vec_holder, pq.d, n);
		n = hnsw_get_vector(vec_holder.data(), heap, index, tids.begin(), n, pq.d);
		if (is_extended()) {
			for (uint8 i = 0; i < ntid; ++i) {
				ps.erase(get_heaptids()[i]);
			}
		} else {
			set_extended();
		}
		const size_t total_size_used = n * (sizeof(uint8) * pq.code_size + sizeof(ItemPointerData));
		constexpr double full_threshold = 0.92;
		const size_t page_cap = TidPQHandler::fullsize(pq.code_size);
		if (page_cap * tid_page_cap * HNSW_HEAPTIDS < n) {
			ereport(ERROR, (errcode(ERRCODE_INVALID_STATUS),
				errmsg("Found irregular bin with excessive size %u", n),
				errhint("Please create vector index with clustering feature disabled "
						"by setting num_cluster = 0")));
		}
		char *buf = (char *)palloc(PlainStore::max_size);
		float **vecs = vec_holder.data();
		ItemPointer cur_tid = tids.data(); 
		uint8 cur_pos = 0;
		const auto load_data = [&]() -> PlainStore::key {
			uint32 l = std::min<uint32>(n, page_cap);
			Size s = l * (sizeof(uint8) * pq.code_size + sizeof(ItemPointerData));
			uint8 *cur = (uint8 *)buf;
			for (uint32 j = 0; j < l; ++j) {
				pq.compute_code(vecs[j], cur);
				cur += pq.code_size;
			}
			memcpy(cur, cur_tid, sizeof(ItemPointerData) * l);
			cur_tid += l;
			vecs += l;
			n -= l;
			return ps.put(buf, s);
		};

		if (total_size_used > PlainStore::max_size * HNSW_HEAPTIDS * full_threshold) {
			set_double_extended();
			Vector<ItemPointerData> holder;
			while (n > 0) {
				for (uint32 i = 0; i < tid_page_cap && n > 0; ++i) {
					holder.push_back(load_data());
				}
				get_heaptids()[cur_pos] = ps.put(holder.data(), holder.size() * sizeof(ItemPointerData));
				++cur_pos;
				holder.clear();
			}
			ann_helper::optional_destroy(holder);
		} else {
			Assert(!is_double_extended());
			while (n > 0) {
				get_heaptids()[cur_pos] = load_data();
				++cur_pos;
			}
		}
		set_ntids(cur_pos);
		pfree(buf);
	}
	void init_pq(Relation index, bool is_building, const float *x, const ItemPointerData &tid,
		ProductQuantizer &pq)
	{
		set_extended();
		set_ntids(1u);
		const Size s = sizeof(uint8) * pq.code_size + sizeof(ItemPointerData);
		void *data = palloc(s);
		pq.compute_code(x, (uint8 *)data);
		*(ItemPointer)((char *)data + sizeof(uint8) * pq.code_size) = tid;
		PlainStore ps(index, HNSW_PS_BLKNO, !is_building);
		auto k = ps.put(data, s);
		ps.destroy();
		get_heaptids()[0] = k;
	}
	uint32 actual_ntids(Relation index, disk_container::PlainStore &ps) const
	{
		const uint8 nkey = ntids();
		if (!is_extended()) {
			return nkey;
		}
		uint32 res = 0;
		for (uint8 i = 0; i < nkey; ++i) {
			ps.get(get_heaptids()[i], [&](const void *, Size size) {
				res += size / sizeof(ItemPointerData);
			});
		}
		return res;
	}
	uint32 actual_ntids_pq(Relation index, uint32 code_size, disk_container::PlainStore &ps,
						   size_t &used_size) const
	{
		const uint8 nkey = ntids();
		const size_t item_size = sizeof(uint8) * code_size + sizeof(ItemPointerData);
		uint32 res = 0;
		used_size = 0;
		const auto iter = [&](const void *, Size s) {
			res += s / item_size;
			used_size += s;
		};
		if (is_double_extended()) {
			for (uint32 i = 0; i < nkey; ++i) {
				ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
					used_size += size;
					ItemPointer k = (ItemPointer)in_data;
					uint32 nptr = size / sizeof(ItemPointerData);
					for (uint32 j = 0; j < nptr; ++j) {
						ps.get(k[j], iter);
					}
				});
			}
		} else {
			for (uint32 i = 0; i < nkey; ++i) {
				ps.get(get_heaptids()[i], iter);
			}
		}
		return res;
	}
	uint32 vacuum_tids(IndexBulkDeleteCallback callback, void *callback_state, Relation index,
					   disk_container::PlainStore &ps, bool &dirty)
	{
		dirty = false;
		const uint8 ntid = ntids();
		if (ntid == 0) {
			return 0;
		}

		if (!is_extended()) {
			uint8 start_idx = 0;
			for (uint8 i = 0; i < ntid; ++i) {
				if (callback(get_heaptids() + i, callback_state, InvalidOid, InvalidBktId)) {
					continue;
				}
				get_heaptids()[start_idx] = get_heaptids()[i];
				++start_idx;
			}
			dirty = start_idx != ntid;
			set_ntids(start_idx);
			return ntid - start_idx;
		}

		ItemPointer buf = (ItemPointer)palloc(sizeof(ItemPointerData) * tid_page_cap);
		uint32 nremoved;
		uint32 nremain;
		ps.set(get_heaptids()[0], [&](const void *in_data, Size size) -> bool {
			ItemPointer data = (ItemPointer)in_data;
			const uint32 ndata = size / sizeof(ItemPointerData);
			uint32 start_idx = 0;
			for (uint32 i = 0; i < ndata; ++i) {
				if (callback(data + i, callback_state, InvalidOid, InvalidBktId)) {
					continue;
				}
				data[start_idx] = data[i];
				++start_idx;
			}
			nremain = start_idx;
			nremoved = ndata - start_idx;
			memcpy(buf, in_data, start_idx * sizeof(ItemPointerData));
			return nremoved > 0;
		});
		if (ntid == 1u) {
			if (nremain == 0) {
				set_ntids(0);
				dirty = true;
			}
			pfree(buf);
			return nremoved;
		}

		uint8 start_idx = 0;
		bool updated = false;
		for (uint8 i = 1u; i < ntid; ++i) {
			ps.set(get_heaptids()[i], [&](const void *in_data, Size size) -> bool {
				uint8 internal_start_idx = 0;
				uint32 ndata = size / sizeof(ItemPointerData);
				uint32 old_nremain = nremain;
				ItemPointer data = (ItemPointer)in_data;
				for (uint32 i = 0; i < ndata; ++i) {
					if (callback(data + i, callback_state, InvalidOid, InvalidBktId)) {
						++nremoved;
						continue;
					}
					if (nremain < tid_page_cap) {
						buf[nremain] = data[i];
						++nremain;
					} else {
						data[internal_start_idx] = data[i];
						++internal_start_idx;
					}
				}
				
				if (nremain >= tid_page_cap) {
					nremain = internal_start_idx;
					auto key = ps.set(get_heaptids()[start_idx], buf,
									  tid_page_cap * sizeof(ItemPointerData));
					if (key != get_heaptids()[start_idx]) {
						dirty = true;
						get_heaptids()[start_idx] = key;
					}
					++start_idx;
					memcpy(buf, data, nremain * sizeof(ItemPointerData));
					updated = false;
				} else if (old_nremain != nremain) {
					updated = true;
				}
				return internal_start_idx != ndata;
			});
			if (i != start_idx) {
				get_heaptids()[start_idx] = get_heaptids()[i];
				dirty = true;
			}
		}
		if (start_idx != ntid) {
			if (updated) {
				get_heaptids()[start_idx] =
					ps.set(get_heaptids()[start_idx], buf, nremain * sizeof(ItemPointerData));
			}
			set_ntids(start_idx);
			dirty = true;
		}
		pfree(buf);
		return nremoved;
	}
	uint32 vacuum_tids_pq(IndexBulkDeleteCallback callback, void *callback_state, Relation index,
					      disk_container::PlainStore &ps, uint32 code_size, bool &dirty)
	{
		const uint8 ntid = ntids();
		if (ntid == 0) {
			dirty = false;
			return 0;
		}
		dirty = true;
		uint32 res = 0;
		Vector<ItemPointerData> tids;
		Vector<uint8> codes;
		const auto gather = [&](const void *in_data, Size size) {
			TidPQHandler handler = {code_size, size, (char *)in_data};
			uint32 n = handler.ndata();
			ItemPointer ptr = handler.get_tid();
			for (uint32 i = 0; i < n; ++i) {
				if (callback(ptr + i, callback_state, InvalidOid, InvalidBktId)) {
					++res;
					continue;
				}
				tids.push_back(ptr[i]);
				uint8 *pq_pos = handler.get_pq() + i * code_size;
				codes.push_back(pq_pos, pq_pos + code_size);
			}
		};
		for (uint8 i = 0; i < ntid; ++i) {
			if (is_double_extended()) {
				ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
					ItemPointer ptr = (ItemPointer)in_data;
					uint32 n = size / sizeof(ItemPointerData);
					for (uint32 i = 0; i < n; ++i) {
						ps.get(ptr[i], gather);
					}
				});
			} else {
				ps.get(get_heaptids()[i], gather);
			}
		}
		ItemPointerData temp_heaptids[HNSW_HEAPTIDS];
		size_t n = tids.size();
		const size_t total_size_used = n * (sizeof(uint8) * code_size + sizeof(ItemPointerData));
		constexpr double full_threshold = 0.92;
		const size_t page_cap = TidPQHandler::fullsize(code_size);
		char *buf = (char *)palloc(PlainStore::max_size);
		uint8 *vecs = codes.data();
		ItemPointer cur_tid = tids.data(); 
		const auto load_data = [&]() -> PlainStore::key {
			uint32 l = std::min<uint32>(n, page_cap);
			Size s = l * (sizeof(uint8) * code_size + sizeof(ItemPointerData));
			uint8 *cur = (uint8 *)buf;
			memcpy(cur, vecs, sizeof(uint8) * code_size * l);
			memcpy(cur + code_size * l, cur_tid, sizeof(ItemPointerData) * l);
			cur_tid += l;
			vecs += l * code_size;
			n -= l;
			return ps.put(buf, s);
		};

		uint8 cur_pos = 0;
		if (total_size_used > PlainStore::max_size * HNSW_HEAPTIDS * full_threshold) {
			set_double_extended();
			Vector<ItemPointerData> holder;
			while (n > 0) {
				for (uint32 i = 0; i < tid_page_cap && n > 0; ++i) {
					holder.push_back(load_data());
				}
				temp_heaptids[cur_pos] = ps.put(holder.data(), holder.size() * sizeof(ItemPointerData));
				++cur_pos;
				holder.clear();
			}
			ann_helper::optional_destroy(holder);
		} else {
			unset_double_extended();
			while (n > 0) {
				temp_heaptids[cur_pos] = load_data();
				++cur_pos;
			}
		}
		set_ntids(cur_pos);
		for (uint8 i = 0; i < cur_pos; ++i) {
			std::swap(get_heaptids()[i], temp_heaptids[i]);
		}
		pfree(buf);
		ann_helper::optional_destroy(tids);
		ann_helper::optional_destroy(codes);
		for (uint8 i = 0; i < ntid; ++i) {
			if (is_double_extended()) {
				ps.get(temp_heaptids[i], [&](const void *in_data, Size size) {
					ItemPointer ptr = (ItemPointer)in_data;
					uint32 n = size / sizeof(ItemPointerData);
					for (uint32 i = 0; i < n; ++i) {
						ps.erase(ptr[i]);
					}
				});
			}
			ps.erase(temp_heaptids[i]);
		}
		return res;
	}
};

#endif /* HNSW_CLUSTER_H */
