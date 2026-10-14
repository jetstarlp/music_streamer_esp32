#include "net_qemu.h"

#include "sdkconfig.h"

#if CONFIG_MS_NET_QEMU_OPENETH

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/semphr.h"

static const char *TAG = "net_qemu";

static SemaphoreHandle_t s_got_ip;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
    xSemaphoreGive(s_got_ip);
}

esp_err_t net_qemu_start(TickType_t timeout)
{
    s_got_ip = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_got_ip, ESP_ERR_NO_MEM, TAG, "semaphore");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "netif");

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;      /* nothing to negotiate in the emulator */
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, TAG, "mac/phy");

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &eth), TAG, "driver install");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)), TAG, "attach");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL, NULL),
                        TAG, "handler");
    ESP_RETURN_ON_ERROR(esp_eth_start(eth), TAG, "start");

    ESP_LOGW(TAG, "QEMU test build: emulated Ethernet instead of WiFi");
    return xSemaphoreTake(s_got_ip, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

#endif /* CONFIG_MS_NET_QEMU_OPENETH */
