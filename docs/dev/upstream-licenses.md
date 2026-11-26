# 上游 SLOOP 许可核查（Task 1）

> 来源：isod89/sloop-fm1 @ `bac9c05ea7627449405aa10e48da21d75b6c8c56`（SLOOP 2.8.5）｜ 核查日期：2026-11-20

随精简子树保留的许可与署名文件：

- `LICENSE`：GPL-3.0 全文；上游代码及本项目衍生固件整体按 GPL-3.0-only 分发。
- `LICENSING.md`：上游许可总说明，含第三方组件与资产边界。
- `LICENSES/Apache-2.0-msfa.txt`：Dexed msfa / FM6 的 Apache-2.0 文本。
- `assets/samples-cc0/CREDITS.txt`、`assets/samples-cc0/ATTRIBUTION.txt`：内置采样的 CC0 1.0 来源与署名。
- `assets/hiphop-pack/CREDITS.txt`：可选 hip-hop 包的 CC0/CC BY 3.0 来源与署名；对应音频未随本任务入库。
- `assets/fonts/Terminus-LICENSE.txt`：Terminus 字体的 SIL OFL 1.1 许可文本；字体数据未随精简子树入库。
- `web/FUKIAI-LICENSE.txt`、`docs/webapp/editor/FUKIAI-LICENSE.txt`：Fukiai 图标字体的 MIT 许可文本（两路径内容相同）；字体数据未随精简子树入库。

DaisySP / Mutable Rings 物理建模代码的 MIT notice 保留在 `firmware/src/phys_dsp.c`、`firmware/src/phys_symp.c` 文件头，无独立 LICENSES 文本可另搬。CC0 WAV（`assets/samples-cc0/`）**自 e3b7a32 起已随树入库**（42 个文件，manifest 233 条），`tools/fetch_cc0.py` 仅作缺失时的重新获取手段。上游 `LICENSING.md` 所述 Hügelton Sample Pack 不进入 SLOOP 固件，本任务也未引入任何该包音频。

---

## Sloop Go 参考副本（`third_party/sloop-fm1-go`）

> 来源：jahlib/sloop-fm1-go @ `ed941b5853f551d1628bc3aecce4fa2010cd6f67`（Sloop Go 1.8.0）｜ 登记日期：2026-11-26

性质：**只读参考副本**（UI 源码 + 截图），供阶段 5 的界面对齐与协议实现对照离线阅读；**不编入固件**，不属于任何构建目标。清单与 SLOOP 共用 `third_party/MANIFEST.sha256`，抓取脚本 `tools/fetch_sloop_go.sh`。

随副本保留的许可与署名文件：

- `LICENSE`：GPL-3.0 全文；上游 `NOTICE.md` 明示为 **GPL-3.0-only**（"GNU General Public License v3.0 only"），多数源码文件头亦标 `SPDX-License-Identifier: GPL-3.0-only`。
- `NOTICE.md`：上游致谢——FM6 算法表取自 msfa（Apache-2.0，经 Dexed 与 Felucca）；启动图标是 SLOOP 图标的矢量转换，上游声明该图标**没有单独许可**、复用前须先与作者确认（本副本只读参考，不复用该图标）。
- `fonts/OFL.txt`：JetBrains Mono 的 SIL OFL 1.1 许可文本；字体数据 `fonts/JetBrainsMono.ttf` 随副本入库（`android/app/src/main/res/font/jetbrains_mono.ttf` 是同一文件的另一份拷贝）。当前**未编入固件**；阶段 5F 若把该字体位图化编入固件，届时在此补记。
