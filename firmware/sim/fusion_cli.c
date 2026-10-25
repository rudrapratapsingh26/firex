/*
 * fusion_cli — a line protocol around the fusion engine.
 *
 * The simulator in tools/simulator drives the real C engine through this
 * harness rather than loading a shared library, so a 32-bit toolchain and a
 * 64-bit Python still work together, and CI needs no ABI glue.
 *
 *   build:  gcc -Icomponents/fusion/include components/fusion/fusion.c \
 *               sim/fusion_cli.c -o build/fusion_cli
 *
 *   stdin:  config <enter> <exit> <elevated_ms> <critical_ms> <flame_alone>
 *                  <flame_score> <flame_ms> <warmup_ms> <rh> <heat_quiet> <gas_suppress> [<mos_pair>]
 *                  (mos_pair, 0 or 1, is optional and defaults to 1)
 *           step <t_ms> <s0> <v0> ... <s4> <v4>
 *   stdout: <state> <changed> <fault> <voters> <voter_count> <a> <b> <eff0..eff4>
 */
#include "fusion.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    fx_engine_t engine;
    fx_config_t cfg;
    fx_config_defaults(&cfg);
    fx_init(&engine, &cfg, 0);

    char line[512];
    /* Every reply is flushed explicitly: the Windows C runtime downgrades line
     * buffering to full buffering on a pipe, which would deadlock the caller. */

    while (fgets(line, sizeof(line), stdin)) {
        if (strncmp(line, "config", 6) == 0) {
            int flame_alone = 1, mos_pair = 1;
            unsigned elevated_ms = 0, critical_ms = 0, flame_ms = 0, warmup_ms = 0;
            const int got = sscanf(line + 6, "%f %f %u %u %d %f %u %u %f %f %f %d",
                                   &cfg.enter, &cfg.exit, &elevated_ms, &critical_ms, &flame_alone,
                                   &cfg.flame_alone_score, &flame_ms, &warmup_ms,
                                   &cfg.rh_context, &cfg.heat_quiet, &cfg.gas_suppress, &mos_pair);
            if (got == 11 || got == 12) {
                cfg.elevated_ms = elevated_ms;
                cfg.critical_ms = critical_ms;
                cfg.flame_alone_ms = flame_ms;
                cfg.warmup_ms = warmup_ms;
                cfg.flame_alone = flame_alone ? true : false;
                cfg.mos_pair = mos_pair ? true : false;
                fx_init(&engine, &cfg, 0);
                printf("ok\n"); fflush(stdout);
            } else {
                printf("err config\n"); fflush(stdout);
            }
            continue;
        }

        if (strncmp(line, "step", 4) == 0) {
            unsigned t_ms = 0;
            float s[FX_MODALITY_COUNT];
            int v[FX_MODALITY_COUNT];
            if (sscanf(line + 4, "%u %f %d %f %d %f %d %f %d %f %d",
                       &t_ms, &s[0], &v[0], &s[1], &v[1], &s[2], &v[2],
                       &s[3], &v[3], &s[4], &v[4]) != 11) {
                printf("err step\n"); fflush(stdout);
                continue;
            }
            fx_sample_t samples[FX_MODALITY_COUNT];
            for (int i = 0; i < FX_MODALITY_COUNT; i++) {
                samples[i].score = s[i];
                samples[i].valid = v[i] ? true : false;
            }
            fx_result_t r = fx_update(&engine, samples, (uint32_t)t_ms);
            printf("%d %d %d %u %u %d %d %.4f %.4f %.4f %.4f %.4f\n",
                   (int)r.state, r.state_changed ? 1 : 0, r.fault ? 1 : 0,
                   (unsigned)r.voters, (unsigned)r.voter_count,
                   (int)r.reason_a, (int)r.reason_b,
                   r.effective[0], r.effective[1], r.effective[2], r.effective[3], r.effective[4]);
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "quit", 4) == 0) {
            break;
        }
    }
    return 0;
}
