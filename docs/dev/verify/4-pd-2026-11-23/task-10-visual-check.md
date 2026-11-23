# Task 10（FM6P 深度 patch 页）视觉/像素检查记录（含评审修复轮）

日期 2026-11-23。**固件** = 修复轮源码构建（`scripts/build.sh` ×2：首遍 `task-10-build1.log`
仅 §5.5 豁免的 CMake/kconfig 提示对且 `console.c` 已重编；次遍 `task-10-build2.log`
`console.c`/`page_fm6p.c`/`cmd.c` 均重编、warning/error **0**），启动 `App version:
d301f46-dirty`（`task-10-boot-log.txt`）。设备 `/dev/ttyACM0`。

> **更正记录**：首轮本文写「第二遍 0 警告」是不成立的——那一轮存档
> `task-10-build2-superseded-warn.log` 实际含 `console.c:1219/1220` 的两条
> `-Wunused-variable`，因为「第二遍」是一次没有重编 `console.c` 的增量 no-op 构建。
> 修复轮删除死变量后重编，并以 `task-10-build1/2.log` 作为真实证据。

截图由 `scripts/inject/inject.py` 的 `Session.capture()` 取（SCR 协议，1280×720 RGB565），
坐标 = 屏幕绝对坐标（页面区 y + 72）。**无图像查看工具**，下面是对 PNG 的像素/区域探针结果
（PIL 读取），人眼读图仍是外部门禁。截图三张取自首轮镜像；修复轮只改状态层/几何常量，
像素结论不依赖它们（修复轮的设备探针见 §3）。

## 1. 三张截图的探针（全部通过）

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

## 2. 首轮设备行为断言（`task-10-fm6p-transcript.log`，同一镜像）

- 锁定页：`TAP SAVE` → `-> FM6P SAVE ERR locked status="NOT FM6"`；`TAP OP2` →
  `op=0 page=0 select`（选择不变）；底部 `+` → `R1 value=99 ERR locked`。
- 解锁（`ENG TRACK 1 9 0`）：`fm6p_engine_ok=1`；`-> FM6P ALG value=5 status="EDIT OK"`；
  `PARAM 63 5` 后写 ALG 曾显示 `ALG MACRO 5`（**修复轮已改为立即 `EDIT OK`**，见 §3）；
  `TAP BANK +/SAVE` → `slot=1 select` / `status="SAVE OK"`，`STORE BANK GET 1` ==
  `STORE BANK LIVE`（逐字节），随后 ERASE 收口。
- 六页 × 8 槽字段名逐一核对：EG=R1..R4,L1..L4；LEVEL=BP,LD,RD,LC,RC,RS,KVS,OL；
  PITCH=MODE,FC,FF,DET,AMS(+3 空)；PEG=PR1..PR4,PL1..PL4；VOICE=ALG,FB,OKS,TRNSP(+4 空)；
  LFO=LFS,LFD,LPMD,LAMD,LKS,LFW,LPMS(+1 空)。
- `UI MEM`：`page id=13 est=7168 delta=7080`（首建）/`6892`（二次重建）。

## 3. 修复轮设备探针（`task-10-fix-transcript.log`，最终镜像）

- `UI MEM page id=13 est=7168 delta=7124`（≤ est；观测区间 6,892–7,124）。
- **底部细调死区**（`FM6P_ADJ_ZONE` 84→72，对象宽 168 ⇒ 死区页面区 x∈[72,96) =
  屏幕 x∈[88,112)）：`TAP 84 646`（rel=68，− 区）→ `-> FM6P R1 value=94 status="EDIT OK"`；
  `TAP 96 646`（rel=80，死区）→ `TAP miss 96,646 page=13`（非零退出、未写值）；
  `TAP 58 646`（− 区）→ `R1 value=93`。
- **ALG 回显同拍**：`TAP 684 92`（PAGE VOICE）→ `op=0 page=4 select`；`PARAM 63 5`
  （`PARAM 63` 回读 5）→ **立即** `TAP 142 646` ⇒ `-> FM6P ALG value=5 status="EDIT OK"`
  （不再残留 `ALG MACRO 5`），随后 `PARAM 63` 回读 **0**、`FM6P INFO 1` 的 `alg=5 macro=0`。
- **锁定页 BANK ±**：`ENG TRACK 1 0 0` 后 `TAP 896 92`/`TAP 812 92` ⇒
  `-> FM6P BANK slot=0 select`（槽号不变、无命令）；`TAP SAVE` ⇒ `ERR locked status="NOT FM6"`；
  底部 `+` ⇒ `ALG value=5 ERR locked status="NOT FM6"`。
- **字段映射复跑（最终镜像）**：六页 × 8 槽与 §2 完全一致。
- 设备收口：轨 1 引擎回 ANALOG preset 0、选中轨回 3（鼓轨）、`STORE BANK LIST` 全空、
  `STORE AUTO ON`。

## 4. 附：Task 9 遗留 minor（`DSYN CRUSH` 回读目标 kit）

`task-10-crush-fix-transcript.log`：选中 kit 0 时 `DSYN CRUSH 2 7` 回 `crush=7`（读目标
kit 2，不再是选中 kit 的投影），随后写回原值 0；`DSYN STORE` 后 `DSYN INFO dirty=0`
（收口，DSU 字节不变）。
