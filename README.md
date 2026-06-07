# AegisFlow

AegisFlow 是一个基于 C++20 的实时风控服务原型。项目当前聚焦在事件接入、基础决策返回和用户实时特征计算，目标是逐步构建一个可扩展的低延迟风控决策引擎。

当前核心链路：

```text
Protobuf Event
 -> HTTP Server
 -> RiskService
 -> WorkerPool
 -> FeatureStore
 -> Decision + FeatureSnapshot
```

## 项目状态

当前进度：第 2 周，特征状态框架。

已完成能力：

```text
HTTP 服务启动
/health 健康检查
/v1/event/report 单事件上报
Protobuf 请求解析与响应序列化
默认风控决策返回
用户维度实时特征更新
滑动窗口计数
最近行为序列
FeatureStore 分片加锁
WorkerPool 异步执行
基础日志输出
```

当前决策逻辑仍保持简单：

```text
user_id 非空 -> PASS, risk_score = 0
user_id 为空 -> REVIEW, risk_score = 10
```

## 核心特征

当前只实现用户维度特征：

| 特征 | 含义 | 窗口 |
|---|---|---:|
| `user_login_1m` | 用户 1 分钟内登录次数 | 60 秒 |
| `user_login_5m` | 用户 5 分钟内登录次数 | 300 秒 |
| `user_login_1h` | 用户 1 小时内登录次数 | 3600 秒 |
| `user_login_fail_5m` | 用户 5 分钟内失败登录次数 | 300 秒 |
| `recent_actions` | 用户最近行为序列 | 最新 20 条 |

特征通过 `Decision.features` 返回，方便调试和验收。

## 架构说明

```text
                 +----------------+
HTTP Request --->| HttpServer     |
                 +-------+--------+
                         |
                         v
                 +-------+--------+
                 | RiskService    |
                 +-------+--------+
                         |
                         v
                 +-------+--------+
                 | WorkerPool     |
                 +-------+--------+
                         |
                         v
                 +-------+--------+
                 | FeatureStore   |
                 +-------+--------+
                         |
             +-----------+-----------+
             v                       v
       UserState              SlidingCounter
```

并发模型：

```text
同一个 user_id 会固定路由到同一个 shard
每个 shard 内部用一把 mutex 保护状态
不同 shard 可以并发更新
单次更新只持有一把 shard 锁
```

## 目录结构

```text
AegisFlow/
├── CMakeLists.txt
├── README.md
├── config/
│   └── server.conf
├── include/aegisflow/
│   ├── app/
│   ├── config/
│   ├── feature/
│   ├── log/
│   ├── net/
│   └── runtime/
├── proto/
│   ├── event.proto
│   └── decision.proto
├── scripts/
│   └── build.sh
├── src/
│   ├── app/
│   ├── config/
│   ├── feature/
│   ├── log/
│   ├── net/
│   ├── runtime/
│   └── main.cpp
├── tests/
└── tools/
    └── send_event.cpp
```

## 环境要求

```text
CMake >= 3.20
C++20
Protobuf
Boost.Asio
Boost.Beast
```

Ubuntu / Debian 示例：

```bash
sudo apt update
sudo apt install -y cmake g++ protobuf-compiler libprotobuf-dev libboost-all-dev
```

## 构建

使用脚本构建：

```bash
./scripts/build.sh
```

或手动构建：

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

构建产物：

```text
build/AegisFlow
build/send_event
```

## 配置

服务启动时需要传入配置文件路径：

```bash
./build/AegisFlow config/server.conf
```

推荐配置格式：

```text
http_server.host = 0.0.0.0
http_server.port = 8080
http_server.io_threads = 1
http_server.max_body_size = 1048576

log.level = INFO
log.file = logs/log.log
```

说明：

```text
host 控制监听地址
port 控制监听端口
io_threads 控制 HTTP 会话处理线程数
max_body_size 控制请求体最大字节数
log.file 所在目录需要提前存在
```

配置键以当前 `main.cpp` 读取的 `http_server.*` 为准。

## 运行服务

```bash
mkdir -p logs
./build/AegisFlow config/server.conf
```

启动成功后，服务会监听配置中的地址和端口。

## 健康检查

```bash
curl http://127.0.0.1:8080/health
```

期望响应：

```json
{"status": "ok"}
```

## 事件上报接口

接口：

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

响应中的核心字段：

```text
decision.event_id
decision.user_id
decision.action
decision.risk_score
decision.reasons
decision.cost_us
decision.features
```

`decision.features` 当前包含：

```text
features.user_id
features.user_login_1m
features.user_login_5m
features.user_login_1h
features.user_login_fail_5m
features.recent_actions
```

## 客户端示例

启动服务后，可以使用内置客户端发送一条登录事件：

```bash
./build/send_event
```

也可以指定地址、端口和用户：

```bash
./build/send_event 127.0.0.1 8080 user_001
```

发送空用户事件：

```bash
./build/send_event 127.0.0.1 8080 --empty-user
```

正常用户示例输出：

```text
event_id=1001
user_id=user_001
action=PASS
risk_score=0
cost_us=...
features.user_login_1m=1
features.user_login_5m=1
features.user_login_1h=1
features.user_login_fail_5m=0
```

连续发送同一用户的 3 次 `LOGIN SUCCESS` 后，特征计数应累加：

```text
features.user_login_1m=3
features.user_login_5m=3
features.user_login_1h=3
features.user_login_fail_5m=0
```

## 错误响应

| 场景 | 状态码 | 说明 |
|---|---:|---|
| 未知路由 | 404 | 仅支持 `/health` 和 `/v1/event/report` |
| Content-Type 错误 | 415 | 事件接口需要 Protobuf 请求体 |
| Body 过大 | 413 | 超过 `max_body_size` |
| Protobuf 解析失败 | 400 | 请求体不是合法 `ReportEventRequest` |
| 缺少 event | 400 | `ReportEventRequest` 未携带 `event` |
| 响应序列化失败 | 500 | 服务内部异常 |

## 测试

单元测试位于 `tests/`：

```text
test_sliding_counter.cpp
test_recent_action_window.cpp
test_user_state.cpp
test_feature_store.cpp
```