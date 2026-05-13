#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

image="${SCREEN_HOPPER_TOOLCHAIN_IMAGE:-screenhopper-gcc10}"
platform="${SCREEN_HOPPER_DOCKER_PLATFORM:-linux/amd64}"
build_dir="${SCREEN_HOPPER_BUILD_DIR:-firmware/build}"

docker build --platform "$platform" -t "$image" -f "$script_dir/Dockerfile.gcc10" "$script_dir"

docker run --rm --platform "$platform" \
    -v "$repo_root:/work" \
    -w /work \
    "$image" \
    bash -lc '
        set -euo pipefail
        output_dir="$1"
        shift
        build_dir=/tmp/screen-hopper-build
        rm -rf "$build_dir"
        cmake -S firmware -B "$build_dir"
        if [ "$#" -gt 0 ]; then
            cmake --build "$build_dir" --target "$@"
        else
            cmake --build "$build_dir"
        fi
        mkdir -p "$output_dir"
        find "$build_dir" -maxdepth 1 -type f \( \
            -name "*.bin" -o \
            -name "*.dis" -o \
            -name "*.elf" -o \
            -name "*.elf.map" -o \
            -name "*.hex" -o \
            -name "*.uf2" \
        \) -exec cp {} "$output_dir/" \;
    ' _ "$build_dir" "$@"
