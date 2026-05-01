# graoh_index + PQ
## 目的
1. 减少存储（不存储原始向量）
2. 降低搜索时内存
3. 加快QPS（实际上不一定能加速，见下文）

## 使用范围
graph_index/hnsw索引

## 使用方法
- quantizer: 启用何种量化方法，NONE|pq|rabitq，默认值NONE
```sql
create index on sift1m using graph_index (repr) WITH (m=5, ef_construction=10, quantizer=pq, parallel_workers=10);
```
注：当表中数据初始数据量小于15000条时，无法计算PQ，退化为原始graph_index索引。即使后续插入了足够数量的数据，也不会自动计算；此处待补充完善实现。

## 版本升级
增加了metablock的存储信息quantizer_metainfo，但存储于尾部，可兼容旧版本。若后续再添加其他存储信息，可能需要考虑升级问题。

## 具体实现
常规的PQ实现。需要注意的有:
1. 内存建图的过程中仍使用原始向量
1. 存储：仅存储了PQ，没有存储原向量。所以**磁盘建图/搜索/插入**，均使用PQ进行距离计算，需要原向量的时候回堆表取。
2. QPS：使用Vector Buffer的时候，PQ相比于使用原始向量不一定会有提升，甚至可能下降。原因是当前瓶颈并不在距离计算上，相关的优化见下文的BulkBuffer。

# BulkBuffer
## VectorBuffer瓶颈
VectorBuffer将向量\PQ码读取到内存缓冲池后，需要定位到该向量在缓冲池中的具体位置。当前定位方法为使用boost::unordered::concurrent_flat_map进行find查找,key为<relation, idx>（idx代表某个向量\PQ码是该releation中的第几条向量\PQ码）。

在系统稳定运行后，瓶颈为get_buffer()函数，该函数在稳定运行状态下可以认为仅执行了concurrent_flat_map的find查找函数。使用sift1m数据集，测试hnsw、 hnswpq索引，该函数占比均为40% ~ 45%。

原因分析：VectorBuffer中的向量数量是百万级的，所以map中元素个数也是百万级的，map自身无法被加载在cpu cache中。故find操作本身也需要访存，造成了瓶颈。

## 解决方案
如果能够将一个索引中的向量/PQ码/RABITQ码，全部读取到内存中的一个了**大块的逻辑上连续内存**，假设该**逻辑连续内存**的起始地址为$StartAddress$, 那么定位一个向量的方式可被简化为：
$StartAddress + idx * dim$。该计算复杂度O(1)，远快于map的find。

简单实验证明，该方案确实能够加速30%~40%左右，相当于完全省去了map的find操作。

该大块连续内存即命名为BulkBuffer，相当于一个**纯内存**的缓存，类似于其他的内存向量数据库。

## 与PQ、RABITQ结合使用
BulkBuffer可以结合原始向量、PQ、RABITQ使用。但如果一个索引存储的是原始向量，将全部向量加载进入内存则内存消耗过大，使用起来存在限制。而PQ、RABITQ的体积较小，全部加载进入内存中的占用较小，可以接受。

## 维护策略
如果已经选择将某索引加载进入内存，则需要保证它的状态总是与磁盘中完全对应。所以，若加载之后对索引进行插入/更新，则需要同时**写磁盘且更新bulkbuffer中的内容**。

## 使用方法
由于BulkBuffer读取时需要读取一个索引的全部向量/PQ，IO延时极大，故设计为admin级别的SQL函数，需要使用时手动触发。增加了以下系统函数：
1. index_memory_load
    - 含义：把一个索引中的全部向量/PQ加载进入内存中。如果是分区表索引，若第二个参数为空，则加载所有分区索引；可以通过索引分区名（类型text）或索引分区OID作为第二个参数，仅加载该索引
    - 参数：与index_inspect完全相同，略
    - 返回值：如果内存足够，所有索引都顺利加载，返回true。如果内存不够则报错
2. index_memory_release
    - 含义：把一个已经加载进入内存的索引从内存中释放掉。如果是分区表索引，若第二个参数为空，则释放所有分区索引；可以通过索引分区名（类型text）或索引分区OID作为第二个参数，仅释放该索引
    - 参数：与index_inspect完全相同，略
    - 返回值：如果传入索引未被加载，返回false；对于分区表索引，如果所有分区索引均未被加载，返回false。其他情况返回true。

```sql
-- 加载进入内存
select index_memory_load('hnswpq1');

-- 执行检索
-- select /*+ indexscan(sift1m hnswpq1) */ _id, $1 <-> repr as dist from hnswpq1 order by dist limit 10;

-- 从内存中释放
select index_memory_release('hnswpq1');
``` 

## 使用建议
适用场景：某个索引作为热点被高频使用，且内存充足，将其全部加载进来也不会影响其他业务。

不适用场景：多个索引均在被使用、没有足够内存，边搜边插（原因见`维护策略`）