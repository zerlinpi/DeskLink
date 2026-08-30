DeskLink Windows 被控端
======================

推荐方式：双击 DeskLink.exe
-------------------------

Windows 发布包提供原生 DeskLink.exe 设置管理器，不需要手工创建环境变量。

1. 解压完整的 Windows ZIP，不要只单独复制某一个 exe。
2. 双击 DeskLink.exe，并允许 Windows 的管理员/UAC 提示。
3. 填写：
   - 信令服务器：例如 wss://control.example.com/ws
   - 设备 ID：例如 office-pc-01
   - 访问码：首次安装必填，之后更新时可留空保留原值
   - STUN/TURN：公网远控建议填写你自己的 TURN 服务
   - 信令令牌接口 / TURN 凭证接口 / dc2 设备凭证：生产鉴权部署时填写
   - 目标帧率 FPS：默认 60，可填写 15-144；高刷设备可尝试 90 / 120 / 144
4. 点击“安装 / 更新并启动”。
5. 服务状态显示“运行中”后，点击“连接诊断”。
6. 诊断没有 [失败] 项后，再在 Web 控制端输入设备 ID 和访问码连接。

高刷新率说明
------------

目标 FPS 支持 15-144，但 144 FPS 是目标能力，不代表所有电脑都一定能稳定输出。
实际可用帧率取决于：

  - 被控显示器刷新率；
  - Intel / NVIDIA / AMD 显卡和驱动；
  - Media Foundation 硬件 H.264 Encoder 能力；
  - 网络带宽、RTT、丢包和抖动；
  - 浏览器硬件解码和显示能力。

高刷视频策略使用 144 -> 120 -> 90 -> 60 -> 45 -> 30 -> 24 -> 15 的稳定档位。
如果硬件视频管线无法在请求的高 FPS 初始化，Agent 会尝试较低兼容高刷档位，
而不是直接失去画面。网络拥堵时仍然优先保护输入延迟，不会为了保持高 FPS 无限排队。

连接诊断
--------

新版 DeskLink.exe 可直接检查：

  - DeskLink Windows Service 是否正在运行；
  - 设备 ID 格式；
  - 无人值守访问码是否已用 DPAPI 保存；
  - Signal /healthz 是否可访问；
  - 公网 Signal 是否仍在使用不安全的 ws://；
  - STUN 是否已配置；
  - TURN 主机 DNS 与 TCP 端口是否可达；
  - Signal Token 模式是否已配置 dc2 设备凭证；
  - 是否已配置短期 TURN Credential API。

诊断结果分为 [通过] / [警告] / [失败]。

  [失败] 代表会直接阻断当前远控链路，应先修复。
  [警告] 代表当前可能能用，但跨公网、复杂 NAT 或正式生产环境存在风险。

“复制诊断”可以把不包含访问码和 dc2 明文的诊断结果复制出来，方便排查。
“复制设备 ID”可以直接复制当前被控端 ID，减少手工输入错误。

GPU / 画面媒体自检
------------------

Windows 发布包包含：

  desklink-media-probe.exe

在远端电脑已经登录到正常桌面的情况下，可以双击或在 PowerShell 中运行：

  .\desklink-media-probe.exe

它不会连接 Signal/TURN，也不会接受远程控制，只在本机实际检查 DeskLink 视频管线：

  1. 创建 D3D11 硬件设备并显示当前 GPU；
  2. 初始化 DXGI Desktop Duplication；
  3. 枚举显示器并尝试取得真实桌面帧；
  4. 使用 D3D11 Video Processor 做 BGRA -> NV12 GPU 转换；
  5. 初始化 Media Foundation 硬件 H.264 编码器；
  6. 实际编码一个 H.264 access unit。

成功时最后会输出：

  MEDIA_PROBE_OK

如果失败，程序会输出对应的 [FAIL] 阶段并返回非 0 退出码。典型意义：

  20  D3D11 硬件设备不可用 / 显卡驱动问题
  30  DXGI Desktop Duplication 不可用 / 非交互桌面会话
  40  GPU Video Processor / NV12 转换不可用
  50  没有兼容的硬件 H.264 Media Foundation 编码器
  51  编码器初始化成功但无法真正产出 H.264 数据

如果 Web 能连接并控制鼠标键盘、但始终没有画面，优先运行此探针。

锁屏 / 用户切换 / RDP
---------------------

DeskLink Service 会监督当前活动交互用户会话。可用时优先 Console；没有可用活动
Console 用户时，会选择真实 WTSActive RDP 会话，因此 Windows Server / RDP-only
主机不再只依赖物理控制台。

锁屏、Fast User Switching、显示模式变化等情况可能让 DXGI Desktop Duplication
返回 DXGI_ERROR_ACCESS_LOST。DeskLink 会在后台节流重试重建 Desktop Duplication；
回到正常可访问桌面后，画面应自动恢复，不需要因为一次重建失败就手工重启 Service。

注意：这不等于已经支持 Windows 登录界面或 UAC Secure Desktop。当前版本不会通过
关闭 UAC 或降低 Windows 安全策略来实现这类控制。

输入断线保护
------------

浏览器失焦、页面隐藏、手动断开、输入 DataChannel 关闭、WebRTC 连接断开/失败和
Service/Agent 停止都会触发远端输入释放。Host 会跟踪已注入的键和鼠标按钮；如果
某次 KEYUP/MOUSEUP 因临时桌面权限边界失败，该状态不会被提前忘记，后续 cleanup
仍可再次尝试释放，降低 Ctrl/Alt/Shift/Win 或鼠标键“粘住”的概率。

访问码和设备凭证不会写进命令行或普通环境变量，而是通过 Windows
machine-scope DPAPI 加密保存在 %ProgramData%\DeskLink。

重要：DeskLink 目前是自部署远控项目。Windows 被控端必须能够连接到已经部署
好的 DeskLink Signal 服务；公网使用还应部署 TURN。只下载 exe、但没有可访问的
信令服务器时，远控连接不会成立。

发布包内容
----------

  DeskLink.exe                 Windows 图形化设置/管理器与连接诊断（推荐入口）
  desklink-agent.exe           被控端实时采集、编码、WebRTC 与输入处理进程
  desklink-service.exe         LocalSystem 无人值守后台服务
  desklink-media-probe.exe     本机 DXGI/D3D11/H.264 视频管线自检
  install-service.ps1          管理员/自动化安装脚本
  uninstall-service.ps1        卸载脚本

命令行 / 自动化安装
------------------

管理员 PowerShell：

  .\install-service.ps1 `
    -SignalUrl "wss://control.example.com/ws" `
    -DeviceId "office-pc-01" `
    -SignalTokenUrl "https://control.example.com/api/v1/signal-token" `
    -StunUrl "stun:turn.example.com:3478" `
    -TurnHost "turn.example.com" `
    -TurnCredentialsUrl "https://control.example.com/api/v1/turn-credentials" `
    -Fps 60

可选：

  -Fps 120
  -BitrateBps 20000000

非敏感配置写入 DeskLink Windows Service 自己的 Environment 注册表值，并在服务
重启时立即生效，不再依赖可能需要重启 Windows 才刷新的 Machine Environment Variables。

如启用了 SignalTokenUrl，请配置服务器生成的 dc2 设备凭证：

  & "$env:ProgramFiles\DeskLink\desklink-service.exe" --store-device-credential

配置无人值守访问码：

  & "$env:ProgramFiles\DeskLink\desklink-service.exe" --store-access-code

然后：

  Restart-Service DeskLink

Windows “未知发布者 / Windows 已保护你的电脑”提示
-----------------------------------------------

这不是 DeskLink 自己的报错，而是 Windows SmartScreen 对未建立发布信誉或没有
受信任 Authenticode 签名的互联网下载程序的提示。

代码层面无法伪造一个受 Windows 信任的发布者。正式消除此提示需要：

  1. 购买或申请受 Windows 信任链认可的 OV/EV 代码签名证书；
  2. 在 CI 发布时使用 signtool 对 DeskLink.exe、desklink-agent.exe、
     desklink-service.exe、desklink-media-probe.exe 进行 Authenticode SHA-256 签名；
  3. 使用可信时间戳服务；
  4. 保持稳定发布者身份逐步建立 SmartScreen reputation。

仓库的发布流程提供可选 Authenticode 签名钩子；没有配置证书 Secret 时仍可生成
开发/测试包，但 Windows 可能继续显示未知发布者提示。

卸载
----

  .\uninstall-service.ps1

彻底移除保护的设备身份和安装文件：

  .\uninstall-service.ps1 -RemoveProtectedSecrets -RemoveFiles
