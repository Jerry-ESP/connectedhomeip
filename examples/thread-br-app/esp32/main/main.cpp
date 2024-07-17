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

#include "DeviceCallbacks.h"
#include <platform/OpenThread/GenericThreadBorderRouterDelegate.h>

#include <common/CHIPDeviceManager.h>
#include <common/Esp32AppServer.h>
#include <common/Esp32ThreadInit.h>
#include <shell_extension/openthread_cli_register.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread_border_router.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "shell_extension/launch.h"

#include <app/clusters/thread-border-router-management-server/thread-br-mgmt-server.h>
#include <app/server/OnboardingCodesUtil.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <platform/ESP32/ESP32Utils.h>

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
#include <platform/ESP32/ESP32DeviceInfoProvider.h>
#else
#include <DeviceInfoProviderImpl.h>
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER

#define REBOOT_COUNT_NVS_KEY "reboot_count"

using namespace ::chip;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceManager;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;

namespace {
#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
DeviceLayer::ESP32FactoryDataProvider sFactoryDataProvider;
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
DeviceLayer::ESP32DeviceInfoProvider gExampleDeviceInfoProvider;
#else
DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
} // namespace

static const char TAG[] = "thread-br-app";

static AppDeviceCallbacks EchoCallbacks;
static AppDeviceCallbacksDelegate sAppDeviceCallbacksDelegate;

static void InitServer(intptr_t context)
{
    // Print QR Code URL
    PrintOnboardingCodes(chip::RendezvousInformationFlags(CONFIG_RENDEZVOUS_MODE));

    Esp32AppServer::Init(); // Init ZCL Data Model and CHIP App Server AND Initialize device attestation config
}

static constexpr EndpointId kThreadBRMgmtEndpoint = 1;

static ThreadBorderRouterManagement::GenericThreadBorderRouterDelegate sThreadBRDelegate;
static ThreadBorderRouterManagement::ServerInstance sThreadBRMgmtInstance(kThreadBRMgmtEndpoint, &sThreadBRDelegate);

static uint8_t get_count()
{
    uint8_t value = 0;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition("nvs", "tbr-app", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %d", err);
        return value;
    }
    err = nvs_get_u8(handle, REBOOT_COUNT_NVS_KEY, &value);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error getting from NVS: %d", err);
    }
    nvs_commit(handle);
    nvs_close(handle);
    return value;
}

static esp_err_t set_count(uint8_t value)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition("nvs", "tbr-app", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %d", err);
        return err;
    }
    err = nvs_set_u8(handle, REBOOT_COUNT_NVS_KEY, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error setting in NVS: %d", err);
    }
    printf("set count %d\n", value);
    nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void perform_reset_count(TimerHandle_t timer)
{
    if (get_count() >= 3) {
        set_count(0);
        printf("perform factoryreset\n");
        ConfigurationMgr().InitiateFactoryReset();
    }
    set_count(0);
    xTimerDelete(timer, 0);
}

extern "C" void app_main()
{
    // Initialize the ESP NVS layer.
    esp_err_t err = nvs_flash_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init() failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t count = get_count();
    count ++;
    set_count(count);
    TimerHandle_t timer = xTimerCreate("factory_reset", pdMS_TO_TICKS(5 * 1000), pdFALSE, NULL,
                                    &perform_reset_count);
    if (!timer) {
        ESP_LOGE(TAG, "Could not initialize timer");
        return;
    }
    xTimerStart(timer, 0);

    err = esp_event_loop_create_default();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default() failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "chip-esp32-thread-br-example starting");
    ESP_LOGI(TAG, "==================================================");

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    if (DeviceLayer::Internal::ESP32Utils::InitWiFiStack() != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to initialize the Wi-Fi stack");
        return;
    }
#endif

#if CONFIG_ENABLE_CHIP_SHELL
#if CONFIG_OPENTHREAD_CLI
    chip::RegisterOpenThreadCliCommands();
#endif
    chip::LaunchShell();
#endif // CONFIG_ENABLE_CHIP_SHELL

    DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);

    CHIPDeviceManager & deviceMgr = CHIPDeviceManager::GetInstance();
    CHIP_ERROR error              = deviceMgr.Init(&EchoCallbacks);
    DeviceCallbacksDelegate::Instance().SetAppDelegate(&sAppDeviceCallbacksDelegate);
    if (error != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "device.Init() failed: %" CHIP_ERROR_FORMAT, error.Format());
        return;
    }

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
    SetCommissionableDataProvider(&sFactoryDataProvider);
    SetDeviceAttestationCredentialsProvider(&sFactoryDataProvider);
#if CONFIG_ENABLE_ESP32_DEVICE_INSTANCE_INFO_PROVIDER
    SetDeviceInstanceInfoProvider(&sFactoryDataProvider);
#endif
#else
    SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
    esp_openthread_set_backbone_netif(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
    ESPOpenThreadInit();

    chip::DeviceLayer::PlatformMgr().ScheduleWork(InitServer, reinterpret_cast<intptr_t>(nullptr));
    sThreadBRMgmtInstance.Init();

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    set_light_driver_default_state();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

extern "C" void otSysProcessDrivers(otInstance *aInstance)
{
    (void)aInstance;
}
