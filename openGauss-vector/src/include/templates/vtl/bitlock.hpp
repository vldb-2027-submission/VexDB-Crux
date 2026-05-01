/**
 * Copyright ...
 */

#ifndef CONTAINER_BITLOCK_H
#define CONTAINER_BITLOCK_H

#include <atomic>
#include <vtl/vector>

#include "miscadmin.h"
struct DummyLocker {
    DummyLocker(size_t size = 0) {}
    ~DummyLocker() = default;
    static void lock(size_t idx) {}
    static void unlock(size_t idx) {}
    static void resize(size_t size) {}
};

class BitLock {
public:
    BitLock(size_t size = 0) : _locks((size + 63ul) / 64ul) { resize(size); }
    ~BitLock() = default;
    void lock(size_t idx)
    {
        size_t word_idx = idx / 64ul;
        size_t bit_idx = idx % 64ul;
        size_t mask = 1ul << bit_idx;
        while (!(_locks[word_idx].fetch_or(mask) & mask)) {
            tick();
        }
    }
    bool trylock(size_t idx)
    {
        size_t word_idx = idx / 64ul;
        size_t bit_idx = idx % 64ul;
        size_t mask = 1ul << bit_idx;
        return !(_locks[word_idx].fetch_or(mask) & mask);
    }
    void unlock(size_t idx)
    {
        size_t word_idx = idx / 64ul;
        size_t bit_idx = idx % 64ul;
        size_t mask = ~(1ul << bit_idx);
        _locks[word_idx].fetch_and(mask);
    }
    void resize(size_t size) { _locks.resize((size + 63ul) / 64ul); }
    void destroy() { _locks.destroy(); }

    bool locked(size_t idx)
    {
        size_t word_idx = idx / 64ul;
        size_t bit_idx = idx % 64ul;
        size_t mask = 1ul << bit_idx;
        return _locks[word_idx].load() & mask;
    }
    void wait(size_t idx)
    {
        size_t word_idx = idx / 64ul;
        size_t bit_idx = idx % 64ul;
        size_t mask = 1ul << bit_idx;
        while (_locks[word_idx].load() & mask) {
            tick();
        }
    }
private:
    Vector<std::atomic<size_t>> _locks;

    void tick() { CHECK_FOR_INTERRUPTS(); pg_usleep(1); }
};

class RWBitLock {
public:
    RWBitLock(size_t size = 0) : _rlocks(size), _wlock(size) {}
    ~RWBitLock() = default;
    void destroy() { _rlocks.destroy(); _wlock.destroy(); }

    void rlock(size_t idx)
    {
        _wlock.wait(idx);
        _rlocks[idx].fetch_add(1);
        _wlock.wait(idx);
    }
    void wlock(size_t idx)
    {
        _wlock.lock(idx);
        while (_rlocks[idx].load()) {
            tick();
        }
    }
    void runlock(size_t idx) { _rlocks[idx].fetch_sub(1); }
    void wunlock(size_t idx) { _wlock.unlock(idx); }
private:
    Vector<std::atomic<uint16>> _rlocks;
    BitLock _wlock;

    void tick() { CHECK_FOR_INTERRUPTS(); pg_usleep(1); }
};
#endif /* CONTAINER_BITLOCK_H */
