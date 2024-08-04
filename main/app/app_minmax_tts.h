#ifndef _APP_MINMAX_TTS_H_
#define _APP_MINMAX_TTS_H_

#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct
    {
        char *model;
        char *text;
        bool stream;
        struct
        {
            char *voice_id;
            int speed;
            int vol;
            int pitch;
        } voice_setting;
        struct
        {
            int sample_rate;
            int bitrate;
            char *format;
            int channel;
        } audio_setting;
    } tts_payload_t;

    /**
     * @defgroup mbedtls_certificate_bundle mbedtls Certificate Bundle Configuration
     * @ingroup app_minmax_tts
     * @{
     *
     * These functions are enabled when the CONFIG_MBEDTLS_CERTIFICATE_BUNDLE option is set.
     * This option configures the use of a pre-defined certificate bundle for secure HTTPS connections.
     *
     * @note To enable these functions, set CONFIG_MBEDTLS_CERTIFICATE_BUNDLE to 1 in the project configuration.
     */

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE

    /**
     * Posts text to TTS service over HTTPS.
     *
     * @param text The text to be processed by the TTS service.
     */
    void app_minmax_tts_https_post(const char *text);
#endif // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE

    /**
     * @}
     */

    /**
     * Deinitializes the TTS application.
     */
    void app_minmax_tts_deinit(void);

    /**
     * Initializes the TTS application.
     */
    void app_minmax_tts_init(void);
#ifdef __cplusplus
}
#endif

#endif