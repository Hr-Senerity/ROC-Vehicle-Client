#!/usr/bin/env bash
set -euo pipefail
repo=$(pwd -P)
revision=$(git -c safe.directory="$repo" rev-parse HEAD);short=${revision:0:12}
bash tools/runtime-source-check.sh "$repo"
artifacts=$repo/out/probe-prep/$short
[[ ! -e $artifacts ]] || exit 2
mkdir -p "$artifacts"
docker compose run --rm dev bash /workspace/tools/container-probe-stage.sh "$revision" 2>&1 | tee "$artifacts/build.log"
docker compose run --rm -v "$artifacts:/export:rw" dev cp -a "/build/probe-context-$short" /export/context
image=roc-vehicle-test-probe:local-$short
docker build --network none --pull=false --build-arg "VEHICLE_REVISION=$revision" -t "$image" "$artifacts/context" 2>&1 | tee "$artifacts/image.log"
docker image inspect "$image" --format '{{.Id}} {{.Os}}/{{.Architecture}} {{.Config.User}} {{index .Config.Labels "org.opencontainers.image.revision"}} {{index .Config.Labels "org.roc.vehicle.test-instrumentation"}}' | tee "$artifacts/identity.txt"
docker run --rm --network none --read-only --cap-drop ALL --security-opt no-new-privileges:true \
 --cpus 1 --memory 512m --memory-swap 512m --pids-limit 64 \
 --tmpfs /tmp:rw,nosuid,nodev,noexec,size=16m "$image" --help | tee "$artifacts/help.log"
docker save --output "$artifacts/roc-vehicle-test-probe-linux-amd64.tar" "$image"
(cd "$artifacts" && sha256sum roc-vehicle-test-probe-linux-amd64.tar context/payload-sha256.txt identity.txt *.log) | tee "$artifacts/SHA256SUMS"
