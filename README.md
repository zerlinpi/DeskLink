# DeskLink

> 面向低延迟远程桌面、远程办公与游戏串流场景的开源远程控制项目。

DeskLink 的目标是实现接近向日葵远程控制与网易 UU 远程一类产品的使用体验：**浏览器可直接控制、P2P 优先、TURN 中继兜底、GPU 采集/编码、低延迟输入、弱网自适应与无人值守**。

当前项目以 **Windows 被控端 + Web 控制端** 为第一阶段主线。它不是传统 VNC/RDP 的简单封装，而是按照“实时交互流媒体系统”设计：视频、输入、信令、文件传输和认证使用相互独立的链路与优先级，避免视频拥塞拖慢鼠标和键盘。

## 核心设计原则

1. **输入优先**：鼠标移动使用独立的低延迟、不可靠/无序 DataChannel；键盘、点击等关键事件走可靠控制通道，降低队头阻塞。
2. **不堆积旧帧**：远控场景宁可主动降码率、降分辨率、降帧率或丢帧，也不允许视频队列不断增长造成“越用越延迟”。
3. **GPU 路径优先**：Windows 使用 DXGI Desktop Duplication + D3D11 GPU 缩放/颜色转换 + Media Foundation 硬件 H.264，尽量减少 CPU 拷贝。
4. **P2P 优先、中继兜底**：WebRTC ICE 优先尝试公网/局域网直连，无法穿透 NAT 时自动回退 TURN UDP/TCP/TLS。
5. **弱网自适应**：综合 RTT、丢包、抖动、可用带宽等遥测动态调整码率、FPS 和分辨率。
6. **安全默认拒绝**：设备、控制端和会话均采用作用域明确的短期凭证；无人值守长期秘密由 Windows Service 使用 DPAPI 保护。

## 当前技术架构

```text
                           ┌──────────────────────┐
                           │   Signaling / Auth   │
                           │ Go + WebSocket/HTTP  │
                           └──────────┬───────────┘
                                      │
                         鉴权 / 会话 / ICE 协商
                                      │
              ┌───────────────────────┴───────────────────────┐
              │                                               │
      Web 控制端（Browser）                              Windows Host
      React + TypeScript                                C++20 / Win32
              │                                               │
              │              WebRTC                           │
              ├──────────── P2P 优先 ─────────────────────────┤
              │                                               │
              │        失败时 TURN Relay                       │
              └────────── coturn UDP/TCP/TLS ─────────────────┘

视频：DXGI → D3D11 → NV12 → HW H.264 → RTP → 浏览器硬解
输入：Browser → 独立 DataChannel → Win32 SendInput
文件：独立可靠 DataChannel + 分块 + SHA-256 + 背压
```

## 已实现能力

### 实时远程画面

- Windows DXGI Desktop Duplication 屏幕采集；锁屏/用户切换/桌面模式变化导致 `DXGI_ERROR_ACCESS_LOST` 后会节流重试重建，回到正常桌面后无需手动重启 Agent。
- D3D11 Video Processor GPU 缩放以及 BGRA → NV12 转换。
- Media Foundation 硬件 H.264 低延迟编码。
- 关闭 B 帧、短 GOP、CBR 等低延迟编码策略。
- H.264 RTP 分包。
- RTP pacing，避免瞬时发送突发。
- NACK 丢包重传。
- RTCP PLI/FIR 关键帧恢复。
- 桌面完全静止时缓存最新 GPU 帧，使新会话或 PLI 仍能快速恢复画面。

### 弱网与低延迟

- 基于遥测的动态码率调整。
- FPS 档位动态降级与恢复，目标范围 15–144 FPS，高刷阶梯为 144 → 120 → 90 → 60 → 45 → 30 → 24 → 15；硬件编码器不接受高刷初始化时自动尝试较低兼容档位。
- 分辨率档位动态降级与恢复。
- 网络路径变化后进行信令重连和 WebRTC ICE Restart。
- 控制端实时显示：
  - Direct P2P / TURN Relay；
  - 网络协议；
  - RTT；
  - 丢包率；
  - 抖动；
  - 解码 FPS；
  - 估算可用带宽。
- 浏览器端对支持的实现设置低 playout/jitter-buffer 延迟提示。

> DeskLink 不承诺在所有网络环境下“绝对不卡顿”。目标是在网络恶化时优先保护输入响应和端到端延迟，而不是维持虚假的高画质后让缓存不断堆积。

### 控制与多显示器

- 鼠标和键盘 Win32 输入注入。
- 指针移动与可靠控制消息拆分为独立 DataChannel。
- 断线、PeerConnection 失败、输入 DataChannel 关闭、窗口失焦及 Service 停止时自动释放卡住的按键和鼠标按钮；释放失败的状态保留并可在后续 cleanup 重试。
- 多显示器枚举与会话内切换。
- 支持负坐标的 Windows 虚拟桌面坐标映射。
- 浏览器 letterbox 场景下的正确鼠标坐标映射。

### 画质档位

Web 控制端当前提供：

- **自动**：根据延迟、丢包和带宽动态调整。
- **原画**：优先保持最高分辨率。
- **高清**：最高约 900p / 45 FPS / 8 Mbps。
- **清晰**：最高约 720p / 30 FPS / 4 Mbps。

后续计划进一步拆分为 **桌面模式** 和 **游戏低延迟模式**：桌面模式优先文字清晰度和高分辨率；游戏模式在弱网时优先降低分辨率和码率、尽量维持高 FPS 与输入响应。

### 剪贴板与文件传输

- 双向 UTF-8 文本剪贴板。
- 浏览器不会静默读取或覆盖本地剪贴板，需要显式用户操作。
- 独立 `file-transfer` 可靠 DataChannel。
- 浏览器 → Windows 拖放、多文件队列。
- Windows → 浏览器文件列表和下载。
- 32 KiB 分块。
- SHA-256 完整性验证。
- 64 位文件偏移。
- `bufferedAmount` 背压。
- 断线续传/恢复逻辑。

Windows 默认传输目录：

```text
%USERPROFILE%\Downloads\DeskLink
```

可使用 `DESKLINK_TRANSFER_DIR` 覆盖。

### 无人值守与安全

- `desklink-service.exe` 作为 LocalSystem Windows Service。
- Service 在活动交互会话中启动并维护 Agent：可用时优先 Console，否则支持选择真实 `WTSActive` RDP 会话。
- 会话切换和崩溃后自动恢复，并带指数退避防止 crash loop。
- 设备长期凭证和无人值守访问码使用机器范围 DPAPI 存储。
- `%ProgramData%\DeskLink` 的敏感文件限制为 SYSTEM / Administrators 访问。
- LocalSystem 本地认证 Broker 保留长期设备凭证，用户会话 Agent 仅得到短期 Signal Token。
- Named Pipe 限制为本地访问，并校验 Agent 用户 SID 与 PID。
- 每设备独立 `dc2` 凭证，服务端仅保存 SHA-256 哈希。
- 控制端 `ck1` Key 注册表与 target-scoped 短期 `ct1` Session。
- 设备吊销会阻止新凭证并终止现有信令会话。
- 一次性 HMAC-SHA256 Access Code challenge/proof，长期 Access Code 不直接放入 WebRTC offer。

## 仓库目录

```text
apps/
  signal/         Go 信令、认证与短期 TURN 凭证服务
  web/            React + TypeScript 浏览器控制端
  windows-agent/  Windows C++ Agent + Windows Service
infra/
  coturn/         TURN 配置
  docker-compose.yml
packages/
  protocol/       协议文档
packaging/
  windows/        Windows 发布与安装脚本
  web/            Web 发布说明
docs/
  ARCHITECTURE.md
  DEVICE_AUTH.md
  NETWORK_TESTING.md
  PRODUCTION_NETWORK.md
  SIGNAL_AUTH.md
  WINDOWS_SERVICE.md
  releases/
```

## 本地开发

### 1. 启动信令服务与 TURN

提供的 Compose 配置主要面向 Linux 开发机/服务器，因为 coturn 使用 host networking。

```bash
cd infra
docker compose up
```

开发配置主要端口：

- TCP `8080`：信令 WebSocket/HTTP。
- UDP/TCP `3478`：TURN/STUN。
- UDP `49160-49200`：TURN Relay 端口范围。

公网部署前必须正确配置 coturn 公网 IP、防火墙以及 TURN 密钥。

### 2. 编译 Windows Host

在 Visual Studio Developer Shell 中执行：

```powershell
cmake -S apps/windows-agent -B build/windows-agent -A x64
cmake --build build/windows-agent --config Release --parallel
```

生成：

```text
DeskLink.exe
desklink-agent.exe
desklink-service.exe
desklink-media-probe.exe
```

开发模式示例：

```powershell
$env:DESKLINK_SIGNAL_URL = "ws://YOUR_SERVER:8080/ws"
$env:DESKLINK_DEVICE_ID = "office-pc"
$env:DESKLINK_ACCESS_CODE = "use-a-long-random-development-code"
$env:DESKLINK_STUN_URL = "stun:YOUR_SERVER:3478"
$env:DESKLINK_TURN_HOST = "YOUR_SERVER"
$env:DESKLINK_TURN_USERNAME = "desklink"
$env:DESKLINK_TURN_PASSWORD = "CHANGE_ME_NOW"

.\build\windows-agent\Release\desklink-agent.exe
```

性能参数（`DESKLINK_FPS` 支持 15–144；60 为兼容默认值，高刷应结合显示器、GPU、网络与浏览器能力使用）：

```powershell
$env:DESKLINK_FPS = "60"
$env:DESKLINK_BITRATE_BPS = "12000000"
$env:DESKLINK_MIN_BITRATE_BPS = "2000000"
$env:DESKLINK_MAX_WIDTH = "1920"
$env:DESKLINK_MAX_HEIGHT = "1080"
$env:DESKLINK_PACING_BPS = "14400000"
$env:DESKLINK_PERFORMANCE_TUNING = "1"
```

### 3. 安装无人值守 Windows Service

```powershell
.\build\windows-agent\Release\desklink-service.exe --install
```

生产风格部署建议把设备凭证和访问码写入 DPAPI：

```powershell
.\build\windows-agent\Release\desklink-service.exe --store-device-credential
.\build\windows-agent\Release\desklink-service.exe --store-access-code
Restart-Service DeskLink
```

详见：

- `docs/WINDOWS_SERVICE.md`
- `docs/DEVICE_AUTH.md`

### 4. 启动 Web 控制端

```bash
cd apps/web
npm ci
npm run dev
```

开发环境：

```dotenv
VITE_SIGNAL_URL=ws://YOUR_SERVER:8080/ws
VITE_STUN_URL=stun:YOUR_SERVER:3478
VITE_TURN_URL=turn:YOUR_SERVER:3478
VITE_TURN_USERNAME=desklink
VITE_TURN_PASSWORD=CHANGE_ME_NOW
```

生产源码构建时的 fallback 示例（部署后优先使用运行时配置）：

```dotenv
VITE_SIGNAL_URL=wss://control.example.com/ws
VITE_CONTROLLER_SESSION_URL=https://control.example.com/api/v1/controller-session
VITE_CONTROLLER_AUTH_REQUIRED=1
VITE_STUN_URL=stun:turn.example.com:3478
VITE_TURN_URL=turn:turn.example.com:3478
VITE_TURN_TLS_URL=turns:turn.example.com:5349
VITE_TURN_CREDENTIALS_URL=https://control.example.com/api/v1/turn-credentials
VITE_TURN_RUNTIME_REQUIRED=1
```

生产部署的 `dist` 支持 **build once, deploy anywhere**：无需重新执行 Vite 构建，直接修改与 `index.html` 同目录的 `desklink-config.js` 即可切换 Signal、Controller Session、STUN/TURN 等公开端点和运行时开关。该文件必须保持公开配置属性，禁止写入 Access Code、Device Credential、Controller secret、Signal token、TURN shared secret 或长期 TURN 用户名/密码。`VITE_*` 仅作为源码构建期 fallback。

## Release 与下载

当前 `main` 使用带 `-dev` 后缀的开发版本号；只有与 `VERSION` 完全匹配的稳定 `vX.Y.Z` tag 才能发布稳定 Release 和稳定容器标签。

正式版本通过 GitHub Releases 发布。每个版本通常包含：

- `desklink-windows-v<version>-x64.zip`：`DeskLink.exe`、Windows Agent/Service、媒体探针与安装/卸载脚本。
- `desklink-web-v<version>-dist.tar.gz`：可直接部署的浏览器控制端静态产物。
- `desklink-web-v<version>-source.tar.gz`：用于审计、开发或自定义构建的 Web 源码包。
- `desklink-signal-v<version>-linux-amd64`：Linux amd64 信令服务。
- `SHA256SUMS.txt`：发布资产 SHA-256 校验值。

发布工作流会在构建前运行 Go 测试、Web 构建和 Windows native smoke tests，并验证 Windows 发布二进制的可移植依赖。

## GitHub Packages

项目使用 **GitHub Container Registry（GHCR）** 发布可直接部署的服务端容器镜像。

信令服务镜像：

```bash
docker pull ghcr.io/zerlinpi/desklink-signal:latest
```

版本发布后也可使用对应版本标签，例如：

```bash
docker pull ghcr.io/zerlinpi/desklink-signal:1.0.4
```

GitHub Actions 会负责构建并发布镜像，因此仓库首页的 **Packages** 区域会显示 DeskLink 的容器包。

## 公网部署建议

- 中国大陆用户应部署自己的 STUN/TURN，不建议生产环境依赖公共 Google STUN。
- 优先 Direct P2P，其次 TURN/UDP，最后使用 TURN/TCP/TLS 作为受限网络兼容方案。
- 用户跨地区后应部署多个 Relay 区域，并按延迟与健康状态选择节点。
- 信令必须使用 HTTPS/WSS 和有效证书。
- TURN 使用 REST 临时凭证，共享 Secret 仅保留在服务端。
- 生产环境限制浏览器 Origin。
- 反向代理日志必须过滤认证 Header、WebSocket subprotocol 和敏感 Query。
- 当前 HMAC challenge 适合高熵 Access Code；如果未来需要支持短 PIN/弱密码，应升级为 PAKE/OPAQUE 类方案。

## 推荐测试顺序

1. 同机浏览器 → Windows Agent。
2. 同一局域网两台设备。
3. 两个不同公网网络，验证 Direct P2P。
4. 强制 TURN/UDP，确认 HUD 显示 `TURN relay`。
5. 分别测试 TURN/TCP 与 TURN/TLS。
6. 验证错误 Access Code 无法建立 PeerConnection。
7. 人工注入丢包、延迟和限速，观察画质先降、控制仍保持响应。
8. 会话中切换 Wi-Fi/网络，验证 ICE Restart。
9. 安装 Windows Service 后测试重启、登录/注销和崩溃恢复。
10. 分别在 Intel / NVIDIA / AMD Windows 硬件上测试编码器与驱动。
11. 测试 Chrome、Edge、Safari 与代表性移动浏览器。

更多弱网测试矩阵见 `docs/NETWORK_TESTING.md`。

## 当前限制

v1.0.0 是可打包运行的基线版本，但尚未宣称达到成熟商业远控产品的完整能力：

- 尚未支持 Windows UAC Secure Desktop / 登录界面控制。
- Windows 二进制尚未 Authenticode 签名，SmartScreen 可能提示。
- 尚未加入系统音频/麦克风转发。
- 尚未加入隐私屏/本地黑屏。
- 多 GPU 和多屏同时合成仍需继续完善。
- 编码管线允许配置至 3840×2160，但 4K/HDR/4:4:4/高刷尚未完成覆盖 Intel/NVIDIA/AMD 和主流浏览器的真实硬件验证，因此当前不作保证。
- Web 端部分网络地址仍是 Vite 构建期配置，后续会增加运行时配置文件以便一个镜像适配不同部署环境。

## 后续技术路线

### M4 — 低延迟体验升级

- 桌面模式 / 游戏模式独立自适应策略。
- NVENC / Intel oneVPL / AMD AMF 显式编码后端与能力探测。
- GPU 零拷贝路径进一步收敛。
- 60 FPS 以上高刷新率 Native Controller 研究。
- 更细粒度的发送队列和端到端延迟遥测。

### M5 — 原生高速通道

浏览器继续使用 WebRTC；Native Controller 计划评估 **QUIC/UDP Datagram** 数据通道，以支持更高刷新率、更可控的拥塞策略与独立流优先级。

### M6 — 完整远控能力

- 音频和麦克风。
- 虚拟显示器。
- 虚拟 HID / 游戏手柄。
- 隐私屏。
- macOS Host。
- 原生桌面/移动 Controller。
- 多区域 Relay 调度与真实公网质量评分。

## 许可证与安全说明

远程控制软件具有较高权限。请仅在你拥有或明确获得授权的设备上部署 DeskLink。公网部署时不要使用仓库中的开发默认密码或静态 TURN 凭证。

详细架构请阅读 `docs/ARCHITECTURE.md`。
