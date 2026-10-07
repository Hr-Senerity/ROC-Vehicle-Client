#!/usr/bin/env bash
set -euo pipefail

# Execute INSIDE the existing dev container. No package install, Docker daemon
# change, host dependency change, endpoint discovery or credential argument.
build_dir=/build/driver-cli
if [[ $# -lt 1 ]]; then
    echo 'Usage: container-driver.sh --build|--help|--precheck CONFIG|--run CONFIG [--allow-remote]' >&2
    exit 2
fi
mode="$1"
case "${mode}" in
    --build|--help) [[ $# -eq 1 ]] || exit 2 ;;
    --precheck|--run)
        [[ $# -eq 2 || ( $# -eq 3 && "$3" == '--allow-remote' ) ]] || exit 2 ;;
    *) exit 2 ;;
esac
cmake -S /workspace -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DROC_VEHICLE_BUILD_DRIVER=ON -DROC_VEHICLE_BUILD_TESTS=OFF \
    -DROC_VEHICLE_BUILD_EXAMPLES=OFF -DROC_VEHICLE_WARNINGS_AS_ERRORS=ON
cmake --build "${build_dir}" --parallel 2 --target roc_vehicle_driver
case "${mode}" in
    --build) exit 0 ;;
    --help) exec "${build_dir}/roc_vehicle_driver" --help ;;
    --precheck|--run)
        if [[ $# -eq 3 ]]; then
            exec "${build_dir}/roc_vehicle_driver" --config "$2" "${mode}" --allow-remote
        fi
        exec "${build_dir}/roc_vehicle_driver" --config "$2" "${mode}" ;;
esac
