/*
 * se_config_golden — writes the ATECC608B configuration vectors through the real
 * components/secure code, so tools/factory (Python) can check its mirror against bytes
 * the node computes:
 *
 *   make se-golden
 *     -> test/se_config_golden.h
 *     -> ../tools/factory/testdata/se_config_golden.json
 *
 * The "factory" zone is synthetic: the serial number, revision and slot layout of
 * cryptoauthlib's example ATECC608 configuration (app/pkcs11/example_pkcs11_config.c), with
 * both zones unlocked and no slot locked, as a blank chip reads. Every case states the step
 * it must produce; the program refuses to write anything if the code disagrees.
 */
#include "fx_se_config.h"

#include <stdio.h>
#include <string.h>

static const uint8_t FACTORY[FX_SE_CONFIG_SIZE] = {
    0x01, 0x23, 0x00, 0x00, 0x00, 0x00, 0x60, 0x02, 0x00, 0x00, 0x00, 0x00, 0xEE, 0x01, 0x01, 0x00,
    0xC0, 0x00, 0x00, 0x01, 0x8F, 0x20, 0xC4, 0x44, 0x87, 0x20, 0x87, 0x20, 0x8F, 0x0F, 0xC4, 0x36,
    0x9F, 0x0F, 0x82, 0x20, 0x0F, 0x0F, 0xC4, 0x44, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x33, 0x00, 0x1C, 0x00, 0x13, 0x00, 0x13, 0x00, 0x7C, 0x00, 0x1C, 0x00, 0x3C, 0x00, 0x33, 0x00,
    0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x30, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x30, 0x00,
};

typedef struct {
    const char *name;
    uint8_t config[FX_SE_CONFIG_SIZE];
    fx_se_step_t step;
} golden_case_t;

enum { CASES = 9 };

static void build(golden_case_t cases[CASES], uint8_t applied[FX_SE_CONFIG_SIZE])
{
    memcpy(applied, FACTORY, FX_SE_CONFIG_SIZE);
    fx_se_config_apply(applied);

    static const char *const names[CASES] = {
        "blank chip",
        "configuration written, not locked",
        "configuration locked",
        "data zone locked, no key",
        "slot 0 locked: provisioned",
        "configured by someone else",
        "data zone locked before configuration",
        "slot 0 marked locked in an unlocked configuration",
        "slot 0 locked before the data zone",
    };
    static const fx_se_step_t steps[CASES] = {
        FX_SE_STEP_WRITE_CONFIG, FX_SE_STEP_LOCK_CONFIG, FX_SE_STEP_LOCK_DATA,
        FX_SE_STEP_GENKEY_LEDGER, FX_SE_STEP_DONE, FX_SE_STEP_REFUSE,
        FX_SE_STEP_REFUSE, FX_SE_STEP_REFUSE, FX_SE_STEP_REFUSE,
    };
    for (int i = 0; i < CASES; i++) {
        cases[i].name = names[i];
        cases[i].step = steps[i];
    }
    memcpy(cases[0].config, FACTORY, FX_SE_CONFIG_SIZE);
    memcpy(cases[1].config, applied, FX_SE_CONFIG_SIZE);
    memcpy(cases[2].config, applied, FX_SE_CONFIG_SIZE);
    cases[2].config[FX_SE_LOCK_CONFIG_AT] = 0x00;
    memcpy(cases[3].config, cases[2].config, FX_SE_CONFIG_SIZE);
    cases[3].config[FX_SE_LOCK_VALUE_AT] = 0x00;
    memcpy(cases[4].config, cases[3].config, FX_SE_CONFIG_SIZE);
    cases[4].config[FX_SE_SLOT_LOCKED_AT] = 0xFE;
    memcpy(cases[5].config, FACTORY, FX_SE_CONFIG_SIZE);
    cases[5].config[FX_SE_LOCK_CONFIG_AT] = 0x00;
    memcpy(cases[6].config, applied, FX_SE_CONFIG_SIZE);
    cases[6].config[FX_SE_LOCK_VALUE_AT] = 0x00;
    memcpy(cases[7].config, applied, FX_SE_CONFIG_SIZE);
    cases[7].config[FX_SE_SLOT_LOCKED_AT] = 0xFE;
    memcpy(cases[8].config, cases[2].config, FX_SE_CONFIG_SIZE);
    cases[8].config[FX_SE_SLOT_LOCKED_AT] = 0xFE;
}

static void c_bytes(FILE *f, const uint8_t *b, size_t n, const char *indent)
{
    for (size_t i = 0; i < n; i++) {
        fprintf(f, "%s0x%02X,%s", (i % 16u) == 0u ? indent : "", b[i], (i % 16u) == 15u ? "\n" : " ");
    }
}

static void hex(FILE *f, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        fprintf(f, "%02x", b[i]);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: se_config_golden <header out> <json out>\n");
        return 2;
    }
    golden_case_t cases[CASES];
    uint8_t applied[FX_SE_CONFIG_SIZE];
    build(cases, applied);
    for (int i = 0; i < CASES; i++) {
        const fx_se_step_t got = fx_se_next_step(cases[i].config, NULL);
        if (got != cases[i].step) {
            fprintf(stderr, "case \"%s\": the code says %s, the vector says %s\n", cases[i].name,
                    fx_se_step_name(got), fx_se_step_name(cases[i].step));
            return 1;
        }
    }

    static const uint8_t INFO[] = {0x07, 0x30, 0x00, 0x00, 0x00};
    uint8_t ramp[FX_SE_CONFIG_SIZE];
    for (unsigned i = 0; i < FX_SE_CONFIG_SIZE; i++) {
        ramp[i] = (uint8_t)i;
    }
    const uint16_t crc_info = fx_se_crc16(INFO, sizeof(INFO));
    const uint16_t crc_ramp = fx_se_crc16(ramp, sizeof(ramp));
    const uint16_t crc_applied = fx_se_crc16(applied, sizeof(applied));

    FILE *h = fopen(argv[1], "w");
    if (!h) {
        perror(argv[1]);
        return 1;
    }
    fprintf(h, "/* Generated by firmware/sim/se_config_golden.c (make se-golden). Do not edit. */\n");
    fprintf(h, "#ifndef SE_CONFIG_GOLDEN_H\n#define SE_CONFIG_GOLDEN_H\n\n#include <stdint.h>\n\n#include \"fx_se_config.h\"\n\n");
    fprintf(h, "static const uint16_t GOLDEN_SE_CRC_INFO = 0x%04X; /* 07 30 00 00 00 */\n", crc_info);
    fprintf(h, "static const uint16_t GOLDEN_SE_CRC_RAMP = 0x%04X; /* 00 01 .. 7F */\n", crc_ramp);
    fprintf(h, "static const uint16_t GOLDEN_SE_APPLIED_CRC = 0x%04X;\n\n", crc_applied);
    fprintf(h, "static const uint8_t GOLDEN_SE_FACTORY[FX_SE_CONFIG_SIZE] = {\n");
    c_bytes(h, FACTORY, FX_SE_CONFIG_SIZE, "    ");
    fprintf(h, "};\n\nstatic const uint8_t GOLDEN_SE_APPLIED[FX_SE_CONFIG_SIZE] = {\n");
    c_bytes(h, applied, FX_SE_CONFIG_SIZE, "    ");
    fprintf(h, "};\n\ntypedef struct {\n    const char *name;\n    uint8_t config[FX_SE_CONFIG_SIZE];\n    fx_se_step_t step;\n} golden_se_case_t;\n\n");
    fprintf(h, "static const golden_se_case_t GOLDEN_SE_CASES[] = {\n");
    for (int i = 0; i < CASES; i++) {
        fprintf(h, "    {\"%s\",\n     {\n", cases[i].name);
        c_bytes(h, cases[i].config, FX_SE_CONFIG_SIZE, "      ");
        fprintf(h, "     },\n     %d /* %s */},\n", (int)cases[i].step, fx_se_step_name(cases[i].step));
    }
    fprintf(h, "};\n\n#endif /* SE_CONFIG_GOLDEN_H */\n");
    fclose(h);

    FILE *j = fopen(argv[2], "w");
    if (!j) {
        perror(argv[2]);
        return 1;
    }
    fprintf(j, "{\n  \"schema\": 1,\n  \"generator\": \"firmware/sim/se_config_golden.c\",\n");
    fprintf(j, "  \"slot_config\": %u,\n  \"key_config\": %u,\n", FX_SE_SLOT_CONFIG, FX_SE_KEY_CONFIG);
    fprintf(j, "  \"crc_vectors\": [\n    {\"data\": \"");
    hex(j, INFO, sizeof(INFO));
    fprintf(j, "\", \"crc16\": %u, \"crc_le\": \"%02x%02x\"},\n    {\"data\": \"", crc_info, crc_info & 0xFFu, crc_info >> 8);
    hex(j, ramp, sizeof(ramp));
    fprintf(j, "\", \"crc16\": %u, \"crc_le\": \"%02x%02x\"}\n  ],\n", crc_ramp, crc_ramp & 0xFFu, crc_ramp >> 8);
    fprintf(j, "  \"factory_config\": \"");
    hex(j, FACTORY, FX_SE_CONFIG_SIZE);
    fprintf(j, "\",\n  \"applied_config\": \"");
    hex(j, applied, FX_SE_CONFIG_SIZE);
    fprintf(j, "\",\n  \"applied_config_crc16\": %u,\n  \"applied_config_crc_le\": \"%02x%02x\",\n", crc_applied,
            crc_applied & 0xFFu, crc_applied >> 8);
    fprintf(j, "  \"cases\": [\n");
    for (int i = 0; i < CASES; i++) {
        fprintf(j, "    {\"name\": \"%s\", \"step\": \"%s\", \"config\": \"", cases[i].name, fx_se_step_name(cases[i].step));
        hex(j, cases[i].config, FX_SE_CONFIG_SIZE);
        fprintf(j, "\"}%s\n", i + 1 < CASES ? "," : "");
    }
    fprintf(j, "  ]\n}\n");
    fclose(j);
    return 0;
}
