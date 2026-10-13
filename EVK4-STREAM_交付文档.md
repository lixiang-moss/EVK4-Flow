# EVK4-STREAM 交付文档：算法接入与 API 使用说明

日期：2026-10-14 · 版本：v1.2

关联：[设计文档](EVK4-STREAM_设计文档.md)

本文面向接入 EVK4 数据的算法开发者。项目负责相机采集和 tensor 构造，算法程序通过 Python API 取得完整输入，再调用模型。采集、构造与算法在同一个 Docker 容器、同一个应用进程中运行。

**当前已实现 `evk4_pipeline`、YAML 配置、CLI、冒烟消费者及 Docker 构建入口。** 本文按当前 Python 包和 C++ 绑定说明可用接口；GUI 与具体模型适配尚未实现。

快速开始见 [README](README.md)。首次接入先看第 11 节构建与冒烟命令，再看第 4 节接收示例。API 定义在第 7 节，模型接入与内存规则在第 8、9 节。当前实现依据为 [Python 接口](python/evk4_pipeline/__init__.py)、[C++ 绑定](src/bindings.cpp)、[CLI](python/evk4_pipeline/__main__.py) 和 [默认配置](configs/bench.yaml)。

## 1 双方分别负责什么

| 工作 | 责任方 | 交付内容 |
|---|---|---|
| 连接 EVK4、设置 bias 和设备 ROI、接收与解码事件 | 本项目 | 模块 1，内部使用官方 OpenEB |
| 分窗、坐标映射、体素累积、数值缩放与裁剪 | 本项目 | 模块 2，由 C++ 持续构造 tensor |
| 缓冲复用、完整输入交付、停止与错误通知 | 本项目 | 本文定义的 `Pipeline` API |
| 输入规格及数值含义 | 算法方提供要求，双方确认 | 优先提供训练时预处理代码、事件小样本与期望输入 |
| 框架对象、batch 维和输入设备适配 | 算法接入代码 | 基础接口保持 NumPy；第 9 节提供可复制的 PyTorch 示例，当前未集成具体模型 |
| 模型加载、推理、历史状态及结果处理 | 算法方 | 自己的模型与调用逻辑 |

算法方不用实现从事件到体素的构造过程。导入 `evk4_pipeline`，就是把本项目的第一、第二部分装进自己的程序。初期没有正式模型时，用第 4 节的接收示例作为第三部分跑通链路。

基础出口是只读 CPU NumPy 数组。若正式模型要求 PyTorch、batch=1 和指定 GPU，双方确认后由接入函数完成这些转换；不要求算法方重写体素算法，也不把框架转换当成重新构造体素。

## 2 tensor 可配置但运行期间固定

**输入规格在启动前配置，在一次运行期间保持不变。** 首期的表示为固定时间窗 OpenEB EventCube，类型固定为 `float32`、布局固定为 CHW；可配置窗长、bins、极性模式、已有输出尺寸以及数值缩放参数。

构造 `Pipeline(...)` 对象时，程序加载配置、打开数据源、应用相机设置并分配缓冲；进入 `with` 时才开始持续采集和 C++ 工作线程。此后复用内存：每窗清零可写数组，按事件更新对应位置，完成后发布。不会对每个事件重新解析 YAML，也不会根据负载自动修改算法输入。

修改方式：结束当前流水线 → 修改配置 → 创建新的 `Pipeline` → 读取新的 `info` → 按算法要求重置历史状态。编辑磁盘上的 YAML 不会改变已运行的流水线。

| 问题 | 约定 |
|---|---|
| 是不是支持任意 tensor | 首期支持本文列出的 EventCube 配置；另一种表示或分窗方法需要明确需求后修改模块 2 |
| 是不是算法方自己写 tensor 构造 | 不需要，算法方提供规格，本项目负责实现 |
| 写死参数会不会更快 | 仅把配置中的数字搬进代码，通常不会明显改变同一处理器的计算成本；针对固定规格专门优化，才可能进一步减少开销 |
| 为什么保留配置 | 启动时确定参数仍可预分配、复用内存，便于适配模型而不增加逐事件配置解析 |
| 改分辨率为什么可能提速 | 减少的是张量规模及相关处理量；收益来自工作量变化，不来自“参数写死” |

## 3 接入前准备

算法方先提供已有模型的输入要求：表示与预处理规则、shape／dtype／布局、是否带 batch 维、输入设备，以及模型单次运行耗时。已有训练预处理代码可以直接作为对齐依据，不能只凭 shape 相同确认兼容。

交付环境采用原生 Linux x86_64 和 Docker。当前构建入口 `scripts/build.sh` 使用 OpenEB 5.2.0，构建带 NumPy、PyYAML 和 `evk4_pipeline` 扩展模块的环境；配置与接收示例已在仓库中。镜像默认从本机 `hppr:capture-jammy` 的 `/opt/metavision` 复制 SDK，可通过 `OPENEB_IMAGE` 指定具有同样安装目录的镜像。第 11 节给出运行命令。

使用 PyTorch 模型时，将双方确认的 PyTorch 与模型依赖装在同一个容器环境中；当前基础镜像未包含 PyTorch，也未自动配置 GPU 访问。

在新电脑交付时，需要一并提供构建所需的 OpenEB 基镜像，或直接交付已经构建好的开发／运行镜像。仅复制仓库后执行 `build.sh`，不会自动从零安装 OpenEB；`OPENEB_IMAGE` 指向的镜像必须有兼容的 `/opt/metavision` 安装，CMake 当前要求 OpenEB 5.2.0。

相机输入需要选定 EVK4 的 USB 访问权限和设备序列号；RAW 输入只需要容器内可读取的文件。相机不要同时被其他采集程序占用。

**先完成模型加载、设备初始化和必要的预热，再启动 Pipeline。** 流水线启动后会持续生成输入，不能在此时花很长时间加载模型。

## 4 最小接收示例

以下脚本可以保存为算法工程中的 `receive_demo.py`。它接收首 20 份完整输入，输出窗口编号和事件数，然后退出；它验证接收流程，不运行模型。仓库另有 `examples/consume.py` 作为持续接收入口，CLI 的 `--smoke` 可直接运行冒烟检查。

```python
import numpy as np
from evk4_pipeline import Pipeline, PipelineError, StreamEnded


def main(config_path="configs/bench.yaml", *, input_raw=None,
         expected_shape=(10, 720, 1280)):
    received = 0
    try:
        with Pipeline(config_path=config_path, input_raw=input_raw) as pipeline:
            info = pipeline.info
            print("stream:", info.stream_id, info.shape, info.dtype, info.layout)
            if info.shape != expected_shape or info.dtype != "float32":
                raise ValueError("输入规格与本示例约定不一致")

            while received < 20:
                with pipeline.read(timeout_ms=1000) as packet:
                    if packet is None:
                        continue
                    array = packet.array
                    meta = packet.meta
                    if not np.isfinite(array).all():
                        raise ValueError("输入包含非有限数值")
                    print(meta.sequence_id, meta.t_start_us,
                          meta.t_end_us, meta.event_count)
                    received += 1

    except StreamEnded as end:
        print("流结束:", end.reason, "已接收:", received)
    except PipelineError as error:
        print("供数失败:", str(error))
        raise
    except KeyboardInterrupt:
        print("用户停止，已接收:", received)

    if received == 20:
        print("接收冒烟通过")
        return 0
    else:
        print("接收冒烟未完成")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
```

直接运行脚本使用全尺寸实机配置；半尺寸配置时将 `expected_shape` 改为 `(10, 360, 640)`。也可以导入 `main`，传入 `config_path="configs/replay-check.yaml"`、`input_raw` 和半尺寸预期 shape 进行 RAW 检查。收到 20 份后返回 0，正常结束但未收满时返回 2，供数错误重新抛出。

全零数组是合法输入；`packet is None` 才表示本次等待没有领到数据。正式算法只在启动时核对输入契约，不需要每窗重复进行全数组数值扫描。

`read()` 的 `with` 负责领取和归还数据；外层 `Pipeline` 的 `with` 负责启动和关闭采集。算法接入时，将循环中的检查／打印替换为自己的同步处理函数即可，具体内存规则见第 8 节。

## 5 配置方法

仓库已有 `configs/bench.yaml`，内容如下。空序列号选择唯一可用设备；多设备时填写目标连接串或序列号。bias 名称和值按实际设备支持情况填写。

```yaml
camera:
  serial: ""
  biases: {}
  roi: null

tensor:
  representation: openeb_event_cube
  window_us: 20000
  bins: 5
  split_polarity: true
  output_size: [1280, 720]
  max_incr_per_pixel: 5.0
  clip_value: 1.0
  spatial_normalization: none

runtime:
  event_queue_mib: 256
  max_backlog_us: 20000
  max_tensor_wait_us: 20000
```

`runtime` 的数值是联调起点，需结合事件率和模型耗时确认，不能当成已验证的机械臂延迟指标。

| 字段 | 含义与修改方式 |
|---|---|
| `camera.serial` | 空字符串选择唯一可用设备，多设备时必须指定；RAW 输入时忽略 |
| `camera.biases` | 设备支持的 bias 名称到数值的映射；空表表示不主动修改 |
| `camera.roi` | `null` 表示全幅，或一个设备支持的 `[x, y, width, height]` 矩形；保留原传感器坐标 |
| `tensor.window_us` | 正整数，单位微秒；首期步长等于窗长，无重叠 |
| `tensor.bins` | 正整数，时间分段数；默认 5 |
| `tensor.split_polarity` | `true` 为 OFF／ON 分离的 `2×bins` 通道；`false` 为有符号的 `bins` 通道 |
| `tensor.output_size` | 顺序是 `[宽, 高]`；首期为 `[1280,720]` 或 `[640,360]` |
| `tensor.max_incr_per_pixel` | 正数，事件累积贡献的缩放除数；默认 5 |
| `tensor.clip_value` | 正数，限制累积值幅度；0 不表示关闭裁剪 |
| `tensor.spatial_normalization` | `none` 不加面积系数；`area` 追加输出与原尺寸的面积比，半尺寸时为 1/4 |
| `runtime.event_queue_mib` | 内部事件队列容量上限，单位 MiB |
| `runtime.max_backlog_us` | 相机时间域中，最新接收进度与处理进度的允许差值 |
| `runtime.max_tensor_wait_us` | 完整 tensor 发布后等待领取的上限，使用主机单调时钟 |

当前没有 GUI 或预览配置；不需要在 YAML 中添加 `preview` 段。

相机和 tensor 参数均停止后修改。启动时核对配置并报告实际输入信息，不因参数不支持而静默改成其他输入。bias 修改会影响事件灵敏度与数量，需和训练／实际场景要求一起确认。

ROI 和输出尺寸不同：ROI 在设备支持的区域内选取事件；`output_size` 决定整幅坐标映射后的网格。设置 ROI 不会自动把输出数组裁成 ROI 大小。半尺寸使用 `x'=floor(x/2)`、`y'=floor(y/2)`；原点仍在左上角，x 向右、y 向下。

## 6 算法实际收到的数据

默认 `packet.array` 的约定如下：

| 属性 | 值 |
|---|---|
| 对象 | `numpy.ndarray`，只读 |
| 存储位置 | CPU 内存，进程内预分配缓冲的视图 |
| shape | `(10, 720, 1280)` |
| dtype | `numpy.dtype("float32")` |
| 布局 | C-contiguous CHW，即 `[通道, y, x]`，x 维连续 |
| batch 维 | 基础接口不带；需要时由接入函数增加 batch=1 |
| 时间范围 | 每份覆盖 `[t_start_us, t_end_us)`，默认 20,000 µs |
| 默认通道顺序 | `bin0_OFF, bin0_ON, bin1_OFF, bin1_ON, …, bin4_OFF, bin4_ON` |

双极性通道索引为 `c=2*b+p`，其中 `p=0` 是 OFF、`p=1` 是 ON。关闭双极性分离后，通道为 `b`，OFF 为负贡献、ON 为正贡献。

每个事件按其时间向相邻时间 bin 线性分配贡献，按配置缩放并在每次更新时裁剪。默认 20 ms／5 bins 的中心位于窗内 2、6、10、14、18 ms；例如窗内 8 ms 的 ON 事件，在原尺寸、默认参数下向 bin 1 和 bin 2 的 ON 通道各贡献 0.1。落在窗口外的 bin 权重按 EventCube 规则舍弃，不借用相邻窗口，也不重新归一化边缘权重。处理器的具体数值规则见[设计文档的张量构建说明](EVK4-STREAM_设计文档.md)。

这是完成累积和约定缩放的输入。算法端不要再次执行同一套体素构造或重复除以 `max_incr_per_pixel`。如果模型还需要其他标准化规则，在输入对齐时明确其公式及执行位置。

20 ms 窗口在源时间连续推进时对应名义 50 Hz，不承诺主机每 20 ms 准时返回一份。无事件但有可靠源时间进度时，可以产生完整的全零窗口；缺少可确认的源时间进度时等待，不按主机定时器伪造输入。尾部不足一窗的数据不交付。

## 7 Python API 参考

公开模块名为 `evk4_pipeline`，导出 `Pipeline`、`PipelineError`、`StreamEnded` 和 `list_devices`。`Pipeline` 提供读取、输入信息、停止及统计。以下返回对象由模块创建，算法方不用自己构造。

### 7.1 创建与关闭流水线

```text
Pipeline(config_path="configs/bench.yaml", *, input_raw=None)
```

| 参数或操作 | 含义 |
|---|---|
| `config_path` | YAML 路径；相对路径按算法进程的当前工作目录解析 |
| `input_raw=None` 或 `input_raw=""` | 从配置指定的 EVK4 采集 |
| `input_raw="/data/sample.raw"` | 从容器内 RAW 文件读取；不连接相机，不应用设备 bias／ROI |
| `Pipeline(...)` | 加载 YAML，打开数据源，应用相机配置并分配缓冲；构造成功后 `info` 已可读 |
| `with pipeline` | 启动持续采集与处理线程，返回同一 `pipeline` 对象 |
| `pipeline.stop() -> None` | 停止源与工作线程；可以重复调用，同一对象不能再次启动 |
| 退出外层 `with` | 自动调用 `stop()`；正常返回、异常和 Ctrl+C 都会执行收尾 |

`config_path` 可传字符串或 `pathlib.Path`；`input_raw` 可传文件路径字符串或 `Path`。构造对象会访问设备或 RAW 文件，但进入 `with` 才开始持续采集。停止后需要新建 `Pipeline` 开始下一次流，不在旧对象上自动重连。首期一个设备对应一个 Pipeline、一个消费循环；公共 Python API 没有 `close()` 或 `start()` 方法。

### 7.2 固定输入信息

`pipeline.info` 在构造成功后即可读取，是 `types.SimpleNamespace` 描述快照：

| 字段 | 类型 | 含义 |
|---|---|---|
| `stream_id` | `str` | 本次运行标识；新建流后改变 |
| `shape` | `tuple[int, int, int]` | 实际输出 `(C,H,W)` |
| `dtype` | `str` | 固定为 `"float32"`；`packet.array.dtype` 才是 NumPy dtype 对象 |
| `layout` | `str` | 首期为 `"CHW"` |
| `sensor_size` | `tuple[int, int]` | 源尺寸 `(宽,高)`，由设备或 RAW 文件确定 |
| `output_size` | `tuple[int, int]` | 输出网格 `(宽,高)` |
| `tensor_config` | `dict` | 本次实际生效的第 5 节 `tensor` 配置，包括默认值 |
| `serial` | `str` | SDK 返回的源序列号 |
| `input_raw` | `str` | RAW 路径；实机输入时为空字符串 |
| `applied_biases` | `dict[str, int]` | 设备回读的 bias 值；RAW 输入时为空 |
| `bias_ranges` | `dict[str, tuple[int, int]]` | SDK 提供的 bias 范围；RAW 输入时为空 |
| `roi` | `list[int]` 或 `None` | 实机配置的 `[x,y,width,height]`；全幅或 RAW 输入时为 `None` |

模型应核对这些字段与已确认的预处理要求。`info` 可以保存在结果记录中，用于解释本次输出。它及其中的字典不是只读对象，但应作为描述信息读取：修改这些字段不会重新配置 C++ 流水线，而且 Python 绑定在创建数组视图时会读取 `info.shape`，不能把改写它当成调整尺寸。运行参数只能通过创建新 Pipeline 更改。

### 7.3 读取与归还

```python
with pipeline.read(timeout_ms=1000) as packet:
    ...
```

`read(timeout_ms: int = 1000)` 返回一个上下文管理器。进入上下文时等待，成功后得到带 `array` 和 `meta` 属性的对象；超时得到 `None`。当前返回对象为 `types.SimpleNamespace`，没有公开的 `TensorPacket` 类型。`timeout_ms` 单位为毫秒，0 表示立即尝试，正数表示最长等待时间，负值报错。它只控制这一次接收等待，不修改事件窗长，也不延长配置中的积压／领取期限。

退出上下文归还缓冲。等待时绑定层释放 GIL，C++ 构建继续推进。首期在一个消费线程中顺序读取，同一时刻最多持有一份；不要嵌套读取，也不要从多个线程同时领取。

| `packet` 字段 | 含义 |
|---|---|
| `array` | 第 6 节定义的只读 NumPy 视图，仅在当前读取上下文内使用 |
| `meta` | 本窗的不可变元数据值对象，可以在上下文外保存 |

| `packet.meta` 字段 | 类型 | 含义 |
|---|---|---|
| `sequence_id` | `int` | 本次流内窗口编号，首份为 0，连续交付时每窗加 1 |
| `t_start_us` | `int` | 本窗相机时间起点，包含该时刻 |
| `t_end_us` | `int` | 本窗相机时间终点，不包含该时刻 |
| `event_count` | `int` | 软件实际收到并累积进本窗的事件数；同一事件分配到两个 bin 仍计一次 |
| `ready_host_us` | `int` | 使本窗能够封窗的事件块／源时间进度入队时刻，主机单调时钟微秒 |
| `published_host_us` | `int` | 完整 tensor 发布时刻，同一主机时钟 |
| `received_host_us` | `int` | 消费者领取时刻，同一主机时钟 |

窗口以源时间固定网格推进，第一份完整窗口的起点由 `t_start_us` 给出；消费端不假定它为 0，也不用首次 Python 读取时刻作为窗起点。`t_start_us`／`t_end_us` 是源时间，不是 Unix 时间，不能与主机时间直接相减。主机字段之间可以相减：`published_host_us - ready_host_us` 是入队后至发布的延迟，`received_host_us - published_host_us` 是发布后至领取的等待；它们不是传感器到模型结果的完整延迟。`event_count` 不证明传感器及 USB 链路绝无未检测丢失。

### 7.4 正常结束与失败

`StreamEnded` 和 `PipelineError` 均为 Python 异常类型。除主动退出循环外，调用方应区分以下结果：

| 结果 | 含义 | 调用方行为 |
|---|---|---|
| `packet is None` | 等待超时，当前没有可领取的完整窗 | 继续等待或按自己的等待策略结束 |
| `StreamEnded.reason == "stopped"` | 本次流水线已被关闭 | 结束消费循环 |
| `StreamEnded.reason == "eof"` | RAW 已结束，完整待交付窗口已取完 | 结束循环；不足一窗的尾部不补齐 |
| `StreamEnded.reason == "time_reset"` | 设备时间发生重置，旧流结束 | 结束循环；重新启动前按模型要求重置状态 |
| `PipelineError` | 配置、数据源或处理负载使本次运行失败 | 结束循环并报告原因，不当成普通超时继续取数 |

`PipelineError` 当前只有异常消息，**没有 `.code` 字段**。通过 `str(error)` 输出原因。运行期常见消息如下；这些是诊断文本，不是另行定义的枚举：

| 消息 | 含义 |
|---|---|
| `event_queue_full` | 事件队列容量已满 |
| `source_backlog_exceeded` | 源时间积压超限 |
| `consumer_lag: previous tensor has not been acquired` | 前一完整窗未领取时下一窗已经完成 |
| `tensor_wait_exceeded` | 已发布输入等待领取超限 |
| `camera_disconnected` | 未主动停止时相机断开 |
| `sdk_error: ...` / `sdk_stop_error: ...` | SDK 运行或停止过程报告错误 |

设备打开、参数校验等其他 C++ 错误也通过消息报告。配置加载发生在 Python 层：文件读取错误可为 `OSError`，YAML 解析错误为 `yaml.YAMLError`，不支持的表示或标准化值为 `ValueError`；它们不会全部转换为 `PipelineError`。

发生故障时停止本次供数，未交付结果丢弃，错误在启动或读取处报告；已经领取的数据保持有效到归还。算法自身抛出的异常按正常 Python 异常处理，不包装成供数错误。

### 7.5 设备枚举与运行统计

`list_devices() -> list[str]` 返回 SDK 可见的相机连接串／序列号；CLI 的 `devices` 使用同一接口。

`pipeline.stats() -> dict` 返回当前统计快照，停止后仍可读取：

| 字段 | 含义 |
|---|---|
| `received_events` / `processed_events` | 已接收及已处理的事件数量 |
| `published_tensors` / `delivered_tensors` | 已发布及已交付的完整 tensor 数量 |
| `queue_capacity_bytes` / `queue_high_water_bytes` | 实际分配的事件队列容量及使用峰值，单位字节 |
| `max_queue_wait_us` | 事件块入队后等待处理的最长时间 |
| `max_source_backlog_us` | 源时间积压峰值 |
| `max_tensor_wait_us` | 已交付 tensor 发布至领取的最长等待 |
| `max_publish_delay_us` | 使窗口可封闭的事件／进度块入队至发布的最大时间差；不是整窗构造总耗时 |
| `started_host_us` / `stopped_host_us` | 主机单调时钟的开始与结束时刻；尚未记录时为 0 |
| `end_reason` | 结束原因字符串；未结束时为空，失败时包含具体消息 |

所有 `_us` 字段以微秒为单位。统计只用于观测，不改变连续交付规则。

## 8 数组生命周期与复制

C++ 直接在预分配的进程内数组中构造 tensor，Python 取得其只读视图。基础 API 的交付不复制整份数组；不需要跨进程共享内存、连接端点或 socket。

内部轮换构建、待领取和消费者持有的缓冲。当前数组在 `with pipeline.read(...)` 内保持稳定，生产者不会覆盖它；退出后该槽位可以被复用。仅保留一个 Python 变量引用，并不能延长读取上下文的有效期。

当前包装层退出读取上下文时会将 `packet.array` 置为 `None`。即使提前写了 `array = packet.array` 保存另一个引用，底层缓冲仍可能在归还后用于下一窗，因此保存引用不等于保存一份历史数据。需要保留时使用下面的显式复制。

同步读取且不保留输入时，可以直接调用算法：

```python
with pipeline.read(timeout_ms=1000) as packet:
    if packet is not None:
        # algorithm_step 为算法方自己的同步函数。
        algorithm_step(packet.array, packet.meta)
```

这里要求 `algorithm_step` 返回前，已经完成对输入内存的读取。若它只提交了后台任务，或者返回的结果仍是输入的切片／视图，需要先取得独立数据或延长当前上下文。

需要长期保留、在上下文外使用或交给其他线程时，在上下文内复制：

```python
with pipeline.read(timeout_ms=1000) as packet:
    if packet is not None:
        owned_array = packet.array.copy()
        meta = packet.meta
        # owned_array 可在退出后保留；packet.array 不可以。
```

不要原地修改 `packet.array`，也不要通过改写只读标志绕过约定。单纯保存数据有一次明确的复制成本；异步 GPU 上传则必须保证源内存在上传读完前不会复用。第 9 节给出生命周期独立的 PyTorch 基础接法。

## 9 PyTorch 模型接入示例

以下示例只针对 **float32、batch=1、BCHW** 模型，需要先与实际模型要求对齐。`prepare_model_input` 和 `run_model` 是本文给出的可复制示例，当前仓库没有集成具体模型或提供这些函数作为 API。算法方传入已加载、已放到目标设备并完成预热的 `model`，以及自己的 `handle_result`。

```python
import numpy as np
import torch
from evk4_pipeline import Pipeline, PipelineError, StreamEnded


def prepare_model_input(array, device):
    # 创建算法自有数据，使返回值不再借用流水线的只读缓冲。
    cpu_input = torch.tensor(array, dtype=torch.float32, device="cpu")
    return cpu_input.unsqueeze(0).to(device=device, non_blocking=False)


def run_model(model, handle_result, *, config_path, device, expected_shape,
              input_raw=None):
    # 调用本函数前完成模型设备初始化和预热。
    model.eval()
    try:
        with Pipeline(config_path=config_path, input_raw=input_raw) as pipeline:
            info = pipeline.info
            if info.shape != expected_shape or info.dtype != "float32":
                raise ValueError("Pipeline 输入规格与模型不一致")
            with torch.inference_mode():
                while True:
                    with pipeline.read(timeout_ms=1000) as packet:
                        if packet is None:
                            continue
                        model_input = prepare_model_input(packet.array, device)
                        meta = packet.meta
                    # model_input 已拥有独立数据，可以归还原始缓冲后推理。
                    result = model(model_input)
                    handle_result(result, meta, info.stream_id)
    except StreamEnded as end:
        print("流结束:", end.reason)
    except PipelineError as error:
        print("供数失败:", str(error))
        raise
    except KeyboardInterrupt:
        print("用户停止")
```

默认配置的 `expected_shape=(10, 720, 1280)`，模型输入为 `(1,10,720,1280)`；`device` 明确传入 `"cpu"` 或双方确认的 `"cuda:0"` 等设备，模型必须位于同一设备。历史状态的初始化／重置由算法方根据 `stream_id` 和结束原因处理。接入示例不再做体素累积或默认之外的数值标准化。

这份基础适配有一次 CPU 复制，选择 GPU 时还有上传开销；增加 batch 维不重新构造体素。它让模型输入独立于流水线缓冲，便于在归还后推理。`torch.tensor` 的复制语义见 [PyTorch 官方文档](https://docs.pytorch.org/docs/2.14/generated/torch.tensor.html)，设备转换参数见 [Tensor.to 文档](https://docs.pytorch.org/docs/2.14/generated/torch.Tensor.to.html)。这是 API 行为参考，不要求部署版本必须为该文档版本。

不要直接把示例改成 `torch.from_numpy(packet.array)` 后立即归还原缓冲：该调用共享 NumPy 存储，且对只读 NumPy 存储创建的 Tensor 进行写入不受支持。需要减少复制时，应针对已确认模型实现并验证缓冲持有与上传完成的配合，而不是只替换一行转换代码。[torch.from_numpy 文档](https://docs.pytorch.org/docs/2.14/generated/torch.from_numpy.html)

## 10 实时运行与超载处理

当前采用连续交付、超载结束。同一流中不静默跳过旧完整窗，也不因模型运行慢而积累无限历史输入。事件队列满、源时间积压超限、完整窗未领取时又完成下一窗或领取等待超限，都会抛出 `PipelineError` 结束本次运行；具体消息见第 7 节。

默认 20 ms 窗口要求消费循环能持续跟上名义 50 Hz。把模型调用、输入适配、结果处理和日志一起计入消费耗时；平均跟得上也需要检查偶发慢调用。模型加载放在启动之前，逐窗大量打印和文件写入会占用这条循环。

| 情况 | 处理方式 |
|---|---|
| 多次返回 `None` | 表示未领到完整窗，检查源时间是否推进；不是算法得到一份全零输入 |
| 输入过载 | 查看事件率、构建速度和队列积压，定位采集／构造侧负载 |
| 消费者过载 | 查看适配、推理和结果处理耗时；按模型允许范围调整窗长、尺寸或算法负载 |
| 模型需要逐窗历史状态 | 保持连续交付，换流后按约定重置；不要自动重启后直接沿用旧状态 |
| 模型确认允许跳窗 | 后续明确改成最新窗策略并报告缺口；本版不提供一个尚未实现的切换开关 |

`timeout_ms=1000` 不是“允许处理延迟达到一秒”，也不是 tensor 的时间窗。消费期限由配置和交付策略决定。缩小 tensor 减少数组相关工作，不缩短既定时间窗，也不自动减少原始事件解码量。

机械臂使用结果时，保留窗口编号、源时间范围和 `stream_id` 的关联，并由控制侧按其时效要求决定是否使用结果。供数模块不负责发送运动指令。

## 11 Docker 中运行及 RAW 联调

### 11.1 构建与相机冒烟

从项目目录执行：

```bash
./scripts/build.sh
./scripts/dev.sh test
./scripts/dev.sh devices
./scripts/dev.sh run --config configs/bench.yaml --smoke --samples 20
```

默认开发镜像为 `evk4-stream:dev`。脚本将项目挂载到 `/workspace`，构建产物在项目 `build/`；Python 路径已经包含 `/workspace/python` 和 `/workspace/build`。`devices` 和 `run` 自动寻找 EVK4 USB 节点并映射给容器，宿主仍需具有该节点的访问权限。多设备时用 `EVK4_USB_DEVICE` 选择 USB 节点，并在 YAML 中指定对应序列号。

输出为逐行 JSON：`info` 给出实际输入与设备设置，`sample` 给出前 20 份摘要，`smoke` 表示首批检查结果，`status` 给出接收统计，最终 `summary` 包含整次运行结果和时延分位数。采样分位数最多统计最近 4096 份输入；`timing_sample_count` 给出实际数量。

| 参数 | 作用 |
|---|---|
| `--smoke` | 检查首 20 份的格式和数值，并检查消费窗口连续性 |
| `--samples 20` | 接收 20 份后退出；0 或省略表示不限制份数 |
| `--duration 5` | 约 5 秒后结束接收循环；0 或省略表示不限制时长 |
| `--config 路径` | 选择 YAML，默认 `configs/bench.yaml` |

`--samples` 与 `--duration` 同时提供时，达到任一限制就结束。`--smoke` 与小于 20 的正数 `--samples` 不能组合；时长不足以收到 20 份时报告 `INCOMPLETE`。对进入接收循环的 `run`／`replay`，退出码 0 表示整次运行 `PASS`，1 为错误，2 为未完成；命令参数解析失败，以及 `devices` 没有枚举到设备，也返回 2。首批 `smoke=PASS` 后仍可能发生过载，要查看最终摘要与退出码。普通 `run` 也持续领取输入，但不做首批数值扫描。

构造 Pipeline 时就失败的情况可能只有 `type="error"` 输出，没有最终 `summary`。Ctrl+C 在进入消费循环后会记录 `reason="interrupted"`；状态仍按是否完成所需冒烟样本判定，因此人工中断不一定返回非零退出码。

### 11.2 运行算法自己的入口

用仓库现有接收示例或自己的算法脚本创建 Pipeline，不另行启动供数服务：

```bash
EVK4_USB_DEVICE=/dev/bus/usb/004/003 \
  ./scripts/dev.sh exec python3 examples/consume.py
```

设备路径需替换为当前 EVK4 节点。`exec` 每次新建一个临时开发容器，不是附着已有服务；只有显式设置 `EVK4_USB_DEVICE` 时才映射相机。把示例替换为 `/workspace/algorithm/receive_demo.py` 或模型入口即可运行自己的代码。当前 `dev.sh` 不自动传递 GPU 参数，GPU 模型需先补齐相应容器环境。

### 11.3 RAW 功能联调

仓库可用官方 RAW writer 生成一个小样本，并使用专门的功能检查配置：

```bash
./scripts/dev.sh exec ./build/make_fixture build/fixture.raw
./scripts/dev.sh replay --input build/fixture.raw \
  --config configs/replay-check.yaml --smoke
```

该样本有 50 个完整窗和不足一窗的尾部。`replay-check.yaml` 使用 640×360 输出、4 MiB 事件队列及 2 秒积压／领取上限。SDK 文件解码可能成批送入数据，因而该配置用于功能与数值联调，不能作为默认 20 ms 期限的低延迟性能证明。

项目外文件可只读挂载：

```bash
EVK4_DATA_DIR=/path/to/recordings ./scripts/dev.sh replay \
  --input /data/sample.raw --config configs/replay-check.yaml --smoke
```

从 Python 创建 RAW 流水线时使用同一接口：

```python
with Pipeline(config_path="configs/replay-check.yaml",
              input_raw="/data/sample.raw") as pipeline:
    # 继续使用同一套 info / read / packet 接口。
    ...
```

首期 RAW 按源时间 1× 节奏送入同一条构建链路，用于数值和接入联调；不另加回放速率接口。文件源仍遵守本版连续交付和超载结束规则。回放时不应用设备 bias／ROI，文件已经反映录制时的设备筛选；tensor 配置仍生效。EOF 后完整窗口取完，再收到 `reason == "eof"` 的 `StreamEnded`，未完整覆盖的尾窗舍弃。

### 11.4 运行镜像与当前范围

无需挂载源码的运行镜像可单独构建：

```bash
./scripts/build.sh --runtime
docker run --rm --init \
  --device /dev/bus/usb/004/003 \
  evk4-stream:runtime run --smoke --samples 20
```

将 USB 节点替换为本机设备路径。运行镜像包含 SDK、应用与配置，入口为 `python3 -m evk4_pipeline`。它使用同一 Pipeline；内置命令为 `devices`、`run` 和 `replay`。

如果交付的是运行镜像，而算法方要运行自己的 Python 文件，需要覆盖 CLI 入口。例如先将算法脚本放在项目的 `algorithm/receive_demo.py`，再执行：

```bash
docker run --rm --init \
  --device /dev/bus/usb/004/003 \
  --mount "type=bind,source=$(pwd)/algorithm,target=/algorithm,readonly" \
  --entrypoint python3 \
  evk4-stream:runtime /algorithm/receive_demo.py
```

运行镜像的工作目录为 `/opt/evk4`，默认 `configs/bench.yaml` 指向镜像内配置。使用自定义配置时另外挂载该文件，并把容器内路径传给 `Pipeline(config_path=...)`。这个命令只示范已存在模块的 CPU 接收方式；模型依赖和 GPU 环境仍需按第 3 节准备。

**当前没有实现 GUI。** 当前没有 `gui`、`status` 或独立 `stop` 命令，也没有可附着的供数服务。命令行持续运行时通过 Ctrl+C 停止，API 使用 `pipeline.stop()`。后续增加显示入口时仍应由同一个应用持有 Pipeline。

## 12 交接确认与完成标准

正式算法接入时，双方只需先确认以下信息：

| 信息 | 应提供的内容 |
|---|---|
| 输入定义 | 训练预处理代码，或明确的表示、分窗、极性、插值、缩放与裁剪规则 |
| 输入对象 | NumPy 或约定的 PyTorch 适配，shape、dtype、batch 与设备 |
| 数值样本 | 一小段事件及双方认可的期望 tensor |
| 运行条件 | 模型环境、典型与较慢单窗耗时、目标更新率与延迟起止点 |
| 时间连续性 | 能否跳窗、流结束／时间重置后如何处理历史状态 |

接收示例通过说明链路可用；事件小样本与期望 tensor 的对比确认数值含义；真实模型在目标机器上的持续运行确认负载和延迟。三者分别完成，不用“收到数组”替代模型输入正确性或实时性结论。

### 12.1 已完成的验证

2026-10-14，在 Intel Core i5-12500H、Docker、OpenEB 5.2.0 和 EVK4 `00050673` 上完成：

| 检查 | 结果 |
|---|---|
| 原生数值用例、Python 集成与冒烟摘要用例 | CTest 两个测试目标全部通过，Python 目标内含 6 个用例 |
| 全幅、20 ms 窗口、5 bins、双极性 | 5.019 s 接收 250 份连续 `[10,720,1280]` 张量，首 20 份数值冒烟通过 |
| bias 与设备 ROI | ON／OFF bias 各设 10 并读回；ROI `[320,180,640,360]`，半尺寸下 20 份张量区域外均为零，区域内有活动 |
| 恢复默认设置 | bias 读回 0、全幅 ROI，最终版本再接收 20 份通过 |
| 独立运行镜像 RAW 回放 | `replay-check.yaml` 下接收 50 个完整窗，首 20 份冒烟通过，正常 EOF；未输出尾窗 |

5 秒实机运行中，收到约 1224 万事件，最大源时间积压 7.866 ms，最大队列等待 6.725 ms。封窗所需事件／进度块进入本机队列至发布的 P99 为 0.340 ms；发布至领取的 P99 为 0.104 ms。这两个耗时不包含固定窗口等待、设备与 USB 延迟，也不是整窗所有累积工作的总耗时。CLI 长时间运行时，分位数取最近最多 4096 份输入。

上述真机日志的 `queue_capacity_bytes` 为 67,067,880，来自约 64 MiB 的队列配置；当前 `configs/bench.yaml` 默认已为 256 MiB。历史数据对应当时的测试条件，不是对当前所有默认参数的重新测量。

设备矩形 ROI 使用官方 `I_ROI.set_lines` 设置行列掩码；SDK 的窗口设置路径在事件验证中出现右侧／底部多一行的边界现象，行列接口验证通过。`info.roi` 来自已应用的 SDK 掩码，实际事件边界另由上述真机检查确认。

这些结果验证三模块通路；正式模型输入对齐、模型负载下的持续性能及 GUI 尚未验证。

### 12.2 API 交接核验

2026-10-14 的交接核验使用当前开发镜像、现有合成 RAW 和 `replay-check.yaml`，实际确认构造后 `info` 可读、返回只读 float32 数组、完整消费 50 个窗口后收到 `eof`、归还后 `packet.array` 置为 `None`，以及 `stats()` 返回字段。此项为无相机的功能核验，使用半尺寸和 2 秒积压／领取上限，不用于证明默认 20 ms 配置下的实时性。

本次还直接执行了第 4 节的 `main` 示例，使用半尺寸 RAW 参数接收 20 份后返回 0；运行镜像覆盖入口执行 Python 并调用 RAW CLI，得到 `PASS`、50 份完整输入和正常 `eof`。文中 Python／Shell 示例通过语法检查，默认 YAML 与仓库配置一致。

PyTorch 适配仍是本文示例，当前基础镜像未安装 PyTorch；它尚未经过目标模型或 GPU 验证。

算法方下一步提供最小模型调用，并共同确认第 9 节适配是否符合最终输入要求，再测目标模型的持续运行表现。
