#!/bin/bash

git submodule update --init
if ! test -f fastrpc/config.h ; then
    cd fastrpc && git checkout master && ./configure && cd ..
fi

