# Rust Core Property / Stress Gates

> 状态：Task 6 验证中。本文档定义门禁，不在完整 `--locked` CI 成功前标记 PASS。

## Generation high-water contract

`RemoteSessionStateMachine` 必须把“当前 authority”和“已见 generation 高水位”分开：

- Session 关闭后，旧/相同 Session generation 不能重新启动会话。
- Control / Pointer channel 关闭后，旧/相同已关闭 generation 不能重新获得 authority。
- Operation timeout 后，旧/相同已结束 generation 不能重新成为当前 operation。
- 新 Session 或新 Peer 建立新的 scoped authority；旧 scope 的回调必须被忽略。
- 任何 stale event 返回 `IgnoreStaleEvent`，且 authoritative snapshot 不发生变化。

确定性 RED 证据：GitHub Actions run `33390962135`，4 个 high-water 合同测试在旧实现上失败；随后最小 reducer 修复进入 GREEN 验证。

## Property gate

`crates/desklink-session/tests/property_stress.rs::stale_generations_never_replace_authority`

- Framework: `proptest = 1.11.0`（精确锁定）
- Cases: `10,000`
- 随机生成 Session / Peer / Control / Pointer / Operation generation
- 对每类 stale callback 比较调用前后的完整 authoritative snapshot
- 覆盖 current authority、关闭/timeout 后的 high-water，以及 session close 后旧 Start

通过条件：10,000 cases 全部完成，无 mutation mismatch、panic 或 reducer error。

## Deterministic stress gate

`crates/desklink-session/tests/property_stress.rs::million_event_stale_race_stress_preserves_authority`

- Event count: exactly `1,000,000`
- Fixed seed: `0xD35C_11A5_C0DE_2026`
- PRNG: test-local xorshift64；不依赖外部随机源
- 混合事件：Control/Pointer/Operation generation advance、close/timeout、旧 generation replay、旧 Session callback、旧 Peer callback
- 每个事件后把 reducer 的 current authority 与测试侧模型逐项对照

通过条件：1,000,000 个事件全部完成，Session 始终保持 Connected、当前 Session/Peer 不被 stale callback 替换，三个 scoped authority 与模型完全一致。

## CI evidence required before PASS

Task 6 只有在同一最新 commit 上以下步骤全部成功后才标记 PASS：

1. `cargo fmt --check`
2. `cargo clippy --locked --workspace --all-targets -- -D warnings`
3. `cargo test --locked --workspace --all-targets`
4. `cargo build --locked -p desklink-core-ffi --release`

`cargo check`、one-shot lock refresh 或 benchmark workflow 单独成功均不足以宣称 Task 6 完成。
