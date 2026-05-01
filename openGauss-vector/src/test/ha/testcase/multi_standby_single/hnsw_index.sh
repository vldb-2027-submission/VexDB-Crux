#!/bin/sh

source ./util.sh

function test_select()
{
primary_res=$(gsql -p $dn1_primary_port -d $1 -r -c "SET enable_seqscan TO off;SELECT id FROM items ORDER BY embedding <-> (SELECT embedding from query where id = 1) LIMIT 1;")
standby_res=$(gsql -p $dn1_standby_port -d $1 -r -c "SET enable_seqscan TO off;SELECT id FROM items ORDER BY embedding <-> (SELECT embedding from query where id = 1) LIMIT 1;")

echo "primary_res: $primary_res"
echo "standby_res: $standby_res"
if [[ "$primary_res" = "$standby_res" ]]; then
  echo "$2 hnsw index success"
else
  echo "$2 hnsw index $failed_keyword"
  exit 1
fi

}

function hnsw_index_test()
{
db_name=$1
gsql -d $db -p $dn1_primary_port -c "create database $db_name;"

gsql -d $db_name -p $dn1_primary_port -c "CREATE TABLE items (id BIGINT PRIMARY KEY, embedding floatvector(128));"
gsql -d $db_name -p $dn1_primary_port -c "CREATE TABLE query (id BIGINT PRIMARY KEY, embedding floatvector(128));"

gsql -d $db_name -p $dn1_primary_port -c "COPY items (id, embedding)
FROM '$vector_script_dir/data/diskann/vec_to_insert_10000.csv'
DELIMITER ' '
CSV;"

gsql -d $db_name -p $dn1_primary_port -c "COPY query (id, embedding)
FROM '$vector_script_dir/data/diskann/vec_to_query_100.csv'
DELIMITER ' '
CSV;"

# build index
gsql -d $db_name -p $dn1_primary_port -c "SET work_mem TO '1GB'; CREATE INDEX ON items USING hnsw (embedding floatvector_l2_ops) WITH (m = 32, ef_construction = 200, parallel_workers = 8);"
echo "build hnsw index success"
test_select "$db_name" "build"

# insert itup
gsql -d $db_name -p $dn1_primary_port -c "SET work_mem TO '1GB'; INSERT INTO items (id, embedding) SELECT id, embedding FROM query WHERE id < 100;"
test_select "$db_name" "insert"

# delete itup + vacuum
gsql -d $db_name -p $dn1_primary_port -c "SET work_mem TO '1GB'; DELETE FROM items WHERE id < 10000;"
gsql -d $db_name -p $dn1_primary_port -c "SET work_mem TO '1GB'; vacuum items;"
echo "vacuum hnsw index success"
test_select "$db_name" "vacuum"
}

function test_1()
{
set_default
check_instance_multi_standby
query_primary

kill_cluster
echo "begin to set parallel recovery param"
gs_guc set -Z datanode -D  $primary_data_dir -c "recovery_max_workers = 4"
start_cluster
hnsw_index_test "hnsw_db_1"
}

function tear_down()
{
sleep 1
gs_guc set -Z datanode -D  $primary_data_dir -c "recovery_max_workers = 1"
gsql -d $db -p $dn1_primary_port -c "DROP DATABASE hnsw_db_1;"
}

test_1
tear_down
