#!/bin/bash

# ERSAP runtime paths (not defined in Dockerfile)
export ERSAP_HOME=$(pwd)/ersap
export ERSAP_USER_DATA=$(pwd)/ersap-data

# ROOT environment — uncomment if ROOT runtime vars are needed outside the compile stage
# source /rootlib/root/bin/thisroot.sh
