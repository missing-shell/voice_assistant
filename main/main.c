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
#include "app_agent.h"

const char *TAG = "main";

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_spiffs_mount();
    bsp_i2c_init();
    bsp_board_init();

    ESP_ERROR_CHECK(esp_netif_init());
    app_wifi_init();
    app_wifi_connect("208", "iot208208208");

    audio_record_init();

    vTaskDelay(pdMS_TO_TICKS(3000));

    if (app_wifi_get_connect_status())
    {
        app_agent_init();
        app_sr_start(false);
    }
}
