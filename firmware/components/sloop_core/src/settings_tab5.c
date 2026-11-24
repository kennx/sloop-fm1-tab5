/* P4-E Task 2：settings 生效副本（spec §5 D4/D5/D7、§8.2）。由 selftest.c 末尾 include
 * （在 sound_tab5.c 之后），因此上游 statics fx_lowcut / flt_reso（fx.c）、rec_tempo /
 * rec_count / rec_quant_off（seq.c）、ml_tab（seq.c）与 store_tab5.c 的 st_load /
 * st_side_load / store_lock / s_frozen / song_persist.h 的纯函数都可见。
 *
 * `s_set` 是普通 `.bss`（不进 .noinit，与 song/dsu 的 RAM 状态同一口径）：`ENG SELFTEST`
 * 的 restore 把它回滚到 init 快照（全零），自检收尾的 settings_reload() 再以 flash 为准
 * 重读——自检那一刻还没落盘的 settings 修改随回滚丢弃（诊断命令不是用户路径，spec §5 D7）。
 * `dirty` 也在里面：restore 之后为 false，reload 再读 flash 的现状，两者一致。
 *
 * 锁序（ADR-009 §3）：写 RAM 与上游 static 在**引擎锁**内，不取存储锁、不写 flash；
 * 读 flash 在存储锁内、随后才取引擎锁应用（store → engine，与 song/dsu reload 同序）。
 * 落盘只经 store_tab5.c 的 song_write_locked（它持存储锁、在引擎锁内读 s_set.f）。 */
#include "sloop_core.h"
#include "sloop_store.h"

#include <stdio.h>
#include <string.h>

static sloop_settings_t s_set;
/* 尾段默认布局（40B）：song_per_put_settings 在「没有尾段且 reso ≠ 0」时拿它补出 40B
 * 再打点（上游 settings_write 同法：非默认才写尾段）。由 settings_reload 在引擎锁内刷新。 */
static uint8_t s_midi_def[SONG_MIDI_MAX];

_Static_assert(SONG_MIDI_MAX == 2u * ML_N + 8u, "尾段长度 = 2*ML_N + 8（上游 MIDI_PART）");

/* 上游 panel.c:87 的 MIDI_SET_DEF（midi_part_fill 读的 midi_set[] 初值，panel.c:89；那个 .c
 * 不在本 TU：与 store_tab5.c 的 SONG_PANEL_DEF / TRK_DEF 同法固定在此）。 */
#define SETTINGS_MIDI_DEF {1, 2, 3, 10, 0, 2, 0, 0, 0, 0}

/* 把 s_set.f 应用到上游 static（调用方持引擎锁）。**不驱动键盘 RGB**（UI 侧按投影做，
 * spec R4）：lights_lvl 只在 RAM 副本里；palette/zoom/theme 同理由 UI 消费副本。 */
static void settings_apply_locked(void)
{
    fx_lowcut = (uint8_t)(s_set.f.lowcut != 0u);
    flt_reso = (uint8_t)(s_set.f.flt_reso > SONG_RESO_MAX ? SONG_RESO_MAX : s_set.f.flt_reso);
    rec_tempo = (uint8_t)(s_set.f.rec_tempo != 0u);
    rec_count = (uint8_t)(s_set.f.rec_count != 0u);
    rec_quant_off = (uint8_t)(s_set.f.rec_quant_off != 0u);
}

/* 尾段默认布局 = 上游 `midi_part_fill()`（project.c:882-897）对本固件状态的输出。那个函数在
 * 本 TU **不编入**（project.c 以 PROJ_HOST 编，midi_part_* 在它的 `#ifndef PROJ_HOST` +
 * `#if FELUCCA_FLASH` 段里），故逐行镜像它，只取默认布局。紧随其后的 `midi_part_default()`
 * （project.c:898-908）**刻意不镜像**：上游用它决定「尾段非默认才写」，这里由
 * song_per_put_settings 按 `reso ≠ 0` 决定是否追加尾段（spec §8.1 的同语义简化）。
 *   - ml_tab（seq.c，本 TU 可见）照抄——本固件没有 MIDI LEARN 入口，恒为 0；
 *   - midi_set[] / drum_preview / step_adv_off / fxfade 都在 panel.c（不在本 TU），本固件
 *     没有任何写入口 ⇒ 恒为上游默认（MIDI_SET_DEF / 0），取字面量；
 *   - byte38（FLT RESO 位）由 song_per_put_settings 随后 RMW 打点，这里留 0。 */
static void settings_tail_fill(uint8_t *b)
{
    static const uint8_t midi_set[10] = SETTINGS_MIDI_DEF;
    for (uint32_t k = 0; k < ML_N; k++) {
        b[2u * k] = (uint8_t)ml_tab[k];
        b[2u * k + 1u] = (uint8_t)(ml_tab[k] >> 8);
    }
    b[2u * ML_N] = midi_set[0], b[2u * ML_N + 1u] = midi_set[1], b[2u * ML_N + 2u] = midi_set[2];
    b[2u * ML_N + 3u] = (uint8_t)(midi_set[3] | midi_set[4] << 7); /* DRUM PREVIEW/STEP ADVANCE = 0 */
    b[2u * ML_N + 4u] = midi_set[5]; /* BEND 低 4 位；FX FADE 高 4 位 = 0（上游默认） */
    b[2u * ML_N + 5u] = midi_set[6];
    b[2u * ML_N + 6u] = (uint8_t)(midi_set[7] | midi_set[9] << 1);
    b[2u * ML_N + 7u] = midi_set[8];
}

/* song_write_locked 专用（调用方持引擎锁）：只有「写成功」且「刚提交的字节就是当前 RAM」
 * 才清 settings dirty——擦写窗口里又来的 set 让 payload 过时，dirty 保留到下一个窗口
 * （与 song_per_saved 对 dsu_dirty 的记账同一口径）。 */
static void settings_written_locked(bool ok, const song_settings_t *written)
{
    if (ok && written != NULL && memcmp(written, &s_set.f, sizeof *written) == 0) s_set.dirty = false;
}

_Static_assert(SLOOP_SET_FIELD_COUNT == 6u, "sloop_set_field_t 增删字段时同步 settings_field_ptr");

static uint32_t *settings_field_ptr(sloop_set_field_t field)
{
    uint32_t *const p[SLOOP_SET_FIELD_COUNT] = {&s_set.f.palette,   &s_set.f.lowcut,  &s_set.f.zoom,
                                                &s_set.f.lights_lvl, &s_set.f.flt_reso, &s_set.f.theme_on};
    return field < SLOOP_SET_FIELD_COUNT ? p[field] : NULL;
}

static uint32_t settings_clamp(sloop_set_field_t field, uint32_t v)
{
    switch (field) {
    case SLOOP_SET_PALETTE: return v < SLOOP_SETTINGS_PALETTES ? v : SLOOP_SETTINGS_PALETTES - 1u;
    case SLOOP_SET_RESO: return v > SONG_RESO_MAX ? SONG_RESO_MAX : v;
    case SLOOP_SET_LIGHTS: return v > 3u ? 3u : v;
    case SLOOP_SET_LOWCUT:
    case SLOOP_SET_ZOOM:
    case SLOOP_SET_THEME_ON: return v != 0u ? 1u : 0u;
    default: return v;
    }
}

const sloop_settings_t *sloop_store_settings(void)
{
    return &s_set; /* 不取锁：字段是字对齐的单次读写；theme_name 只在 reload（开机/自检收尾）变 */
}

bool sloop_store_settings_dirty(void)
{
    return sloop_store_ready() && s_set.dirty;
}

void sloop_store_settings_dirty_mark(void)
{
    engine_lock(); /* 与 settings_written_locked 的 check+clear 同一把锁 */
    /* CMD_REC_CONFIG 直接改上游 static；保存前把三项回填到生效副本，否则 dirty 只会
     * 把旧的 0 再写一遍。 */
    s_set.f.rec_tempo = rec_tempo != 0u;
    s_set.f.rec_count = rec_count != 0u;
    s_set.f.rec_quant_off = rec_quant_off != 0u;
    s_set.dirty = true;
    engine_unlock();
}

esp_err_t sloop_store_settings_set(sloop_set_field_t field, uint32_t v)
{
    if (field >= SLOOP_SET_FIELD_COUNT) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    v = settings_clamp(field, v);
    engine_lock();
    if (s_frozen) { /* 取锁后复核（与其它写入口同口径） */
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    *settings_field_ptr(field) = v;
    settings_apply_locked();
    s_set.dirty = true;
    engine_unlock();
    s_input_ms = store_esp_ms(); /* 一次用户动作：延迟写的 idle 门限从这一刻起算 */
    return ESP_OK;
}

esp_err_t sloop_store_settings_save(void)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    esp_err_t err = song_write_locked();
    store_unlock();
    s_sections_tried_ms = store_esp_ms(); /* 尝试过就推进 gap（失败也按 5s 窗口重试） */
    if (err == ESP_OK) sloop_hal_settings_flush();
    return err;
}

/* 以 flash 为准重读并应用（开机与 `ENG SELFTEST` 收尾）。读不到（未就绪/没有对象/对象
 * 损坏）⇒ 全零 + 上游默认 palette（settings_init：MONO=4，lowcut/zoom=0）；palette 越界
 * 同样归 4。读 flash 期间来的 set 会被本次 reload 覆盖（与 song_reload 的代次守卫不同：
 * reload 只在开机/自检收尾这两个没有用户输入的时点调用）。总是 ESP_OK。 */
esp_err_t sloop_store_settings_reload(void)
{
    song_settings_t f;
    song_theme_view_t theme;
    bool from_flash = false, theme_ok = false, ready = sloop_store_ready();

    memset(&f, 0, sizeof f);
    memset(&theme, 0, sizeof theme);
    if (ready) {
        uint8_t side[SONG_SIDE_LEN];
        bool have_side = false;
        store_lock();
        int n = st_load(OBJ_SETTINGS, &proj_tmp.raw, sizeof proj_tmp.raw);
        from_flash = song_per_get_settings(proj_tmp.raw, n > 0 ? (uint32_t)n : 0u, &f);
        have_side = st_side_load(OBJ_SETTINGS, side, SONG_SIDE_LEN) == 0;
        store_unlock();
        theme_ok = have_side && song_per_side_theme(side, &theme);
    }
    /* 读不到（未就绪/没有对象/对象损坏）：全零 + 上游默认 palette（settings_init：MONO=4，
     * R13）——P4-E 之前的固件把 palette=0 写进对象，0 在这里不再是「没有设置」的含义；
     * palette 越界同样归 4。 */
    if (!from_flash) {
        memset(&f, 0, sizeof f);
        f.palette = SLOOP_SETTINGS_PALETTES - 1u;
    }
    if (f.palette >= SLOOP_SETTINGS_PALETTES) f.palette = SLOOP_SETTINGS_PALETTES - 1u;

    engine_lock();
    s_set.f = f;
    s_set.theme_valid = theme_ok;
    memcpy(s_set.theme_name, theme.name, sizeof s_set.theme_name);
    s_set.theme = theme;
    s_set.dirty = false;
    settings_apply_locked();
    settings_tail_fill(s_midi_def);
    engine_unlock();
    ESP_LOGI(TAG, "settings: %s palette=%u lowcut=%u zoom=%u lights=%u reso=%u rec=%u/%u/%u theme=%s",
             from_flash ? "flash" : "默认", (unsigned)f.palette, (unsigned)f.lowcut, (unsigned)f.zoom,
             (unsigned)f.lights_lvl, (unsigned)f.flt_reso, (unsigned)f.rec_tempo,
             (unsigned)f.rec_count, (unsigned)f.rec_quant_off, theme_ok ? s_set.theme_name : "none");
    return ESP_OK;
}
