#!/usr/bin/env bash
set -euo pipefail
# Explicit local-engine integration entry point. Never changes the server guard
# or dev compose, provisions credentials, or accepts secrets on the command line.
[[ $# == 3 ]] || exit 2
image=$1
root=$2
mode=$3
[[ $image =~ ^roc-vehicle-runtime:local-[0-9a-f]{12}$ ]] || exit 2
[[ $root =~ ^/opt/roc-vehicle-it/[A-Za-z0-9_-]+$ ]] || exit 2
[[ $mode == --precheck || $mode == --run || $mode == --run-detached ]] || exit 2
[[ $(docker context inspect --format '{{.Endpoints.docker.Host}}') == unix://* ]] || { echo local_unix_engine_required >&2; exit 2; }
[[ $(docker info --format '{{.OSType}}') == linux && $(uname -m) == x86_64 ]] || exit 2
[[ -d $root && $(realpath -e "$root") == "$root" ]] || exit 2
[[ $(stat -c '%a %u %g' "$root") == '700 1000 1000' ]] || exit 2
[[ $(docker image inspect "$image" --format '{{.Os}}/{{.Architecture}} {{.Config.User}}') == 'linux/amd64 1000:1000' ]] || exit 2
available_kib=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
free_kib=$(df -Pk "$root" | awk 'NR==2 {print $4}')
[[ $available_kib -ge 1048576 && $free_kib -ge 1048576 ]] || { echo local_runtime_resource_headroom_insufficient >&2; exit 2; }
network=none
driver_mode=--precheck
extra=(--rm)
if [[ $mode != --precheck ]]; then network=bridge; driver_mode=--run; fi
if [[ $mode == --run-detached ]]; then extra=(--detach); fi
exec docker run "${extra[@]}" --name "roc-vehicle-it-$(basename "$root")" \
    --label roc.vehicle.integration=local-client --network "$network" \
    --read-only --cap-drop ALL --security-opt no-new-privileges:true \
    --cpus 1 --memory 512m --memory-swap 512m --pids-limit 64 \
    --tmpfs /tmp:rw,nosuid,nodev,noexec,size=16m \
    --mount "type=bind,source=$root,target=/run/vehicle" \
    "$image" --config /run/vehicle/config.json "$driver_mode" --allow-remote
