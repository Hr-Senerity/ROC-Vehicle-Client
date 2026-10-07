#!/usr/bin/env bash
set -euo pipefail

# Only the existing project container; no network, credentials or host installs.
evidence_dir=/build/v08-g1-g5-evidence
mkdir -p "${evidence_dir}"
bash /workspace/tools/container-build.sh 2>&1 | tee "${evidence_dir}/full.log"
for compiler in gcc clang; do
    ctest --test-dir "/build/container-${compiler}" --output-on-failure \
        -R '^(v08_network_faults|v08_wire_recovery|v08_state_faults|v08_delivery_recovery|storage_security|file_sink_faults)$' \
        --repeat until-fail:25 2>&1 | tee "${evidence_dir}/${compiler}-repeat.log"
    ctest --test-dir "/build/container-${compiler}" -V \
        -R '^(v08_network_faults|v08_wire_recovery)$' 2>&1 \
        | tee "${evidence_dir}/${compiler}-wire.log"
done
