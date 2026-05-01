# Vector Cache Documentation
All header code is listed in `smgr.h` file and implementation code is in `smgr.cpp` file.

## Part 1: Storage
### Interface
 - `vec_read` and `read_vector`: load a vector as floats into buffer, given its offset location (with a format of size_t).
 - `vec_write` and `write_vector`: write a vector as floats to disk at its offset position.
 - `write/read_vector` is a wrapper of `vec_write/read` with specific use on floats, where the latter treat everything as chars.

### Limitation
 - It's better to keep the dimention always the same within one relation, since the location is calculated by `dim * loc`. However, it's completely doable to store stuff with different length with some adaption on the use and interface.
 - The basic assumption relies on the use case where there is hardly no overwrite on the stored content. It it a plain storage which has no concurrency or transaction handling.
 - It's allowed to write at an extended position. For instance, if there are 10 elements stored, writing to position 12 is allowed and the content in position 11 is undefined until writen. (I'm not even sure whether we can read it.)

### Implemetation
 - All data that is stored through the interface goes to `VECTOR_FORKNUM`, you can find corresponding files ending with "_vec" in the data directory.
 - Each relation store is splitted into 1GB file blocks, exactly like how main fork store acts. It's OK to write across two or more blocks with one interface call, the cross write logic is handled inside.
 - The inside implementation still relies on PG storage handling, we just added a new fork number.

#### TODO
 - WAL

## Part 2: Cache
### Interface
 - `vec_read_buffer`: read out a buffer given relation, position, and dimension. Please refor to `VecBuffer` for the format of returned structure.
 - The basic use routine: call `vec_read_buffer` to get the buffer, use `get_vector` to get the contents in the buffer, and after done reading the vector, call `release` to free the pin on the buffer.
 - Write to the buffer is undefined, just call `vec_write` or `write_vector` to write to disk directly.
 - `vec_invalidate_buffer_cache`: invalidate vector buffer as the contents are updated.
 - GUC `vector_buffers` and `vector_buffer_thread_num` controls the max size of buffer and the number of backend buffer management threads. Currently they are POSTMASTER level (set on startup), but they can be changed to SIGHUP (changed at runtime) if needed.
 - Set `RECORD_HIT_RATIO` to true and call `vec_buffer_report_stats` somewhere to report the stats of vector buffers for performance analysis.

### Limitation
 - Again, no write to the buffer.
 - Still under testing and development.

### Implementation
 - Design Layout
   - TBA
 - Frontend (consume cache)
   - TBA
 - Backend (evict and distribute cache)
   - TBA

### TODO
 - Resource management handler to prevent buffer leaks. (Done)
 - Dynamic cache allocation across dimensions. (Done, there may still be a bug about freelist leak)

## Part 3: Performance
vector cache performance increased by about 1% than result below after commit f45b412172cf01a691864e90c254d9b2b8147380.
##### SIFT1M
use shared buffers:
 - full - 5.3ms
 - 620MB - 7ms ~ 93% hit rate
 - 500MB - 8.6ms ~ 86% hit rate
 - 400MB - 11.1ms ~ 76% hit rate

use vector buffers:
 - full - 3.2ms
 - 470MB - 4.0ms ~ 91% hit rate
 - 400MB - 4.8ms ~ 82% hit rate
 - 360MB - 4.8ms ~ 76% hit rate
##### GIST1M
use shared buffers:
 - full - 17.0ms
 - 2GB - 27.4ms ~ 78% hit rate
 - 1GB - 36.0ms ~ 59% hit rate
 - 500MB - 41.6ms ~ 47% hit rate

use vector buffers:
 - full - 8.4ms
 - 2GB - 11.5ms ~ 86% hit rate
 - 1GB - 14.2ms ~ 67% hit rate
 - 500MB - 15.9ms ~ 53% hit rate

##### GIST1M + SIFT1M
use vector buffers:
 - 1GB - 