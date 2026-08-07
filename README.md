## miniGateway
轻量级的边缘计算网关

目前已经实现：

- Poll 与 Push 两类采集源（模拟）；
- 固定间隔、全局顺序执行的 Poll 调度；
- 统一 `RawBatch -> Event/Reading` 数据模型；

系统流程图：
```text
                               main thread
                                   |
                 make_config / drivers / processors / handler
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
                           FinalEventHandler 
                             (print_event)      

```

## License
The source code of this project is licensed under the MIT License.
This project uses the following third-party dependencies:
- nlohmann/json
- - License: MIT
- - Repository: https://github.com/nlohmann/json.git