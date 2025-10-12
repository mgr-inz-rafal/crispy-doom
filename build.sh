#!/bin/bash

pushd .
mkdir build
cd build
cmake ..
make -j$(nproc)
popd