#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"

#include "app_audio.h"
#include "bsp_board.h"
#include "bsp/esp-bsp.h"
#include "audio_player.h"

#include "app_wifi.h"

static const char *TAG = "app_audio";

#if !CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite
static bool mute_flag = true;
#endif
bool record_flag = false;
static uint32_t record_total_len = 0;
audio_play_finish_cb_t audio_play_finish_cb = NULL;

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    bsp_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);
    // restore the voice volume upon unmuting
    if (setting == AUDIO_PLAYER_UNMUTE)
    {
        bsp_codec_volume_set(CONFIG_VOLUME_LEVEL, NULL);
    }
    return ESP_OK;
}

static esp_err_t audio_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;
    ret = bsp_codec_set_fs(rate, bits_cfg, ch);

    bsp_codec_mute_set(true);
    bsp_codec_mute_set(false);
    bsp_codec_volume_set(CONFIG_VOLUME_LEVEL, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));

    return ret;
}

static void audio_player_cb(audio_player_cb_ctx_t *ctx)
{
    switch (ctx->audio_event)
    {
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
        ESP_LOGI(TAG, "Player IDLE");
        bsp_codec_set_fs(16000, 16, 2);
        if (audio_play_finish_cb)
        {
            audio_play_finish_cb();
        }
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_COMPLETED_PLAYING_NEXT:
        ESP_LOGI(TAG, "Player NEXT");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
        ESP_LOGI(TAG, "Player PLAYING");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
        ESP_LOGI(TAG, "Player PAUSE");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_SHUTDOWN:
        ESP_LOGI(TAG, "Player SHUTDOWN");
        break;
    default:
        break;
    }
}

/**
 * Initializes audio recording.
 * This function is responsible for allocating memory for audio recording buffers, initializing the file iterator,
 * and setting up the audio player with specific configuration and callback functions.
 *
 * @note If DEBUG_SAVE_PCM is defined, it will allocate memory from SPIRAM for recording audio buffer and audio receive buffer.
 * If memory allocation fails, it will print an error message and return immediately.
 */
void audio_record_init()
{

    /* Configure audio player with specific functions and priority */
    audio_player_config_t config = {.mute_fn = audio_mute_function,
                                    .write_fn = bsp_i2s_write,
                                    .clk_set_fn = audio_codec_set_fs,
                                    .priority = 5};
    /* Create a new audio player instance with the given configuration */
    ESP_ERROR_CHECK(audio_player_new(config));
    /* Register the audio player callback function */
    audio_player_callback_register(audio_player_cb, NULL);
}

void audio_register_play_finish_cb(audio_play_finish_cb_t cb)
{
    audio_play_finish_cb = cb;
}

/**
 * @brief Audio playback task function
 *
 * This function is responsible for playing audio files. It first opens the file, reads the WAV header information,
 * sets the audio codec parameters according to the header information, and then reads the audio data in chunks and sends it to the codec for playback.
 *
 * @param filepath Pointer to the file path of the audio file
 * @return esp_err_t Return the execution result, ESP_OK for success, other values for failure
 */
esp_err_t audio_play_task(void *filepath)
{
    /* File pointer */
    FILE *fp = NULL;
    /* File status structure */
    struct stat file_stat;
    /* Error handling variable, initialized to success status */
    esp_err_t ret = ESP_OK;

    /* Define the size of each read chunk, 4096 bytes here */
    const size_t chunk_size = 4096;
    /* Allocate a buffer for reading audio data */
    uint8_t *buffer = malloc(chunk_size);
    /* If the buffer allocation fails, jump to the error handling part */
    ESP_GOTO_ON_FALSE(NULL != buffer, ESP_FAIL, EXIT, TAG, "buffer malloc failed");

    /* Get the file status information, if failed, jump to the error handling part */
    ESP_GOTO_ON_FALSE(-1 != stat(filepath, &file_stat), ESP_FAIL, EXIT, TAG, "Failed to stat file");

    /* Open the audio file, if failed, jump to the error handling part */
    fp = fopen(filepath, "r");
    ESP_GOTO_ON_FALSE(NULL != fp, ESP_FAIL, EXIT, TAG, "Failed create record file");

    /* Read the WAV header information, if reading fails, jump to the error handling part */
    wav_header_t wav_head;
    int len = fread(&wav_head, 1, sizeof(wav_header_t), fp);
    ESP_GOTO_ON_FALSE(len > 0, ESP_FAIL, EXIT, TAG, "Read wav header failed");

    /* If it is not a WAV file (does not contain "fmt" and "data" subblocks), treat it as PCM format */
    if (NULL == strstr((char *)wav_head.Subchunk1ID, "fmt") &&
        NULL == strstr((char *)wav_head.Subchunk2ID, "data"))
    {
        ESP_LOGI(TAG, "PCM format");
        /* Set the file pointer to the beginning of the file */
        fseek(fp, 0, SEEK_SET);
        /* Set the audio parameters for PCM format */
        wav_head.SampleRate = 16000;
        wav_head.NumChannels = 2;
        wav_head.BitsPerSample = 16;
    }

    /* Output audio parameters and set the codec's sampling rate and bit width */
    ESP_LOGI(TAG, "frame_rate= %" PRIi32 ", ch=%d, width=%d", wav_head.SampleRate, wav_head.NumChannels, wav_head.BitsPerSample);
    bsp_codec_set_fs(wav_head.SampleRate, wav_head.BitsPerSample, I2S_SLOT_MODE_STEREO);

    /* Mute the codec, then unmute to ensure the volume is set correctly */
    bsp_codec_mute_set(true);
    bsp_codec_mute_set(false);
    /* Set the playback volume */
    bsp_codec_volume_set(CONFIG_VOLUME_LEVEL, NULL);

    /* Read and play audio data in chunks */
    size_t cnt, total_cnt = 0;
    do
    {
        /* Read audio data into the buffer */
        len = fread(buffer, 1, chunk_size, fp);
        /* If the read length is 0 or less, end the loop */
        if (len <= 0)
        {
            break;
        }
        /* If the read length is greater than 0, write the data to the codec */
        else if (len > 0)
        {
            bsp_i2s_write(buffer, len, &cnt, portMAX_DELAY);
            total_cnt += cnt;
        }
    } while (1);

EXIT:
    /* Close the file and free the buffer before returning */
    if (fp)
    {
        fclose(fp);
    }
    if (buffer)
    {
        free(buffer);
    }
    /* Return the execution result */
    return ret;
}
