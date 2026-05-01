#!/bin/bash
#######################################################################
# Copyright (c): 2020-2025, Huawei Tech. Co., Ltd.
# descript: Compile and pack vexdb
#           Return 0 means OK.
#           Return 1 means failed.
# version:  2.0
# date:     2020-08-08
#######################################################################
declare build_version_mode='release'
declare build_binarylib_dir='None'
declare wrap_binaries='NO'
declare not_optimized=''
declare config_file=''
declare extra_config_opt=''

#########################################################################
##read command line paramenters
#######################################################################

function print_help()
{
    echo "Usage: $0 [OPTION]
    -h|--help                         show help information
    -m|--version_mode                 this values of paramenter is debug, release, memcheck or mini, the default value is release
    -3rd|--binarylib_dir              the parent directory of binarylibs
    -pkg|--package                    (deprecated option)package the project,by default, only compile the project
    -wrap|--wrap_binaries             wrop up the project binaries. By default, only compile the project
    -nopt|--not_optimized             on kunpeng platform, like 1616 version, without LSE optimized
    --config_opt                      extra configuration flags
    -f|--config_file                  set postgresql.conf.sample from config_file when packing
    -T|--tassl                        build with tassl
    -lic                              build licensed version
    "
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            print_help
            exit 1
            ;;
        -m|--version_mode)
            if [ "$2"X = X ]; then
                echo "no given correct version information, such as: debug/release/memcheck/mini"
                exit 1
            fi
            build_version_mode=$2
            shift 2
            ;;
        -pkg|--package|-wrap|--wrap_binaries)
            wrap_binaries='YES'
            shift 1
            ;;
        -3rd|--binarylib_dir)
            if [ "$2"X = X ]; then
                echo "no given binarylib directory values"
                exit 1
            fi
            build_binarylib_dir=$2
            shift 2
            ;;
        -nopt|--not_optimized)
            not_optimized='-nopt'
            shift 1
            ;;
        --config_opt)
            if [ "$2"X = X ]; then
                echo "no extra configure options provided"
                exit 1
            fi
            extra_config_opt=$2
            echo "extra onfigure flag: ${extra_config_opt}"
            shift 2
            ;;
        -f|--config_file)
            if [[ ! -f "$2" ]]
            then
                echo "config_file does not exist"
                exit 1
            fi
            config_file=$(realpath "$2")
            shift 2
            ;;
        -T|--tassl)
            build_with_tassl='-T'
            shift 1
            ;;
        -lic)
            build_with_license='-lic'
            shift 1
            ;;    
         *)
            echo "Internal Error: option processing error: $1" 1>&2
            echo "please input right paramtenter, the following command may help you"
            echo "./build.sh --help or ./build.sh -h"
            exit 1
    esac
done


ROOT_DIR=$(cd $(dirname "${BASH_SOURCE[0]}") && pwd)
echo "ROOT_DIR : $ROOT_DIR"
cd build/script
chmod a+x build_opengauss.sh
./build_opengauss.sh -m ${build_version_mode} -3rd ${build_binarylib_dir} --config_opt "${extra_config_opt}" ${not_optimized} -pkg server ${build_with_tassl} ${build_with_license}
if [ "${wrap_binaries}"X = "YES"X ]
then
    chmod a+x package_opengauss.sh
    if [ X$config_file = "X" ];then
        ./package_opengauss.sh -3rd ${build_binarylib_dir} -m ${build_version_mode}
    else
        ./package_opengauss.sh -3rd ${build_binarylib_dir} -m ${build_version_mode} -f ${config_file}
    fi
fi
exit 0
