#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <metavision/sdk/base/events/event_cd.h>

namespace evk4_flow {

using EventInput = Metavision::EventCD;

struct Config {
    std::string serial;
    std::map<std::string, int> biases;
    std::optional<std::array<int, 4>> roi;
    std::int64_t window_us = 20000;
    int bins = 5;
    bool split_polarity = true;
    int width = 1280;
    int height = 720;
    float max_incr_per_pixel = 5.f;
    float clip_value = 1.f;
    bool spatial_area = false;
    std::size_t event_queue_mib = 256;
    std::int64_t max_backlog_us = 20000;
    std::int64_t max_tensor_wait_us = 20000;
};

struct PacketMeta {
    std::uint64_t sequence_id = 0;
    std::int64_t start_us = 0;
    std::int64_t end_us = 0;
    std::uint64_t event_count = 0;
    std::int64_t ready_host_us = 0;
    std::int64_t published_host_us = 0;
    std::int64_t received_host_us = 0;
};

struct Packet {
    std::shared_ptr<std::vector<float>> storage;
    PacketMeta meta;
};

struct StreamInfo {
    std::string stream_id;
    std::string serial;
    std::string input_raw;
    int source_width = 0;
    int source_height = 0;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::map<std::string, int> applied_biases;
    std::map<std::string, std::pair<int, int>> bias_ranges;
    std::optional<std::array<int, 4>> roi;
};

struct Stats {
    std::uint64_t received_events = 0;
    std::uint64_t processed_events = 0;
    std::uint64_t published_tensors = 0;
    std::uint64_t delivered_tensors = 0;
    std::size_t queue_capacity_bytes = 0;
    std::size_t queue_high_water_bytes = 0;
    std::int64_t max_queue_wait_us = 0;
    std::int64_t max_source_backlog_us = 0;
    std::int64_t max_tensor_wait_us = 0;
    std::int64_t max_publish_delay_us = 0;
    std::int64_t started_host_us = 0;
    std::int64_t stopped_host_us = 0;
    std::string end_reason;
};

class StreamEnded : public std::runtime_error {
public:
    explicit StreamEnded(const std::string &reason) : std::runtime_error(reason), reason(reason) {}
    std::string reason;
};

class PipelineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Internal builder seam: the same official reslicer and processor used by Pipeline.
// The callback consumes the completed current output synchronously and may bind
// another preallocated buffer before returning. No incomplete tail is flushed.
class TensorBuilder {
public:
    using OnWindow = std::function<void(const PacketMeta &)>;
    TensorBuilder(const Config &config, int source_width, int source_height, OnWindow on_window);
    ~TensorBuilder();
    void bind_output(const std::shared_ptr<std::vector<float>> &output);
    void process(const EventInput *begin, const EventInput *end, std::int64_t ready_host_us = 0);
    void progress(std::int64_t source_time_us, std::int64_t ready_host_us = 0);
    void finish();
    std::size_t tensor_size() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Pipeline {
public:
    explicit Pipeline(const Config &config, const std::string &input_raw = "");
    ~Pipeline();
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;
    void start();
    std::shared_ptr<Packet> read(int timeout_ms);
    void release();
    void stop();
    StreamInfo info() const;
    Stats stats() const;
    static std::vector<std::string> list_devices();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace evk4_flow
