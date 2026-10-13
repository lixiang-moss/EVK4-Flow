#include "evk4_stream/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <metavision/hal/facilities/i_events_stream_decoder.h>
#include <metavision/hal/facilities/i_ll_biases.h>
#include <metavision/hal/facilities/i_roi.h>
#include <metavision/sdk/stream/camera.h>

namespace evk4_stream {
namespace {
constexpr std::size_t block_event_count = 4096;
using Clock = std::chrono::steady_clock;

std::int64_t host_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

struct EventBlock {
    std::array<EventInput, block_event_count> events;
    std::size_t count = 0;
    std::int64_t source_time = -1;
    std::int64_t enqueued_host_us = 0;
};

Metavision::Camera open_camera(const Config &config, const std::string &raw) {
    if (raw.empty()) {
        auto serial = config.serial;
        if (serial.empty()) {
            const auto devices = Pipeline::list_devices();
            if (devices.empty())
                throw PipelineError("no camera found");
            if (devices.size() != 1)
                throw PipelineError("multiple cameras found; set camera.serial explicitly");
            serial = devices.front();
        }
        return Metavision::Camera::from_serial(serial);
    }
    Metavision::FileConfigHints hints;
    hints.real_time_playback(true);
    return Metavision::Camera::from_file(raw, hints);
}
} // namespace

struct Pipeline::Impl {
    Config config;
    std::string input_raw;
    StreamInfo stream_info;
    Stats counters;
    Metavision::Camera camera;
    std::unique_ptr<TensorBuilder> builder;
    std::array<std::shared_ptr<std::vector<float>>, 3> buffers;
    std::shared_ptr<std::vector<float>> building;
    std::shared_ptr<Packet> pending;
    std::shared_ptr<Packet> held;
    std::vector<EventBlock> blocks;
    std::vector<std::size_t> free_blocks;
    std::vector<std::size_t> queue;
    std::size_t queue_head = 0;
    std::size_t queue_size = 0;
    std::int64_t latest_source_time = -1;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    bool started = false;
    bool source_done = false;
    bool stop_requested = false;
    bool ended = false;
    std::string error;

    Impl(const Config &c, const std::string &raw) : config(c), input_raw(raw) {
        if (!((c.width == 1280 && c.height == 720) || (c.width == 640 && c.height == 360)))
            throw PipelineError("EVK4 tensor output_size must be [1280,720] or [640,360]");
        if (c.event_queue_mib == 0 || c.max_backlog_us <= 0 || c.max_tensor_wait_us <= 0)
            throw PipelineError("event_queue_mib, max_backlog_us and max_tensor_wait_us must be positive");
        try {
            camera = open_camera(c, raw);
            stream_info.source_width = camera.geometry().get_width();
            stream_info.source_height = camera.geometry().get_height();
            if (stream_info.source_width != 1280 || stream_info.source_height != 720)
                throw PipelineError("this EVK4 pipeline expects a 1280x720 source");
            stream_info.serial = camera.get_camera_configuration().serial_number;
            stream_info.input_raw = raw;
            stream_info.width = c.width;
            stream_info.height = c.height;
            stream_info.channels = c.bins * (c.split_polarity ? 2 : 1);
            stream_info.stream_id = stream_info.serial + "-" + std::to_string(host_us());
            if (raw.empty())
                configure_camera();
            builder = std::make_unique<TensorBuilder>(c, stream_info.source_width, stream_info.source_height,
                [this](const PacketMeta &meta) { publish(meta); });
            for (auto &buffer : buffers)
                buffer = std::make_shared<std::vector<float>>(builder->tensor_size());
            building = buffers[0];
            builder->bind_output(building);
            const auto block_count = c.event_queue_mib * 1024 * 1024 / sizeof(EventBlock);
            blocks.resize(block_count);
            queue.resize(block_count);
            free_blocks.reserve(block_count);
            for (std::size_t i = 0; i < block_count; ++i)
                free_blocks.push_back(i);
            counters.queue_capacity_bytes = block_count * sizeof(EventBlock);
            camera.cd().add_callback([this](const EventInput *begin, const EventInput *end) {
                enqueue_events(begin, end);
            });
            auto *decoder = camera.get_device().get_facility<Metavision::I_EventsStreamDecoder>();
            if (!decoder)
                throw PipelineError("event stream has no decoded source-time facility");
            // The SDK decoder calls this only after flushing CD callbacks. Both
            // callbacks enter this same FIFO, preserving events-before-progress.
            decoder->add_time_callback([this](Metavision::timestamp t) { enqueue_progress(t); });
            camera.add_runtime_error_callback([this](const Metavision::CameraException &e) {
                std::lock_guard<std::mutex> lock(mutex);
                fail_locked("sdk_error: " + std::string(e.what()));
            });
            camera.add_status_change_callback([this](const Metavision::CameraStatus &status) {
                if (status != Metavision::CameraStatus::STOPPED)
                    return;
                std::lock_guard<std::mutex> lock(mutex);
                if (!stop_requested && error.empty()) {
                    if (input_raw.empty())
                        fail_locked("camera_disconnected");
                    else
                        source_done = true;
                }
                changed.notify_all();
            });
        } catch (const PipelineError &) {
            throw;
        } catch (const std::exception &e) {
            throw PipelineError(e.what());
        }
    }

    void configure_camera() {
        auto *biases = camera.get_device().get_facility<Metavision::I_LL_Biases>();
        if (!biases && !config.biases.empty())
            throw PipelineError("camera does not expose bias controls");
        if (biases) {
            for (const auto &entry : biases->get_all_biases()) {
                Metavision::LL_Bias_Info info;
                if (biases->get_bias_info(entry.first, info))
                    stream_info.bias_ranges[entry.first] = info.get_bias_range();
            }
            for (const auto &entry : config.biases) {
                Metavision::LL_Bias_Info info;
                if (!biases->get_bias_info(entry.first, info) || !info.is_modifiable())
                    throw PipelineError("unsupported or read-only bias: " + entry.first);
                const auto range = info.get_bias_range();
                if (entry.second < range.first || entry.second > range.second)
                    throw PipelineError("bias outside supported range: " + entry.first);
                if (!biases->set(entry.first, entry.second))
                    throw PipelineError("could not apply bias: " + entry.first);
            }
            stream_info.applied_biases = biases->get_all_biases();
        }
        auto *roi = camera.get_device().get_facility<Metavision::I_ROI>();
        if (config.roi) {
            if (!roi)
                throw PipelineError("camera does not expose ROI controls");
            const auto &r = *config.roi;
            if (r[0] < 0 || r[1] < 0 || r[2] <= 0 || r[3] <= 0 ||
                r[0] + r[2] > stream_info.source_width || r[1] + r[3] > stream_info.source_height)
                throw PipelineError("ROI must be inside the native sensor dimensions");
            // Use the SDK pixel masks for exact [x, x + width) bounds. The
            // Gen4.1 window-register path includes an extra endpoint pixel.
            std::vector<bool> cols(stream_info.source_width, false);
            std::vector<bool> rows(stream_info.source_height, false);
            std::fill(cols.begin() + r[0], cols.begin() + r[0] + r[2], true);
            std::fill(rows.begin() + r[1], rows.begin() + r[1] + r[3], true);
            if (!roi->set_mode(Metavision::I_ROI::Mode::ROI) ||
                !roi->set_lines(cols, rows) || !roi->enable(true))
                throw PipelineError("could not apply device ROI");
            // get_windows() is empty in line mode; get_lines() reads the SDK's
            // applied masks. Actual event bounds are checked by hardware tests.
            std::vector<bool> actual_cols, actual_rows;
            if (!roi->get_lines(actual_cols, actual_rows) || actual_cols != cols || actual_rows != rows)
                throw PipelineError("device ROI mask readback differs from the requested rectangle");
            stream_info.roi = r;
        } else if (roi && !roi->enable(false)) {
            throw PipelineError("could not disable device ROI for full-frame acquisition");
        }
    }

    bool stopping_locked() const { return stop_requested || !error.empty(); }

    void fail_locked(const std::string &reason) {
        if (error.empty() && !stop_requested) {
            error = reason;
            counters.end_reason = reason;
            pending.reset();
        }
        changed.notify_all();
    }

    void queue_block_locked(std::size_t index) {
        queue[(queue_head + queue_size) % queue.size()] = index;
        ++queue_size;
        counters.queue_high_water_bytes = std::max(counters.queue_high_water_bytes, queue_size * sizeof(EventBlock));
        changed.notify_all();
    }

    void enqueue_events(const EventInput *begin, const EventInput *end) {
        while (begin != end) {
            const auto count = std::min<std::size_t>(end - begin, block_event_count);
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping_locked())
                return;
            if (free_blocks.empty()) {
                fail_locked("event_queue_full");
                return;
            }
            const auto index = free_blocks.back();
            free_blocks.pop_back();
            auto &block = blocks[index];
            std::copy_n(begin, count, block.events.begin());
            block.count = count;
            block.source_time = begin[count - 1].t;
            block.enqueued_host_us = host_us();
            latest_source_time = block.source_time;
            counters.received_events += count;
            queue_block_locked(index);
            begin += count;
        }
    }

    void enqueue_progress(std::int64_t t) {
        if (t < 0)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping_locked())
            return;
        latest_source_time = t;
        // Consecutive watermarks with no events between them can share one item.
        if (queue_size > 0) {
            auto &tail = blocks[queue[(queue_head + queue_size - 1) % queue.size()]];
            if (tail.count == 0 && t >= tail.source_time) {
                tail.source_time = t;
                tail.enqueued_host_us = host_us();
                return;
            }
        }
        if (free_blocks.empty()) {
            fail_locked("event_queue_full");
            return;
        }
        const auto index = free_blocks.back();
        free_blocks.pop_back();
        auto &block = blocks[index];
        block.count = 0;
        block.source_time = t;
        block.enqueued_host_us = host_us();
        queue_block_locked(index);
    }

    void check_pending_age_locked() {
        if (pending && host_us() - pending->meta.published_host_us > config.max_tensor_wait_us)
            fail_locked("tensor_wait_exceeded");
    }

    void publish(const PacketMeta &meta) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            check_pending_age_locked();
            if (!error.empty())
                throw PipelineError(error);
            if (stop_requested)
                throw StreamEnded("stopped");
            if (pending)
                throw PipelineError("consumer_lag: previous tensor has not been acquired");
            pending = std::make_shared<Packet>();
            pending->storage = building;
            pending->meta = meta;
            pending->meta.published_host_us = host_us();
            ++counters.published_tensors;
            counters.max_publish_delay_us = std::max(counters.max_publish_delay_us,
                pending->meta.published_host_us - meta.ready_host_us);
            for (auto &candidate : buffers) {
                if (candidate != pending->storage && (!held || candidate != held->storage)) {
                    building = candidate;
                    break;
                }
            }
            changed.notify_all();
        }
        builder->bind_output(building);
    }

    void run() {
        try {
            while (true) {
                std::size_t index;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    while (queue_size == 0 && !source_done && !stopping_locked()) {
                        if (pending) {
                            changed.wait_until(lock, Clock::time_point(std::chrono::microseconds(
                                pending->meta.published_host_us + config.max_tensor_wait_us + 1)));
                            check_pending_age_locked();
                        } else {
                            changed.wait(lock);
                        }
                    }
                    check_pending_age_locked();
                    if (stopping_locked())
                        break;
                    if (queue_size == 0 && source_done) {
                        counters.end_reason = "eof";
                        break;
                    }
                    index = queue[queue_head];
                    queue_head = (queue_head + 1) % queue.size();
                    --queue_size;
                    const auto &block = blocks[index];
                    const auto wait = host_us() - block.enqueued_host_us;
                    counters.max_queue_wait_us = std::max(counters.max_queue_wait_us, wait);
                    const auto backlog = std::max<std::int64_t>(0, latest_source_time - block.source_time);
                    counters.max_source_backlog_us = std::max(counters.max_source_backlog_us, backlog);
                    if (backlog > config.max_backlog_us) {
                        fail_locked("source_backlog_exceeded");
                        break;
                    }
                }
                auto &block = blocks[index];
                if (block.count)
                    builder->process(block.events.data(), block.events.data() + block.count, block.enqueued_host_us);
                else
                    builder->progress(block.source_time, block.enqueued_host_us);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    counters.processed_events += block.count;
                    free_blocks.push_back(index);
                }
            }
            builder->finish();
        } catch (const StreamEnded &e) {
            std::lock_guard<std::mutex> lock(mutex);
            stop_requested = true;
            pending.reset();
            counters.end_reason = e.reason;
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(mutex);
            fail_locked(e.what());
        }
        // Only this control path stops the camera, never an SDK callback.
        try {
            camera.stop();
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(mutex);
            fail_locked("sdk_stop_error: " + std::string(e.what()));
        }
        std::lock_guard<std::mutex> lock(mutex);
        ended = true;
        counters.stopped_host_us = host_us();
        changed.notify_all();
    }
};

Pipeline::Pipeline(const Config &config, const std::string &input_raw) :
    impl_(std::make_unique<Impl>(config, input_raw)) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::start() {
    auto &s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.started || s.ended)
            throw PipelineError("create a new Pipeline to start a new stream");
        s.started = true;
        s.counters.started_host_us = host_us();
    }
    try {
        if (!s.camera.start())
            throw PipelineError("camera did not start");
        s.worker = std::thread([&s] { s.run(); });
    } catch (const std::exception &e) {
        s.camera.stop();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.fail_locked(e.what());
        s.ended = true;
        s.counters.stopped_host_us = host_us();
        throw PipelineError(e.what());
    }
}

std::shared_ptr<Packet> Pipeline::read(int timeout_ms) {
    if (timeout_ms < 0)
        throw PipelineError("timeout_ms must be nonnegative");
    auto &s = *impl_;
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.started)
        throw PipelineError("start the Pipeline before reading");
    if (s.held)
        throw PipelineError("release the current packet before reading another");
    s.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&s] {
        return s.pending || s.ended || !s.error.empty() || s.stop_requested;
    });
    s.check_pending_age_locked();
    if (!s.error.empty())
        throw PipelineError(s.error);
    if (s.pending) {
        s.held = std::move(s.pending);
        s.held->meta.received_host_us = host_us();
        s.counters.max_tensor_wait_us = std::max(s.counters.max_tensor_wait_us,
            s.held->meta.received_host_us - s.held->meta.published_host_us);
        ++s.counters.delivered_tensors;
        s.changed.notify_all();
        return s.held;
    }
    if (s.ended || s.stop_requested)
        throw StreamEnded(s.counters.end_reason.empty() ? "stopped" : s.counters.end_reason);
    return nullptr;
}

void Pipeline::release() {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.held.reset();
    s.changed.notify_all();
}

void Pipeline::stop() {
    auto &s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.ended && !s.stop_requested) {
            s.stop_requested = true;
            s.pending.reset();
            if (s.error.empty())
                s.counters.end_reason = "stopped";
        }
        if (!s.started)
            s.ended = true;
        s.changed.notify_all();
    }
    if (s.worker.joinable())
        s.worker.join();
}

StreamInfo Pipeline::info() const {
    return impl_->stream_info;
}

Stats Pipeline::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->counters;
}

std::vector<std::string> Pipeline::list_devices() {
    std::vector<std::string> devices;
    for (const auto &source : Metavision::Camera::list_online_sources()) {
        for (const auto &serial : source.second) {
            if (std::find(devices.begin(), devices.end(), serial) == devices.end())
                devices.push_back(serial);
        }
    }
    return devices;
}

} // namespace evk4_stream
