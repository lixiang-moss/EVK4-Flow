"""CLI and smoke consumer; no model framework or display dependencies."""

import argparse
from collections import deque
import json
import time

import numpy as np

from . import Pipeline, StreamEnded, list_devices


def emit(kind, **values):
    print(json.dumps({"type": kind, **values}, ensure_ascii=False), flush=True)


def summarize(array):
    minimum, maximum = float(array.min()), float(array.max())
    # NumPy min/max propagate NaN and expose either infinity, so these reductions
    # already cover the whole array's finite-value check.
    if not (np.isfinite(minimum) and np.isfinite(maximum)):
        raise ValueError("Non-finite tensor values")
    return {"minimum": minimum, "maximum": maximum,
            "nonzero": int(np.count_nonzero(array != 0))}


def run(args):
    received = checked = 0
    previous = None
    publish_delays, delivery_delays = deque(maxlen=4096), deque(maxlen=4096)
    reason = "stopped"
    pipeline = Pipeline(args.config, input_raw=getattr(args, "input", ""))
    emit("info", **vars(pipeline.info))
    started = time.monotonic()
    next_status = started + 1
    try:
        with pipeline:
            while not args.duration or time.monotonic() - started < args.duration:
                with pipeline.read(timeout_ms=100) as packet:
                    if packet is None:
                        continue
                    meta, array = packet.meta, packet.array
                    if args.smoke:
                        if previous is not None and (
                            meta.sequence_id != previous[0] + 1 or meta.t_start_us != previous[1]
                        ):
                            raise ValueError("Non-contiguous input in continuous-delivery mode")
                        if checked == 0 and (
                            array.shape != pipeline.info.shape or array.dtype != np.float32
                            or not array.flags.c_contiguous or array.flags.writeable
                        ):
                            raise ValueError("Input shape, dtype, layout or read-only flag mismatch")
                        if checked < 20:
                            emit("sample", sequence_id=meta.sequence_id,
                                 t_start_us=meta.t_start_us, t_end_us=meta.t_end_us,
                                 event_count=meta.event_count, **summarize(array))
                            checked += 1
                            if checked == 20:
                                emit("smoke", status="PASS", checked=checked)
                    previous = (meta.sequence_id, meta.t_end_us)
                    publish_delays.append(meta.published_host_us - meta.ready_host_us)
                    delivery_delays.append(meta.received_host_us - meta.published_host_us)
                    received += 1
                if args.samples and received >= args.samples:
                    break
                now = time.monotonic()
                if now >= next_status:
                    emit("status", received=received, checked=checked, **pipeline.stats())
                    next_status = now + 1
    except StreamEnded as end:
        reason = end.reason
    except KeyboardInterrupt:
        reason = "interrupted"
    except Exception as error:
        emit("summary", status="FAIL", reason=str(error), received=received,
             checked=checked, statistics=pipeline.stats())
        return 1
    status = "PASS" if (not args.smoke or checked == 20) else "INCOMPLETE"
    timings = {}
    for name, values in (("publish_us", publish_delays), ("delivery_us", delivery_delays)):
        if values:
            timings[name] = dict(zip(("p50", "p95", "p99", "maximum"),
                                    map(float, np.percentile(values, [50, 95, 99, 100]))))
    emit("summary", status=status, reason=reason, received=received, checked=checked,
         elapsed_s=round(time.monotonic() - started, 3), timings=timings,
         timing_sample_count=len(publish_delays),
         statistics=pipeline.stats())
    return 0 if status == "PASS" else 2


def main():
    parser = argparse.ArgumentParser(description="EVK4-Flow: acquisition → EventCube → consumer")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("devices", help="List camera serial numbers")
    for name in ("run", "replay"):
        command = subparsers.add_parser(name)
        command.add_argument("--config", default="configs/bench.yaml")
        command.add_argument("--smoke", action="store_true", help="Check the first 20 tensors")
        command.add_argument("--samples", type=int, default=0, help="Stop after N tensors; 0 keeps running")
        command.add_argument("--duration", type=float, default=0, help="Stop after N seconds; 0 keeps running")
        if name == "replay":
            command.add_argument("--input", required=True)
    args = parser.parse_args()
    if args.command == "devices":
        devices = list_devices()
        emit("devices", serials=devices)
        return 0 if devices else 2
    if args.samples < 0 or args.duration < 0:
        parser.error("samples and duration must be nonnegative")
    if args.smoke and 0 < args.samples < 20:
        parser.error("--smoke needs at least 20 samples")
    try:
        return run(args)
    except (RuntimeError, ValueError, OSError) as error:
        emit("error", reason=str(error))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
