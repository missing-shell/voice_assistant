#ifndef _APP_MINMAX_LLM_H_
#define _APP_MINMAX_LLM_H_

#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct
    {
        char *bot_name;
        char *content;
    } bot_setting_t;

    typedef struct
    {
        char *sender_type;
        char *sender_name;
        char *text;
    } message_t;

    typedef struct
    {
        bot_setting_t bot_setting[1]; // Initialize as an array of one element
        message_t messages[1];        // Initialize as an array of one element
        struct
        {
            char *sender_type;
            char *sender_name;
        } reply_constraints;
        char *model;
        uint16_t tokens_to_generate;
        float temperature;
        float top_p;
    } llm_payload_t;

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE

    /**
     * @brief Posts data to the LLM service over HTTPS
     *
     * This function sends a POST request to the LLM service with the specified text.
     * It is only available when the certificate bundle is configured.
     *
     * @param text The text to be sent in the POST request.
     *
     * @return None
     */
    void app_minmax_llm_https_post(const char *text);
#endif // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE

    /**
     * @brief Deinitializes the LLM service
     *
     * This function deinitializes the LLM service, freeing any allocated resources.
     *
     * @return None
     */
    void app_minmax_llm_deinit(void);

    /**
     * @brief Initializes the LLM service
     *
     * This function initializes the LLM service, setting up any necessary configurations.
     *
     * @return None
     */
    void app_minmax_llm_init(void);
#ifdef __cplusplus
}
#endif

#endif