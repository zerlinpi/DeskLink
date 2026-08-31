DeskLink Browser Controller
===========================

DeskLink Web 现在支持“Build once, deploy anywhere”。正式发布会提供两个独立包：

- desklink-web-vX.Y.Z-dist.tar.gz：已经完成 Vite 构建，可直接部署。
- desklink-web-vX.Y.Z-source.tar.gz：Web 源码与 lockfile，供二次开发/审计。

直接部署 dist 包
----------------

1. 解压 dist 包到 HTTPS Web 根目录。
2. 编辑 desklink-config.js，填写当前部署使用的公开端点：

   - signalUrl
   - controllerSessionUrl
   - stunUrl
   - turnUrl
   - turnTlsUrl
   - turnCredentialsUrl
   - features.controllerAuthRequired
   - features.turnRuntimeRequired
   - features.forceRelay
   - features.lanFirstIce

3. 不需要重新 npm install/npm ci，也不需要重新 Vite build。
4. desklink-config.js 应使用 no-store/no-cache 策略；仓库自带 nginx.conf 已为该文件设置 no-store。

`null` 表示继续使用构建期值（如果存在），然后使用应用默认值。部署时可以直接填字符串/布尔值覆盖。

LAN Direct / TURN 回退
----------------------

默认 `features.lanFirstIce` 为启用状态：在非 relay-only 模式下，浏览器先使用 host/STUN candidates 发起直连，避免在局域网可直达时把 TURN 凭证获取放在首连关键路径上。如果约 1.5 秒内仍未建立连接，DeskLink 会补充 TURN 配置并通过 ICE restart 自动回退到 relay 能力。

- `features.lanFirstIce: true`：推荐默认值，优先尝试 LAN/P2P 直连。
- `features.lanFirstIce: false`：恢复为首连前准备完整 ICE/TURN 配置。
- `features.forceRelay: true`：始终强制 relay，LAN-first 会自动停用，适合限制网络或 TURN 验证环境。

LAN-first 只改变 ICE 候选准备与回退时机，不绕过 Controller Session、Access Code 或 WebRTC DTLS/SRTP 安全边界。

安全边界
--------

desklink-config.js 是公开静态文件，任何浏览器用户都可以读取。禁止写入：

- Access Code
- Device Credential
- 长期 Controller Secret
- Signal Auth Token
- coturn TURN REST shared secret
- 长期 TURN username/password

TURN 应优先通过 turnCredentialsUrl 获取短期凭证；Controller 应通过 controllerSessionUrl 获取短期、target-scoped session。

从源码构建
----------

Node/npm 版本按 package.json/packageManager 与 CI 配置执行。解压 source 包后：

     npm ci
     npm test
     npm run build

`npm run build` 会执行 typecheck、Vite production build，并验证 dist 中 runtime-config 契约没有被构建优化破坏。

生产部署要求
------------

- 对外使用 HTTPS/WSS 和有效证书。
- Web 与 Signal 可以同域反向代理，也可以使用 desklink-config.js 指向独立控制平面。
- 浏览器端不得保存长期 Host 凭证。
- TURN REST secret 仅存在服务端。
- 文件、剪贴板、Web Crypto、Fullscreen、Wake Lock 等浏览器能力应在 secure context 中使用。
