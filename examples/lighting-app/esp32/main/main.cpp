/*
 *
 *    Copyright (c) 2021-2023 Project CHIP Authors
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

#include "AppTask.h"
#include "esp_log.h"
#include <common/CHIPDeviceManager.h>
#include <common/Esp32AppServer.h>
#include <common/Esp32ThreadInit.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "spi_flash_mmap.h"
#else
#include "esp_spi_flash.h"
#endif
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "shell_extension/launch.h"
#include "shell_extension/openthread_cli_register.h"
#include <app/server/Dnssd.h>
#include <app/server/OnboardingCodesUtil.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <credentials/CertificationDeclaration.h>
#include <platform/ESP32/ESP32Utils.h>

#if CONFIG_ENABLE_ESP_INSIGHTS_SYSTEM_STATS
#include <tracing/esp32_trace/insights_sys_stats.h>
#define START_TIMEOUT_MS 60000
#endif

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_PW_RPC
#include "Rpc.h"
#endif

#include "DeviceWithDisplay.h"

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
#include <platform/ESP32/ESP32DeviceInfoProvider.h>
#else
#include <DeviceInfoProviderImpl.h>
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER

#if CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#endif

#if CONFIG_ENABLE_ESP_INSIGHTS_TRACE
#include <esp_insights.h>
#include <tracing/esp32_trace/esp32_tracing.h>
#include <tracing/registry.h>
#endif

using namespace ::chip;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceManager;
using namespace ::chip::DeviceLayer;
using namespace ::chip::Crypto;

static const char TAG[] = "MFG-TEST-APP";

static AppDeviceCallbacks EchoCallbacks;
static AppDeviceCallbacksDelegate sAppDeviceCallbacksDelegate;

namespace {
#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
DeviceLayer::ESP32FactoryDataProvider sFactoryDataProvider;
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
DeviceLayer::ESP32DeviceInfoProvider gExampleDeviceInfoProvider;
#else
DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER

#if CONFIG_SEC_CERT_DAC_PROVIDER
DeviceLayer::ESP32SecureCertDACProvider gSecureCertDACProvider;
#endif // CONFIG_SEC_CERT_DAC_PROVIDER

chip::Credentials::DeviceAttestationCredentialsProvider * get_dac_provider(void)
{
#if CONFIG_SEC_CERT_DAC_PROVIDER
    return &gSecureCertDACProvider;
#elif CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
    return &sFactoryDataProvider;
#else  // EXAMPLE_DAC_PROVIDER
    return chip::Credentials::Examples::GetExampleDACProvider();
#endif
}

} // namespace

uint8_t s_dac_cert_buffer[kMaxDERCertLength];  // 600 bytes
uint8_t s_pai_cert_buffer[kMaxDERCertLength];  // 600 bytes
uint8_t s_paa_cert_buffer[kMaxDERCertLength];  // 600 bytes
uint8_t s_cd_buffer[Credentials::kMaxCMSSignedCDMessage];

MutableByteSpan paa_span;
MutableByteSpan pai_span;
MutableByteSpan dac_span;
MutableByteSpan cd_span;

uint8_t s_garbage_buffer[128];

CHIP_ERROR verify_factory_information()
{
    DeviceInstanceInfoProvider *factory_provider = GetDeviceInstanceInfoProvider();
    VerifyOrReturnError(factory_provider, CHIP_ERROR_INTERNAL, ESP_LOGE(TAG, "ERROR: Failed to get the factory provider impl"));

    char vendor_name[32];
    char product_name[32];
    char hardware_ver_str[32];

    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t hardware_ver;

    factory_provider->GetVendorName(vendor_name, 32);
    factory_provider->GetVendorId(vendor_id);
    factory_provider->GetProductName(product_name, 32);
    factory_provider->GetProductId(product_id);
    factory_provider->GetHardwareVersionString(hardware_ver_str, 32);
    factory_provider->GetHardwareVersion(hardware_ver);

    ESP_LOGI(TAG, "---------- factory info verify ----------");
    ESP_LOGI(TAG, "Vendor Name:              %s", vendor_name);
    ESP_LOGI(TAG, "Vendor ID:                0x%04X", vendor_id);
    ESP_LOGI(TAG, "Product Name:             %s", product_name);
    ESP_LOGI(TAG, "Product ID:               0x%04X", product_id);
    ESP_LOGI(TAG, "Hardware Version String:  %s", hardware_ver_str);
    ESP_LOGI(TAG, "Hardware Version:         0x%04X", hardware_ver);

    return CHIP_NO_ERROR;
}

CHIP_ERROR read_certs_in_spans()
{
    // DAC Provider implementation
    DeviceAttestationCredentialsProvider * dac_provider = GetDeviceAttestationCredentialsProvider();
    VerifyOrReturnError(dac_provider, CHIP_ERROR_INTERNAL, ESP_LOGE(TAG, "ERROR: Failed to get the DAC provider impl"));

    // Read DAC
    dac_span = MutableByteSpan(s_dac_cert_buffer);
    CHIP_ERROR err = dac_provider->GetDeviceAttestationCert(dac_span);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to read the DAC, %" CHIP_ERROR_FORMAT, err.Format()));

    // Read PAI
    pai_span = MutableByteSpan(s_pai_cert_buffer);
    err = dac_provider->GetProductAttestationIntermediateCert(pai_span);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to read the PAI Certificate %" CHIP_ERROR_FORMAT, err.Format()));

    // Read CD
    cd_span = MutableByteSpan(s_cd_buffer);
    err = dac_provider->GetCertificationDeclaration(cd_span);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to read the CD Certificate %" CHIP_ERROR_FORMAT, err.Format()));

    ESP_LOGI(TAG, "\n----------Read CD Success----------\n");

    return CHIP_NO_ERROR;
}

CHIP_ERROR dump_cert_details(const char *type, ByteSpan cert_span)
{
    ESP_LOGI(TAG, "---------- %s ----------", type);

    // Get VID, PID from the certificate
    AttestationCertVidPid vidpid;
    CHIP_ERROR err = ExtractVIDPIDFromX509Cert(cert_span, vidpid);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to extract VID and PID, error: %" CHIP_ERROR_FORMAT, err.Format()));

    if (vidpid.mVendorId.HasValue()) {
        ESP_LOGI(TAG, "Vendor ID: 0x%04X", vidpid.mVendorId.Value());
    }

    if (vidpid.mProductId.HasValue()) {
        ESP_LOGI(TAG, "Product ID: 0x%04X", vidpid.mProductId.Value());
    }

    // Get Public key from the certificate
    P256PublicKey pubkey;
    err = ExtractPubkeyFromX509Cert(cert_span, pubkey);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to extract public key, error: %" CHIP_ERROR_FORMAT, err.Format()));

    // Print public key
    // ESP_LOGI(TAG, "Public Key encoded as hex string:");
    // for (uint8_t i = 0; i < pubkey.Length(); i++) {
    //     printf("%02x", pubkey.ConstBytes()[i]);
    // }
    // printf("\n\n");

    // Get AKID from the certificate
    uint8_t akid_buffer[64];
    MutableByteSpan akid_span(akid_buffer);
    err = ExtractAKIDFromX509Cert(cert_span, akid_span);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to extract AKID, error: %" CHIP_ERROR_FORMAT, err.Format()));

    // Print AKID
    // ESP_LOGI(TAG, "X509v3 Authority Key Identifier:");
    // printf("%02x", akid_span.data()[0]);
    // for (uint8_t i = 1; i < akid_span.size(); i++) {
    //     printf(":%02X", akid_span.data()[i]);
    // }
    // printf("\n\n");

    // Get SKID from the certificate
    uint8_t skid_buffer[64];
    MutableByteSpan skid_span(skid_buffer);
    err = ExtractSKIDFromX509Cert(cert_span, skid_span);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to extract SKID, error: %" CHIP_ERROR_FORMAT, err.Format()));

    // Print SKID
    // ESP_LOGI(TAG, "X509v3 Subject Key Identifier:");
    // printf("%02x", skid_span.data()[0]);
    // for (uint8_t i = 1; i < skid_span.size(); i++) {
    //     printf(":%02X", skid_span.data()[i]);
    // }
    // printf("\n\n");

    ESP_LOGI(TAG, "------------------------------\n");

    return CHIP_NO_ERROR;
}

CHIP_ERROR test_dac(ByteSpan dac)
{
    ESP_LOGI(TAG, "---------- Test DAC ----------");
    // DAC Provider implementation
    DeviceAttestationCredentialsProvider * dac_provider = GetDeviceAttestationCredentialsProvider();
    VerifyOrReturnError(dac_provider, CHIP_ERROR_INTERNAL, ESP_LOGE(TAG, "ERROR: Failed to get the DAC provider impl"));

    // Get Public key from the certificate
    P256PublicKey pubkey;
    CHIP_ERROR err = ExtractPubkeyFromX509Cert(dac, pubkey);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to get DAC public key, error: %" CHIP_ERROR_FORMAT, err.Format()));

    // Garbage
    esp_fill_random(s_garbage_buffer, sizeof(s_garbage_buffer));
    ByteSpan mts_span(s_garbage_buffer);

    // signature
    P256ECDSASignature signature;
    MutableByteSpan signature_span{ signature.Bytes(), signature.Capacity() };

    // Generate attestation signature
    err = dac_provider->SignWithDeviceAttestationKey(mts_span, signature_span);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to sign the message with DAC key, error: %" CHIP_ERROR_FORMAT, err.Format()));

    ESP_LOGI(TAG, "Message signed with DAC key: OK");
    // ESP_LOG_BUFFER_HEX(TAG, signature_span.data(), signature_span.size());

    P256ECDSASignature signature_to_verify;

    ReturnErrorOnFailure(signature_to_verify.SetLength(signature_span.size()));
    memcpy(signature_to_verify.Bytes(), signature_span.data(), signature_span.size());

    err = pubkey.ECDSA_validate_msg_signature(s_garbage_buffer, sizeof(s_garbage_buffer), signature_to_verify);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ESP_LOGE(TAG, "ERROR: Failed to validate signature, error: %" CHIP_ERROR_FORMAT, err.Format()));

    ESP_LOGI(TAG, "Signature Verification: OK");
    ESP_LOGI(TAG, "------------------------------\n");
    return CHIP_NO_ERROR;
}

extern "C" void app_main()
{
    // Initialize the ESP NVS layer.
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init() failed: %s", esp_err_to_name(error));
        return;
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "DAC And Factory Information Verify");
    ESP_LOGI(TAG, "==================================================");

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
    SetCommissionableDataProvider(&sFactoryDataProvider);
#if CONFIG_ENABLE_ESP32_DEVICE_INSTANCE_INFO_PROVIDER
    SetDeviceInstanceInfoProvider(&sFactoryDataProvider);
#endif
#endif

    SetDeviceAttestationCredentialsProvider(get_dac_provider());

    read_certs_in_spans();

    // Dump PAI details
    CHIP_ERROR err = dump_cert_details("PAI", pai_span);
    VerifyOrReturn(err == CHIP_NO_ERROR, ESP_LOGE(TAG, "ERROR: Failed to dump PAI certificate details, error: %" CHIP_ERROR_FORMAT, err.Format()));

    // Dump DAC details
    err = dump_cert_details("DAC", dac_span);
    VerifyOrReturn(err == CHIP_NO_ERROR, ESP_LOGE(TAG, "ERROR: Failed to dump DAC certificate details, error: %" CHIP_ERROR_FORMAT, err.Format()));

    // Sign the message with DAC key and verify with public key in DAC certificate
    err = test_dac(dac_span);
    VerifyOrReturn(err == CHIP_NO_ERROR, ESP_LOGE(TAG, "ERROR: Failed to Sign and Verify using DAC keypair, error: %" CHIP_ERROR_FORMAT, err.Format()));

    verify_factory_information();
}
