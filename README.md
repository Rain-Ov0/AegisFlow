# AegisFlow

AegisFlow 是一个基于 C++20 的实时风控服务原型。

第 1 周目标：跑通最小协议链路。

```text
Protobuf Event -> HTTP Server -> RiskService -> Protobuf Decision
```

当前版本只实现基础能力：

* HTTP 服务启动
* `/health` 健康检查
* Protobuf 请求解析
* 默认风控决策返回
* 基础日志输出

---

## 目录结构

```text
AegisFlow/
├── CMakeLists.txt
├── proto/
│   ├── event.proto
│   └── decision.proto
├── include/aegisflow/
├── src/
├── config/
│   └── server.conf
├── tools/
│   └── send_event.cpp
└── scripts/
    └── build.sh
```

---

## 环境依赖

```bash
sudo apt update
sudo apt install -y cmake g++ protobuf-compiler libprotobuf-dev libboost-all-dev
```

要求：

```text
CMake >= 3.20
C++20
Protobuf
Boost.Asio / Boost.Beast
```

---

## 编译

```bash
mkdir -p build
cd build
cmake ..
make -j
```

编译后生成：

```text
build/aegisflow
build/send_event
```

---

## 配置

默认配置文件：

```text
config/server.conf
```

示例：

```text
server.host=0.0.0.0
server.port=8080
server.io_threads=2
server.max_body_size=1048576

log.level=info
log.file=logs/aegisflow.log
```

---

## 启动服务

在项目根目录执行：

```bash
./build/aegisflow --config config/server.conf
```

如果当前版本还未支持 `--config` 参数，则默认读取：

```text
config/server.conf
```

---

## 健康检查

```bash
curl http://127.0.0.1:8080/health
```

期望返回：

```json
{"status":"ok"}
```

---

## 事件上报接口

### 单事件上报

```text
POST /v1/event/report
Content-Type: application/x-protobuf
```

请求体：

```text
aegisflow.v1.ReportEventRequest
```

响应体：

```text
aegisflow.v1.ReportEventResponse
```

当前默认决策逻辑：

```text
user_id 非空 -> PASS, risk_score = 0
user_id 为空 -> REVIEW, risk_score = 10
```

---

## 客户端测试

推荐使用测试客户端发送 Protobuf 请求：

```bash
./build/send_event
```

期望输出：

```text
event_id=1001 action=PASS risk_score=0
```

---

## 错误处理

服务端需要覆盖以下基础错误：

| 场景              | 状态码 |
| --------------- | --: |
| 未知路由            | 404 |
| Method 不支持      | 405 |
| Content-Type 错误 | 415 |
| Body 过大         | 413 |
| Protobuf 解析失败   | 400 |
| 服务内部异常          | 500 |

---

## 日志

第一周日志重点用于排查链路问题，至少包含：

```text
config loaded
server started
request received
protobuf parse failed
unknown route
response decision action
server error
```

---

## 第一周验收标准

```text
1. cmake 能编译通过
2. ./aegisflow --config config/server.conf 能启动
3. curl http://127.0.0.1:8080/health 返回 ok
4. 客户端发送 Protobuf Event，服务返回默认 PASS Decision
5. 日志能打印启动、请求、错误信息
```

---

当前阶段只追求：

```text
协议链路闭环
```
