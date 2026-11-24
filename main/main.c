#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "sys/time.h"
#include "nvs_flash.h"
#include "heartbeat.h"

#define AP_SSID                 CONFIG_ESP_WIFI_AP_SSID
#define AP_PASSWORD             CONFIG_ESP_WIFI_AP_PASSWORD
#define AP_CHANNEL              CONFIG_ESP_WIFI_AP_CHANNEL
#define AP_MAX_CLIENTS          CONFIG_ESP_MAX_STA_CONN_AP
#define CAMPUS_SSID             CONFIG_ESP_WIFI_REMOTE_AP_SSID
#define CAMPUS_PASSWORD         CONFIG_ESP_WIFI_REMOTE_AP_PASSWORD
#define MAX_DRIVER_RETRIES      CONFIG_ESP_MAXIMUM_STA_RETRY

#define AP_GATEWAY              "192.168.77.1"
#define CAMPUS_LOGIN_URL        "http://login.cqu.edu.cn"
#define INTERNET_PROBE_URL      "http://connectivitycheck.platform.hicloud.com/generate_204"
#define NAPT_TABLE_SIZE         512
#define DNS_MAIN_OFFER_OPTION   0x02
#define PROBE_INTERVAL_US       (30LL * 1000LL * 1000LL)
#define PROBE_RETRY_US          (5LL * 1000LL * 1000LL)
#define PROBE_TIMEOUT_MS        6000
#define CAMPUS_SCAN_INTERVAL_US (60LL * 1000LL * 1000LL)
#define DNS_PORT                53
#define DNS_PACKET_CAPACITY     1232
#define DNS_UPSTREAM_TIMEOUT_S  3
#define DNS_TASK_STACK_SIZE     4096
#define INITIAL_RETRY_US        (1LL * 1000LL * 1000LL)
#define MAX_RETRY_US            (30LL * 1000LL * 1000LL)
#define DHCP_TIMEOUT_US         (20LL * 1000LL * 1000LL)

#define EVENT_STA_CONNECT       BIT0
#define EVENT_STA_GOT_IP        BIT1
#define EVENT_STA_DISCONNECTED  BIT2
#define EVENT_STA_ASSOCIATED    BIT3
#define EVENT_STA_LOST_IP       BIT4
#define EVENT_AP_READY          BIT5

typedef enum {
    PROBE_CHECKING,
    PROBE_ONLINE,
    PROBE_PORTAL,
    PROBE_UNVERIFIED,
} probe_state_t;

static const char *const PROBE_STATE_NAMES[] = {
    [PROBE_CHECKING] = "checking",
    [PROBE_ONLINE] = "online",
    [PROBE_PORTAL] = "portal",
    [PROBE_UNVERIFIED] = "unverified",
};

extern const char status_html_start[] asm("_binary_status_html_start");

static const char *TAG = "cqu_a1_router";
static EventGroupHandle_t s_events;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static atomic_bool s_napt_enabled;
static atomic_bool s_campus_associated;
static atomic_bool s_campus_connected;
static atomic_uint s_link_generation;
static atomic_int s_probe_state;
static atomic_int s_last_http_status;
static atomic_uint s_last_disconnect_reason;
static atomic_uint_fast32_t s_campus_dns_address;

static bool campus_ssid_is_configured(void)
{
    return CAMPUS_SSID[0] != '\0' &&
           strcmp(CAMPUS_SSID, "SET-AFTER-SCANNING-CAMPUS-WIFI") != 0;
}

static void configure_ap_network(void)
{
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 77, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 77, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = ip_info.ip;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));

    uint8_t offer_main_dns = DNS_MAIN_OFFER_OPTION;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_main_dns, sizeof(offer_main_dns)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    ESP_LOGI(TAG, "Router address %s. Your phone and printer will use this private subnet.",
             AP_GATEWAY);
    ESP_LOGI(TAG, "Hotspot DHCP advertises a local DNS relay that follows the campus DNS assignment.");
}

static esp_err_t router_status_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, status_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t router_api_status_handler(httpd_req_t *request)
{
    const bool connected = atomic_load(&s_campus_connected);
    const probe_state_t probe = atomic_load(&s_probe_state);
    esp_netif_ip_info_t ip = {0};
    wifi_ap_record_t upstream = {0};
    wifi_sta_list_t clients = {0};
    const bool have_ip = connected && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK;
    const bool associated = esp_wifi_sta_get_ap_info(&upstream) == ESP_OK;
    const bool have_clients = esp_wifi_ap_get_sta_list(&clients) == ESP_OK;
    char address[16] = "";
    if (have_ip) {
        snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
    }
    const int64_t uptime = esp_timer_get_time() / 1000000;
    const int32_t last_heartbeat = heartbeat_last_success_seconds();
    char response[448];

    const int response_length = snprintf(
        response, sizeof(response),
        "{\"campus_connected\":%s,\"campus_online\":%s,\"nat_enabled\":%s,"
        "\"probe_state\":\"%s\",\"http_status\":%d,\"wan_ip\":\"%s\","
        "\"rssi\":%d,\"channel\":%u,\"clients\":%d,\"disconnect_reason\":%u,"
        "\"uptime_seconds\":%" PRId64 ",\"heartbeat_state\":\"%s\","
        "\"heartbeat_age_seconds\":%" PRId64 ",\"device_alert_state\":\"%s\"}\n",
        connected ? "true" : "false",
        connected && probe == PROBE_ONLINE ? "true" : "false",
        connected && atomic_load(&s_napt_enabled) ? "true" : "false",
        connected ? PROBE_STATE_NAMES[probe] : (associated ? "dhcp" : "offline"),
        connected ? atomic_load(&s_last_http_status) : 0,
        address, associated ? upstream.rssi : 0, associated ? upstream.primary : 0,
        have_clients ? clients.num : -1, atomic_load(&s_last_disconnect_reason),
        uptime, heartbeat_status(), last_heartbeat < 0 ? -1 : uptime - last_heartbeat,
        device_alert_status());

    if (response_length < 0 || (size_t)response_length >= sizeof(response)) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not format router status.");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, response_length);
}

static esp_err_t accept_hotspot_http_client(httpd_handle_t server, int socket_fd)
{
    (void)server;
    struct sockaddr_storage local = {0};
    socklen_t length = sizeof(local);
    if (getsockname(socket_fd, (struct sockaddr *)&local, &length) != 0) {
        return ESP_FAIL;
    }
    const uint32_t gateway = inet_addr(AP_GATEWAY);
    if (local.ss_family == AF_INET) {
        const struct sockaddr_in *ip = (const struct sockaddr_in *)&local;
        return ip->sin_addr.s_addr == gateway ? ESP_OK : ESP_FAIL;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *ip = (const struct sockaddr_in6 *)&local;
        if (IN6_IS_ADDR_V4MAPPED(&ip->sin6_addr) &&
            memcmp(&ip->sin6_addr.s6_addr[12], &gateway, sizeof(gateway)) == 0) {
            return ESP_OK;
        }
    }
#endif
    return ESP_FAIL;
}

static void start_status_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 2;
    config.lru_purge_enable = true;
    config.open_fn = accept_hotspot_http_client;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t status_page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = router_status_handler,
    };
    const httpd_uri_t api_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = router_api_status_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api_status));
    ESP_LOGI(TAG, "Status page available at http://%s/ after connecting to the router hotspot.",
             AP_GATEWAY);
}

static void update_dns_relay_upstream(void)
{
    esp_netif_dns_info_t dns = {0};
    uint32_t address = 0;
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 &&
        dns.ip.u_addr.ip4.addr != inet_addr(AP_GATEWAY)) {
        address = dns.ip.u_addr.ip4.addr;
    }

    const uint32_t previous = atomic_exchange(&s_campus_dns_address, address);
    if (address != 0 && address != previous) {
        ESP_LOGI(TAG, "Hotspot DNS is forwarding to the campus resolver.");
    }
}

static int forward_dns_query(uint32_t address, uint8_t *packet, size_t length)
{
    if (address == 0) {
        return -1;
    }
    const int upstream_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (upstream_socket < 0) {
        return -1;
    }

    const struct sockaddr_in upstream = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = address,
    };
    const struct timeval timeout = {.tv_sec = DNS_UPSTREAM_TIMEOUT_S};
    const uint16_t query_id = (packet[0] << 8) | packet[1];
    int response_length = -1;

    // A connected UDP socket only accepts responses from the configured resolver.
    if (setsockopt(upstream_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        connect(upstream_socket, (const struct sockaddr *)&upstream, sizeof(upstream)) == 0 &&
        send(upstream_socket, packet, length, 0) == (ssize_t)length) {
        const int received = recv(upstream_socket, packet, DNS_PACKET_CAPACITY, 0);
        if (received >= 12 && ((packet[0] << 8) | packet[1]) == query_id &&
            (packet[2] & 0x80U) != 0) {
            response_length = received;
        }
    }
    close(upstream_socket);
    return response_length;
}

static void dns_relay_task(void *arg)
{
    (void)arg;

    xEventGroupWaitBits(s_events, EVENT_AP_READY, pdFALSE, pdTRUE, portMAX_DELAY);

    const int relay_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (relay_socket < 0) {
        ESP_LOGE(TAG, "Could not create the local DNS listener (errno %d).", errno);
        vTaskDelete(NULL);
        return;
    }

    struct ifreq hotspot = {0};
    const struct sockaddr_in local_address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = inet_addr(AP_GATEWAY),
    };
    if (esp_netif_get_netif_impl_name(s_ap_netif, hotspot.ifr_name) != ESP_OK ||
        setsockopt(relay_socket, SOL_SOCKET, SO_BINDTODEVICE, &hotspot, sizeof(hotspot)) != 0 ||
        bind(relay_socket, (const struct sockaddr *)&local_address, sizeof(local_address)) != 0) {
        ESP_LOGE(TAG, "Could not bind DNS to the hotspot (errno %d).", errno);
        close(relay_socket);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Local DNS relay is ready for hotspot clients.");

    for (;;) {
        uint8_t packet[DNS_PACKET_CAPACITY];
        struct sockaddr_in client_address = {0};
        socklen_t client_address_length = sizeof(client_address);
        const int packet_length = recvfrom(
            relay_socket, packet, sizeof(packet), 0,
            (struct sockaddr *)&client_address, &client_address_length);

        if (packet_length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "The local DNS listener reported an error (errno %d).", errno);
            continue;
        }

        if (packet_length < 12 || (packet[2] & 0x80U) != 0) {
            continue;
        }

        const unsigned generation = atomic_load(&s_link_generation);
        const int response_length = forward_dns_query(atomic_load(&s_campus_dns_address),
                                                      packet, packet_length);
        if (response_length >= 0 && generation == atomic_load(&s_link_generation)) {
            (void)sendto(relay_socket, packet, response_length, 0,
                         (const struct sockaddr *)&client_address, client_address_length);
        }
    }
}

static esp_netif_t *start_softap(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_country_code("CN", true));
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create the printer Wi-Fi interface.");
        abort();
    }
    s_ap_netif = ap_netif;
    configure_ap_network();

    wifi_config_t ap_config = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = 0,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strlcpy((char *)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen((const char *)ap_config.ap.ssid);
    strlcpy((char *)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_LOGI(TAG, "Printer Wi-Fi network: %s, channel %d, maximum %d clients.",
             AP_SSID, AP_CHANNEL, AP_MAX_CLIENTS);
    return ap_netif;
}

static void start_campus_station(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create the campus Wi-Fi interface.");
        abort();
    }

    wifi_config_t station_config = {0};
    strlcpy((char *)station_config.sta.ssid, CAMPUS_SSID,
            sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password, CAMPUS_PASSWORD,
            sizeof(station_config.sta.password));
    station_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station_config.sta.failure_retry_cnt = MAX_DRIVER_RETRIES;
    station_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (campus_ssid_is_configured()) {
        ESP_LOGI(TAG, "Campus Wi-Fi SSID: %s", CAMPUS_SSID);
    } else {
        ESP_LOGW(TAG, "The actual campus SSID is not configured. The printer hotspot is still available.");
        ESP_LOGW(TAG, "Scan for a 2.4 GHz campus network and set it under Campus Wi-Fi router configuration.");
    }
}

static void invalidate_upstream(void)
{
    atomic_store(&s_campus_connected, false);
    atomic_fetch_add(&s_link_generation, 1);
    atomic_store(&s_campus_dns_address, 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(s_events, EVENT_AP_READY);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, EVENT_STA_CONNECT);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        atomic_store(&s_campus_associated, true);
        atomic_store(&s_last_disconnect_reason, 0);
        xEventGroupSetBits(s_events, EVENT_STA_ASSOCIATED);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        atomic_store(&s_campus_associated, false);
        atomic_store(&s_last_disconnect_reason, event->reason);
        invalidate_upstream();
        xEventGroupSetBits(s_events, EVENT_STA_DISCONNECTED);
        if (event->reason == WIFI_REASON_NO_AP_FOUND) {
            ESP_LOGW(TAG, "Could not find upstream AP for %s (reason %u).", CAMPUS_SSID, event->reason);
        } else if (event->reason == WIFI_REASON_AUTH_FAIL ||
                   event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
            ESP_LOGW(TAG, "Campus Wi-Fi rejected the connection. Check its security settings and credentials.");
        } else {
            ESP_LOGW(TAG, "Campus Wi-Fi disconnected (reason %d). Reconnecting automatically.",
                     (int)event->reason);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        invalidate_upstream();
        xEventGroupSetBits(s_events, EVENT_STA_LOST_IP);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP &&
        atomic_load(&s_campus_associated)) {
        const ip_event_got_ip_t *event = event_data;
        if (atomic_load(&s_campus_connected) && !event->ip_changed) {
            return;
        }
        atomic_store(&s_probe_state, PROBE_CHECKING);
        atomic_store(&s_last_http_status, 0);
        atomic_fetch_add(&s_link_generation, 1);
        atomic_store(&s_campus_connected, true);
        ESP_LOGI(TAG, "Campus Wi-Fi assigned IPv4 address " IPSTR ".", IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, EVENT_STA_GOT_IP);
    }
}

static void disable_napt(void)
{
    if (!atomic_load(&s_napt_enabled)) {
        return;
    }

    if (esp_netif_napt_disable(s_ap_netif) == ESP_OK) {
        atomic_store(&s_napt_enabled, false);
        ESP_LOGW(TAG, "IPv4 forwarding paused until the campus Wi-Fi reconnects.");
    } else {
        ESP_LOGE(TAG, "Failed to disable IPv4 forwarding.");
    }
}

static int64_t schedule_station_retry(int64_t *retry_delay_us)
{
    const int64_t delay = *retry_delay_us;
    const int64_t doubled = delay * 2;
    *retry_delay_us = doubled < MAX_RETRY_US ? doubled : MAX_RETRY_US;
    ESP_LOGI(TAG, "Retrying campus Wi-Fi in %" PRId64 " s.", delay / 1000000);
    return esp_timer_get_time() + delay;
}

static void prepare_campus_forwarding(void)
{
    if (esp_netif_set_default_netif(s_sta_netif) != ESP_OK) {
        ESP_LOGE(TAG, "Could not select campus Wi-Fi as the outbound interface.");
        return;
    }
    update_dns_relay_upstream();

    if (atomic_load(&s_napt_enabled)) {
        return;
    }

    const esp_err_t result = esp_netif_napt_enable(s_ap_netif);
    if (result == ESP_OK) {
        atomic_store(&s_napt_enabled, true);
        ESP_LOGI(TAG, "IPv4 NAT is active so your phone can reach the campus login page.");
    } else {
        ESP_LOGE(TAG, "Could not enable IPv4 NAT (error %s).",
                 esp_err_to_name(result));
    }
}

static esp_err_t read_probe_header(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_HEADER ||
        strcasecmp(event->header_key, "Location") != 0) {
        return ESP_OK;
    }

    const char *host = event->header_value;
    if (strncasecmp(host, "http://", 7) == 0) {
        host += 7;
    } else if (strncasecmp(host, "https://", 8) == 0) {
        host += 8;
    } else if (strncmp(host, "//", 2) == 0) {
        host += 2;
    } else {
        return ESP_OK;
    }

    // The CQU gateway may redirect to its DNS name or its current portal address.
    static const char *const portal_hosts[] = {"login.cqu.edu.cn", "10.10.8.162"};
    for (size_t i = 0; i < sizeof(portal_hosts) / sizeof(portal_hosts[0]); i++) {
        size_t size = strlen(portal_hosts[i]);
        if (strncasecmp(host, portal_hosts[i], size) == 0 &&
            strchr(":/?#", host[size]) != NULL) {
            *(bool *)event->user_data = true;
            break;
        }
    }
    return ESP_OK;
}

static probe_state_t check_campus_access(int *status_code)
{
    bool portal_redirect = false;
    const esp_http_client_config_t config = {
        .url = INTERNET_PROBE_URL,
        .timeout_ms = PROBE_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .addr_type = HTTP_ADDR_TYPE_INET,
        .event_handler = read_probe_header,
        .user_data = &portal_redirect,
        .user_agent = "ESP32-S3-CQU-A1-router/1.1",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return PROBE_UNVERIFIED;
    }

    // A captive portal can return a redirect and close before sending its body.
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0) {
        *status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    if (*status_code == 204) {
        return PROBE_ONLINE;
    }
    if (*status_code == 511 ||
        (*status_code >= 300 && *status_code < 400 && portal_redirect)) {
        return PROBE_PORTAL;
    }
    return PROBE_UNVERIFIED;
}

static int64_t perform_campus_probe(unsigned generation)
{
    int status_code = 0;
    const probe_state_t state = check_campus_access(&status_code);
    if (generation == atomic_load(&s_link_generation) && atomic_load(&s_campus_connected)) {
        atomic_store(&s_last_http_status, status_code);
        if (atomic_exchange(&s_probe_state, state) != state) {
            ESP_LOGI(TAG, "Campus probe: %s (HTTP %d).", PROBE_STATE_NAMES[state], status_code);
            if (state == PROBE_PORTAL) {
                ESP_LOGI(TAG, "Connect to %s and sign in at %s.", AP_SSID, CAMPUS_LOGIN_URL);
            }
        }
    }
    return esp_timer_get_time() + (state == PROBE_ONLINE ? PROBE_INTERVAL_US : PROBE_RETRY_US);
}

static void log_campus_scan(void)
{
    uint8_t ssid[] = CAMPUS_SSID;
    wifi_scan_config_t scan = {
        .ssid = ssid,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t result = esp_wifi_scan_start(&scan, true);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "2.4 GHz scan for %s failed: %s", CAMPUS_SSID, esp_err_to_name(result));
        return;
    }

    uint16_t count = 0;
    wifi_ap_record_t ap = {0};
    result = esp_wifi_scan_get_ap_num(&count);
    if (result == ESP_OK && count > 0) {
        result = esp_wifi_scan_get_ap_record(&ap);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_clear_ap_list());

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not read 2.4 GHz scan results for %s: %s",
                 CAMPUS_SSID, esp_err_to_name(result));
    } else if (count == 0) {
        ESP_LOGW(TAG, "2.4 GHz scan: no access point for %s at the board's location.", CAMPUS_SSID);
    } else {
        ESP_LOGI(TAG, "2.4 GHz scan: %u AP(s) for %s; strongest channel %u, RSSI %d dBm, auth mode %d.",
                 count, CAMPUS_SSID, ap.primary, ap.rssi, ap.authmode);
    }
}

static void network_monitor_task(void *arg)
{
    (void)arg;
    int64_t next_connect_us = 0;
    int64_t retry_delay_us = INITIAL_RETRY_US;
    int64_t dhcp_deadline_us = 0;
    int64_t next_probe_us = 0;
    int64_t next_scan_us = 0;
    unsigned configured_generation = 0;

    for (;;) {
        const EventBits_t events = xEventGroupWaitBits(
            s_events,
            EVENT_STA_CONNECT | EVENT_STA_GOT_IP | EVENT_STA_DISCONNECTED |
                EVENT_STA_ASSOCIATED | EVENT_STA_LOST_IP,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(1000));

        if (!campus_ssid_is_configured()) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        const unsigned generation = atomic_load(&s_link_generation);
        if (atomic_load(&s_campus_connected)) {
            if (configured_generation != generation) {
                disable_napt();
                prepare_campus_forwarding();
                configured_generation = generation;
                retry_delay_us = INITIAL_RETRY_US;
                dhcp_deadline_us = 0;
                next_probe_us = 0;
                next_scan_us = 0;
            }

            if (now >= next_probe_us) {
                if (atomic_load(&s_napt_enabled)) {
                    update_dns_relay_upstream();
                } else {
                    prepare_campus_forwarding();
                }
                next_probe_us = perform_campus_probe(generation);
            }
            continue;
        }

        disable_napt();
        if (atomic_load(&s_campus_associated)) {
            if (dhcp_deadline_us == 0 || (events & EVENT_STA_LOST_IP)) {
                dhcp_deadline_us = now + DHCP_TIMEOUT_US;
            } else if (now >= dhcp_deadline_us && !atomic_load(&s_campus_connected)) {
                ESP_LOGW(TAG, "Campus DHCP timed out; reconnecting Wi-Fi.");
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
                dhcp_deadline_us = now + DHCP_TIMEOUT_US;
            }
            continue;
        }

        dhcp_deadline_us = 0;
        if (events & EVENT_STA_DISCONNECTED) {
            next_connect_us = schedule_station_retry(&retry_delay_us);
        }
        if (now >= next_connect_us) {
            if (now >= next_scan_us) {
                log_campus_scan();
                next_scan_us = esp_timer_get_time() + CAMPUS_SCAN_INTERVAL_US;
            }
            const esp_err_t connect_result = esp_wifi_connect();
            if (connect_result != ESP_OK) {
                ESP_LOGW(TAG, "Could not start the campus Wi-Fi connection (%s).",
                         esp_err_to_name(connect_result));
                next_connect_us = schedule_station_retry(&retry_delay_us);
            } else {
                next_connect_us = esp_timer_get_time() + MAX_RETRY_US;
            }
        }
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "Could not allocate the Wi-Fi connection event group.");
        abort();
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    if (strlen(AP_SSID) == 0 || strlen(AP_SSID) >= sizeof(((wifi_config_t *)0)->ap.ssid) ||
        strlen(AP_PASSWORD) < 8 || strlen(AP_PASSWORD) >= sizeof(((wifi_config_t *)0)->ap.password)) {
        ESP_LOGE(TAG, "Set a Wi-Fi name up to 32 characters and a WPA2 password of 8 to 63 characters.");
        abort();
    }

    start_softap();
    start_campus_station();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                               wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Configured NAPT table for up to %d concurrent IPv4 mappings.",
             NAPT_TABLE_SIZE);
    ESP_LOGI(TAG, "Join the printer Wi-Fi network from a phone to check router status.");

    start_status_server();

    if (xTaskCreate(dns_relay_task, "dns_relay", DNS_TASK_STACK_SIZE, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the local DNS relay.");
        abort();
    }

    if (xTaskCreate(network_monitor_task, "network_monitor", 7168, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the campus Wi-Fi network monitor task.");
        abort();
    }
    heartbeat_start(&s_campus_connected, &s_napt_enabled, &s_link_generation);
}
