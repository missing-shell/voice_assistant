#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "cJSON.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "audio_player.h"

#include "app_baidu_iar.h"

#define IAR_SPEECH_SIZE (256 * 1024)
static const char *TAG = "[APP_Baidu_IAR]";

static iar_payload_t _g_request_body;
static esp_http_client_handle_t _g_client = NULL;

// 发送缓冲区
static uint8_t *iar_tx_buffer = NULL;
static uint32_t iar_tx_total_len = 0;
// 音频接收缓冲区
static uint8_t *iar_rx_buffer = NULL;
static uint32_t iar_rx_total_len = 0;

static iar_payload_t _app_baidu_iar_payload_init(const char *speech, const int wav_len)
{
    iar_payload_t body = {
        .format = "pcm",
        .rate = 16000,
        .channel = 1,
        .token = CONFIG_Baidu_IAR_Access_Token,
        .cuid = "ESP-BOX-3",
        .speech = speech,
        .len = wav_len};

    return body;
}

static char *_app_baidu_iar_payload_to_json(const iar_payload_t *body)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create cJSON object.");
        return NULL;
    }

    cJSON_AddStringToObject(root, "format", body->format);
    cJSON_AddNumberToObject(root, "rate", body->rate);
    cJSON_AddNumberToObject(root, "channel", body->channel);
    cJSON_AddStringToObject(root, "token", body->token);
    cJSON_AddStringToObject(root, "cuid", body->cuid);
    cJSON_AddStringToObject(root, "speech", body->speech);
    cJSON_AddNumberToObject(root, "len", body->len);

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str)
    {
        ESP_LOGE(TAG, "Failed to print JSON string.");
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_Delete(root);

    // Add a call to free the memory allocated by cJSON_PrintUnformatted.
    char *result = strdup(json_str); // Duplicate the string for safe return.
    free(json_str);                  // Free the original cJSON allocated memory.
    ESP_LOGI(TAG, "resule=%s", result);
    return result;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        iar_rx_total_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        if ((iar_rx_total_len + evt->data_len) < IAR_SPEECH_SIZE)
        {
            memcpy(iar_rx_buffer + iar_rx_total_len, (char *)evt->data, evt->data_len);
            iar_rx_total_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH:%" PRIu32 ", %" PRIu32 " K", iar_rx_total_len, iar_rx_total_len / 1024);
        ESP_LOGI(TAG, "%s", iar_rx_buffer);
        ESP_LOGI(TAG, "--IAR Finished--");

        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
static void _https_with_url_init(void)
{
    if (_g_client == NULL)
    {
        esp_http_client_config_t config = {
            .url = CONFIG_Baidu_IAR_Base_URL,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size_tx = 8192, // Adjusted header buffer size
        };

        ESP_LOGI(TAG, "\npost_url=%s\n", config.url);

        _g_client = esp_http_client_init(&config);
        if (_g_client == NULL)
        {
            ESP_LOGE(TAG, "Error creating HTTP client");
            return;
        }
    }
}
void app_baidu_iar_https_post(const char *speech, const int wav_len)
{
    if (_g_client == NULL)
    {
        _https_with_url_init();
    }
    esp_http_client_set_method(_g_client, HTTP_METHOD_POST);
    esp_http_client_set_header(_g_client, "Content-Type", "application/json");
    esp_http_client_set_header(_g_client, "Accept", "application/json");

    _g_request_body = _app_baidu_iar_payload_init((char *)speech, wav_len);
    char *post_data = _app_baidu_iar_payload_to_json(&_g_request_body);

    esp_http_client_set_post_field(_g_client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(_g_client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Status = %d, content_length = %" PRIu64,
                 esp_http_client_get_status_code(_g_client),
                 esp_http_client_get_content_length(_g_client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }

    free(post_data);
}
#endif // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
void app_baidu_iar_deinit(void)
{

    heap_caps_free(iar_tx_buffer);
    heap_caps_free(iar_rx_buffer);

    if (_g_client != NULL)
    {
        esp_http_client_cleanup(_g_client);
    }
}

void app_baidu_iar_task(void *pvParameters)
{
    iar_tx_buffer = heap_caps_calloc(1, IAR_SPEECH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(iar_tx_buffer);
    ESP_LOGI(TAG, "iar_tx_buffer with a size: %zu\n", IAR_SPEECH_SIZE);

    iar_rx_buffer = heap_caps_calloc(1, IAR_SPEECH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(iar_rx_buffer);
    ESP_LOGI(TAG, "iar_rx_buffer with a size: %zu\n", IAR_SPEECH_SIZE);

    _https_with_url_init();
}

void app_baidu_iar_init(void)
{
    xTaskCreatePinnedToCore(app_baidu_iar_task, "app_baidu_iar_task", 5 * 1024, NULL, 6, NULL, 0);
}