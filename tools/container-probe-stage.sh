#!/usr/bin/env bash
set -euo pipefail
# TEST-ONLY scratch payload. Exact committed source, never retags Release.
[[ $# == 1 && $1 =~ ^[0-9a-f]{40}$ ]] || exit 2
revision=$1; short=${revision:0:12}
[[ $(git -c safe.directory=/workspace -C /workspace rev-parse HEAD) == "$revision" ]] || exit 2
bash /workspace/tools/runtime-source-check.sh /workspace
source_dir=/build/probe-source-$short
build_dir=/build/probe-release-$short
stage_dir=/build/probe-context-$short
[[ ! -e $source_dir && ! -e $stage_dir ]] || exit 2
install -d "$source_dir"
git -c safe.directory=/workspace -C /workspace archive "$revision" | tar -x -C "$source_dir"
cmake -S "$source_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DROC_VEHICLE_BUILD_DRIVER=ON -DROC_VEHICLE_BUILD_TESTS=ON \
 -DROC_VEHICLE_BUILD_EXAMPLES=OFF -DROC_VEHICLE_WARNINGS_AS_ERRORS=ON
cmake --build "$build_dir" --parallel 2 --target roc_vehicle_driver_crash_probe
rootfs=$stage_dir/rootfs
install -d "$rootfs/opt/roc-vehicle" "$rootfs/etc/ssl/certs" "$rootfs/usr/share/doc/roc-vehicle-probe"
install -m 0755 "$build_dir/roc_vehicle_driver_crash_probe" "$rootfs/opt/roc-vehicle/roc_vehicle_driver_crash_probe"
strip --strip-unneeded "$rootfs/opt/roc-vehicle/roc_vehicle_driver_crash_probe"
ldd "$rootfs/opt/roc-vehicle/roc_vehicle_driver_crash_probe" | tee "$stage_dir/dynamic-dependencies.txt"
! grep -q 'not found' "$stage_dir/dynamic-dependencies.txt"
mapfile -t libraries < <(awk '/=>/ {if ($3 ~ "^/") print $3} {if ($1 ~ "^/") print $1}' "$stage_dir/dynamic-dependencies.txt")
[[ ${#libraries[@]} -gt 0 ]] || exit 2
for library in "${libraries[@]}"; do [[ $library == /* && -f $library ]] || exit 2; cp -L --parents "$library" "$rootfs"; done
install -m 0644 /etc/ssl/certs/ca-certificates.crt "$rootfs/etc/ssl/certs/ca-certificates.crt"
for package in libc6 libstdc++6 libgcc-s1 gcc-11-base; do cp "/usr/share/doc/$package/copyright" "$rootfs/usr/share/doc/roc-vehicle-probe/$package.copyright"; done
cp -L --parents /usr/share/common-licenses/LGPL-2.1 /usr/share/common-licenses/GPL-3 "$rootfs"
cp "$source_dir/LICENSE" "$rootfs/usr/share/doc/roc-vehicle-probe/VEHICLE-LICENSE"
# This is a local test payload, not a binary redistribution clearance.
cp "$source_dir/docker/probe/Dockerfile" "$stage_dir/Dockerfile"
sha256sum "$rootfs/opt/roc-vehicle/roc_vehicle_driver_crash_probe" "$rootfs/etc/ssl/certs/ca-certificates.crt" | tee "$stage_dir/payload-sha256.txt"
echo "TEST_ONLY_SOURCE=$revision"
