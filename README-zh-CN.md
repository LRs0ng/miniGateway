# miniGateway
## 简体中文 | [ENGLISH](README.md)
轻量级的边缘计算网关

一个轻量级、单进程的 C++ 边缘计算网关，支持可插拔协议驱动、可配置数据处理与设备控制，并可通过可选的 MQTT 或自定义发布插件扩展事件输出。

目前已经实现：

- 支持 Poll（轮询） 与 Push（推送） 两类采集源；
- 固定间隔、全局顺序执行的 Poll 调度；
- 统一 `RawBatch -> Event/Reading` 数据模型；
- 通过 `IDataProcessor` 串联规则和推理处理；
- 通过 `IEventPublisher` 输出
- 通过JSON、PluginRegistry 和 LoadLibrary/dlopen 在启动期装配共享库插件

核心只依赖四个抽象接口：
- IProtocolDriver：协议设备产生 RawBatch；
- IDataProcessor：处理或追加 Event 中的 Reading；
- IEventPublisher：把最终 Event 交给外部系统；
- IDeviceControlSource：给device发送控制指令；

## Quick Start
### 1. 默认启动内容
第一次运行建议直接使用平台缺省配置：
- Windows 自动选择 `example/configs/defconfig_windows.json`；
- Linux 自动选择 `example/configs/defconfig_linux.json`；
- Poll Simulator 模拟周期采集温度和压力；
- Push Simulator 模拟主动推送振动数据；
- Threshold、Window Average 和 Inference 三个 Processor 处理采集结果；
- Print Event Publisher 把 Event/Reading 输出到终端；
- Periodic Control Source 轮流向两个 Simulator 发送控制指令；
- 程序运行约 `10000 ms` 后自动停止并打印汇总统计。

### 2. 准备构建环境

最低要求如下：

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows 或 Linux；当前不支持 macOS |
| CMake | `3.20` 或更高版本 |
| C++ 编译器 | 支持 C++20 的 MSVC、GCC 或 Clang |
| C 编译器 | 仅在显式启用 `GATEWAY_ENABLE_MQTT=ON` 构建 Paho MQTT C 时需要支持 C99；默认 OFF 构建不需要 |
| 构建工具 | MinGW Make、Unix Make 等与生成器匹配的工具 |
| 外部服务 | 默认配置不需要；只有实际启用 MQTT Publisher 时才需要 MQTT 服务器 |

### 3. 下载
```bash
git clone --recursive https://github.com/LRs0ng/miniGateway.git
```
or
```bash
git clone https://github.com/LRs0ng/miniGateway.git
cd miniGateway
git submodule update --init --recursive
```

### 4. Windows + MinGW
进入项目文件夹后：
```powershell
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
```

### 5. linux
安装必要环境：
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```
构建，进入项目文件夹后：
```bash
mkdir build
cd build
cmake ..
make
```
### 6、运行
#### windows
```powershell
.\miniGateway.exe
```
### linux
```bash
./miniGateway
```
### 7. 使用 MQTT
#### windows
```powershell
cmake .. -G "MinGW Makefiles" -DGATEWAY_ENABLE_MQTT=ON -DGATEWAY_CONFIG="configs/defconfig_mqtt_windows.json"
```
#### linux
```bash
cmake .. -DGATEWAY_ENABLE_MQTT=ON -DGATEWAY_CONFIG="configs/defconfig_mqtt_linux.json"
```
构建完成后，打开 `build\config.json` 文件
```json
    {
      "id": "mqtt",
      "type": "mqtt",
      "library": "./gateway_mqtt_event_publisher.dll",
      "enabled": false,
      "config": {
        "host": "127.0.0.1",
        "port": 1883,
        "keepalive": 30,
        "client_id": "edge-gateway",
        "topic_prefix": "edge/events",
        "qos": 1,
        "retain": false,
        "clean_session": true,
        "username": "",
        "password": "",
        "reconnect_delay": 1,
        "reconnect_delay_max": 30,
        "reconnect_exponential_backoff": true
      }
    }
```
更改默认配置，`"enabled": false` 置为 `"enabled": true`，修改默认`"host": "127.0.0.1"`和端口`"port": 1883`与MQTT服务器匹配，配置`"username": ""`和`"password": ""`
#### 接收MQTT消息

该程序发布的主题名称格式为：
```text
<topic_prefix>/<device_id>/event
```
见json配置文件中的`"topic_prefix": "edge/events"`字段，如果我要使用mqtt接收虚拟采集设备 `simulator_poll` 的信息，从json可知`simulator_poll` 的 `"id": "machine-01"`，因此客户端需要订阅的mqtt主题为 `edge/events/machine-01/event`。

## 1. 系统总体框图

```mermaid
flowchart TB
    CONFIG["config.json"] --> BOOT["gateway_example<br/>load_config() + PluginRegistry"]

    subgraph CORE["gateway_core（单进程核心）"]
        RUNTIME["GatewayRuntime<br/>配置校验与生命周期管理"]
        SCHEDULER["SchedulerEngine<br/>固定间隔顺序调度"]
        IO_GATE["每设备 I/O gate<br/>std::timed_mutex"]
        RAW_QUEUE["BoundedQueue&lt;RawBatch&gt;<br/>采集汇流"]
        WORKER["Processing worker"]
        NORMALIZER["Normalizer<br/>RawBatch -> Event/Reading"]
        PIPELINE["ProcessingPipeline"]
        FAN_OUT["Publisher fan-out"]
        CONTROL_QUEUE["ControlQueue<br/>控制与 deadline"]
        DISPATCHER["DeviceControlDispatcher<br/>设备路由与异步执行"]

        RUNTIME -.->|"创建、启动与停止"| SCHEDULER
        RUNTIME -.->|"创建、启动与停止"| WORKER
        RUNTIME -.->|"创建、启动与停止"| DISPATCHER
        SCHEDULER -->|"到期的 Poll Group"| IO_GATE
        RAW_QUEUE --> WORKER --> NORMALIZER --> PIPELINE --> FAN_OUT
        CONTROL_QUEUE --> DISPATCHER --> IO_GATE
    end

    subgraph PLUGINS["独立共享库插件（核心之外）"]
        DRIVERS["协议 Driver<br/>IProtocolDriver<br/>Poll / Push / Control"]
        PROCESSORS["数据处理插件<br/>IDataProcessor"]
        PUBLISHERS["事件发布插件<br/>IEventPublisher<br/>Print / MQTT / ..."]
        CONTROL_SOURCES["控制来源插件<br/>IDeviceControlSource<br/>MQTT / HTTP / IPC / ..."]
    end

    subgraph EXTERNAL["外部设备与系统"]
        DEVICES["现场设备<br/>Modbus / OPC UA / Camera / ..."]
        SERVICES["消息 Broker / 云平台 / 业务系统"]
        CONTROL_CLIENTS["控制客户端"]
    end

    BOOT -->|"LoadLibraryW / dlopen<br/>create_plugin"| DRIVERS
    BOOT -->|"LoadLibraryW / dlopen<br/>create_plugin"| PROCESSORS
    BOOT -->|"LoadLibraryW / dlopen<br/>create_plugin"| PUBLISHERS
    BOOT -->|"LoadLibraryW / dlopen<br/>create_plugin"| CONTROL_SOURCES
    BOOT -->|"GatewayConfig + 插件实例"| RUNTIME

    DEVICES <-->|"协议通信"| DRIVERS
    IO_GATE -->|"poll() / control()"| DRIVERS
    DRIVERS -->|"Poll 返回或 Push SampleSink<br/>RawBatch"| RAW_QUEUE
    PIPELINE <-->|"process(Event)"| PROCESSORS
    PROCESSORS -->|"ProcessingContext::submit_control()"| CONTROL_QUEUE
    FAN_OUT -->|"publish(Event)"| PUBLISHERS --> SERVICES
    CONTROL_CLIENTS --> CONTROL_SOURCES -->|"ControlSink"| CONTROL_QUEUE
```

实线表示运行期的数据或控制流，虚线表示 `GatewayRuntime` 的生命周期管理关系。Driver、Processor、Publisher 和 ControlSource 的具体实现都位于共享库中；核心只通过四个接口持有和调用它们

## 2. 数据流路径

```text
              Poll Driver shared library                  Push Driver shared library
                        |                                             |
                        |                                             |
SchedulerEngine -> PollCallback -> GatewayRuntime::poll_group         |
                        |                                             |
                        + ------------------ RawBatch ----------------+
                                                |
                                                v
                                   BoundedQueue<RawBatch>
                                                |
                                      processing worker
                                                |
                          Normalizer -> ProcessingPipeline
                                                |
                                      publish_event(Event)
                                                |
                                                |
                                                v                                      
                                       Print Event Publisher 
                                       (test shared library)
```
## 3. 控制流路径
```text
外部 MQTT/HTTP/IPC/...                 Processing worker
          |                                  |
IDeviceControlSource                 IDataProcessor::process
          |                           ProcessingContext::submit_control
          +------------------+---------------+
                             |
                     GatewayRuntime::submit_control
                             |
                     DeviceControlDispatcher
                     -> bounded ControlQueue
                             |
                      control worker (FIFO)
                             |
                    per-device timed_mutex <----- Poll scheduler worker
                             |                     uses same device gate
                    IProtocolDriver::control
                             |
                     ControlCompletion callback
                             |
                    外部协议响应/状态更新
```

## 4. 启动接入插件
插件装配分为四步：

1. [CMakeLists.txt](CMakeLists.txt) 把每个实现编译成独立 `SHARED` 库；可执行程序只链接 `gateway_core`。
2. [config.json](config.json) 为每个插件实例声明 `type`、统一的 `library` 字段和私有 `config`。`library` 必须是带平台后缀的完整库文件名（例如 `.dll` 或 `.so`），可以是相对路径或绝对路径。
3. [PluginRegistry::load_dynamic_plugins()](include/gateway/plugin_registry.hpp) 在 `main()` 启动 Runtime 前一次性调用：Windows 使用 `LoadLibraryW`/`GetProcAddress`，Linux 使用 `dlopen(RTLD_NOW | RTLD_LOCAL)`/`dlsym`。
4. Loader 从每个库解析 `create_plugin(const char*)` 和 `destroy_plugin(void*)`，登记工厂；随后 `PluginRegistry::create()` 按 JSON 顺序创建实例。

每个插件库都通过 [plugin_api.hpp](include/gateway/plugin_api.hpp) 导出同名 C 符号。C ABI 只覆盖入口函数；创建出的对象仍实现 C++ 接口，因此主程序、`gateway_core` 与插件仍应使用 ABI 兼容的编译器和运行库。

## 5. 编写自己的插件

接入真实协议 Driver：

1. 实现 `IProtocolDriver`，根据能力返回 Poll 或 Push；
2. Poll Driver 实现 `poll(group, deadline)`；Push Driver 保存并调用 `SampleSink`；
3. 导出 `extern "C" void* create_plugin(const char*)`，在库内解析私有 JSON 并创建对象；
4. 导出 `extern "C" void destroy_plugin(void*)`，在创建对象的同一库内完成析构和释放；
5. 新建独立 `SHARED` CMake 目标并链接 `gateway_core`；
6. 在 JSON 的 Device 中填写 `driver`、带后缀的 `library` 完整路径和 插件私有`config`（如果插件自身有配置需求的话）。

Processor 、 Publisher 和 ControlSource 的步骤相同，分别实现 `IDataProcessor` 、 `IEventPublisher` 和 `IDeviceControlSource`。

## 6. 参考插件样例
| 插件类别 | 核心接口 | 示例共享库 | JSON 配置位置 |
| --- | --- | --- | --- |
| Driver | `IProtocolDriver` | [`gateway_poll_simulator`（`simulator_poll`）](tests\simulation\poll_simulator.cpp)、[`gateway_push_simulator`（`simulator_push`）](tests\simulation\push_simulator.cpp) | `devices[]` 的 `driver/library/driver_config` |
| Processor | `IDataProcessor` | [`gateway_threshold_processor`（`threshold`）](src\processor\threshold_processor.cpp)、[`gateway_window_average_processor`（`window_average`）](src\processor\window_average_processor.cpp)、[`gateway_inference_processor`（`inference`）](src\processor\inference_processor.cpp) | `processors[]` |
| Event Publisher | `IEventPublisher` | [`gateway_print_event_publisher`（`print`）](tests\simulation\print_event_publisher.cpp) | `event_publishers[]` |
| Device Control Source | `IDeviceControlSource` | [`periodic_control_source`](tests\simulation\periodic_control_source.cpp) | `device_control_sources[]` |

这四类库都只导出相同的 `create_plugin` 和 `destroy_plugin` 两个 C 符号，插件类别由其所在 JSON 配置区决定，实例进入核心后仍通过对应的 C++ 虚接口工作。

## License
The source code of this project is licensed under the MIT License.
This project uses the following third-party dependencies:
- nlohmann/json
- - License: MIT
- - Repository: https://github.com/nlohmann/json.git
- eclipse-paho/paho.mqtt.c
- - License: EPL-2.0
- - Repository: https://github.com/eclipse-paho/paho.mqtt.c.git