/* SPDX-License-Identifier: GPL-3.0-only
 * P4-D Task 1: Tab5 DSYN / FM6 patch / 采样槽适配的同源 host 门禁骨架。
 * hostsim.c 提供真实上游引擎/音序/鼓；project.c(PROJ_HOST) 是 P4-B 起的同源格式层。
 * Task 2 起在 project.c 之后 include sound_tab5.c 并写失败测试；本任务只有编译骨架，
 * 保证 Task 1 与 Task 2 之间的分支始终可运行（红测由 Task 2 引入）。
 */
#define main hostsim_main
#include "../third_party/sloop-fm1/tests/hostsim.c"
#undef main

#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i) { return i < NPART ? i : 0; }
#include "../third_party/sloop-fm1/firmware/src/project.c"
#undef PROJ_HOST

int main(void)
{
    printf("SYNTH HOST TEST PENDING\n");
    return 0;
}
