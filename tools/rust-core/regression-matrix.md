# Rust Core Prototype 回归矩阵

> 适用范围：`prototype/rust-core` Phase 1。Rust 当前只验证 lifecycle / generation / recovery / C ABI，不接管 DXGI、D3D11、Media Foundation、libdatachannel、SendInput、Windows Service 或 Web Controller。任何生产 authority transfer 都必须晚于 Phase-1 decision gate。

## 回归原则

- Prototype branch 上 Rust 测试通过，不代表生产 Windows 行为自动通过。
- Phase 1 要保持现有生产实现不变，并重复运行 Signal、Web、Windows、部署相关回归。
- Hosted runner 能验证编译、协议、纯逻辑和多数 native smoke；GPU/驱动、真实 P2P/TURN、Secure Desktop、输入手感、长时间稳定性仍要求真实机器。
- 任一不可解释的功能退化、crash、authority mismatch 或资源增长都阻断 Rust authority transfer。
- 回滚优先级：先关闭/不引入 Rust shadow；如已进入 shadow 阶段，`DESKLINK_ENABLE_RUST_CORE_SHADOW=OFF` 必须恢复纯 C++ 路径。Phase 1 不修改 `main` 的 production authority。

## 矩阵

| Area | Existing automated evidence | Phase-1 / shadow required real-machine check | Failure / rollback action |
| --- | --- | --- | --- |
| Video capture / DXGI | Windows Release build；`desklink-media-probe` 可构建 | Win10/11 多 GPU：连接、锁屏/解锁、display change、DXGI ACCESS_LOST 恢复 | 停止 authority transfer；保持 C++ capture path |
| H.264 / Media Foundation | `desklink-h264-annexb-smoke`；Release build | Intel/NVIDIA/AMD：1080p60；可用机器上的高分辨率/高刷；encoder reset/recovery | Rust core 不得成为视频退化归因的掩盖层；回到无 shadow 基线 |
| Video adaptation / high refresh | `desklink-video-policy-smoke` | 15–144 FPS 请求、弱网降码率/FPS/分辨率、实际显示刷新率 | 不承诺 4K144；保留现有 policy |
| Pointer wire protocol | `desklink-pointer-wire-smoke` | Browser→Host 连续 pointer move、拖拽、高 DPI、多显示器边界 | 关闭 shadow；比较 generation/channel decision mismatch |
| Input channel authority | `desklink-input-channel-authority-smoke` | Control/Pointer channel replacement、断线后 release-all、重复连接 | stale close 若撤销新 channel 立即 STOP |
| Keyboard / SendInput | Windows Release build；input authority smoke 间接覆盖 channel ownership | 修饰键、按下/抬起、重复键、断线清键、IME/布局基础场景 | 保持 C++ SendInput authority；阻断 Rust input authority |
| Multi-monitor / DPI | Windows build | 不同 scale、负坐标/排列、monitor switch、browser viewport mapping | 记录为产品回归，不能用 reducer test 抵消 |
| Clipboard | Windows build (`clipboard_win32.cpp`) | 文本双向复制、断线/重连、锁屏后恢复 | 保持现有 C++ clipboard path |
| File transfer | `desklink-file-transfer-download-policy-smoke`；`desklink-file-transfer-chunk-policy-smoke` | 大文件、取消、重连、目标路径/磁盘错误、重复 chunk | Rust lifecycle 不得改变 transfer ownership；失败则关闭 shadow |
| Authentication / Access Proof | `desklink-access-proof-smoke`；Signal tests；Web Access Code proof vector | 错误 Access Code、重连、credential rotation、并发旧 peer callback | 任何旧 generation 越权为 STOP 条件 |
| Stable Device ID | `desklink-device-identity-smoke` | 重启/Service reinstall 后保持相同 ID；OS reinstall 新 ID | 不让 Rust prototype改 identity semantics |
| Peer signaling scope | `desklink-peer-signal-scope-smoke` | 旧 peer callback、replacement peer、Signal reconnect | Rust/C++ decision mismatch 阻断 shadow advance |
| P2P | Signal tests + Windows/Web build，只验证代码路径可构建 | 两台真实网络主机：直连成功率、TTFF、重连 | 无真实结果不得声明 P2P 无回归 |
| TURN fallback | Compose/network helper syntax checks；Signal/Web/Windows build | 强制 relay、TURN auth、TLS/UDP/TCP 可用场景 | 保持现有 TURN config；记录恢复时间 |
| Signal reconnect | Signal Go tests；Rust `desklink-recovery` policy tests | 中断 Signal 后恢复；确认旧 Session completion 不影响新 Session | stale recovery 能 mutate current state => STOP |
| ICE / transport recovery | Rust RecoveryCoordinator transport policy；Windows build | 网络切换、UDP loss、ICE restart、TURN fallback | authority mismatch / recovery regression => STOP |
| Ctrl+Alt+Del / SAS | `desklink-secure-attention-smoke`；`desklink-service-auth-smoke` | Win10/11 且本地 policy 允许 Services：实际 Ctrl+Alt+Del；验证 PID/SID broker | 不改 UAC/SAS policy；失败保持 Service broker 原实现 |
| Secure Desktop capture/control | 无完整 production support；SAS smoke 不等价于 Secure Desktop capture | 登录/UAC Secure Desktop 为独立未完成能力 | 不把 SAS 或 Rust prototype 描述为已解决 Secure Desktop |
| Windows Service / active session | `desklink-active-session-smoke`；`desklink-service-auth-smoke`；Release build | Service restart、RDP/FUS/console session switch、锁屏/解锁 | Rust shadow 默认 OFF；Service 稳定性回归即关闭 shadow |
| Host capabilities | `desklink-host-capabilities-smoke` + schema fixture validation | 不同 Windows/GPU 上 capability 与实际功能一致 | 不允许 Rust core自行发明 capability |
| Web Controller | `npm ci`、`npm audit --audit-level=moderate`、typecheck、Vitest、production build | Chrome/Edge 连接、断线、输入、Ctrl+Alt+Del UI 状态 | Web 不接 Rust；任何协议差异先回滚 shadow |
| Signal service | `go test ./...` + versioned build + provisioning tool checks | 自托管 Signal reconnect、controller/device auth | Go Signal 保持生产 authority |
| Packaging / portable Windows | Windows static dependency validation、PowerShell syntax | 干净 Win10/11 安装、Service start、卸载/重装 | Prototype 不进入 release packaging，除非 decision gate ADVANCE |
| Deployment | Compose config + network helper syntax；后续 Phase-1 执行部署 smoke | 自托管 Signal/TURN/Web 最小部署连接 | 部署失败阻断 Phase-1 ADVANCE |
| Memory / soak | 当前无 Rust 生产集成，因此没有可代表用户的长期结果 | 8h/24h/72h：Private Bytes/Working Set、CPU/GPU、crash、recovery | 未完成长期结果前不转 production authority |

## 当前自动化 target 清单

Windows CMake 当前可见的 native smoke targets：

- `desklink-service-auth-smoke`
- `desklink-access-proof-smoke`
- `desklink-h264-annexb-smoke`
- `desklink-video-policy-smoke`
- `desklink-pointer-wire-smoke`
- `desklink-input-channel-authority-smoke`
- `desklink-peer-signal-scope-smoke`
- `desklink-file-transfer-download-policy-smoke`
- `desklink-file-transfer-chunk-policy-smoke`
- `desklink-active-session-smoke`
- `desklink-device-identity-smoke`
- `desklink-secure-attention-smoke`
- `desklink-host-capabilities-smoke`

`tools/windows/run-native-smokes.ps1` 应在 Phase-1 decision gate 重新运行完整集合，而不是只挑与 Rust 看似相关的 target。

## Phase-1 rollback boundary

当前最安全 rollback 是删除/放弃 `prototype/rust-core` 分支，不影响 `main`。即使 Phase-1 结论为 ADVANCE，下一阶段也只能先做 shadow integration：C++ 继续 authoritative，Rust 仅镜像 normalized lifecycle events 并报告 decision mismatch；CMake 开关必须默认 OFF。
