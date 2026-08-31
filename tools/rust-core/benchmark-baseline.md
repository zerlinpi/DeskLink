# Rust Core Prototype 基准基线

> 状态：Phase 1 基线采集中。本文档记录可复现实测，不在首次采样前预设通过阈值。微基准结果只描述 reducer / C ABI 的本地 CPU 成本，不等同于端到端远程桌面延迟。

## 1. 基准环境

| 项目 | 记录 |
| --- | --- |
| Prototype branch | `prototype/rust-core` |
| Rust | `1.85.1` |
| Cargo dependency mode | `--locked` |
| Benchmark framework | Criterion `0.5.1` |
| Commit SHA | 待首次 benchmark run 填写 |
| GitHub Actions run | 待首次 benchmark run 填写 |
| Runner OS / CPU | 待首次 benchmark run 填写 |
| Session command | `cargo bench --locked -p desklink-session --bench session_bench -- --warm-up-time 1 --measurement-time 2` |
| FFI command | `cargo bench --locked -p desklink-core-ffi --bench ffi_bench -- --warm-up-time 1 --measurement-time 2` |

## 2. Rust microbenchmark

首次采样要求数值输出，不在采样前定义 pass/fail 阈值。记录 Criterion 给出的时间区间与吞吐量；`ns/event` 如由 batch 时间换算，必须标记为“derived”。

| Benchmark | Events / batch | Criterion time | Throughput | ns/event | Evidence |
| --- | ---: | --- | --- | ---: | --- |
| valid lifecycle | 100,000 | 待测 | 待测 | 待测 | 待测 |
| valid lifecycle | 1,000,000 | 待测 | 待测 | 待测 | 待测 |
| stale session event | 100,000 | 待测 | 待测 | 待测 | 待测 |
| stale session event | 1,000,000 | 待测 | 待测 | 待测 | 待测 |
| peer replacement | 100,000 | 待测 | 待测 | 待测 | 待测 |
| peer replacement | 1,000,000 | 待测 | 待测 | 待测 | 待测 |
| control replacement | 100,000 | 待测 | 待测 | 待测 | 待测 |
| control replacement | 1,000,000 | 待测 | 待测 | 待测 | 待测 |
| pointer replacement | 100,000 | 待测 | 待测 | 待测 | 待测 |
| pointer replacement | 1,000,000 | 待测 | 待测 | 待测 | 待测 |

## 3. Direct reducer 与 C ABI 对照

该组基准使用相同的当前/迟到 operation 语义，Criterion batched setup 在计时主体之外创建对应的 connected state，目标是隔离 `desklink_core_apply` 边界附加成本，而不是测连接建立成本。

| Path | Event class | Events / batch | Criterion time | Throughput | ns/event | FFI overhead vs direct |
| --- | --- | ---: | --- | --- | ---: | --- |
| Direct reducer | current | 100,000 | 待测 | 待测 | 待测 | baseline |
| C ABI | current | 100,000 | 待测 | 待测 | 待测 | 待测 |
| Direct reducer | current | 1,000,000 | 待测 | 待测 | 待测 | baseline |
| C ABI | current | 1,000,000 | 待测 | 待测 | 待测 | 待测 |
| Direct reducer | stale | 100,000 | 待测 | 待测 | 待测 | baseline |
| C ABI | stale | 100,000 | 待测 | 待测 | 待测 | 待测 |
| Direct reducer | stale | 1,000,000 | 待测 | 待测 | 待测 | baseline |
| C ABI | stale | 1,000,000 | 待测 | 待测 | 待测 | 待测 |

## 4. Phase-1 端到端指标采集定义

这些指标必须在后续真实 Windows 主机 / 控制端环境中采集。Rust microbenchmark 不能替代它们。

| 指标 | 定义 / 采集方式 | Phase-1 要求 |
| --- | --- | --- |
| Connection Success Rate | 固定网络/认证场景下，成功进入可交互 Connected 的次数 ÷ 总尝试次数 | Rust shadow 前后使用同一场景、同一重复次数 |
| Time To First Frame (TTFF) | 控制端发起连接到首个可显示视频帧的单调时钟差 | 记录 p50/p95/max，分 P2P/TURN |
| Input Latency | 控制端输入事件发送到 Host 接收/注入确认的时间差；若无统一时钟则记录可校准 instrumentation | 不以浏览器视觉主观值代替 |
| Motion-to-Photon | 真实输入动作到控制端显示对应像素变化；需高速相机/可校准硬件或等效测量链 | 真实机器指标，不从 reducer benchmark 推导 |
| Signal Recovery Time | Signal 链路断开到恢复可继续会话的时间 | 覆盖 backoff 与 session generation rotation |
| ICE Recovery Time | 传输故障/ICE restart 到媒体与控制恢复的时间 | 覆盖 P2P 与 TURN fallback |
| CPU | Agent/Service/Controller 在固定分辨率、FPS、码率场景的进程 CPU | 记录 idle、1080p60、4K 可支持场景 |
| GPU | Capture / conversion / encode 的 GPU engine 利用率 | Intel/NVIDIA/AMD 分开记录，不能跨 GPU 泛化 |
| Private Bytes / Working Set | `desklink-agent.exe`、`desklink-service.exe` 的进程内存 | 连接前、稳定运行、recovery 后、soak 结束 |
| Crash Count | Agent / Service / Controller 非预期退出次数 | 8h/24h/72h soak 分别记录 |
| 8h / 24h / 72h stability | 连续连接、重连、锁屏/解锁、网络扰动后的错误/资源增长/崩溃 | Phase-1 至少定义采集；authority transfer 前必须有真实结果 |

## 5. 解释原则

1. 首次 Criterion 数据建立 baseline，不事后挑选阈值来“证明”Rust 更快。
2. 后续回归阈值只能基于重复采样的方差、runner 稳定性和真实产品预算制定。
3. GitHub hosted runner 的绝对性能只能用于 prototype microbenchmark 趋势；不能据此承诺 Windows 用户的端到端延迟。
4. FFI benchmark 如果更慢，必须保留真实结果；Phase-1 的主要判断仍是确定性、stale-race 正确性、集成风险和总体产品回归，而不是单一 ns/event。
