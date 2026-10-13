ARG OPENEB_IMAGE=hppr:capture-jammy
FROM ${OPENEB_IMAGE} AS openeb

FROM ubuntu:22.04 AS dependencies
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-numpy python3-yaml \
    libusb-1.0-0 libprotobuf23 libhdf5-cpp-103-1 libboost-timer1.74.0 \
    libopencv-core4.5d libopencv-imgproc4.5d libopencv-highgui4.5d libopencv-videoio4.5d \
    && rm -rf /var/lib/apt/lists/*
# Reuse the installed OpenEB 5.2.0 SDK, not the other camera runtimes in that image.
COPY --from=openeb /opt/metavision /opt/metavision
ENV CMAKE_PREFIX_PATH=/opt/metavision \
    LD_LIBRARY_PATH=/opt/metavision/lib \
    MV_HAL_PLUGIN_PATH=/opt/metavision/lib/metavision/hal/plugins \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

FROM dependencies AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    python3-dev pybind11-dev libopencv-dev libboost-timer-dev \
    && rm -rf /var/lib/apt/lists/*
ENV PYTHONPATH=/workspace/python:/workspace/build
WORKDIR /workspace
CMD ["bash"]

FROM dev AS application
COPY . /workspace
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --parallel 2

FROM dependencies AS runtime
COPY --from=application /workspace/build/_evk4_pipeline*.so /opt/evk4/build/
COPY python /opt/evk4/python
COPY configs /opt/evk4/configs
ENV PYTHONPATH=/opt/evk4/python:/opt/evk4/build
WORKDIR /opt/evk4
ENTRYPOINT ["python3", "-m", "evk4_pipeline"]
CMD ["--help"]
