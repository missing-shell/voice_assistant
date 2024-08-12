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

#include "app_minmax_tts.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 8192
#define MAX_FILE_SIZE (1 * 1024 * 1024)
static const char *TAG = "[APP_MinMax_TTS]";

static tts_payload_t _g_request_body;
static esp_http_client_handle_t _g_client = NULL;

// 音频接收缓冲区
static uint8_t *tts_rx_buffer = NULL;
static uint32_t tts_rx_total_len = 0;

static bool is_data = false;

// 定义解析状态枚举类型
typedef enum
{
    WAITING_FOR_DATA, // 等待 "data:" 的开始
    WAITING_FOR_CRLF, // 等待 "\r\n" 的结束标记
} parse_state_t;

static parse_state_t CUR_PARSE_STATE = WAITING_FOR_DATA; // 当前解析状态
static int cur_cnt_pos = 0;                              // 当前处理到的字符位置
static int json_start_pos = 0;                           // JSON 起始位置
static int json_end_pos = 0;

/**
 * @brief Internal function to initialize TTS (Text-to-Speech) payload data.
 *
 * This function creates an instance of the `tts_payload_t` structure to store detailed configurations for a TTS request, including model name, text content, streaming flag, voice settings (such as voice ID, speed, volume, pitch), and audio settings (such as sample rate, bitrate, audio format, channel count).
 *
 * @param text The text content to be converted into speech.
 * @return tts_payload_t An initialized TTS payload structure.
 */
static tts_payload_t _app_minmax_tts_payload_init(const char *text)
{
    tts_payload_t body = {
        .model = strdup("speech-01-turbo"),
        .text = strdup(text),
        .stream = true,
        .voice_setting = {
            .voice_id = strdup("female-tianmei"),
            .speed = 1,
            .vol = 1,
            .pitch = 0},
        .audio_setting = {.sample_rate = 16000, .bitrate = 128000, .format = strdup("mp3"), .channel = 1}};

    return body;
}

/**
 * Converts TTS payload data to a JSON string.
 *
 * @param body A pointer to the TTS payload structure containing the data to be converted.
 * @return A dynamically allocated string containing the JSON representation of the payload data.
 */
static char *_app_minmax_tts_payload_to_json(const tts_payload_t *body)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "model", body->model);
    cJSON_AddStringToObject(root, "text", body->text);
    cJSON_AddBoolToObject(root, "stream", body->stream);

    cJSON *voice_setting = cJSON_CreateObject();
    cJSON_AddStringToObject(voice_setting, "voice_id", body->voice_setting.voice_id);
    cJSON_AddNumberToObject(voice_setting, "speed", body->voice_setting.speed);
    cJSON_AddNumberToObject(voice_setting, "vol", body->voice_setting.vol);
    cJSON_AddNumberToObject(voice_setting, "pitch", body->voice_setting.pitch);
    cJSON_AddItemToObject(root, "voice_setting", voice_setting);

    cJSON *audio_setting = cJSON_CreateObject();
    cJSON_AddNumberToObject(audio_setting, "sample_rate", body->audio_setting.sample_rate);
    cJSON_AddNumberToObject(audio_setting, "bitrate", body->audio_setting.bitrate);
    cJSON_AddStringToObject(audio_setting, "format", body->audio_setting.format);
    cJSON_AddNumberToObject(audio_setting, "channel", body->audio_setting.channel);
    cJSON_AddItemToObject(root, "audio_setting", audio_setting);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

/**
 * @brief Parses JSON data and extracts audio data.
 *
 * This function takes JSON data and its length, parses the JSON, and extracts
 * the audio data. If successful, it converts the audio data into binary format.
 *
 * @param json_data The JSON data string.
 * @param json_len The length of the JSON data.
 */
static void _parse_and_write_audio(const char *json_data, size_t json_len)
{
    cJSON *root = cJSON_ParseWithLength(json_data, json_len);

    if (!root)
    {
        ESP_LOGE(TAG, "Failed to parse JSON data.");
        return;
    }

    // Get the "data" object
    cJSON *data = cJSON_GetObjectItem(root, "data");

    if (!data)
    {
        cJSON_Delete(root);
        return; // "data" is empty, return directly
    }

    // Extract the "audio" field from the "data" object
    cJSON *audio = cJSON_GetObjectItem(data, "audio");

    if (!audio)
    {
        ESP_LOGE(TAG, "Missing 'audio' field in JSON data.");
        cJSON_Delete(root);
        return;
    }

    // Get the value of the "audio" field
    const char *audio_hex = audio->valuestring;
    if (strcmp(audio_hex, "") == 0)
    {
        cJSON_Delete(root);
        return;
    }

    // Convert the audio data from a hexadecimal string to binary data
    size_t audio_len = strlen(audio_hex) / 2;

    void *speechptr = heap_caps_malloc(audio_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!speechptr)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for audio data.");
        cJSON_Delete(root);
        return;
    }

    for (size_t i = 0; i < audio_len; i++)
    {
        char hex[3] = {audio_hex[i * 2], audio_hex[i * 2 + 1], '\0'};
        ((uint8_t *)speechptr)[i] = (uint8_t)strtol(hex, NULL, 16);
    }

    free(speechptr); // Free the allocated memory

    cJSON_Delete(root);
}

/**
 * @brief Updates the parsing state based on the incoming data.
 *
 * This function processes incoming data and updates the parsing state according
 * to the current state and the content of the data. It handles transitions between
 * different parsing states and invokes the appropriate processing functions.
 */
static void _update_parse_state()
{
    const char *p = (char *)tts_rx_buffer + cur_cnt_pos; // Get the current character being processed
    switch (CUR_PARSE_STATE)
    {

    case WAITING_FOR_DATA:

        // Check if the "data:" start marker has been found
        if (strncmp(p, "data:", 5) == 0)
        {
            CUR_PARSE_STATE = WAITING_FOR_CRLF;
            json_start_pos = cur_cnt_pos + 5; // Skip "data: " and position to the start of the JSON
            p += 5;
            ESP_LOGW(TAG, "[STATE]: WAITING_FOR_DATA -> READING_JSON");
        }
        break;
    case WAITING_FOR_CRLF:
        // Check if the end-of-line marker "\n" has been encountered
        if (strncmp(p, "\n", 1) == 0)
        {
            json_end_pos = cur_cnt_pos; // Position at the end of the JSON
            ESP_LOGW(TAG, "[STATE]: WAITING_FOR_CRLF -> JSON_COMPLETE");

            // Get the JSON data string
            const char *json_str = (const char *)(tts_rx_buffer + json_start_pos);
            size_t json_len = json_end_pos - json_start_pos;

            // Process the JSON data
            _parse_and_write_audio(json_str, json_len);
            CUR_PARSE_STATE = WAITING_FOR_DATA; // Reset state to wait for the next JSON block
        }

        break;
    default:
        // Unknown state, can add error handling here
        ESP_LOGE(TAG, "[STATE]: Error STATE");
        break;
    }

    cur_cnt_pos++; // Move to the next character position
}

/**
 * @brief Initializes the parsing state variables.
 *
 * This function sets the initial values for the parsing state variables,
 * preparing the parser for incoming data.
 */
static void _init_parse_state()
{
    CUR_PARSE_STATE = WAITING_FOR_DATA; // Current parsing state
    cur_cnt_pos = 0;                    // Current character position being processed
    json_start_pos = 0;                 // Start position of the JSON data
    json_end_pos = 0;                   // End position of the JSON data
}
/**
 * Handles HTTP client events.
 *
 * This function processes various HTTP client events such as connection, header sent, data received, finish, and disconnection.
 * It also handles parsing JSON data from the response and writing the audio data to a file.
 *
 * @param evt A pointer to the event information.
 * @return The error code indicating the result of handling the event.
 */
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        tts_rx_total_len = 0;
        is_data = false;
        break;
    case HTTP_EVENT_ON_DATA:
        if ((tts_rx_total_len + evt->data_len) < MAX_FILE_SIZE)
        {
            memcpy(tts_rx_buffer + tts_rx_total_len, (char *)evt->data, evt->data_len);
            tts_rx_total_len += evt->data_len;
        }
        is_data = true;
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
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
            .url = CONFIG_MINMAX_TTS_URL,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size_tx = 2048, // Adjusted header buffer size
            .buffer_size = 10240,
        };
        _g_client = esp_http_client_init(&config);
        if (_g_client == NULL)
        {
            ESP_LOGE(TAG, "Error creating HTTP client");
            return;
        }
    }
}
void app_minmax_tts_https_post(const char *text)
{
    uint32_t starttime = esp_log_timestamp();
    ESP_LOGE(TAG, "[Start] create_TTS_request, timestamp:%" PRIu32, starttime);

    if (_g_client == NULL)
    {
        _https_with_url_init();
    }

    esp_http_client_set_url(_g_client, CONFIG_MINMAX_TTS_URL);
    esp_http_client_set_method(_g_client, HTTP_METHOD_POST);

    // Set header
    char auth_header[700];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_MINMAX_API_KEY);
    esp_http_client_set_header(_g_client, "Authorization", auth_header);
    esp_http_client_set_header(_g_client, "Content-Type", "application/json");

    _g_request_body = _app_minmax_tts_payload_init(text);
    char *post_data = _app_minmax_tts_payload_to_json(&_g_request_body);
    ESP_LOGI(TAG, "test=%s", _g_request_body.text);

    esp_http_client_set_post_field(_g_client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(_g_client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %" PRIu64,
                 esp_http_client_get_status_code(_g_client),
                 esp_http_client_get_content_length(_g_client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    ESP_LOGE(TAG, "[End] create_TTS_request, + offset:%" PRIu32, esp_log_timestamp() - starttime);

    free(post_data);
}
#endif // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE

void app_minmax_tts_deinit(void)
{

    free(_g_request_body.model);
    free(_g_request_body.text);
    free(_g_request_body.voice_setting.voice_id);
    free(_g_request_body.audio_setting.format);

    heap_caps_free(tts_rx_buffer);

    if (_g_client != NULL)
    {
        esp_http_client_cleanup(_g_client);
    }
}
void parse_response_task(void *pvParameters)
{
    _init_parse_state();
    while (true)
    {

        if (is_data && cur_cnt_pos <= tts_rx_total_len)
        {
            _update_parse_state();
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    vTaskDelete(NULL);
}
void app_minmax_tts_init(void)
{
    tts_rx_buffer = heap_caps_calloc(1, MAX_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(tts_rx_buffer);
    ESP_LOGI(TAG, "tts_rx_buffer with a size: %zu\n", MAX_FILE_SIZE);
    _https_with_url_init();
    xTaskCreatePinnedToCore(parse_response_task, "parse_response_task", 1024 * 4, NULL, 1, NULL, 1);
}