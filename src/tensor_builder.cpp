#include "evk4_flow/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <metavision/sdk/core/algorithms/event_buffer_reslicer_algorithm.h>
#include <metavision/sdk/core/preprocessors/event_cube_processor.h>

namespace evk4_flow {
namespace {
using Processor = Metavision::EventCubeProcessor<const EventInput *>;
using Reslicer = Metavision::EventBufferReslicerAlgorithm;
constexpr std::size_t mapping_block_events = 4096;

std::int64_t host_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void validate_tensor(const Config &c, int source_width, int source_height) {
    if (c.window_us <= 0 || c.bins <= 0 || !std::isfinite(c.max_incr_per_pixel) ||
        c.max_incr_per_pixel <= 0 || !std::isfinite(c.clip_value) || c.clip_value <= 0)
        throw PipelineError("window_us, bins, max_incr_per_pixel and clip_value must be positive");
    if (source_width <= 0 || source_height <= 0 || c.width <= 0 || c.height <= 0 ||
        !((c.width == source_width && c.height == source_height) ||
          (2 * c.width == source_width && 2 * c.height == source_height)))
        throw PipelineError("tensor output must use full source size or half width and height");
}

std::unique_ptr<Processor> make_processor(const Config &c, int sw, int sh) {
    validate_tensor(c, sw, sh);
    return std::make_unique<Processor>(c.window_us, c.width, c.height, c.bins, c.split_polarity,
        c.max_incr_per_pixel, c.clip_value,
        c.spatial_area ? static_cast<float>(c.width) / sw : 1.f,
        c.spatial_area ? static_cast<float>(c.height) / sh : 1.f);
}

EventInput map_event(const EventInput &event, int sw, int sh, int divisor, std::int64_t origin) {
    if (event.x >= sw || event.y >= sh || (event.p != 0 && event.p != 1))
        throw PipelineError("decoded event coordinate or polarity is outside the source format");
    return EventInput(event.x / divisor, event.y / divisor, event.p, event.t - origin);
}
} // namespace

struct TensorBuilder::Impl {
    Config config;
    int source_width;
    int source_height;
    int divisor;
    OnWindow on_window;
    std::unique_ptr<Processor> processor;
    Reslicer reslicer;
    std::shared_ptr<std::vector<float>> output;
    Metavision::Tensor tensor;
    std::array<EventInput, mapping_block_events> mapped;
    std::int64_t origin = -1;
    std::int64_t last_time = -1;
    std::int64_t frame_start = 0;
    std::int64_t source_ready_host_us = 0;
    std::uint64_t sequence = 0;
    bool finished = false;

    Impl(const Config &c, int sw, int sh, OnWindow callback) :
        config(c), source_width(sw), source_height(sh), divisor(1),
        on_window(std::move(callback)), processor(make_processor(c, sw, sh)),
        reslicer([this](Reslicer::ConditionStatus, Metavision::timestamp end, std::size_t count) {
            PacketMeta meta;
            meta.sequence_id = sequence++;
            meta.start_us = origin + frame_start;
            meta.end_us = origin + end;
            meta.event_count = count;
            meta.ready_host_us = source_ready_host_us;
            auto previous_output = output;
            on_window(meta);
            frame_start = end;
            if (output == previous_output)
                std::fill(output->begin(), output->end(), 0.f);
        }, Reslicer::Condition::make_n_us(c.window_us)) { divisor = sw / c.width; }

    void observe(std::int64_t t) {
        if (t < last_time)
            throw StreamEnded("time_reset");
        if (origin < 0)
            origin = t;
        last_time = t;
    }
};

TensorBuilder::TensorBuilder(const Config &config, int sw, int sh, OnWindow callback) :
    impl_(std::make_unique<Impl>(config, sw, sh, std::move(callback))) {}
TensorBuilder::~TensorBuilder() = default;

void TensorBuilder::bind_output(const std::shared_ptr<std::vector<float>> &output) {
    if (!output || output->size() != tensor_size())
        throw PipelineError("builder output buffer has the wrong size");
    impl_->output = output;
    std::fill(output->begin(), output->end(), 0.f);
    impl_->tensor.create(impl_->processor->get_output_shape(), Metavision::BaseType::FLOAT32,
                         reinterpret_cast<std::byte *>(output->data()), false);
}

std::size_t TensorBuilder::tensor_size() const {
    return impl_->processor->get_output_shape().get_nb_values();
}

void TensorBuilder::process(const EventInput *begin, const EventInput *end, std::int64_t ready_host_us) {
    auto &s = *impl_;
    s.source_ready_host_us = ready_host_us ? ready_host_us : host_us();
    if (s.finished)
        throw PipelineError("builder already finished");
    if (!s.output)
        throw PipelineError("builder has no output buffer");
    while (begin != end) {
        const auto count = std::min<std::size_t>(end - begin, mapping_block_events);
        for (std::size_t i = 0; i < count; ++i) {
            s.observe(begin[i].t);
            s.mapped[i] = map_event(begin[i], s.source_width, s.source_height, s.divisor, s.origin);
        }
        s.reslicer.process_events(s.mapped.data(), s.mapped.data() + count,
            [&s](const EventInput *first, const EventInput *last) {
                s.processor->process_events(s.frame_start, first, last, s.tensor);
            });
        begin += count;
    }
}

void TensorBuilder::progress(std::int64_t source_time_us, std::int64_t ready_host_us) {
    auto &s = *impl_;
    s.source_ready_host_us = ready_host_us ? ready_host_us : host_us();
    if (source_time_us < 0)
        return; // SDK has not established its first source timestamp yet.
    if (s.finished)
        throw PipelineError("builder already finished");
    if (!s.output)
        throw PipelineError("builder has no output buffer");
    s.observe(source_time_us);
    // SDK progress runs after its CD forwarders flush. All events preceding t
    // have been seen. The SDK reslicer uses '< ts', hence +1 for microsecond time.
    s.reslicer.notify_elapsed_time(source_time_us - s.origin + 1);
}

void TensorBuilder::finish() {
    impl_->finished = true; // Deliberately do not call reslicer.flush().
}

} // namespace evk4_flow
