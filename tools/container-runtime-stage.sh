#!/usr/bin/env bash
set -euo pipefail
# INSIDE the pinned network-none dev image. Never install dependencies.
[[ $# -eq 1 && $1 =~ ^[0-9a-f]{40}$ ]] || exit 2
revision=$1
[[ $(git -c safe.directory=/workspace -C /workspace rev-parse HEAD) == "$revision" ]] || exit 2
bash /workspace/tools/runtime-source-check.sh /workspace
build_dir=/build/runtime-release-${revision:0:12}
stage_dir=/build/runtime-context-${revision:0:12}
[[ ! -e $stage_dir ]] || { echo 'runtime staging already exists; use its existing evidence or a new commit' >&2; exit 2; }
source_dir=/build/runtime-source-${revision:0:12}
[[ ! -e $source_dir ]] || exit 2
install -d "$source_dir"
git -c safe.directory=/workspace -C /workspace archive "$revision" | tar -x -C "$source_dir"
echo 'runtime source = exact Git commit archive'
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DROC_VEHICLE_BUILD_DRIVER=ON -DROC_VEHICLE_BUILD_TESTS=OFF \
    -DROC_VEHICLE_BUILD_EXAMPLES=OFF -DROC_VEHICLE_WARNINGS_AS_ERRORS=ON
cmake --build "$build_dir" --parallel 2 --target roc_vehicle_driver
rootfs=$stage_dir/rootfs
install -d "$rootfs/opt/roc-vehicle" "$rootfs/etc/ssl/certs" "$rootfs/usr/share/doc/roc-vehicle-runtime"
install -m 0755 "$build_dir/roc_vehicle_driver" "$rootfs/opt/roc-vehicle/roc_vehicle_driver"
strip --strip-unneeded "$rootfs/opt/roc-vehicle/roc_vehicle_driver"
# Only ldd our own just-built executable. Copy the full loader/library closure,
# dereferencing symlinks so the scratch image cannot inherit a missing target.
ldd "$rootfs/opt/roc-vehicle/roc_vehicle_driver" | tee "$stage_dir/dynamic-dependencies.txt"
! grep -q 'not found' "$stage_dir/dynamic-dependencies.txt"
mapfile -t libraries < <(awk '/=>/ {if ($3 ~ "^/") print $3} {if ($1 ~ "^/") print $1}' "$stage_dir/dynamic-dependencies.txt")
[[ ${#libraries[@]} -gt 0 ]] || exit 2
for library in "${libraries[@]}"; do
    [[ $library == /* && -f $library ]] || exit 2
    cp -L --parents "$library" "$rootfs"
done
install -m 0644 /etc/ssl/certs/ca-certificates.crt "$rootfs/etc/ssl/certs/ca-certificates.crt"
for package in libc6 libstdc++6 libgcc-s1 gcc-11-base; do
    cp "/usr/share/doc/$package/copyright" "$rootfs/usr/share/doc/roc-vehicle-runtime/$package.copyright"
done
cp -L --parents /usr/share/common-licenses/LGPL-2.1 /usr/share/common-licenses/GPL-3 "$rootfs"
cp "$source_dir/LICENSE" "$rootfs/usr/share/doc/roc-vehicle-runtime/VEHICLE-LICENSE"
# Local test payload only. External binary redistribution needs a separate
# dependency license/source review; this staging tool does not grant clearance.
cp "$source_dir/docker/runtime/Dockerfile" "$stage_dir/Dockerfile"
sha256sum "$rootfs/opt/roc-vehicle/roc_vehicle_driver" "$rootfs/etc/ssl/certs/ca-certificates.crt" | tee "$stage_dir/payload-sha256.txt"
readelf -h "$rootfs/opt/roc-vehicle/roc_vehicle_driver"
echo "vehicle_revision=$revision system_revision=a9c601d6977d89000438854a24e3199573004a2f"
