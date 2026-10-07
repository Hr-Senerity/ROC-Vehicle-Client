#!/usr/bin/env bash
set -euo pipefail
# Execute in WSL from this clean repository. Local build/export/smoke only;
# no server, registry, SYSTEM volume, port publishing or external network.
repo=$(pwd -P)
[[ -f $repo/compose.yaml && -f $repo/tools/container-runtime-stage.sh ]] || exit 2
revision=$(git -c safe.directory="$repo" rev-parse HEAD)
bash tools/runtime-source-check.sh "$repo"
resume=false
if [[ $# -gt 0 ]]; then
    [[ $# -eq 2 && $1 == --resume-stage && $2 =~ ^[0-9a-f]{40}$ ]] || exit 2
    revision=$2; resume=true
    # Resume only completed, checksum-verifiable local staging after a packaging
    # tool interruption, and only if all program/image inputs are unchanged.
    git -c safe.directory="$repo" diff --quiet "$revision" HEAD -- apps src include CMakeLists.txt LICENSE \
        docker/runtime tests/integration/runtime_inputs.cpp || exit 2
fi
short=${revision:0:12}
artifacts=$repo/out/server-prep/$short
if [[ $resume == false ]]; then
    [[ ! -e $artifacts ]] || { echo 'export exists; preserve its evidence instead of overwriting' >&2; exit 2; }
    mkdir -p "$artifacts"
    docker image inspect roc-vehicle-client-dev:ubuntu22.04-v1 \
        --format '{{.Id}} {{.Os}}/{{.Architecture}}' | tee "$artifacts/dev-image.txt"
    docker compose run --rm dev bash /workspace/tools/container-runtime-stage.sh "$revision" \
        2>&1 | tee "$artifacts/release-build.log"
else
    [[ -d $artifacts && ! -e $artifacts/context && ! -e $artifacts/runtime-image.txt ]] || exit 2
    [[ $(docker image inspect roc-vehicle-client-dev:ubuntu22.04-v1 --format '{{.Id}} {{.Os}}/{{.Architecture}}') == $(<"$artifacts/dev-image.txt") ]] || exit 2
    docker compose run --rm dev bash -lc \
        "sha256sum -c /build/runtime-context-$short/payload-sha256.txt && readelf -h /build/runtime-context-$short/rootfs/opt/roc-vehicle/roc_vehicle_driver" \
        2>&1 | tee "$artifacts/stage-resume.log"
fi
# Only this generated-output directory gains a writable extra mount.
docker compose run --rm -v "$artifacts:/export:rw" dev bash -lc \
    "cp -a /build/runtime-context-$short /export/context" 2>&1 | tee "$artifacts/stage-copy.log"
image=roc-vehicle-runtime:local-$short
docker build --network none --pull=false --build-arg "VEHICLE_REVISION=$revision" \
    -t "$image" "$artifacts/context" 2>&1 | tee "$artifacts/image-build.log"
docker image inspect "$image" --format '{{.Id}} {{.Os}}/{{.Architecture}} {{.Size}} {{.Config.User}} {{index .Config.Labels "org.opencontainers.image.revision"}}' \
    | tee "$artifacts/runtime-image.txt"
[[ $(docker image inspect "$image" --format '{{.Os}}/{{.Architecture}} {{.Config.User}}') == 'linux/amd64 1000:1000' ]] || exit 2
docker compose run --rm dev bash -lc \
    "cmake --build /build/container-gcc --parallel 2 --target roc_vehicle_runtime_inputs && install -d -m 0700 /build/runtime-smoke-$short && /build/container-gcc/roc_vehicle_runtime_inputs --create /build/runtime-smoke-$short" \
    2>&1 | tee "$artifacts/smoke-inputs.log"
common=(--network none --read-only --cap-drop ALL --security-opt no-new-privileges:true \
    --cpus 1 --memory 512m --memory-swap 512m --pids-limit 64 \
    --tmpfs /tmp:rw,nosuid,nodev,noexec,size=16m \
    --mount "type=volume,source=roc-vehicle-client-build,target=/run/vehicle,volume-subpath=runtime-smoke-$short")
docker run --rm "${common[@]}" "$image" --help | tee "$artifacts/runtime-help.log"
docker run --rm "${common[@]}" "$image" --config /run/vehicle/config.json --precheck | tee "$artifacts/runtime-precheck.log"
name=roc-vehicle-runtime-smoke-$short
# Keep exactly our own test container ID; cleanup never enumerates others.
container=$(docker run -d --name "$name" "${common[@]}" "$image" --config /run/vehicle/config.json --run)
cleanup() { docker rm -f "$container" >/dev/null 2>&1 || true; }
trap cleanup EXIT
ready=false
for ((i=0;i<60;i++)); do
    if docker logs "$container" 2>&1 | grep -q 'ws_backoff'; then ready=true; break; fi
    sleep 0.1
done
[[ $ready == true ]] || exit 2
set +e
docker run --rm "${common[@]}" "$image" --config /run/vehicle/config.json --run >"$artifacts/runtime-lock.log" 2>&1
competitor=$?
set -e
[[ $competitor -eq 3 ]] && grep -q root_in_use "$artifacts/runtime-lock.log"
docker inspect "$container" --format 'read_only={{.HostConfig.ReadonlyRootfs}} network={{.HostConfig.NetworkMode}} memory={{.HostConfig.Memory}} swap={{.HostConfig.MemorySwap}} nanocpus={{.HostConfig.NanoCpus}} pids={{.HostConfig.PidsLimit}} ports={{json .HostConfig.PortBindings}} caps={{json .HostConfig.CapDrop}} security={{json .HostConfig.SecurityOpt}}' \
    | tee "$artifacts/runtime-budget.log"
docker stop --time 10 "$container" >/dev/null
[[ $(docker inspect "$container" --format '{{.State.ExitCode}} {{.State.OOMKilled}}') == '0 false' ]] || exit 2
docker logs "$container" 2>&1 | tee "$artifacts/runtime-stop.log"
docker compose run --rm dev /build/container-gcc/roc_vehicle_runtime_inputs --verify "/build/runtime-smoke-$short" \
    2>&1 | tee "$artifacts/runtime-permissions.log"
docker run --rm "${common[@]}" "$image" --config /run/vehicle/config.json --precheck | tee "$artifacts/runtime-reopen.log"
cleanup; trap - EXIT
docker save --output "$artifacts/roc-vehicle-runtime-linux-amd64.tar" "$image"
(cd "$artifacts" && sha256sum roc-vehicle-runtime-linux-amd64.tar *.log *-image.txt context/payload-sha256.txt context/dynamic-dependencies.txt) \
    | tee "$artifacts/SHA256SUMS"
echo 'S2 local runtime export and smoke passed; server preflight/transfer/run NOT executed'
