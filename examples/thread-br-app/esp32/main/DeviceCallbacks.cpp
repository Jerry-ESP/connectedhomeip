/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * @file DeviceCallbacks.cpp
 *
 * Implements all the callbacks to the application from the CHIP Stack
 *
 **/

#include "DeviceCallbacks.h"
#include <light_driver.h>
#include "common/Esp32ThreadInit.h"
#include "platform/ESP32/OpenthreadLauncher.h"
#include "platform/ThreadStackManager.h"
#include <esp_log.h>
#include <esp_openthread_border_router.h>
#include <esp_openthread_lock.h>
#include <lib/support/logging/CHIPLogging.h>

#include <app/util/util.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <lib/support/logging/CHIPLogging.h>

static const char TAG[] = "thread-br-app-callbacks";

using namespace chip;
using namespace chip::Inet;
using namespace chip::System;
using namespace chip::app::Clusters;

uint16_t light_endpoint = 2;

void AppDeviceCallbacks::PostAttributeChangeCallback(EndpointId endpointId, ClusterId clusterId, AttributeId attributeId,
                                                     uint8_t type, uint16_t size, uint8_t * value)
{
    ESP_LOGI(TAG, "PostAttributeChangeCallback - Cluster ID: '0x%" PRIx32 "', EndPoint ID: '0x%x', Attribute ID: '0x%" PRIx32 "'",
             clusterId, endpointId, attributeId);

    if (endpointId == light_endpoint)
    {
        switch (clusterId)
        {
        case OnOff::Id:
            OnOnOffPostAttributeChangeCallback(endpointId, attributeId, value);
            break;

        case LevelControl::Id:
            OnLevelControlAttributeChangeCallback(endpointId, attributeId, value);
            break;

        case ColorControl::Id:
            OnColorControlAttributeChangeCallback(endpointId, attributeId, value);
            break;

        default:
            ESP_LOGI(TAG, "Unhandled cluster ID: %" PRIu32, clusterId);
            break;
        }
    }

    ESP_LOGI(TAG, "Current free heap: %u\n", static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
}

void AppDeviceCallbacksDelegate::OnIPv6ConnectivityEstablished(void)
{
    static bool sThreadBRInitialized = false;
    if (!sThreadBRInitialized)
    {
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_openthread_border_router_init();
        esp_openthread_lock_release();
        sThreadBRInitialized = true;
    }
}

void AppDeviceCallbacks::OnOnOffPostAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    bool onoff;

    VerifyOrExit(attributeId == OnOff::Attributes::OnOff::Id,
                 ESP_LOGI(TAG, "Unhandled Attribute ID: '0x%" PRIx32 "'", attributeId));
    VerifyOrExit(endpointId == light_endpoint, ESP_LOGE(TAG, "Unexpected EndPoint ID: `0x%02x'", endpointId));

    onoff = (bool)(*value);

    printf("onoff attribute change callback, new value: %d\n", onoff);
    light_matter_set_power(onoff);

exit:
    return;
}

void AppDeviceCallbacks::OnLevelControlAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    uint8_t level;
    bool onoff;

    VerifyOrExit(attributeId == LevelControl::Attributes::CurrentLevel::Id,
                 ESP_LOGI(TAG, "Unhandled Attribute ID: '0x%" PRIx32 "'", attributeId));
    VerifyOrExit(endpointId == light_endpoint, ESP_LOGE(TAG, "Unexpected EndPoint ID: `0x%02x'", endpointId));

    level = *value;

    printf("levelcontrol attribute change callback, new value: %d\n", level);
    OnOff::Attributes::OnOff::Get(endpointId, &onoff);
    if(onoff == true) {
        light_matter_set_brightness(level);
    }

exit:
    return;
}

// Currently ColorControl cluster is supported for ESP32C3_DEVKITM and ESP32S3_DEVKITM which have an on-board RGB-LED
void AppDeviceCallbacks::OnColorControlAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    using namespace ColorControl::Attributes;

    uint8_t hue, saturation, color_mode;
    uint16_t color_x, color_y, color_temp;
    app::DataModel::Nullable<uint8_t> level;

    VerifyOrExit(endpointId == light_endpoint, ESP_LOGE(TAG, "Unexpected EndPoint ID: `0x%02x'", endpointId));

    if (attributeId == ColorControl::Attributes::CurrentHue::Id) {
        hue = *value;
        light_matter_set_hue(hue);
    } else if (attributeId == ColorControl::Attributes::CurrentSaturation::Id) {
        saturation = *value;
        light_matter_set_saturation(saturation);
    } else if (attributeId == ColorControl::Attributes::ColorTemperatureMireds::Id) {
        memcpy(&color_temp, value, 2);
        light_matter_set_temperature(color_temp);
    } else if (attributeId == ColorControl::Attributes::CurrentX::Id ||
                attributeId == ColorControl::Attributes::CurrentY::Id) {
        CurrentX::Get(endpointId, &color_x);
        CurrentY::Get(endpointId, &color_y);
        LevelControl::Attributes::CurrentLevel::Get(endpointId, level);
        light_matter_set_current_xy(color_x, color_y, level.Value());
    } else if (attributeId == ColorControl::Attributes::ColorMode::Id) {
        color_mode = (uint8_t)(*value);
        light_matter_set_colormode(color_mode);
    }

exit:
    return;
}

/** @brief OnOff Cluster Init
 *
 * This function is called when a specific cluster is initialized. It gives the
 * application an opportunity to take care of cluster initialization procedures.
 * It is called exactly once for each endpoint where cluster is present.
 *
 * @param endpoint   Ver.: always
 *
 * emberAfOnOffClusterInitCallback happens before the stack initialize the cluster
 * attributes to the default value.
 * The logic here expects something similar to the deprecated Plugins callback
 * emberAfPluginOnOffClusterServerPostInitCallback.
 *
 */
void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
    ESP_LOGI(TAG, "emberAfOnOffClusterInitCallback");
    light_driver_init();
}
