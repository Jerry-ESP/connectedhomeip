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

#include <esp_log.h>
#include <lightbulb.h>

#define STANDARD_BRIGHTNESS 100
#define STANDARD_HUE 360
#define STANDARD_SATURATION 100
#define STANDARD_TEMPERATURE_FACTOR 1000000

/** Matter max values (used for remapping attributes) */
#define MATTER_BRIGHTNESS 254
#define MATTER_HUE 254
#define MATTER_SATURATION 254
#define MATTER_TEMPERATURE_FACTOR 1000000
#define MATTER_CURRENT_XY_DIVISOR 65536.0f

#define REMAP_TO_RANGE(value, from, to) ((value * to) / from)

#define REMAP_TO_RANGE_INVERSE(value, factor) (factor / (value ? value : 1))

esp_err_t light_driver_init();

esp_err_t light_matter_set_power(bool onoff);
esp_err_t light_matter_set_brightness(uint8_t level);
esp_err_t light_matter_set_hue(uint8_t hue);
esp_err_t light_matter_set_saturation(uint8_t saturation);
esp_err_t light_matter_set_current_xy(uint16_t color_x, uint16_t color_y, uint8_t level);
esp_err_t light_matter_set_temperature(uint16_t color_temp);
esp_err_t light_matter_set_colormode(uint8_t color_mode);
