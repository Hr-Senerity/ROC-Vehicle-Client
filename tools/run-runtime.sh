#!/usr/bin/env bash
set -euo pipefail
# Operator-only launcher: does not upload, SSH, provision credentials or inspect SYSTEM.
[[ $# -ge 3 && $# -le 4 ]] || exit 2
image=$1
root=$2
mode=$3
[[ $image =~ ^roc-vehicle-runtime:local-[0-9a-f]{12}$ && $root =~ ^/opt/roc-vehicle-it/[A-Za-z0-9_-]+$ ]] || exit 2
[[ $mode == --precheck || $mode == --run ]] || exit 2
[[ $# -eq 3 || $4 == --allow-remote ]] || exit 2
[[ -d $root && $(realpath -e "$root") == "$root" ]] || exit 2
[[ $(stat -c '%a %u %g' "$root") == '700 1000 1000' ]] || exit 2
[[ $(docker image inspect "$image" --format '{{.Os}}/{{.Architecture}} {{.Config.User}}') == 'linux/amd64 1000:1000' ]] || exit 2
# Explicit --allow-remote permits outbound only, never port publishing or host networking.
network=none
if [[ $# -eq 4 && $mode == --run ]]; then network=bridge; fi
[[ $(uname -m) == x86_64 ]] || exit 2
if [[ $mode == --run ]]; then
    # Fail closed instead of modifying the host to obtain headroom. Operators
    # must additionally check existing SYSTEM/container usage before running.
    available_kib=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    free_kib=$(df -Pk "$root" | awk 'NR==2 {print $4}')
    [[ $available_kib -ge 1048576 && $free_kib -ge 1048576 ]] || { echo 'runtime_resource_headroom_insufficient' >&2; exit 2; }
fi
exec docker run --rm --name "roc-vehicle-it-$(basename "$root")" \
    --network "$network" --read-only --cap-drop ALL --security-opt no-new-privileges:true \
    --cpus 1 --memory 512m --memory-swap 512m --pids-limit 64 \
    --tmpfs /tmp:rw,nosuid,nodev,noexec,size=16m \
    --mount "type=bind,source=$root,target=/run/vehicle" \
    "$image" --config /run/vehicle/config.json "$mode" "${@:4}"
