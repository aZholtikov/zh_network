/**
 * @file
 * Header file for the zh_network component.
 *
 */

#pragma once

#include "string.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "zh_vector.h"
#ifdef CONFIG_IDF_TARGET_ESP8266
#include "esp_system.h"
#else
#include "esp_random.h"
#include "esp_mac.h"
#endif

/**
 * @brief Unique identifier of ESP-NOW interface events base. Used when registering the event handler.
 *
 */
#define ESP_EVENT_BASE ZH_NETWORK

/**
 * @brief Maximum value of the transmitted data size.
 *
 * @note Value range from 1 to 218. Smaller size - higher transmission speed.
 *
 * @attention All devices on the network must have the same ZH_NETWORK_MAX_MESSAGE_SIZE.
 */
#define ZH_NETWORK_MAX_MESSAGE_SIZE 218

/**
 * @brief Default values for zh_network_init_config_t structure for initial initialization of ESP-NOW interface.
 *
 */
#define ZH_NETWORK_INIT_CONFIG_DEFAULT() \
    {                                    \
        .network_id = 0xFAFBFCFD,        \
        .task_priority = 4,              \
        .stack_size = 3072,              \
        .queue_size = 32,                \
        .max_waiting_time = 1000,        \
        .id_vector_size = 100,           \
        .route_vector_size = 100,        \
        .wifi_interface = WIFI_IF_STA    \
    }

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Structure for initial initialization of ESP-NOW interface.
     *
     * @note Before initialize ESP-NOW interface recommend initialize zh_network_init_config_t structure with default values.
     *
     * @code zh_network_init_config_t config = ZH_NETWORK_INIT_CONFIG_DEFAULT() @endcode
     */
    typedef struct
    {
        uint32_t network_id;             ///< A unique ID for the mesh network. @attention The ID must be the same for all nodes in the network.
        uint8_t task_priority;           ///< Task priority for the ESP-NOW messages processing. @note It is not recommended to set a value less than 4.
        uint16_t stack_size;             ///< Stack size for task for the ESP-NOW messages processing. @note The minimum size is 3072 bytes.
        uint8_t queue_size;              ///< Queue size for task for the ESP-NOW messages processing. @note The size depends on the number of messages to be processed. It is not recommended to set the value less than 32.
        uint16_t max_waiting_time;       ///< Maximum time to wait a response message from target node (in milliseconds). @note If a response message from the target node is not received within this time, the status of the sent message will be "sent fail".
        uint16_t id_vector_size;         ///< Maximum size of unique ID of received messages. @note If the size is exceeded, the first value will be deleted. Minimum recommended value: number of planned nodes in the network + 10%.
        uint16_t route_vector_size;      ///< The maximum size of the routing table. @note If the size is exceeded, the first route will be deleted. Minimum recommended value: number of planned nodes in the network + 10%.
        wifi_interface_t wifi_interface; ///< WiFi interface (STA or AP) used for ESP-NOW operation. @note The MAC address of the device depends on the selected WiFi interface.
    } zh_network_init_config_t;

    /// \cond
    ESP_EVENT_DECLARE_BASE(ESP_EVENT_BASE);
    /// \endcond

    /**
     * @brief Enumeration of possible ESP-NOW events.
     *
     */
    typedef enum
    {
        ZH_NETWORK_ON_RECV_EVENT, ///< The event when the ESP-NOW message was received.
        ZH_NETWORK_ON_SEND_EVENT  ///< The event when the ESP-NOW message was sent.
    } zh_network_event_type_t;

    /**
     * @brief Enumeration of possible status of sent ESP-NOW message.
     *
     */
    typedef enum
    {
        ZH_NETWORK_SEND_SUCCESS, ///< If ESP-NOW message was sent success.
        ZH_NETWORK_SEND_FAIL     ///< If ESP-NOW message was sent fail.
    } zh_network_on_send_event_type_t;

    /**
     * @brief Structure for sending data to the event handler when an ESP-NOW message was sent.
     *
     * @note Should be used with ZH_NETWORK event base and ZH_NETWORK_ON_SEND_EVENT event.
     */
    typedef struct
    {
        uint8_t mac_addr[6];                    ///< MAC address of the device to which the ESP-NOW message was sent. @note
        zh_network_on_send_event_type_t status; ///< Status of sent ESP-NOW message. @note
    } zh_network_event_on_send_t;

    /**
     * @brief Structure for sending data to the event handler when an ESP-NOW message was received.
     *
     * @note Should be used with ZH_NETWORK event base and ZH_NETWORK_ON_RECV_EVENT event.
     */
    typedef struct
    {
        uint8_t mac_addr[6]; ///< MAC address of the sender ESP-NOW message. @note
        uint8_t *data;       ///< Pointer to the data of the received ESP-NOW message. @note
        uint8_t data_len;    ///< Size of the received ESP-NOW message. @note
    } zh_network_event_on_recv_t;

    /**
     * @brief Initialize ESP-NOW interface.
     *
     * @note Before initialize ESP-NOW interface recommend initialize zh_network_init_config_t structure with default values.
     *
     * @code zh_network_init_config_t config = ZH_NETWORK_INIT_CONFIG_DEFAULT() @endcode
     *
     * @param[in] config Pointer to ESP-NOW initialized configuration structure. Can point to a temporary variable.
     *
     * @return
     *              - ESP_OK if initialization was success
     *              - ESP_ERR_INVALID_ARG if parameter error
     *              - ESP_ERR_WIFI_NOT_INIT if WiFi is not initialized
     *              - ESP_FAIL if any internal error
     */
    esp_err_t zh_network_init(zh_network_init_config_t *config);

    /**
     * @brief Deinitialize ESP-NOW interface.
     *
     * @return
     *              - ESP_OK if deinitialization was success
     *              - ESP_FAIL if ESP-NOW is not initialized
     */
    esp_err_t zh_network_deinit(void);

    /**
     * @brief Send ESP-NOW data.
     *
     * @param[in] target Pointer to a buffer containing an eight-byte target MAC. Can be NULL for broadcast.
     * @param[in] data Pointer to a buffer containing the data for send.
     * @param[in] data_len Sending data length.
     *
     * @note The function will return an ESP_ERR_INVALID_STATE error if less than 50% of the size set at initialization remains in the message queue.
     *
     * @return
     *              - ESP_OK if sent was success
     *              - ESP_ERR_INVALID_ARG if parameter error
     *              - ESP_ERR_INVALID_STATE if queue for outgoing data is almost full
     *              - ESP_FAIL if ESP-NOW is not initialized
     */
    esp_err_t zh_network_send(const uint8_t *target, const uint8_t *data, const uint8_t data_len);

#ifdef __cplusplus
}
#endif