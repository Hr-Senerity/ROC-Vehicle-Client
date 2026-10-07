#!/usr/bin/env bash
set -euo pipefail

source_root=${ROC_VEHICLE_SOURCE_ROOT:-/workspace}
build_root=${ROC_VEHICLE_BUILD_ROOT:-/build}

configure_build_test() {
    local name="$1"
    local c_compiler="$2"
    local cxx_compiler="$3"
    local build_dir="${build_root}/${name}"

    cmake \
        -S "${source_root}" \
        -B "${build_dir}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER="${c_compiler}" \
        -DCMAKE_CXX_COMPILER="${cxx_compiler}" \
        -DROC_VEHICLE_WARNINGS_AS_ERRORS=ON \
        -DROC_VEHICLE_BUILD_DRIVER=ON
    cmake --build "${build_dir}" --parallel 2
    ctest --test-dir "${build_dir}" --output-on-failure
}

configure_build_test container-gcc gcc g++
configure_build_test container-clang clang clang++
