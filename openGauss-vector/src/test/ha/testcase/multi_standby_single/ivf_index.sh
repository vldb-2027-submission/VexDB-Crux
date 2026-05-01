#!/bin/sh

source ./util.sh

function test_select()
{
primary_res=$(gsql -p $dn1_primary_port -d $1 -r -c "SET enable_seqscan TO off;set ivf_probes=1024;SELECT id FROM $2 ORDER BY embedding <-> (SELECT embedding from query where id = 1) LIMIT 1;")
standby_res=$(gsql -p $dn1_standby_port -d $1 -m -c "SET enable_seqscan TO off;set ivf_probes=1024;SELECT id FROM $2 ORDER BY embedding <-> (SELECT embedding from query where id = 1) LIMIT 1;")
echo "db is : $1"
echo "primary_res: $primary_res"
echo "standby_res: $standby_res"
if [[ "$primary_res" = "$standby_res" ]]; then
  echo "$3 ivf index success"
else
  echo "$3 ivf index $failed_keyword"
  exit 1
fi

}
function ivf_index_test()
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

# build index and test
gsql -d $db_name -p $dn1_primary_port -c "set maintenance_work_mem=307200; CREATE INDEX ON items USING ivfflat (embedding floatvector_l2_ops) with(ivf_nlist=1024);"
test_select "$db_name" "items" "build"

# insert itup and test
gsql -d $db_name -p $dn1_primary_port -c "CREATE TABLE items_insert (id bigserial PRIMARY KEY, embedding floatvector(128));"
gsql -d $db_name -p $dn1_primary_port -c "CREATE INDEX ON items_insert USING ivfflat (embedding floatvector_l2_ops);"
gsql -d $db_name -p $dn1_primary_port -c "set maintenance_work_mem=307200; INSERT INTO items_insert (id, embedding) SELECT id, embedding FROM items;"
test_select "$db_name" "items_insert" "insert"

# delete itup + vacuum, and test
gsql -d $db_name -p $dn1_primary_port -c "delete from items where id < 10000;"
gsql -d $db_name -p $dn1_primary_port -c "vacuum items;"
test_select "$db_name" "items" "vacuum"
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
ivf_index_test "ivf_db_1"
}


function tear_down()
{
sleep 1
gs_guc set -Z datanode -D  $primary_data_dir -c "recovery_max_workers = 1"
gsql -d $db -p $dn1_primary_port -c "DROP DATABASE ivf_db_1;"
}

test_1
tear_down