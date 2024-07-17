// Copyright 2022 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <math.h>
#include <string.h>
#include <esp_log.h>
#include <app/util/util.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <lib/support/logging/CHIPLogging.h>

#include <lightbulb.h>

#include <light_driver.h>

#define PWM_RED_PIN 10
#define PWM_GREEN_PIN 11
#define PWM_BLUE_PIN 12

using namespace chip;
using namespace chip::Inet;
using namespace chip::System;
using namespace chip::app::Clusters;

extern uint16_t light_endpoint;

typedef struct {
    bool switch_fade;   /* Whether to enable the fade when the switch state changes. */
    bool color_fade;    /* Whether to enable the fade when the color/brightness state changes.*/
} __attribute__((packed)) light_config_t;

static const char *TAG = "light_driver";

static light_config_t light_config = {
    .switch_fade = false,
    .color_fade = true,
};

static esp_err_t light_driver_set_power(bool power)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Set power: %d", power);

    lightbulb_status_t lightbulb_status;
    err |= lightbulb_get_all_detail(&lightbulb_status);
    err |= lightbulb_set_fades_function(light_config.switch_fade);
    lightbulb_status.on = power;
    err |= lightbulb_update_status(&lightbulb_status, true);

    return err;
}

static esp_err_t light_driver_set_brightness(uint16_t level)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Set brightness: %d", level);

    lightbulb_status_t lightbulb_status;
    err |= lightbulb_get_all_detail(&lightbulb_status);
    err |= lightbulb_set_fades_function(light_config.color_fade);
    if (lightbulb_status.mode == WORK_COLOR) {
        lightbulb_status.value = level;
    } else if (lightbulb_status.mode == WORK_WHITE) {
        lightbulb_status.brightness = level;
    } else {
        lightbulb_status.brightness = level;
    }
    err |= lightbulb_update_status(&lightbulb_status, true);

    return err;
}

static esp_err_t light_driver_set_hue(uint16_t hue)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Set hue %d", hue);

    lightbulb_status_t lightbulb_status;
    err |= lightbulb_get_all_detail(&lightbulb_status);
    err |= lightbulb_set_fades_function(light_config.color_fade);
    lightbulb_status.hue = hue;
    err |= lightbulb_update_status(&lightbulb_status, true);

    return err;
}

static esp_err_t light_driver_set_saturation(uint16_t saturation)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Set saturation %d", saturation);

    lightbulb_status_t lightbulb_status;
    err |= lightbulb_get_all_detail(&lightbulb_status);
    err |= lightbulb_set_fades_function(light_config.color_fade);
    lightbulb_status.saturation = saturation;
    err |= lightbulb_update_status(&lightbulb_status, true);

    return err;
}

static esp_err_t light_driver_set_xyy(float x, float y, float level)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Set xyy %f, %f, %f", x, y, level);

    err = lightbulb_set_xyy(x, y, level);
    uint8_t r, g, b, s, v = 0;
    uint16_t h = 0;
    lightbulb_status_t lightbulb_status;

    lightbulb_xyy2rgb(x, y, level, &r, &g, &b);
    lightbulb_rgb2hsv(r, g, b, &h, &s, &v);

    err |= lightbulb_get_all_detail(&lightbulb_status);
    err |= lightbulb_set_fades_function(light_config.color_fade);
    lightbulb_status.mode = WORK_COLOR;
    lightbulb_status.hue = h;
    lightbulb_status.saturation = s;
    lightbulb_status.value = v;
    err |= lightbulb_update_status(&lightbulb_status, true);

    return err;
}

static esp_err_t light_driver_set_cct(uint32_t cct)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Set cct %ld", cct);

    lightbulb_status_t lightbulb_status;
    err |= lightbulb_get_all_detail(&lightbulb_status);
    err |= lightbulb_set_fades_function(light_config.color_fade);
    err |= lightbulb_kelvin2percentage(cct, &lightbulb_status.cct_percentage);
    err |= lightbulb_update_status(&lightbulb_status, true);

    return err;
}

static esp_err_t light_driver_set_colormode(uint8_t color_mode)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Set colormode %d", color_mode);

    lightbulb_status_t lightbulb_status;
    err |= lightbulb_get_all_detail(&lightbulb_status);
    err |= lightbulb_set_fades_function(light_config.color_fade);
    lightbulb_status.mode = (lightbulb_works_mode_t)color_mode;
    err |= lightbulb_update_status(&lightbulb_status, true);

    return err;
}

esp_err_t light_driver_init()
{
    lightbulb_config_t lightbulb_config;
    lightbulb_gamma_config_t gamma_data;
    lightbulb_power_limit_t power_limit_config;

    memset(&gamma_data, 0x0, sizeof(lightbulb_gamma_config_t));
    memset(&lightbulb_config, 0x0, sizeof(lightbulb_config_t));
    memset(&power_limit_config, 0x0, sizeof(lightbulb_power_limit_t));

    lightbulb_config.type = DRIVER_ESP_PWM;
    lightbulb_config.capability.enable_status_storage = false;
    lightbulb_config.capability.enable_fade = true;
    lightbulb_config.capability.enable_lowpower = false;
    lightbulb_config.capability.sync_change_brightness_value = true;
    lightbulb_config.capability.disable_auto_on = true;
    lightbulb_config.capability.fade_time_ms = 800;

    lightbulb_config.capability.led_beads = LED_BEADS_3CH_RGB;
    lightbulb_config.init_status.mode = WORK_COLOR;

    lightbulb_config.capability.enable_precise_cct_control = false;

    lightbulb_config.cct_mix_mode.standard.kelvin_min = 2200;
    lightbulb_config.cct_mix_mode.standard.kelvin_max = 7000;

    power_limit_config.white_max_brightness = 100;
    power_limit_config.white_min_brightness = 10;
    power_limit_config.color_max_value = 100;
    power_limit_config.color_min_value = 10;
    power_limit_config.color_max_power = 100;
    power_limit_config.white_max_power = 100;

    gamma_data.balance_coefficient[0] = (float)1.0;
    gamma_data.balance_coefficient[1] = (float)1.0;
    gamma_data.balance_coefficient[2] = (float)1.0;
    gamma_data.balance_coefficient[3] = (float)1.0;
    gamma_data.balance_coefficient[4] = (float)1.0;
    gamma_data.curve_coefficient = (float)1.0;

    lightbulb_config.driver_conf.pwm.freq_hz = 4000;
    lightbulb_config.capability.enable_hardware_cct = true;
    lightbulb_config.driver_conf.pwm.phase_delay.flag = 0;

    lightbulb_config.io_conf.pwm_io.red = (gpio_num_t)PWM_RED_PIN;
    lightbulb_config.io_conf.pwm_io.green = (gpio_num_t)PWM_GREEN_PIN;
    lightbulb_config.io_conf.pwm_io.blue = (gpio_num_t)PWM_BLUE_PIN;

    lightbulb_config.gamma_conf = &gamma_data;
    lightbulb_config.external_limit = &power_limit_config;

    ESP_LOGI(TAG, "pwm_hz           %d", (int)lightbulb_config.driver_conf.pwm.freq_hz);
    ESP_LOGI(TAG, "temperature_mode %d", (int)lightbulb_config.capability.enable_hardware_cct );

    // show io
    ESP_LOGI(TAG, "io red   %d", (int)lightbulb_config.io_conf.pwm_io.red);
    ESP_LOGI(TAG, "io green %d", (int)lightbulb_config.io_conf.pwm_io.green);
    ESP_LOGI(TAG, "io blue  %d", (int)lightbulb_config.io_conf.pwm_io.blue);
    ESP_LOGI(TAG, "io cold  %d", (int)lightbulb_config.io_conf.pwm_io.cold_cct);
    ESP_LOGI(TAG, "io warm  %d", (int)lightbulb_config.io_conf.pwm_io.warm_brightness);

    esp_err_t err = lightbulb_init(&lightbulb_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lightbulb driver init failed");
    }

    return err;
}

/* Do any conversions/remapping for the actual value here */
esp_err_t light_matter_set_power(bool onoff)
{
    ESP_LOGI(TAG, "Set power");
    return light_driver_set_power(onoff);
}

esp_err_t light_matter_set_brightness(uint8_t level)
{
    ESP_LOGI(TAG, "Set brightness");
    double value = REMAP_TO_RANGE(level * 1.0, MATTER_BRIGHTNESS, STANDARD_BRIGHTNESS);
    uint16_t brightness = (uint16_t)ceil(value);

    return light_driver_set_brightness(brightness);
}

esp_err_t light_matter_set_hue(uint8_t hue)
{
    ESP_LOGI(TAG, "Set hue");
    int value = (int)ceil((hue * 1.0 * STANDARD_HUE) / MATTER_HUE);

    return light_driver_set_hue(value);
}

esp_err_t light_matter_set_saturation(uint8_t saturation)
{
    ESP_LOGI(TAG, "Set saturation");
    uint16_t value = REMAP_TO_RANGE(saturation, MATTER_SATURATION, STANDARD_SATURATION);

    return light_driver_set_saturation(value);
}

esp_err_t light_matter_set_current_xy(uint16_t color_x, uint16_t color_y, uint8_t level)
{
    ESP_LOGI(TAG, "Set current xy");

    float current_x = ((float)color_x) / MATTER_CURRENT_XY_DIVISOR;
    float current_y = ((float)color_y) / MATTER_CURRENT_XY_DIVISOR;
    float level_f = REMAP_TO_RANGE(level, MATTER_BRIGHTNESS, STANDARD_BRIGHTNESS);

    return light_driver_set_xyy(current_x, current_y, level_f);
}

esp_err_t light_matter_set_temperature(uint16_t color_temp)
{
    ESP_LOGI(TAG, "Set temperature");
    uint32_t value = REMAP_TO_RANGE_INVERSE(color_temp, STANDARD_TEMPERATURE_FACTOR);

    return light_driver_set_cct(value);
}

esp_err_t light_matter_set_colormode(uint8_t color_mode)
{
    ESP_LOGI(TAG, "Set colormode");
    lightbulb_works_mode_t mode;

    if (color_mode== (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation) {
        mode = WORK_COLOR;
    } else if (color_mode == (uint8_t)ColorControl::ColorMode::kColorTemperature) {
        mode = WORK_WHITE;
    } else if (color_mode == (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY) {
        // The xyy color space is currently not supported by any ecosystem,
        // it is only used for certification testing
        mode = WORK_COLOR;
    }

    return light_driver_set_colormode((uint8_t)mode);
}

void set_light_driver_default_state()
{
    bool onoff;
    uint8_t color_mode = (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;
    uint8_t hue;
    uint8_t saturation;
    uint16_t color_x, color_y;
    uint16_t color_temp;
    app::DataModel::Nullable<uint8_t> level;
    OnOff::Attributes::OnOff::Get(light_endpoint, &onoff);
    // ColorControl::Attributes::ColorMode::Set(light_endpoint,color_mode , app::MarkAttributeDirty::kNo);
    if(onoff == true) {
        light_matter_set_power(true);
        ColorControl::Attributes::ColorMode::Get(light_endpoint, &color_mode);
        if (color_mode == (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation) {
            ColorControl::Attributes::CurrentHue::Get(light_endpoint, &hue);
            ColorControl::Attributes::CurrentSaturation::Get(light_endpoint, &saturation);
            light_matter_set_colormode(WORK_COLOR);
            light_matter_set_hue((uint16_t)hue);
            light_matter_set_saturation((uint16_t)saturation);
        } else if (color_mode == (uint8_t)ColorControl::ColorMode::kColorTemperature) {
            //unsupport color temperature
            // light_driver_set_colormode(WORK_WHITE);
            ESP_LOGW(TAG, "unsupported color temperature mode");
        } else if (color_mode == (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY) {
            ColorControl::Attributes::CurrentX::Get(light_endpoint, &color_x);
            ColorControl::Attributes::CurrentY::Get(light_endpoint, &color_y);
            LevelControl::Attributes::CurrentLevel::Get(light_endpoint, level);
            printf("default color_x:%d----color_y:%d---level:%d\n", color_x, color_y, level.Value());
            light_matter_set_colormode(WORK_COLOR);
            light_matter_set_current_xy(color_x, color_y, level.Value());
        } else {
            ESP_LOGW(TAG, "unknown color mode");
        }
    }
}
