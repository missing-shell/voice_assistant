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

#include "app_minmax_llm.h"
#include "app_minmax_tts.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 8192
#define LLM_SIZE (1 * 1024 * 1024)
static const char *TAG = "[APP_MinMax_LLM]";

static esp_http_client_handle_t _g_client = NULL;
static llm_payload_t _g_request_payload;

static uint8_t *llm_rx_buffer = NULL;
static uint32_t llm_rx_total_len = 0;

/**
 * @brief Parses the "reply" field from a JSON response.
 *
 * This function takes a JSON string and extracts the "reply" field,
 * logs the reply content, and triggers text-to-speech processing.
 *
 * @param json_str The JSON string to parse.
 */
static void _parse_from_response_json(const char *json_str)
{
    cJSON *response_json = cJSON_Parse(json_str);

    if (response_json != NULL)
    {
        // TODO：解析message并将其添加到下一次请求中，以实现会话上下文
        cJSON *reply = cJSON_GetObjectItem(response_json, "reply");

        if (reply != NULL && reply->type == cJSON_String)
        {

            ESP_LOGI(TAG, "Reply: %s\n", reply->valuestring);
            app_minmax_tts_https_post(reply->valuestring);
        }
        else
        {

            ESP_LOGE(TAG, "Failed to find the 'reply' field or it's not a string.\n");
        }

        cJSON_Delete(response_json);
    }
    else
    {

        ESP_LOGE(TAG, "Error parsing JSON.\n");
    }
}

/**
 * @brief Initializes the payload for an LLM request.
 *
 * This function creates and initializes a payload structure with the necessary
 * fields for making a request to the language model API.
 *
 * @param text The user input text to be processed by the language model.
 * @return A populated llm_payload_t structure ready for use in the request.
 */
static llm_payload_t _app_minmax_llm_payload_init(const char *text)
{
    llm_payload_t payload =
        {
            .bot_setting =
                {
                    {.bot_name = strdup("智能语音助理"),
                     .content = strdup("你是一个陪伴老年人的智能语音助理，使用中文和精简的语言，语气平和的回答问题")}},
            .messages =
                {
                    {.sender_type = strdup("USER"),
                     .sender_name = strdup("ESP-BOX-3"),
                     .text = strdup(text)}},
            .reply_constraints =
                {
                    .sender_type = strdup("BOT"),
                    .sender_name = strdup("智能语音助理")

                },
            .model = strdup("abab5.5s-chat"),
            .tokens_to_generate = 256,
            .temperature = 0.2f,
            .top_p = 0.95f

        };
    return payload;
}
/**
 * @brief Converts an LLM payload to a JSON string.
 *
 * This function takes an LLM payload structure and converts it into a JSON string
 * that can be used for making requests to the language model API.
 *
 * @param body The payload structure containing the data to convert to JSON.
 * @return A dynamically allocated string containing the JSON representation of the payload.
 *         The caller is responsible for freeing this memory when no longer needed.
 */
static char *_app_minmax_llm_payload_to_json(const llm_payload_t *body)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *bot_setting_array = cJSON_CreateArray();
    cJSON *bot_setting_item = cJSON_CreateObject();
    cJSON *messages_array = cJSON_CreateArray();
    cJSON *messages_item = cJSON_CreateObject();
    cJSON *reply_constraints = cJSON_CreateObject();

    // Populate the bot_setting item
    cJSON_AddStringToObject(bot_setting_item, "bot_name", body->bot_setting[0].bot_name);
    cJSON_AddStringToObject(bot_setting_item, "content", body->bot_setting[0].content);

    // Add the bot_setting item to the array
    cJSON_AddItemToArray(bot_setting_array, bot_setting_item);

    // Populate the messages item
    cJSON_AddStringToObject(messages_item, "sender_type", body->messages[0].sender_type);
    cJSON_AddStringToObject(messages_item, "sender_name", body->messages[0].sender_name);
    cJSON_AddStringToObject(messages_item, "text", body->messages[0].text);

    // Add the messages item to the array
    cJSON_AddItemToArray(messages_array, messages_item);

    // Populate the reply_constraints object
    cJSON_AddStringToObject(reply_constraints, "sender_type", body->reply_constraints.sender_type);
    cJSON_AddStringToObject(reply_constraints, "sender_name", body->reply_constraints.sender_name);

    // Add sub-objects to the root object
    cJSON_AddItemToObject(root, "bot_setting", bot_setting_array);
    cJSON_AddItemToObject(root, "messages", messages_array);
    cJSON_AddItemToObject(root, "reply_constraints", reply_constraints);
    cJSON_AddStringToObject(root, "model", body->model);
    cJSON_AddNumberToObject(root, "tokens_to_generate", body->tokens_to_generate);
    cJSON_AddNumberToObject(root, "temperature", body->temperature);
    cJSON_AddNumberToObject(root, "top_p", body->top_p);

    // Convert the JSON object to a string
    char *json_str = cJSON_PrintUnformatted(root);

    ESP_LOGI(TAG, "%s\n", json_str);

    // Clean up the cJSON objects
    cJSON_Delete(root);

    return json_str;
}
/**
 * @brief HTTP event handler function
 *
 * This function handles events from the HTTP client and performs corresponding actions based on different event types.
 * Event types include error, successful connection, header sent, header received, data received, request finished, disconnected, and redirection.
 *
 * @param evt Pointer to an esp_http_client_event_t structure containing the event details.
 *
 * @return esp_err_t The return status of the function.
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
        llm_rx_total_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        if ((llm_rx_total_len + evt->data_len) < LLM_SIZE)
        {
            memcpy(llm_rx_buffer + llm_rx_total_len, (char *)evt->data, evt->data_len);
            llm_rx_total_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH:%" PRIu32 ", %" PRIu32 " K", llm_rx_total_len, llm_rx_total_len / 1024);

        // Parse JSON data
        ESP_LOGI(TAG, "%s\n", llm_rx_buffer);
        ESP_LOGI(TAG, "--LLM Finished--");

        _parse_from_response_json((char *)llm_rx_buffer);

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
 * @brief Initializes the HTTPS client with a URL
 *
 * This function initializes the HTTPS client using the provided configuration.
 * It checks if the global client pointer is null before initializing a new client.
 * If the initialization fails, it logs an error message.
 *
 * @return None
 */
static void _https_with_url_init(void)
{
    if (_g_client == NULL)
    {
        esp_http_client_config_t config = {
            .url = CONFIG_MINMAX_LLM_URL,
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
void app_minmax_llm_https_post(const char *text)
{
    if (_g_client == NULL)
    {
        _https_with_url_init();
    }

    esp_http_client_set_url(_g_client, CONFIG_MINMAX_LLM_URL);
    esp_http_client_set_method(_g_client, HTTP_METHOD_POST);

    // Set the Authorization header
    char auth_header[700];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_MINMAX_API_KEY);
    esp_http_client_set_header(_g_client, "Authorization", auth_header);
    esp_http_client_set_header(_g_client, "Content-Type", "application/json");

    // Set the payload
    _g_request_payload = _app_minmax_llm_payload_init(text);
    char *post_data = _app_minmax_llm_payload_to_json(&_g_request_payload);
    ESP_LOGI(TAG, "test=%s", _g_request_payload.messages->text);

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

void app_minmax_llm_deinit(void)
{
    // Free the allocated memory for the payload
    free(_g_request_payload.bot_setting[0].bot_name);
    free(_g_request_payload.bot_setting[0].content);
    free(_g_request_payload.messages[0].sender_type);
    free(_g_request_payload.messages[0].sender_name);
    free(_g_request_payload.messages[0].text);
    free(_g_request_payload.reply_constraints.sender_type);
    free(_g_request_payload.reply_constraints.sender_name);
    free(_g_request_payload.model);

    heap_caps_free(llm_rx_buffer);

    if (_g_client != NULL)
    {
        esp_http_client_cleanup(_g_client);
    }
}

void app_minmax_llm_init(void)
{
    llm_rx_buffer = heap_caps_calloc(1, LLM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(llm_rx_buffer);
    ESP_LOGI(TAG, "llm_rx_buffer with a size: %zu\n", LLM_SIZE);
    _https_with_url_init();
}