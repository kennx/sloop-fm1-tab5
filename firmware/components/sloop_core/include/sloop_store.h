#ifndef SLOOP_STORE_H
#define SLOOP_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 存储层（P4-B）：上游 SLOOP 的 storage.c（对象 = 一对 4KB 扇区，A/B + 32B 提交头 +
 * CRC + 回读校验）与 project.c 的格式层（FUN6 写、FUN5/4/3/2/1 读）原样编入本组件，
 * 数据落在原始分区 sloopstore（ADR-009）。返回码约定：
 *   ESP_OK                 成功
 *   ESP_ERR_NOT_FOUND      槽为空（RAM 槽无效且 flash 无有效副本）
 *   ESP_ERR_INVALID_STATE  走带中（对齐上游 "STOP BEFORE SAVE"）、存储未就绪，或诊断冻结
 *                          （`ENG FREEZE` / `ENG SELFTEST` 期间）——冻结时所有写/载入
 *                          入口在改任何状态之前就返回
 *   ESP_ERR_NOT_FINISHED   flash 擦/写/回读校验失败（RAM 槽保留新值，与上游一致）
 *   ESP_ERR_INVALID_ARG    槽号/对象名越界；或 flash 对象存在但 `proj_import` 拒绝
 *                          （任何已知格式都不匹配 ⇒ `STORE ERR invalid`，与空槽的
 *                          `ESP_ERR_NOT_FOUND`/`empty` 区分）
 *   ESP_ERR_INVALID_SIZE   `STORE RAW` 的长度/块越界（`STORE ERR size`）
 * 所有 API 不得在 ISR 中调用；写操作由存储锁串行化（console/UI/自动存档共用）。 */

#define SLOOP_STORE_PROJECTS 4u
#define SLOOP_STORE_PRESETS 32u
#define SLOOP_STORE_BANK_SLOTS 27u
#define SLOOP_STORE_BANK_PACKED 128u

typedef struct {
    bool ready;               /* PSRAM 槽/暂存已分配且分区 mmap 就绪 */
    uint32_t objects_ok;      /* flash 中能读出有效副本的工程对象数（0..4） */
    uint32_t faults_armed;    /* HAL 故障注入是否待命中（1 = 已 arm 未触发） */
    uint32_t faults_taken;    /* 自上次 arm/clear 起命中次数 */
    uint32_t autosave_hash;   /* 最近一次写入（或开机恢复）的工程捕获 hash；0 = 无 */
    uint32_t autosave_writes; /* 本次开机以来的自动存档写入次数 */
    bool autosave_on;         /* 自动存档开关（`STORE AUTO ON|OFF`，默认 ON） */
    int32_t last_save_xrun;   /* 上一次写动作（save/erase/preset/bank/RAW/autosave）的 xrun 增量 */
    uint32_t last_save_ms;    /* 上一次写动作的耗时（含擦除 + 写 + 回读校验；spec §5 的 ≤150ms 口径） */
} sloop_store_stats_t;

/* sloop_core_init 内调用（ADR-009 §5：snapshot → store init → autosave resume）：
 * PSRAM 分配（RAM 工程槽 4×3,816B、旧格式暂存、4,096B 通用暂存）→ HAL 分区/mmap →
 * 四个工程槽开机 fetch。任一失败只让 ready=false（不崩溃、不阻断启动）。 */
esp_err_t sloop_store_init(void);
bool sloop_store_ready(void);
void sloop_store_stats(sloop_store_stats_t *out);

/* 工程 A–D（0..3）：RAM 槽（PSRAM）+ flash 双层，与上游 project.c 同语义。
 * save = 引擎锁内 capture 到 RAM 槽 → 释放锁 → st_save（擦写期间不持引擎锁）；
 * load = RAM 槽有效则直接用，否则 st_load + proj_import 回填，再引擎锁内 proj_apply；
 * erase = 槽空则 **不写 flash**、回 ESP_ERR_NOT_FOUND（上游 project_erase 的
 *         `EMPTY SECTION`：多一次擦写没有意义）；否则写入空对象（len=0）+ 清 RAM 槽；
 * hash = project_t.sum（0 = 空/无效）。 */
bool sloop_store_project_used(uint32_t slot);
uint32_t sloop_store_project_hash(uint32_t slot);
uint32_t sloop_store_live_hash(void); /* 当前引擎状态的捕获 hash（不写 flash） */
esp_err_t sloop_store_project_save(uint32_t slot);
esp_err_t sloop_store_project_load(uint32_t slot);
esp_err_t sloop_store_project_erase(uint32_t slot);

/* 诊断（P4-B Task 5 评审 R18，**测试专用**；破坏性：覆盖**诊断槽 3** 的 RAM+flash 对象，
 * 槽 0–2 不碰）：以当前引擎状态捕获一份内存工程，按 case 改写出 `project_load` 的
 * fallback 边界，存到槽 3 → 清 RAM 槽（强制 flash 回落 + `proj_import`）→ 走公共 LOAD
 * → 回读各轨结果（engine/preset/steps/atk/e0），供注入脚本断言：
 *   "fmt1"    轨 0 的 preset 写 0xFF、engine 写 0（格式 1 没有预置号，引擎来自工程）⇒
 *             载入后 preset 取 `TRK_DEF[0][1]` 的默认预置（808 BOOM 的值）+ `steps_clear`；
 *             轨 1 的 engine/preset 必须保留工程里的值；
 *   "foreign" 轨 1 的 engine 写 NENGINES+2（本固件没有的引擎）⇒ `proj_foreign_parts`
 *             换回默认引擎+预置（`TRK_DEF[1]`），图案保留。
 * 走带中/诊断冻结拒绝（ESP_ERR_INVALID_STATE）；case 名未知 ESP_ERR_INVALID_ARG。
 * 保存失败时诊断槽 3 的 RAM 缓存一并清空（不把改写过的合成工程留给后续 LOAD）。 */
typedef struct {
    struct {
        uint8_t engine, preset; /* eng_req / preset（LOAD 之后） */
        uint8_t steps;          /* 非空步数（合成轨按 step.n、鼓轨按 dstep.on） */
        int16_t atk, e0;        /* P_ATK / P_E0（默认声音的写值判据） */
    } trk[4];
} sloop_store_synth_t;
esp_err_t sloop_store_project_synth(const char *case_name, sloop_store_synth_t *out);

/* 旧格式/工程字段诊断（`STORE PROJ FIELDS n`，spec §6.3）：只读**flash 对象**
 * （`st_load` → `proj_import`），**不改引擎、不改 RAM 槽**——故它是「旧格式 fixture 被
 * 正确转换」的机器判据，与 `STORE PROJ LOAD`（会 proj_apply）分开。fmt 由对象魔数判定
 * （1/4/5/6）；空槽（无有效副本）ESP_ERR_NOT_FOUND，对象存在但导入失败 ESP_ERR_INVALID_ARG，
 * 槽号越界 ESP_ERR_INVALID_ARG，未就绪 ESP_ERR_INVALID_STATE。`out` 只在成功时写。
 * **CRC 损坏的对象与空对象同口径**：`st_load` 读不出「头合法 + CRC 对」的副本 ⇒ 回
 * ESP_ERR_NOT_FOUND（console 的 `STORE ERR empty`），不区分「从未写过」与「写坏了」。 */
typedef struct {
    uint8_t fmt, sel;        /* 原始格式号；选中轨（格式 1 没有 sel ⇒ 0） */
    int16_t bpm, swing, dust; /* G_BPM / G_SWING / G_DUST（格式 1..3 的 SWING 已按 MPC 缩放） */
    uint8_t drdly;           /* 2.5 的 G_DRDLY（格式 <5 没有这个字节 ⇒ 0） */
    struct {
        uint8_t engine, preset;
        int16_t level, atk;  /* P_LEVEL / P_ATK（上游 TP 默认值或工程里的值） */
        int16_t e0, e1;      /* P_E0 / P_E1（格式 5/4 的 P_E0.. 搬移、格式 1 的引擎默认） */
        uint8_t n[8];        /* 前 8 步：合成轨 step.n；鼓轨 = 置位 lane 数 */
        uint16_t v[8];       /* 合成轨 step.note[0]；鼓轨 = lane 掩码（on[0] | on[1] << 8） */
    } trk[4];
} sloop_store_fields_t;
esp_err_t sloop_store_project_fields(uint32_t slot, sloop_store_fields_t *out);

/* RAW 上传通道（`STORE RAW BEGIN/<hex>/END`，spec §4.4）：把任意对象 payload 分块搬进
 * 4,096B 暂存，再以 `st_save` **原样**写入（CRC/提交头由 storage.c 生成）。用途：旧格式
 * 工程 fixture（3,816B 级）放不进 console 单行。语义：
 *   begin(len)  —— len 1..SLOOP_STORE_RAW_MAX=3,840（= storage.c 的 ST_PAYLOAD_MAX：一个
 *                  flash 对象能写的 payload 上限；超限 ⇒ ESP_ERR_INVALID_SIZE，不会等到
 *                  END 才回 save）；重复 begin 覆盖上一次；
 *   append(..)  —— 累计收到的字节数；超过 begin 声明的 len（或内部计数已越界）⇒
 *                  ESP_ERR_INVALID_SIZE（状态保持，调用方可 ABORT 重来）；
 *                  未 begin ⇒ ESP_ERR_INVALID_ARG；
 *   end(obj,n)  —— 必须已收到恰好 len 字节；**无论对象名/槽号是否合法，END 都退出上传态**
 *                  （错误后重试必须重新 BEGIN）；`obj` 目前只支持 "project"（→
 *                  OBJ_PROJECT0 + slot，slot 0..3，工程槽的 RAM 缓存按开机 fetch 的语义
 *                  重新 `proj_import`，导入失败则槽无效 ⇒ 之后 LOAD 走 flash 并回
 *                  invalid）；**只写请求的那一个对象**；走带中拒绝 =
 *                  ESP_ERR_INVALID_STATE（与其它写 API 同序）；
 *   abort()     —— 丢弃暂存状态（幂等）。
 * 暂存是与 `up_stage()`/`proj_tmp` **共用**的 4,096B（PSRAM）：任何其它存储操作都可能
 * 借用同一块暂存，所以**每一条其它 `STORE` 子命令在入口都先 abort 未完成的上传**
 * （console 的 R27 纪律；C API 的调用方同样必须这么做——未 abort 就穿插别的存储操作，
 * 上传内容会被静默覆盖，END 写进 flash 的是别人的字节）。`end` 持存储锁、只写请求的
 * 那一个对象、擦写期间不持引擎锁；`received` 供 console 回显。 */
#define SLOOP_STORE_RAW_MAX 3840u /* = storage.c 的 ST_PAYLOAD_MAX（4KB 扇区 − 256B 头/侧区） */
esp_err_t sloop_store_raw_begin(uint32_t len);
esp_err_t sloop_store_raw_append(const uint8_t *data, uint32_t n);
uint32_t sloop_store_raw_received(void);
esp_err_t sloop_store_raw_end(const char *obj, uint32_t slot);
void sloop_store_raw_abort(void);

/* 用户预置 0..31（上游 upreset.c：32 槽 / 2 个银行对象，`UPB1` 记录布局原样）。
 * 写 = 当前**选中轨**的声音（引擎 + 除混音/图案参数外的全部参数 + 前 16 步图案）；
 * 读可指定任意合成轨；鼓轨没有声音可存 ⇒ 写/读都回 ESP_ERR_INVALID_ARG（写另出
 * ui_message("DRUM TRACK: NO SOUND")，与上游 SAVE > USER 页一致）。
 * 名字大写、0 结尾（13 字节，上游 up_name 口径）；空槽 not_found；整表查询只读 mmap，
 * 但会在存储锁内重扫银行索引（见 store_tab5.c 的 .bss 回滚说明）。
 * load 不写 flash、可在走带中调用（上游 MIDI PC 语义）；store/erase 走带中拒绝。 */
uint32_t sloop_store_preset_count(void); /* 已用槽数（0..32） */
bool sloop_store_preset_used(uint32_t slot);
bool sloop_store_preset_name(uint32_t slot, char out[13]);
uint8_t sloop_store_preset_engine(uint32_t slot); /* 引擎号；空槽 0xFF */
esp_err_t sloop_store_preset_store(uint32_t slot);
esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track);
esp_err_t sloop_store_preset_erase(uint32_t slot);

/* FM6 银行 0..26（上游 fm6_bank.c：27 个 128B packed patch 在**一个** storage.c 对象里，
 * `FM6B`/ver 1 布局原样，XIP 原地读，写 = 整银行重写 + 回读校验）。
 * 槽 k 的音源选择是 PTCH = 8 + k（上游 fm6_slot_get：< 8 是工厂槽 F1..F8；PTCH 即本
 * 引擎的 P_E7，参数 id 70）。put/erase 走带中拒绝（ESP_ERR_INVALID_STATE；擦写前必须停
 * 走带，sector erase 会静音）；put 的 packed 记录须是 fm6_pack 的 128B 口径。
 * 读入口（used/get）只读 mmap，但会在存储锁内重扫银行索引（`.bss` 回滚，见下）。
 * get 失败（空槽/槽号越界/未就绪）时 **out 不被写**——调用方不得把上一次的内容当结果。
 * bank_current 把**选中轨**的 live patch（fm6_patch，可来自工厂槽或银行槽）打包成 128B
 * ——PUT 的输入；选中鼓轨时输出全 0（鼓轨没有 patch），console 在调用前就拒绝。
 * bank_current **不依赖存储就绪**（只读引擎状态、只取引擎锁）：分区/mmap 不可用时仍可
 * 导出当前音色；需要存储的是 put/erase（未就绪回 ESP_ERR_INVALID_STATE）。
 * 银行索引 `fm6_bank_cur/used` 是 fm6_bank.c 的普通 `.bss`，`ENG SELFTEST` 的 restore 会
 * 清空它（ADR-009 §8）：故**每个**入口在存储锁内重跑 `fm6_bank_boot()` 重扫（写入口尤其必须：
 * 上游从「当前副本」起笔，空索引会把同银行的其它槽抹掉）。同一回滚也清掉 `fm6_bank_read`
 * 钩子 ⇒ **自检之后、下一次 bank 入口或重启之前**，PTCH ≥ 8 的轨载入的是 `FM6_INIT`
 * （DI-20 的第二个症状；bank 对象与 flash 字节不受影响）。 */
bool sloop_store_bank_used(uint32_t slot);
bool sloop_store_bank_get(uint32_t slot, uint8_t out[SLOOP_STORE_BANK_PACKED]);
esp_err_t sloop_store_bank_put(uint32_t slot, const uint8_t *packed);
esp_err_t sloop_store_bank_erase(uint32_t slot);
void sloop_store_bank_current(uint8_t *packed);

/* 设备侧 torn-write 故障注入套件（**测试专用**；ADR-009 §3「掉电不破坏旧副本」）。
 * 对象 = `OBJ_SETTINGS`（P4-C 起有真实写入者：PER3 歌曲 + side record）。
 * **快照 = 两个完整 4KB 副本扇区**：套件先 `sloop_hal_storage_read` 两个扇区的原始字节
 * （提交头 + payload + 偏移 64..255 的 side record + 擦除态；8KB 一次性 PSRAM），六个
 * 场景结束后 `sloop_hal_storage_fault_clear()` → 每扇区擦一次 + 按 256B 块原样写回 →
 * **逐字节读回校验**。arrangement（payload 48..83）、DSU/MIDI 尾段与 side record 因此都
 * 逐字节不变（评审 R26 的收口判据）。
 * **破坏性**：运行期间该对象的内容被反复覆盖（跨掉电无保证；不是用户路径）。
 * 六个场景（每个都自己归一、自己断言前提；失败场景还必须命中预期写操作——读回
 * 扇区指纹区分 erase 与 prog，只数 faults_taken 分不出来）：
 *   1 三次提交 → 载入 seq 大者：v2 在 B（copy 1）胜、v3 回 A（copy 0）胜；
 *   2 擦除失败 → 写失败 → 旧副本（B，v2 的字节）完整可读；
 *   3 payload 半写 → 写失败 → 旧副本完整可读；
 *   4 头写失败 → 写失败 → 旧副本完整可读；
 *   5 写后破坏 payload → 载入报错（CRC 拒绝），不返回坏数据；
 *   6 `st_save(obj, NULL, 0)` → 对象为空（len=0）。
 * 全程持存储锁（自动存档/歌曲 tick 的 trylock 会跳过，不与套件交错）。`out`：cases = 场景
 * 总数（6）、passed = 通过数、fail_case = 第一个失败场景名（全过为 NULL）；快照读失败为
 * "snapshot-settings"，恢复或逐字节校验失败为 "restore-settings"（**恢复失败优先**——
 * 那意味着设置对象已被留在坏状态，比任何单个场景更重要）。返回：ESP_OK 全过；
 * ESP_ERR_NOT_FINISHED 有场景/快照/恢复失败；ESP_ERR_NO_MEM 拿不到 8KB 快照；
 * ESP_ERR_INVALID_STATE 未就绪/走带中（out->cases 已填、passed=0）；out==NULL 为
 * ESP_ERR_INVALID_ARG。不得在 ISR 调用。 */
typedef struct {
    uint32_t cases;        /* 场景总数（6） */
    uint32_t passed;       /* 通过数（0..cases） */
    const char *fail_case; /* 第一个失败场景名；NULL = 全过 */
} sloop_store_test_t;
esp_err_t sloop_store_test(sloop_store_test_t *out);

/* 自动存档与上电恢复（上游 `project.c` 的 autosave_tick/autosave_resume，语义照抄）：
 * 条件全满足才把当前工作工程写进 `OBJ_AUTOSAVE`——非播放（`!song.playing &&
 * !transport_req`）、无输入 ≥2.5s、距上次保存 ≥20s、无发声（合成/鼓 voice 均 inactive）、
 * 捕获 hash 与上次写入不同、存储未忙、未诊断冻结；同一状态不重复写（flash 寿命）。
 * `note_activity` 由输入派发调用（物理/注入键盘、TAP 命中、切页与焦点变更、成功的
 * `state_apply`），只记录时间戳；`tick` 由 1s 周期任务调用；
 * `autosave_resume` 在 `sloop_core_init` 的 `sloop_store_init` 之后、UI/音频启动之前调用
 * ——`ENG SELFTEST` 的工厂态快照已经拍完，基线不受影响；
 * `autosave_start` 必须在 `autosave_resume` **之后**调用（先起任务的那一拍会把工厂态
 * 写进 autosave、覆盖随后要恢复的工程），幂等。 */
void sloop_store_note_activity(uint32_t now_ms);
void sloop_store_autosave_tick(uint32_t now_ms);
esp_err_t sloop_store_autosave_resume(void);
esp_err_t sloop_store_autosave_start(void);
/* 诊断开关（`STORE AUTO ON|OFF`，测试用；默认 ON）。关闭后 tick 不再写 flash，
 * 开机恢复仍照常发生。返回设置后的状态。 */
bool sloop_store_autosave_enable(bool on);

/* ---- 歌曲顺序：OBJ_SETTINGS 的 PER3 子集（P4-C Task 7，ADR-009 §6 / ADR-010 §4）----
 * 上游把 16 项 A–D 歌曲顺序（count/loop/(scene,bars)×16）放在设置对象 `persist_t` 的
 * 偏移 48；A–D 的内容仍是 FUN6 工程对象（`sloop_store_project_*`）。写规则：
 *   - 没有设置对象：首次保存构造 canonical PER3（88B persist + 1,464B 默认 DSU =
 *     1,552B）+ 默认 80B side record（theme 0 + 空收藏 + CRC）；
 *   - 合法 PER3（84/88/1,552/1,584/1,588/1,592B）：整个 payload 保留，只替换 48..83；
 *     尚无 DSU 的补默认 DSU（1,552B），已有 MIDI 尾段与 side 逐字节保留（侧区全 0xFF
 *     才写默认 80B side：自校验不过的 2.7 beta 主题记录也不覆盖）；
 *   - PER2/PER1：按上游 persist_boot 口径迁移（palette/lowcut/zoom/panel 保留，缺失的
 *     取默认），随后写成 canonical PER3；
 *   - 未知 magic / 长度不是任何历史格式：拒绝写（ESP_ERR_INVALID_ARG，console 的
 *     `STORE ERR invalid`），原扇区一个字节都不动。
 * `song_get` 只读**内存** arrangement（不需要存储就绪）；`song_set` 只改内存并置 dirty，
 * 落盘由 1s 任务的 `sections_tick` 在「停止 + 无发声 + 输入空闲 ≥1.5s + 距上次尝试 ≥5s」
 * 时完成（失败保留 dirty，下个窗口重试）；`song_save` 是上游 `arrangement_save` 的同义
 * 入口（停止态立即写，播放中 ESP_ERR_INVALID_STATE = `STOP BEFORE SAVE`）；
 * `song_reload` 从 flash 读回并**以 flash 为准**清 dirty（开机与 SELFTEST 之后用：
 * `arrangement` 是 arranger.c 的 static `.bss`，自检的 restore 会把它回滚到零）。读不到时
 * 落上游**默认歌曲**（count=4、A/B/C/D 各 4 小节；这是 `persist_boot` 的第一步，不留
 * count=0 的非法空歌曲）；对象损坏的字节不被覆盖（不再尝试保存），回 ESP_ERR_INVALID_ARG；
 * 读 flash 期间若来了 `song_set`，以 RAM 为准（代次守卫）。
 * `sections_tick(now_ms)` 由 1s 任务以 esp_timer 毫秒调用；诊断冻结时不写，渲染路径
 * 绝不写 flash。`song_info` 是 `STORE SONG INFO` 的诊断投影（只读 flash 对象）。 */
#define SLOOP_SONG_ENTRIES 16u
typedef struct {
    uint8_t scene, bars;
} sloop_song_entry_t;
typedef struct {
    uint8_t count, loop;
    sloop_song_entry_t entry[SLOOP_SONG_ENTRIES];
} sloop_song_config_t;
typedef enum {
    SLOOP_SONG_EMPTY = 0, /* 没有对象（st_load 读不出有效副本） */
    SLOOP_SONG_PER3,      /* 合法 PER3（含历史长度/MIDI 尾段） */
    SLOOP_SONG_PER2,      /* 合法 PER2（保存时会迁移） */
    SLOOP_SONG_PER1,      /* 合法 PER1（保存时会迁移） */
    SLOOP_SONG_INVALID,   /* 未知 magic / 长度不认识：拒绝覆盖 */
} sloop_song_kind_t;
typedef struct {
    uint16_t len;    /* flash 对象的 payload 长度（0 = 空） */
    uint8_t kind;    /* sloop_song_kind_t */
    uint8_t side;    /* 1 = 当前副本的 side record 自校验通过（magic + CRC） */
    uint8_t count;   /* 读回的歌曲项数（PER1/2/空：默认歌曲的 4 项） */
    uint8_t loop;
} sloop_store_song_info_t;
esp_err_t sloop_store_song_get(sloop_song_config_t *out);
esp_err_t sloop_store_song_set(const sloop_song_config_t *cfg);
esp_err_t sloop_store_song_save(void);
esp_err_t sloop_store_song_reload(void);
esp_err_t sloop_store_song_info(sloop_store_song_info_t *out);
/* 有没有「改了还没落盘」的歌曲（诊断/测试用；Task 8 的 SONG 页状态行也读它）。 */
bool sloop_store_song_dirty(void);
void sloop_store_sections_tick(uint32_t now_ms);

#endif
