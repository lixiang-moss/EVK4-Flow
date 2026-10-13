#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "evk4_flow/pipeline.hpp"

using evk4_flow::Config;
using evk4_flow::EventInput;
using evk4_flow::PacketMeta;
using evk4_flow::TensorBuilder;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Config small_config() {
    Config config;
    config.width = 8;
    config.height = 6;
    return config;
}

struct Window {
    PacketMeta meta;
    std::vector<float> values;
};

struct Capture {
    std::vector<Window> windows;
    std::shared_ptr<std::vector<float>> output;
    TensorBuilder builder;

    explicit Capture(const Config &config, int source_width = 8, int source_height = 6) :
        builder(config, source_width, source_height,
                [this](const PacketMeta &meta) { windows.push_back({meta, *output}); }) {
        output = std::make_shared<std::vector<float>>(builder.tensor_size());
        builder.bind_output(output);
    }

    void process(const std::vector<EventInput> &events) {
        builder.process(events.data(), events.data() + events.size());
    }
};

struct Cell {
    int channel;
    int y;
    int x;
    float value;
};

// Hand-written sparse expectations, not a second EventCube implementation.
void expect_cells(const Window &window, const Config &config, const std::vector<Cell> &cells) {
    const auto channels = config.bins * (config.split_polarity ? 2 : 1);
    const auto size = static_cast<std::size_t>(channels) * config.height * config.width;
    require(window.values.size() == size, "unexpected tensor size");
    const auto nonzero = std::count_if(window.values.begin(), window.values.end(),
                                      [](float value) { return std::abs(value) > 1e-6f; });
    require(nonzero == static_cast<std::ptrdiff_t>(cells.size()), "unexpected nonzero cells");
    for (const auto &cell : cells) {
        const auto index = (cell.channel * config.height + cell.y) * config.width + cell.x;
        const auto actual = window.values.at(index);
        require(std::isfinite(actual) && std::abs(actual - cell.value) < 2e-6f,
                "wrong value at channel=" + std::to_string(cell.channel) +
                    ", y=" + std::to_string(cell.y) + ", x=" + std::to_string(cell.x) +
                    ": expected " + std::to_string(cell.value) + ", got " + std::to_string(actual));
    }
}

void temporal_edges_and_polarity() {
    const auto config = small_config();
    Capture capture(config);
    capture.process({{0, 0, 0, 100000}, {1, 0, 1, 108000}, {2, 0, 0, 118000},
                     {3, 0, 1, 119999}, {4, 0, 1, 120000}});
    require(capture.windows.size() == 1, "boundary event must close only the preceding window");
    capture.builder.progress(140000);
    capture.builder.finish();
    require(capture.windows.size() == 2, "finish must not publish an incomplete tail");

    const auto &first = capture.windows[0];
    require(first.meta.sequence_id == 0 && first.meta.start_us == 100000 && first.meta.end_us == 120000,
            "windows must be anchored at the first event, including a nonzero timestamp");
    require(first.meta.event_count == 4, "right-edge event belongs to the following window");
    // Edges lose the out-of-window weight; 8 ms splits evenly between bins 1 and 2.
    expect_cells(first, config, {{0, 0, 0, 0.1f}, {3, 0, 1, 0.1f}, {5, 0, 1, 0.1f},
                                 {8, 0, 2, 0.2f}, {9, 0, 3, 0.10005f}});
    const auto &second = capture.windows[1];
    require(second.meta.sequence_id == 1 && second.meta.start_us == 120000 &&
                second.meta.end_us == 140000 && second.meta.event_count == 1,
            "wrong metadata for the boundary event's window");
    expect_cells(second, config, {{1, 0, 4, 0.1f}});
}

void signed_values_and_incremental_clipping() {
    auto config = small_config();
    config.split_polarity = false;
    config.max_incr_per_pixel = 1.f;
    Capture capture(config);
    capture.process({{0, 0, 1, 0}, {1, 1, 1, 2000}, {1, 1, 1, 2000}, {1, 1, 1, 2000},
                     {1, 1, 0, 2000}, {2, 1, 0, 2000}, {2, 1, 0, 2000}, {2, 1, 0, 2000},
                     {3, 1, 1, 2000}});
    capture.builder.progress(20000);
    require(capture.windows.size() == 1 && capture.windows[0].meta.event_count == 9,
            "signed window lost events");
    // Three ON increments saturate at +1, then one OFF returns to zero.
    // Clipping only once after summing would incorrectly leave +1 at (1,1).
    expect_cells(capture.windows[0], config,
                 {{0, 0, 0, 0.5f}, {0, 1, 2, -1.f}, {0, 1, 3, 1.f}});
}

void half_resolution_and_area_normalization() {
    Config config;
    config.width = 640;
    config.height = 360;
    const std::vector<EventInput> events = {
        {1279, 719, 1, 0},
        {100, 50, 0, 2000}, {101, 50, 0, 2000}, {100, 51, 0, 2000}, {101, 51, 0, 2000},
        {100, 50, 0, 2000}, {101, 50, 0, 2000}, {100, 51, 0, 2000}, {101, 51, 0, 2000},
        {101, 51, 1, 8000},
    };
    for (bool spatial_area : {false, true}) {
        config.spatial_area = spatial_area;
        Capture capture(config, 1280, 720);
        capture.process(events);
        capture.builder.progress(20000);
        require(capture.windows.size() == 1 && capture.windows[0].meta.event_count == 10,
                "spatial mapping lost events");
        const auto scale = spatial_area ? 0.25f : 1.f;
        // The 2x2 source block maps to one cell. Area scaling happens before
        // clipping: 8 / 5 / 4 = 0.4, rather than clip(8 / 5) / 4 = 0.25.
        expect_cells(capture.windows[0], config,
                     {{0, 25, 50, spatial_area ? 0.4f : 1.f}, {1, 359, 639, 0.1f * scale},
                      {3, 25, 50, 0.1f * scale}, {5, 25, 50, 0.1f * scale}});
    }
}

void empty_windows_and_unfinished_tail() {
    const auto config = small_config();
    Capture capture(config);
    capture.process({{0, 0, 0, 12345}});
    capture.builder.progress(72345);
    require(capture.windows.size() == 3, "source time must close intervening empty windows");
    expect_cells(capture.windows[0], config, {{0, 0, 0, 0.1f}});
    for (std::size_t index = 1; index < 3; ++index) {
        const auto &window = capture.windows[index];
        require(window.meta.sequence_id == index && window.meta.event_count == 0 &&
                    window.meta.start_us == 12345 + static_cast<std::int64_t>(index) * 20000 &&
                    window.meta.end_us == 32345 + static_cast<std::int64_t>(index) * 20000,
                "wrong metadata for an empty window");
        expect_cells(window, config, {});
    }
    capture.process({{1, 1, 1, 73000}});
    capture.builder.finish();
    require(capture.windows.size() == 3, "EOF must discard the incomplete tail");
}

void sdk_chunk_boundaries_do_not_change_tensors() {
    const auto config = small_config();
    const std::vector<EventInput> events = {
        {0, 0, 0, 1000}, {1, 0, 1, 3000}, {2, 0, 1, 9000}, {3, 0, 0, 20999},
        {4, 0, 1, 21000}, {5, 0, 0, 29000}, {6, 0, 1, 61000},
    };
    Capture whole(config);
    whole.process(events);
    whole.builder.progress(81000);
    whole.builder.finish();

    Capture chunks(config);
    std::size_t offset = 0;
    for (std::size_t count : {1, 2, 1, 1, 2}) {
        chunks.builder.process(events.data() + offset, events.data() + offset + count);
        offset += count;
    }
    chunks.builder.progress(81000);
    chunks.builder.finish();
    require(whole.windows.size() == 4 && chunks.windows.size() == whole.windows.size(),
            "chunk boundaries changed the number of windows");
    const std::vector<std::uint64_t> event_counts = {4, 2, 0, 1};
    for (std::size_t index = 0; index < whole.windows.size(); ++index) {
        const auto &left = whole.windows[index];
        const auto &right = chunks.windows[index];
        require(left.meta.event_count == event_counts[index] &&
                    left.meta.event_count == right.meta.event_count &&
                    left.meta.sequence_id == right.meta.sequence_id &&
                    left.meta.start_us == right.meta.start_us && left.meta.end_us == right.meta.end_us,
                "chunk boundaries changed window metadata");
        require(left.values == right.values, "chunk boundaries changed tensor values");
    }
}

} // namespace

int main() {
    try {
        temporal_edges_and_polarity();
        signed_values_and_incremental_clipping();
        half_resolution_and_area_normalization();
        empty_windows_and_unfinished_tail();
        sdk_chunk_boundaries_do_not_change_tensors();
        std::cout << "PASS: temporal edges, polarity, incremental clipping, spatial mapping, "
                     "area normalization, empty/tail windows, and chunk equivalence\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
