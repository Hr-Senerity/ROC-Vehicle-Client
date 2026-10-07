#!/usr/bin/env bash
set -euo pipefail

# Run only in the dedicated dev container. SYS_PTRACE is required by LSan in
# this runtime; do not change compose or install anything on the host/WSL.
build_dir=/build/v08-clang-sanitizers
evidence_dir=/build/v08-g1-g5-evidence
mkdir -p "${evidence_dir}"
cmake -S /workspace -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DROC_VEHICLE_WARNINGS_AS_ERRORS=ON \
    -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build "${build_dir}" --parallel 2 --target \
    roc_vehicle_sys001_contract_tests roc_vehicle_protocol_tests \
    roc_vehicle_state_store_tests roc_vehicle_task_protocol_tests \
    roc_vehicle_task_http_client_tests roc_vehicle_road_network_tests \
    roc_vehicle_session_core_tests roc_vehicle_package_tests roc_vehicle_package_http_tests \
    roc_vehicle_file_sink_tests roc_vehicle_road_v2_tests roc_vehicle_road_http_tests \
    roc_vehicle_artifact_receiver_tests roc_vehicle_storage_security_tests roc_vehicle_v08_network_fault_tests
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir "${build_dir}" --output-on-failure \
    -R '^(sys001_contract|protocol|state_store|task_protocol|task_http_client|road_network|session_core|package|package_http|file_sink|road_v2|road_v2_limits|road_http|artifact_receiver|storage_security|v08_network_faults)$' \
    2>&1 | tee "${evidence_dir}/sanitizers.log"
