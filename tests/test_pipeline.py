"""Small RAW integration tests; hardware is exercised separately by the CLI."""

import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

import numpy as np

from evk4_pipeline import Pipeline, PipelineError, StreamEnded
from evk4_pipeline.__main__ import summarize


class SmokeSummaryTests(unittest.TestCase):
    def test_summary_reads_values_and_accepts_zero(self):
        self.assertEqual(summarize(np.zeros(5, dtype=np.float32)),
                         {"minimum": 0.0, "maximum": 0.0, "nonzero": 0})
        self.assertEqual(summarize(np.array([0, -2, 3], dtype=np.float32)),
                         {"minimum": -2.0, "maximum": 3.0, "nonzero": 2})

    def test_summary_rejects_nan_and_infinities(self):
        for value in (np.nan, np.inf, -np.inf):
            with self.subTest(value=value), self.assertRaises(ValueError):
                summarize(np.array([0, value, 1], dtype=np.float32))


class PipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="pipeline-test-", dir="build")
        cls.root = Path(cls.directory.name)
        cls.raw = cls.root / "events.raw"
        subprocess.run(["build/make_fixture", str(cls.raw)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def config(self, **runtime):
        path = self.root / "config.yaml"
        path.write_text(json.dumps({
            "camera": {"biases": {"not_applied_to_raw": 123}},
            "tensor": {"output_size": [640, 360]},
            "runtime": {"event_queue_mib": 4, "max_backlog_us": 2000000,
                        "max_tensor_wait_us": 2000000, **runtime},
        }))
        return path

    def test_raw_continuity_values_and_eof(self):
        sequences = []
        with Pipeline(self.config(), input_raw=self.raw) as pipeline:
            self.assertEqual(pipeline.info.shape, (10, 360, 640))
            while True:
                try:
                    with pipeline.read(1000) as packet:
                        self.assertIsNotNone(packet)
                        seq = packet.meta.sequence_id
                        sequences.append(seq)
                        self.assertEqual(packet.meta.t_start_us, seq * 20000)
                        self.assertEqual(packet.meta.t_end_us, (seq + 1) * 20000)
                        self.assertEqual(packet.meta.event_count, 3)
                        self.assertAlmostEqual(float(packet.array.sum()), 0.5, places=5)
                        self.assertFalse(packet.array.flags.writeable)
                        self.assertTrue(packet.array.flags.c_contiguous)
                except StreamEnded as end:
                    self.assertEqual(end.reason, "eof")
                    break
        self.assertEqual(sequences, list(range(50)))
        self.assertEqual(pipeline.stats()["delivered_tensors"], 50)

    def test_held_array_survives_overload_and_stop(self):
        with Pipeline(self.config(), input_raw=self.raw) as pipeline:
            with pipeline.read(1000) as packet:
                self.assertIsNotNone(packet)
                array = packet.array
                expected = array.copy()
                # A pending second window cannot be replaced by a third one.
                time.sleep(0.15)
                pipeline.stop()
                np.testing.assert_array_equal(array, expected)
            with self.assertRaises(PipelineError):
                with pipeline.read(100):
                    pass

    def test_stale_pending_input_is_rejected(self):
        with Pipeline(self.config(max_tensor_wait_us=5000), input_raw=self.raw) as pipeline:
            time.sleep(0.04)
            with self.assertRaises(PipelineError):
                with pipeline.read(100):
                    pass

    def test_explicit_stop_reports_normal_end(self):
        with Pipeline(self.config(), input_raw=self.raw) as pipeline:
            pipeline.stop()
            with self.assertRaises(StreamEnded) as ended:
                with pipeline.read(100):
                    pass
            self.assertEqual(ended.exception.reason, "stopped")


if __name__ == "__main__":
    unittest.main()
