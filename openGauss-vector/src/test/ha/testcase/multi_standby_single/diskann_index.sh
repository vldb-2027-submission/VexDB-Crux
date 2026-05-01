#!/bin/sh

source ./util.sh

function test_select()
{

primary_res=$(gsql -p $dn1_primary_port -d $1 -r -c "SET enable_seqscan TO off; SELECT id FROM items ORDER BY embedding <-> (SELECT embedding from query where id = 1) LIMIT 1;")
standby_res=$(gsql -p $dn1_standby_port -d $1 -r -c "SET enable_seqscan TO off; SELECT id FROM items ORDER BY embedding <-> (SELECT embedding from query where id = 1) LIMIT 1;")

echo "db is : $1"
echo "primary_res: $primary_res"
echo "standby_res: $standby_res"
if [[ "$primary_res" = "$standby_res" ]]; then
  echo "$2 diskann index success"
else
  echo "$2 diskann index $failed_keyword"
  exit 1
fi

}

function test_filter_select()
{

primary_res=$(gsql -p $dn1_primary_port -d $1 -r -c "SET enable_seqscan TO off; SELECT id, embedding <-> (SELECT embedding FROM query WHERE id = 1) AS d FROM items WHERE id > 100 AND id < 1000 ORDER BY d LIMIT 10;")
standby_res=$(gsql -p $dn1_standby_port -d $1 -r -c "SET enable_seqscan TO off; SELECT id, embedding <-> (SELECT embedding FROM query WHERE id = 1) AS d FROM items WHERE id > 100 AND id < 1000 ORDER BY d LIMIT 10;")

echo "db is : $1"
echo "primary_res: $primary_res"
echo "standby_res: $standby_res"

if [[ "$primary_res" = "$standby_res" ]]; then
  echo "$2 index success"
else
  echo "$2 index $failed_keyword"
  exit 1
fi

}

function diskann_index_test()
{
db_name=$1

gsql -d $db -p $dn1_primary_port -c "create database $db_name;"

# create diskann index

gsql -d $db_name -p $dn1_primary_port -c "CREATE TABLE items (id BIGINT, embedding floatvector(128));"
gsql -d $db_name -p $dn1_primary_port -c "CREATE TABLE query (id BIGINT, embedding floatvector(128));"

gsql -d $db_name -p $dn1_primary_port -c "COPY items (id, embedding)
FROM '$vector_script_dir/data/diskann/vec_to_insert_10000.csv'
DELIMITER ' '
CSV;"

gsql -d $db_name -p $dn1_primary_port -c "COPY query (id, embedding)
FROM '$vector_script_dir/data/diskann/vec_to_query_100.csv'
DELIMITER ' '
CSV;"


gsql -d $db_name -p $dn1_primary_port -c "CREATE INDEX idx_diskann ON items USING diskann (embedding floatvector_l2_ops) WITH (parallel_workers=8);"
gsql -d $db_name -p $dn1_primary_port -c "CREATE INDEX idx_diskann_inplace_filter ON items USING diskann (embedding,id) WITH (parallel_workers=8);"

# test build diskann search
test_select "$db_name" "build" 
test_filter_select "$db_name" "inplace_attr_filter build"

# insert diskann index
gsql -d $db_name -p $dn1_primary_port -c "INSERT INTO items (id, embedding) SELECT * FROM query WHERE id < 100;"

# test insert diskann search
test_select "$db_name" "insert"
test_filter_select "$db_name" "inplace_attr_filter insert"

# delete diskann index and vacuum
gsql -d $db_name -p $dn1_primary_port -c "DELETE FROM items WHERE id > 7000;"

gsql -d $db_name -p $dn1_primary_port -c "VACUUM FULL items;"
# test delete diskann search 
test_select "$db_name" "delete"
test_filter_select "$db_name" "inplace_attr_filter dekete"
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
diskann_index_test "diskann_db_1"
}

function tear_down()
{
sleep 1
gs_guc set -Z datanode -D  $primary_data_dir -c "recovery_max_workers = 1"
gsql -d $db -p $dn1_primary_port -c "DROP DATABASE diskann_db_1;"
}

test_1
tear_down