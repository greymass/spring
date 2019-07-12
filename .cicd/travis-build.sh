#!/bin/bash
set -e
CPU_CORES=$(getconf _NPROCESSORS_ONLN)
if [[ "$(uname)" == Darwin ]]; then
    brew install ccache
    export PATH="/usr/local/opt/ccache/libexec:$PATH"
    echo 'Detected Darwin, building natively.'
    [[ -d eos ]] && cd eos
    [[ ! -d build ]] && mkdir build
    cd build
    cmake ..
    make -j $CPU_CORES
    ctest -j $CPU_CORES -LE _tests --output-on-failure -T Test # run unit tests
else # linux
    echo 'Detected Linux, building in Docker.'
    echo "$ docker pull eosio/producer:ci-$IMAGE_TAG"
    docker pull eosio/producer:ci-$IMAGE_TAG
    # per-distro additions to docker commands
    [[ $IMAGE_TAG  == centos-7 ]] && PRE_COMMANDS="source /opt/rh/devtoolset-8/enable && source /opt/rh/rh-python36/enable &&"
    [[ $IMAGE_TAG == amazonlinux-2 ]] && CMAKE_EXTRAS="-DCMAKE_CXX_COMPILER='clang++' -DCMAKE_C_COMPILER='clang'" # explicitly set to clang as gcc is default
    [[ $IMAGE_TAG == ubuntu-16.04 ]] && CMAKE_EXTRAS="$CMAKE_EXTRAS -DCMAKE_TOOLCHAIN_FILE='/tmp/pinned_toolchain.cmake'" # pinned only
    [[ $IMAGE_TAG == amazonlinux-2 || $IMAGE_TAG == centos-7 ]] && EXPORTS="export PATH=/usr/lib64/ccache:$PATH &&" || EXPORTS="export PATH=/usr/lib/ccache:$PATH &&" # ccache needs to come first in the list (devtoolset-8 overrides that if we include this in the Dockerfile)
    # docker
    docker run --rm -v $(pwd):/eos -v /usr/lib/ccache -v $HOME/.ccache:/opt/.ccache -e CCACHE_DIR=/opt/.ccache eosio/producer:ci-$IMAGE_TAG bash -c "echo \$CXX && echo \$CC && $PRE_COMMANDS ccache -s && mkdir /eos/build && cd /eos/build && $EXPORTS cmake -DCMAKE_BUILD_TYPE='Release' -DCORE_SYMBOL_NAME='SYS' -DOPENSSL_ROOT_DIR='/usr/include/openssl' -DBUILD_MONGO_DB_PLUGIN=true $CMAKE_EXTRAS /eos && make -j $(getconf _NPROCESSORS_ONLN)"
fi 