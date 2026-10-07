#!/usr/bin/env bash
set -euo pipefail

evidence_dir=${ROC_VEHICLE_EVIDENCE_DIR:-/build/v08-driver-evidence}
mkdir -p "${evidence_dir}"
bash /workspace/tools/container-build.sh 2>&1 | tee "${evidence_dir}/full.log"
for compiler in gcc clang; do
    ctest --test-dir "/build/container-${compiler}" -V \
        -R '^(driver_prechecks|driver_joint|driver_process_recovery|driver_context)$' \
        2>&1 | tee "${evidence_dir}/${compiler}-joint.log"
    ctest --test-dir "/build/container-${compiler}" --output-on-failure \
        -R '^(driver_prechecks|driver_joint|driver_process_recovery|driver_context|session_core|device_client_stress)$' \
        --repeat until-fail:10 2>&1 | tee "${evidence_dir}/${compiler}-repeat.log"
done
# The separately authorized local LSan container is required for the next script.
