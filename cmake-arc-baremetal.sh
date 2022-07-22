#!/bin/sh

if [ -z $1 ]; then echo "$0 <path to root folder of CMakeList.txt>"; exit 1; fi
SRC_DIR=$(realpath $1)

ARC_ROOT=/opt/ARC/MetaWare/arc
ARC_TOOLCHAIN_CMAKE=${ARC_ROOT}/cmake/arc-mwdt.toolchain.cmake
ARC_TCF=${ARC_ROOT}/tcf/em4_ecc.tcf

CMAKE_DEFS=" -DCMAKE_TOOLCHAIN_FILE=${ARC_TOOLCHAIN_CMAKE}"
CMAKE_DEFS="${CMAKE_DEFS} -DARC_CFG_TCF_PATH=${ARC_TCF}"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_ENABLE_KEM_SIKE=OFF -DOQS_ENABLE_KEM_SIDH=OFF"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_PERMIT_UNSUPPORTED_ARCHITECTURE=ON"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_USE_OPENSSL=OFF"
CMAKE_DEFS="${CMAKE_DEFS} -DOQS_BUILD_ONLY_LIB=ON"

LOG_DIR="../log/"
LOG_FILE="${LOG_DIR}/cmake-arc-oqs-ninja-noopenssl.log"

echo "cmake parms = ${CMAKE_DEFS}"
cmake --debug --trace-expand -GNinja ${CMAKE_DEFS} ${SRC_DIR} 2>&1 | tee ${LOG_FILE}

