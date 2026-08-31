# Rust Core Prototype 基准基线

> 状态：Phase 1 首次基线已采集。本文档记录可复现实测，不把首次采样结果反向包装成“通过阈值”。微基准结果只描述 reducer / C ABI 的本地 CPU 成本，不等同于端到端远程桌面延迟。

## 1. 基准环境

| 项目 | 记录 |
| --- | --- |
| Prototype branch | `prototype/rust-core` |
| Rust | `1.85.1` |
| Cargo dependency mode | `--locked` |
| Benchmark framework | Criterion `0.5.1` |
| Commit SHA | `3ab2b1f463f4c27d47137ba0b29d2fe707a2851f` |
| GitHub Actions run | `33378639405` (`Rust Core Benchmark`) |
| Runner OS / image | Ubuntu `24.04.4`, `ubuntu-24.04`, image `20260823.283.1` |
| Runner CPU | workflow 未采集具体 CPU 型号；因此不能把绝对数值跨 runner / Windows 主机直接比较 |
| Session command | `cargo bench --locked -p desklink-session --bench session_bench -- --warm-up-time 1 --measurement-time 2` |
| FFI command | `cargo bench --locked -p desklink-core-ffi --bench ffi_bench -- --warm-up-time 1 --measurement-time 2` |

Criterion 每项使用 10 个 samples。下面的 `ns/event` 均由 Criterion 区间中点对应的 batch time ÷ event count 换算，因此标记为 derived。

## 2. Rust microbenchmark

| Benchmark | Events / batch | Criterion time | Throughput | derived ns/event | Evidence |
| --- | ---: | --- | --- | ---: | --- |
| valid lifecycle | 100,000 | `[2.2559, 2.2587, 2.2623] ms` | `[44.202, 44.274, 44.328] Melem/s` | `22.587` | run `33378639405` |
| valid lifecycle | 1,000,000 | `[22.559, 22.608, 22.691] ms` | `[44.071, 44.233, 44.329] Melem/s` | `22.608` | run `33378639405` |
| stale session event | 100,000 | `[2.0509, 2.0560, 2.0718] ms` | `[48.268, 48.639, 48.759] Melem/s` | `20.560` | run `33378639405` |
| stale session event | 1,000,000 | `[20.518, 20.643, 20.836] ms` | `[47.994, 48.444, 48.738] Melem/s` | `20.643` | run `33378639405` |
| peer replacement | 100,000 | `[2.2607, 2.2614, 2.2624] ms` | `[44.201, 44.221, 44.235] Melem/s` | `22.614` | run `33378639405` |
| peer replacement | 1,000,000 | `[22.622, 22.646, 22.683] ms` | `[44.085, 44.157, 44.204] Melem/s` | `22.646` | run `33378639405` |
| control replacement | 100,000 | `[1.0586, 1.0589, 1.0596] ms` | `[94.379, 94.434, 94.465] Melem/s` | `10.589` | run `33378639405` |
| control replacement | 1,000,000 | `[10.579, 10.580, 10.582] ms` | `[94.504, 94.519, 94.530] Melem/s` | `10.580` | run `33378639405` |
| pointer replacement | 100,000 | `[1.0591, 1.0599, 1.0607] ms` | `[94.280, 94.352, 94.416] Melem/s` | `10.599` | run `33378639405` |
| pointer replacement | 1,000,000 | `[10.586, 10.589, 10.594] ms` | `[94.397, 94.441, 94.468] Melem/s` | `10.589` | run `33378639405` |

100k 与 1M 两档的 derived `ns/event` 接近，说明这次 hosted-runner 采样没有明显的 batch-size 非线性；这只用于判断 microbenchmark 本身是否稳定，不代表 Windows 产品链路的延迟预算。

## 3. Direct reducer 与 C ABI 对照

该组基准使用相同的当前/迟到 operation 语义，Criterion batched setup 在计时主体之外创建对应的 connected state，目标是隔离 `desklink_core_apply` 边界附加成本，而不是测连接建立成本。

| Path | Event class | Events / batch | Criterion time | Throughput | derived ns/event | FFI overhead vs direct |
| --- | --- | ---: | --- | --- | ---: | --- |
| Direct reducer | current | 100,000 | `[1.0540, 1.0552, 1.0574] ms` | `[94.570, 94.770, 94.876] Melem/s` | `10.552` | baseline |
| C ABI | current | 100,000 | `[1.5563, 1.5623, 1.5772] ms` | `[63.405, 64.006, 64.255] Melem/s` | `15.623` | `+5.071 ns/event`, `+48.06%` |
| Direct reducer | current | 1,000,000 | `[10.541, 10.551, 10.565] ms` | `[94.651, 94.777, 94.870] Melem/s` | `10.551` | baseline |
| C ABI | current | 1,000,000 | `[15.505, 15.621, 15.787] ms` | `[63.343, 64.015, 64.494] Melem/s` | `15.621` | `+5.070 ns/event`, `+48.05%` |
| Direct reducer | stale | 100,000 | `[2.1093, 2.1201, 2.1350] ms` | `[46.839, 47.167, 47.410] Melem/s` | `21.201` | baseline |
| C ABI | stale | 100,000 | `[2.1215, 2.1267, 2.1297] ms` | `[46.955, 47.020, 47.137] Melem/s` | `21.267` | `+0.066 ns/event`, `+0.31%` |
| Direct reducer | stale | 1,000,000 | `[20.983, 21.086, 21.131] ms` | `[47.324, 47.424, 47.657] Melem/s` | `21.086` | baseline |
| C ABI | stale | 1,000,000 | `[21.213, 21.256, 21.367] ms` | `[46.800, 47.045, 47.142] Melem/s` | `21.256` | `+0.170 ns/event`, `+0.81%` |

解释：current path 的 FFI 相对增幅约 48%，但绝对附加成本在这次 runner 上约 `5.07 ns/event`；stale path 的绝对附加成本约 `0.07–0.17 ns/event`。这组数字只建立首次基线，不构成生产接入判定阈值，也不能推导用户侧输入或视频延迟。

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
5. 在至少有多次独立 benchmark run 之前，不把单次 hosted-runner 数据设为硬失败阈值；当前 workflow 负责确保 benchmark 可编译、可执行并产生可审计输出。
