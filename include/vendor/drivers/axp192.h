#ifndef AXP192_H
#define AXP192_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AXP192_I2C_ADDR 0x34

/**
 * @brief Bring up the AXP192 rails the board needs before anything else runs.
 *
 * On the M5Stack Core2 (and the Core2 for AWS, which shares the same power
 * tree) nothing on the front of the device is powered directly by the ESP32:
 *
 *   - LDO2  -> LCD logic + TF card slot (3.3V)
 *   - LDO3  -> vibration motor (left off here)
 *   - DCDC3 -> LCD backlight
 *   - GPIO4 -> LCD reset
 *   - GPIO1 -> status LED (open drain, active low)
 *   - GPIO2 -> speaker enable
 *
 * So this must run *before* the LVGL panel driver probes the ILI9342C,
 * otherwise the panel is unpowered and still held in reset and the display
 * stays dark with no error.
 *
 * The AXP192 shares the internal I2C bus with the FT6336U touch controller,
 * the BM8563 RTC and the MPU6886, so the port must already be initialized
 * (lvgl_i2c_init) when this is called.
 *
 * @return ESP_OK on success, an error code otherwise.
 */
esp_err_t axp192_init(void);

/**
 * @brief Set the LCD backlight level (0-100%).
 *
 * The backlight is the DCDC3 rail, not a PWM'd GPIO. Levels map onto
 * 2.50V-3.30V, which is the usable range for the Core2 panel; 0% switches the
 * rail off entirely.
 */
esp_err_t axp192_set_backlight(uint8_t percentage);

/**
 * @brief Approximate battery charge level (0-100%).
 *
 * The AXP192 has no fuel gauge, only a battery voltage ADC, so this is a
 * linear estimate across the 3.30V-4.20V discharge window.
 */
esp_err_t axp192_get_power_level(uint8_t *percentage);

/** @brief True while the charger is actually charging the pack. */
bool axp192_is_charging(void);

/** @brief True when a battery is attached to the AXP192. */
bool axp192_is_battery_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* AXP192_H */
