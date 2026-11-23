# 阶段 1 bring-up 人工验证清单（机器侧完成后的人测项）

> 背景：阶段 1 全部任务已机器侧完成并合并 dev（2026-11-19，feat/bringup → dev @ 6b54824）。
> 本清单为 roadmap §3 完成判据中**需要人手/人耳/人眼**的验证项（听音已通过，余 3 项）；全部通过后：
> 1. 在 `docs/dev/verification-log.md` 把对应 ⏳ 行改判 ✅（补日期/提交号/证据）；
> 2. `docs/deferred-issues.md` 删除 DI-4（键盘矩阵）条目；
> 3. `docs/design/002-port-roadmap.md` §1 阶段 1 状态 🔶 → ✅，即可考虑 dev→main 里程碑合并（§7.4）。
> 详细操作脚本存档于 SDD 任务报告（`task-5/6/7-report.md`）；本文件为长期落点，二者以本文件为准。

## 1. 触摸五点校准 + 按压截屏（P1-4）

```bash
scripts/flash.sh /dev/ttyACM0 --monitor   # 保持监视
```
- 依次轻点屏幕 **左上 / 右上 / 左下 / 右下 / 中心** 各一下；
- monitor 中 5 组 `touch (x,y)` 日志应近似 (40,40)/(1240,40)/(40,680)/(1240,680)/(640,360)，容差 ±40px；
- **硬判据**：左上坐标必须最小、右下必须最大（无旋转/镜像错误）；
- 再**按住右下角不放**，另开终端 `python3 scripts/screenshot.py /dev/ttyACM0`，目检 PNG 右下角有白色准星对准触点。
- 通过证据：准星截屏晋升本目录 + 日志行改判。

## 2. 键盘 70 键全矩阵 + 组合（P1-5，DI-4）

```bash
scripts/flash.sh /dev/ttyACM0 --monitor   # 启动自检应打印 keyboard: OK (0x6D)
```
- 逐键按下并松开全部 70 键：每键各产生一条 `KB 0x.. down …` 与 `KB 0x.. up …`，键名与丝印一致；
- 组合：`Aa+字母`（上档）、`Ctrl+Alt+Sym+任意键`、方向键 ↑↓←→ 连按——组合中每键事件独立、无丢失；
- 结束后 console 输 `KB` 核对事件计数（down 数 = up 数 = 按键次数）；
- monitor transcript 存 `firmware/build/verify/kb-test.log` 后晋升本目录（DI-4 要求）。

## 3. 440Hz 听音（P1-6）—— ✅ 已通过（2026-11-19）

- 首测无声 → 定位 es8388 输出音量初始化缺口（取证与修复见 `docs/dev/device-notes.md` §5、fix 4278509）→ 修复后人测出声。
- 音高精度未细究（以出声为判据），阶段 2 音频调优时复核（DI-6，2026-11-23 余项并入 DI-39）。

<!-- 原始步骤保留备查：
console 输入：AUD TONE 440 3000；扬声器与 3.5mm 耳机各听一次：3 秒稳定纯音，
起始无爆音、结束无咔哒；音高为标准 A4（偏高≈8% ⇒ 采样率错配，回查 fs 与 I2S 配置）。 -->


## 4. §5.4 视觉复看测试图（P1-2）

- 读取 `docs/dev/verify/1-bringup-2026-11-19/test-pattern.png`（或重抓一张）；
- 目检：8 色彩条顺序正确（白黄青绿品红蓝黑）、圆/方/线/文本可读、四角 L 标记完整、无花屏/撕裂/错位/缺色——这些是像素探针未覆盖的部分（文本可读性、几何观感）；
- 通过后在 verification-log 的 ⏳ 行改判。

---

完成余下 3 项后，阶段 1 全部门禁闭环（机器侧 8 项 + 听音人测已登记于 verification-log，见 SHA256SUMS）。
