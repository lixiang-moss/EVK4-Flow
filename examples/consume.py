"""Replace consume() with the partner's model adapter; no display required."""

from evk4_pipeline import Pipeline, StreamEnded


def consume(array, meta):
    # Add batch/framework/device conversion here if required by your model.
    # Complete all reads before leaving pipeline.read(), or copy the array.
    print(meta.sequence_id, array.shape, meta.event_count)


with Pipeline(config_path="configs/bench.yaml") as pipeline:
    print(vars(pipeline.info))
    try:
        while True:
            with pipeline.read(timeout_ms=1000) as packet:
                if packet is not None:
                    consume(packet.array, packet.meta)
    except StreamEnded as end:
        print("Stream ended:", end.reason)
