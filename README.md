# VexDB-Crux: A Disk-Based Cross-Modal Vector Index
VexDB is a mature vector database system. Crux is a novel indexing technique integrated into it. The core source code for Crux is located at: ``./openGauss-vector/src/gausskernel/storage/access/crux/``

# Getting Started
First, follow the instructions in ``./openGauss-vector/README.en.md`` to configure, compile, and install VexDB.

Next, you need to set up your environment variables. You can append the following ``env.source`` configuration to your ``~/.bashrc`` file:

env.source 
```sh
export WORKBASE=/home/xxx/workspace
export DATA=$WORKBASE/data
export DATABASE=$DATA/single_data
export GAUSSLOG=$DATA/log
export CODE_BASE=$WORKBASE/code_prepare/openGauss-vector     # openGauss-server的路径
export BINARYLIBS=$WORKBASE/third_party_libs    # binarylibs的路径
export GAUSSHOME=$WORKBASE/install/
export GCC_PATH=$BINARYLIBS/buildtools/gcc10.3
export CC=$GCC_PATH/gcc/bin/gcc
export CXX=$GCC_PATH/gcc/bin/g++
export PYTHON_PATH=/home/xxx/python
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$PYTHON_PATH/lib:$GCC_PATH/gcc/lib64:$GCC_PATH/isl/lib:$GCC_PATH/mpc/lib:$GCC_PATH/mpfr/lib:$GCC_PATH/gmp/lib:$BINARYLIBS/kernel/dependency/OpenBLAS/lib:$LD_LIBRARY_PATH
export LIBRARY_PATH=$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:$GCC_PATH/gcc/bin:/home/xxx/.local/bin:$BINARYLIBS/kernel/platform/openjdk8/x86_64/jdk/bin:$PYTHON_PATH/bin:$PATH
export PYTHON=$PYTHON_PATH/bin/python3
export GPORT=28457
mkdir -p ${WORKBASE}/asan
export ASAN_OPTIONS="detect_leaks=1:verbosity=0:log_path=${WORKBASE}/asan/mem.log:halt_on_error=0:abort_on_error=1:detect_odr_violation=0:disable_coredump=0:unmap_shadow_on_exit=1"
export DEBUG_TYPE=debug
export JAVA_HOME=$BINARYLIBS/kernel/platform/openjdk8/x86_64/jdk
export CPUPROFILE=$WORKBASE/profiler/prof.out
export CPUPROFILE_FREQUENCY=2997

alias list='ls -la'
alias fh='find . -name '
alias untar='tar -zxvf $1'
alias ttar='tar -czvf $1'
alias gs='git status'  
alias ga='git commit --date="$(date)" --amend'
alias gb='git branch -v'  
alias gr='git remote -v'

alias gaussdb='vexdb'
alias gauss_run='vexdb -D $DATABASE -p $GPORT'
alias gsinit_single='rm -rf $DATABASE && vex_initdb -D $DATABASE --pwpasswd=gauss@234 --nodename=datanode'
alias gsstart='vex_ctl -D $DATABASE -o "-p $GPORT" start'
alias gsrestart='vex_ctl -D $DATABASE -o "-p $GPORT" restart'
alias gsstop='vex_ctl -D $DATABASE -o "-p $GPORT" stop'
alias conn='vsql -d postgres -p $GPORT -r'
alias rd='cd $CODE_BASE/src/gausskernel/storage/access/bm25 && make clean -s && cd $CODE_BASE/src/common/backend/utils/cache && rm -f ts_cache.o'
alias rdd='cd $CODE_BASE/src/gausskernel/storage/access/hnsw && make clean -s'
alias mk='cd $CODE_BASE && make -sj10'
alias rmk='rd && mk'
alias rdmk='rdd && mk'
alias mi='cd $CODE_BASE && make install -sj6'
alias mkmi='mk && mi'
alias rmkmi='rmk && mi'
alias rdmkmi='rdmk && mi'
alias gsps='ps ux | grep "vexdb\|vex_ctl"'
alias gdb='/home/gdb8/gdb'
alias gdbgauss='/home/gdb8/gdb --args vexdb -D $DATABASE -p $GPORT'

cleanall() {
    rm -rf $GAUSSHOME
    cd $CODE_BASE
    make clean -sj6 > /dev/null
}

mdsd() {
    cleanall
    cd $CODE_BASE
    bash ./config.sh debug
    mkmi
}

mdsr() {
    cleanall
    cd $CODE_BASE
    bash ./config.sh release
    mkmi
}

mdsm() {
    cleanall
    cd $CODE_BASE
    bash ./config.sh memcheck
    mkmi
}

compile_pagehack() {
    cd $CODE_BASE/contrib/pagehack
    make -s
    cp pagehack.so $GAUSSHOME/lib/
    cp pagehack $GAUSSHOME/bin/
    cd - &>/dev/null
}
```

write env.source into ~/.bashrc

# Data Preparation

1. Download Original Data
Run the preparation script to download the datasets used in the paper. The files will be saved in the ``./data`` directory.
Taking the Yandex Text-to-Image (``t2i-10M``) dataset as an example:
```shell
bash prepare_data.sh t2i-10M
```

2. knn computation
```shell
python3 compute_knn.py --base ./data/t2i-10M/base.10M.fbin --query ./data/t2i-10M/train.10M.fbin --output ./data/t2i-10M/gt.t2b.A.bin
```

3. csv & sql files preparation
```shell 
python3 fbin2csv.py ./data/t2i-10M/base.10M.fbin ./data/t2i-10M/base.csv 0
python3 fbin2csv.py ./data/t2i-10M/train.10M.fbin ./data/t2i-10M/reference.csv 10000000
python3 convert_sql.py ./data/t2i-10M/query.10k.fbin ./data/t2i-10M/query.10k.sql
```

# Building and Searching Crux
1. Parameter Configuration
The parameters for the Crux index are defined in ``./openGauss-vector/src/include/access/crux/crux.h``. Ensure they match your specific dataset configuration:
```C++
#define VEC_DIM 200
#define QUERYSIZE 10000000
#define GTSIZE 10000000
constexpr char *filename = "./data/t2i-10M/gt.t2b.A.bin";

#define Default_M 17
#define Act_cluster_num 4
#define Default_Cluster_num 20
#define Default_efConstruction 500
#define Default_NumberGroundTruth 100
```

2. Index construction & search:
Use the ``vsql`` terminal (aliased as ``conn``) to create the database, load the data, build the index, and execute queries:
```shell
conn -c "create database d1"

conn -d d1 -c "create table t_t2i_crux (_id int, repr floatvector(200));"

conn -d d1 -c "copy t_t2i_crux from './data/t2i-10M/reference.csv' csv;"

conn -d d1 -c "copy t_t2i_crux from './data/t2i-10M/data.csv' csv;"

time conn -d d1 -c "create index on t_t2i_crux using crux(repr floatvector_ip_ops);"

time conn -d d1 -f ./data/t2i-10M/query.10k.sql > ./data/t2i-10M/res.query.10k.txt
```