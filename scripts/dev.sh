#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
command=${1:---help}
if (($#)); then shift; fi

case "$command" in
  devices|run|replay|test|exec) ;;
  *)
    cat <<'HELP'
Usage: scripts/dev.sh devices
       scripts/dev.sh run --config configs/bench.yaml --smoke --samples 20
       scripts/dev.sh replay --input /data/sample.raw --config configs/bench.yaml --smoke
       scripts/dev.sh test
       scripts/dev.sh exec COMMAND [ARGS...]

EVK4_USB_DEVICE selects the EVK4 USB node; otherwise it is found in sysfs.
EVK4_DATA_DIR optionally mounts a host data directory read-only at /data.
HELP
    exit 0
    ;;
esac

docker_args=(run --rm --init --user "$(id -u):$(id -g)" --workdir /workspace
  --mount "type=bind,source=$project_dir,target=/workspace")
if [[ -t 0 && -t 1 ]]; then docker_args+=(-it); fi
if [[ -n ${EVK4_DATA_DIR:-} ]]; then
  docker_args+=(--mount "type=bind,source=$EVK4_DATA_DIR,target=/data,readonly")
fi

if [[ "$command" == devices || "$command" == run || ( "$command" == exec && -n ${EVK4_USB_DEVICE:-} ) ]]; then
  usb_device=${EVK4_USB_DEVICE:-}
  if [[ -z "$usb_device" ]]; then
    for usb_path in /sys/bus/usb/devices/*; do
      [[ -f "$usb_path/idVendor" && -f "$usb_path/idProduct" ]] || continue
      if [[ $(<"$usb_path/idVendor") == 04b4 && $(<"$usb_path/idProduct") == 00f5 ]]; then
        printf -v usb_device '/dev/bus/usb/%03d/%03d' "$(<"$usb_path/busnum")" "$(<"$usb_path/devnum")"
        break
      fi
    done
  fi
  if [[ -n "$usb_device" ]]; then
    docker_args+=(--device "$usb_device:$usb_device")
  elif [[ "$command" == run ]]; then
    echo 'EVK4 USB device not found. Connect the camera or set EVK4_USB_DEVICE.' >&2
    exit 1
  fi
fi

case "$command" in
  test) app_command=(ctest --test-dir build --output-on-failure "$@") ;;
  exec) app_command=("$@") ;;
  *) app_command=(python3 -m evk4_pipeline "$command" "$@") ;;
esac
exec docker "${docker_args[@]}" "${EVK4_DEV_IMAGE:-evk4-flow:dev}" "${app_command[@]}"
