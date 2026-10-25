#include "ads1115_reg.h"

static const float FULL_SCALE[] = {6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f};
static const uint16_t RATE_SPS[] = {8u, 16u, 32u, 64u, 128u, 250u, 475u, 860u};

uint16_t fx_ads1115_single_shot_config(uint8_t channel, fx_ads1115_pga_t pga, fx_ads1115_dr_t dr)
{
    if (channel > 3u || (unsigned)pga > 5u || (unsigned)dr > 7u) {
        return 0u;
    }
    uint16_t w = 0x8000u;                        /* OS: start a conversion */
    w |= (uint16_t)((4u + channel) << 12);      /* MUX: AINx against GND */
    w |= (uint16_t)((unsigned)pga << 9);        /* PGA */
    w |= 0x0100u;                                /* MODE: single-shot */
    w |= (uint16_t)((unsigned)dr << 5);         /* DR */
    w |= 0x0003u;                                /* COMP_QUE: comparator off */
    return w;
}

bool fx_ads1115_conversion_ready(uint16_t config_reg)
{
    return (config_reg & 0x8000u) != 0u;
}

uint32_t fx_ads1115_conversion_ms(fx_ads1115_dr_t dr)
{
    if ((unsigned)dr > 7u) {
        return 0u;
    }
    const uint32_t sps = RATE_SPS[dr];
    return (1000u + sps - 1u) / sps + 1u;
}

float fx_ads1115_full_scale(fx_ads1115_pga_t pga)
{
    return (unsigned)pga > 5u ? 0.0f : FULL_SCALE[pga];
}

float fx_ads1115_volts(int16_t raw, fx_ads1115_pga_t pga)
{
    return (float)raw * fx_ads1115_full_scale(pga) / 32768.0f;
}
