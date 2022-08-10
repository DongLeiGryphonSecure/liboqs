#!/bin/sh

SRC_DIR=$(dirname $(realpath $0))
if [ $SRC_DIR = $(pwd) ]; then echo "this should be run in build dir"; exit 1; fi

#ARC_ROOT=/cyg/opt/ARC/MetaWare/arc
#ARC_ROOT=/cygdriver/ARC/MetaWare/arc
ARC_ROOT=/opt/ARC/MetaWare/arc
ARC_TOOLCHAIN_CMAKE=${ARC_ROOT}/cmake/arc-mwdt.toolchain.cmake
ARC_TCF=${ARC_ROOT}/tcf/em4_ecc.tcf

CMAKE_DEFS=" -DCMAKE_TOOLCHAIN_FILE=${ARC_TOOLCHAIN_CMAKE}"
CMAKE_DEFS="${CMAKE_DEFS} -DARC_CFG_TCF_PATH=${ARC_TCF}"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_ENABLE_KEM_SIKE=OFF -DOQS_ENABLE_KEM_SIDH=OFF"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_PERMIT_UNSUPPORTED_ARCHITECTURE=ON"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_USE_OPENSSL=OFF"
CMAKE_DEFS="${CMAKE_DEFS} -DCMAKE_BUILD_TYPE=Debug"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_OPT_TARGET=generic"
#CMAKE_DEFS="${CMAKE_DEFS} -DOQS_BUILD_ONLY_LIB=ON"

LOG_DIR=$(realpath "../log")
[ -d ]
LOG_FILE="${LOG_DIR}/cmake-arc-oqs-ninja-noopenssl.log"
CMAKE_GEN=Ninja
BUILD_DIR=

echo "cmake parms = ${CMAKE_DEFS}"
echo "ARC_TCF = ${ARC_TCF}"
echo "ARC_TOOLCHAIN = ${ARC_TOOLCHAIN_CMAKE}"
echo "LOG_FILE = ${LOG_FILE}"
echo "Generate = ${CMAKE_GEN}"
read -p "Continue cmake build generate ? (y/n) " rc
[ $rc = y ] || exit 1
cmake --debug-output --trace-expand -G${CMAKE_GEN} ${CMAKE_DEFS} ${SRC_DIR} 2>&1 | tee ${LOG_FILE}
exit 0

