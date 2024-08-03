#ifndef __APP_WIFI_H__
#define __APP_WIFI_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include "stdbool.h"

    /**
     * @brief Gets the Wi-Fi connection status.
     *
     * This function checks whether the Wi-Fi module is currently connected to an Access Point (AP).
     *
     * @return true if connected, false otherwise
     */
    bool app_wifi_get_connect_status(void);

    /**
     * @brief Connects to a specified Wi-Fi network.
     *
     * Attempts to connect to a Wi-Fi network using the provided Service Set Identifier (SSID) and password.
     *
     * @param ssid The SSID of the Wi-Fi network to connect to
     * @param password The password for the Wi-Fi network
     */
    void app_wifi_connect(const char *ssid, const char *password);

    /**
     * @brief Starts the smart configuration mode.
     *
     * Puts the Wi-Fi module into smart configuration mode, waiting for network configuration information from a mobile device.
     */
    void app_wifi_smartconfig_start(void);

    /**
     * @brief Initializes the Wi-Fi module.
     *
     * Performs necessary initialization steps to prepare the Wi-Fi module.
     */
    void app_wifi_init(void);

#ifdef __cplusplus
}
#endif

#endif // __APP_WIFI_H__