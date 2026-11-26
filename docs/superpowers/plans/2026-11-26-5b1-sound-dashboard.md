# 5B-1 Sound 单轨仪表盘 Implementation Plan（含 5B-0 原型验证）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把现有 SOUND 参数页换成 Sloop Go 式的「单轨编辑仪表盘」：页眉选轨/换引擎/按名选预置，双列可滚动的 13 张卡片呈现选中轨的全部参数组（ANALOG 引擎 109 个槽），触摸拖动、键盘、`TAP`/`KEY` 注入都能改到每一个参数；退役 MOD/SCALE/VOICE/GLOBAL/FX 五张参数页。

**Architecture:** 卡片成员是纯数据表（`sound_layout.{h,c}`，host 可测）；页面主体是 `page_sound.{h,c}`——一个滚动容器 + 13 张自绘卡片对象，槽的状态放 PSRAM 静态数组；它作为参数页框架（`page_params.c`）的一个新特征位 `PARAM_FEAT_DASHBOARD` 挂在 SOUND 页上，页眉两行（ENGINE/PRESET/KIT ±）、FM6 银行行、页级 `−/+` 细调按钮全部复用现有代码。滚动手动驱动（`lv_obj_scroll_by_bounded`），卡片接管自己的手势。键盘沿用 ADR-008 语义，在 `sloop_ui_param_focus_move/adjust` 里分流。

**Tech Stack:** ESP-IDF v5.5.5、LVGL 9.6（`lv_draw_arc`/`lv_draw_label`/`lv_obj_scroll_by_bounded`）、FreeRTOS、gcc host 单测、pyserial 注入。

**Spec:** `docs/superpowers/specs/2026-11-26-phase5b-sound-mixer-device-design.md`（§4.2–§4.3、§5.2–§5.3、§5.6–§5.8、§9）。本计划只覆盖 §7 的 **5B-0 + 5B-1**；Mixer（5B-2）、Device（5B-3）、收口（5B-4）各自另有计划。

## Global Constraints

- 页面区原点 `UI_TAB_BAR_H 72`、`knob.c` 的 160px 旋钮几何、切页顺序「建目标 → 切换 → 销毁源组」、`CONFIG_LV_MEM_SIZE`：**一律不动**。
- Sound 页 `est_bytes = 12288`，**上限 12,768**（基础占用按 14,000B 计，SEQ 两页全建 → 首建 Sound：14,000 + 32,256 + est + 4,096 ≤ 63,120）。实测超 est 时先改对象设计（第一退路：银行行 7 个对象并成 1 个自绘对象；第二退路：细调按钮并入提示行对象），**不得抬 est、不得扩池**。SEQ → Sound 过渡期实测 `pool_free ≥ 4,096`。
- 新增静态数组（槽状态、布局缓存）必须落 PSRAM（`EXT_RAM_BSS_ATTR` 或 `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`）；内部堆门线不变（`ENG MEM internal_min ≥ 61,440`）。
- 槽数按引擎钉死：ANALOG **109**、其它合成引擎 **108**、SAMPLE **106**（`edit[5]/edit[7]` 占位被过滤）；`G_DRCH` 在 Drums 与 MIDI 两卡各占一槽。
- 键盘语义（ADR-008）：`←/→` 焦点 ±1（扁平序，环绕，跳过不存在的槽与折叠卡片）、`↑/↓` 值 ∓1、`ENTER` +1；`TAB` 翻页不变。
- 所有写操作只经 `state_apply`：TRACK 作用域 `CMD_SET_PARAM{a=选中轨, b=id, v}`，GLOBAL 作用域 `CMD_SET_GLOBAL{a=id, v}`。鼓轨逐槽规则：`drum && scope != GLOBAL && drum_ok == 0` ⇒ 锁定（`--`，不发命令）。
- 手势实施约束（已对照 LVGL 9.6 源码）：滚动容器与卡片对象都清 `LV_OBJ_FLAG_SCROLLABLE`；手动滚动用 `lv_obj_scroll_by_bounded()`；`PRESS_LOCK` 默认开着；滚动手势结束后的 `CLICKED`/`SHORT_CLICKED`/长按必须被抑制。
- 构建零新增警告；构建/烧录只走 `scripts/build.sh`、`scripts/flash.sh`；提交按 AGENTS.md §7（`<type>(<模块>): 中文摘要` + 四标签；不同 type 不混提）；每任务 review clean 后 push；不合并 `main`。
- 设备步骤凡标 **[操作员]** 的需人手（拔插/拧旋钮）；拿不到操作员时做完其余步骤，在报告里列 `WAITING_OPERATOR`，不得声称通过。

## Review Focus

1. **拖动中途手指滑出环带**：已开始的旋钮拖动在松手前一直有效（环带外只忽略该次移动事件，不结束拖动、不切换成滚动）；松手后的 `CLICKED` 不得触发卡片折叠。（Task 2、Task 3 的设备断言）
2. **焦点槽在上下文变化后消失**（如焦点在 `P_FTYPE` 时换到非 ANALOG 引擎，或换到鼓轨）：焦点夹到最近的存在槽，`UI SOUND FOCUS` 始终返回存在的槽，不崩溃。（Task 3 host 测试 `sound_layout_flat_move` + 设备断言）
3. **折叠/展开后滚动偏移越界**：滚动被重新钳位到 `[0, content_h − view_h]`，焦点槽保持可见。（Task 4 设备断言）
4. **选择器打开时上下文变化**（SEL 层 Ctrl+1..4 换轨、或 `CMD_SET_ENGINE` 换引擎）：选择器立即关闭，不留悬空的列表与高亮。（Task 6 设备断言）
5. **诊断冻结下的写**（`ENG FREEZE ON`）：拖动/键盘/`TAP` 改值被状态层拒绝——值不变、无崩溃；解冻后恢复。（Task 3 设备断言）

---

### Task 0: 文档前置（路线与阶段 5 总设计的修订）

**Files:**
- Modify: `docs/design/002-port-roadmap.md`（§1 总览第 5 行、§7 子阶段表、§9 变更记录）
- Modify: `docs/superpowers/specs/2026-11-26-phase5-sloop-go-alignment-design.md`（§6 的 5B 行、新增 5F 行、§5.1/§5.2 提及壳的位置）
- Modify: `docs/superpowers/specs/2026-11-26-phase5b-sound-mixer-device-design.md`（状态行）
- Modify: `docs/design/008-param-pages.md`（文末新增「Sound 仪表盘」一节，含 §5.6 预算与「实测登记」占位）

**Interfaces:**
- Produces: 文档口径 = 5B「页面先行」（Sound + Mixer + Device），新壳/主题/字体为 **5F**（排在 5E 之后，入口条件 = 旧页清零）；本计划开工的依据。

- [ ] **Step 1: 修订路线图与阶段 5 总设计**

路线图 §7 表：5B 行的目标改为「Sound 单轨仪表盘 + Mixer + Device 页；外壳/主题/字体移出」、状态 `🔶 进行中（5B-1，2026-11-26，feat/phase5b）`；在 5E 之后新增 5F 行「新壳 + 主题 + 字体（页面全部重建后一次性换壳；脚本只围绕最终导航模型改写一次）」。阶段 5 总设计 §6 同步：5B 标题与内容按新规格改写，新增 5F 小节（内容取 5B 规格 §5.1），§5.2 的「U2 新壳 + 逐页重建」一段加一句「壳在 5F 落地（原 5B 的壳部分后移，依据 5B 规格 §4.1）」。5B 规格状态行改为「**已接受**（2026-11-26，用户确认）」。两处 §9 变更记录各加一行。

- [ ] **Step 2: ADR-008 新节占位**

`docs/design/008-param-pages.md` 文末新增「## Sound 仪表盘（5B-1，进行中）」：三句话写设计（卡片成员纯数据表 `sound_layout.h`、每卡一个自绘对象、作为 `PARAM_FEAT_DASHBOARD` 挂在参数页框架上）+「预算：est 12,288（上限 12,768）」+ 一行「⏳ 实测登记：池 delta / build_ms / SEQ→SOUND 过渡 pool_free（Task 2）」。

- [ ] **Step 3: 核对并提交**

Run: `grep -n '5F' docs/design/002-port-roadmap.md docs/superpowers/specs/2026-11-26-phase5-sloop-go-alignment-design.md | wc -l`
Expected: ≥ 4。
```bash
git add docs/ && git commit   # docs(design): 阶段 5 改为页面先行并登记 Sound 仪表盘设计
git push
```

---

### Task 1: 卡片成员表与布局纯函数（host TDD）

**Files:**
- Create: `firmware/components/sloop_ui/src/sound_layout.h`、`firmware/components/sloop_ui/src/sound_layout.c`
- Create: `firmware/components/sloop_ui/test/sound_layout_test.c`
- Create: `scripts/sound-test.sh`
- Modify: `firmware/components/sloop_ui/CMakeLists.txt`（`SRCS` 加 `src/sound_layout.c`）

**Interfaces:**
- Produces（纯数据 + 纯函数，不含 LVGL）:

```c
#define SOUND_CARDS_MAX     13
#define SOUND_SLOT_TABLE_MAX 109    /* 表内成员总数（含仅 ANALOG 的 P_FTYPE） */
#define SOUND_SLOT_NONE     0xFFFFu
typedef struct { uint8_t scope; uint8_t id; uint8_t gate; uint8_t drum_ok; } sound_slot_t;
/* scope = sloop_desc_scope_t；gate：0 = 总是，1 = 仅 ANALOG 引擎；drum_ok 同 param_slot_t */
typedef struct { const char *title; uint8_t slot_count; const sound_slot_t *slots; } sound_card_t;
extern const sound_card_t k_sound_cards[SOUND_CARDS_MAX];

#define SOUND_COL_W 616
#define SOUND_COL_GAP 16
#define SOUND_CARD_PAD 12
#define SOUND_TITLE_H 36
#define SOUND_CELL_W 140
#define SOUND_CELL_H 132
#define SOUND_KNOB_D 96
#define SOUND_RING_MIN_R 30
#define SOUND_RING_MAX_R 52

typedef struct { uint8_t card; int16_t col, y, h, rows; } sound_card_pos_t;

uint16_t sound_layout_slot_index(uint8_t card, uint8_t slot);          /* 越界 = SOUND_SLOT_NONE */
bool    sound_layout_slot_locate(uint16_t flat, uint8_t *card, uint8_t *slot);
/* 两列 staggered 布局，**内容坐标**（原点 = 列 0 / y 0，不含页边距）：折叠卡片高 = SOUND_TITLE_H，卡片间距 8；
 * 返回内容总高。页面坐标 = 内容坐标 + (16, 视口顶) − 滚动量，由调用方换算。 */
int16_t sound_layout_place(uint16_t collapsed, sound_card_pos_t out[SOUND_CARDS_MAX]);
/* 槽在内容坐标里的矩形（x = col*(616+16) + 12 + slot%4*140；y = 卡片 y + 36 + slot/4*132）。卡片折叠/槽越界返回 false。 */
bool    sound_layout_slot_rect(const sound_card_pos_t *pos, uint8_t slot,
                               int16_t *x, int16_t *y, int16_t *w, int16_t *h);
/* 存在位图（SOUND_SLOT_TABLE_MAX 位）上移动焦点：delta 可为 ±1；跳过不存在的槽与折叠卡片，环绕；无存在槽返回 false。 */
bool    sound_layout_flat_move(const uint32_t present[4], uint16_t collapsed, int16_t from, int8_t delta,
                               uint16_t *out);
/* 从 from 起找第一个存在的槽（from 可为 SOUND_SLOT_NONE = 从头）。 */
bool    sound_layout_first_present(const uint32_t present[4], uint16_t *out);
uint16_t sound_layout_present_count(const uint32_t present[4]);
bool    sound_ring_hit(int32_t dx, int32_t dy);                         /* 30 ≤ r ≤ 52 */
```

- [ ] **Step 1: 写失败测试 `test/sound_layout_test.c`**

断言宏与 `knob_test.c` 同风格（`ASSERT`、每例 `PASS <name>`、末行 `SOUND LAYOUT TEST PASSED`）。用例：

```c
static void test_cards(void) {            /* 成员 = 5B 规格 §5.2 表（012 §3 的槽位映射） */
    ASSERT(SOUND_CARDS_MAX == 13);
    static const uint8_t want[SOUND_CARDS_MAX] = {4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7};
    uint16_t total = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        ASSERT(k_sound_cards[c].slot_count == want[c]);
        total = (uint16_t)(total + k_sound_cards[c].slot_count);
        for (uint8_t s = 0; s < k_sound_cards[c].slot_count; s++) {
            const sound_slot_t *x = &k_sound_cards[c].slots[s];
            if (x->scope == SLOOP_DESC_GLOBAL) ASSERT(x->id < SLOOP_CORE_G_COUNT);
            else ASSERT(x->id < SLOOP_CORE_P_COUNT);
        }
    }
    ASSERT(total == SOUND_SLOT_TABLE_MAX);          /* 109 */
    ASSERT(strcmp(k_sound_cards[0].title, "Global") == 0);
    ASSERT(strcmp(k_sound_cards[12].title, "Texture") == 0);
    /* 抽钉：Global 卡 = G_BPM G_SWING G_CLOCK G_TUNE；Master 卡第 4 槽 = G_ROLL、第 5 槽 = TFLT(TRACK)；
       Engine edit 卡第 9 槽 = P_FTYPE 且 gate=1；Drums 卡首槽与 MIDI 卡第 4 槽同为 G_DRCH */
    ASSERT(k_sound_cards[0].slots[0].scope == SLOOP_DESC_GLOBAL);
    ASSERT(k_sound_cards[1].slots[3].scope == SLOOP_DESC_GLOBAL && k_sound_cards[1].slots[4].scope == SLOOP_DESC_TRACK);
    ASSERT(k_sound_cards[4].slots[8].gate == 1);
    ASSERT(k_sound_cards[10].slots[0].id == k_sound_cards[11].slots[3].id);
    /* 鼓轨规则：仅 PATTERN(3..6)/SLICER/INSERT/TFLT/LEVEL/PAN 与 GLOBAL 槽 drum_ok */
    uint8_t ok = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++)
        for (uint8_t s = 0; s < k_sound_cards[c].slot_count; s++) {
            const sound_slot_t *x = &k_sound_cards[c].slots[s];
            if (x->scope != SLOOP_DESC_GLOBAL && x->drum_ok) ok++;
        }
    ASSERT(ok == 16);     /* 012 §3「鼓」列：PATTERN 4 + SLICER 4 + INSERT 4 + INSERT 2 1 + TFLT/LEVEL/PAN 3 */
}
```
```c
static void test_index(void) {
    uint8_t c, s;
    ASSERT(sound_layout_slot_index(0, 0) == 0);
    ASSERT(sound_layout_slot_index(0, 4) == SOUND_SLOT_NONE);
    ASSERT(sound_layout_slot_index(1, 0) == 4 && sound_layout_slot_index(4, 8) == 4 + 5 + 9 + 11 + 8);
    ASSERT(sound_layout_slot_locate(108, &c, &s) && c == 12 && s == 6);
    ASSERT(!sound_layout_slot_locate(109, &c, &s));
}
static void test_place(void) {
    sound_card_pos_t p[SOUND_CARDS_MAX];
    int16_t h = sound_layout_place(0, p);
    /* 行数 = ceil(n/4)：Global 1 行 ⇒ h = 36 + 132 + 12 = 180；FX 6 行 ⇒ 36 + 792 + 12 = 840 */
    ASSERT(p[0].rows == 1 && p[0].h == 180 && p[0].y == 0 && p[0].col == 0);
    ASSERT(p[6].rows == 6 && p[6].h == 840);
    /* staggered：第 2 张（Master 5 槽 = 2 行，h = 36 + 264 + 12 = 312）落在右列；每张卡 x = 16 + col*(616+16) */
    ASSERT(p[1].rows == 2 && p[1].h == 312 && p[1].col == 1 && p[1].y == 0);
    ASSERT(p[1].col != p[0].col);
    /* 折叠第 6 张：高 180，总高变短 */
    int16_t h2 = sound_layout_place(1u << 6, p);
    ASSERT(p[6].h == 180 && h2 < h);
    /* 每张卡不得越过另一列已放卡片：同列卡片 y 严格递增且不重叠 */
    for (uint8_t i = 0; i < SOUND_CARDS_MAX; i++)
        for (uint8_t j = 0; j < SOUND_CARDS_MAX; j++)
            if (i != j && p[i].col == p[j].col && p[j].y > p[i].y) ASSERT(p[j].y >= p[i].y + p[i].h + 8);
}
static void test_rect_and_move(void) {
    sound_card_pos_t p[SOUND_CARDS_MAX]; int16_t x, y, w, h;
    (void)sound_layout_place(0, p);
    ASSERT(sound_layout_slot_rect(&p[0], 0, &x, &y, &w, &h) && w == SOUND_CELL_W && h == SOUND_CELL_H);
    ASSERT(sound_layout_slot_rect(&p[0], 1, &x, &y, &w, &h) && x == p[0].col * (SOUND_COL_W + SOUND_COL_GAP) + SOUND_CARD_PAD + SOUND_CELL_W);
    ASSERT(!sound_layout_slot_rect(&p[0], 4, &x, &y, &w, &h));
    uint32_t present[4] = {0};    /* 全存在 */
    for (uint16_t i = 0; i < SOUND_SLOT_TABLE_MAX; i++) present[i >> 5] |= 1ul << (i & 31);
    uint16_t f;
    ASSERT(sound_layout_flat_move(present, 0, 0, 1, &f) && f == 1);
    ASSERT(sound_layout_flat_move(present, 0, 0, -1, &f) && f == 108);     /* 环绕 */
    present[0] &= ~(1ul << 1);                                              /* 槽 1 不存在 */
    ASSERT(sound_layout_flat_move(present, 0, 0, 1, &f) && f == 2);
    present[0] &= ~1ul; present[0] |= 1ul << 1;                             /* 槽 0 不存在：从 1 起向后环绕到 108 */
    ASSERT(sound_layout_flat_move(present, 0, 1, -1, &f) && f == 108);
    /* 折叠整张 Global 卡 ⇒ 焦点越过它 */
    ASSERT(sound_layout_flat_move(present, 1u << 0, 3, 1, &f) && f == 4);
    ASSERT(sound_layout_present_count(present) == 108);
    uint32_t none[4] = {0}; uint16_t g;
    ASSERT(!sound_layout_flat_move(none, 0, 0, 1, &g) && !sound_layout_first_present(none, &g));
    ASSERT(sound_layout_first_present(present, &g) && g == 1);
    ASSERT(sound_ring_hit(0, 30) && sound_ring_hit(36, 36) && !sound_ring_hit(0, 29) && !sound_ring_hit(0, 53) && !sound_ring_hit(0, 0));
}
```

- [ ] **Step 2: 写 `scripts/sound-test.sh` 并确认失败**

照 `scripts/knob-test.sh` 的结构（`CC` 覆盖、`-std=c11 -O2 -Wall -Wextra -I src -I include -I ../sloop_core/include -I test/stub`，产物 `firmware/build/sound-test/sound_layout_test`，成功打印 `SOUND LAYOUT TEST PASSED`）。编译 `test/sound_layout_test.c` + `src/sound_layout.c`（本任务不需要 `page_layout.c`/`knob_math.c`）。
Run: `scripts/sound-test.sh` ⇒ 退出码非 0（头文件不存在）。

- [ ] **Step 3: 实现 `sound_layout.{h,c}`**

成员照 5B 规格 §5.2 表（id 取上游 `third_party/sloop-fm1/firmware/src/core.h` 的 `P_*`/`G_*` 枚举序号，**用枚举名不用魔数**；`P_FTYPE`、`G_ROLL`、`G_FXFADE` 等若 `sloop_core.h` 未暴露则在上游头里取——`sound_layout.c` 与 `page_layout.c` 同样包含上游头的方式）。`gate` 仅 `P_FTYPE` 为 1。`drum_ok` 按 012 §3「鼓」列逐槽。`sound_layout_place`：列宽 616、间距 16，卡片依次放入「当前累计高度较小」的列（相等放左列），卡片间距 8，内容总高 = 两列较大者。全部无 LVGL 依赖。

- [ ] **Step 4: 测试通过并构建**

Run: `scripts/sound-test.sh && scripts/knob-test.sh && scripts/build.sh`
Expected: `SOUND LAYOUT TEST PASSED`；`KNOB TEST PASSED`；构建退出码 0、0 新增警告。

- [ ] **Step 5: 提交**

```bash
git add firmware/components/sloop_ui/src/sound_layout.* firmware/components/sloop_ui/test/sound_layout_test.c firmware/components/sloop_ui/CMakeLists.txt scripts/sound-test.sh
git commit   # feat(components/sloop_ui): 新增 Sound 仪表盘的卡片成员表与布局纯函数
git push
```

---

### Task 2: 页面骨架与真机原型测量（5B-0）

**Files:**
- Create: `firmware/components/sloop_ui/src/page_sound.h`、`firmware/components/sloop_ui/src/page_sound.c`
- Modify: `firmware/components/sloop_ui/src/page_layout.h`（`PARAM_FEAT_DASHBOARD 0x08u`；`page_layout_is_dashboard()`）、`src/page_layout.c`（SOUND 条目 `features |= PARAM_FEAT_DASHBOARD`、`est_bytes = 12288`）
- Modify: `firmware/components/sloop_ui/src/page_params.c`（build/update/tap/forget 的仪表盘分支）、`src/page_params.h`（分支说明）
- Modify: `firmware/components/sloop_ui/CMakeLists.txt`（`SRCS` 加 `src/page_sound.c`）
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`（新增 TAP 种类 `SLOOP_TAP_SOUND_FOCUS`、`SLOOP_TAP_SOUND_CARD`；`theme.h` 末尾的页内原型区加 `page_sound_*` 声明）
- Modify: `firmware/main/console.c`（TAP 回显两个新分支）
- Modify: `docs/design/008-param-pages.md`（实测登记）

**Interfaces:**
- Consumes: Task 1 的 `sound_layout_*`。
- Produces（`page_sound.h`，页内坐标 = 页面区相对坐标；`page_params.c` 在 LVGL 锁内调用）:

```c
lv_obj_t *page_sound_build(lv_obj_t *parent);        /* 主体：滚动容器 + 13 张卡片；NULL = 分配失败 */
void page_sound_update(const app_state_t *st);       /* 每拍 */
void page_sound_forget(void);                        /* 页面销毁：清控件指针 */
bool page_sound_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
```
  `page_params.c` 的分支：`page_layout_is_dashboard(pg)` 为真时——build 不建 8 个旋钮、不建 chip 行，改调 `page_sound_build(parent)`，页眉行/银行行/`−/+` 按钮照建（按钮 y 在仪表盘页为 40）；update 先跑页眉/银行行/按钮的既有逻辑，再调 `page_sound_update(st)`，跳过槽循环与 `sloop_ui_zoom_knob`；tap 先走既有命中（按钮/页眉行/银行行），未命中再调 `page_sound_tap`；forget 追加 `page_sound_forget()`；`page_params_test_knob`（`UI KNOB`）在仪表盘页返回 `ESP_ERR_NOT_SUPPORTED`。

- [ ] **Step 1: 特征位与框架分支**

按 Interfaces 落 `PARAM_FEAT_DASHBOARD`、`page_layout_is_dashboard()`、SOUND 条目（`est_bytes = 12288`）与 `page_params.c` 四处分支。此时 `page_sound_*` 只有空实现（build 返回一个 `make_plain(parent)`）。

- [ ] **Step 2: 实现骨架 `page_sound.c`**

- 静态状态（**全部 `EXT_RAM_BSS_ATTR`**）：`sound_card_pos_t s_pos[13]`、`uint16_t s_collapsed`、`int16_t s_scroll`、卡片对象指针 `lv_obj_t *s_cards[13]`、`lv_obj_t *s_scroll_obj`、`int32_t s_gesture_card`（当前手势的卡片，−1 = 滚动/无）、`int8_t s_gesture_slot`、`int32_t s_gesture_last_y`、`bool s_gesture_moved`。
- `page_sound_build`：滚动容器 `make_plain(parent)`，pos `(16, 80)`，size `(1248, 564)`，`lv_obj_set_scrollable(obj, false)`，`lv_obj_set_clip_corner`/默认裁剪（子对象超出视口不画）；13 张卡片 `make_plain(container)`，pos/size 取 `sound_layout_place(0, s_pos)`（容器内 = 内容坐标），`lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE)`、`lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE)`，事件回调 `card_event_cb`（`LV_EVENT_ALL`）。
- 绘制（`LV_EVENT_DRAW_MAIN`）：卡片底 = 圆角矩形 `UI_PANEL`（半径 10）；标题 16pt `UI_TEXT` 起于 `(12, 8)`，右端 `▾`；每个槽：圆心 `(槽矩形中心 x, 矩形顶 + 12 + 48)`，底弧 `UI_PANEL_HI` 宽 8、扫角 135°→405°，名称 14pt 在弧上方，值占位 `"--"` 16pt 在弧内。写法照 `knob.c` 的 `knob_draw`（`lv_draw_rect`/`lv_draw_arc`/`lv_draw_label`，文本用静态缓冲、`text_local = 0`）。
- 手势（`card_event_cb`）：`LV_EVENT_PRESSED` → 记 `s_gesture_card`、按下点、`s_gesture_moved = false`；若按下点命中某槽环带（`sound_ring_hit` 对槽圆心）⇒ `s_gesture_slot = 该槽`，否则 `−1`。`LV_EVENT_PRESSING` → `lv_indev_get_point`；位移绝对值 > 4 ⇒ `s_gesture_moved = true`；`s_gesture_slot < 0` 时 `lv_obj_scroll_by_bounded(s_scroll_obj, 0, last_y − y, LV_ANIM_OFF)`；`s_scroll = lv_obj_get_scroll_y(s_scroll_obj)`。`LV_EVENT_RELEASED`/`LV_EVENT_PRESS_LOST` → 清手势。`LV_EVENT_CLICKED`/`SHORT_CLICKED`/`LONG_PRESSED*` → 若 `s_gesture_moved` 则忽略（`lv_indev_wait_release` 不需要）。
- 坐标换算（全文件唯一一处）：内容点 = `(x − 16, y − 80 + s_scroll)`（页面区相对坐标进来）；绘制/TAP/槽矩形都用它。
- TAP（本任务只判命中）：`page_sound_tap` 把 `(x,y)` 换到内容坐标，逐卡 `rect_hit`；命中的卡片标题行 ⇒ 填 `SLOOP_TAP_SOUND_CARD{a=card}` 返回 true；命中某槽 ⇒ `SLOOP_TAP_SOUND_FOCUS{a=card, b=slot, v=扁平下标}` 返回 true；否则 false。

- [ ] **Step 3: 构建与烧录**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh`
Expected: 第二次构建 0 警告；`Hash of data verified` ×3。

- [ ] **Step 4: 真机测量（不需要操作员）**

用 pyserial（写法照 `scripts/selftest-heap.py`）：
- `UI sound` ⇒ `UI page=sound (6)`；`UI MEM` ⇒ `SOUND … est=12288 delta=<实测> build_ms=<实测>`；记录 `pool_used/pool_free`。
- 内部堆：进入页面前后 `ENG MEM` 的 `internal_free` 差 ≤ 256B（静态数组在 PSRAM）；`internal_min ≥ 61,440`。
- SEQ → Sound 过渡：`UI step` → `UI transport` → `UI sound`，读 `UI MEM` 的 `pool_free` 与构建日志行 `池剩余` ⇒ ≥ 4,096。
- 滚动：`TAP` 在卡片空白处无法拖动——本任务只验证「`UI step` ↔ `UI sound` 往返 5 次后 `UI MEM page id=6 … delta` 不变、`pool_free` 往返差 ≤ 128B」。
- `python3 scripts/screenshot.py /dev/ttyACM0` ⇒ 截屏成功，存 `firmware/build/verify/proto-sound-<时间戳>.png`。
把上述读数写进报告；证据复制到 `docs/dev/verify/5-pb1-2026-11-26/task-2-proto-*`。
Run: `python3 scripts/selftest-heap.py /dev/ttyACM0` ⇒ 退出码 0。

- [ ] **Step 5: 预算裁决与文档**

若实测 `delta > 12288`：执行第一退路（银行行 7 个对象并为 1 个自绘对象）后重测；仍超则第二退路（细调按钮并入提示行）；每一步的读数都写进报告。把最终读数（delta、build_ms、SEQ→SOUND 过渡 pool_free）回填 ADR-008 的「实测登记」行。

- [ ] **Step 6: 提交**

```bash
git add firmware/ && git commit   # feat(components/sloop_ui): Sound 页改为卡片仪表盘骨架并挂到参数页框架
git add docs/ && git commit       # docs(design): 登记 Sound 仪表盘原型的池与构建读数
git push
```

---

### Task 3: 槽的值、描述符、锁定与拖动改值

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_sound.c`、`src/page_sound.h`
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`（console 入口原型）、`firmware/main/console.c`（`UI SOUND CARD/SLOT`、`UI SOUND KNOB`、TAP 回显补全）
- Modify: `docs/dev/reference/console-and-injection.md`（`UI` 行与 TAP 回显）

**Interfaces:**
- Consumes: `sloop_core_desc_track_live(uint8_t track, uint32_t id, sloop_desc_t *out)`、`sloop_core_desc(scope, engine, id, out)`、`knob_format_value`、`knob_angle_delta`、`knob_drag_value`、`lv_atan2`、`state_apply`、`state_selected_track()`、`state_read_p/g()`。
- Produces（`sloop_ui.h` 公共入口；标「持锁」的函数内部取 `bsp_display_lock(1000)`，写法照 `sloop_ui_param_chip_set`）:

```c
bool sloop_ui_sound_dashboard_active(void);                       /* 无锁 */
uint16_t sloop_ui_sound_slot_total(void);                         /* 无锁：当前上下文存在的槽数 */
esp_err_t sloop_ui_sound_slot_info(uint16_t flat, uint8_t *card, uint8_t *slot, uint8_t *scope,
                                   uint8_t *id, const char **label, bool *locked, int16_t *value); /* 无锁 */
esp_err_t sloop_ui_sound_slot_rect(uint16_t flat, int16_t *x, int16_t *y, int16_t *w, int16_t *h); /* 持锁；屏幕绝对坐标 = (16 + 内容x, 72 + 80 + 内容y − 滚动量) */
esp_err_t sloop_ui_sound_scroll_get(int16_t *y, int16_t *max_y); /* 持锁 */
esp_err_t sloop_ui_sound_scroll_set(int16_t y);                   /* 持锁；钳位 [0, max] */
esp_err_t sloop_ui_sound_test_knob(uint16_t flat, int32_t dx, int32_t dy, bool press, int16_t *value); /* 持锁 */
```
  返回码：非仪表盘页或页未建 `ESP_ERR_INVALID_STATE`；槽不存在/越界 `ESP_ERR_NOT_FOUND`；环带外 `ESP_ERR_NOT_FOUND`；锁定槽 `ESP_ERR_NOT_ALLOWED`。

- [ ] **Step 1: 存在位图、描述符与值**

`page_sound.c` 增加（PSRAM）：`uint32_t s_present[4]`、`struct { sloop_desc_t desc; int16_t value; bool shown, locked; } s_slot[SOUND_SLOT_TABLE_MAX]`、脏检查键 `s_ctx_key`（选中轨、引擎、`P_ITYPE`、`P_ESYNC`、`st->core.trk[sel].engine`）。`page_sound_update`：上下文键变化 ⇒ 重解析：对每个表成员，`gate` 不满足（非 ANALOG）或描述符取不到或 `label` 为空或（`min == max && enum_count == 0`）⇒ 不存在；TRACK 作用域用 `sloop_core_desc_track_live(sel, id)`，GLOBAL 用 `sloop_core_desc(SLOOP_DESC_GLOBAL, 0, id)`；`locked = drum && scope != GLOBAL && drum_ok == 0`；每次拍：存在的槽取值（`st->p[id]` / `st->g[id]`），值/锁定变化才 `lv_obj_invalidate_area(卡片, 槽矩形)：卡片是容器的子对象，矩形用卡片内坐标 = 槽内容矩形 − 卡片内容原点；滚动由容器 pos 体现，不进矩形`。绘制：存在且未锁 ⇒ 弧 + 名称 + `knob_format_value` 的值；锁定 ⇒ 无弧、`--`。

- [ ] **Step 2: 拖动改值**

把手势里的槽分支接上：`PRESSED` 命中环带且槽存在未锁 ⇒ 记锚点角度 `lv_atan2(dy, dx)` 与锚点值；`PRESSING` ⇒ `knob_angle_delta` → `knob_drag_value(&desc, anchor, delta)`，值变化即写：TRACK/ENGINE 作用域 `{CMD_SET_PARAM, a=state_selected_track(), b=id, v}`，GLOBAL `{CMD_SET_GLOBAL, a=id, v}`；环带外的移动事件忽略但**不结束拖动**。写入函数 `sound_slot_write(uint16_t flat, int16_t v, int16_t *applied)` 与 TAP/键盘/注入共用（写后经 `slot_value_now` 同款窄读回填）。

- [ ] **Step 3: console 与注入入口**

按 Interfaces 落公共入口；`console.c` 新增子命令（`UI` 后第二个词是 `SOUND` 时进入，写法照 `UI <参数页名> CHIP`）：
- `UI SOUND CARD <n> SLOT <m>` ⇒ `UI SOUND SLOT card=%u slot=%u flat=%u present=%u scope=%u id=%u label="%s" locked=%u value=%d x=%d y=%d w=%d h=%d`（不存在的槽：`present=0`，其后字段为 `-`/0）；`UI ERR sound (invalid|not_built|not_dashboard|absent)`。
- `UI SOUND SCROLL [<y>]` ⇒ `UI SOUND SCROLL y=%d max=%d`。
- `UI SOUND STATS` ⇒ `UI SOUND STATS cards=%u slots=%u scroll_max=%d collapsed=0x%04X`（slots = 当前上下文存在槽数）。
- `UI SOUND KNOB <flat> <press|drag> <dx> <dy>` ⇒ `UI SOUND KNOB flat=%u %s dx=%ld dy=%ld value=%d`；`… ignored (ring band)`；`… ignored (locked)`。实现：换算到槽圆心 + dx/dy 的点，调用与 `card_event_cb` **同一**的 `sound_handle_pressed/pressing(flat, point)`（`knob_test_press` 的做法）。
TAP 回显：`SLOOP_TAP_SOUND_FOCUS` ⇒ `-> SOUND focus card=%u slot=%u flat=%u CMD_SET_PARAM`（锁定槽加 ` locked=1`）；`SLOOP_TAP_SOUND_CARD` ⇒ `-> SOUND card=%u collapsed=%u`。TAP 命中槽只设焦点（本任务先把焦点记为 `s_focus_flat`，键盘在 Task 4 接）。

- [ ] **Step 4: 构建、烧录；设备核对（不需要操作员）**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh`
- 选轨 0、引擎 ANALOG：`UI SOUND CARD 4 SLOT 8` ⇒ `present=1 … label="TYPE"`；`UI SOUND CARD 1 SLOT 3` ⇒ `label="ROLL"`；`UI sound` 后 `UI MEM` 的 `delta ≤ 12288`。
- 逐卡首槽标签 = 描述符（13 条 `UI SOUND CARD n SLOT 0`，把 label 与 `ENG DESC`/现有 `UI sample CHIP` 的口径对不上时如实记录）。
- 拖动：取 Voice 卡的 LEVEL 槽（扁平下标由 `UI SOUND CARD 5 SLOT 0` 读出）：`UI SOUND KNOB <f> press 0 -40`（环带上）⇒ `value` 不变；`UI SOUND KNOB <f> drag 40 0` ⇒ 值变化；`ENG SNAP` 的 `trk k=0 … level=` 与之相符；`UI SOUND KNOB <f> drag -40 0` ⇒ 回到原值；`UI SOUND KNOB <f> press 0 0` ⇒ `ignored (ring band)`。
- 锁定：`KEY` 注入 Ctrl+4 选鼓轨（`KEY 0x40 down` → `KEY 0x04 down` / `KEY 0x04 up` → `KEY 0x40 up`，写法照 `inject.py` 的 `key_with_layer`）后 `UI SOUND CARD 4 SLOT 0` ⇒ `locked=1`，`UI SOUND KNOB … drag 40 0` ⇒ `ignored (locked)`，`ENG SNAP` 不变；`UI SOUND CARD 9 SLOT 0`（PATTERN）⇒ `locked=0`。
- 冻结：`ENG FREEZE ON` ⇒ 拖动后值不变；`ENG FREEZE OFF` 恢复。
- 滚动后 `TAP` 命中：`UI SOUND SCROLL 400` ⇒ `y=400`；`TAP` 落在此时屏幕上的某槽 ⇒ `-> SOUND focus …`；拖动手势（`UI SOUND KNOB … press/drag`）后卡片折叠状态不变（`s_collapsed` 仍 0）。
- `ENG MEM`：`internal_min ≥ 61,440`；`python3 scripts/selftest-heap.py /dev/ttyACM0` ⇒ 退出码 0；截屏一张存证。

- [ ] **Step 5: 提交**

```bash
git add firmware/ && git commit   # feat(components/sloop_ui): Sound 仪表盘按描述符取值并支持拖动改值与注入
git add docs/ && git commit       # docs(dev): 登记 UI SOUND 子命令与 TAP 回显
git push
```

---

### Task 4: 键盘焦点、细调按钮、选轨条、折叠与读数行

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_sound.c`、`src/page_sound.h`、`src/page_params.c`（`sloop_ui_param_focus_move/adjust`/`param_step`/`apply_focus` 的仪表盘分流；build 时把 `−/+` 按钮放到 y 40）
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`、`firmware/main/console.c`（`UI SOUND FOCUS`、`UI SOUND CARDTOGGLE`）
- Modify: `docs/dev/reference/console-and-injection.md`

**Interfaces:**
- Produces（`page_sound.h`，无 LVGL 锁）:

```c
void page_sound_focus_move(int8_t delta);     /* ←/→ */
bool page_sound_adjust(int8_t delta);         /* ↑/↓/ENTER：焦点槽 ±1；返回是否发了命令 */
uint16_t page_sound_focus_get(void);
void page_sound_focus_set(uint16_t flat);     /* 只写焦点；滚动在下一拍 */
void page_sound_card_toggle(uint8_t card);    /* 无锁：翻折叠位；布局在下一拍重排 */
bool page_sound_card_collapsed(uint8_t card);
```
  公共入口：`esp_err_t sloop_ui_sound_focus_get(uint16_t *flat, uint8_t *card, uint8_t *slot)`（无锁）、`esp_err_t sloop_ui_sound_focus_set(uint16_t flat)`（持锁：写焦点并立即滚到可见）、`esp_err_t sloop_ui_sound_card_toggle(uint8_t card)`（持锁）。

- [ ] **Step 1: 焦点与键盘**

PSRAM 静态 `uint16_t s_focus_flat`（`portMUX` 保护，写法照 `page_params.c` 的 `focus_get/set`）。`page_sound_focus_move` ⇒ `sound_layout_flat_move(s_present, s_collapsed, s_focus_flat, delta, &next)`。`page_sound_adjust` ⇒ 焦点槽存在且未锁 ⇒ `sound_slot_write(值 ± 1)`。`page_params.c`：`sloop_ui_param_focus_move/adjust` 在当前页是仪表盘时转调上面两个函数；`param_step`（`−/+` 按钮与 `TAP` 的 `param_tap_button`）同样转调 `page_sound_adjust`；`apply_focus` 不再调用 `sloop_ui_zoom_knob`。`page_sound_update` 每拍：焦点变化 ⇒ 只失效旧/新两个槽的矩形；并保证焦点槽可见（矩形超出视口 ⇒ `lv_obj_scroll_by_bounded` 到 `y − 80` 或 `y + h − 644`）。

- [ ] **Step 2: 选轨条、提示/读数行、折叠**

- 选轨条：一个自绘对象 `(960,4) 304×32`，4 格各 76px，格内 `1..4`（16pt，当前轨轨色、其余 `UI_TEXT_DIM`，当前格底 `UI_PANEL_SEL`）；点击 ⇒ `{CMD_SELECT_TRACK, a=k}`；TAP 命中 ⇒ 复用 `SLOOP_TAP_TRACK_CARD{a=k}`（回显不变）。合成轨/鼓轨都显示。
- 提示/读数行：一个自绘对象 `(16,40) 1072×32`：ZOOM 关 ⇒ 14pt `UI_TEXT_DIM` 提示文本；ZOOM 开（`st->settings.zoom`）⇒ 28pt `UI_TEXT` 显示 `标签 值`（焦点槽）。
- `−/+` 按钮：仪表盘页建在 `(1096,40)`/`(1184,40)`（`BTN_Y` 在该页为 40），样式与回调沿用。
- 折叠：标题行点击（且手势未移动）⇒ `page_sound_card_toggle`；下一拍 `sound_layout_place(80, s_collapsed, s_pos)` 重排卡片 pos/size，`s_scroll` 重新钳位（`lv_obj_scroll_by_bounded` 到范围内），焦点槽滚到可见。

- [ ] **Step 3: console**

`UI SOUND FOCUS [<n>]` ⇒ `UI SOUND FOCUS flat=%u card=%u slot=%u label="%s"`（不存在的 n ⇒ `UI ERR sound (absent)`）。`UI SOUND CARDTOGGLE <n>` ⇒ `UI SOUND CARDTOGGLE card=%u collapsed=%u`。

- [ ] **Step 4: 构建、烧录；设备核对（不需要操作员）**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh`
- `UI sound`；`UI SOUND FOCUS` ⇒ 首个存在槽（`flat=0 card=0 slot=0 label="BPM"`）；`KEY 0x4C down`/`KEY 0x4C up`（→）⇒ `flat=1`；`KEY 0x4A`（←）⇒ 回 0；再按一次 ← ⇒ 环绕到 `flat=108`（SAMPLE 引擎时为最后一个存在槽）。
- `KEY 0x4C` ×4 后 `UI SOUND SCROLL` ⇒ `y` > 0（焦点滚入可见）；`UI SOUND SCROLL 0` 后把焦点设到末槽（`UI SOUND FOCUS 108`）⇒ `y` 自动回到使该槽可见的位置。
- 调值：`UI SOUND FOCUS` 读 Level 槽的 flat，`KEY 0x3B down/up`（↑）⇒ `ENG SNAP` 的 `trk k=0 … level` +1；`KEY 0x3B` 再按两次后 `KEY 0x4B`（↓）三次 ⇒ 回到原值 −1；`TAP` 细调 `+`（屏幕 `(1224, 72+40+20)`）⇒ 回到原值；`TAP` 细调 `−` ⇒ 原值 −1；还原。
- 选轨条：`TAP 998 92`（第 1 格中心）⇒ `-> TRACKS select track=0 …`；`TAP 1150 92`（第 3 格）⇒ `track=2`，`STATE SNAP selected_track=2`，`UI` 仍是 `page=sound`；还原到轨 0。
- 折叠：`UI SOUND CARDTOGGLE 6` ⇒ `collapsed=1`；`UI SOUND SCROLL 99999` ⇒ `y=max`（变小）；`UI SOUND FOCUS 60` ⇒ `y` 使该槽可见；`UI SOUND CARDTOGGLE 6` ⇒ 还原。
- 消失槽：`UI SOUND FOCUS` 设到 `P_FTYPE`（`UI SOUND CARD 4 SLOT 8` 读出的 flat），然后点页眉 ENGINE 行的 `+` 区（`TAP 283 92`）一次换到非 ANALOG 引擎 ⇒ `UI SOUND FOCUS` 返回存在的槽（`card=4 slot≤7`），不崩溃；点 `−` 区（`TAP 37 92`）换回 ANALOG。
- ZOOM：`UI system` 里把 ZOOM 打开（`TAP`/`KEY` 走 SYSTEM 页行）后回 `UI sound` ⇒ 读数行显示焦点槽（截屏存证）；关闭。
- `ENG MEM internal_min ≥ 61,440`；`selftest-heap.py` 退出码 0。

- [ ] **Step 5: 提交**

```bash
git add firmware/ && git commit   # feat(components/sloop_ui): Sound 仪表盘接入键盘焦点、细调按钮、选轨条与折叠
git add docs/ && git commit       # docs(dev): 登记 UI SOUND FOCUS/CARDTOGGLE
git push
```

---

### Task 5: 引擎/预置/kit 名的只读 core API（host TDD）

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_core.h`、`firmware/components/sloop_core/src/desc_tab5.h`（`sloop_core_sound_read` 旁）
- Modify: `scripts/synth-host-test.c`（新增用例）、`scripts/synth-test.sh`（若需）

**Interfaces:**
- Produces:

```c
uint8_t sloop_core_nengines(void);                                   /* = SLOOP_CORE_NENGINES */
bool sloop_core_engine_name(uint32_t engine, char *buf, uint32_t n); /* 越界 false；buf 以 0 结尾 */
uint8_t sloop_core_npresets(uint32_t engine);                        /* 越界 0 */
bool sloop_core_preset_name(uint32_t engine, uint32_t preset, char *buf, uint32_t n);
uint8_t sloop_core_nkits(void);                                      /* = DRUM_KIT_DESC 的枚举数 */
bool sloop_core_kit_name(uint32_t kit, char *buf, uint32_t n);
```
  全部只读、不取引擎锁（静态表）；实现只读 `ENGINES[]`/`presets[]`/`DRUM_KIT_DESC.names`，不建镜像表；名字拷贝复用 `sound_copy_name`。

- [ ] **Step 1: 写失败测试（`scripts/synth-host-test.c`）**

```c
static void test_sound_names(void) {
    char b[16];
    ASSERT(sloop_core_nengines() == 12);
    ASSERT(sloop_core_engine_name(0, b, sizeof b) && strcmp(b, "ANALOG") == 0);
    ASSERT(!sloop_core_engine_name(12, b, sizeof b));
    uint32_t total = 0;
    for (uint32_t e = 0; e < 12; e++) {
        uint8_t n = sloop_core_npresets(e);
        for (uint32_t p = 0; p < n; p++) {
            ASSERT(sloop_core_preset_name(e, p, b, sizeof b) && b[0] != '\0');
            total++;
        }
        ASSERT(!sloop_core_preset_name(e, n, b, sizeof b));
    }
    ASSERT(total > 0);
    ASSERT(sloop_core_nkits() > 0 && sloop_core_kit_name(0, b, sizeof b) && b[0] != '\0');
    ASSERT(!sloop_core_kit_name(sloop_core_nkits(), b, sizeof b));
}
```
  调用点加进该文件的用例表；Run: `scripts/synth-test.sh` ⇒ 退出码非 0（函数未定义）。

- [ ] **Step 2: 实现并测试通过**

Run: `scripts/synth-test.sh && scripts/host-test.sh && scripts/build.sh`
Expected: `SYNTH HOST TEST PASSED`；`ALL HOST TESTS PASSED`；构建退出码 0、0 新增警告。

- [ ] **Step 3: 提交**

```bash
git add firmware/components/sloop_core scripts/synth-host-test.c && git commit   # feat(components/sloop_core): 提供引擎、预置与鼓 kit 名的只读查询
git push
```

---

### Task 6: 选择器（引擎 chips + 预置名列表）

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_sound.c`、`src/page_sound.h`
- Modify: `firmware/components/sloop_ui/src/page_params.c`（`sound_row_event_cb`/`page_params_tap` 的页眉行：命中**中段名字区**（x 在两个 ± 区之间）⇒ `page_sound_picker_open(row)`，TAP 回显 `SLOOP_TAP_SOUND_PICK_OPEN{a=row}`；行的几何不改——与 Sloop Go 一致（点名字打开选择器）；提示行文案注明「点页眉名字选择引擎/预置」）
- Modify: `firmware/components/sloop_ui/src/input.c`（选择器打开时的方向键/ENTER/ESC 分流）、`include/sloop_ui.h`（TAP 种类 `SLOOP_TAP_SOUND_PICK_OPEN`、`SLOOP_TAP_SOUND_PICK`；原型）、`firmware/main/console.c`（`UI SOUND PICK`、回显）
- Modify: `docs/dev/reference/console-and-injection.md`、`docs/deferred-issues.md`（**删除 DI-54**）

**Interfaces:**
- Consumes: Task 5 的名字 API；`CMD_SET_ENGINE{a=track, v=engine}`、`CMD_SET_PRESET{a=track, v=preset}`、鼓轨 kit `CMD_SET_PARAM{a=3, b=SLOOP_CORE_P_COUNT−8, v=kit}`（与 `sound_step` 同）。
- Produces（`page_sound.h`）:

```c
void page_sound_picker_open(uint8_t row);     /* 0 = 引擎（鼓轨 = kit），1 = 预置 */
void page_sound_picker_close(void);
bool page_sound_picker_active(void);
void page_sound_picker_move(int8_t dx, int8_t dy);   /* dx = 引擎 ±1，dy = 列表高亮 ±1 */
bool page_sound_picker_confirm(void);                 /* 发命令并关闭；返回是否发了命令 */
/* 持锁：列表第 n 行的屏幕矩形（含 72）；n 越界/未打开 = ESP_ERR_INVALID_STATE / NOT_FOUND */
esp_err_t sloop_ui_sound_pick_rect(uint8_t n, int16_t *x, int16_t *y, int16_t *w, int16_t *h);
esp_err_t sloop_ui_sound_pick_info(uint8_t n, char *buf, uint32_t buf_n, bool *current);   /* 持锁；第 n 行文字与是否当前项 */
```

- [ ] **Step 1: 面板与绘制**

打开时在页面区之上建一个覆盖对象（`s_root` 的子对象，pos `(160, 72+120)`，size `(960, 440)`，`UI_PANEL`，圆角 12；按需创建、关闭即 `lv_obj_delete`，`page_sound_forget` 也删）：顶部一行引擎 chips（单对象自绘行，高 40：12 个 chip 按名字宽 11×字符 + 24，当前引擎 `UI_ACCENT` 底；鼓轨时这一行是 kit chips，横向放不下时只显示前 N 个并在 TAP/键盘上按序环绕）；下方列表为一个自绘对象，行高 32，一屏 11 行，可滚动（高亮行自动滚入）；FM6 引擎（`npresets == 0`）列表区显示一行提示 `FM6: USE BANK ROW`。池开销临时 ≤ 4KB（`UI MEM` 打开前后 `pool_free` 差记录在报告里）。

- [ ] **Step 2: 交互**

触摸/TAP：chip 行第 i 个 ⇒ 高亮引擎 i（鼓轨：选 kit 并关闭）；列表第 n 行 ⇒ 选该预置并关闭。键盘：选择器打开时 `page_sound_focus_move` → `page_sound_picker_move(±1, 0)`、`page_sound_adjust` → 高亮 ±1、ENTER → `page_sound_picker_confirm`、ESC（`ACT_PAGE_PLAY`）→ `page_sound_picker_close`（`input.c`：该 case 先判 `page_sound_picker_active()`）。上下文变化（选中轨/引擎/`npresets` 变）⇒ `page_sound_update` 关闭选择器。

- [ ] **Step 3: console 与 TAP 回显**

`UI SOUND PICK [<n>]`：无 n ⇒ `UI SOUND PICK open=%u row=%u engine=%u highlight=%u rows=%u`；有 n ⇒ `UI SOUND PICK n=%u text="%s" current=%u x=%d y=%d w=%d h=%d`。TAP：页眉行名字区 ⇒ `-> SOUND pick open row=%u`；chip ⇒ `-> SOUND pick engine=%u CMD_SET_ENGINE`（鼓轨 `kit=%u CMD_SET_PARAM`）；列表行 ⇒ `-> SOUND pick preset=%u CMD_SET_PRESET`。

- [ ] **Step 4: 构建、烧录；设备核对（不需要操作员）**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh`
- 轨 0：`TAP 160 92`（ENGINE 行名字区中心，y = 72+4+16）⇒ `-> SOUND pick open row=0`；`UI SOUND PICK` ⇒ `open=1 row=0 engine=0 rows=12`；`UI SOUND PICK 0` ⇒ `text="ANALOG" current=1`；`UI SOUND PICK 8` ⇒ `"FM6"`；`TAP` 其矩形中心 ⇒ `-> SOUND pick engine=8 CMD_SET_ENGINE`，`STATE SNAP`/`ENG SNAP` 显示轨 0 引擎 8，`UI SOUND FOCUS` 返回存在的槽。
- FM6 下再开 row0 ⇒ 关闭；开 row1（`TAP 512 92`）⇒ `rows=0`（提示）；换回 ANALOG。
- 预置：`TAP 512 92` ⇒ `open=1 row=1`；`UI SOUND PICK 0` ⇒ `current=1`；`UI SOUND PICK 3` ⇒ 用其矩形 `TAP` ⇒ `-> SOUND pick preset=3 CMD_SET_PRESET`，`ENG SNAP` 的 preset=3，页眉 PRESET 名同步（`sloop_ui_bank_status_text` 无关；用 `STATE SNAP`/`ENG SNAP`）。
- 键盘：打开 row1 后 `KEY 0x4B`（↓）×2、`KEY 0x3D`（ENTER）⇒ 选到第 2 个预置；打开后 `KEY 0x00`（ESC）⇒ 关闭且**不**切到 PLAY 页（`UI` 仍是 `page=sound`）。
- 上下文变化关闭：打开选择器后 `KEY` 注入 Ctrl+2 选轨 2（`KEY 0x40 down` → `KEY 0x02 down`/`up` → `KEY 0x40 up`）⇒ `UI SOUND PICK` ⇒ `open=0`。
- 鼓轨：Ctrl+4 选鼓轨，`TAP 160 92` ⇒ row0 为 kit chips；`UI SOUND PICK 0` ⇒ 第一个 kit 名；选第 2 个 ⇒ `kit=1 CMD_SET_PARAM`，`ENG SNAP trk k=3` 的 P_E0=1。
- 池：打开前后 `UI MEM pool_free` 差 ≤ 4,096；关闭后差回到 0 ±128。`ENG MEM internal_min ≥ 61,440`；`selftest-heap.py` 退出码 0；三张截屏（引擎面板/预置面板/鼓 kit 面板）存证。

- [ ] **Step 5: 文档与提交**

`docs/deferred-issues.md` 删除 DI-54（按名浏览全部工厂预置已落地）。`console-and-injection.md` 补页眉名字区的 TAP 语义与 `UI SOUND PICK`。
```bash
git add firmware/ && git commit   # feat(components/sloop_ui): Sound 页眉加入引擎与预置选择器
git add docs/ && git commit       # docs(dev): 登记选择器命令并关闭 DI-54
git push
```

---

### Task 7: 退役五张参数页并同步导航与页表

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_layout.c`（`PARAM_PAGES(X)` 只留 SOUND 与 SAMPLE；`PARAM_PAGE_COUNT` 7→2）、`src/page_layout.h`（注释）、`src/ui_nav.h`（`X(EDIT, APP_PAGE_ANALOG)`、`X(MIX, APP_PAGE_TOOLS)`）
- Modify: `firmware/components/sloop_ui/test/knob_test.c`（页表完整性用例按 2 页改；被删页的槽表断言移除——其内容已由 `sound_layout_test.c` 钉住）
- Modify: `scripts/inject/inject.py`（`NAV_ORDER` 13 页、`NAV_GROUP_PAGES`、`PARAM_EDIT_SUM_EST`/`PARAM_MIX_SUM_EST`、`registered=13`、`run_nav` 的 EDIT/MIX 子签期望、`run_params` 去掉退役页段、`BANK_ROW_Y = 72 + 76`、`soak` 相关 `POOL_RT_REP` 的 `mix` 项）、`scripts/soak.py`（若 `POOL_RT_REP` 在此）、`scripts/screenshot_matrix.py`（条目表去掉 mod/scale/voice/global/fx/sound 旧条目，加 `sound` 条目占位 ×2——完整条目在 Task 8 定）、`scripts/soak-selftest.py`（若其对组成员有钉死）
- Modify: `docs/design/005-ui-architecture.md`（组表：EDIT/MIX 单页）、`docs/design/008-param-pages.md`（布局表只余 SOUND/SAMPLE）、`docs/design/012-feature-parity-checklist.md`（§2.1/§2.2 的 Tab5 落点改为「Sound 页 · <卡片名>卡」；§3 表前加注「族页槽位表自 5B-1 起由 `sound_layout.h` 承载，本表保留为上游映射说明」）、`AGENTS.md` §3/§5.1、`docs/dev/reference/firmware-map.md`

**Interfaces:**
- Produces: 页表 13 页：`PLAY → TRACKS → SOUND → TOOLS → SAMPLE → DSYN → FM6P → STEP → TRANSPORT → SONG → PROJECTS → PRESETS → SYSTEM`；组：`PLAY{PLAY} TRACKS{TRACKS} EDIT{SOUND} MIX{TOOLS} SYNTH{SAMPLE,DSYN,FM6P} SEQ{STEP,TRANSPORT} SONG{SONG} FILE{PROJECTS,PRESETS} SYS{SYSTEM}`。`APP_PAGE_GLOBAL/FX/MOD/SCALE/VOICE` 保留为不注册的枚举值（`UI <name>` ⇒ `UI ERR not_registered <id>`）。

- [ ] **Step 1: 布局表与组表**

按 Interfaces 改 `PARAM_PAGES`/`PARAM_PAGE_COUNT`/`UI_GROUP_LIST`；`page_params.c` 里只对被删页成立的分支（`PARAM_CHIP_WHEN_ANALOG`、chip 行、`UI <page> CHIP` 的多页假设）保留但确认对 SAMPLE 页无副作用；`knob_test.c` 的页表用例改为 2 页并通过。

- [ ] **Step 2: 脚本页表同步**

Run（逐个确认不再引用退役页）: `grep -n -E '"(mod|scale|voice|global|fx)"' scripts/inject/inject.py scripts/soak.py scripts/screenshot_matrix.py | wc -l`
Expected: 逐条确认只剩注释与无关的同名标识（页名引用为 0）。`run_params` 删除退役页段（保留 SAMPLE/DSYN/FM6P/TOOLS/REC/锁 段），在场景头注明「族页段由 sound 场景接管（Task 8）」。`run_nav`：EDIT/MIX 为单页组 ⇒ `UI TAB EDIT` 落 `sound`、`UI TAB MIX` 落 `tools`，子签行为空；有序对 ⑩ 的 Σest 账面改用新值（EDIT 12,288、MIX 3,584）。

- [ ] **Step 3: 构建与 host 测试**

Run: `scripts/build.sh && scripts/build.sh && scripts/knob-test.sh && scripts/sound-test.sh && python3 scripts/soak-selftest.py && python3 scripts/screenshot.py --selftest && python3 scripts/screenshot_matrix.py --selftest`
Expected: 第二次构建 0 警告；全部退出码 0。

- [ ] **Step 4: 烧录；设备核对**

Run: `scripts/flash.sh && scripts/inject/nav.sh && scripts/inject/params.sh && scripts/inject/store.sh && scripts/inject/song.sh && scripts/inject/sys.sh`
Expected: 全部 `全部断言与像素抽检通过`；`UI MEM registered=13`；`UI mod` ⇒ `UI ERR not_registered 15`；`UI TAB SEQ` → `UI sound` 的过渡 `pool_free ≥ 4,096`。

- [ ] **Step 5: 文档与提交**

```bash
git add firmware/ && git commit   # refactor(components/sloop_ui): 退役五张参数页并收拢导航组表
git add scripts/ && git commit    # test(scripts): 页表同步为 13 页并迁出退役页的场景段
git add AGENTS.md docs/ && git commit   # docs(design): 登记参数页退役与 Sound 卡片槽位权威表
git push
```

---

### Task 8: `sound.sh` 注入场景

**Files:**
- Create: `scripts/inject/sound.sh`（入口，照 `tracks.sh`）
- Modify: `scripts/inject/inject.py`（`run_sound`、`SCENARIOS["sound"]`；`screenshot_matrix.py` 的 sound 条目定为 6 个：默认 / 滚动后 / 选择器-引擎 / 选择器-预置 / 鼓轨 / 折叠）
- Modify: `docs/dev/reference/scripts.md`、`verification-gates.md`、`AGENTS.md` §5.1（场景数 10 → 11）

**Interfaces:**
- Consumes: Task 3/4/6 的全部 console 入口与 TAP 回显。
- Produces: `bash scripts/inject/sound.sh [PORT]`，产物 `inject-sound-<时间戳>.log` + PNG；末行 `[PASS] sound: 全部断言与像素抽检通过`。

- [ ] **Step 1: 写 `run_sound`**

结构照 `run_params`（归一 → 段落 → 收口还原）。段落与断言（真值一律取 `ENG SNAP`/`STATE SNAP`）：
① 归一：停走带、轨 0、引擎 ANALOG、预置 0（`CMD_SET_ENGINE/PRESET` 经 TAP 页眉或既有 console 入口）、`UI sound`、`UI SOUND SCROLL 0`、折叠清零。
② 池：`UI MEM` 的 `est=12288`、`delta ≤ est`、`build_ms ≤ 100`。
③ 结构：`UI SOUND SLOT_TOTAL`（新增 console：`UI SOUND STATS cards=13 slots=109 scroll_max=%d`）⇒ `slots=109`；逐卡 `UI SOUND CARD n SLOT 0` 的 label = 期望表（13 个字符串常量，实施者从 `ENG DESC`/规格表取）；`P_FTYPE` 槽 present=1。
④ 写：每张卡首槽 `UI SOUND KNOB` 拖动 +90° ⇒ 值变化（锁定槽除外），`ENG SNAP`/`STATE SNAP` 相符，拖回还原。
⑤ 拖动门禁：按下不改值 / +90° 改值 / 拖回回原值 / 环带外忽略 / 锁定槽忽略。
⑥ 键盘：焦点 ±1 与环绕、↑↓ 调值、ENTER +1、细调 `TAP ±`、折叠跳过。
⑦ 滚动与折叠：`SCROLL` 钳位、折叠后重排、焦点槽自动可见、滚动手势后的释放不折叠；截屏像素抽检：卡片底 `UI_PANEL`、选中槽弧 `UI_ACCENT`、锁定槽 `--` 区域为面板底。
⑧ 选轨条与页眉：四格 TAP 选轨留在本页；点页眉名字开选择器、选引擎/预置/kit（真值断言）。
⑨ 锁定：Ctrl+4 到鼓轨 ⇒ Engine edit 卡全锁、PATTERN 可编辑；换轨还原。
⑩ 有序对：`UI step` → `UI transport` → `UI sound` 过渡 `pool_free ≥ 4,096`；往返 10 次池泄漏 ≤ 128B。
⑪ 截屏 6 张（见 Interfaces）。
- [ ] **Step 2: 设备运行**

Run: `scripts/build.sh && scripts/flash.sh && scripts/inject/sound.sh`
Expected: `[PASS] sound`，0 FAIL。失败 ⇒ 修实现或断言出处，不放宽断言。
- [ ] **Step 3: 文档与提交**

```bash
git add scripts/ && git commit   # test(scripts): 新增 Sound 仪表盘注入场景
git add AGENTS.md docs/ && git commit   # docs(dev): 登记 sound 场景与门禁
git push
```

---

### Task 9: 5B-1 收口回归与证据

**Files:**
- Create: `docs/dev/verify/5-pb1-2026-11-26/`（证据包 + `readme.md`）、`docs/dev/closeout-phase5b1.md`
- Modify: `docs/dev/verification-log.md`、`docs/design/002-port-roadmap.md`（5B 行：5B-1 ✅）、`docs/design/008-param-pages.md`（实测登记定稿）、`docs/superpowers/specs/2026-11-26-phase5b-sound-mixer-device-design.md`（5B-1 小节标注完成与偏差）、本计划（Task 复选框）

- [ ] **Step 1: 最终固件全矩阵（FM-1 未接；同一提交）**

Run（留转录）:
1. `scripts/build.sh --clean && scripts/build.sh && scripts/build.sh` ⇒ 退出码 0，第二次 0 警告。
2. `scripts/host-test.sh`、`state-test.sh`、`knob-test.sh`、`sound-test.sh`、`seq-test.sh`、`synth-test.sh`、`editor-proto-test.sh`、`usb-fm1-test.sh` ⇒ 全部退出码 0。
3. `python3 scripts/soak-selftest.py`、`screenshot.py --selftest`、`screenshot_matrix.py --selftest`、`fm1-smoke.py --selftest` ⇒ 退出码 0。
4. `scripts/flash.sh` ⇒ `Hash of data verified`；启动日志无 fatal/abort；**[操作员]** 拔掉 FM-1 后再做本步。
5. `python3 scripts/device-selftest.py /dev/ttyACM0` ⇒ 182/182；`python3 scripts/selftest-heap.py /dev/ttyACM0` ⇒ 退出码 0。
6. 十一条注入场景（含 `sound.sh`）⇒ 0 FAIL；`scripts/screenshot_matrix.py` ⇒ 全过。
7. 硬复位 + 30s 后 `python3 scripts/soak.py /dev/ttyACM0 --duration 1800 --interval 60` ⇒ 退出码 0。
8. **[操作员]** 插回 FM-1 → `python3 scripts/fm1-smoke.py /dev/ttyACM0` ⇒ PASS；FM-1 已接下 `device-selftest.py` 182/182、`selftest-heap.py` 退出码 0。
Expected: 全绿。

- [ ] **Step 2: 证据入库与文档**

证据复制到 `docs/dev/verify/5-pb1-2026-11-26/`（构建、host、启动日志、182、堆、十一条场景 stdout、截屏矩阵联系表与代表图、soak 摘要、冒烟转录），`readme.md`（门禁→命令→结论 + SHA256），`verification-log.md` 逐项登记；写 `docs/dev/closeout-phase5b1.md`（做成了什么/偏差/读数/裁决/人测项/下一步 5B-2 入口）；路线图 5B 行标 5B-1 ✅。
- [ ] **Step 3: 提交与人眼目检**

```bash
git add docs/ && git commit   # docs(dev): 5B-1 收口报告与验证证据
git push
```
向用户提交三页截屏（Sound 默认/滚动/选择器）做**人眼目检**（对照 `third_party/sloop-fm1-go/screenshots/1.png`）；结论回填收口报告。

---

## Self-Review 记录

- **Spec 覆盖**（5B 规格 §5.2/§5.3/§5.6/§5.8 中属于 5B-1 的项）：卡片成员表 → Task 1；骨架/手势/池 → Task 2；值/锁定/拖动/注入 → Task 3；键盘/细调/选轨条/折叠/ZOOM → Task 4；名字 API → Task 5；选择器 → Task 6；退役与页表 → Task 7；证据 → Task 8/9。**不属本计划**：Mixer（§5.4）、Device（§5.5）、TRACKS 退役、`select_track()` 改页名 → 5B-2/5B-3。
- **类型一致**：`sound_layout_*`、`page_sound_*`、`sloop_ui_sound_*` 的签名在 Task 1/2/3/4/6 间一致；`flat` 一律 `uint16_t`，`SOUND_SLOT_NONE = 0xFFFF`。
- **Review Focus** 的五条分别钉在 Task 2/3（1、5）、Task 1+Task 3（2）、Task 4（3）、Task 6（4）。
- **留给实施者的检查点**（已在步骤里写明）：Task 1 的鼓轨可编辑槽数期望值；Task 2 的预算退路触发条件；Task 7 的 `knob_test.c` 用例改法。
