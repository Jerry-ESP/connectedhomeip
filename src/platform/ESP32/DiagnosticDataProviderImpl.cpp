/*
 *
 *    Copyright (c) 2021-2022 Project CHIP Authors
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
 *    @file
 *          Provides an implementation of the DiagnosticDataProvider object
 *          for ESP32 platform.
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/CHIPMemString.h>
#include <platform/DiagnosticDataProvider.h>
#include <platform/ESP32/DiagnosticDataProviderImpl.h>
#include <platform/ESP32/ESP32Utils.h>

#include "esp_event.h"
#include "esp_heap_caps_init.h"
#include "esp_log.h"
#include "esp_netif.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "spi_flash_mmap.h"
#else
#include "esp_spi_flash.h"
#endif
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"

using namespace ::chip;
using namespace ::chip::TLV;
using namespace ::chip::DeviceLayer;
using namespace ::chip::DeviceLayer::Internal;
using namespace ::chip::app::Clusters::GeneralDiagnostics;

namespace {

InterfaceTypeEnum GetInterfaceType(const char * if_desc)
{
    if (strncmp(if_desc, "ap", strnlen(if_desc, 2)) == 0 || strncmp(if_desc, "sta", strnlen(if_desc, 3)) == 0)
        return InterfaceTypeEnum::kWiFi;
    if (strncmp(if_desc, "openthread", strnlen(if_desc, 10)) == 0)
        return InterfaceTypeEnum::kThread;
    if (strncmp(if_desc, "eth", strnlen(if_desc, 3)) == 0)
        return InterfaceTypeEnum::kEthernet;
    return InterfaceTypeEnum::kUnspecified;
}

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
app::Clusters::WiFiNetworkDiagnostics::SecurityTypeEnum MapAuthModeToSecurityType(wifi_auth_mode_t authmode)
{
    using app::Clusters::WiFiNetworkDiagnostics::SecurityTypeEnum;
    switch (authmode)
    {
    case WIFI_AUTH_OPEN:
        return SecurityTypeEnum::kNone;
    case WIFI_AUTH_WEP:
        return SecurityTypeEnum::kWep;
    case WIFI_AUTH_WPA_PSK:
        return SecurityTypeEnum::kWpa;
    case WIFI_AUTH_WPA2_PSK:
        return SecurityTypeEnum::kWpa2;
    case WIFI_AUTH_WPA3_PSK:
        return SecurityTypeEnum::kWpa3;
    default:
        return SecurityTypeEnum::kUnspecified;
    }
}

app::Clusters::WiFiNetworkDiagnostics::WiFiVersionEnum GetWiFiVersionFromAPRecord(wifi_ap_record_t ap_info)
{
    using app::Clusters::WiFiNetworkDiagnostics::WiFiVersionEnum;
    if (ap_info.phy_11n)
        return WiFiVersionEnum::kN;
    else if (ap_info.phy_11g)
        return WiFiVersionEnum::kG;
    else if (ap_info.phy_11b)
        return WiFiVersionEnum::kB;
    else
        return WiFiVersionEnum::kUnknownEnumValue;
}
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI

} // namespace

namespace chip {
namespace DeviceLayer {

DiagnosticDataProviderImpl & DiagnosticDataProviderImpl::GetDefaultInstance()
{
    static DiagnosticDataProviderImpl sInstance;
    return sInstance;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetCurrentHeapFree(uint64_t & currentHeapFree)
{
    currentHeapFree = esp_get_free_heap_size();
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetCurrentHeapUsed(uint64_t & currentHeapUsed)
{
    currentHeapUsed = heap_caps_get_total_size(MALLOC_CAP_DEFAULT) - esp_get_free_heap_size();
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetCurrentHeapHighWatermark(uint64_t & currentHeapHighWatermark)
{
    currentHeapHighWatermark = heap_caps_get_total_size(MALLOC_CAP_DEFAULT) - esp_get_minimum_free_heap_size();
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetRebootCount(uint16_t & rebootCount)
{
    uint32_t count = 0;

    CHIP_ERROR err = ConfigurationMgr().GetRebootCount(count);

    if (err == CHIP_NO_ERROR)
    {
        VerifyOrReturnError(count <= UINT16_MAX, CHIP_ERROR_INVALID_INTEGER_VALUE);
        rebootCount = static_cast<uint16_t>(count);
    }

    return err;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetUpTime(uint64_t & upTime)
{
    System::Clock::Timestamp currentTime = System::SystemClock().GetMonotonicTimestamp();
    System::Clock::Timestamp startTime   = PlatformMgrImpl().GetStartTime();

    if (currentTime >= startTime)
    {
        upTime = std::chrono::duration_cast<System::Clock::Seconds64>(currentTime - startTime).count();
        return CHIP_NO_ERROR;
    }

    return CHIP_ERROR_INVALID_TIME;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetTotalOperationalHours(uint32_t & totalOperationalHours)
{
    uint64_t upTime = 0;

    if (GetUpTime(upTime) == CHIP_NO_ERROR)
    {
        uint32_t totalHours = 0;
        if (ConfigurationMgr().GetTotalOperationalHours(totalHours) == CHIP_NO_ERROR)
        {
            VerifyOrReturnError(upTime / 3600 <= UINT32_MAX, CHIP_ERROR_INVALID_INTEGER_VALUE);
            totalOperationalHours = totalHours + static_cast<uint32_t>(upTime / 3600);
            return CHIP_NO_ERROR;
        }
    }

    return CHIP_ERROR_INVALID_TIME;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetBootReason(BootReasonType & bootReason)
{
    bootReason = BootReasonType::kUnspecified;
    uint8_t reason;
    reason = static_cast<uint8_t>(esp_reset_reason());
    if (reason == ESP_RST_UNKNOWN)
    {
        bootReason = BootReasonType::kUnspecified;
    }
    else if (reason == ESP_RST_POWERON)
    {
        bootReason = BootReasonType::kPowerOnReboot;
    }
    else if (reason == ESP_RST_BROWNOUT)
    {
        bootReason = BootReasonType::kBrownOutReset;
    }
    else if (reason == ESP_RST_SW)
    {
        bootReason = BootReasonType::kSoftwareReset;
    }
    else if (reason == ESP_RST_INT_WDT)
    {
        bootReason = BootReasonType::kSoftwareWatchdogReset;
        /* Reboot can be due to hardware or software watchdog*/
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetNetworkInterfaces(NetworkInterface ** netifpp)
{
    esp_netif_t * netif     = esp_netif_next(NULL);
    NetworkInterface * head = NULL;
    uint8_t ipv6_addr_count = 0;
    esp_ip6_addr_t ip6_addr[kMaxIPv6AddrCount];
    if (netif == NULL)
    {
        ChipLogError(DeviceLayer, "Failed to get network interfaces");
    }
    else
    {
        for (esp_netif_t * ifa = netif; ifa != NULL; ifa = esp_netif_next(ifa))
        {
            NetworkInterface * ifp = new NetworkInterface();
            esp_netif_ip_info_t ipv4_info;
            Platform::CopyString(ifp->Name, esp_netif_get_ifkey(ifa));
            ifp->name          = CharSpan::fromCharString(ifp->Name);
            ifp->isOperational = true;
            ifp->type          = GetInterfaceType(esp_netif_get_desc(ifa));
            ifp->offPremiseServicesReachableIPv4.SetNull();
            ifp->offPremiseServicesReachableIPv6.SetNull();
            if (esp_netif_get_mac(ifa, ifp->MacAddress) != ESP_OK)
            {
                ChipLogError(DeviceLayer, "Failed to get network hardware address");
            }
            else
            {
                ifp->hardwareAddress = ByteSpan(ifp->MacAddress, 6);
            }
#if !CONFIG_DISABLE_IPV4
            if (esp_netif_get_ip_info(ifa, &ipv4_info) == ESP_OK)
            {
                memcpy(ifp->Ipv4AddressesBuffer[0], &(ipv4_info.ip.addr), kMaxIPv4AddrSize);
                ifp->Ipv4AddressSpans[0] = ByteSpan(ifp->Ipv4AddressesBuffer[0], kMaxIPv4AddrSize);
                ifp->IPv4Addresses       = app::DataModel::List<ByteSpan>(ifp->Ipv4AddressSpans, 1);
            }
#endif

            static_assert(kMaxIPv6AddrCount <= UINT8_MAX, "Count might not fit in ipv6_addr_count");
            static_assert(ArraySize(ip6_addr) >= LWIP_IPV6_NUM_ADDRESSES, "Not enough space for our addresses.");
            auto addr_count = esp_netif_get_all_ip6(ifa, ip6_addr);
            if (addr_count < 0)
            {
                ipv6_addr_count = 0;
            }
            else
            {
                ipv6_addr_count = static_cast<uint8_t>(min(addr_count, static_cast<int>(kMaxIPv6AddrCount)));
            }
            for (uint8_t idx = 0; idx < ipv6_addr_count; ++idx)
            {
                memcpy(ifp->Ipv6AddressesBuffer[idx], ip6_addr[idx].addr, kMaxIPv6AddrSize);
                ifp->Ipv6AddressSpans[idx] = ByteSpan(ifp->Ipv6AddressesBuffer[idx], kMaxIPv6AddrSize);
            }
            ifp->IPv6Addresses = app::DataModel::List<ByteSpan>(ifp->Ipv6AddressSpans, ipv6_addr_count);

            ifp->Next = head;
            head      = ifp;
        }
    }
    *netifpp = head;
    return CHIP_NO_ERROR;
}

void DiagnosticDataProviderImpl::ReleaseNetworkInterfaces(NetworkInterface * netifp)
{
    while (netifp)
    {
        NetworkInterface * del = netifp;
        netifp                 = netifp->Next;
        delete del;
    }
}

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiBssId(MutableByteSpan & BssId)
{
    constexpr size_t bssIdSize = 6;
    VerifyOrReturnError(BssId.size() >= bssIdSize, CHIP_ERROR_BUFFER_TOO_SMALL);

    wifi_ap_record_t ap_info;
    esp_err_t err;

    err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK)
    {
        return CHIP_ERROR_READ_FAILED;
    }

    memcpy(BssId.data(), ap_info.bssid, bssIdSize);
    BssId.reduce_size(bssIdSize);
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiSecurityType(app::Clusters::WiFiNetworkDiagnostics::SecurityTypeEnum & securityType)
{
    using app::Clusters::WiFiNetworkDiagnostics::SecurityTypeEnum;

    securityType = SecurityTypeEnum::kUnspecified;
    wifi_ap_record_t ap_info;
    esp_err_t err;

    err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK)
    {
        securityType = MapAuthModeToSecurityType(ap_info.authmode);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiVersion(app::Clusters::WiFiNetworkDiagnostics::WiFiVersionEnum & wifiVersion)
{
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    VerifyOrReturnError(err == ESP_OK, ESP32Utils::MapError(err));

    wifiVersion = GetWiFiVersionFromAPRecord(ap_info);
    VerifyOrReturnError(wifiVersion != app::Clusters::WiFiNetworkDiagnostics::WiFiVersionEnum::kUnknownEnumValue,
                        CHIP_ERROR_INTERNAL);
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiChannelNumber(uint16_t & channelNumber)
{
    channelNumber = 0;
    wifi_ap_record_t ap_info;
    esp_err_t err;

    err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK)
    {
        channelNumber = ap_info.primary;
        return CHIP_NO_ERROR;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiRssi(int8_t & rssi)
{
    rssi = 0;
    wifi_ap_record_t ap_info;
    esp_err_t err;

    err = esp_wifi_sta_get_ap_info(&ap_info);

    if (err == ESP_OK)
    {
        rssi = ap_info.rssi;
        return CHIP_NO_ERROR;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiBeaconRxCount(uint32_t & beaconRxCount)
{
    esp_err_t err;
    uint32_t rev_count;
    uint32_t total_count;

    err = esp_wifi_internal_sta_get_beacon_statis(&rev_count, &total_count);

    if (err == ESP_OK)
    {
        beaconRxCount = rev_count;
        return CHIP_NO_ERROR;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiBeaconLostCount(uint32_t & beaconLostCount)
{
    esp_err_t err;
    uint32_t rev_count;
    uint32_t total_count;

    err = esp_wifi_internal_sta_get_beacon_statis(&rev_count, &total_count);

    if (err == ESP_OK)
    {
        beaconLostCount = total_count - rev_count;
        return CHIP_NO_ERROR;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiCurrentMaxRate(uint64_t & currentMaxRate)
{
    esp_err_t err;
    wifi_phy_rate_t rate;

    err = esp_wifi_internal_sta_get_cur_tx_rate(&rate);

    printf("read current rate: %d\n", static_cast<uint8_t>(rate));

    if (err == ESP_OK)
    {
        switch (rate)
        {
            case WIFI_PHY_RATE_1M_L:
                currentMaxRate = 1000000;
                break;
            case WIFI_PHY_RATE_2M_L:
                currentMaxRate = 2000000;
                break;
            case WIFI_PHY_RATE_5M_L:
                currentMaxRate = 5000000;
                break;
            case WIFI_PHY_RATE_11M_L:
                currentMaxRate = 11000000;
                break;
            case WIFI_PHY_RATE_2M_S:
                currentMaxRate = 2000000;
                break;
            case WIFI_PHY_RATE_5M_S:
                currentMaxRate = 5000000;
                break;
            case WIFI_PHY_RATE_11M_S:
                currentMaxRate = 11000000;
                break;
            case WIFI_PHY_RATE_48M:
                currentMaxRate = 48000000;
                break;
            case WIFI_PHY_RATE_24M:
                currentMaxRate = 24000000;
                break;
            case WIFI_PHY_RATE_12M:
                currentMaxRate = 12000000;
                break;
            case WIFI_PHY_RATE_6M:
                currentMaxRate = 6000000;
                break;
            case WIFI_PHY_RATE_54M:
                currentMaxRate = 54000000;
                break;
            case WIFI_PHY_RATE_36M:
                currentMaxRate = 36000000;
                break;
            case WIFI_PHY_RATE_18M:
                currentMaxRate = 18000000;
                break;
            case WIFI_PHY_RATE_9M:
                currentMaxRate = 9000000;
                break;
            case WIFI_PHY_RATE_MCS0_LGI:
                currentMaxRate = 6500000;
                break;
            case WIFI_PHY_RATE_MCS1_LGI:
                currentMaxRate = 13000000;
                break;
            case WIFI_PHY_RATE_MCS2_LGI:
                currentMaxRate = 19500000;
                break;
            case WIFI_PHY_RATE_MCS3_LGI:
                currentMaxRate = 26000000;
                break;
            case WIFI_PHY_RATE_MCS4_LGI:
                currentMaxRate = 39000000;
                break;
            case WIFI_PHY_RATE_MCS5_LGI:
                currentMaxRate = 52000000;
                break;
            case WIFI_PHY_RATE_MCS6_LGI:
                currentMaxRate = 58500000;
                break;
            case WIFI_PHY_RATE_MCS7_LGI:
                currentMaxRate = 65000000;
                break;
            case WIFI_PHY_RATE_MCS0_SGI:
                currentMaxRate = 7200000;
                break;
            case WIFI_PHY_RATE_MCS1_SGI:
                currentMaxRate = 14400000;
                break;
            case WIFI_PHY_RATE_MCS2_SGI:
                currentMaxRate = 21700000;
                break;
            case WIFI_PHY_RATE_MCS3_SGI:
                currentMaxRate = 28900000;
                break;
            case WIFI_PHY_RATE_MCS4_SGI:
                currentMaxRate = 43300000;
                break;
            case WIFI_PHY_RATE_MCS5_SGI:
                currentMaxRate = 57800000;
                break;
            case WIFI_PHY_RATE_MCS6_SGI:
                currentMaxRate = 65000000;
                break;
            case WIFI_PHY_RATE_MCS7_SGI:
                currentMaxRate = 72200000;
                break;
            case WIFI_PHY_RATE_LORA_250K:
                currentMaxRate = 250000;
                break;
            case WIFI_PHY_RATE_LORA_500K:
                currentMaxRate = 500000;
                break;
            default:
                currentMaxRate = 0;
            break;
        }

        return CHIP_NO_ERROR;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketMulticastRxCount(uint32_t & packetMulticastRxCount)
{
    esp_err_t err = ESP_OK;
    uint32_t u_rev_count;
    uint32_t m_rev_count;

    packetMulticastRxCount = 0;

    for (uint8_t i = 0; i < 4; i++)
    {
        err |= esp_wifi_internal_get_rx_data_statis(i, &u_rev_count, &m_rev_count);
        if (err != ESP_OK)
        {
            break;
        }
        printf("%s---%d--u_rev_count:%ld\n", __FUNCTION__, __LINE__, m_rev_count);
        packetMulticastRxCount += m_rev_count;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketMulticastTxCount(uint32_t & packetMulticastTxCount)
{
    esp_err_t err = ESP_OK;
    uint32_t u_tx_total_count;
    uint32_t u_tx_retry_count;
    uint32_t u_tx_success_count;
    uint32_t u_tx_fail_count;
    uint32_t m_tx_count;

    packetMulticastTxCount = 0;

    for (uint8_t i = 0; i < 4; i++)
    {
        err |= esp_wifi_internal_get_tx_data_statis(i, &u_tx_total_count, &u_tx_retry_count, &u_tx_success_count, &u_tx_fail_count, &m_tx_count);
        if (err != ESP_OK)
        {
            break;
        }
        printf("%s---%d--m_tx_count:%ld\n", __FUNCTION__, __LINE__, m_tx_count);
        packetMulticastTxCount += m_tx_count;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketUnicastRxCount(uint32_t & packetUnicastRxCount)
{
    esp_err_t err = ESP_OK;
    uint32_t u_rev_count;
    uint32_t m_rev_count;

    packetUnicastRxCount = 0;

    for (uint8_t i = 0; i < 4; i++)
    {
        err |= esp_wifi_internal_get_rx_data_statis(i, &u_rev_count, &m_rev_count);
        if (err != ESP_OK)
        {
            break;
        }
        printf("%s---%d--u_rev_count:%ld\n", __FUNCTION__, __LINE__, u_rev_count);
        packetUnicastRxCount += u_rev_count;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketUnicastTxCount(uint32_t & packetUnicastTxCount)
{
    esp_err_t err = ESP_OK;
    uint32_t u_tx_total_count;
    uint32_t u_tx_retry_count;
    uint32_t u_tx_success_count;
    uint32_t u_tx_fail_count;
    uint32_t m_tx_count;

    packetUnicastTxCount = 0;

    for (uint8_t i = 0; i < 4; i++)
    {
        err |= esp_wifi_internal_get_tx_data_statis(i, &u_tx_total_count, &u_tx_retry_count, &u_tx_success_count, &u_tx_fail_count, &m_tx_count);
        if (err != ESP_OK)
        {
            break;
        }
        printf("%s---%d--u_tx_total_count:%ld--packetUnicastTxCount:%ld\n", __FUNCTION__, __LINE__, u_tx_total_count, packetUnicastTxCount);
        packetUnicastTxCount += u_tx_total_count;
    }

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiOverrunCount(uint64_t & overrunCount)
{
    esp_err_t err = ESP_OK;

    overrunCount = 0;

    return ESP32Utils::MapError(err);
}

CHIP_ERROR DiagnosticDataProviderImpl::ResetWiFiNetworkDiagnosticsCounts()
{
    esp_err_t err;

    err = esp_wifi_internal_reset_tx_data_statis();
    err |= esp_wifi_internal_reset_rx_data_statis();
    err |= esp_wifi_internal_reset_tx_statis();
    err |= esp_wifi_internal_reset_beacon_statis();

    if (err == ESP_OK)
    {
        return CHIP_NO_ERROR;
    }

    return ESP32Utils::MapError(err);
}
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI

DiagnosticDataProvider & GetDiagnosticDataProviderImpl()
{
    return DiagnosticDataProviderImpl::GetDefaultInstance();
}

} // namespace DeviceLayer
} // namespace chip
