#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "bsp/esp-bsp.h"
#include "bsp_board.h"

#include "audio_player.h"
// APP
#include "app_wifi.h"
#include "app_minmax_tts.h"
#include "app_audio.h"
#include "app_minmax_llm.h"
#include "app_baidu_iar.h"
#include "app_sr.h"

static const char *TAG = "APP_AGENT";
void app_agent_task(void *arg)
{
    ESP_LOGI(TAG, "--APP_AGENT_START--");
    app_minmax_tts_init();
    app_minmax_llm_init();
    app_baidu_iar_init();
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    vTaskDelete(NULL);
}
void app_agent_init(void)
{
    xTaskCreate(app_agent_task, "app_agent_task", 8192, NULL, 7, NULL);
}