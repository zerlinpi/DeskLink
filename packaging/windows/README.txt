DeskLink Windows 被控端
======================

推荐方式：双击 DeskLink.exe
-------------------------

v1.0.1 起，Windows 发布包提供原生 DeskLink.exe 设置管理器。

1. 解压完整的 Windows ZIP，不要只单独复制某一个 exe。
2. 双击 DeskLink.exe，并允许 Windows 的管理员/UAC 提示。
3. 填写：
   - 信令服务器：例如 wss://control.example.com/ws
   - 设备 ID：例如 office-pc-01
   - 访问码：首次安装必填，之后更新时可留空保留原值
   - STUN/TURN：公网远控建议填写你自己的 TURN 服务
   - 信令令牌接口 / TURN 凭证接口 / dc2 设备凭证：生产鉴权部署时填写
4. 点击“安装 / 更新并启动”。
5. 服务状态显示“运行中”后，点击“连接诊断”。
6. 诊断没有 [失败] 项后，再在 Web 控制端输入设备 ID 和访问码连接。

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

访问码和设备凭证不会写进命令行或普通环境变量，而是通过 Windows
machine-scope DPAPI 加密保存在 %ProgramData%\DeskLink。

重要：DeskLink 目前是自部署远控项目。Windows 被控端必须能够连接到已经部署
好的 DeskLink Signal 服务；公网使用还应部署 TURN。只下载 exe、但没有可访问的
信令服务器时，远控连接不会成立。

发布包内容
----------

  DeskLink.exe            Windows 图形化设置/管理器与连接诊断（推荐入口）
  desklink-agent.exe      被控端实时采集、编码、WebRTC 与输入处理进程
  desklink-service.exe    LocalSystem 无人值守后台服务
  install-service.ps1     管理员/自动化安装脚本
  uninstall-service.ps1   卸载脚本

命令行 / 自动化安装
------------------

管理员 PowerShell：

  .\install-service.ps1 `
    -SignalUrl "wss://control.example.com/ws" `
    -DeviceId "office-pc-01" `
    -SignalTokenUrl "https://control.example.com/api/v1/signal-token" `
    -StunUrl "stun:turn.example.com:3478" `
    -TurnHost "turn.example.com" `
    -TurnCredentialsUrl "https://control.example.com/api/v1/turn-credentials"

v1.0.1 起，非敏感配置写入 DeskLink Windows Service 自己的 Environment
注册表值，并在服务重启时立即生效，不再依赖可能需要重启 Windows 才刷新的
Machine Environment Variables。

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
     desklink-service.exe 进行 Authenticode SHA-256 签名；
  3. 使用可信时间戳服务；
  4. 保持稳定发布者身份逐步建立 SmartScreen reputation。

仓库的发布流程会提供可选 Authenticode 签名钩子；没有配置证书 Secret 时仍可
生成开发/测试包，但 Windows 可能继续显示未知发布者提示。

卸载
----

  .\uninstall-service.ps1

彻底移除保护的设备身份和安装文件：

  .\uninstall-service.ps1 -RemoveProtectedSecrets -RemoveFiles
