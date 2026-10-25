#include "fx_ads1115.h"

#include "esp_check.h"
#include "freertos/task.h"

#define I2C_TIMEOUT_MS 50
#define READY_POLLS 5

static const char *TAG = "ads1115";

esp_err_t fx_ads1115_open(i2c_master_bus_handle_t bus, uint8_t addr, fx_ads1115_pga_t pga,
                          fx_ads1115_dr_t dr, fx_ads1115_t *out)
{
    ESP_RETURN_ON_FALSE(bus && out, ESP_ERR_INVALID_ARG, TAG, "bus and out are required");
    *out = (fx_ads1115_t){.pga = pga, .dr = dr};

    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, addr, I2C_TIMEOUT_MS), TAG,
                        "no ADS1115 answering at 0x%02x", addr);

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &out->dev), TAG, "add device");

    out->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(out->lock, ESP_ERR_NO_MEM, TAG, "mutex");
    return ESP_OK;
}

static esp_err_t read_register(fx_ads1115_t *adc, uint8_t reg, uint16_t *value)
{
    uint8_t rx[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(adc->dev, &reg, 1, rx, sizeof(rx), I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *value = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

esp_err_t fx_ads1115_read_volts(fx_ads1115_t *adc, uint8_t channel, float *volts)
{
    if (!adc || !adc->dev || !adc->lock || !volts) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t config = fx_ads1115_single_shot_config(channel, adc->pga, adc->dr);
    if (config == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(adc->lock, portMAX_DELAY);

    const uint8_t start[3] = {FX_ADS1115_REG_CONFIG, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF)};
    esp_err_t err = i2c_master_transmit(adc->dev, start, sizeof(start), I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(fx_ads1115_conversion_ms(adc->dr)));
        err = ESP_ERR_TIMEOUT;
        for (int i = 0; i < READY_POLLS; i++) {
            uint16_t reg = 0;
            const esp_err_t rd = read_register(adc, FX_ADS1115_REG_CONFIG, &reg);
            if (rd != ESP_OK) {
                err = rd;
                break;
            }
            if (fx_ads1115_conversion_ready(reg)) {
                err = ESP_OK;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    if (err == ESP_OK) {
        uint16_t raw = 0;
        err = read_register(adc, FX_ADS1115_REG_CONVERSION, &raw);
        if (err == ESP_OK) {
            *volts = fx_ads1115_volts((int16_t)raw, adc->pga);
        }
    }

    xSemaphoreGive(adc->lock);
    return err;
}
