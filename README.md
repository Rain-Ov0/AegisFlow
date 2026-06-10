# AegisFlow

AegisFlow 是一个基于 C++20 的实时风控服务原型。项目当前完成到第 3 周：在基础 HTTP 接入、默认决策和用户窗口特征之上，加入了 IP / 设备关联特征、热点 IP TopK 和 Count-Min Sketch 高频风险行为估计。

核心链路：

```text
Protobuf Event
 -> HTTP Server
 -> RiskService
 -> WorkerPool
 -> FeatureStore
 -> User / IP / Device / TopK / CMS features
 -> Decision + FeatureSnapshot
```

## 项目状态

当前进度：第 3 周，实时风控特征计算。

已完成能力：

```text
HTTP 服务启动
/health 健康检查
/v1/event/report 单事件上报
Protobuf 请求解析与响应序列化
默认风控决策返回
WorkerPool 异步执行 FeatureStore
用户维度滑动窗口计数
用户最近行为序列
IP 10 分钟 distinct user 统计
设备 10 分钟 distinct account 统计
Space-Saving TopK 热点 IP 估计
Count-Min Sketch 风险行为频次估计
FeatureStore 分片加锁
基础日志输出
```

当前决策逻辑仍保持简单：

```text
user_id 非空 -> PASS, risk_score = 0
user_id 为空 -> REVIEW, risk_score = 10
```

规则引擎尚未接入，`Decision.features` 用于调试和验收实时特征。

## 核心特征

| 特征 | 含义 | 窗口 / 语义 |
|---|---|---:|
| `user_login_1m` | 用户 1 分钟内登录次数 | 60 秒 |
| `user_login_5m` | 用户 5 分钟内登录次数 | 300 秒 |
| `user_login_1h` | 用户 1 小时内登录次数 | 3600 秒 |
| `user_login_fail_5m` | 用户 5 分钟内失败登录次数 | 300 秒 |
| `recent_actions` | 用户最近行为序列 | 最新 20 条 |
| `ip_distinct_user_10m` | 同一 IP 10 分钟内不同用户数 | 10 分钟 |
| `device_distinct_account_10m` | 同一设备 10 分钟内不同账号数 | 10 分钟 |
| `ip_topk_estimated_count` | IP 在 TopK 结构中的估计次数 | 运行周期累计 |
| `ip_in_topk` | IP 是否达到热点 TopK 展示条件 | 运行周期累计 |
| `cms_risk_behavior_count` | 风险行为 key 的 CMS 估计次数 | 运行周期累计 |

风险行为 key 当前格式：

```text
ip|scene|EventType|EventResult
```

示例：

```text
203.0.113.10|login|LOGIN|FAIL
```

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
      +------------------+------------------+
      v                  v                  v
  UserState        SlidingDistinct      TopK / CMS
```

FeatureStore 内部状态：

```text
user_id   -> UserState
ip        -> SlidingDistinct(user_id)
device_id -> SlidingDistinct(user_id)
ip        -> ShardedTopK
risk_key  -> ShardedCountMinSketch
```

并发模型：

```text
用户状态使用 64 个 shard
IP distinct 使用 64 个 shard
设备 distinct 使用 64 个 shard
TopK 使用 16 个 shard
CMS 使用 16 个 shard
每次只持有一把 shard 锁
不同维度分阶段更新，避免多锁嵌套死锁
```

## 数据结构复杂度

| 模块 | 更新复杂度 | 查询复杂度 | 空间复杂度 |
|---|---:|---:|---:|
| `SlidingCounter<60>` | `O(60)`，常数级 | `O(60)`，常数级 | `O(60)` |
| `RecentActionWindow<20>` | `O(1)` | `O(20)` | `O(20)` |
| `SlidingDistinct` | 均摊 `O(1)` | 均摊 `O(1)` | `O(owner 下 distinct member 数)` |
| `SpaceSavingTopK` | `O(log K)` | `O(1)` estimate | `O(K)` |
| `CountMinSketch` | `O(depth)` | `O(depth)` | `O(depth * width)` |
| `ShardedTopK` | `O(log K)` | `O(1)` estimate | `O(shard * K)` |
| `ShardedCountMinSketch` | `O(depth)` | `O(depth)` | `O(depth * width)` |

当前关键参数：

```text
user shard num = 64
ip/device distinct window = 10min
ip/device distinct bucket = 10s
ip/device distinct max members = 5000
topk shard num = 16
topk capacity per shard = 100
cms shard num = 16
cms depth = 4
cms total width = 16384
cms memory ~= 4 * 16384 * 8 = 512KB
```

## 边界处理

| 场景 | 当前行为 |
|---|---|
| `user_id` 为空 | 不创建用户状态，RiskService 返回 `REVIEW` |
| `ip` 为空 | 跳过 IP distinct、TopK、CMS |
| `device_id` 为空 | 跳过设备 distinct |
| 事件时间晚于 `now_ms` | 不更新 FeatureStore |
| 事件时间超过 1 小时历史窗口 | 不更新 FeatureStore，只返回已有用户快照 |
| 同一 IP 下同一用户重复出现 | distinct 仍只计 1 个用户 |
| 同一设备下同一账号重复出现 | distinct 仍只计 1 个账号 |
| distinct 超过 `max_members` | `SlidingDistinct::degraded()` 置 true，并拒绝新增 member |
| CMS 冲突 | 估计值可能偏大，不会低于真实计数 |
| TopK 误差 | 低频 key 可能被替换，高频 key 在足够请求量后保留 |

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
    ├── benchmark_feature_store.cpp
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
build/benchmark_feature_store
build/send_event
```

## 运行服务

服务启动时需要传入配置文件路径：

```bash
mkdir -p logs
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
features.ip_distinct_user_10m
features.device_distinct_account_10m
features.cms_risk_behavior_count
features.ip_topk_estimated_count
features.ip_in_topk
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

模拟同一 IP 连续登录多个账号后，响应中的调试特征应出现：

```text
features.ip_distinct_user_10m 增加
features.ip_topk_estimated_count 增加
features.ip_in_topk 在热点流量足够高后变为 true
```

模拟同一 IP 的失败登录攻击后，响应中的 CMS 估计值应升高：

```text
features.cms_risk_behavior_count 增加
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
test_sliding_distinct.cpp
test_space_saving_topk.cpp
test_count_min_sketch.cpp
test_feature_store_ip_device.cpp
test_feature_store_attack_flow.cpp
```

运行方式：

```bash
cd build
ctest --output-on-failure
```

重点验收项：

```text
同一 IP 下同一 user 多次出现，ip_distinct_user_10m 仍为 1
同一 IP 下多个 user，ip_distinct_user_10m 正确增长
同一 device 下多个账号，device_distinct_account_10m 正确增长
超过 10 分钟后 distinct member 过期
攻击 IP 连续失败登录后进入 TopK
攻击 IP 的 CMS 风险行为估计值升高
多线程并发写不同 user_id 不崩溃
多线程并发写同一个 user_id 计数正确
```

## 压测记录

第 3 周建议使用 FeatureStore 层微基准或 HTTP 压测分别观察核心计算开销和端到端开销。当前已使用 `benchmark_feature_store` 对 FeatureStore 核心路径做多轮本地压测。

运行方式：

```bash
./build/benchmark_feature_store
```

推荐流量模型：

```text
正常流量：
10000 user
1000 ip
5000 device
每个 ip 分布均匀
LOGIN SUCCESS

攻击流量：
1 个 attack_ip
1000 个 user
同一 attack_device
全部 LOGIN FAIL
```

期望现象：

```text
attack_ip 的 ip_distinct_user_10m 接近攻击用户数
attack_device 的 device_distinct_account_10m 接近攻击用户数
attack_ip 的 ip_topk_estimated_count 明显高于普通 IP
attack_ip 在热点流量足够高后 ip_in_topk 为 true
attack_ip 对应的 cms_risk_behavior_count 明显升高
```

目标
| 指标 | 目标 |
|---|---:|
| 单机 FeatureStore 吞吐 | `5w QPS+` |
| FeatureStore P99 | `< 1ms` |
| TopK K | `100` |
| CMS 内存 | `< 1MB` |
| 单个 IP distinct 上限 | `5000` |

本地多轮压测结果：

| 日期 | 机器 | 轮数 | normal_events | attack_events | QPS 平均 | QPS 最低 | QPS 最高 | P50 平均 | P95 平均 | P99 平均 | P99 最差 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2026-06-10 | mint-virtual-machine | 20 | 10000 | 1000 | 63178.85 | 50017.73 | 70395.95 | 13.65us | 22.85us | 43.95us | 64us |

攻击流量特征在 20 轮中保持一致：

```text
attack_ip_distinct_user_10m=1000
attack_device_distinct_account_10m=1000
attack_ip_topk_estimated_count=1000
attack_ip_in_topk=true
attack_cms_risk_behavior_count=1000
```

结论：

```text
FeatureStore 多轮 QPS 均达到 5w QPS+ 目标
P99 最差 64us，低于 1ms 目标
攻击 IP distinct、设备 distinct、TopK、CMS 特征均符合验收预期
```
