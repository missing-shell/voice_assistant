#ifndef _APP_BAIDU_IAR_H_
#define _APP_BAIDU_IAR_H_

#ifdef __cplusplus
extern "C"
{
#endif
    /**
     * @brief Structure to hold the payload for a Baidu IAR request.
     *
     * This structure defines the necessary fields for a request to the Baidu Intelligent
     * Audio Recognition (IAR) service. It includes information about the audio format,
     * sample rate, channel count, user credentials, and the encoded speech data.
     */
    typedef struct
    {
        char *format; /**< Audio format, e.g., "pcm". */
        int rate;     /**< Sample rate in Hz, e.g., 16000. */
        int channel;  /**< Number of audio channels, typically 1 for mono. */
        char *cuid;   /**< Client unique identifier. */
        char *token;  /**< Access token for the Baidu IAR service. */
        char *speech; /**< Base64 encoded audio file content. The binary content of the  audio file
                         should be read and then base64 encoded before being placed here. */
        int len;      /**< Original size of the audio file in bytes, i.e., the number of bytes before
                          base64 encoding. This refers to the size of the binary content. */
    } iar_payload_t;

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    /**
     * @brief Posts data to the Baidu IAR service via HTTPS.
     *
     * This function performs an HTTPS POST request to the Baidu IAR service using the provided
     * speech data. It encodes the speech data using Base64, creates a payload, and sends it
     * to the Baidu IAR service.
     *
     * @param speech Pointer to the speech data in WAV format.
     * @param wav_len Length of the speech data in bytes.
     */
    void app_baidu_iar_https_post(const char *speech, const uint32_t wav_len);
#endif // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE

    /**
     * @brief Deinitializes resources used by the Baidu IAR service.
     *
     * This function frees up memory allocated for the IAR response buffer and
     * deinitializes the HTTP client, ensuring proper cleanup of resources.
     */
    void app_baidu_iar_deinit(void);

    /**
     * @brief Initializes resources for the Baidu IAR service.
     *
     * This function initializes the necessary resources for making requests to the Baidu IAR service.
     * It allocates memory for the IAR response buffer and sets up the HTTP client.
     */
    void app_baidu_iar_init(void);

#ifdef __cplusplus
}
#endif

#endif