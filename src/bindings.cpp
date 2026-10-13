#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "evk4_flow/pipeline.hpp"

namespace py = pybind11;
using namespace evk4_flow;

PYBIND11_MODULE(_evk4_pipeline, m) {
    py::register_exception<StreamEnded>(m, "StreamEnded");
    py::register_exception<PipelineError>(m, "PipelineError");
    py::class_<Config>(m, "Config")
        .def(py::init<>())
        .def_readwrite("serial", &Config::serial)
        .def_readwrite("biases", &Config::biases)
        .def_readwrite("roi", &Config::roi)
        .def_readwrite("window_us", &Config::window_us)
        .def_readwrite("bins", &Config::bins)
        .def_readwrite("split_polarity", &Config::split_polarity)
        .def_readwrite("width", &Config::width)
        .def_readwrite("height", &Config::height)
        .def_readwrite("max_incr_per_pixel", &Config::max_incr_per_pixel)
        .def_readwrite("clip_value", &Config::clip_value)
        .def_readwrite("spatial_area", &Config::spatial_area)
        .def_readwrite("event_queue_mib", &Config::event_queue_mib)
        .def_readwrite("max_backlog_us", &Config::max_backlog_us)
        .def_readwrite("max_tensor_wait_us", &Config::max_tensor_wait_us);

    py::class_<PacketMeta>(m, "PacketMeta")
        .def_readonly("sequence_id", &PacketMeta::sequence_id)
        .def_readonly("t_start_us", &PacketMeta::start_us)
        .def_readonly("t_end_us", &PacketMeta::end_us)
        .def_readonly("event_count", &PacketMeta::event_count)
        .def_readonly("ready_host_us", &PacketMeta::ready_host_us)
        .def_readonly("published_host_us", &PacketMeta::published_host_us)
        .def_readonly("received_host_us", &PacketMeta::received_host_us);
    py::class_<Packet, std::shared_ptr<Packet>>(m, "Packet")
        .def_readonly("meta", &Packet::meta)
        .def("array", [](const Packet &packet, const std::array<py::ssize_t, 3> &shape) {
            auto *owner = new std::shared_ptr<std::vector<float>>(packet.storage);
            py::capsule lifetime(owner, [](void *ptr) {
                delete static_cast<std::shared_ptr<std::vector<float>> *>(ptr);
            });
            py::array array(py::dtype::of<float>(),
                            {shape[0], shape[1], shape[2]},
                            {shape[1] * shape[2] * py::ssize_t(sizeof(float)),
                             shape[2] * py::ssize_t(sizeof(float)), py::ssize_t(sizeof(float))},
                            packet.storage->data(), lifetime);
            array.attr("setflags")(false);
            return array;
        });

    py::class_<Pipeline>(m, "Pipeline")
        .def(py::init<const Config &, const std::string &>(), py::arg("config"), py::arg("input_raw") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("start", &Pipeline::start, py::call_guard<py::gil_scoped_release>())
        .def("read", &Pipeline::read, py::call_guard<py::gil_scoped_release>())
        .def("release", &Pipeline::release)
        .def("stop", &Pipeline::stop, py::call_guard<py::gil_scoped_release>())
        .def("info", [](const Pipeline &pipeline) {
            auto info = pipeline.info();
            py::dict result;
            result["stream_id"] = info.stream_id;
            result["serial"] = info.serial;
            result["input_raw"] = info.input_raw;
            result["shape"] = py::make_tuple(info.channels, info.height, info.width);
            result["sensor_size"] = py::make_tuple(info.source_width, info.source_height);
            result["output_size"] = py::make_tuple(info.width, info.height);
            result["dtype"] = "float32";
            result["layout"] = "CHW";
            result["applied_biases"] = info.applied_biases;
            result["bias_ranges"] = info.bias_ranges;
            result["roi"] = info.roi;
            return result;
        })
        .def("stats", [](const Pipeline &pipeline) {
            auto stats = pipeline.stats();
            py::dict result;
#define STAT(name) result[#name] = stats.name
            STAT(received_events);
            STAT(processed_events);
            STAT(published_tensors);
            STAT(delivered_tensors);
            STAT(queue_capacity_bytes);
            STAT(queue_high_water_bytes);
            STAT(max_queue_wait_us);
            STAT(max_source_backlog_us);
            STAT(max_tensor_wait_us);
            STAT(max_publish_delay_us);
            STAT(started_host_us);
            STAT(stopped_host_us);
            STAT(end_reason);
#undef STAT
            return result;
        });
    m.def("list_devices", &Pipeline::list_devices, py::call_guard<py::gil_scoped_release>());
}
