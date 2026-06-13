# AegisFlow

AegisFlow 是一个基于 C++20 的实时风控决策服务原型。它通过 HTTP + Protobuf 接收登录、支付、点击、发布等行为事件，在内存中维护用户 / IP / 设备维度的实时特征，结合规则 DSL、黑名单缓存、TopK、Count-Min Sketch 等结构输出风控决策：

```text
PASS / REVIEW / REJECT
```

项目重点放在 C++ 后端工程能力：网络接入、Protobuf 编解码、线程池、分片状态、低锁并发、规则执行、缓存降级、单元测试和压测闭环。

## 核心能力

```text
HTTP 服务启动与健康检查
/v1/event/report 单事件上报
Protobuf 请求解析与响应序列化
WorkerPool 异步执行业务处理
用户维度滑动窗口计数
用户最近行为序列
IP 10 分钟 distinct user 统计
设备 10 分钟 distinct account 统计
Space-Saving TopK 热点 IP 估计
Count-Min Sketch 风险行为频次估计
规则 DSL 解析、校验与执行
规则 AST / 轻量 DAG 构建
相同 Condition 节点复用
AND / OR / NOT / 括号表达式
规则按 scene 匹配并按 priority 返回命中
DecisionAggregator 合并 PASS / REVIEW / REJECT
MySQL 黑名单启动加载
Redis 黑名单缓存可选接入
Bloom Filter + TTL LRU 本地黑名单判断
Redis / MySQL 不可用时核心链路降级运行
FeatureStore 微基准压测
HTTP / Protobuf 端到端压测
```

## 架构

```text
Client / Benchmark
        |
        v
   +------------+
   | HttpServer |
   +-----+------+
         |
         v
   +-------------+
   | RiskService |
   +------+------+
          |
          v
   +------------+
   | WorkerPool |
   +------+-----+
          |
          v
   +--------------+
   | FeatureStore |
   +------+-------+
          |
          +--> UserState
          +--> SlidingDistinct(IP / Device)
          +--> ShardedTopK
          +--> ShardedCountMinSketch
          |
          v
   +------------------+
   | BlacklistManager |
   +--------+---------+
            |
            v
      +------------+
      | RuleEngine |
      +-----+------+
            |
            v
   +--------------------+
   | DecisionAggregator |
   +---------+----------+
             |
             v
 Decision + FeatureSnapshot + Reasons
```

核心链路：

```text
ReportEventRequest
 -> HttpServer
 -> RiskService
 -> WorkerPool
 -> FeatureStore.updateAndGet
 -> BlacklistManager.checkEvent
 -> RuleEngine.evaluate
 -> DecisionAggregator.aggregate
 -> ReportEventResponse
```

## 特征体系

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
| `user_black_hit` | 用户是否命中黑名单 | 本地 + Redis |
| `ip_black_hit` | IP 是否命中黑名单 | 本地 + Redis |
| `device_black_hit` | 设备是否命中黑名单 | 本地 + Redis |

风险行为 key 格式：

```text
ip|scene|EventType|EventResult
```

示例：

```text
203.0.113.10|login|LOGIN|FAIL
```

## 规则 DSL

默认规则文件：

```text
config/rules.dsl
```

规则格式：

```text
RULE <name>
SCENE <scene|all>
PRIORITY <int>
IF <expr>
THEN <PASS|REVIEW|REJECT> REASON "<reason_code>"
```

示例：

```text
RULE login_fail_review
SCENE login
PRIORITY 100
IF user.login_fail_5m >= 5
THEN REVIEW REASON "too_many_failed_login"
```

表达式支持：

```text
AND OR NOT
()
>= <= > < == !=
number
true false
feature_name
```

当前支持的 DSL 字段：

| DSL 字段 | 类型 | 对应特征 |
|---|---|---|
| `user.login_1m` | number | `user_login_1m` |
| `user.login_5m` | number | `user_login_5m` |
| `user.login_1h` | number | `user_login_1h` |
| `user.login_fail_5m` | number | `user_login_fail_5m` |
| `ip.distinct_user_10m` | number | `ip_distinct_user_10m` |
| `device.distinct_account_10m` | number | `device_distinct_account_10m` |
| `cms.risk_behavior_count` | number | `cms_risk_behavior_count` |
| `ip.in_topk` | bool | `ip_in_topk` |
| `user.black_hit` | bool | `user_black_hit` |
| `ip.black_hit` | bool | `ip_black_hit` |
| `device.black_hit` | bool | `device_black_hit` |

默认规则覆盖：

```text
黑名单用户 / IP / 设备 -> REJECT
5 分钟失败登录过多 -> REVIEW
1 分钟 / 5 分钟登录频率突增 -> REVIEW
同一 IP 攻击多个用户 -> REVIEW
同一设备绑定多个账号 -> REVIEW
疑似撞库攻击 -> REJECT
热点 IP 高频风险行为 -> REVIEW / REJECT
```

决策合并规则：

```text
REJECT > REVIEW > PASS
```

多个规则命中时，最终 action 取最严重动作，`reasons` 保留命中的 reason code，`risk_score` 按命中动作累加。

## 黑名单链路

黑名单来源表：

```text
config/risk_blacklist.sql
```

支持实体：

```text
user
ip
device
```

判断顺序：

```text
Event
 -> user / ip / device 构造本地 key
 -> Bloom Filter 判断是否可能存在
 -> 本地精确表确认
 -> TTL LRU 缓存结果
 -> Redis 可用时补充查询
 -> 写入 FeatureSnapshot 的 black_hit 字段
 -> 规则 DSL 统一决策
```

说明：

```text
Bloom Filter 只做快速排除，不直接作为拒绝依据
Redis 不可用不会阻断主流程
MySQL 启动加载失败时服务仍可运行，但本地黑名单为空
黑名单命中统一走规则引擎，方便解释 reason
```

## 并发模型

FeatureStore 内部状态：

```text
user_id   -> UserState
ip        -> SlidingDistinct(user_id)
device_id -> SlidingDistinct(user_id)
ip        -> ShardedTopK
risk_key  -> ShardedCountMinSketch
```

关键参数：

```text
user shard num = 64
ip/device distinct shard num = 64
ip/device distinct window = 10min
ip/device distinct bucket = 10s
ip/device distinct max members = 5000
topk shard num = 16
topk capacity per shard = 100
cms shard num = 16
cms depth = 4
cms total width = 16384
cms memory ~= 512KB
```

并发策略：

```text
不同状态维度分片加锁
每个阶段只持有一把 shard 锁
用户、IP、设备、TopK、CMS 分阶段更新
避免多锁嵌套导致死锁
```

## 快速开始

### 环境要求

```text
CMake >= 3.20
C++20
Protobuf
Boost.Asio / Boost.Beast
MySQL 或 MariaDB client library
hiredis optional
```

Ubuntu / Debian 示例：

```bash
sudo apt update
sudo apt install -y cmake g++ protobuf-compiler libprotobuf-dev libboost-all-dev default-libmysqlclient-dev libhiredis-dev
```

### 构建

普通构建：

```bash
./scripts/build.sh
```

推荐压测使用 Release 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

构建产物：

```text
build/AegisFlow
build/send_event
build/benchmark_feature_store
build/benchmark_http
```

### 启动服务

```bash
mkdir -p logs
./build/AegisFlow config/server.conf
```

`config/server.conf` 中的核心配置：

```text
server.host = 0.0.0.0
server.port = 8080
server.io_threads = 2
server.max_body_size = 1048576

worker_pool.threads = 4
rule.file = config/rules.dsl

mysql.host = 127.0.0.1
mysql.port = 3306
mysql.user = admin
mysql.password = 123
mysql.database = aegisflow

redis.host = 127.0.0.1
redis.port = 6379
redis.password = 123456
redis.timeout_ms = 50

log.level = INFO
log.file = logs/log.log
```

服务端兼容 `server.*` 和 `http_server.*` 两套 HTTP 配置键。正式压测建议把 `log.level` 调整为 `WARN` 或 `ERROR`，避免逐请求日志影响 QPS。

### 健康检查

```bash
curl http://127.0.0.1:8080/health
```

期望响应：

```json
{"status": "ok"}
```

### 发送测试事件

```bash
./build/send_event
```

指定地址、端口和用户：

```bash
./build/send_event 127.0.0.1 8080 user_001
```

发送空用户事件：

```bash
./build/send_event 127.0.0.1 8080 --empty-user
```

## HTTP 接口

### 上报事件

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

核心响应字段：

```text
decision.event_id
decision.user_id
decision.action
decision.risk_score
decision.reasons
decision.cost_us
decision.features
```

错误响应：

| 场景 | 状态码 | 说明 |
|---|---:|---|
| 未知路由 | 404 | 仅支持 `/health` 和 `/v1/event/report` |
| Content-Type 错误 | 415 | 事件接口需要 Protobuf 请求体 |
| Body 过大 | 413 | 超过 `server.max_body_size` |
| Protobuf 解析失败 | 400 | 请求体不是合法 `ReportEventRequest` |
| 缺少 event | 400 | 请求体未携带 `event` |
| 响应序列化失败 | 500 | 服务内部序列化异常 |

## 测试

运行全部单元测试：

```bash
ctest --test-dir build --output-on-failure
```

测试覆盖：

```text
SlidingCounter / SlidingDistinct
RecentActionWindow / UserState
BloomFilter / LRU / TTL Cache
Count-Min Sketch / Space-Saving TopK
FeatureStore 正常流量和攻击流量
RuleLexer / RuleParser / RuleValidator
RuleEngine / DecisionAggregator
BlacklistManager
RiskService integration
```

当前验证结果：

```text
19/19 tests passed
```

## 压测

项目包含两类压测工具：核心内存计算微基准和 HTTP 端到端压测。

### FeatureStore 微基准

```bash
./build/benchmark_feature_store
```

默认流量模型：

```text
正常流量：
10000 user
1000 ip
5000 device
LOGIN SUCCESS

攻击流量：
1 个 attack_ip
1000 个 user
同一 attack_device
LOGIN FAIL
```

本地多轮结果：

| 日期 | 机器 | 轮数 | normal_events | attack_events | QPS 平均 | QPS 最低 | QPS 最高 | P99 平均 | P99 最差 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2026-06-10 | mint-virtual-machine | 20 | 10000 | 1000 | 63178.85 | 50017.73 | 70395.95 | 43.95us | 64us |

攻击流量特征结果：

```text
attack_ip_distinct_user_10m=1000
attack_device_distinct_account_10m=1000
attack_ip_topk_estimated_count=1000
attack_ip_in_topk=true
attack_cms_risk_behavior_count=1000
```

### HTTP / Protobuf 端到端压测

先启动服务：

```bash
mkdir -p logs
./build/AegisFlow config/server.conf
```

直接运行压测器：

```bash
./build/benchmark_http --requests 20000 --threads 8 --attack-ratio 0.20
```

或使用脚本保存多轮结果：

```bash
REQUESTS=20000 THREADS=8 ATTACK_RATIO=0.20 ROUNDS=3 ./scripts/pressure_test.sh
```

关键输出：

```text
qps / success_qps / error_rate
latency_p50_us / latency_p95_us / latency_p99_us
service_cost_p50_us / service_cost_p95_us / service_cost_p99_us
action_PASS / action_REVIEW / action_REJECT
reason_<reason_code>
```

本地端到端 10 轮压测结果：

| 日期 | 机器 | 日志级别 | 轮数 | requests/轮 | threads | attack_ratio | 总请求 | 成功率 | QPS 平均 | QPS 最低 | QPS 最高 |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2026-06-13 | zh-LEGION-REN9000K-34IRZ | ERROR | 10 | 20000 | 8 | 0.20 | 200000 | 100% | 18517.66 | 16952.28 | 20247.63 |

延迟汇总：

| 指标 | 平均值 | 最差值 |
|---|---:|---:|
| HTTP P50 | 88.90us | 93us |
| HTTP P95 | 154.70us | 192us |
| HTTP P99 | 259.50us | 359us |
| service P99 | 105.50us | 144us |
| latency max | 821616.90us | 901112us |

10 轮累计决策分布：

```text
PASS=99009
REVIEW=61490
REJECT=39501
```

这组结果说明，在本机回环 HTTP/Protobuf 压测、`log.level=ERROR`、8 个 keep-alive 连接下，端到端平均吞吐达到 `1.85w QPS`，错误率为 `0`，HTTP P99 平均为 `259.50us`，服务内部处理 P99 平均为 `105.50us`。连续多轮压测会让 TopK / CMS 等运行周期累计特征持续升高，因此后续轮次中部分正常流量也可能命中 `hot_ip_high_frequency` 并进入 REVIEW，这是有状态风控链路的预期现象。`latency_max` 仍存在 0.75s - 0.90s 长尾，正式定位时建议继续补充 P999、CPU、内存、上下文切换和客户端/服务端分机压测数据。

## 目录结构

```text
AegisFlow/
├── CMakeLists.txt
├── README.md
├── config/
│   ├── risk_blacklist.sql
│   ├── rules.dsl
│   └── server.conf
├── include/aegisflow/
│   ├── app/
│   ├── cache/
│   ├── config/
│   ├── feature/
│   ├── log/
│   ├── net/
│   ├── risk/
│   ├── rule/
│   ├── runtime/
│   └── storage/
├── proto/
│   ├── decision.proto
│   └── event.proto
├── scripts/
│   ├── build.sh
│   └── pressure_test.sh
├── src/
│   ├── app/
│   ├── config/
│   ├── feature/
│   ├── log/
│   ├── net/
│   ├── risk/
│   ├── rule/
│   ├── runtime/
│   ├── storage/
│   └── main.cpp
├── tests/
└── tools/
    ├── benchmark_feature_store.cpp
    ├── benchmark_http.cpp
    └── send_event.cpp
```

## 边界说明

```text
当前只实现单事件上报接口，proto 中的 batch message 预留但未接入 HTTP 路由
MySQL 用于启动加载黑名单，不承担实时特征写入
Redis 是黑名单缓存增强项，不是主链路强依赖
FeatureStore 以内存状态为主，服务重启后实时窗口状态不恢复
CMS 估计值可能偏大，不会低于真实计数
TopK 对低频 key 存在替换误差，高频 key 在足够流量后稳定保留
```
