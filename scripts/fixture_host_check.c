/* SPDX-License-Identifier: GPL-3.0-only
 * fixture_host_check.c — 主机侧交叉校验：用 vendored project.c 的 proj_import 导入
 * gen_project_fixtures.py 生成的旧格式 fixture，并按设备 FIELDS 的同一投影打印字段。
 *
 * 目的（P4-B Task 9）：把「Python 期望值模型 vs 上游 C 语义」的差异在主机上暴露，
 * 不必占设备轮次。编译纪律与 tests/project_test.c 相同：hostsim.c + PROJ_HOST 的
 * project.c，-I build/gen -I firmware/src。由 `gen_project_fixtures.py --host-check`
 * 编译/运行（需要先跑 scripts/host-test.sh 生成 build/gen 的头）。
 *
 * 用法：fixture_host_check <fun5.bin|fun4.bin|fun1.bin>
 * 输出：一行 key=value（与 console 的 `STORE PROJ FIELDS` 同序；见
 * gen_project_fixtures.py 的 field_keys()）。
 */
#define main hostsim_main
#include <stddef.h>
#include "../third_party/sloop-fm1/tests/hostsim.c"
#undef main
#define PROJ_HOST 1
/* ui.c:581 的 TRK_DEF（ANALOG / DIGITAL / SAMPLE）：格式 1 的第 2..4 轨用它 */
static uint32_t trk_def_engine(uint32_t i)
{
    static const uint8_t E[NPART] = {0, 1, 4};
    return i < NPART ? E[i] : 0u;
}
#include "../third_party/sloop-fm1/firmware/src/project.c"

int main(int argc, char **argv)
{
    static project_t q;
    static union {
        project_t v6;
        project_v5_t v5;
        project_v4_t v4;
        project_v3_t v3;
        project_v2_t v2;
        project_v1_t v1;
    } buf;
    FILE *f;
    int n;
    uint32_t magic, fmt;
    if (argc != 2) {
        fprintf(stderr, "用法: %s <fixture.bin>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    n = (int)fread(&buf, 1, sizeof buf, f);
    fclose(f);
    magic = ((const uint32_t *)&buf)[0];
    fmt = magic == PROJ_MAGIC ? 6u : magic == PROJ_MAGIC_V5 ? 5u : magic == PROJ_MAGIC_V4 ? 4u :
          magic == PROJ_MAGIC_V3 ? 3u : magic == PROJ_MAGIC_V2 ? 2u : magic == PROJ_MAGIC_V1 ? 1u : 0u;
    if (!proj_import(&q, &buf, n)) {
        fprintf(stderr, "%s: proj_import 拒绝（%d 字节，magic=0x%08lX）\n", argv[1], n,
                (unsigned long)magic);
        return 1;
    }
    printf("fmt=%lu sel=%u bpm=%d swing=%d dust=%d drdly=%u", (unsigned long)fmt, (unsigned)q.sel,
           (int)q.g[0], (int)q.g[1], (int)q.g[27], (unsigned)q.drdly);
    for (uint32_t k = 0; k < NTRK; k++) {
        const proj_trk_t *t = &q.t[k];
        printf(" t%lu.eng=%u t%lu.pre=%u t%lu.lvl=%d t%lu.atk=%d t%lu.e0=%d t%lu.e1=%d",
               (unsigned long)k, (unsigned)t->engine, (unsigned long)k, (unsigned)t->preset,
               (unsigned long)k, (int)pt_get(t, P_LEVEL), (unsigned long)k, (int)pt_get(t, P_ATK),
               (unsigned long)k, (int)t->e[0], (unsigned long)k, (int)t->e[1]);
        for (uint32_t i = 0; i < 8u; i++)
            if (k == TRK_DRUM) {
                uint32_t m = (uint32_t)t->dstep[i].on[0] | (uint32_t)t->dstep[i].on[1] << 8;
                printf(" t%lu.s%lu=%u:%lu", (unsigned long)k, (unsigned long)i,
                       (unsigned)__builtin_popcount(m), (unsigned long)m);
            } else {
                printf(" t%lu.s%lu=%u:%u", (unsigned long)k, (unsigned long)i,
                       (unsigned)t->step[i].n, (unsigned)t->step[i].note[0]);
            }
    }
    printf("\n");
    return 0;
}
