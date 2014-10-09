#!/bin/bash

git submodule update --init
sudo apt-get install g++ make libev-dev libprotobuf-dev libprotoc-dev protobuf-compiler libssl-dev libboost-dev libibverbs-dev libboost-program-options-dev --yes
if ! test -f fastrpc/config.h ; then
    cd fastrpc && git checkout master && ./configure && cd ..
fi

