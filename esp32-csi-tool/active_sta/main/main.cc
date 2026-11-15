#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_eap_client.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "../../_components/nvs_component.h"
#include "../../_components/sd_component.h"
#include "../../_components/csi_component.h"
#include "../../_components/time_component.h"
#include "../../_components/input_component.h"
#include "../../_components/sockets_component.h"

/*
 * The examples use WiFi configuration that you can set via 'idf.py menuconfig'.
 *
 * If you'd rather not, just change the below entries to strings with
 * the config you want - ie #define ESP_WIFI_SSID "mywifissid"
 */
#define ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD

/* Optional, git-ignored: main/eap_credentials.h
 *
 *     #define EAP_IDENTITY "user@institution.edu"
 *     #define EAP_USERNAME "user@institution.edu"
 *     #define EAP_PASSWORD "..."
 *
 * Overrides the menuconfig values, so the password never lands in sdkconfig.
 */
#if __has_include("eap_credentials.h")
#include "eap_credentials.h"
#endif
#ifndef EAP_IDENTITY
#define EAP_IDENTITY CONFIG_ESP_EAP_IDENTITY
#endif
#ifndef EAP_USERNAME
#define EAP_USERNAME CONFIG_ESP_EAP_USERNAME
#endif
#ifndef EAP_PASSWORD
#define EAP_PASSWORD CONFIG_ESP_EAP_PASSWORD
#endif

#ifdef CONFIG_WIFI_CHANNEL
#define WIFI_CHANNEL CONFIG_WIFI_CHANNEL
#else
#define WIFI_CHANNEL 6
#endif

#ifdef CONFIG_SHOULD_COLLECT_CSI
#define SHOULD_COLLECT_CSI 1
#else
#define SHOULD_COLLECT_CSI 0
#endif

#ifdef CONFIG_SHOULD_COLLECT_ONLY_LLTF
#define SHOULD_COLLECT_ONLY_LLTF 1
#else
#define SHOULD_COLLECT_ONLY_LLTF 0
#endif

#ifdef CONFIG_SEND_CSI_TO_SERIAL
#define SEND_CSI_TO_SERIAL 1
#else
#define SEND_CSI_TO_SERIAL 0
#endif

#ifdef CONFIG_SEND_CSI_TO_SD
#define SEND_CSI_TO_SD 1
#else
#define SEND_CSI_TO_SD 0
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

static const char *TAG = "Active CSI collection (Station)";

esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (!real_time_set) {
                    char *data = (char *) malloc(evt->data_len + 1);
                    strncpy(data, (char *) evt->data, evt->data_len);
                    data[evt->data_len + 1] = '\0';
                    time_set(data);
                    free(data);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

//// en_sys_seq: see https://github.com/espressif/esp-idf/blob/master/docs/api-guides/wifi.rst#wi-fi-80211-packet-send for details
esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);

/* The scan-at-boot diagnostic needs the radio idle: esp_wifi refuses a scan
 * while a connect is in flight, and STA_START fires a connect immediately. So
 * the event handler only connects once this flag is set. */
static volatile bool s_connect_enabled = false;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_connect_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "Retry connecting to the AP (reason %d)", d ? d->reason : -1);
        if (s_connect_enabled) {
            esp_wifi_connect();
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool is_wifi_connected() {
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT);
}

void station_init() {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /* Keep 802.1X credentials in RAM, not NVS: a stale enterprise blob in
     * NVS otherwise survives a reflash and quietly overrides this config. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_sta_config_t wifi_sta_config = {};
    /* In STA mode `channel` is a "only look here" hint; a fixed 6 would
     * silently miss an AP on any other channel. Enterprise networks have no
     * reason to pin it, so scan the whole band. */
#if CONFIG_ESP_WIFI_ENTERPRISE
    wifi_sta_config.channel = 0;
    wifi_sta_config.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
#else
    wifi_sta_config.channel = WIFI_CHANNEL;
    wifi_sta_config.threshold.authmode = WIFI_AUTH_WPA2_PSK;
#endif
    wifi_config_t wifi_config = {
            .sta = wifi_sta_config,
    };

    strlcpy((char *) wifi_config.sta.ssid, ESP_WIFI_SSID, sizeof(ESP_WIFI_SSID));
    strlcpy((char *) wifi_config.sta.password, ESP_WIFI_PASS, sizeof(ESP_WIFI_PASS));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

#if CONFIG_ESP_WIFI_ENTERPRISE
    if (strlen(EAP_IDENTITY) == 0 || strlen(EAP_USERNAME) == 0 ||
        strlen(EAP_PASSWORD) == 0) {
        ESP_LOGE(TAG, "enterprise mode but EAP identity/username/password is "
                      "empty; set them in menuconfig or main/eap_credentials.h");
    } else {
        /* eduroam is EAP-PEAP carrying MSCHAPv2 inside the TLS tunnel. No CA
         * is pinned here, so the server certificate is NOT verified: fine on
         * a bench, not something to ship. Add esp_eap_client_set_ca_cert()
         * (or esp_eap_client_use_default_cert_bundle(true)) for real use. */
        ESP_ERROR_CHECK(esp_eap_client_set_identity(
            (const uint8_t *)EAP_IDENTITY, strlen(EAP_IDENTITY)));
        ESP_ERROR_CHECK(esp_eap_client_set_username(
            (const uint8_t *)EAP_USERNAME, strlen(EAP_USERNAME)));
        ESP_ERROR_CHECK(esp_eap_client_set_password(
            (const uint8_t *)EAP_PASSWORD, strlen(EAP_PASSWORD)));
        ESP_ERROR_CHECK(esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP));
        ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
        ESP_LOGI(TAG, "enterprise login enabled as %s", EAP_USERNAME);
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_ps(WIFI_PS_NONE);

#if CONFIG_ESP_WIFI_ENTERPRISE
    ESP_LOGI(TAG, "connecting to SSID:%s as %s", ESP_WIFI_SSID, EAP_USERNAME);
#else
    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
#endif
}

TaskHandle_t xHandle = NULL;

void vTask_socket_transmitter_sta_loop(void *pvParameters) {
    for (;;) {
        socket_transmitter_sta_loop(&is_wifi_connected);
    }
}

void config_print() {
    printf("\n\n\n\n\n\n\n\n");
    printf("-----------------------\n");
    printf("ESP32 CSI Tool Settings\n");
    printf("-----------------------\n");
    printf("PROJECT_NAME: %s\n", "ACTIVE_STA");
    printf("CONFIG_ESPTOOLPY_MONITOR_BAUD: %d\n", CONFIG_ESPTOOLPY_MONITOR_BAUD);
    printf("CONFIG_ESP_CONSOLE_UART_BAUDRATE: %d\n", CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    printf("IDF_VER: %s\n", IDF_VER);
    printf("-----------------------\n");
    printf("WIFI_CHANNEL: %d\n", WIFI_CHANNEL);
    printf("ESP_WIFI_SSID: %s\n", ESP_WIFI_SSID);
#if CONFIG_ESP_WIFI_ENTERPRISE
    printf("WIFI_ENTERPRISE: 1\n");
    printf("EAP_IDENTITY: %s\n", EAP_IDENTITY);
    printf("EAP_USERNAME: %s\n", EAP_USERNAME);
    printf("EAP_PASSWORD: %s\n", strlen(EAP_PASSWORD) ? "(set)" : "(EMPTY)");
#else
    printf("WIFI_ENTERPRISE: 0\n");
    printf("ESP_WIFI_PASSWORD: %s\n", ESP_WIFI_PASS);
#endif
    printf("PACKET_RATE: %i\n", CONFIG_PACKET_RATE);
    printf("SHOULD_COLLECT_CSI: %d\n", SHOULD_COLLECT_CSI);
    printf("SHOULD_COLLECT_ONLY_LLTF: %d\n", SHOULD_COLLECT_ONLY_LLTF);
    printf("SEND_CSI_TO_SERIAL: %d\n", SEND_CSI_TO_SERIAL);
    printf("SEND_CSI_TO_SD: %d\n", SEND_CSI_TO_SD);
    printf("-----------------------\n");
    printf("\n\n\n\n\n\n\n\n");
}


/* One-shot scan dump: tells us whether the SSID we want is even audible from
 * this board, and with which authmode. Printed once at boot, before the
 * connect retry loop starts spamming reason 201. */
static void wifi_scan_dump(void) {
    wifi_scan_config_t scan = {};
    scan.show_hidden = true;
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        return;
    }
    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK || n == 0) {
        ESP_LOGW(TAG, "scan: no APs visible");
        return;
    }
    wifi_ap_record_t *recs = (wifi_ap_record_t *) calloc(n, sizeof(*recs));
    if (recs == NULL) {
        return;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    ESP_LOGI(TAG, "scan: %u APs visible", (unsigned) n);
    for (int i = 0; i < n; i++) {
        /* Wanted SSID is flagged; the rest are listed too so it is obvious
         * when it is simply absent from the 2.4 GHz band. */
        ESP_LOGW(TAG, "%s '%s' ch%u rssi %d auth %d",
                 strstr((const char *) recs[i].ssid, ESP_WIFI_SSID) ? "MATCH" : "AP   ",
                 (const char *) recs[i].ssid, recs[i].primary,
                 recs[i].rssi, (int) recs[i].authmode);
    }
    free(recs);
}

/* Scan first, then hand the radio back to the normal connect path. */
static void wifi_scan_report(void) {
    wifi_scan_dump();
    s_connect_enabled = true;
    esp_wifi_connect();
}

extern "C" void app_main() {
    config_print();
    nvs_init();
    sd_init();
    station_init();
    csi_init((char *) "STA");
    wifi_scan_report();

#if !(SHOULD_COLLECT_CSI)
    printf("CSI will not be collected. Check `idf.py menuconfig  # > ESP32 CSI Tool Config` to enable CSI");
#endif

    /* The ESP32-C6 is unicore (CONFIG_FREERTOS_UNICORE=y), so pinning to core 1
     * trips an assert in xTaskCreatePinnedToCore. Use the last core that
     * actually exists. */
    BaseType_t created = xTaskCreatePinnedToCore(
        &vTask_socket_transmitter_sta_loop, "socket_transmitter_sta_loop",
        10000, (void *) &is_wifi_connected, 5, &xHandle,
        portNUM_PROCESSORS - 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create socket transmitter task");
    }
}
