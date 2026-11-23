# Task 10（FM6P 深度 patch 页）视觉/像素检查记录

日期 2026-11-23；固件 = 本任务工作区构建（`scripts/build.sh` ×2，第二遍 0 警告）；设备 `/dev/ttyACM0`。
截图由 `scripts/inject/inject.py` 的 `Session.capture()` 取（SCR 协议，1280×720 RGB565），
坐标 = 屏幕绝对坐标（页面区 y + 72）。**无图像查看工具**，下面是对 PNG 的像素/区域探针结果
（PIL 读取），人眼读图仍是外部门禁。

## 三张截图的探针（全部通过）

| 探针 | locked（选中轨 = ANALOG，锁定页） | voice（FM6，PAGE VOICE） | eg-op3（PAGE EG + OP3） |
|---|---|---|---|
| 状态行区域 (1000..1264, 80..104) 非底色像素 | 404（红字 `NOT FM6`，采样点 (1200,92)=(224,84,40)≈UI_ERROR） | 0（无动作、无宏 → 空行） | 0（换页/换算子清掉结果） |
| OP3 chip 底 (142,80) | — | — | (224,176,40)=UI_ACCENT（选中） |
| OP1 chip 底 (24,80) | (224,176,40)=UI_ACCENT（op=0 选中，锁定态仍显示当前算子） | — | (32,40,48)≈UI_PANEL_HI（未选中） |
| VOICE chip 底 (660,80) | — | (224,176,40)=UI_ACCENT | — |
| EG chip 底 (388,80) | — | (32,40,48)=未选中 | (224,176,40)=选中 |
| 旋钮盒非底色像素（槽 0..7） | 8 个都有（全部 `--`，弧底 + 名称 + `--`） | 4545/4658/5086/4933/**0/0/0/0**（VOICE = 4 字段 + 4 空槽隐藏） | 4963/4809/4715/4813/4973/4782/4406/4399（EG = 8 字段全建） |
| BANK 行区域 (792..916, 76..108) 非底色 | 3852（`-  B01  +`） | 3852 | 3852 |
| SAVE chip 区域 (924..996, 76..108) 非底色 | 2188 | 2188 | 2188 |
| 底部细调区域 (16..184, 624..668) 非底色 | 7276 | 7276 | 7276 |

## 结论

- 页眉三组控件（12 chip / BANK 三区 / SAVE / 状态行）都绘制在预期位置；选中 chip 用
  UI_ACCENT，未选中用 UI_PANEL_HI。
- 空槽隐藏生效（VOICE 页槽 4..7 区域 0 个非底色像素），没有残留旧槽内容。
- 锁定页状态行为红字 `NOT FM6`；解锁且无动作时为空行（不显示过期结果）。
- 设备侧行为断言（同一 transcript，`task-10-fm6p-transcript.log`）：锁定页
  `TAP SAVE` → `-> FM6P SAVE ERR locked status="NOT FM6"`；`TAP OP2` →
  `-> FM6P op=0 page=0 select`（选择不变）；底部 `+` →
  `-> FM6P R1 value=99 ERR locked status="NOT FM6"`。解锁后（`ENG TRACK 1 9 0`）：
  `STATE SNAP fm6p_engine_ok=1`、`-> FM6P ALG value=5 status="EDIT OK"`
  （`FM6P INFO` 的 `alg` 4→5）、`PARAM 63 5` 后 `-> FM6P ALG value=6 status="ALG MACRO 5"`
  且 `PARAM 63` 回读 0、`TAP BANK +/SAVE` → `slot=1 select` / `status="SAVE OK"` 且
  `STORE BANK GET 1` 与 `STORE BANK LIVE` 逐字节相同（随后 ERASE 收口）。
- 六页 × 8 槽的字段名逐一核对（TAP 底部 ± 回显）：EG=R1..R4,L1..L4；
  LEVEL=BP,LD,RD,LC,RC,RS,KVS,OL；PITCH=MODE,FC,FF,DET,AMS(+3 空)；
  PEG=PR1..PR4,PL1..PL4；VOICE=ALG,FB,OKS,TRNSP(+4 空)；LFO=LFS,LFD,LPMD,LAMD,LKS,LFW,LPMS(+1 空)。
- `UI MEM`：`page id=13 title=FM6P est=7168 delta=7080`（首建）/`6892`（第二次跨组重建）
  —— 两次都 ≤ est；`GROUP SYNTH sum_est=20480` 不变。
- 设备收口：轨 1 引擎回 ANALOG preset 0、选中轨回 3（鼓轨）、`STORE BANK LIST` 全空、
  `STORE AUTO ON`（`autosave_writes=0`，未改写工作工程）。

## 附：Task 9 遗留 minor（`DSYN CRUSH` 回读目标 kit）

`task-10-crush-fix-transcript.log`：选中 kit 0 时 `DSYN CRUSH 2 7` 回 `crush=7`（读目标 kit 2，
不再是选中 kit 的投影），随后写回原值 0；`DSYN STORE` 后 `DSYN INFO dirty=0`（收口，DSU 字节不变）。
