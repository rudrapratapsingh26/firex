/*
 * ADS1115 over I2C (ESP-IDF i2c_master). The MQ-2 and MQ-135 drivers share one
 * converter from separate sampling tasks, so the write-config, wait,
 * read-conversion sequence is serialised by a mutex.
 */
#ifndef FX_ADS1115_H
#define FX_ADS1115_H

#include "ads1115_reg.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t lock;
    fx_ads1115_pga_t pga;
    fx_ads1115_dr_t dr;
} fx_ads1115_t;

esp_err_t fx_ads1115_open(i2c_master_bus_handle_t bus, uint8_t addr, fx_ads1115_pga_t pga,
                          fx_ads1115_dr_t dr, fx_ads1115_t *out);

/* One single-shot conversion on AIN0-3, in volts at the pin. */
esp_err_t fx_ads1115_read_volts(fx_ads1115_t *adc, uint8_t channel, float *volts);

#ifdef __cplusplus
}
#endif
#endif /* FX_ADS1115_H */
