/*
 * Home Accessory Architect
 *
 * Copyright 2019-2021 José Antonio Jiménez Campos (@RavenSystem)
 *
 */

/*
 * Based on esp-wifi-config library by Maxim Kulkin (@MaximKulkin), licensed under the MIT License.
 * https://github.com/maximkulkin/esp-wifi-config
 */

#include <stdio.h>
#include <string.h>
#include <sysparam.h>
#include <FreeRTOS.h>
#include <esplibs/libmain.h>
#include <espressif/esp_common.h>

#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>
#include <lwip/dhcp.h>
#include <lwip/etharp.h>

#include <spiflash.h>

#include <semphr.h>

#include <http-parser/http_parser.h>
#include <dhcpserver.h>

#include "form_urlencoded.h"

#include <rboot-api.h>
#include <homekit/homekit.h>

#include <timers_helper.h>

#include "header.h"

#define SETUP_ANNOUNCER_DESTINATION     "255.255.255.255"
#define SETUP_ANNOUNCER_PORT            "4567"


typedef enum {
    ENDPOINT_UNKNOWN = 0,
    ENDPOINT_INDEX,
    ENDPOINT_SETTINGS,
    ENDPOINT_SETTINGS_UPDATE
} endpoint_t;

typedef struct _wifi_network_info {
    char ssid[33];
    uint8_t bssid[6];
    char rssi[4];
    char channel[3];
    bool secure;

    struct _wifi_network_info *next;
} wifi_network_info_t;

typedef struct {
    char* ssid_prefix;
    char* password;
    char* custom_hostname;
    int param;
    void (*on_wifi_ready)();
    
    struct addrinfo* res;

    TimerHandle_t auto_reboot_timer;
    
    TaskHandle_t sta_connect_timeout;
    TaskHandle_t setup_announcer;
    
    wifi_network_info_t* wifi_networks;
    SemaphoreHandle_t wifi_networks_mutex;

    uint8_t check_counter;
    
    bool end_setup: 1;
} wifi_config_context_t;

static wifi_config_context_t* context;

typedef struct _client {
    int fd;

    http_parser parser;
    endpoint_t endpoint;
    uint8_t* body;
    size_t body_length;
} client_t;

static void wifi_config_station_connect();
static void wifi_config_softap_start();

int wifi_config_remove_sys_param() {
    unsigned char sector[SPI_FLASH_SECTOR_SIZE];
    
    for (uint8_t i = 0; i < SYSPARAMSIZE; i++) {
        if (!spiflash_erase_sector(SYSPARAMSECTOR + (i * SPI_FLASH_SECTOR_SIZE))) {
            ERROR("Erasing sysparam");
            return -1;
        }
        
        memset(sector, 0xFF, sizeof(sector));
        if (!spiflash_write(SYSPARAMSECTOR + (i * SPI_FLASH_SECTOR_SIZE), sector, sizeof(sector))) {
            ERROR("Format sysparam (1/2)");
            return -1;
        }
        
        memset(sector, 0x0, sizeof(sector));
        if (!spiflash_write(SYSPARAMSECTOR + (i * SPI_FLASH_SECTOR_SIZE), sector, sizeof(sector))) {
            ERROR("Format sysparam (2/2)");
            return -1;
        }
    }
    
    return 0;
}

static void body_malloc(client_t* client) {
    uint16_t body_size = MAX_BODY_LEN;
    do {
        client->body = malloc(body_size);
        body_size -= 200;
    } while (!client->body);
}

static client_t *client_new() {
    client_t *client = malloc(sizeof(client_t));
    memset(client, 0, sizeof(client_t));
    
    body_malloc(client);

    http_parser_init(&client->parser, HTTP_REQUEST);
    client->parser.data = client;

    return client;
}

static void client_free(client_t* client) {
    if (!client) {
        return;
    }
    
    if (client->body) {
        free(client->body);
    }

    free(client);
    client = NULL;
}

static void client_send(client_t *client, const char *payload, size_t payload_size) {
    lwip_write(client->fd, payload, payload_size);
}

static void client_send_chunk(client_t *client, const char *payload) {
    int len = strlen(payload);
    char buffer[10];
    int buffer_len = snprintf(buffer, sizeof(buffer), "%x\r\n", len);
    client_send(client, buffer, buffer_len);
    client_send(client, payload, len);
    client_send(client, "\r\n", 2);
}

static void client_send_redirect(client_t *client, int code, const char *redirect_url) {
    INFO("Redirecting to %s", redirect_url);
    char buffer[128];
    size_t len = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %d \r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, redirect_url);
    client_send(client, buffer, len);
}

IRAM bool wifi_config_got_ip() {
    struct ip_info info;
    if (sdk_wifi_get_ip_info(STATION_IF, &info) && ip4_addr1_16(&info.ip) != 0) {
        return true;
    }
        
    return false;
}

void wifi_config_resend_arp() {
    struct netif *netif = sdk_system_get_netif(STATION_IF);
    if (netif && (netif->flags & NETIF_FLAG_LINK_UP) && (netif->flags & NETIF_FLAG_UP)) {
        LOCK_TCPIP_CORE();
        int res = etharp_gratuitous(netif);
        UNLOCK_TCPIP_CORE();
        if (res != 0) {
            ERROR("ARP Gratuitous %i", res);
        }
    }
}

static void wifi_smart_connect_task(void* arg) {
    uint8_t *best_bssid = arg;
    
    INFO("Best BSSID: %02x%02x%02x%02x%02x%02x", best_bssid[0], best_bssid[1], best_bssid[2], best_bssid[3], best_bssid[4], best_bssid[5]);
    
    sysparam_set_data(WIFI_BSSID_SYSPARAM, best_bssid, (size_t) 6, true);
    
    sdk_wifi_station_disconnect();
    
    char* wifi_ssid = NULL;
    sysparam_get_string(WIFI_SSID_SYSPARAM, &wifi_ssid);
    
    struct sdk_station_config sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    
    strncpy((char*) sta_config.ssid, wifi_ssid, sizeof(sta_config.ssid));
    sta_config.ssid[sizeof(sta_config.ssid) - 1] = 0;

    char *wifi_password = NULL;
    sysparam_get_string(WIFI_PASSWORD_SYSPARAM, &wifi_password);
    if (wifi_password) {
       strncpy((char *)sta_config.password, wifi_password, sizeof(sta_config.password));
    }
    
    sta_config.bssid_set = 1;
    memcpy(sta_config.bssid, best_bssid, 6);
    
    sdk_wifi_station_set_config(&sta_config);
    sdk_wifi_station_set_auto_connect(true);
    sdk_wifi_station_connect();
    
    free(wifi_ssid);
    free(best_bssid);
    
    if (wifi_password) {
        free(wifi_password);
    }
    
    vTaskDelete(NULL);
}

static void wifi_scan_sc_done(void* arg, sdk_scan_status_t status) {
    if (status != SCAN_OK) {
        ERROR("Wifi smart connect scan");
        if (!wifi_config_got_ip()) {
            sdk_wifi_station_connect();
        }
    }

    char* wifi_ssid = NULL;
    sysparam_get_string(WIFI_SSID_SYSPARAM, &wifi_ssid);
    
    if (!wifi_ssid) {
        return;
    }
    
    INFO("Searching best BSSID for %s", wifi_ssid);
    
    struct sdk_bss_info* bss = (struct sdk_bss_info*) arg;
    // first one is invalid
    bss = bss->next.stqe_next;

    int8_t best_rssi = INT8_MIN;
    uint8_t* best_bssid = malloc(6);
    bool found = false;
    while (bss) {
        if (strcmp(wifi_ssid, (char*) bss->ssid) == 0) {
            INFO("RSSI %i, Ch %i - %02x%02x%02x%02x%02x%02x", bss->rssi, bss->channel, bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5]);

            if (bss->rssi > (best_rssi + BEST_RSSI_MARGIN)) {
                found = true;
                best_rssi = bss->rssi;
                memcpy(best_bssid, bss->bssid, 6);
            }
        }
        
        bss = bss->next.stqe_next;
    }
    
    if (best_rssi == INT8_MIN) {
        free(wifi_ssid);
        sdk_wifi_station_connect();
        return;
    }
    
    uint8_t *wifi_bssid = NULL;
    size_t len = 6;
    bool is_binary = true;
    sysparam_get_data(WIFI_BSSID_SYSPARAM, &wifi_bssid, &len, &is_binary);
    
    free(wifi_ssid);
    
    if (found) {
        if (wifi_bssid && memcmp(best_bssid, wifi_bssid, 6) == 0) {
            INFO("Best BSSID is the same");
            free(wifi_bssid);
            if (!wifi_config_got_ip()) {
                sdk_wifi_station_connect();
            }
            return;
        }
        
        if (xTaskCreate(wifi_smart_connect_task, "wifi_smart", 512, (void*) best_bssid, (tskIDLE_PRIORITY + 1), NULL) == pdPASS) {
            if (wifi_bssid) {
                free(wifi_bssid);
            }
            return;
        }
    }
    
    if (wifi_bssid) {
        free(wifi_bssid);
    }
    
    sdk_wifi_station_connect();
}

static void wifi_scan_sc_task(void* arg) {
    INFO("Start Wifi smart connect scan");
    vTaskDelay(MS_TO_TICKS(2000));
    sdk_wifi_station_scan(NULL, wifi_scan_sc_done);
    vTaskDelete(NULL);
}

void wifi_config_smart_connect() {
    int8_t wifi_mode = 0;
    sysparam_get_int8(WIFI_MODE_SYSPARAM, &wifi_mode);
    
    if (wifi_mode < 2 || xTaskCreate(wifi_scan_sc_task, "wifi_scan_smart", 384, NULL, (tskIDLE_PRIORITY + 2), NULL) != pdPASS) {
        if (!wifi_config_got_ip()) {
            sdk_wifi_station_connect();
        }
    }
}

uint8_t wifi_config_connect(const uint8_t mode);
void wifi_config_reset() {
    INFO("Wifi reset");
    sdk_wifi_station_disconnect();
    
    struct sdk_station_config sta_config;
    memset(&sta_config, 0, sizeof(sta_config));

    strncpy((char*) sta_config.ssid, "none", sizeof(sta_config.ssid));
    sta_config.ssid[sizeof(sta_config.ssid) - 1] = 0;
    
    sdk_wifi_station_set_config(&sta_config);
    sdk_wifi_station_set_auto_connect(false);
    sdk_wifi_station_connect();
}

static void wifi_networks_free() {
    wifi_network_info_t* wifi_network = context->wifi_networks;
    while (wifi_network) {
        wifi_network_info_t* next = wifi_network->next;
        free(wifi_network);
        wifi_network = next;
    }
    context->wifi_networks = NULL;
}

static void wifi_scan_done_cb(void *arg, sdk_scan_status_t status) {
    if (status != SCAN_OK || !context) {
        ERROR("Wifi scan");
        return;
    }
    
    xSemaphoreTake(context->wifi_networks_mutex, portMAX_DELAY);

    wifi_networks_free();

    struct sdk_bss_info *bss = (struct sdk_bss_info *)arg;
    // first one is invalid
    bss = bss->next.stqe_next;

    while (bss) {
        //INFO("%s (%i) Ch %i - %02x%02x%02x%02x%02x%02x", bss->ssid, bss->rssi, bss->channel, bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5]);

        wifi_network_info_t* net = context->wifi_networks;
        while (net) {
            if (!memcmp(net->bssid, bss->bssid, 6)) {
                break;
            }
            net = net->next;
        }
        
        if (!net) {
            wifi_network_info_t *net = malloc(sizeof(wifi_network_info_t));
            memset(net, 0, sizeof(*net));
            strncpy(net->ssid, (char *)bss->ssid, sizeof(net->ssid));
            memcpy(net->bssid, bss->bssid, 6);
            itoa(bss->rssi, net->rssi, 10);
            itoa(bss->channel, net->channel, 10);
            net->secure = bss->authmode != AUTH_OPEN;
            net->next = context->wifi_networks;

            context->wifi_networks = net;
        }

        bss = bss->next.stqe_next;
    }

    xSemaphoreGive(context->wifi_networks_mutex);
}

static void wifi_scan_task(void *arg) {
    INFO("Start Wifi scan");
    sdk_wifi_station_scan(NULL, wifi_scan_done_cb);
    vTaskDelete(NULL);
}

static void setup_announcer_task() {
    const struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_DGRAM,
    };
    
    while (getaddrinfo(SETUP_ANNOUNCER_DESTINATION, SETUP_ANNOUNCER_PORT, &hints, &context->res) != 0) {
        vTaskDelay(MS_TO_TICKS(200));
    }

    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(SOFTAP_IF, macaddr);
    char message[13];
    snprintf(message, 13, "HAA-%02X%02X%02X\r\n", macaddr[3], macaddr[4], macaddr[5]);
    
    for (;;) {
        int s = socket(context->res->ai_family, context->res->ai_socktype, 0);
        if (s >= 0) {
            lwip_sendto(s, message, strlen(message), 0, context->res->ai_addr, context->res->ai_addrlen);
            lwip_close(s);
        }
        
        vTaskDelay(MS_TO_TICKS(3000));
    }
}

#include "index.html.h"

static void wifi_config_server_on_settings(client_t *client) {
    esp_timer_change_period(context->auto_reboot_timer, AUTO_REBOOT_LONG_TIMEOUT);
    
    xTaskCreate(wifi_scan_task, "wifi_scan", 384, NULL, (tskIDLE_PRIORITY + 2), NULL);
    
    static const char http_prologue[] =
        "HTTP/1.1 200 \r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";

    client_send(client, http_prologue, sizeof(http_prologue) - 1);
    client_send_chunk(client, html_settings_header);
    
    char *text = NULL;
    sysparam_status_t status;
    status = sysparam_get_string(HAA_JSON_SYSPARAM, &text);
    if (status == SYSPARAM_OK) {
        client_send_chunk(client, text);
        free(text);
        text = NULL;
    }
    client_send_chunk(client, html_settings_middle);
    
    if (context->param == 1) {
        client_send_chunk(client, "<div style=\"color:red\">FLASH corrupted! Reset settings recommended</div>");
    }
    client_send_chunk(client, html_settings_flash_error);
    
    status = sysparam_get_string(OTA_VERSION_SYSPARAM, &text);
    if (text) {
        client_send_chunk(client, text);
        free(text);
        text = NULL;
    }
    client_send_chunk(client, html_settings_otaversion);
    
    bool auto_ota = false;
    status = sysparam_get_bool(AUTO_OTA_SYSPARAM, &auto_ota);
    if (status == SYSPARAM_OK && auto_ota) {
        client_send_chunk(client, "checked");
    }
    client_send_chunk(client, html_settings_autoota);
    
    int8_t int8_value = 0;
    sysparam_get_int8(WIFI_MODE_SYSPARAM, &int8_value);
    if (int8_value == 0) {
        client_send_chunk(client, "selected");
    }
    client_send_chunk(client, html_wifi_mode_0);
    
    if (int8_value == 1) {
        client_send_chunk(client, "selected");
    }
    client_send_chunk(client, html_wifi_mode_1);
    
    if (int8_value == 2) {
        client_send_chunk(client, "selected");
    }
    client_send_chunk(client, html_wifi_mode_2);
    
    if (int8_value == 3) {
        client_send_chunk(client, "selected");
    }
    client_send_chunk(client, html_wifi_mode_3);
    
    if (int8_value == 4) {
        client_send_chunk(client, "selected");
    }
    client_send_chunk(client, html_wifi_mode_4);
    
    // Wifi Networks
    char buffer[125];
    char bssid[13];
    if (xSemaphoreTake(context->wifi_networks_mutex, MS_TO_TICKS(5000))) {
        wifi_network_info_t* net = context->wifi_networks;
        while (net) {
            snprintf(bssid, 13, "%02x%02x%02x%02x%02x%02x", net->bssid[0], net->bssid[1], net->bssid[2], net->bssid[3], net->bssid[4], net->bssid[5]);
            snprintf(
                buffer, sizeof(buffer),
                html_network_item,
                net->secure ? "secure" : "unsecure", bssid, net->ssid, net->ssid, net->rssi, net->channel, bssid
            );
            client_send_chunk(client, buffer);

            net = net->next;
        }

        xSemaphoreGive(context->wifi_networks_mutex);
    }

    client_send_chunk(client, html_settings_wifi);
    
    // Custom repo server
    status = sysparam_get_string(CUSTOM_REPO_SYSPARAM, &text);
    if (status == SYSPARAM_OK) {
        client_send_chunk(client, text);
        free(text);
    }
    client_send_chunk(client, html_settings_reposerver);
    
    int32_t int32_value;
    
    status = sysparam_get_int32(PORT_NUMBER_SYSPARAM, &int32_value);
    if (status == SYSPARAM_OK) {
        char str_port[6];
        itoa(int32_value, str_port, 10);
        client_send_chunk(client, str_port);
    } else {
        client_send_chunk(client, "80");
    }
    client_send_chunk(client, html_settings_repoport);
    
    int8_value = 0;
    sysparam_get_int8(PORT_SECURE_SYSPARAM, &int8_value);
    if (int8_value == 1) {
        client_send_chunk(client, "checked");
    }
    client_send_chunk(client, html_settings_repossl);

    client_send_chunk(client, "");
}

static void wifi_config_context_free(wifi_config_context_t* context) {
    if (context->ssid_prefix) {
        free(context->ssid_prefix);
    }

    if (context->password) {
        free(context->password);
    }

    wifi_networks_free();

    free(context);
    context = NULL;
}

static void wifi_config_server_on_settings_update_task(void* args) {
    client_t* client = args;
    context->end_setup = true;
    
    while (context->end_setup) {
        vTaskDelay(MS_TO_TICKS(200));
    }
    
    sdk_wifi_station_disconnect();
    
    INFO("Update settings");
    
    wifi_config_context_free(context);
    
    form_param_t *form = form_params_parse((char*) client->body);
    client_free(client);
    
    if (!form) {
        ERROR("No memory. Rebooting...");
        vTaskDelay(MS_TO_TICKS(3000));
        sdk_system_restart();
    }
    
    form_param_t *reset_sys_param = form_params_find(form, "reset_sys");
    form_param_t *reset_hk_param = form_params_find(form, "reset");
    
    if (reset_sys_param) {
        if (reset_hk_param) {
            homekit_server_reset();
        }
        
        wifi_config_remove_sys_param();
        vTaskDelay(MS_TO_TICKS(500));
        wifi_config_reset();
        
    } else {
        form_param_t *conf_param = form_params_find(form, "conf");
        form_param_t *nowifi_param = form_params_find(form, "nowifi");
        form_param_t *ota_param = form_params_find(form, "ota");
        form_param_t *autoota_param = form_params_find(form, "autoota");
        form_param_t *wifimode_param = form_params_find(form, "wifimode");
        form_param_t *irrx_param = form_params_find(form, "irrx");
        form_param_t *ssid_param = form_params_find(form, "ssid");
        form_param_t *bssid_param = form_params_find(form, "bssid");
        form_param_t *password_param = form_params_find(form, "password");
        form_param_t *reposerver_param = form_params_find(form, "reposerver");
        form_param_t *repoport_param = form_params_find(form, "repoport");
        form_param_t *repossl_param = form_params_find(form, "repossl");
        
        // Remove saved states
        int hk_total_ac = 0;
        sysparam_get_int32(TOTAL_SERV_SYSPARAM, &hk_total_ac);
        char saved_state_id[5];
        memset(saved_state_id, 0, 5);
        for (uint32_t int_saved_state_id = 100; int_saved_state_id <= hk_total_ac * 100; int_saved_state_id++) {
            itoa(int_saved_state_id, saved_state_id, 10);
            sysparam_set_data(saved_state_id, NULL, 0, false);
        }
        
        if (conf_param && conf_param->value) {
            sysparam_set_string(HAA_JSON_SYSPARAM, conf_param->value);
        } else {
            sysparam_set_string(HAA_JSON_SYSPARAM, "");
        }
        
        if (autoota_param) {
            sysparam_set_bool(AUTO_OTA_SYSPARAM, true);
        } else {
            sysparam_set_bool(AUTO_OTA_SYSPARAM, false);
        }
        
        if (ota_param) {
            rboot_set_temp_rom(1);
        }
        
        if (nowifi_param) {
            sysparam_set_data(WIFI_SSID_SYSPARAM, NULL, 0, false);
            sysparam_set_data(WIFI_PASSWORD_SYSPARAM, NULL, 0, false);
        }
        
        if (irrx_param && irrx_param->value) {
            const int8_t irrx_gpio = (strtol(irrx_param->value, NULL, 10)) + 100;
            if (irrx_gpio >= 100) {
                sysparam_set_int8(HAA_SETUP_MODE_SYSPARAM, irrx_gpio);
            }
        }
        
        int last_config_number = 0;
        sysparam_get_int32(LAST_CONFIG_NUMBER_SYSPARAM, &last_config_number);
        last_config_number++;
        if (last_config_number > 65535) {
            last_config_number = 1;
        }
        
        if (reset_hk_param) {
            homekit_server_reset();
            last_config_number = 1;
        }
        sysparam_set_int32(LAST_CONFIG_NUMBER_SYSPARAM, last_config_number);
        
        if (reposerver_param && reposerver_param->value) {
            sysparam_set_string(CUSTOM_REPO_SYSPARAM, reposerver_param->value);
        } else {
            sysparam_set_string(CUSTOM_REPO_SYSPARAM, "");
        }
        
        if (repoport_param && repoport_param->value) {
            const int32_t port = strtol(repoport_param->value, NULL, 10);
            sysparam_set_int32(PORT_NUMBER_SYSPARAM, port);
        } else {
            sysparam_set_int32(PORT_NUMBER_SYSPARAM, 80);
        }
        
        if (repossl_param) {
            sysparam_set_int8(PORT_SECURE_SYSPARAM, 1);
        } else {
            sysparam_set_int8(PORT_SECURE_SYSPARAM, 0);
        }
        
        if (ssid_param && ssid_param->value) {
            sysparam_set_string(WIFI_SSID_SYSPARAM, ssid_param->value);

            if (bssid_param && bssid_param->value && strlen(bssid_param->value) == 12) {
                uint8_t bssid[6];
                char hex[3];
                memset(hex, 0, 3);
                
                for (uint8_t i = 0; i < 6; i++) {
                    hex[0] = bssid_param->value[(i * 2)];
                    hex[1] = bssid_param->value[(i * 2) + 1];
                    bssid[i] = (uint8_t) strtol(hex, NULL, 16);
                }

                sysparam_set_data(WIFI_BSSID_SYSPARAM, bssid, (size_t) 6, true);
                
            } else {
                sysparam_set_data(WIFI_BSSID_SYSPARAM, NULL, 0, true);
            }
            
            if (password_param->value) {
                sysparam_set_string(WIFI_PASSWORD_SYSPARAM, password_param->value);
            } else {
                sysparam_set_string(WIFI_PASSWORD_SYSPARAM, "");
            }
        }
        
        sysparam_compact();
        
        if (wifimode_param && wifimode_param->value) {
            int8_t current_wifi_mode = 0;
            int8_t new_wifi_mode = strtol(wifimode_param->value, NULL, 10);
            sysparam_get_int8(WIFI_MODE_SYSPARAM, &current_wifi_mode);
            sysparam_set_int8(WIFI_MODE_SYSPARAM, new_wifi_mode);
            
            if (current_wifi_mode != new_wifi_mode) {
                vTaskDelay(MS_TO_TICKS(3000));
                wifi_config_reset();
            }
        }
    }

    INFO("\nRebooting...\n\n");
    
    vTaskDelay(MS_TO_TICKS(3000));
    
    sdk_system_restart();
}

static int wifi_config_server_on_url(http_parser *parser, const char *data, size_t length) {
    client_t *client = (client_t*) parser->data;

    client->endpoint = ENDPOINT_UNKNOWN;
    if (parser->method == HTTP_GET) {
        if (!strncmp(data, "/settings", length)) {
            client->endpoint = ENDPOINT_SETTINGS;
        } else if (!strncmp(data, "/", length)) {
            client->endpoint = ENDPOINT_INDEX;
        }
    } else if (parser->method == HTTP_POST) {
        if (!strncmp(data, "/settings", length)) {
            client->endpoint = ENDPOINT_SETTINGS_UPDATE;
        }
    }

    if (client->endpoint == ENDPOINT_UNKNOWN) {
        char *url = strndup(data, length);
        ERROR("Unknown: %s %s", http_method_str(parser->method), url);
        free(url);
    }

    return 0;
}

static int wifi_config_server_on_body(http_parser *parser, const char *data, size_t length) {
    client_t *client = parser->data;
    //client->body = realloc(client->body, client->body_length + length + 1);
    memcpy(client->body + client->body_length, data, length);
    client->body_length += length;
    client->body[client->body_length] = 0;

    return 0;
}

static int wifi_config_server_on_message_complete(http_parser *parser) {
    client_t *client = parser->data;

    switch(client->endpoint) {
        case ENDPOINT_INDEX: {
            client_send_redirect(client, 301, "/settings");
            break;
        }
            
        case ENDPOINT_SETTINGS: {
            wifi_config_server_on_settings(client);
            break;
        }
        
        case ENDPOINT_SETTINGS_UPDATE: {
            esp_timer_change_period(context->auto_reboot_timer, AUTO_REBOOT_LONG_TIMEOUT);
            if (context->sta_connect_timeout) {
                vTaskDelete(context->sta_connect_timeout);
            } else {
                sysparam_set_int8(HAA_SETUP_MODE_SYSPARAM, 0);
            }
            
            if (context->setup_announcer) {
                vTaskDelete(context->setup_announcer);
                if (context->res) {
                    freeaddrinfo(context->res);
                }
            }
            
            xTaskCreate(wifi_config_server_on_settings_update_task, "settings_update", 512, client, (tskIDLE_PRIORITY + 1), NULL);
            return 0;
        }
        
        case ENDPOINT_UNKNOWN: {
            INFO("Unknown");
            client_send_redirect(client, 302, "http://192.168.4.1:4567/settings");
            break;
        }
    }

    if (client->body) {
        free(client->body);
        client->body = NULL;
        client->body_length = 0;
    }

    return 0;
}

static http_parser_settings wifi_config_http_parser_settings = {
    .on_url = wifi_config_server_on_url,
    .on_body = wifi_config_server_on_body,
    .on_message_complete = wifi_config_server_on_message_complete,
};

static void http_task(void *arg) {
    INFO("Start HTTP server");
    
    context->end_setup = false;
    
    struct sockaddr_in serv_addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(WIFI_CONFIG_SERVER_PORT);
    
    bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(listenfd, 2);

    char data[128];

    for (;;) {
        int fd = accept(listenfd, (struct sockaddr *)NULL, (socklen_t *)NULL);
        if (fd < 0) {
            vTaskDelay(MS_TO_TICKS(200));
            continue;
        }
        
        const struct timeval rcvtimeout = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeout, sizeof(rcvtimeout));
        
        const int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

        const int interval = 5;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));

        const int maxpkt = 4;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(maxpkt));

        client_t *client = client_new();
        client->fd = fd;

        //int data_total = 0;
        
        for (;;) {
            int data_len = lwip_read(client->fd, data, sizeof(data));
            
            //data_total += data_len;
            //INFO("lwip_read %d, %d", data_len, data_total);

            if (data_len > 0) {
                http_parser_execute(
                    &client->parser, &wifi_config_http_parser_settings,
                    data, data_len
                );
            } else {
                break;
            }
        }
        
        if (context->end_setup) {
            static const char payload[] = "HTTP/1.1 200\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<center>OK</center>";
            client_send(client, payload, sizeof(payload) - 1);
            lwip_close(client->fd);
            break;
        }

        INFO("Client disconnected");
        
        lwip_close(client->fd);
        client_free(client);
    }
    
    context->end_setup = false;
    
    INFO("Stop HTTP server");
    vTaskDelete(NULL);
}

static void wifi_config_softap_start() {
    sdk_wifi_set_opmode(STATIONAP_MODE);

    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(SOFTAP_IF, macaddr);

    struct sdk_softap_config softap_config;
    softap_config.ssid_len = snprintf(
        (char *)softap_config.ssid, sizeof(softap_config.ssid),
        "%s-%02X%02X%02X", context->ssid_prefix, macaddr[3], macaddr[4], macaddr[5]
    );
    softap_config.ssid_hidden = 0;
    softap_config.channel = 6;
    if (context->password) {
        softap_config.authmode = AUTH_WPA_WPA2_PSK;
        strncpy((char *)softap_config.password,
                context->password, sizeof(softap_config.password));
    } else {
        softap_config.authmode = AUTH_OPEN;
    }
    softap_config.max_connection = 2;
    softap_config.beacon_interval = 100;

    INFO("Start AP SSID=%s", softap_config.ssid);

    struct ip_info ap_ip;
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    IP4_ADDR(&ap_ip.gw, 0, 0, 0, 0);
    sdk_wifi_set_ip_info(SOFTAP_IF, &ap_ip);

    sdk_wifi_softap_set_config(&softap_config);

    ip4_addr_t first_client_ip;
    first_client_ip.addr = ap_ip.ip.addr + htonl(1);

    context->wifi_networks_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(context->wifi_networks_mutex);

    INFO("Start DHCP server");
    dhcpserver_start(&first_client_ip, 4);
    //dhcpserver_set_router(&ap_ip.ip);
    //dhcpserver_set_dns(&ap_ip.ip);

    xTaskCreate(http_task, "http", 640, NULL, (tskIDLE_PRIORITY + 1), NULL);
}

static void wifi_config_sta_connect_timeout_task() {
    if (context->custom_hostname) {
        struct netif *netif = NULL;
        for (;;) {
            netif = sdk_system_get_netif(STATION_IF);
            if (netif) {
                break;
            }
            vTaskDelay(MS_TO_TICKS(100));
        }
        
        netif->hostname = context->custom_hostname;
        INFO("Set hostname");
    }

    for (;;) {
        vTaskDelay(MS_TO_TICKS(1000));
        
        if (sdk_wifi_station_get_connect_status() == STATION_GOT_IP) {
            if (context->on_wifi_ready) {
                context->on_wifi_ready();
                
                wifi_config_context_free(context);
                
            } else {
                LOCK_TCPIP_CORE();
                dhcpserver_stop();
                sdk_wifi_set_opmode(STATION_MODE);
                UNLOCK_TCPIP_CORE();
                
                xTaskCreate(setup_announcer_task, "setup_announcer", 512, NULL, (tskIDLE_PRIORITY + 0), &context->setup_announcer);
            }
            
            break;
            
        } else if (sdk_wifi_get_opmode() == STATION_MODE) {
            context->check_counter++;
            if (context->check_counter > 35) {
                context->check_counter = 0;
                wifi_config_reset();
                vTaskDelay(MS_TO_TICKS(5000));
                wifi_config_connect(0);
                vTaskDelay(MS_TO_TICKS(1000));
                
            } else if (context->check_counter % 13 == 0) {
                wifi_config_resend_arp();
            }
        }
    }
    
    if (context) {
        context->sta_connect_timeout = NULL;
    }
    vTaskDelete(NULL);
}

static void auto_reboot_run() {
    bool auto_ota = false;
    sysparam_get_bool(AUTO_OTA_SYSPARAM, &auto_ota);
    if (auto_ota) {
        INFO("Auto OTA Update enabled");
        rboot_set_temp_rom(1);
    }

    INFO("\nAuto Rebooting...\n");
    if (context->sta_connect_timeout) {
        vTaskDelete(context->sta_connect_timeout);
    }
    
    if (context->setup_announcer) {
        vTaskDelete(context->setup_announcer);
        if (context->res) {
            freeaddrinfo(context->res);
        }
    }
    
    vTaskDelay(MS_TO_TICKS(500));
    
    sdk_system_restart();
}

uint8_t wifi_config_connect(const uint8_t mode) {
    char *wifi_ssid = NULL;
    sysparam_get_string(WIFI_SSID_SYSPARAM, &wifi_ssid);
    
    if (wifi_ssid) {
        sdk_wifi_station_disconnect();
        
        struct sdk_station_config sta_config;
        memset(&sta_config, 0, sizeof(sta_config));
        
        strncpy((char*) sta_config.ssid, wifi_ssid, sizeof(sta_config.ssid));
        sta_config.ssid[sizeof(sta_config.ssid) - 1] = 0;

        char *wifi_password = NULL;
        sysparam_get_string(WIFI_PASSWORD_SYSPARAM, &wifi_password);
        if (wifi_password) {
           strncpy((char *)sta_config.password, wifi_password, sizeof(sta_config.password));
        }

        int8_t wifi_mode = 0;
        sysparam_get_int8(WIFI_MODE_SYSPARAM, &wifi_mode);
        
        uint8_t *wifi_bssid = NULL;
        size_t len = 6;
        bool is_binary = true;
        sysparam_get_data(WIFI_BSSID_SYSPARAM, &wifi_bssid, &len, &is_binary);
        if (wifi_bssid) {
            INFO("Saved BSSID: %02x%02x%02x%02x%02x%02x", wifi_bssid[0], wifi_bssid[1], wifi_bssid[2], wifi_bssid[3], wifi_bssid[4], wifi_bssid[5]);
        } else {
            INFO("Saved BSSID: none");
        }
        
        if (wifi_mode < 2 || (wifi_mode == 4 && mode == 1)) {
            if ((wifi_mode == 1 || (wifi_mode == 4 && mode == 1)) && wifi_bssid) {
                sta_config.bssid_set = 1;
                memcpy(sta_config.bssid, wifi_bssid, 6);
                INFO("Wifi Mode: Forced BSSID");
                
                //INFO("Wifi Mode: Forced BSSID %02x%02x%02x%02x%02x%02x", sta_config.bssid[0], sta_config.bssid[1], sta_config.bssid[2], sta_config.bssid[3], sta_config.bssid[4], sta_config.bssid[5]);

            } else {
                INFO("Wifi Mode: Normal");
                sta_config.bssid_set = 0;
            }

            sdk_wifi_set_opmode(STATION_MODE);
            sdk_wifi_station_set_config(&sta_config);
            sdk_wifi_station_set_auto_connect(true);
            sdk_wifi_station_connect();
            
        } else {
            INFO("Wifi Mode: Roaming");
            sysparam_set_data(WIFI_BSSID_SYSPARAM, NULL, 0, false);
            sdk_wifi_set_opmode(STATION_MODE);
            sdk_wifi_station_set_config(&sta_config);
            sdk_wifi_station_set_auto_connect(true);
            
            if (wifi_mode == 4) {
                xTaskCreate(wifi_scan_sc_task, "wifi_scan_smart", 384, NULL, (tskIDLE_PRIORITY + 2), NULL);
            } else {
                wifi_config_smart_connect();
            }
        }
        
        free(wifi_ssid);
        
        if (wifi_password) {
            free(wifi_password);
        }
        
        if (wifi_bssid) {
            free(wifi_bssid);
        }
        
        return 1;
        
    } else {
        INFO("No Wifi config found");
    }
    
    return 0;
}

static void wifi_config_station_connect() {
    if (wifi_config_connect(0) == 1) {
        xTaskCreate(wifi_config_sta_connect_timeout_task, "sta_connect", 512, NULL, (tskIDLE_PRIORITY + 1), &context->sta_connect_timeout);
        
        if (!context->on_wifi_ready) {
            INFO("HAA Setup");
            
            int8_t setup_mode = 0;
            sysparam_get_int8(HAA_SETUP_MODE_SYSPARAM, &setup_mode);
            
            sysparam_set_int8(HAA_SETUP_MODE_SYSPARAM, 0);

            if (setup_mode == 1) {
                INFO("Enabling auto reboot");
                context->auto_reboot_timer = esp_timer_create(AUTO_REBOOT_TIMEOUT, false, NULL, auto_reboot_run);
                esp_timer_start(context->auto_reboot_timer);
            }
            
            wifi_config_softap_start();
        }
        
    } else {
        wifi_config_softap_start();
    }
    
    vTaskDelete(NULL);
}

void wifi_config_init(const char* ssid_prefix, const char* password, void (*on_wifi_ready)(), const char* custom_hostname, const int param) {
    INFO("Wifi config init");
    if (password && strlen(password) < 8) {
        ERROR("Password");
        return;
    }

    context = malloc(sizeof(wifi_config_context_t));
    memset(context, 0, sizeof(*context));

    context->ssid_prefix = strndup(ssid_prefix, 33 - 7);
    if (password) {
        context->password = strdup(password);
    }
    
    if (custom_hostname) {
        context->custom_hostname = strdup(custom_hostname);
    }

    context->on_wifi_ready = on_wifi_ready;
    context->param = param;

    xTaskCreate(wifi_config_station_connect, "wifi_config", 512, NULL, (tskIDLE_PRIORITY + 1), NULL);
}
