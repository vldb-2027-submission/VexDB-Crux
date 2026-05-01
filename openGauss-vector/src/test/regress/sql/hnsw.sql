SET maintenance_work_mem TO '8GB';
SET work_mem TO '1GB';

DROP TABLE IF EXISTS items_test_hnsw;

CREATE TABLE items_test_hnsw (id int, embedding floatvector(128));

ALTER TABLE items_test_hnsw SET (parallel_workers = 15);
ALTER TABLE items_test_hnsw ALTER COLUMN embedding SET STORAGE PLAIN;

DO $$
BEGIN
    RAISE NOTICE '-----------------Start to copy data--------------------';
END $$;

-- Copy data from CSV to table
COPY items_test_hnsw (id, embedding)
FROM '@abs_srcdir@/data/hnsw_data/hnsw_data.csv'
DELIMITER ' '
CSV;

DO $$
BEGIN
    RAISE NOTICE '-----------------Start to create index--------------------';
END $$;

CREATE INDEX ON items_test_hnsw USING hnsw (embedding floatvector_l2_ops) WITH (m = 32, ef_construction = 200, parallel_workers = 15);

DO $$
BEGIN
    RAISE NOTICE '-----------------Index Creation Done--------------------';
END $$;

-- 开始增删改查操作
DO $$
BEGIN
    RAISE NOTICE '-----------------Start to perform fastcheck operations--------------------';
END $$;

-- 插入操作：插入新的数据
INSERT INTO items_test_hnsw (id, embedding)
VALUES 
(1001, '[0.0,0.1,0.2,0.3,32.0,31.0,14.0,10.0,11.0,78.0,55.0,10.0,45.0,83.0,11.0,6.0,14.0,57.0,102.0,75.0,20.0,8.0,3.0,5.0,67.0,17.0,19.0,26.0,5.0,0.0,1.0,22.0,60.0,26.0,7.0,1.0,18.0,22.0,84.0,53.0,85.0,119.0,119.0,4.0,24.0,18.0,7.0,7.0,1.0,81.0,106.0,102.0,72.0,30.0,6.0,0.0,9.0,1.0,9.0,119.0,72.0,1.0,4.0,33.0,119.0,29.0,6.0,1.0,0.0,1.0,14.0,52.0,119.0,30.0,3.0,0.0,0.0,55.0,92.0,111.0,2.0,5.0,4.0,9.0,22.0,89.0,96.0,14.0,1.0,0.0,1.0,82.0,59.0,16.0,20.0,5.0,25.0,14.0,11.0,4.0,0.0,0.0,1.0,26.0,47.0,23.0,4.0,0.0,0.0,4.0,38.0,83.0,30.0,14.0,9.0,4.0,9.0,17.0,23.0,41.0,0.0,0.0,2.0,8.0,19.0,25.0,23.0,1.0]'),
(1002, '[0.0,0.9,0.8,0.7,32.0,31.0,14.0,10.0,11.0,78.0,55.0,10.0,45.0,83.0,11.0,6.0,14.0,57.0,102.0,75.0,20.0,8.0,3.0,5.0,67.0,17.0,19.0,26.0,5.0,0.0,1.0,22.0,60.0,26.0,7.0,1.0,18.0,22.0,84.0,53.0,85.0,119.0,119.0,4.0,24.0,18.0,7.0,7.0,1.0,81.0,106.0,102.0,72.0,30.0,6.0,0.0,9.0,1.0,9.0,119.0,72.0,1.0,4.0,33.0,119.0,29.0,6.0,1.0,0.0,1.0,14.0,52.0,119.0,30.0,3.0,0.0,0.0,55.0,92.0,111.0,2.0,5.0,4.0,9.0,22.0,89.0,96.0,14.0,1.0,0.0,1.0,82.0,59.0,16.0,20.0,5.0,25.0,14.0,11.0,4.0,0.0,0.0,1.0,26.0,47.0,23.0,4.0,0.0,0.0,4.0,38.0,83.0,30.0,14.0,9.0,4.0,9.0,17.0,23.0,41.0,0.0,0.0,2.0,8.0,19.0,25.0,23.0,1.0]');

DO $$
BEGIN
    RAISE NOTICE 'Insertion Complete: % rows added', (SELECT COUNT(*) FROM items_test_hnsw WHERE id IN (1001, 1002));
END $$;

-- 查询操作：查找最接近给定向量的5个数据
SELECT id, embedding
FROM items_test_hnsw
ORDER BY embedding <-> '[0.0,0.1,0.2,0.3,32.0,31.0,14.0,10.0,11.0,78.0,55.0,10.0,45.0,83.0,11.0,6.0,14.0,57.0,102.0,75.0,20.0,8.0,3.0,5.0,67.0,17.0,19.0,26.0,5.0,0.0,1.0,22.0,60.0,26.0,7.0,1.0,18.0,22.0,84.0,53.0,85.0,119.0,119.0,4.0,24.0,18.0,7.0,7.0,1.0,81.0,106.0,102.0,72.0,30.0,6.0,0.0,9.0,1.0,9.0,119.0,72.0,1.0,4.0,33.0,119.0,29.0,6.0,1.0,0.0,1.0,14.0,52.0,119.0,30.0,3.0,0.0,0.0,55.0,92.0,111.0,2.0,5.0,4.0,9.0,22.0,89.0,96.0,14.0,1.0,0.0,1.0,82.0,59.0,16.0,20.0,5.0,25.0,14.0,11.0,4.0,0.0,0.0,1.0,26.0,47.0,23.0,4.0,0.0,0.0,4.0,38.0,83.0,30.0,14.0,9.0,4.0,9.0,17.0,23.0,41.0,0.0,0.0,2.0,8.0,19.0,25.0,23.0,1.0]'
LIMIT 5;

DO $$
BEGIN
    RAISE NOTICE 'Query Complete: Retrieved 5 closest vectors';
END $$;

-- 更新操作：更新某个id的向量
UPDATE items_test_hnsw
SET embedding = '[0.0,0.0,0.0,0.0,0.0,31.0,14.0,10.0,11.0,78.0,55.0,10.0,45.0,83.0,11.0,6.0,14.0,57.0,102.0,75.0,20.0,8.0,3.0,5.0,67.0,17.0,19.0,26.0,5.0,0.0,1.0,22.0,60.0,26.0,7.0,1.0,18.0,22.0,84.0,53.0,85.0,119.0,119.0,4.0,24.0,18.0,7.0,7.0,1.0,81.0,106.0,102.0,72.0,30.0,6.0,0.0,9.0,1.0,9.0,119.0,72.0,1.0,4.0,33.0,119.0,29.0,6.0,1.0,0.0,1.0,14.0,52.0,119.0,30.0,3.0,0.0,0.0,55.0,92.0,111.0,2.0,5.0,4.0,9.0,22.0,89.0,96.0,14.0,1.0,0.0,1.0,82.0,59.0,16.0,20.0,5.0,25.0,14.0,11.0,4.0,0.0,0.0,1.0,26.0,47.0,23.0,4.0,0.0,0.0,4.0,38.0,83.0,30.0,14.0,9.0,4.0,9.0,17.0,23.0,41.0,0.0,0.0,2.0,8.0,19.0,25.0,23.0,1.0]'
WHERE id = 1001;

DO $$
BEGIN
    RAISE NOTICE 'Update Complete: Vector updated for id 1001';
END $$;

-- 删除操作：删除指定id的数据
DELETE FROM items_test_hnsw
WHERE id = 1002;

DO $$
BEGIN
    RAISE NOTICE 'Delete Complete: id 1002 removed';
END $$;

-- 检查表中剩余的数据量
DO $$
BEGIN
    RAISE NOTICE 'Final Check: Remaining rows in the table %', (SELECT COUNT(*) FROM items_test_hnsw);
END $$;

-- 测试在分区表上创建hnsw索引
DO $$
BEGIN
    RAISE NOTICE '-----------------build hnsw on partition and select on partition--------------------';
END $$;

CREATE OR REPLACE FUNCTION random_array(dim integer,min_value int, max_value int)

RETURNS text

AS $$

SELECT REGEXP_REPLACE(REGEXP_REPLACE(array_agg(round(random()* (max_value - min_value + 1) + min_value,3))::text,'{','['),'}',']')

FROM generate_series(1, dim);

$$

LANGUAGE SQL

VOLATILE

COST 1;

--创建range分区表

create table t_1194515a(c1 int unique,c2 floatvector(3))

partition by range(c1)

(

partition p1 values less than(50),

partition p2 values less than(1000));

create table t_1194515b(c1 int unique,c2 floatvector(3))

partition by range(c1)

(

partition p1 values less than(50),

partition p2 values less than(1000));


--insert 

insert into t_1194515a values(1,'[1,2,3]');

insert into t_1194515a values(2,'[4,5,6]'),(3,'[7,8,9]');

insert into t_1194515a values(4,null);

INSERT INTO t_1194515a SELECT i, random_array(3,1,3)::floatvector(3) FROM generate_series(5, 999) AS i;

insert into t_1194515b values(1,'[1,2,3]');


--创建索引 
CREATE INDEX idx_1194515b ON t_1194515a USING hnsw(c2 floatvector_l2_ops) local;

DO $$
BEGIN
    RAISE NOTICE '-----------------Fastcheck operations completed--------------------';
END $$;