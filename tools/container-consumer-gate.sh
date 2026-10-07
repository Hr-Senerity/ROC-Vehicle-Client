#!/usr/bin/env bash
set -euo pipefail
revision=${1:?exact source revision required}
[[ $revision =~ ^[0-9a-f]{40}$ ]] || exit 2
short=${revision:0:12}
git -c safe.directory=/workspace -C /workspace diff --quiet "$revision" -- \
    src include CMakeLists.txt cmake || exit 2
evidence=${ROC_VEHICLE_EVIDENCE_DIR:-/build/v08-candidate12-evidence}
mkdir -p "$evidence"
source=/build/consumer-source-$short
[[ ! -e $source ]] || exit 2
install -d "$source"
git -c safe.directory=/workspace -C /workspace archive "$revision" | tar -x -C "$source"
for compiler in gcc clang; do
    prefix=/build/candidate-release-install-$short-$compiler
    library_build=/build/consumer-library-$short-$compiler
    cxx=g++; cc=gcc
    if [[ $compiler == clang ]]; then cxx=clang++; cc=clang; fi
    cmake -S "$source" -B "$library_build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DROC_VEHICLE_BUILD_DRIVER=OFF -DROC_VEHICLE_BUILD_TESTS=OFF \
        -DROC_VEHICLE_BUILD_EXAMPLES=OFF -DROC_VEHICLE_WARNINGS_AS_ERRORS=ON
    cmake --build "$library_build" --parallel 2
    cmake --install "$library_build" --prefix "$prefix"
    [[ -f $prefix/lib/libroc_vehicle_client.a && -d $prefix/include/roc/vehicle ]]
    # The installed tree is consumed without the project build tree or its
    # source include paths. Its test probe must never be an installed product.
    ! find "$prefix" -type f | grep -E 'crash_probe|driver_crash|test_instrumentation'
    build=/build/installed-release-consumer-$short-$compiler
    cmake -S /workspace/tests/consumer -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_PREFIX_PATH="$prefix;/opt/roc-deps"
    cmake --build "$build" --parallel 2
    state=/build/consumer-release-private-$short-$compiler
    install -d -m 0700 "$state"
    "$build/roc_vehicle_installed_consumer" "$state"
    [[ $(stat -c '%a' "$state/state.sqlite3") == 600 ]]
    sha256sum "$prefix/lib/libroc_vehicle_client.a" "$build/roc_vehicle_installed_consumer"
done 2>&1 | tee "$evidence/installed-release-consumer.log"
