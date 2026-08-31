# DeskLink production deployment

This stack turns the server side into one same-origin HTTPS deployment:

```text
Browser
  |
  | HTTPS / WSS
  v
Caddy :443
  |
  v
DeskLink Web (Nginx)
  |-- /ws ---------> Signal :8080
  |-- /api/v1/* ---> Signal :8080
  |
  +---------------- WebRTC ------------------+
                                              |
                                  P2P first / TURN fallback
                                              |
                                      coturn :3478
```

## Requirements

- Linux server with a public IPv4 address.
- Docker Engine + Docker Compose plugin.
- A DNS A/AAAA record for the DeskLink domain pointing to the server.
- Firewall/security-group rules:
  - TCP 80, 443, 3478
  - UDP 443, 3478
  - UDP 49160-49200

`coturn` uses host networking in this production profile, so run this stack on Linux.

## 1. Bootstrap credentials

From the repository root:

```bash
cd infra/production
sh bootstrap.sh control.example.com 203.0.113.10 office-pc-01 admin
```

The script creates:

- `.env` with independent random Signal and TURN master secrets;
- `secrets/devices.json` containing only the SHA-256 hash of the Windows host credential;
- `secrets/controllers.json` containing only the SHA-256 hash of the Web controller key and its allowed device IDs;
- an empty device revocation file.

It prints two values once:

- `dc2...` — enter this in the Windows `DeskLink.exe` **设备凭证** field;
- `ck1...` — enter this in the Web controller together with the controller account.

Do not commit `.env`, `secrets/`, `dc2...`, or `ck1...` values.

## 2. Start the server

```bash
docker compose up -d --build
```

Caddy obtains and renews the public HTTPS certificate automatically. The Web controller is built with same-origin Signal/API URLs, so browser traffic uses the public HTTPS/WSS origin instead of exposing internal container addresses.

Check:

```bash
docker compose ps
curl -fsS https://control.example.com/healthz
```

Expected health response:

```json
{"ok":true}
```

## 3. Configure the Windows host

Open `DeskLink.exe` as administrator and use:

```text
信令服务器       wss://control.example.com/ws
设备 ID          office-pc-01
访问码            <choose a strong unattended access code>
STUN 地址         stun:control.example.com:3478
TURN 主机         control.example.com
TURN 端口         3478
信令令牌接口      https://control.example.com/api/v1/signal-token
TURN 凭证接口     https://control.example.com/api/v1/turn-credentials
设备凭证          dc2... printed by bootstrap.sh
```

Click **安装 / 更新并启动**, then **连接诊断**. Resolve every `[失败]` item before testing from another network.

## 4. Connect from the browser

Open:

```text
https://control.example.com
```

Enter:

- controller account (`admin` in the example);
- the `ck1...` controller key printed by bootstrap;
- device ID (`office-pc-01`);
- the Windows host access code.

DeskLink first attempts Direct P2P. If NAT traversal fails, it obtains a short-lived TURN credential from Signal and falls back to coturn.

## Credential rotation

Rotate one Windows device credential:

```bash
docker run --rm \
  -v "$(cd ../.. && pwd):/repo" \
  -w /repo golang:1.23-alpine \
  go run ./tools/auth/rotate-device-registry-credential.go \
  ./infra/production/secrets/devices.json office-pc-01
chmod 444 secrets/devices.json
```

Copy the new `dc2...` value into the Windows `DeskLink.exe` manager and apply the configuration.

Rotate a controller key and its allowed devices:

```bash
docker run --rm \
  -v "$(cd ../.. && pwd):/repo" \
  -w /repo golang:1.23-alpine \
  go run ./tools/auth/set-controller-registry-key.go \
  ./infra/production/secrets/controllers.json admin office-pc-01
chmod 444 secrets/controllers.json
```

## SmartScreen note

This production server stack does not change Windows executable trust. Windows SmartScreen warnings are solved by signing `DeskLink.exe`, `desklink-agent.exe`, and `desklink-service.exe` with a trusted Authenticode OV/EV certificate in the Release workflow. Without such a certificate, Windows may still show an unknown-publisher warning even when the server deployment is correct.
