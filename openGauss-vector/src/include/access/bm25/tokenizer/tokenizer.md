# BM25 Tokenizer Documentation
## Part 0: Tokenizer Introduction
分词器是自然语言处理流程中，预处理需要使用的通用组件，负责将连续文本分割为独立单元（单词、子词或字符），分词器可以基于规则、统计信息或深度学习等多种方法；中文分词词典是分词器的一种具体实现，用于将连续汉字序列切分为独立汉字词组，本系统中使用的jieba分词词典是基于统计信息的中文分词器。
## Part 1: Usage
### 分词词典创建
创建`分词词典`时，需指定模板为vex_jieba分词模板。该分词模板支持中英文混合分词与处理，其中对英文单词的处理使用了内核中的english_stem词典。
```SQL
CREATE TEXT SEARCH DICTIONARY <dict_name> (
    TEMPLATE = vex_jieba
    [, option = value [, ... ]]
);
```

目前创建语句支持以下两个自选参数：
- stopwords：停用词词典，该词典中的词与符号会在分词结果中被**剔除**。
    1. default/不指定：使用系统自带的默认停用词词典。
    2. empty：创建一个空的停用词词典，可以通过SQL语句自行插入停用词。
    3. 一个已存在的`分词词典`名字：创建一个和指定`分词词典`所使用的停用词词典相同的停用词词典（若参照的`分词词典`使用了系统默认停用词词典，则新创建词典也会使用）。

- userdict：用户自定义词典，该词典中的词会在分词结果中被**完整保留**。不能与`delimiter`一起使用。
    1. default/不指定：使用系统自带的默认词典。
    2. empty：创建一个空的自定义词典，可以通过SQL语句自行插入用户自定义关键词。
    3. 一个已存在的`分词词典`名字：创建一个和指定`分词词典`所使用的词典相同的词典（若参照的`分词词典`使用了系统默认词典，则新创建词典也会使用）。

- caseSensitive：词典大小写敏感控制。不能与`delimiter`一起使用。
  1. 不指定/true：大小写敏感，加入的关键词须严格大小写对应才能匹配出来。
  2. false：大小写不敏感

- delimiter：自定义分隔符。不能与`userdict`或`caseSensitive`一起使用。句子会简单地按指定的分隔符来分割。
  1. 不指定：默认不启用，使用`userdict`

### 词典修改
由用户创建的分词词典，均可以修改停用词和自定义词典，修改操作包括插入与全部删除。

#### 停用词词典修改
停用词修改通过vexjieba_add_stopwords函数完成。参数为词典名称与待插入的停用词。
```sql
SELECT vexjieba_add_stopwords(
    <dict_name_or_oid>,
    ARRAY[stopword1, stopword2, ...]);
```
删除语句通过vexjieba_delete_stopwords函数完成。参数为词典名称，会把该词典所使用的停用词全部清除。
```SQL
SELECT vexjieba_delete_stopwords(<dict_name_or_oid>);
```
- 内容输入格式为text[]，支持使用SQL将其他介质内容存储到数据库中后转化为该格式，例如:
```sql
CREATE TEMP TABLE stopwords (s text);
INSERT INTO stopwords VALUES ('1'), ('2'), ('3');
SELECT vexjieba_add_stopwords(<dict_name_or_oid>, (SELECT array_agg(s) FROM stopwords));
```

### 用户自定义词典修改
自定义词典插入关键词通过vexjieba_add_userdict函数完成。参数为词典名称与待插入的关键词。
- 插入关键词时可以指定词性与词频，以逗号','分隔。词频取值需为正整数，可参考<https://blog.csdn.net/NINIi619/article/details/129977087>，若不指定词频，则设置为10000（大于默认词典中绝大部分词的词频，确保能够被切分出来）。
```SQL
SELECT vexjieba_add_userdict(
    <dict_name_or_oid>,
    ARRAY[keyword1, keyword2, ...]);
```
删除语句通过vexjieba_delete_userdict函数完成。参数为词典名称，会把该词典所使用的用户自定义词典全部清除。
```SQL
SELECT vexjieba_delete_userdict(<dict_name_or_oid>);
```
- 内容输入格式为text[]，支持使用SQL将其他介质内容存储到数据库中后转化为该格式，请见上文停词部分示例。

### 词典重加载
当前分词词典不支持热加载功能。故修改词典后，需要进行词典重加载，重加载后修改方可生效。
```SQL
SELECT vexjieba_reload(<dict_name_or_oid>);
```

### 完整用例：
```SQL
-- 使用默认分词词典cn_tokenizer
SELECT bm25_tokenize('清华大学数据库实验室正在研发不仅高并发又高性能还高可用的vexdb向量数据库');
-- 分词结果:{清华大学,数据库,实验室,正在,研发,高,并发,高性能,高,可用,vexdb,向量,数据库}

-- 创建一个分词词典cn_dict，停用词词典为默认停用词，用户词典为空
CREATE TEXT SEARCH DICTIONARY cn_dict(
    template = pg_catalog.vex_jieba,
    stopwords = empty,
    userdict = empty);
-- 插入自定义停用词
SELECT vexjieba_add_stopwords(
    'cn_dict',
    ARRAY['不仅', '又', '还', '的']);
-- 插入自定义关键词
SELECT vexjieba_add_userdict(
    'cn_dict',
    ARRAY['清华大学数据库实验室,10000,nt', 'vexdb向量数据库,nz', '高并发,10000', '高可用', '高性能']);

-- 重新加载词典cn_dict并使用
CALL vexjieba_reload('cn_dict');
SELECT bm25_tokenize('清华大学数据库实验室正在研发不仅高并发又高性能还高可用的vexdb向量数据库', 'cn_dict');
-- 分词结果:{清华大学数据库实验室,正在,研发,高并发,高性能,高可用,vexdb向量数据库}
```
## Part 2: Design and Implementation
#### 系统表使用（`ts_cache.cpp`,`tsearchcmd.cpp`）
- pg_ts_dict：分词词典与内核中的全文搜索词典共用该系统表，并适配全文搜索词典的接口与SQL操作命令。
- pg_ts_content：分词词典专用系统表。用于存储用户自定义词典的信息，使得用户自定义词典不需要依赖于文件内容，在省去文件处理操作的同时提高了数据库系统安全性，统一了用户数据输入入口。
#### jieba分词模块(cppjieba/*)
分词器代码源自开源项目[cppjieba](https://github.com/yanyiwu/cppjieba)并进行了深入优化，支持多语言分词且内存消耗约为原代码的1%。
- 内核集成
    - 将字符串`string`类操作全部改写为C风格`char *`完成
    - 调整词典处理逻辑与顺序，支持停用词过滤、带空格关键词识别
    - 结合内核snowball英文词典对英文进行处理，具有多语言分词能力
- 内存优化
    - 使用double array trie(`darts.h`，源自开源项目[dart-cppjieba](https://github.com/byronhe/cppjieba))存储关键词
    - 释放词典创建过程中的使用的内存资源
#### 分词器初始化模块 (`dict_jieba.cpp`)
- 功能：初始化jieba分词器，加载停用词/用户词典
- 关键函数：
  - `djieba_init()`  
    - 解析`StopWords`和`UserDict`参数（支持`default`/`empty`/其他分词词典为模板）
    - 首次初始化时保存配置信息；非首次时直接构建分词器
    - 路径管理：自动获取系统默认词典路径（如`jieba_dict`, `hmm_model`）
  - `djieba_lexize()`  
    - 分词效果测试函数

#### 分词器资源管理模块 (`tokenizer.cpp`)
- 功能：管理全局分词器资源，支持动态更新词典
- 核心组件：
  - `TokenizerResource`结构  
    ```cpp
    struct TokenizerResource {
        Jieba* tokenizer;  // 分词器实例
        Oid dict_id;       // 词典OID
        int threads_num;   // 引用计数
        MemoryContext ctx; // 内存上下文
    };
    ```
  - 全局资源池  
    - 固定大小数组（`max_using_dict_num=16`）
    - 通过`g_instance.bm25_cxt.buffer`访问

- 关键机制：
  - 资源获取 (`get_bm25_dict()`)  
    - 优先从资源池匹配词典OID → 未命中时随机选择空闲槽位进行替换
    - 并发控制：自旋锁 + 指数退避等待
  - 资源释放 (`release_bm25_dict()`)
    - 释放当前线程使用的词典
  - 动态更新接口  
    ```cpp
    vexjieba_add_stopwords()  // 添加停用词
    vexjieba_add_userdict()   // 添加用户词典
    vexjieba_delete_stopwords() // 清空停用词
    vexjieba_delete_userdict() // 清空用户词典
    vexjieba_reload()         // 重载词典
    ```
  - 词典持久化与主备同步  
    - 使用系统表`pg_ts_content`存储词典内容
    - 类型标识：`'s'`（停用词） / `'u'`（用户词典）

#### 词典操作模块
- 核心函数：
  - `get_ts_content()`：从`pg_ts_content`加载词典内容
  - `update_ts_content()`：合并新词到现有词典（自动去重）
  - `clear_ts_content()`：清空词典（构造空数组替换）

#### 并发安全与资源跟踪
- 锁机制：  
  - `LWLock`（`BM25DictBufferLock`）保护资源池
- 引用计数：  
  - `threads_num`统计正在使用某词典资源的线程数
- 资源所有者集成：  
  ```cpp
  ResourceOwnerRememberBm25Dict()  // 事务资源跟踪
  ResourceOwnerForgetBm25Dict()    // 释放时解除关联
  ```
