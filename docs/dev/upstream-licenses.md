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
