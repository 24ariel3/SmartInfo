#!/bin/sh
export IDF_PATH=~/esp/esp-idf
. $IDF_PATH/export.sh  #get_idf
rm  -rf  build  #mkdir  -p  build  #cd build
idf.py build  #cmake  -G "Ninja"  ..  -DCMAKE_BUILD_TYPE=Release  #ninja  -j 4
