#include "direct_connection.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#define AP_RESPONSE_SIZE 1024
#define AP_HISTORY_MAX_ITEMS 10

static const char *TAG = "AP_SERVER";

static esp_err_t send_json(httpd_req_t *request, const char *json)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json);
}

/* Only requests that arrived on the SoftAP address are served; the same
 * HTTP server is otherwise reachable from the station LAN. */
static bool request_on_ap(httpd_req_t *request)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info;
    if (ap == NULL || esp_netif_get_ip_info(ap, &ip_info) != ESP_OK) {
        return false;
    }
    struct sockaddr_storage local;
    socklen_t length = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(request), (struct sockaddr *)&local,
                    &length) != 0) {
        return false;
    }
    if (local.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&local)->sin_addr.s_addr == ip_info.ip.addr;
    }
#if LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        /* Dual-stack socket: IPv4-mapped address ::ffff:a.b.c.d. */
        const uint8_t *bytes =
            (const uint8_t *)&((struct sockaddr_in6 *)&local)->sin6_addr;
        static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                  0, 0, 0xff, 0xff};
        return memcmp(bytes, mapped_prefix, sizeof(mapped_prefix)) == 0 &&
               memcmp(bytes + 12, &ip_info.ip.addr, 4) == 0;
    }
#endif
    return false;
}

/* Returns ESP_OK when the request may proceed, otherwise sends the error. */
static esp_err_t authorize(httpd_req_t *request)
{
    ap_server_t *server = request->user_ctx;
    if (!request_on_ap(request)) {
        httpd_resp_set_status(request, "403 Forbidden");
        httpd_resp_sendstr(request, "{\"detail\":\"Local API is only available on the device AP\"}");
        return ESP_FAIL;
    }
    char header[16 + 64 + 8];
    const char *token = NULL;
    if (httpd_req_get_hdr_value_str(request, "Authorization", header,
                                    sizeof(header)) == ESP_OK &&
        strncmp(header, "Bearer ", 7) == 0) {
        token = header + 7;
    }
    if (token == NULL || server->callbacks.check_token == NULL ||
        !server->callbacks.check_token(server->callbacks.context, token)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer");
        httpd_resp_set_type(request, "application/json");
        httpd_resp_sendstr(request, "{\"detail\":\"Device token required\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t call_provider(
    httpd_req_t *request, direct_json_provider_t provider)
{
    ap_server_t *server = request->user_ctx;
    if (provider == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_501_METHOD_NOT_IMPLEMENTED, "Not implemented");
    }
    char response[AP_RESPONSE_SIZE];
    esp_err_t result = provider(
        server->callbacks.context, response, sizeof(response));
    if (result != ESP_OK) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }
    return send_json(request, response);
}

static esp_err_t health_handler(httpd_req_t *request)
{
    return send_json(request, "{\"status\":\"ok\",\"service\":\"saems-device\"}");
}

static esp_err_t status_handler(httpd_req_t *request)
{
    if (authorize(request) != ESP_OK) {
        return ESP_OK;
    }
    ap_server_t *server = request->user_ctx;
    return call_provider(request, server->callbacks.status);
}

static esp_err_t latest_handler(httpd_req_t *request)
{
    if (authorize(request) != ESP_OK) {
        return ESP_OK;
    }
    ap_server_t *server = request->user_ctx;
    return call_provider(request, server->callbacks.latest_telemetry);
}

static esp_err_t config_get_handler(httpd_req_t *request)
{
    if (authorize(request) != ESP_OK) {
        return ESP_OK;
    }
    ap_server_t *server = request->user_ctx;
    return call_provider(request, server->callbacks.get_config);
}

static size_t history_limit(httpd_req_t *request)
{
    char query[64];
    char value[16];
    size_t limit = AP_HISTORY_MAX_ITEMS;
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "limit", value, sizeof(value)) == ESP_OK) {
        long parsed = strtol(value, NULL, 10);
        if (parsed > 0 && parsed < AP_HISTORY_MAX_ITEMS) {
            limit = (size_t)parsed;
        }
    }
    return limit;
}

static esp_err_t history_handler(httpd_req_t *request)
{
    if (authorize(request) != ESP_OK) {
        return ESP_OK;
    }
    ap_server_t *server = request->user_ctx;
    if (server->callbacks.telemetry_history == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_501_METHOD_NOT_IMPLEMENTED, "Not implemented");
    }
    size_t limit = history_limit(request);
    size_t response_size = limit * 768U + 128U;
    char *response = calloc(1, response_size);
    if (response == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    esp_err_t result = server->callbacks.telemetry_history(
        server->callbacks.context, limit, response, response_size);
    if (result == ESP_OK) {
        result = send_json(request, response);
    } else {
        result = httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }
    free(response);
    return result;
}

static esp_err_t receive_json(httpd_req_t *request, direct_json_consumer_t consumer)
{
    ap_server_t *server = request->user_ctx;
    if (consumer == NULL || request->content_len <= 0 ||
        request->content_len >= AP_RESPONSE_SIZE) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    char body[AP_RESPONSE_SIZE];
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int result = httpd_req_recv(request,
                                    body + received,
                                    request->content_len - received);
        if (result <= 0) {
            return httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST, "Incomplete request body");
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    esp_err_t result = consumer(
        server->callbacks.context, body, received);
    if (result != ESP_OK) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, esp_err_to_name(result));
    }
    return send_json(request, "{\"status\":\"accepted\"}");
}

static esp_err_t config_put_handler(httpd_req_t *request)
{
    if (authorize(request) != ESP_OK) {
        return ESP_OK;
    }
    ap_server_t *server = request->user_ctx;
    return receive_json(request, server->callbacks.set_config);
}

static esp_err_t command_handler(httpd_req_t *request)
{
    if (authorize(request) != ESP_OK) {
        return ESP_OK;
    }
    ap_server_t *server = request->user_ctx;
    esp_err_t result = receive_json(request, server->callbacks.command);
    return result;
}

esp_err_t ap_server_start(
    ap_server_t *server, const direct_connection_callbacks_t *callbacks)
{
    if (server == NULL || callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(server, 0, sizeof(*server));
    server->callbacks = *callbacks;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Config writes parse JSON and commit NVS synchronously. The default 4K
     * HTTP task stack is insufficient for that nested path, just as on BLE. */
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    httpd_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&handle, &config), TAG, "start server");
    server->server = handle;

    const httpd_uri_t uris[] = {
        {.uri = "/v1/health", .method = HTTP_GET,
         .handler = health_handler, .user_ctx = server},
        {.uri = "/v1/status", .method = HTTP_GET,
         .handler = status_handler, .user_ctx = server},
        {.uri = "/v1/telemetry/latest", .method = HTTP_GET,
         .handler = latest_handler, .user_ctx = server},
        {.uri = "/v1/telemetry/history", .method = HTTP_GET,
         .handler = history_handler, .user_ctx = server},
        {.uri = "/v1/config", .method = HTTP_GET,
         .handler = config_get_handler, .user_ctx = server},
        {.uri = "/v1/config", .method = HTTP_PUT,
         .handler = config_put_handler, .user_ctx = server},
        {.uri = "/v1/commands/pump", .method = HTTP_POST,
         .handler = command_handler, .user_ctx = server},
    };
    for (size_t index = 0; index < sizeof(uris) / sizeof(uris[0]); index++) {
        esp_err_t result = httpd_register_uri_handler(handle, &uris[index]);
        if (result != ESP_OK) {
            httpd_stop(handle);
            server->server = NULL;
            return result;
        }
    }
    server->running = true;
    ESP_LOGI(TAG, "Local REST API ready on http://192.168.4.1");
    return ESP_OK;
}

bool ap_server_is_running(ap_server_t *server)
{
    return server != NULL && server->running;
}
