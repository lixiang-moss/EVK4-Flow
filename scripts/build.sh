#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dev_image=${EVK4_DEV_IMAGE:-evk4-stream:dev}
mode=${1:-dev}

case "$mode" in
  dev|--image-only)
    docker build --target dev -f "$project_dir/docker/Dockerfile" \
      --build-arg "OPENEB_IMAGE=${OPENEB_IMAGE:-hppr:capture-jammy}" \
      -t "$dev_image" "$project_dir"
    if [[ "$mode" == dev ]]; then
      "$project_dir/scripts/dev.sh" exec cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
      "$project_dir/scripts/dev.sh" exec cmake --build build --parallel "${EVK4_BUILD_JOBS:-2}"
    fi
    ;;
  --runtime)
    docker build --target runtime -f "$project_dir/docker/Dockerfile" \
      --build-arg "OPENEB_IMAGE=${OPENEB_IMAGE:-hppr:capture-jammy}" \
      -t "${EVK4_RUNTIME_IMAGE:-evk4-stream:runtime}" "$project_dir"
    ;;
  *)
    echo 'Usage: scripts/build.sh [dev | --image-only | --runtime]' >&2
    exit 2
    ;;
esac
