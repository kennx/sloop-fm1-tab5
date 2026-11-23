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

/* ---- 采样槽 USR1..USR4（P4-D Task 4，spec §3.1/§4.2）---------------------------
 * 上游 4×80KiB 原始槽（flash 0xA0000/0xB4000/0xC8000/0xE7000，header 480B + data@512，
 * IMA ADPCM 4bit）。**不做 A/B 对象**：flash 侧就是上游布局，PSRAM 镜像
 * （`sloop_hal_user_samples()`，selftest.c 把 `SMP_USER_XIP` 指过去）是引擎唯一的读路径。
 * 权威关系：flash 是持久副本；镜像在开机、上传收尾（END）、擦除与 `ENG SELFTEST`
 * 收尾时同步。`info` 只读镜像（与引擎同源），flash 只在 init/END/ERASE 使用。
 * 上传协议（console `SMP`，Task 7）：
 *   begin(slot)            擦槽的 header 扇区并**同时使镜像失效**（header 0xFF、`usr_nz`/
 *                          zone 清零、`smp_user_gen` 递增）——槽立即对引擎不可见；清掉
 *                          上一次未完成的上传（同一时点只允许一个 open slot）；
 *   hdr_append(p, n)       累积 480B header（console 一行上限 120B，故分 4 次）；挂起中
 *                          没有 open slot、走带中、冻结或存储未就绪都拒绝；
 *   write(slot, off, ..)   off ≥ 512、256 对齐、off+n ≤ 0x14000、n ≤ 256；跨 4KB 边界
 *                          的扇区在首次写入前擦除；写成后同步镜像对应偏移（不动 header）；
 *   end(slot)              校验 header（magic/version/nz/data_len/zone 合法）与 CRC（对
 *                          **flash 回读**的数据算，`st_crc32`），最后写 header（提交点）。
 *                          **写 header 成功即提交**：上传随即结束；随后的 512B header 回读
 *                          若失败，用刚落盘的 header 字节补齐镜像（闪存里已是完整采样），
 *                          仍回 ESP_OK，槽立即可用，不需要额外 reload；
 *   erase(slot)            先失效（header 0xFF + 扫描递增 gen，正在发声的 voice 在下一次
 *                          取 zone 时结束——上游 `ed_smp_erase` 同序），再清镜像数据区、
 *                          擦 20 个扇区（80KiB/4KB）；
 *   reload(slot)           flash→镜像 + 扫描（开机装载与自检收尾；`void`）。只要求 HAL 与
 *                          镜像在位——开机装载在 READY 置位之前完成，故不能走
 *                          `sloop_store_ready()` 门。
 * **Tab5 安全差异（spec §3.5，明确接受）**：走带中 begin/hdr_append/write/end/erase 一律
 * 拒绝（`ESP_ERR_INVALID_STATE`）——80KiB 擦写会关 cache、必然可闻，按 "STOP BEFORE SAVE"
 * 纪律处理；上游允许播放中上传。写入期间持存储锁、**不持引擎锁**（`smp_user_scan` 与
 * 镜像失效的那一小段在引擎锁内，设备上 `fm1_irq_off` 是空壳，引擎锁才是真保护）。
 * `info` 在存储锁内拷贝 header 并校验数据 CRC（最坏等一个 flash 写窗口），SAMPLE 页不会
 * 读到 END/ERASE 中途的撕裂 header。
 * 返回码：
 *   ESP_OK                    成功（`info`：未使用/不可用也算成功，`out->used == 0`）
 *   ESP_ERR_INVALID_ARG       槽号 ≥ 4、`out == NULL`；END 的 header 长度/魔数/版本/nz/
 *                             data_len 或 zone 非法（console `SMP ERR header|zones`）
 *   ESP_ERR_INVALID_SIZE      header 累积超过 480B；write 的 off/n 越界（`size`）
 *   ESP_ERR_INVALID_CRC       END 的数据 CRC 不符（`crc`）
 *   ESP_ERR_INVALID_STATE     存储未就绪、诊断冻结、走带中，或没有对应的 open slot
 *   ESP_ERR_NOT_FINISHED      flash 擦/写/读失败（`flash`）；END 的 header 写失败也在内
 * END 失败（header 未写入）时上传保持 open（可重发数据后重试），槽仍是空的。 */
#define SLOOP_STORE_SAMPLES     4u
#define SLOOP_STORE_SAMPLE_SIZE 0x14000u /* 80KiB / 槽（20 个 4KB 扇区） */
#define SLOOP_STORE_SAMPLE_DATA 512u     /* header 480B + 32B padding，数据从这里起 */
typedef struct {
    uint8_t used;    /* header 合法（magic/version/nz/zone + 数据 CRC）且数据可用 */
    uint8_t zones;   /* header 的 nz（1..16）；未使用为 0 */
    char name[9];    /* header 的 8 字符名 + NUL（未使用为 ""） */
    uint16_t data_kb; /* data_len / 1024（未使用为 0） */
} sloop_store_sample_info_t;
esp_err_t sloop_store_sample_info(uint32_t slot, sloop_store_sample_info_t *out);
esp_err_t sloop_store_sample_begin(uint32_t slot);
esp_err_t sloop_store_sample_hdr_append(const uint8_t *p, uint32_t n);
esp_err_t sloop_store_sample_write(uint32_t slot, uint32_t off, const uint8_t *data, uint32_t n);
esp_err_t sloop_store_sample_end(uint32_t slot);
esp_err_t sloop_store_sample_erase(uint32_t slot);
void sloop_store_sample_reload(uint32_t slot);

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
 * 暂存是 **RAW 专用的 4,096B PSRAM**（`s_raw_stage`；P4-D 评审 I1，不与 `up_stage()`
 * /`proj_tmp` 共用）：settings/DSU/song 的 `st_load` 与 1s tick 都不会覆盖它，因此
 * `BEGIN → （song tick / ENG SELFTEST） → END` 提交的仍是上传的原始字节。console 仍
 * 在其它 `STORE` 子命令入口 abort 未完成的上传（R27 的会话确定性约定），但这条已不再
 * 是缓冲安全的必要条件。`end` 持存储锁、只写请求的那一个对象、擦写期间不持引擎锁；
 * `received` 供 console 回显。 */
#define SLOOP_STORE_RAW_MAX 3840u /* = storage.c 的 ST_PAYLOAD_MAX（4KB 扇区 − 256B 头/侧区） */
esp_err_t sloop_store_raw_begin(uint32_t len);
esp_err_t sloop_store_raw_append(const uint8_t *data, uint32_t n);
uint32_t sloop_store_raw_received(void);
esp_err_t sloop_store_raw_end(const char *obj, uint32_t slot);
/* 单独替换 OBJ_SETTINGS 的 80B side record，payload 原样随新副本提交；无 payload 时
 * 按当前 RAM 构造 canonical。停止/冻结/存储锁纪律与 raw_end 相同，成功后按 R15 重载。 */
esp_err_t sloop_store_raw_side(const uint8_t *side);
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
 *   - 没有设置对象：首次保存构造 canonical PER3（88B persist + 1,464B 当前 RAM 的 DSU =
 *     1,552B）+ 默认 80B side record（theme 0 + 空收藏 + CRC）；
 *   - 合法 PER3（84/88/1,552/1,584/1,588/1,592B）：整个 payload 保留，只替换 48..83；
 *     88..1,551 一律写成当前 RAM 的 DSU（84/88B 因此升级到 1,552B，P4-D Task 5）；
 *     已有 MIDI 尾段与 side 逐字节保留（侧区全 0xFF
 *     才写默认 80B side：自校验不过的 2.7 beta 主题记录也不覆盖）；
 *   - PER2/PER1：按上游 persist_boot 口径迁移（palette/lowcut/zoom/panel 保留，缺失的
 *     取默认），随后写成 canonical PER3；
 *   - 未知 magic / 长度不是任何历史格式：拒绝写（ESP_ERR_INVALID_ARG，console 的
 *     `STORE ERR invalid`），原扇区一个字节都不动。
 * `song_get` 只读**内存** arrangement（不需要存储就绪）；`song_set` 只改内存并置 dirty，
 * **不要求存储就绪**（未就绪时 RAM 配置仍可编辑/校验，落盘由 tick 的 ready 检查拒绝；
 * UI 的 SONG 页在未就绪时另行拒绝配置编辑并显示 NO STORAGE）。落盘由 1s 任务的
 * `sections_tick` 在「停止 + 无发声 + 输入空闲 ≥1.5s + 距上次尝试 ≥5s」时完成（失败
 * 保留 dirty，下个窗口重试）；`song_save` 是上游 `arrangement_save` 的同义
 * 入口（停止态立即写，播放中 ESP_ERR_INVALID_STATE = `STOP BEFORE SAVE`）；
 * `song_reload` 从 flash 读回并**以 flash 为准**清 dirty（开机与 SELFTEST 之后用：
 * `arrangement` 是 arranger.c 的 static `.bss`，自检的 restore 会把它回滚到零）。读不到时
 * 落上游**默认歌曲**（count=4、A/B/C/D 各 4 小节；这是 `persist_boot` 的第一步，不留
 * count=0 的非法空歌曲）；对象损坏的字节不被覆盖（不再尝试保存），回 ESP_ERR_INVALID_ARG；
 * 读 flash 期间若来了 `song_set`，以 RAM 为准（代次守卫）。
 * `sections_tick(now_ms)` 由 1s 任务以 esp_timer 毫秒调用；诊断冻结时不写，渲染路径
 * 绝不写 flash。`song_info` 是 `STORE SONG INFO` 的诊断投影（只读 flash 对象）。 */
#define SLOOP_SONG_ENTRIES 16u
#define SLOOP_SONG_BARS_MAX 64u /* 每项 1..64 bars（上游 arr_valid） */
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
/* SONG REC 完成（含 STOP 收尾）由 seq 适配层对账后置 song dirty（幂等；延迟写按同一
 * 门限落盘）。 */
void sloop_store_song_dirty_mark(void);
void sloop_store_sections_tick(uint32_t now_ms);

/* ---- SYN kit 银行（DSU）：OBJ_SETTINGS 的 88..1,551（P4-D Task 5，spec §3.4）--------
 * 上游把 4 个 SYN kit（`dsu_bank_t`，1,464B）放在设置对象 `persist_t` 之后；`dsu` 是
 * drum_synth.c 的 static，鼓声部直接从 RAM 读它。两条路径：
 *   reload()  —— 开机（`sloop_store_init`，工程 fetch 与歌曲之后）与 `ENG SELFTEST` 收尾：
 *                settings 88..1,551 → `dsu`（`dsu_valid` 才覆盖，每个 kit 过 `dsu_fix_kit`；
 *                没有对象/长度不足/银行非法 → 工厂 SYN kit）。`dsu` 与 `dsu_dirty` 都是
 *                `.bss`，自检的 restore 会回滚它们，故必须重载。存储未就绪 →
 *                ESP_ERR_INVALID_STATE（`dsu` 落工厂默认），无有效 DSU → ESP_ERR_NOT_FOUND。
 *   store()   —— 显式 STORE（DSYN 页的 STORE chip）：停止态把**当前 RAM 的 DSU** 连同当前
 *                歌曲与 side 一起写进 settings（上游 `settings_write` 一次写整份 persist_t，
 *                没有只写 DSU 的对象）。走带中/诊断冻结/未就绪 → ESP_ERR_INVALID_STATE
 *                （`STOP BEFORE SAVE` 口径）；settings 对象损坏（未知 magic/长度）→
 *                ESP_ERR_INVALID_ARG 且不覆盖；擦写失败 → ESP_ERR_NOT_FINISHED。
 * `dirty()` 表示「RAM 的 SYN kit 与最近一次**成功写盘**不一致」：DSYN 编辑置位；任何一次
 * 成功的 settings 保存（显式 DSU STORE、显式歌曲保存、sections/歌曲延迟写）在写成功且
 * 提交的字节就是当前 RAM 时清——**不区分**是谁顺带写的（spec §3.4）。DSYN-only 修改不会
 * 自行触发 1s 延迟写（上游同：只在下一次 settings 写时落盘）。 */
esp_err_t sloop_store_dsu_reload(void);
esp_err_t sloop_store_dsu_store(void);
bool sloop_store_dsu_dirty(void);

/* ---- live sections A–D（P4-C Task 8）：捕获在 sloop_seq_section（引擎锁内，见
 * sloop_seq.h），这里只做 dirty 记账、停止态载入/擦除与延迟写。A–D 的内容仍是 FUN6
 * 工程对象（OBJ_PROJECT0 + slot），与普通 PROJECT SAVE 同一对象。
 *   dirty_mark_locked(slot) 置 section dirty + ++section gen；**调用方必须已持引擎锁**
 *                     （与 proj_capture 同一临界区，erase 的 gen 比较才能看到每一次捕获；
 *                     本函数不取锁，不取存储锁——捕获路径很短）
 *   sections_dirty()  有任一段未落盘（`dirty` 投影/诊断读；存储未就绪时 false）
 *   section_load(slot) 仅停止态（走带中 ESP_ERR_NOT_ALLOWED），存储未就绪
 *                     ESP_ERR_INVALID_STATE；把 **RAM 槽**应用到引擎（不回落 flash、
 *                     不写存储），live_sec = slot；空槽 NOT_FOUND
 *   section_erase(slot) 仅停止态；空槽 NOT_FOUND 且不写 flash；成功写空对象、清 RAM 槽
 *                     与 dirty 位、live_sec 归 -1，并从歌曲顺序里 `arr_drop_scene`
 *                     （有项被删就置 song dirty；一段不剩回默认歌曲）。擦写期间若有新的
 *                     section capture（gen 变化）则保留新捕获与 dirty 位，槽由下一个
 *                     tick 重写——不静默丢掉并发写入
 * 1s 任务的 `sections_tick` 把 dirty 段与歌曲放在同一拍、同一门限（停止 + 无发声 +
 * 输入空闲 ≥1.5s + 间隔 ≥5s）里写：引擎锁内把槽快照进暂存，**擦写期间不持引擎锁**。 */
void sloop_store_section_dirty_mark_locked(uint32_t slot);
bool sloop_store_sections_dirty(void);
esp_err_t sloop_store_section_load(uint32_t slot);
esp_err_t sloop_store_section_erase(uint32_t slot);

/* ---- settings 字段（OBJ_SETTINGS 的 persist_t 头 + MIDI 尾段，P4-E Task 1，spec §8.1）----
 * 上游 persist_t（88B）的 palette@4 / lowcut@8 / zoom@12、lights word@84 里本固件拥有的
 * 5 个位（lights_lvl 0..3 / rec_tempo 9 / rec_count 10 / theme_on 30 / rec_quant_off 31）、
 * 以及 MIDI 尾段 byte38 bits 2..5 的 flt_reso（0..10）。字节层的读写是 src/song_persist.h 的
 * `song_per_put_settings` / `song_per_get_settings`（纯函数，host 同源）；
 * `song_per_side_theme` 把 side record 的 theme_t（"THM2" + name[8] + RGB565 色表 + CRC）
 * 校验并展开成下面的 RGB888 视图。字段取值即上游原值（lowcut/zoom 0|1，palette 是内置
 * 调色板号）。 */
typedef struct {
    uint32_t palette, lowcut, zoom, lights_lvl, theme_on, rec_tempo, rec_count, rec_quant_off, flt_reso;
} song_settings_t;
typedef struct {
    uint8_t c[5][3];   /* 屏幕五级色，RGB888 */
    uint8_t trk[4][3]; /* 四轨色，RGB888 */
    uint8_t bg[3];     /* 背景 */
    uint8_t hi[3];     /* 强调色（触摸/光标/播放头） */
    char name[9];      /* 上游 name[8] 补 NUL */
} song_theme_view_t;

/* ---- settings 生效副本（P4-E Task 2，spec §5 D4/D5/D7、§8.2）-------------------------
 * 上游 panel.c 的 `settings`/`lights_*` 与 fx.c/seq.c 的 `fx_lowcut`/`flt_reso`/`rec_tempo`/
 * `rec_count`/`rec_quant_off` 在本固件里由一份 RAM 副本 `s_set` 统一承载：
 *   reload() —— 开机（`sloop_store_init`，歌曲/DSU 之后）与 `ENG SELFTEST` 收尾：从 flash
 *               **以 flash 为准**重读（palette/lowcut/zoom/lights 五位/尾段 reso + side
 *               theme）并应用；没有对象/对象损坏/存储未就绪 → 其余字段全零 + 默认
 *               palette 4 = MONO（R13，上游 settings_init 同法）。
 *   set()    —— 停止/播放皆可：只改内存 + 应用上游 static（引擎锁内）+ 置 dirty，**不写
 *               flash**；冻结/未就绪 → ESP_ERR_INVALID_STATE。
 *   save()   —— 停止态立即写（上游 settings_save 同义；与歌曲/DSU 同一次 A/B 提交）；
 *               播放中 ESP_ERR_INVALID_STATE（`STOP BEFORE SAVE`）；成功才清 dirty，并同拍
 *               flush 平台设置的 NVS dirty。
 *   落盘还走 1s 任务的 `sections_tick`：settings dirty 与歌曲/段落同一门限（停止 + 无发声
 *   + 输入空闲 ≥1.5s + 间隔 ≥5s）、同一次写；任何一次成功的 settings 保存都**顺带清 dirty
 *   ——前提是刚提交的字节就是当前 RAM**（擦写窗口里又来的修改让 payload 过时，dirty 保留）。
 * 应用范围：core 只把 lowcut/reso/REC 三项写进上游 static；`lights_lvl` 只在 RAM 副本里
 * （键盘 RGB 是 UI 侧按投影驱动，spec R4）；palette/zoom/theme 同理由 UI 消费副本。
 * 自检：`sloop_core_selftest` 跑用例前把 `fx_lowcut`/`flt_reso` 归零（golden 基线口径），
 * 收尾 reload 按 flash 重放。 */
#define SLOOP_SETTINGS_PALETTES 5u
typedef enum {
    SLOOP_SET_PALETTE,
    SLOOP_SET_LOWCUT,
    SLOOP_SET_ZOOM,
    SLOOP_SET_LIGHTS,
    SLOOP_SET_RESO,
    SLOOP_SET_THEME_ON,
    SLOOP_SET_FIELD_COUNT
} sloop_set_field_t;
typedef struct {
    song_settings_t f;
    bool theme_valid;
    char theme_name[9];
    song_theme_view_t theme; /* P4-E Task 5 R21：side theme 的 RGB888 生效副本 */
    bool dirty;
} sloop_settings_t;
const sloop_settings_t *sloop_store_settings(void); /* 生效副本（存储未就绪 ⇒ 字段全零、palette=4、theme_valid=false） */
/* 改内存 + apply + dirty；冻结回 INVALID_STATE；palette 钳 0..4、reso 钳 0..10、
 * lights 钳 0..3、bool 钳 0/1；字段越界回 INVALID_ARG（不写）。 */
esp_err_t sloop_store_settings_set(sloop_set_field_t field, uint32_t v);
esp_err_t sloop_store_settings_save(void);   /* 停止态立即写；播放中 INVALID_STATE；成功清 dirty */
esp_err_t sloop_store_settings_reload(void); /* 以 flash 为准重读 + apply；boot/自检收尾调用 */
bool sloop_store_settings_dirty(void);
void sloop_store_settings_dirty_mark(void);  /* REC 配置变更用（只置 dirty，不改值） */

#endif
