#pragma once

#include "string.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "zh_vector.h"

#define ZH_NETWORK_MAX_MESSAGE_SIZE 218 // Maximum value of the transmitted data size (value range from 1 to 218).

#define ZH_NETWORK_INIT_CONFIG_DEFAULT() \
    {                                    \
        .network_id = 0xFAFBFCFD,        \
        .task_priority = 4,              \
        .stack_size = 2048,              \
        .queue_size = 32,                \
        .max_waiting_time = 500,         \
        .id_vector_size = 100,           \
        .route_vector_size = 100,        \
        .wifi_interface = WIFI_IF_STA    \
    }

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct zh_network_init_config_t
    {
        uint32_t network_id;             // A unique ID for the mesh network.
        uint8_t task_priority;           // Task priority for the messages processing.
        uint16_t stack_size;             // Task stask size for the messages processing.
        uint8_t queue_size;              // Size of messages processing queue.
        uint16_t max_waiting_time;       // Maximum time to wait a response message from target node. In milliseconds.
        uint16_t id_vector_size;         // Maximum size of unique ID of received messages. If the size is exceeded, the first value will be deleted.
        uint16_t route_vector_size;      // The maximum size of the routing table. If the size is exceeded, the first route will be deleted.
        wifi_interface_t wifi_interface; // WiFi interface used for ESP-NOW data transmission.
    } __attribute__((packed)) zh_network_init_config_t;

    ESP_EVENT_DECLARE_BASE(ZH_NETWORK);

    typedef enum zh_network_event_type_t
    {
        ZH_NETWORK_ON_RECV_EVENT,
        ZH_NETWORK_ON_SEND_EVENT
    } __attribute__((packed)) zh_network_event_type_t;

    typedef enum zh_network_on_send_event_type_t
    {
        ZH_NETWORK_SEND_SUCCESS,
        ZH_NETWORK_SEND_FAIL
    } __attribute__((packed)) zh_network_on_send_event_type_t;

    typedef struct zh_network_event_on_send_t
    {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN];
        zh_network_on_send_event_type_t status;
    } __attribute__((packed)) zh_network_event_on_send_t;

    typedef struct zh_network_event_on_recv_t
    {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN];
        uint8_t *data;
        uint8_t data_len;
    } __attribute__((packed)) zh_network_event_on_recv_t;

    /**
     * @brief      Initialization of zh_network.
     *
     * @param[in]  config  Pointer to an initialized zh_network configuration structure. May point to a temporary variable.
     *
     * @return
     *              - ESP_OK if initialization was successful
     *              - ESP_ERR_WIFI_NOT_INIT if WiFi is not initialized by esp_wifi_init
     *              - ESP_ERR_INVALID_SIZE if the maximum value of the transmitted data size is incorrect
     */
    esp_err_t zh_network_init(zh_network_init_config_t *config);

    /**
     * @brief      Deinitialization of zh_network.
     *
     */
    void zh_network_deinit(void);

    /**
     * @brief      Sending zh_network data.
     *
     * @param[in]  target    Pointer to a buffer containing the eight-byte target MAC. Can be NULL for broadcast transmission.
     * @param[in]  data      Pointer to the buffer containing the data to be sent.
     * @param[in]  data_len  Length of transmitted data.
     *
     * * @return
     *              - ESP_OK if sent was successful
     *              - ESP_ERR_INVALID_SIZE if any error
     */
    esp_err_t zh_network_send(const uint8_t *target, const uint8_t *data, const uint8_t data_len);

    /**
     * @brief      Get routing table.
     *
     * @return
     *              - Pointer to routing table
     */
    void *zh_network_get_route(void);

#ifdef __cplusplus
}
#endif