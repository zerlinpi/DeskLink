# DeskLink 版本与发布规则

DeskLink 以仓库根目录 `VERSION` 作为项目版本的唯一权威来源。`main` 当前开发线使用 `1.0.5-dev`；正式发布 `v1.0.5` 前，必须在同一个 release commit 中把 `VERSION` 和 `apps/web/package.json` 同步为 `1.0.5`，补齐 `docs/releases/v1.0.5.md`，通过 CI 后再为该 commit 创建 `v1.0.5` Git tag。

## 版本传播

- `VERSION`：项目版本唯一权威来源。
- `apps/web/package.json`：必须与 `VERSION` 一致，由 `tools/version/check-version.py` 强制检查。
- `apps/web/package-lock.json`：依赖解析快照；CI 检查其根包名称和 dependency/devDependency 图与 `package.json` 一致，但不把它当成第二个项目版本源。
- Windows VERSIONINFO：CMake 直接读取根 `VERSION`。开发版 `1.0.5-dev` 的数值资源版本仍为 `1,0,5,0`，字符串 `FileVersion/ProductVersion` 保留完整 `1.0.5-dev`。
- Signal：Release、Packages/Container build 和 CI build 都通过 linker `-X main.buildVersion=<VERSION>` 注入，可通过 `/versionz` 查询正在运行的 build 版本。

## CI invariant

每个 push / pull request 都必须运行 `repository-invariants`，无论 component path filter 是否命中：

```bash
python tools/version/check-version.py
```

`VERSION`、`tools/version/**`、`.github/workflows/**`、`packages/protocol/**` 属于 repository-wide invariant。修改这些路径会触发 Signal、Web、Windows 和 repository checks，禁止出现只有 change detector 成功、所有真实构建都 skipped 的“假绿色”。

## GHCR 标签

普通 `main` commit 只发布：

```text
ghcr.io/zerlinpi/desklink-signal:main
ghcr.io/zerlinpi/desklink-signal:sha-<commit>
```

稳定 tag `v1.0.5` 才发布：

```text
ghcr.io/zerlinpi/desklink-signal:1.0.5
ghcr.io/zerlinpi/desklink-signal:latest
ghcr.io/zerlinpi/desklink-signal:sha-<commit>
```

预发布 tag（例如 `v1.0.5-rc.1`）发布：

```text
ghcr.io/zerlinpi/desklink-signal:1.0.5-rc.1
ghcr.io/zerlinpi/desklink-signal:sha-<commit>
```

预发布 tag 不更新 `latest`。普通 `main` push 永远不能写稳定 semver tag，因此正式镜像版本永久绑定创建该 Git tag 的 commit。

## Stable Windows Release

稳定 `vX.Y.Z` Release 必须满足：

1. `python tools/version/check-version.py --require-stable --tag "vX.Y.Z"` 通过；
2. Windows 二进制使用 SHA-256 Authenticode 签名；
3. 使用 RFC3161 时间戳；
4. `signtool verify /pa /all /v` 通过；
5. 缺少签名证书时 Release 直接失败，不能降级发布 unsigned remote-control executable。

开发 CI/本地构建允许 unsigned，但不得因此推断稳定 Release 也允许 unsigned。

## 发布后

`v1.0.5` 发布完成后，`main` 的下一个正常开发 commit 应尽快切到下一开发版本（例如 `1.0.6-dev`），避免继续在已经正式发布的版本号上累积代码。
