/*
 * ADS1115 register arithmetic. Pure C: the I2C transport lives in hw/ads1115.c,
 * so the config word and the count-to-volts conversion are tested on a laptop.
 */
#ifndef FX_ADS1115_REG_H
#define FX_ADS1115_REG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FX_ADS1115_ADDR_DEFAULT 0x48
#define FX_ADS1115_REG_CONVERSION 0x00
#define FX_ADS1115_REG_CONFIG 0x01

/* Programmable gain: the full-scale range, not the supply. Inputs must still
 * stay inside VDD + 0.3 V whatever the gain says. */
typedef enum {
    FX_ADS_PGA_6V144 = 0,
    FX_ADS_PGA_4V096 = 1,
    FX_ADS_PGA_2V048 = 2,
    FX_ADS_PGA_1V024 = 3,
    FX_ADS_PGA_0V512 = 4,
    FX_ADS_PGA_0V256 = 5
} fx_ads1115_pga_t;

typedef enum {
    FX_ADS_DR_8 = 0,
    FX_ADS_DR_16 = 1,
    FX_ADS_DR_32 = 2,
    FX_ADS_DR_64 = 3,
    FX_ADS_DR_128 = 4,
    FX_ADS_DR_250 = 5,
    FX_ADS_DR_475 = 6,
    FX_ADS_DR_860 = 7
} fx_ads1115_dr_t;

/* Config word that starts one single-ended, single-shot conversion on AIN0-3
 * with the comparator disabled. Returns 0 for a channel above 3. */
uint16_t fx_ads1115_single_shot_config(uint8_t channel, fx_ads1115_pga_t pga, fx_ads1115_dr_t dr);

/* True once the OS bit reads back as 1: the conversion has finished. */
bool fx_ads1115_conversion_ready(uint16_t config_reg);

/* Worst-case wait for one conversion at this data rate, with a 1 ms margin. */
uint32_t fx_ads1115_conversion_ms(fx_ads1115_dr_t dr);

float fx_ads1115_full_scale(fx_ads1115_pga_t pga);
float fx_ads1115_volts(int16_t raw, fx_ads1115_pga_t pga);

#ifdef __cplusplus
}
#endif
#endif /* FX_ADS1115_REG_H */
