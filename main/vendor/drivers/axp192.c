#include "vendor/drivers/axp192.h"

#ifdef CONFIG_HAS_AXP192

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_i2c/i2c_manager.h"

static const char *TAG = "AXP192";

/* The PMU sits on the same internal bus as the touch controller, so reuse the
 * port the LVGL touch driver already configures rather than opening a second
 * master on the same pins. */
#ifdef CONFIG_LV_I2C_TOUCH_PORT
#define AXP192_I2C_PORT CONFIG_LV_I2C_TOUCH_PORT
#else
#define AXP192_I2C_PORT 0
#endif

/* AXP192 register map (datasheet v1.1) -- only what the Core2 needs. */
#define AXP192_REG_POWER_STATUS   0x00 /* bit5 = VBUS present */
#define AXP192_REG_CHARGE_STATUS  0x01 /* bit6 = charging, bit5 = battery present */
#define AXP192_REG_DCDC_LDO_EN    0x12 /* bit6 EXTEN, bit3 LDO3, bit2 LDO2, bit1 DCDC3, bit0 DCDC1 */
#define AXP192_REG_DCDC1_VOLT     0x26
#define AXP192_REG_DCDC3_VOLT     0x27 /* LCD backlight rail */
#define AXP192_REG_LDO23_VOLT     0x28 /* high nibble LDO2, low nibble LDO3 */
#define AXP192_REG_VBUS_IPSOUT    0x30
#define AXP192_REG_CHARGE_CTRL1   0x33
#define AXP192_REG_ADC_EN1        0x82
#define AXP192_REG_GPIO1_CTRL     0x92
#define AXP192_REG_GPIO2_CTRL     0x93
#define AXP192_REG_GPIO34_CTRL    0x95
#define AXP192_REG_GPIO34_OUT     0x96
#define AXP192_REG_BAT_VOLT_H     0x78 /* 12-bit, 1.1 mV/LSB */

#define AXP192_EN_EXTEN           (1 << 6)
#define AXP192_EN_LDO3            (1 << 3)
#define AXP192_EN_LDO2            (1 << 2)
#define AXP192_EN_DCDC3           (1 << 1)
#define AXP192_EN_DCDC1           (1 << 0)

#define AXP192_GPIO4_OUT_BIT      (1 << 1) /* reg 0x96 bit1 = GPIO4 level */

/* DCDC1/DCDC3 encode 0.7V-3.5V in 25 mV steps. */
#define AXP192_DCDC_MV(mv) ((uint8_t)(((mv) - 700) / 25))

/* Backlight rail range. Below ~2.5V the panel is effectively dark, above
 * 3.3V is out of spec for the Core2 LED string. */
#define AXP192_BACKLIGHT_MIN_MV 2500
#define AXP192_BACKLIGHT_MAX_MV 3300

/* Linear voltage window used to estimate charge level. */
#define AXP192_BATT_EMPTY_MV 3300
#define AXP192_BATT_FULL_MV  4200

static bool s_initialized = false;

static esp_err_t axp192_read_reg(uint8_t reg, uint8_t *value) {
  return lvgl_i2c_read(AXP192_I2C_PORT, AXP192_I2C_ADDR, reg, value, 1);
}

static esp_err_t axp192_write_reg(uint8_t reg, uint8_t value) {
  return lvgl_i2c_write(AXP192_I2C_PORT, AXP192_I2C_ADDR, reg, &value, 1);
}

/* Read-modify-write so unrelated rails in a shared control register are not
 * clobbered -- reg 0x12 in particular also gates the ESP32's own DCDC1. */
static esp_err_t axp192_update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask) {
  uint8_t current = 0;
  esp_err_t err = axp192_read_reg(reg, &current);
  if (err != ESP_OK) return err;
  uint8_t updated = (uint8_t)((current & (uint8_t)~clear_mask) | set_mask);
  if (updated == current) return ESP_OK;
  return axp192_write_reg(reg, updated);
}

esp_err_t axp192_init(void) {
  if (s_initialized) return ESP_OK;

  /* Probe first: a missing or different PMU should fail loudly here rather
   * than leave every later write silently timing out. */
  uint8_t status = 0;
  esp_err_t err = axp192_read_reg(AXP192_REG_POWER_STATUS, &status);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AXP192 not responding at 0x%02X on I2C port %d: %s",
             AXP192_I2C_ADDR, (int)AXP192_I2C_PORT, esp_err_to_name(err));
    return err;
  }

  /* VBUS is not current-limited by the PMU on this board (the USB port feeds
   * the boost converter directly), and VBUS must not be used as an input
   * source when the bus is being driven by the board itself. */
  err = axp192_write_reg(AXP192_REG_VBUS_IPSOUT, 0x80);

  /* GPIO1 (status LED) and GPIO2 (speaker enable) as NMOS open-drain outputs,
   * both left high-impedance (LED off, speaker muted) until something asks. */
  if (err == ESP_OK) err = axp192_write_reg(AXP192_REG_GPIO1_CTRL, 0x00);
  if (err == ESP_OK) err = axp192_write_reg(AXP192_REG_GPIO2_CTRL, 0x00);

  /* GPIO4 drives the panel's reset line. Configure GPIO3/4 as outputs while
   * preserving the GPIO3 bits (mask 0x72 matches M5GFX's power-on sequence). */
  if (err == ESP_OK) err = axp192_update_reg(AXP192_REG_GPIO34_CTRL, 0x8D, 0x84);

  /* LDO2 = 3.3V (LCD logic + TF card), LDO3 = 1.8V (vibration motor, left off).
   * Both nibbles step 100 mV from 1.8V, so 0xF0 == LDO2 3.3V / LDO3 1.8V. */
  if (err == ESP_OK) err = axp192_write_reg(AXP192_REG_LDO23_VOLT, 0xF0);
  /* EXTEN gates the 5V boost that feeds the M-Bus, and with it the M5GO
   * bottom base -- including its 10 SK6812 LEDs on G25, which accept data and
   * report success while staying dark when the base has no power.
   *
   * Confirmed on hardware: with this bit set the bar lights, without it every
   * rgbmode command reports success into the dark. Note that M5GFX is no guide
   * here -- its Core2 table only ORs in LDO2 (`0x12, 0x04, 0xFF`) and leaves
   * EXTEN alone, because it is a display library with no interest in the base.
   * The 0x12 = 0x4D write that does set EXTEN belongs to Light_M5StickC, a
   * different product. */
  if (err == ESP_OK) {
    err = axp192_update_reg(AXP192_REG_DCDC_LDO_EN, AXP192_EN_LDO3,
                            AXP192_EN_LDO2 | AXP192_EN_EXTEN);
  }

  /* Pulse the panel reset now that its logic rail is up. */
  if (err == ESP_OK) err = axp192_update_reg(AXP192_REG_GPIO34_OUT, AXP192_GPIO4_OUT_BIT, 0x00);
  if (err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(20));
    err = axp192_update_reg(AXP192_REG_GPIO34_OUT, 0x00, AXP192_GPIO4_OUT_BIT);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  /* Enable the battery/VBUS ADCs so the status bar has something to read, and
   * charge at 4.2V / 100 mA, which is what the Core2 pack expects. */
  if (err == ESP_OK) err = axp192_write_reg(AXP192_REG_ADC_EN1, 0xFF);
  if (err == ESP_OK) err = axp192_write_reg(AXP192_REG_CHARGE_CTRL1, 0xC0);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AXP192 power-on sequence failed: %s", esp_err_to_name(err));
    return err;
  }

  /* Leave the backlight rail OFF. This runs before lvgl_driver_init(), so the
   * panel is powered and out of reset but has no controller init and no frame
   * yet -- lighting it here shows garbage at full brightness. display_manager
   * keeps the panel dark on purpose (display_manager.c, "Keep the panel dark
   * until the first real view has been drawn") and main.c turns the backlight
   * on via set_backlight_brightness() once the startup view's first refresh
   * completes, which also applies the user's maximum-brightness setting.
   * Disable explicitly rather than just skipping: the rail may still be on
   * from a previous boot. */
  esp_err_t bl_err = axp192_update_reg(AXP192_REG_DCDC_LDO_EN, AXP192_EN_DCDC3, 0x00);
  if (bl_err != ESP_OK) {
    ESP_LOGW(TAG, "Could not park the backlight rail off: %s", esp_err_to_name(bl_err));
  }

  s_initialized = true;
  ESP_LOGI(TAG, "AXP192 initialized (LDO2 3.3V panel/TF rail, panel out of reset, backlight off)");
  return ESP_OK;
}

esp_err_t axp192_set_backlight(uint8_t percentage) {
  if (percentage > 100) percentage = 100;

  if (percentage == 0) {
    return axp192_update_reg(AXP192_REG_DCDC_LDO_EN, AXP192_EN_DCDC3, 0x00);
  }

  uint32_t millivolts = AXP192_BACKLIGHT_MIN_MV +
                        ((AXP192_BACKLIGHT_MAX_MV - AXP192_BACKLIGHT_MIN_MV) * percentage) / 100;
  esp_err_t err = axp192_write_reg(AXP192_REG_DCDC3_VOLT, AXP192_DCDC_MV(millivolts));
  if (err != ESP_OK) return err;
  return axp192_update_reg(AXP192_REG_DCDC_LDO_EN, 0x00, AXP192_EN_DCDC3);
}

esp_err_t axp192_get_power_level(uint8_t *percentage) {
  if (percentage == NULL) return ESP_ERR_INVALID_ARG;
  if (!s_initialized) return ESP_ERR_INVALID_STATE;

  /* 12-bit result split across 0x78 (high 8) and 0x79 (low 4). */
  uint8_t raw[2] = {0, 0};
  esp_err_t err = lvgl_i2c_read(AXP192_I2C_PORT, AXP192_I2C_ADDR,
                                AXP192_REG_BAT_VOLT_H, raw, sizeof(raw));
  if (err != ESP_OK) return err;

  uint32_t counts = ((uint32_t)raw[0] << 4) | (raw[1] & 0x0F);
  uint32_t millivolts = (counts * 11) / 10; /* 1.1 mV per LSB */

  if (millivolts <= AXP192_BATT_EMPTY_MV) {
    *percentage = 0;
  } else if (millivolts >= AXP192_BATT_FULL_MV) {
    *percentage = 100;
  } else {
    *percentage = (uint8_t)(((millivolts - AXP192_BATT_EMPTY_MV) * 100) /
                            (AXP192_BATT_FULL_MV - AXP192_BATT_EMPTY_MV));
  }
  return ESP_OK;
}

bool axp192_is_charging(void) {
  if (!s_initialized) return false;
  uint8_t status = 0;
  if (axp192_read_reg(AXP192_REG_CHARGE_STATUS, &status) != ESP_OK) return false;
  return (status & (1 << 6)) != 0;
}

bool axp192_is_battery_connected(void) {
  if (!s_initialized) return false;
  uint8_t status = 0;
  if (axp192_read_reg(AXP192_REG_CHARGE_STATUS, &status) != ESP_OK) return false;
  return (status & (1 << 5)) != 0;
}

#else /* !CONFIG_HAS_AXP192 */

/* main/CMakeLists.txt globs every source under main/, so keep a definition
 * here to avoid an empty translation unit on boards without an AXP192. */
esp_err_t axp192_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t axp192_set_backlight(uint8_t percentage) {
  (void)percentage;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t axp192_get_power_level(uint8_t *percentage) {
  (void)percentage;
  return ESP_ERR_NOT_SUPPORTED;
}
bool axp192_is_charging(void) { return false; }
bool axp192_is_battery_connected(void) { return false; }

#endif /* CONFIG_HAS_AXP192 */
