# EVK4-STREAM

基于 OpenEB 5.2.0 的 EVK4 采集与 tensor 供数程序，使用 Docker 开发和运行。

## 架构

一个应用进程，三个模块：

```text
EVK4 / RAW → 官方 SDK 采集与解码 → 有界事件队列
          → C++ EventCube 构建线程 → 只读 NumPy → 冒烟消费者 / 算法
```

- **采集与设置**：连接 EVK4，设置 bias 和矩形 ROI，支持 RAW 回放。
- **tensor 构建**：复用官方分窗与 EventCube 处理器，输出 CPU float32、CHW 数组。
- **消费者**：冒烟检查或自定义算法，通过进程内 `Pipeline` 接收数据。

默认 20 ms 窗口、5 个时间 bin、OFF/ON 分离，输出 `[10,720,1280]`；另支持 `[10,360,640]`。连续交付，积压或消费超时会明确结束供数。当前提供 CLI。

详细说明：[设计文档](EVK4-STREAM_设计文档.md) · [交付文档与 API](EVK4-STREAM_交付文档.md)

## 环境与构建

需要 Linux、Docker Engine，以及已安装 OpenEB 5.2.0 的 SDK 镜像。SDK 安装目录为 `/opt/metavision`；默认复用本机 `hppr:capture-jammy`，也可通过 `OPENEB_IMAGE` 指定符合这一目录结构的镜像。构建时需要联网安装 Ubuntu 依赖。

```bash
./scripts/build.sh
./scripts/dev.sh test
```

项目挂载到容器 `/workspace`，编译产物位于 `build/`。修改 C++ 后重新编译：

```bash
./scripts/dev.sh exec cmake --build build --parallel 2
```

## 实机运行

连接 EVK4，确保宿主当前用户有 USB 访问权限：

```bash
./scripts/dev.sh devices
./scripts/dev.sh run --config configs/bench.yaml --smoke --samples 20
```

脚本自动查找 EVK4 USB 节点。需要显式选择时设置 `EVK4_USB_DEVICE=/dev/bus/usb/004/003`，路径以本机实际枚举为准。

`--smoke` 检查前 20 份输入的格式、连续性和数值；`--samples 20` 收到 20 份后退出。省略 `--samples` 持续运行到 Ctrl+C，也可使用 `--duration 10`。输出为逐行 JSON，最终 `summary` 和退出码表示整次运行结果。

修改 [configs/bench.yaml](configs/bench.yaml) 后重新启动：

- `camera`：设备、bias、矩形 ROI。ROI 保留原传感器坐标。
- `tensor`：窗口、bins、极性、输出尺寸与数值归一化。半尺寸由构建模块映射坐标，不改变设备原生分辨率。
- `runtime`：事件队列容量、源时间积压和 tensor 领取等待上限。

## RAW 回放

生成测试样本并回放：

```bash
./scripts/dev.sh exec ./build/make_fixture build/fixture.raw
./scripts/dev.sh replay --input build/fixture.raw --config configs/replay-check.yaml --smoke
```

使用已有录制文件：

```bash
EVK4_DATA_DIR=/path/to/recordings ./scripts/dev.sh replay \
  --input /data/sample.raw --config configs/replay-check.yaml --smoke
```

`replay-check.yaml` 使用半尺寸和较宽的等待限值，用于功能验证。RAW 解码可能成批交付；这份配置不作为低延迟性能配置。回放不应用设备 bias 或 ROI。

## 接入算法

替换 [examples/consume.py](examples/consume.py) 中的 `consume(array, meta)`，通过同一进程的 `Pipeline` 读取 tensor：

```bash
EVK4_USB_DEVICE=/dev/bus/usb/004/003 ./scripts/dev.sh exec python3 examples/consume.py
```

数组为只读视图，仅在 `with pipeline.read(...)` 上下文内使用；保存或异步使用时复制。完整 API、字段和适配示例见[交付文档](EVK4-STREAM_交付文档.md)。

## 独立运行镜像

```bash
./scripts/build.sh --runtime
docker run --rm --init \
  --device /dev/bus/usb/004/003 \
  evk4-stream:runtime run --smoke --samples 20
```

替换为本机 USB 路径。运行镜像包含 SDK、应用和默认配置，无需挂载源码。
