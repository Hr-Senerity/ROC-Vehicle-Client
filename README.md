# ROC-Vehicle-Client

面向 ROC-SYSTEM 的独立 C++17 车端通信库，核心不依赖 ROS。
当前程序版本为 `0.2.1-rc2`，CMake 安装版本为 `0.2.1`。
ROS1/ROS2 可作为上层适配器接入，不属于核心库。

## 功能与范围

- JSON Device Protocol v1：WS/WSS、hello/heartbeat/telemetry、ack/error、
  `task.available`、sequence 持久化和自动重连。
- HTTP/HTTPS：接受任务、租约、manifest、流式下载和状态上报。
- map package v2：逐文件校验，保留原始 PGM+YAML 和地图元数据。
- road-network v1/v2：拓扑、曲线和平台生成的有序 samples，不自行重采样。
- SQLite 状态事件 Outbox：发送前持久化，重启后用原 event/request/lease 精确重放。
- 长度、Content-Type、SHA-256、结构/语义校验；耐久原子保存或受控接收器提交。

不实现旧二进制协议、HTTP 周期找任务、WS/Base64 制品传输、动态能力协商、
自动格式降级或 CSV 消费。

`delivered` 仅表示资源完成校验并耐久保存或受控接收器提交，
不表示机器人已经加载、应用地图或执行轨迹。

验证平台为 Linux/amd64、Ubuntu 22.04、GCC 11.4 和 Clang 14。
通信实现支持 TLS，但生产 TLS、长稳、Windows 接收器和机器人加载不属于当前
认证范围。pre-1.0 版本不承诺稳定 ABI。

## 依赖和许可

| 依赖 | 锁定版本 | 用途 | 上游许可 |
| --- | --- | --- | --- |
| Boost.Asio/Beast | 1.92.0 | HTTP/WebSocket | BSL-1.0 |
| jsoncons | 1.7.0 | JSON 与格式检查 | BSL-1.0 |
| OpenSSL | 3.5.8 | TLS/SHA-256 | Apache-2.0 |
| SQLite | 3.53.4 | 状态事件持久化 | 上游 public-domain 声明 |

构建工具：CMake 3.20+、C++17 编译器、Ninja、Python 3。
测试使用 CTest、自有测试程序和 Python unittest，不要求 Catch2。
下载来源及 SHA-256 锁定在 `docker/dev/Dockerfile` 和 `cmake/Dependencies.lock.cmake`。

Vehicle 自有代码采用 [MIT](LICENSE)，不改变第三方许可。本仓库不附带第三方
实现或运行二进制。上游参考：[Boost](https://www.boost.org/LICENSE_1_0.txt)、
[jsoncons](https://github.com/danielaparker/jsoncons/tree/v1.7.0)、
[OpenSSL](https://github.com/openssl/openssl/blob/openssl-3.5.8/LICENSE.txt)、
[SQLite](https://sqlite.org/copyright.html)。
重新分发依赖或静态链接二进制时，须另行满足相应版权、许可及归属声明要求。

## 容器构建与测试

推荐独立 Docker，不向 Windows 或 WSL 宿主安装项目依赖。在项目根目录执行：

```sh
# 首次创建镜像需要网络，依赖只安装到镜像内。
docker compose build dev

# GCC/Clang 全量构建与测试。
docker compose run --rm dev bash tools/container-build.sh

# 无需服务器的版本示例。
docker compose run --rm dev /build/container-gcc/roc_vehicle_info

# Linux 客户端驱动帮助。
docker compose run --rm dev bash tools/container-driver.sh --help

# 源码导出边界测试。
docker compose run --rm dev python3 -Werror tests/tools/test_source_candidate_export.py
```

Windows 可在 WSL 终端执行。开发容器默认无网络、源码挂载只读、构建使用
专用卷 `roc-vehicle-client-build`，不挂载 Docker socket。
通信/TLS 测试使用受控本地端点，不需要真实服务器或 Device token。
LSan 使用独立专用测试容器权限，不要为测试修改宿主权限。

## 安装与 CMake 接入

在依赖已经准备好的 Linux 环境或开发容器中：

```sh
cmake -S . -B /build/library -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DROC_VEHICLE_BUILD_TESTS=OFF \
  -DROC_VEHICLE_BUILD_EXAMPLES=OFF
cmake --build /build/library --parallel 2
cmake --install /build/library --prefix /build/install
```

上层项目使用安装后的目标，不引用本项目内部源码目录：

```cmake
find_package(ROCVehicleClient 0.2.1 CONFIG REQUIRED)
add_executable(my_vehicle main.cpp)
target_compile_features(my_vehicle PRIVATE cxx_std_17)
target_link_libraries(my_vehicle PRIVATE roc::vehicle_client)
```

将安装前缀加入 `CMAKE_PREFIX_PATH`，并提供 OpenSSL/SQLite 开发依赖。
完整版本可通过 `ROCVehicleClient_FULL_VERSION` 或
`roc::vehicle::library_version()` 查询。
可运行的安装消费示例位于 `tests/consumer/`。

## 接入与运行

主要入口为 `DeviceClient`、`DeviceTaskHttpClient`、`DurableStateStore`、
`FileSink`、`PackageSink` 和受控接收器，头文件位于 `include/roc/vehicle/`。

WS 回调应快速返回，只向队列投递任务。阻塞 HTTP 下载和交付由独立工作线程
处理。应用负责保持客户端、token provider 和回调捕获对象的生命周期；
不要在 I/O 回调中销毁客户端或执行阻塞下载。

Linux 示例驱动串联了通知队列、单个 HTTP worker、Outbox 和文件交付。
模板为 `examples/driver-config.template.json` 与
`examples/owning-map-context.template.json`，替换占位符后才能使用。

```sh
roc_vehicle_driver --config /private/config.json --precheck
roc_vehicle_driver --config /private/config.json --run
# 非 loopback 端点需明确启用；生产应使用 HTTPS/WSS。
roc_vehicle_driver --config /private/config.json --run --allow-remote
```

设备鉴权为 `Authorization: Device <token>`，任务使用 `X-Task-Lease`。
Token 从受保护文件/provider 读取，不放在命令行。
专用状态目录为 `0700`，敏感文件为 `0600`。
不要提交真实地址、token、lease、私钥、SQLite 状态或带凭据日志。
HTTP/WS 仅用于明确选择的 Demo 环境，不是生产安全传输。

重启时先处理持久化事件，未确认终态保留原请求和租约。
需要人工协调的事件阻止再次接受同一任务，不用新 event/attempt 补报旧交付。
发送 SIGTERM/SIGINT 进行受控停止，不要自行清空持久化状态。

## 源码结构

`include/` 公共接口；`src/` 核心实现；`apps/` 驱动；
`examples/` 示例；`tests/` 测试；`cmake/` 构建配置；
`docker/` 容器配方；`tools/` 通用脚本。

运行校验随源码提供，构建不需要外部合同快照或内部文档。
源码发行不附带运行镜像、安装二进制、真实配置或私有状态。
