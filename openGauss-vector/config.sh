#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2010-2018. All rights reserved.
# description: build config main entrance
# date: 2024-07-15
# version: 1.1.0

mode="release"
 
if [ "$#" -gt 0 ]; then
    param="$1"
    if [ "$param" == "debug" ]; then
        mode="debug"
    elif [ "$param" == "memcheck" ]; then
        mode="memcheck"
    fi
fi

if [ "$mode" == "debug" ]; then
    ./configure --gcc-version=10.3.0 CC=g++ CFLAGS="-O0" --prefix=$GAUSSHOME --3rd=$BINARYLIBS --enable-debug --enable-cassert --enable-thread-safety --with-readline --without-zlib # --with-python --with-includes='/home/huangmingwei/python/include/python3.10'
elif [ "$mode" == "memcheck" ]; then
    ./configure --gcc-version=10.3.0 CC=g++ CFLAGS="-O0" --prefix=$GAUSSHOME --3rd=$BINARYLIBS --enable-debug --enable-cassert --enable-thread-safety --with-readline --without-zlib --enable-memory-check
else
    ./configure --gcc-version=10.3.0 CC=g++ CFLAGS="-O2 -g3" --prefix=$GAUSSHOME --3rd=$BINARYLIBS --enable-thread-safety --with-readline --without-zlib # --with-python --with-includes='/home/huangmingwei/python/include/python3.10'
fi

