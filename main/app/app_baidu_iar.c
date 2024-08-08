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
#include "mbedtls/base64.h"

#include "app_baidu_iar.h"
#include "app_minmax_llm.h"
#include "app_minmax_tts.h"

#define IAR_SPEECH_SIZE (256 * 1024)
static const char *TAG = "[APP_Baidu_IAR]";

static iar_payload_t _g_request_body;
static esp_http_client_handle_t _g_client = NULL;

static uint8_t *iar_rx_buffer = NULL;
static uint32_t iar_rx_total_len = 0;

/**
 * @brief Parses the text from a JSON string.
 *
 * This function takes a JSON string as input and extracts the text value from the "result" array.
 * The extracted text is returned as a dynamically allocated C string.
 *
 * @param json_str The JSON string to parse.
 * @return A pointer to the extracted text, or NULL if parsing fails.
 */
static char *_ira_parse_text_from_json(const char *json_str)
{
    cJSON *json = cJSON_Parse(json_str);
    if (!json)
    {
        ESP_LOGE(TAG, "Failed to parse JSON string.");
        return NULL;
    }

    cJSON *result_array = cJSON_GetObjectItem(json, "result");
    if (!result_array || !cJSON_IsArray(result_array))
    {
        ESP_LOGE(TAG, "Invalid JSON format: 'result' field not found or not an array.");
        cJSON_Delete(json);
        return NULL;
    }

    cJSON *first_result_item = cJSON_GetArrayItem(result_array, 0);
    if (!first_result_item || !cJSON_IsString(first_result_item))
    {
        ESP_LOGE(TAG, "Invalid JSON format: first item in 'result' array is not a string.");
        cJSON_Delete(json);
        return NULL;
    }

    const char *text = cJSON_GetStringValue(first_result_item);
    if (!text)
    {
        ESP_LOGE(TAG, "Failed to extract text from JSON.");
        cJSON_Delete(json);
        return NULL;
    }

    char *text_copy = (char *)malloc(strlen(text) + 1);
    if (!text_copy)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for text copy.");
        cJSON_Delete(json);
        return NULL;
    }

    strcpy(text_copy, text);

    cJSON_Delete(json);

    return text_copy;
}

/**
 * @brief Initializes the payload structure for the Baidu IAR request.
 *
 * This function creates and initializes a payload structure (`iar_payload_t`)
 * with the provided speech data and additional metadata required for the Baidu IAR service.
 *
 * @param speech Base64 encoded speech data.
 * @param wav_len Length of the original speech data in bytes before encoding.
 * @return An initialized `iar_payload_t` structure containing the request payload.
 */
static iar_payload_t _app_baidu_iar_payload_init(const char *speech, const int wav_len)
{
    iar_payload_t body = {
        .format = "pcm",                        ///< Audio format, typically PCM for raw audio data.
        .rate = 16000,                          ///< Sample rate in Hz, commonly 16kHz for speech recognition.
        .channel = 1,                           ///< Number of audio channels, 1 for mono.
        .token = CONFIG_Baidu_IAR_Access_Token, ///< Access token for Baidu IAR service authentication.
        .cuid = "ESP-BOX-3",                    ///< Client unique ID, identifies the device or application.
        .speech = speech,                       ///< Base64 encoded speech data to be recognized.
        .len = wav_len};                        ///< Length of the wav speech data.

    return body;
}

/**
 * @brief Converts the payload structure to a JSON string.
 *
 * This function takes an `iar_payload_t` structure and converts it into a JSON string.
 * The JSON string can then be used as the payload for a Baidu IAR API request.
 *
 * @param body Pointer to the `iar_payload_t` structure containing the payload data.
 * @return A dynamically allocated C string containing the JSON representation of the payload,
 *         or NULL if the conversion fails.
 */
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

    char *result = strdup(json_str);
    free(json_str);
    return result;
}

/**
 * @brief Handles HTTP client events.
 *
 * This function is the event handler for the HTTP client. It processes different events
 * during the HTTP transaction, such as connection, header sent, data received, and finish.
 *
 * @param evt Pointer to the `esp_http_client_event_t` structure containing the event details.
 * @return The ESP_OK error code on success, or another error code on failure.
 */
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

        char *text = _ira_parse_text_from_json((char *)iar_rx_buffer);
        if (text)
        {
            ESP_LOGI(TAG, "Extracted text: %s", text);
            app_minmax_llm_https_post(text);
            free(text);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to extract text from JSON.");
        }
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
/**
 * @brief Initializes the HTTPS client with a specific URL.
 *
 * This function initializes the HTTPS client with the specified configuration,
 * including the URL, event handler, and certificate bundle attachment.
 * It ensures that the client is only initialized once.
 */
static void _https_with_url_init(void)
{
    if (_g_client == NULL)
    {
        esp_http_client_config_t config = {
            .url = CONFIG_Baidu_IAR_Base_URL,           ///< Base URL for the Baidu IAR service.
            .event_handler = _http_event_handler,       ///< Event handler for HTTP client events.
            .crt_bundle_attach = esp_crt_bundle_attach, ///< Function to attach the certificate bundle.
            .buffer_size_tx = 8192,                     ///< Adjusted header buffer size for transmission.
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
void app_baidu_iar_https_post(const char *speech, const uint16_t wav_len)
{
    if (_g_client == NULL)
    {
        _https_with_url_init();
    }
    esp_http_client_set_method(_g_client, HTTP_METHOD_POST);
    esp_http_client_set_header(_g_client, "Content-Type", "application/json");
    esp_http_client_set_header(_g_client, "Accept", "application/json");

    static uint16_t speech_base64_size = 0;
    mbedtls_base64_encode(NULL, 0, &speech_base64_size, (const unsigned char *)speech, wav_len);
    ESP_LOGI(TAG, "speech size after base64 encoding: %zu", speech_base64_size); // 输出编码后数据的大小

    char *speech_base64 = heap_caps_calloc(1, speech_base64_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(speech_base64);

    mbedtls_base64_encode((unsigned char *)speech_base64, speech_base64_size, &speech_base64_size, (const unsigned char *)speech, wav_len);

    _g_request_body = _app_baidu_iar_payload_init(speech_base64, wav_len);
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
    heap_caps_free(iar_rx_buffer);

    free(_g_request_body.format);
    free(_g_request_body.token);
    free(_g_request_body.cuid);
    free(_g_request_body.speech);

    if (_g_client != NULL)
    {
        esp_http_client_cleanup(_g_client);
    }
}
void app_baidu_iar_init(void)
{
    iar_rx_buffer = heap_caps_calloc(1, IAR_SPEECH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(iar_rx_buffer);
    ESP_LOGI(TAG, "iar_rx_buffer with a size: %zu\n", IAR_SPEECH_SIZE);

    _https_with_url_init();
}