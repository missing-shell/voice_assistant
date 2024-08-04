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

// 用于保存解析后的音频数据
static char *hex_audio_data = NULL;
static size_t hex_audio_data_len = 0;

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
        .stream = false,
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
 * Writes hexadecimal audio data to a file as binary data.
 *
 * This function takes a hexadecimal string representing audio data and writes it to a file in binary format.
 * It first converts the hexadecimal string into binary data, then opens a file on SPIFFS, and writes the binary data to the file.
 *
 * @param hex_audio_data A null-terminated string containing the hexadecimal audio data.
 * @param hex_audio_data_len The length of the hexadecimal audio data string (in bytes).
 */
static void _write_hex_to_file(const char *hex_audio_data, size_t hex_audio_data_len)
{
    // Ensure the hex_audio_data is not null
    if (!hex_audio_data)
    {
        ESP_LOGE(TAG, "hex_audio_data is not provided.");
        return;
    }

    // Allocate memory for the binary data
    uint8_t *binary_audio_data = (uint8_t *)heap_caps_malloc(hex_audio_data_len / 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!binary_audio_data)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for binary_audio_data.");
        return;
    }

    // Convert the hexadecimal string to binary data
    for (size_t i = 0; i < hex_audio_data_len; i += 2)
    {
        char byte[3] = {hex_audio_data[i], hex_audio_data[i + 1], '\0'};
        binary_audio_data[i / 2] = (uint8_t)strtol(byte, NULL, 16);
    }

    // Open the file for writing binary data
    FILE *fp = fopen("/spiffs/output.mp3", "wb");
    if (!fp)
    {
        ESP_LOGE(TAG, "Failed to open file 'output.mp3'.");
        heap_caps_free(binary_audio_data);
        return;
    }

    // Write the binary data to the file
    size_t written = fwrite(binary_audio_data, 1, hex_audio_data_len / 2, fp);
    if (written != hex_audio_data_len / 2)
    {
        ESP_LOGE(TAG, "Failed to write all data to file 'output.mp3'.");
    }
    else
    {
        ESP_LOGI(TAG, "Written %zu bytes to file 'output.mp3'.", written);
    }

    // Clean up resources
    heap_caps_free(binary_audio_data);
    fclose(fp);
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
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        tts_rx_total_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        if ((tts_rx_total_len + evt->data_len) < MAX_FILE_SIZE)
        {
            memcpy(tts_rx_buffer + tts_rx_total_len, (char *)evt->data, evt->data_len);
            tts_rx_total_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH:%" PRIu32 ", %" PRIu32 " K", tts_rx_total_len, tts_rx_total_len / 1024);

        // Parse JSON data
        cJSON *root = cJSON_Parse((const char *)tts_rx_buffer);
        if (root != NULL)
        {
            cJSON *data_obj = cJSON_GetObjectItem(root, "data");
            if (data_obj && data_obj->type == cJSON_Object)
            {
                cJSON *audio_item = cJSON_GetObjectItem(data_obj, "audio");
                if (audio_item && audio_item->type == cJSON_String)
                {
                    size_t hex_audio_data_len = strlen(audio_item->valuestring);

                    ESP_LOGI(TAG, "hex_audio_data_len: %zu", hex_audio_data_len);

                    _write_hex_to_file(audio_item->valuestring, hex_audio_data_len);
                }
            }
            cJSON_Delete(root);
        }
        ESP_LOGI(TAG, "--TTS Finished--");
        // Play the audio file
        FILE *fp = fopen("/spiffs/output.mp3", "r");
        if (fp)
        {
            ESP_LOGI(TAG, "Starting play");
            audio_player_play(fp);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to open file.");
        }

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

void _https_with_url_init(void)
{
    if (_g_client == NULL)
    {
        esp_http_client_config_t config = {
            .url = CONFIG_MINMAX_TTS_URL,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size_tx = 2048, // Adjusted header buffer size
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
    if (_g_client == NULL)
    {
        _https_with_url_init();
    }

    esp_http_client_set_url(_g_client, CONFIG_MINMAX_TTS_URL);
    esp_http_client_set_method(_g_client, HTTP_METHOD_POST);

    // 设置请求头
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
void app_minmax_tts_init(void)
{
    tts_rx_buffer = heap_caps_calloc(1, MAX_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(tts_rx_buffer);
    ESP_LOGI(TAG, "tts_rx_buffer with a size: %zu\n", MAX_FILE_SIZE);
    _https_with_url_init();
}