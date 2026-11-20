# ADR-004: 上游 sloop-fm1 引入机制 — vendoring + 固定 commit 抓取脚本

> 状态：已接受（阶段 2 开工决策，D1）｜ 日期：2026-11-20
> 关联：`docs/superpowers/specs/2026-11-20-phase2-engine-port-design.md` §3；AGENTS.md §8（上游跟进）

## 背景

阶段 2 需要把上游 SLOOP 核心（bac9c05，GPL-3.0）引入本仓库构建 `sloop_core`。约束：构建可复现、GPL 对应源完整、上游快速迭代下可 diff 跟随、上游 `src/` 零修改（适配层隔离）。

## 选项

| 选项 | 评估 |
|---|---|
| **A. vendoring（采纳）**：`third_party/sloop-fm1/` 精简上游子树，`tools/fetch_upstream.sh` 按固定 commit 抓取 + SHA 校验 | 构建不依赖网络/submodule 状态；许可文件随仓；升级 = 改 pin 重跑脚本 + `git diff third_party/` |
| B. git submodule | pin 干净但引入 init/网络状态依赖；GPL 对应源随仓性弱；克隆与升级持续依赖外部通道和凭据，vendored 树入库后则可完全离线构建 |
| C. subtree | 与 A 能力相当，但合并上游历史进本仓库历史，diff 噪音大；无额外收益 |

## 决策

**A**。`third_party/sloop-fm1/` 只读禁改；适配层全部在 `firmware/components/sloop_core/`（组合 TU + Tab5 shim）。fetch 脚本：
- pin 完整 commit SHA（当前 `bac9c05ea7627449405aa10e48da21d75b6c8c56`）；
- 抓取后生成并校验全部 vendored 文件的 SHA256 清单（`third_party/MANIFEST.sha256`）；
- 默认经 SSH `git@github.com:isod89/sloop-fm1.git` 抓取（2026-11-20 实证可用），保留 `--mirror` 备用前缀与 `--ref` 升级参数；本机 HTTPS git/curl 已实证失败。

## 后果

- 仓库体积增加；实际 vendor 构建所需子树、协议/README 与全部许可/署名文件（233 文件），不搬上游 docs 正文与站点；`assets/samples-cc0/` 随树（golden 保真所需）。
- 升级流程明确：改 pin → 重跑脚本 → 审 diff → 重跑 §5.5 全门禁（含 host-test.sh golden）。
- 网络通道已由 Task 1 实证为 SSH；一旦树入库，构建完全离线。
