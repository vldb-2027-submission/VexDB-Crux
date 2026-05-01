-- !!!删除同名表!!!!!!
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_name = 'test_table') THEN
        DROP TABLE test_table;
        RAISE NOTICE 'Table test_table dropped successfully';
    ELSE
        RAISE NOTICE 'Table test_table does not exist';
    END IF;

    IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_name = 'items_test') THEN
        DROP TABLE items_test;
        RAISE NOTICE 'Table items_test dropped successfully';
    ELSE
        RAISE NOTICE 'Table items_test does not exist';
    END IF;
END $$;



-- 创建表
DO $$
BEGIN
    CREATE TABLE test_table (
        id SERIAL PRIMARY KEY,
        fvdata floatvector(3),
        intdata INT[],
        float8array float8[]
    );
    CREATE TABLE items_test (id bigserial PRIMARY KEY, embedding floatvector(3), fvdata2 floatvector(3));

    RAISE NOTICE 'Table created successfully';
EXCEPTION
    WHEN duplicate_table THEN
        RAISE NOTICE 'Table already exists';
    WHEN OTHERS THEN
        RAISE NOTICE 'Table creation failed: %', SQLERRM;
END $$;

-- 插入数据
DO $$
BEGIN
    INSERT INTO test_table (fvdata) VALUES ('[1.1, 2.2, 3.3]');
    RAISE NOTICE 'Inserted data: [1.1, 2.2, 3.3]';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Insert data failed: [1.1, 2.2, 3.3] %', SQLERRM;
END $$;

DO $$
BEGIN
    INSERT INTO test_table (fvdata) VALUES ('[4.4, 5.5, 6.6]');
    RAISE NOTICE 'Inserted data: [4.4, 5.5, 6.6]';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Insert data failed: [4.4, 5.5, 6.6] %', SQLERRM;
END $$;

DO $$
BEGIN
    INSERT INTO test_table (fvdata) VALUES ('[7.7, 8.8, 9.9]');
    RAISE NOTICE 'Inserted data: [7.7, 8.8, 9.9]';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Insert data failed: [7.7, 8.8, 9.9] %', SQLERRM;
END $$;

-- 这行数据插入预计会失败
DO $$
BEGIN
    INSERT INTO test_table (fvdata) VALUES ('[1.1,2.2,3.3,4.4]');
    RAISE NOTICE 'Inserted data: [1.1,2.2,3.3,4.4]';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Invalid data check successfully: [1.1,2.2,3.3,4.4] %', SQLERRM;
END $$;

-- 查询数据
DO $$
DECLARE
    r RECORD;
BEGIN
    FOR r IN SELECT * FROM test_table LOOP
        RAISE NOTICE 'Query result: id=%, fvdata=%', r.id, r.fvdata;
    END LOOP;
END $$;

-- 删除数据
DO $$
BEGIN
    DELETE FROM test_table WHERE id = 3;
    RAISE NOTICE 'Deleted data where id = 3';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Delete data failed: id=3 %', SQLERRM;
END $$;

-- 再次查询数据，确认删除操作已经执行
DO $$
DECLARE
    r RECORD;
BEGIN
    FOR r IN SELECT * FROM test_table LOOP
        RAISE NOTICE 'Query result after delete: id=%, fvdata=%', r.id, r.fvdata;
    END LOOP;
END $$;

-- 测试 array_to_floatvector 函数
DO $$
DECLARE
    r floatvector;
BEGIN
    r := array_to_floatvector(ARRAY[1.1,2.2,3.3]::float4[], 3);
    RAISE NOTICE 'Test array_to_floatvector with float4: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test array_to_floatvector with float4 failed: %', SQLERRM;
END $$;

DO $$
DECLARE
    r floatvector;
BEGIN
    r := array_to_floatvector(ARRAY[1, 2, 3]::int[], 3);
    RAISE NOTICE 'Test array_to_floatvector with int: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test array_to_floatvector with int failed: %', SQLERRM;
END $$;

-- 测试 floatvector_to_float4 函数
DO $$
DECLARE
    r float4[];
BEGIN
    r := floatvector_to_float4('[1.1,2.2,3.3]'::floatvector);
    RAISE NOTICE 'Test floatvector_to_float4: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_to_float4 failed: %', SQLERRM;
END $$;

-- 测试 l2_distance 函数
DO $$
DECLARE
    r RECORD;
BEGIN
    RAISE NOTICE 'INSERT some test data';
    INSERT INTO items_test (embedding,fvdata2) VALUES ('[1,2,3]','[-3,1,-2]'), ('[3,1,2]','[1,2,3]'), ('[4,5,6]','[7,8,-9]'), ('[7,8,9]','[7,8,9]'), ('[10,11,12]','[13,15,16]'), ('[13,15,16]','[13,15,16]'), ('[14,15,-16]','[-15,-15,16]'), ('[15,15,16]','[-10,-11,-12]');
    RAISE NOTICE 'Test 5 nearest neighbor with [3,1,2] in l2_distance:';
    FOR r IN SELECT * FROM items_test ORDER BY embedding <-> '[3,1,2]' LIMIT 5 LOOP
        RAISE NOTICE 'id=%, embedding=%', r.id, r.embedding;
    END LOOP;
END $$;

-- 测试 floatvector_l2_squared_distance 函数
DO $$
DECLARE
    r float8;
BEGIN
    RAISE NOTICE 'INSERT some test data';
    r := (SELECT floatvector_l2_squared_distance(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_l2_squared_distance: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_l2_squared_distance failed: %', SQLERRM;
END $$;

-- 测试 inner_product 函数
DO $$
DECLARE
    r float8;
BEGIN
    r := (SELECT inner_product(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test inner_product: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test inner_product failed: %', SQLERRM;
END $$;

-- 测试 floatvector_negative_inner_product 函数
DO $$
DECLARE
    r float8;
BEGIN
    r := (SELECT floatvector_negative_inner_product(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_negative_inner_product: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_negative_inner_product failed: %', SQLERRM;
END $$;

-- 测试 cosine_distance 函数
DO $$
DECLARE
    r float8;
BEGIN
    r := (SELECT cosine_distance(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test cosine_distance: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test cosine_distance failed: %', SQLERRM;
END $$;

-- 测试 floatvector_spherical_distance 函数
DO $$
BEGIN
    RAISE NOTICE 'floatvector_spherical_distance()';
END $$;
SELECT 
    embedding,
    fvdata2,
    floatvector_spherical_distance(embedding,  fvdata2) AS spherical_distance
FROM 
    items_test;

-- 测试 floatvector_dims 函数
DO $$
DECLARE
    r int;
BEGIN
    r := (SELECT floatvector_dims(fvdata) FROM test_table WHERE id = 1);
    RAISE NOTICE 'Test floatvector_dims: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_dims failed: %', SQLERRM;
END $$;

-- 测试 floatvector_norm 函数
DO $$
DECLARE
    r float8;
BEGIN
    r := (SELECT floatvector_norm(fvdata) FROM test_table WHERE id = 1);
    RAISE NOTICE 'Test floatvector_norm: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_norm failed: %', SQLERRM;
END $$;

-- 测试 floatvector_add 函数
DO $$
DECLARE
    r floatvector;
BEGIN
    r := (SELECT floatvector_add(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_add: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_add failed: %', SQLERRM;
END $$;

-- 测试 floatvector_sub 函数
DO $$
DECLARE
    r floatvector;
BEGIN
    r := (SELECT floatvector_sub(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_sub: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_sub failed: %', SQLERRM;
END $$;

-- 测试 floatvector_lt 函数
DO $$
DECLARE
    r boolean;
BEGIN
    r := (SELECT floatvector_lt(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_lt: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_lt failed: %', SQLERRM;
END $$;

-- 测试 floatvector_le 函数
DO $$
DECLARE
    r boolean;
BEGIN
    r := (SELECT floatvector_le(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_le: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_le failed: %', SQLERRM;
END $$;

-- 测试 floatvector_eq 函数
DO $$
DECLARE
    r boolean;
BEGIN
    r := (SELECT floatvector_eq(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_eq: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_eq failed: %', SQLERRM;
END $$;

-- 测试 floatvector_ne 函数
DO $$
DECLARE
    r boolean;
BEGIN
    r := (SELECT floatvector_ne(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_ne: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_ne failed: %', SQLERRM;
END $$;

-- 测试 floatvector_ge 函数
DO $$
DECLARE
    r boolean;
BEGIN
    r := (SELECT floatvector_ge(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_ge: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_ge failed: %', SQLERRM;
END $$;

-- 测试 floatvector_gt 函数
DO $$
DECLARE
    r boolean;
BEGIN
    r := (SELECT floatvector_gt(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_gt: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_gt failed: %', SQLERRM;
END $$;

-- 测试 floatvector_cmp 函数
DO $$
DECLARE
    r int;
BEGIN
    r := (SELECT floatvector_cmp(a.fvdata, b.fvdata)
          FROM test_table a, test_table b
          WHERE a.id = 1 AND b.id = 2);
    RAISE NOTICE 'Test floatvector_cmp: %', r;
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Test floatvector_cmp failed: %', SQLERRM;
END $$;

-- floatvector_accum
CREATE TEMP TABLE accum_result (
    result float8[]
);

INSERT INTO accum_result VALUES (ARRAY[0, 0.0, 0.0, 0.0]);
DO
$$
DECLARE
    rec RECORD;
BEGIN
    FOR rec IN SELECT fvdata FROM test_table
    LOOP
        UPDATE accum_result
        SET result = floatvector_accum(result, rec.fvdata);
    END LOOP;
END
$$;

-- 显示累积的结果
DO $$
BEGIN
    RAISE NOTICE 'Test result of floatvector_accum()';
END $$;
SELECT * FROM accum_result;


-- 测试 floatvector_combine 函数
DO $$
BEGIN
    RAISE NOTICE 'Test result of floatvector_combine()';
END $$;
SELECT floatvector_combine(ARRAY[1.1,2.2,3.3]::float4[], ARRAY[4.4,5.5,6.6]::float4[]);


-- 测试 floatvector_avg 函数
DO $$
BEGIN
    RAISE NOTICE 'floatvector_avg()';
END $$;
select floatvector_avg(array[5, 1, 2.5, 3]::float8[]);
select floatvector_avg(array[2, 1, 2.5, 3]::float8[]);