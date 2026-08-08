# miniGateway
轻量级的边缘计算网关

目前已经实现：

- Poll 与 Push 两类采集源（模拟）；
- 固定间隔、全局顺序执行的 Poll 调度；
- 统一 `RawBatch -> Event/Reading` 数据模型；
- 通过 `IDataProcessor` 串联规则和推理处理；
- 通过 `IEventPublisher` 输出

核心只依赖三个抽象接口：
- IProtocolDriver：协议设备产生 RawBatch；
- IDataProcessor：处理或追加 Event 中的 Reading；
- IEventPublisher：把最终 Event 交给外部系统。

## 1.系统流程图：
```text
                               main thread
                                   |
                         read json & configure
                                   |
                                   v
                         +--------------------+
                         |   GatewayRuntime   |
                         +--------------------+
                           |                |
                  Poll path|                |Push path
                           v                v
                SchedulerEngine      PushSimulatorDriver
                (scheduler thread)      (push thread)
                           |                |
                           v                |
                 SequentialExecutor         |
                           |                |
                           v                |
                   Poll protocol driver     |
                           |                |
                           +------ RawBatch-+
                                   |
                                   v
                         BoundedQueue<RawBatch>
                                   |
                                   v
                         processing thread
                                   |
                 Normalizer -> ProcessingPipeline
                                   |
                                   |                         
                                   v                        
                             event_publisher    

```

## 2.数据主路径:

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

## 3.接入新插件

接入真实协议 Driver：

1. 实现 `IProtocolDriver`，根据能力返回 Poll 或 Push；
2. Poll Driver 实现 `poll(group, deadline)`；Push Driver 保存并调用 `SampleSink`；
3. 提供 `register_xxx_plugin(PluginRegistry&)`，登记类型字符串和无捕获工厂函数；
4. 新建独立 `SHARED` CMake 目标并链接 `gateway_core`；
5. 把目标链接到启动程序，并在 [linked_plugins.cpp](src/bootstrap/linked_plugins.cpp) 调用注册函数；
6. 在 JSON 的 Device 中填写相同 `driver` 类型及 `driver_config`。

Processor 和 Publisher 的步骤相同，分别实现 `IDataProcessor` 与 `IEventPublisher`。

## License
The source code of this project is licensed under the MIT License.
This project uses the following third-party dependencies:
- nlohmann/json
- - License: MIT
- - Repository: https://github.com/nlohmann/json.git