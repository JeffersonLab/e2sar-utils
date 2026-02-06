#!/bin/bash
source "$(pwd)/../root/bin/thisroot.sh"
export CODA=`pwd`/coda
export CODA_BIN=${CODA}/${MACHINE}/bin
export CODA_LIB=${CODA}/${MACHINE}/lib

export ERSAP_HOME=`pwd`/ersap
export ERSAP_USER_DATA=`pwd`/ersap-data

export PATH=${CODA_BIN}:${CODA}/common/bin:${ERSAP_HOME}/bin:${PATH}
