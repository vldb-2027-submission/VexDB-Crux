# Inverse Filtered Vector Index Documentation

## Part 1: B+ Tree
Basic implementation code is in the current folder and the usage is in diskann index implementaion under the logical flow where `DiskAnnUseBTree` is true.

### Interface

### Limitation

### Implementation

### TODO

## Part 2: Vector Index
The interface design and implementation is in `src/include/access/diskann/vector_bt.h` file.

### Interface
 - A vector index should implement the following interface:
 - `void insert(size_t idx, ItemPointerData tid)`: insert a vector, tid pair with vector format is a offset.
 - `void vacuum()`: do vacuum, the input parameter is to be determined.
 - `size_t search(float *dist_out, ItemPointerData *iptr, Datum float_vec, TupleDesc tdesc, void *param)`: search top-k nearest vectors to the given float_vec, put their distance and tid to input address, and return the actual searched size.
 - `bool idle()`: whether the index is under a split or merge operation.
 - `bool prepare_merge()`: start a merge operation, return false if failed.
 - `BlockNumber prepare_split()`: start a split operation and create a new index, return InvalidBlockNumber if failed.
 - `void finish_operation()`: set idle flags to accept future operations.
 - `void insert_node_blkno(BlockNumber blkno)`: connect a btree node to the index.
 - `void remove_node_blkno(BlockNumber blkno)`: remove a btree node from the index.
 - `BlockNumber *get_node_blkno(size_t &nblkno)`: get all connected btree nodes in the index, nblkno is the number of nodes.
 - `void split_to(VectorIndex &other, void *right_map)`: split the index, half of the vectors are inserted into the other index which has the same type, which vectors are splitted is determined by `right_map` whose format is to be determined. The function is expected to run in the backend.
 - `void merge(Index &other)`: merge all itself to other index which has the same type. The function is expected to run in the backend.
 - `BlockNumber leftmost_node()`: leftmost corresponding node blkno.
 - `BlockNumber next_index()`: next index blkno.

### Implementation
 - All vector index should only store the offset of a vector which is represented by a size_t number. The offset is assigned at insert or build, and recollected after vacuum. Refer to `src/include/access/diskann/storage_interface/vector_smgr.md` for the actual storage and use of the vector.

## Part 2.1: IVF Index
### Implementation
 - Split
   - TBA
 - Merge: not implemented

### TODO
 - TBA

## Part 2.2: DiskANN Index
### Implementation
 - Split
   - TBA
 - Merge: not implemented

### TODO
 - TBA

## Part 3: Integration
The interface is listed in `src/include/access/diskann/vector_bt.h` file now, and the implementation will also be in the `src/include/access/diskann/container/bplustree` folder.

### Implementation
 - B+树中每个非叶子节点会指向一个或者多个向量索引，索引数量级别分为2w, 10w, 50w, 250w, 1250w。一个一层节点最多承载8w条数据，正常使用情况下最少承载1k条；二层节点最多承载2kw数据，最少承载4w。三层节点最少承载150w。在最高容量场景下，二层节点对应1250w量级索引，而每个一层节点会对应10w到250w之间三个量级的索引；最低容量场景下，一层节点对应2w量级索引，二层节点对应10w和50w量级索引，三层节点对应250w和1250w量级索引。
 - 每个向量索引会对应多个非叶子节点，所有节点层高相同。其实际数据量一般不会超过对应数据量级的两倍。
 - 插入操作：将输入向量插入到插入路径中每一个非叶子节点对应的向量索引中。如果被插入的向量索引里的数据量超过了预计最大值，则会向后端发送该索引的分裂任务。
 - On merge, TBA
 - 检索操作：执行一或多次路径探索划出检索条件所覆盖的节点。随后在该覆盖上选择一组数量最小且与覆盖节点的重合率大于80\%的向量索引集合(此处叶节点也被看作为一个向量索引，但只需要实现`search`接口)。最后对集合中的每个索引进行检索并归集结果。
 - 后端线程：实现launcher和worker线程，launcher根据任务池内容和worker线程数量控制拉起线程并分配任务，worker线程执行任务。worker线程最大数量由GUC参数max_vector_indexer_worker_threads(范围2-64)设置。前端发起任务只需要调用接口`add_vector_index_task`将任务放入池中即可，任务状态可以通过`vector_indexer_task_status`查询，所有worker挂载数据存放于`IndexerThreadStatus`结构下，可以在其中增删成员以实现更多功能。理想使用将任务划分成多段，在每段任务执行结束后由worker根据当前执行状态发起下一段任务。

### Algorithm
 - Insert
   - 向量插入在B+树插入完成后进行。我们以向量检索作为最终返回结果，
   B+树插入中报错或者打断向量索引本身不受影响。
   - 每个非叶子树节点指向最多新旧两个索引，旧索引为当前有叶子节点全部数据的索引，
   新索引为正在与旧索引发生分裂/合并操作的索引。目前设定为只往新索引插入数据，
   查询时同时检索新旧索引，以此简化操作的复杂性。
   - B+树插入完成后会有探寻路径遗留，对路径上每个非叶子节点的索引由下往上进行向量插入。
 - Split
   - 索引分裂，该操作在后端进行。
    1. 等待索引`idle`状态为true。
    2. 执行索引`prepare_split`生成新索引，如果失败则返回步骤一。
    3. 调用`get_node_blkno`接口拿到该索引全部对应节点，分配新索引到部分节点上，
    分配行为使用读锁访问节点。 整个过程写锁旧索引，保证索引数据一致。
    4. 生成right_map，调用`split_to`开始分裂操作。该操作过程中需保持正常读写业务正常。
    5. 调用新索引的`get_node_blkno`设置对应树节点指向的新索引落成为旧索引，
    随后调用索引接口`finish_operation`。
   - 节点分裂
     - 新节点新旧索引同步旧节点，并对每个最新索引调用`insert_node_blkno`接口。
     - 分裂完成后对每个在节点上挂载的索引发送可能的索引分裂任务。
   - 根节点分裂
     - 进行节点分裂操作后，根节点分裂在插入父节点过程中会产生新的根节点，该过程中三个节点全部被写锁，此时标记新根节点的旧索引为原来索引，创建新索引向后端发送根节点索引构建任务。新建索引的预期容量计算会在后面提到。
     - 该任务必须立即执行，如果后端任务线程无可用资源且无法中断则优先尝试用BGwoker替代，若BGWoker也不可用则无视上限在插入操作中拉起为该任务专门的后端线程，如果直接拉起线程依然失败则将任务插队放入。
     - 产生n级根节点时如果n-1级节点索引仍然没有构建完成，则标记该根节点无效，对一切插入操作报错。
     - 在新根节点高度等于1的特殊情况下，旧索引指向InvalidBlockNumber。
     - (后端执行)遍历叶节点插入向量到新索引中，同时维护一个或者多个遍历范围。
     - 新根节点接受插入时根据插入标量判断标量是否在插入范围内，如果不在范围内则不需要进行插入；注意处于范围边界(等于)仍然需要进行向量插入，因为插入位置永远在边界左边。
 - Search
   - 单范围查询
     - 由上而下找到各层范围的左右终点。
     - 由左右终节点找到向量索引起始点，遍历起始点找到当前层级下的所有相关索引。
     - 确定左右端索引，使得所有索引至少有80\%对应节点被范围覆盖。若左右端索引进行了左右移动，则向下继续根据移动后的剩余范围进行索引划分。向下$m$层后最多会有$2^m$个范围。
     - 剩余范围的另一端需要通过遍历当前层节点到找到下一个索引对应节点为止。
     - 找到叶子层后返回所有剩余范围对应叶节点和选中索引。
   - 多范围查询
     - 多范围查询包括多列范围查询(a > 7 and b > 7)或者IN查询。
     - IN条件查询：对IN范围中的所有内容单独处理，每条内容处理为等于条件进行范围生成。特殊场景如a < any(1,2,3)则处理为a < 1后再进行单范围查询，目前所有支持范围内非等号数组操作(<,<=,>,>=)均支持以上归并行为。最后对所有的包括索引集取并集，该处理方式完全兼容非向量查询，但无法将IN(1,2,3,4,5,6)处理成BETWEEN 1 AND 6 的形式，使得返回结果多为低级索引或者叶节点，无法合并为主要的高级索引。 
     - 多范围查询主要难点在于范围只有在叶子层节点才能准确显现，上层节点无法准确表示自身是否囊括符合多列条件的数据，比如对于条件(a > 7 and b > 7)，上层节点可以包含(9,2),(9,7),(10,2),(11,2)，如果不对数据分布做任何假设，只能知道最多不超过$3/5$的数据可能符合条件(非整形条件的通用情况下上限为$4/5$且需要复杂的计算规则)，但不能估计其子节点包含符合条件的数据比例。所以任何从上到下的遍历方法必须要在最下层做出具体决定，且无法对上层数据进行有效可信的剪枝策略。以下文档中的公式可以通过VScode中`Markdown All in One`扩展打开本文档进行阅读。
     - 即使在范围个数较少的情况，不建议将多列范围查询退化成IN条件查询，转化后无法利用其原对应的连续范围关系找到最优的索引对应。但是，对于IN查询个数过多导致可能只返回叶子节点的情况，我们可以将IN查询转化为多范围查询。
     - 对于通用多范围情况我们只用叶节点进行算法建模。将范围本身视作数据域，表示为符合查询条件的所有叶节点，此处将其由数字代表，则有$\mathcal{U} = \set{1,2,\dots,n}$，其向量索引则可以表示为$\mathcal{S} = \set{s \subseteq \mathcal{U}}$，表示对应中间节点覆盖的范围，该范围计算会在后面讨论。问题目标是找到最优索引集$C\subseteq S$覆盖所有范围且总共查询代价最低：
     $$C = \argmin_{\bigcup_{s\in C} s = \mathcal{U}} w(C)$$
     另外定义函数
     $$overlap : S \to [0,1] ,s.t.\; overlap(s) = \frac{|s \;\cap\; \mathcal{U}|}{\mathcal{N}(s)}$$
     为索引的重合率，我们只考虑重合率大于0.8的索引。叶子节点在查询时也可以视作向量索引。
     - 我们不考虑集合排除操作，比如不考虑重合率下的索引：$S'=\set{\set{1},\set{2},\set{3},\set{4},\set{5},\set{1,2,3,4,5}}$，对范围 $\mathcal{U}=\set{1,2,3,4}$还可以用$C = \set{1,2,3,4,5} - \set{5}$表示，实现上则为对两个索引进行查询后在前者结果中剔除后者结果，但由于不能保证两索引查询结果互相独立，该实现不具有很高的鲁棒性。
     - 直接解法可以使用线性编程，求最优解：
     $$
     \begin{align}
     \text{minimize } & \sum_{s\in S} w_s x_s, & \text{(weighted index cost)}\nonumber\\
     \text{subject to } & \bigl( \sum_{s:e\in s} x_s \bigr) \ge 1 \;\forall\, e \in \mathcal{U}, & \text{(cover every node)}\nonumber\\
     & x_s \in \set{0, 1} \;\forall\, s \in S, & \text{(either in the set cover or not)}\nonumber
     \end{align}
     $$
     范围只涉及包含数据超过90\%符合条件的叶节点，若不符合条件则直接进行暴力搜索。注意由于最基本的向量索引单位是一层中间节点，我们可以将这里的范围表示$\mathcal{U}$和索引范围表示$S$简化为一层中间节点替代，对不完全被覆盖的叶节点部分，若其父节点覆盖率超过90\%则直接替换($90\% * 90\% > 80\%$)，反之则直接全部暴力搜索该部分节点，不涉及算法中的节点选择。实现上我们使用最简单的解法复杂度$O(|\mathcal{U}|^3)$，由于我们从一层节点开始表示范围，对于低选择率的查询$|\mathcal{U}|$通常不会很大。
     - 上述解法没有利用每层节点最多对应一个索引的限制，即每层的索引范围不互相接含。比如若索引范围集$S$只包含一层节点索引，那么必然 $\bigcup_{s\in S} s = \mathcal{U}$且$s_{1} \cap s_{2} = \empty\;\forall\, s_{1},s_{2}\in S$，此时我们可以直接得到$C = S$。
     - 根据该特点，我们从每层索引看待该问题，则有贪心算法
     $$
     f(S,\mathcal{U}) = \begin{cases}
      \empty & S = \empty \\
      \underset{\text{leftmost}\;s\;\text{in each layer}}{\argmin} w(s) + W \circ f(greedy(S,s), \mathcal{U}-s) & S \neq \empty
     \end{cases}
     $$
     尝试使用各层当前最靠左的索引，然后使用贪心规则排除部分可选索引递归寻找最优解，即最小总代价的索引集($W$表示总代价计算函数)。最简单的贪心算法：$greedy(S,c) = \set{s | s\in S \;\&\; s \cap c = \empty}$ 收敛迭代次数为层数乘以底层索引数(默认底层索引数量最大)，但明显会使答案不包括索引集间互相包含的可能，对于以下场景：
     $$\mathcal{U} = \set{1,2,3,4},\; S = \set{\set{1},\set{2},\set{3,4},\set{1,2,3}}$$
     上述贪心算法无法找到$C=\set{\set{1,2,3},\set{3,4}}$。
     - 注意到以上例子无法找到最优解的原因是$\set{1,2,3}$和$\set{3,4}$互相排除，如果将这种排除关系设置为单向，即$\set{1,2,3}$不排除$\set{3,4}$，则可以处理任何可能的索引集合。另外对于完全包含的索引依然可以排除，以此更快的进行迭代。则有：
     $$
     greedy(S,c) = \set{s | s\in S \;\text{and}\; s \cap c \neq s \;\text{and}\; (s \cap c = \empty \;\text{or}\; l(s) < l(c))}
     $$
     其中$l(s)$表示索引层高。使用该算法复杂度不变。
     - B树多条件查询：首先根据最小值查询到起始点，然后往右遍历出所有符合条件的数据，根据查询条件如果能够确定不需要继续遍历则提前结束。
     - 索引范围计算：对于单条多范围条件组，我们可以用同样的方法，对起始点路径在树上的不同层节点遍历出覆盖范围，再对范围内的索引进行数据采样计算覆盖率。但对于由极端数组条件场景转化成的多范围查询，如a IN (1,2,$\dots$,999,1000) AND b IN (1,2,$\dots$,999,1000)排列组合下来甚至超过总数据量的情况，只能够遍历叶子节点计算所有包含条件数据的叶节点以及同时遍历包含有效索引的中间节点确定其最下叶节点对应关系。其具体实现可由包含索引的最高层节点开始，由下往上向右遍历。最上层节点因为有索引其数据量上限不超过2千万涉及70万(最多)或者7万(最少)叶节点，计算过程基本不需要考虑内存瓶颈。
 - Merge: not implemented
 - Fault Tolerance
   - TBA (store running new index nlkno to meta, periodically investigate whether these task is going well)
 - Cost Estimation
   - 该混合索引支持纯粹条件查询和在标量条件上的向量查询，前者的代价估计与B+树本身的代价估计实现没有本质区别。而向标混合查询则需要对查询涉及的向量索引数量和级别进行估算。
   - 单范围标量检索的查询规则确定，只需要索引数据总量和条件范围选择率则可以索引级别和数量的上下限，两者皆可以从PG内置接口从系统缓存获取信息，不需要打开索引。
   - 多范围标量检索，首先需要处理IN查询为多组查询(非等于数组条件接可以转化为单组条件)，然后对每组查询进行单独条件选择率计算后将代价相加，若IN内容数量过多则直接使用全部条件选择率使用默认估算。对于每组中的多范围条件，从索引列顺序开始根据每列选择率以及ndistinct进行范围数量估计，对于多范围下的单范围片段取总条件率后取平均值估计单个范围大小。若单范围条件的总选择率大于等于90%，我们则默认该查询可以被合并为一个大索引查询，则直接用单范围代价估计计算，反之，则使用多范围数量乘以平均单范围条件选择率的代价。

### TODO
 - ~~分裂算法步骤3,4操作时树节点分裂/合并造成节点泄露的并行问题。~~ 修改节点时通过扫描而不是反向查找遍历索引，Done
 - ~~索引删除中调用和后端任务删除接口。~~ Done
 - 范围索引重合率估算效率提升，比如在索引信息中记录对应节点high key。
 - ~~查询算法中剩余范围计算加速，需要在索引中记录最左节点。~~ Done
 - 查询算法并行问题排查。

