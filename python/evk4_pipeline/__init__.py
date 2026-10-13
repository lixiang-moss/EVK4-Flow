"""In-process EVK4 acquisition and EventCube delivery."""

from pathlib import Path
from types import SimpleNamespace

import yaml

import _evk4_pipeline as _native

PipelineError = _native.PipelineError
list_devices = _native.list_devices


class StreamEnded(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def _load_config(path):
    data = yaml.safe_load(Path(path).read_text())
    camera = data.get("camera", {})
    tensor = data.get("tensor", {})
    runtime = data.get("runtime", {})
    if tensor.get("representation", "openeb_event_cube") != "openeb_event_cube":
        raise ValueError("This version implements only openeb_event_cube")
    normalization = tensor.get("spatial_normalization", "none")
    if normalization not in ("none", "area"):
        raise ValueError("spatial_normalization must be none or area")
    config = _native.Config()
    config.serial = camera.get("serial", "")
    config.biases = camera.get("biases", {})
    config.roi = camera.get("roi")
    config.width, config.height = tensor.get("output_size", [1280, 720])
    for key in ("window_us", "bins", "split_polarity", "max_incr_per_pixel", "clip_value"):
        if key in tensor:
            setattr(config, key, tensor[key])
    config.spatial_area = normalization == "area"
    for key in ("event_queue_mib", "max_backlog_us", "max_tensor_wait_us"):
        if key in runtime:
            setattr(config, key, runtime[key])
    actual_tensor = {
        "representation": "openeb_event_cube", "window_us": config.window_us,
        "bins": config.bins, "split_polarity": config.split_polarity,
        "output_size": [config.width, config.height],
        "max_incr_per_pixel": config.max_incr_per_pixel,
        "clip_value": config.clip_value, "spatial_normalization": normalization,
    }
    return config, actual_tensor


class _ReadContext:
    def __init__(self, pipeline, timeout_ms):
        self.pipeline = pipeline
        self.timeout_ms = timeout_ms
        self.packet = None

    def __enter__(self):
        try:
            native_packet = self.pipeline._native.read(self.timeout_ms)
        except _native.StreamEnded as end:
            raise StreamEnded(str(end)) from None
        if native_packet is None:
            return None
        self.packet = SimpleNamespace(
            array=native_packet.array(self.pipeline.info.shape), meta=native_packet.meta
        )
        return self.packet

    def __exit__(self, *_):
        if self.packet is not None:
            self.pipeline._native.release()
            self.packet.array = None
            self.packet = None


class Pipeline:
    """One source and one consumer. Arrays are borrowed only inside read()."""

    def __init__(self, config_path="configs/bench.yaml", *, input_raw=None):
        config, tensor_config = _load_config(config_path)
        self._native = _native.Pipeline(config, "" if input_raw is None else str(input_raw))
        self.info = SimpleNamespace(**self._native.info(), tensor_config=tensor_config)

    def __enter__(self):
        self._native.start()
        return self

    def __exit__(self, *_):
        self.stop()

    def read(self, timeout_ms=1000):
        return _ReadContext(self, timeout_ms)

    def stop(self):
        self._native.stop()

    def stats(self):
        return self._native.stats()


__all__ = ["Pipeline", "PipelineError", "StreamEnded", "list_devices"]
