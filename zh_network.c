/**
 * @file
 * The main code of the zh_network component.
 */

#include "zh_network.h"

/// \cond
#define ZH_NETWORK_DATA_SEND_SUCCESS BIT0
#define ZH_NETWORK_DATA_SEND_FAIL BIT1
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
/// \endcond

static void s_zh_network_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
#ifdef CONFIG_IDF_TARGET_ESP8266
static void s_zh_network_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int data_len);
#else
static void s_zh_network_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len);
#endif
static void s_zh_network_processing(void *pvParameter);

static const char *TAG = "zh_network";

static EventGroupHandle_t s_zh_network_send_cb_status_event_group_handle = {0};
static QueueHandle_t s_zh_network_queue_handle = {0};
static TaskHandle_t s_zh_network_processing_task_handle = {0};
static SemaphoreHandle_t s_id_vector_mutex = {0};
static zh_network_init_config_t s_zh_network_init_config = {0};
static zh_vector_t s_id_vector = {0};
static zh_vector_t s_route_vector = {0};
static zh_vector_t s_response_vector = {0};
static uint8_t s_self_mac[6] = {0};
static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool s_zh_network_is_initialized = false;

/// \cond
typedef enum
{
    ZH_NETWORK_BROADCAST,        // Broadcast.
    ZH_NETWORK_UNICAST,          // Unicast.
    ZH_NETWORK_DELIVERY_CONFIRM, // Unicast.
    ZH_NETWORK_SEARCH_REQUEST,   // Broadcast.
    ZH_NETWORK_SEARCH_RESPONSE   // Broadcast.
} __attribute__((packed)) zh_network_message_type_t;

typedef struct
{
    zh_network_message_type_t message_type;
    uint32_t network_id;
    uint32_t message_id;
    uint32_t confirm_id;
    uint8_t original_target_mac[6];
    uint8_t original_sender_mac[6];
    uint8_t sender_mac[6];
    uint8_t data[ZH_NETWORK_MAX_MESSAGE_SIZE];
    uint8_t data_len;
} __attribute__((packed)) zh_network_data_t;

typedef struct
{
    uint8_t original_target_mac[6];
    uint8_t intermediate_target_mac[6];
} __attribute__((packed)) zh_network_routing_table_t;

typedef enum
{
    ZH_NETWORK_ON_SEND,
    ZH_NETWORK_ON_RECV,
    ZH_NETWORK_WAIT_ROUTE,
    ZH_NETWORK_WAIT_RESPONSE,
} __attribute__((packed)) zh_network_queue_id_t;

typedef struct
{
    zh_network_queue_id_t id;
    uint64_t time;
    zh_network_data_t data;
} __attribute__((packed)) zh_network_queue_t;

ESP_EVENT_DEFINE_BASE(ZH_NETWORK);
/// \endcond

esp_err_t zh_network_init(zh_network_init_config_t *config)
{
    ESP_LOGI(TAG, "ESP-NOW initialization begin.");
    if (config == NULL)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. Invalid argument.");
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) == ESP_ERR_WIFI_NOT_INIT)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. WiFi not initialized.");
        return ESP_ERR_WIFI_NOT_INIT;
    }
    if (sizeof(zh_network_data_t) > ESP_NOW_MAX_DATA_LEN)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. The maximum value of the transmitted data size is incorrect.");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_zh_network_init_config.wifi_interface == WIFI_IF_STA)
    {
        esp_read_mac(s_self_mac, ESP_MAC_WIFI_STA);
    }
    else
    {
        esp_read_mac(s_self_mac, ESP_MAC_WIFI_SOFTAP);
    }
    s_zh_network_init_config = *config;
    s_zh_network_send_cb_status_event_group_handle = xEventGroupCreate();
    s_zh_network_queue_handle = xQueueCreate(s_zh_network_init_config.queue_size, sizeof(zh_network_queue_t));
    zh_vector_init(&s_id_vector, sizeof(uint32_t), false);
    zh_vector_init(&s_route_vector, sizeof(zh_network_routing_table_t), false);
    zh_vector_init(&s_response_vector, sizeof(uint32_t), false);
    s_id_vector_mutex = xSemaphoreCreateMutex();
    if (esp_now_init() != ESP_OK || esp_now_register_send_cb(s_zh_network_send_cb) != ESP_OK || esp_now_register_recv_cb(s_zh_network_recv_cb) != ESP_OK)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. Internal error.");
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(&s_zh_network_processing, "NULL", s_zh_network_init_config.stack_size, NULL, s_zh_network_init_config.task_priority, &s_zh_network_processing_task_handle, tskNO_AFFINITY) != pdPASS)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. Internal error.");
        return ESP_FAIL;
    }
    s_zh_network_is_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW initialization success.");
    return ESP_OK;
}

esp_err_t zh_network_deinit(void)
{
    ESP_LOGI(TAG, "ESP-NOW deinitialization begin.");
    vEventGroupDelete(s_zh_network_send_cb_status_event_group_handle);
    vQueueDelete(s_zh_network_queue_handle);
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    zh_vector_free(&s_id_vector);
    zh_vector_free(&s_route_vector);
    zh_vector_free(&s_response_vector);
    vTaskDelete(s_zh_network_processing_task_handle);
    s_zh_network_is_initialized = false;
    ESP_LOGI(TAG, "ESP-NOW deinitialization success.");
    return ESP_OK;
}

esp_err_t zh_network_send(const uint8_t *target, const uint8_t *data, const uint8_t data_len)
{
    if (target == NULL)
    {
        ESP_LOGI(TAG, "Adding outgoing ESP-NOW data to MAC FF:FF:FF:FF:FF:FF to queue begin.");
    }
    else
    {
        ESP_LOGI(TAG, "Adding outgoing ESP-NOW data to MAC %02X:%02X:%02X:%02X:%02X:%02X to queue begin.", MAC2STR(target));
    }
    if (s_zh_network_is_initialized == false)
    {
        ESP_LOGE(TAG, "Adding outgoing ESP-NOW data to queue fail. ESP-NOW not initialized.");
        return ESP_FAIL;
    }
    if (data_len == 0 || data == NULL || data_len > ZH_NETWORK_MAX_MESSAGE_SIZE)
    {
        ESP_LOGE(TAG, "Adding outgoing ESP-NOW data to queue fail. Invalid argument.");
        return ESP_ERR_INVALID_ARG;
    }
    if (uxQueueSpacesAvailable(s_zh_network_queue_handle) < s_zh_network_init_config.queue_size / 2)
    {
        ESP_LOGW(TAG, "Adding outgoing ESP-NOW data to queue fail. Queue is almost full.");
        return ESP_ERR_INVALID_STATE;
    }
    zh_network_queue_t zh_network_queue = {0};
    zh_network_queue.id = ZH_NETWORK_ON_SEND;
    zh_network_data_t *send_data = &zh_network_queue.data;
    send_data->network_id = s_zh_network_init_config.network_id;
    send_data->message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
    memcpy(send_data->original_sender_mac, &s_self_mac, 6);
    if (target == NULL)
    {
        send_data->message_type = ZH_NETWORK_BROADCAST;
        memcpy(send_data->original_target_mac, &s_broadcast_mac, 6);
    }
    else
    {
        if (memcmp(target, &s_broadcast_mac, 6) != 0)
        {
            send_data->message_type = ZH_NETWORK_UNICAST;
            memcpy(send_data->original_target_mac, target, 6);
        }
        else
        {
            send_data->message_type = ZH_NETWORK_BROADCAST;
            memcpy(send_data->original_target_mac, &s_broadcast_mac, 6);
        }
    }
    memcpy(&send_data->data, data, data_len);
    send_data->data_len = data_len;
    if (target == NULL)
    {
        ESP_LOGI(TAG, "Adding outgoing ESP-NOW data to MAC FF:FF:FF:FF:FF:FF to queue success.");
    }
    else
    {
        ESP_LOGI(TAG, "Adding outgoing ESP-NOW data to MAC %02X:%02X:%02X:%02X:%02X:%02X to queue success.", MAC2STR(target));
    }
    if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
    {
        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
    }
    return ESP_OK;
}

static void s_zh_network_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        xEventGroupSetBits(s_zh_network_send_cb_status_event_group_handle, ZH_NETWORK_DATA_SEND_SUCCESS);
    }
    else
    {
        xEventGroupSetBits(s_zh_network_send_cb_status_event_group_handle, ZH_NETWORK_DATA_SEND_FAIL);
    }
}

#ifdef CONFIG_IDF_TARGET_ESP8266
static void s_zh_network_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int data_len)
#else
static void s_zh_network_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len)
#endif
{
#ifdef CONFIG_IDF_TARGET_ESP8266
    ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue begin.", MAC2STR(mac_addr));
#else
    ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue begin.", MAC2STR(esp_now_info->src_addr));
#endif
    if (uxQueueSpacesAvailable(s_zh_network_queue_handle) < s_zh_network_init_config.queue_size / 2)
    {
        ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Queue is almost full.");
        return;
    }
    if (data_len == sizeof(zh_network_data_t))
    {
        zh_network_data_t *recv_data = (zh_network_data_t *)data;
        if (memcmp(&recv_data->network_id, &s_zh_network_init_config.network_id, sizeof(recv_data->network_id)) != 0)
        {
            ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Incorrect mesh network ID.");
            return;
        }
        for (uint16_t i = 0; i < zh_vector_get_size(&s_id_vector); ++i)
        {
            uint32_t *message_id = zh_vector_get_item(&s_id_vector, i);
            if (memcmp(&recv_data->message_id, message_id, sizeof(recv_data->message_id)) == 0)
            {
                ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Repeat message received.");
                return;
            }
        }
        if (xSemaphoreTake(s_id_vector_mutex, portTICK_PERIOD_MS) == pdTRUE)
        {
            zh_vector_push_back(&s_id_vector, &recv_data->message_id);
            if (zh_vector_get_size(&s_id_vector) > s_zh_network_init_config.id_vector_size)
            {
                zh_vector_delete_item(&s_id_vector, 0);
            }
            xSemaphoreGive(s_id_vector_mutex);
        }
        zh_network_queue_t zh_network_queue = {0};
        zh_network_queue.id = ZH_NETWORK_ON_RECV;
        recv_data = &zh_network_queue.data;
        memcpy(recv_data, data, data_len);
#ifdef CONFIG_IDF_TARGET_ESP8266
        memcpy(recv_data->sender_mac, mac_addr, 6);
#else
        memcpy(recv_data->sender_mac, esp_now_info->src_addr, 6);
#endif
#ifdef CONFIG_IDF_TARGET_ESP8266
        ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue success.", MAC2STR(mac_addr));
#else
        ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue success.", MAC2STR(esp_now_info->src_addr));
#endif
        if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
        {
            ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
        }
    }
    else
    {
        ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Incorrect ESP-NOW data size.");
    }
}

static void s_zh_network_processing(void *pvParameter)
{
    zh_network_queue_t zh_network_queue = {0};
    while (xQueueReceive(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY) == pdTRUE)
    {
        bool flag = false;
        zh_network_data_t *zh_network_data = &zh_network_queue.data;
        switch (zh_network_queue.id)
        {
        case ZH_NETWORK_ON_SEND:;
            ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processing begin.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
            esp_now_peer_info_t *peer = heap_caps_malloc(sizeof(esp_now_peer_info_t), MALLOC_CAP_8BIT);
            if (peer == NULL)
            {
                ESP_LOGE(TAG, "Outgoing ESP-NOW data processing fail. Memory allocation fail or no free memory in the heap.");
                heap_caps_free(peer);
                goto ZH_NETWORK_ON_SEND_EXIT;
            }
            memset(peer, 0, sizeof(esp_now_peer_info_t));
            peer->ifidx = s_zh_network_init_config.wifi_interface;
            if (zh_network_data->message_type == ZH_NETWORK_BROADCAST || zh_network_data->message_type == ZH_NETWORK_SEARCH_REQUEST || zh_network_data->message_type == ZH_NETWORK_SEARCH_RESPONSE)
            {
                memcpy(peer->peer_addr, &s_broadcast_mac, 6);
                if (memcmp(zh_network_data->original_sender_mac, &s_self_mac, 6) == 0)
                {
                    if (xSemaphoreTake(s_id_vector_mutex, portTICK_PERIOD_MS) == pdTRUE)
                    {
                        zh_vector_push_back(&s_id_vector, &zh_network_data->message_id);
                        if (zh_vector_get_size(&s_id_vector) > s_zh_network_init_config.id_vector_size)
                        {
                            zh_vector_delete_item(&s_id_vector, 0);
                        }
                        xSemaphoreGive(s_id_vector_mutex);
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Checking routing table to MAC %02X:%02X:%02X:%02X:%02X:%02X.", MAC2STR(zh_network_queue.data.original_target_mac));
                for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                {
                    zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                    if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, 6) == 0)
                    {
                        memcpy(peer->peer_addr, routing_table->intermediate_target_mac, 6);
                        flag = true;
                        ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X is found. Forwarding via MAC %02X:%02X:%02X:%02X:%02X:%02X.", MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        break;
                    }
                }
                if (flag == false)
                {
                    ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X not found.", MAC2STR(zh_network_queue.data.original_target_mac));
                    if (zh_network_data->message_type == ZH_NETWORK_UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    else
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    zh_network_queue.id = ZH_NETWORK_WAIT_ROUTE;
                    zh_network_queue.time = esp_timer_get_time() / 1000;
                    if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_SEARCH_REQUEST;
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, 6);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    heap_caps_free(peer);
                    goto ZH_NETWORK_ON_SEND_EXIT;
                }
            }
            if (esp_now_add_peer(peer) != ESP_OK)
            {
                ESP_LOGE(TAG, "Outgoing ESP-NOW data processing fail. Internal error with adding peer.");
                heap_caps_free(peer);
                goto ZH_NETWORK_ON_SEND_EXIT;
            }
            zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
            if (on_send == NULL)
            {
                ESP_LOGE(TAG, "Outgoing ESP-NOW data processing fail. Memory allocation fail or no free memory in the heap.");
                heap_caps_free(peer);
                heap_caps_free(on_send);
                goto ZH_NETWORK_ON_SEND_EXIT;
            }
            memset(on_send, 0, sizeof(zh_network_event_on_send_t));
            memcpy(on_send->mac_addr, zh_network_data->original_target_mac, 6);
            if (esp_now_send((uint8_t *)peer->peer_addr, (uint8_t *)zh_network_data, sizeof(zh_network_data_t)) != ESP_OK)
            {
                ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                heap_caps_free(peer);
                heap_caps_free(on_send);
                goto ZH_NETWORK_ON_SEND_EXIT;
            }
            EventBits_t bit = xEventGroupWaitBits(s_zh_network_send_cb_status_event_group_handle, ZH_NETWORK_DATA_SEND_SUCCESS | ZH_NETWORK_DATA_SEND_FAIL, pdTRUE, pdFALSE, 50 / portTICK_PERIOD_MS);
            if ((bit & ZH_NETWORK_DATA_SEND_SUCCESS) != 0)
            {
                if (memcmp(zh_network_data->original_sender_mac, &s_self_mac, 6) == 0)
                {
                    if (zh_network_data->message_type == ZH_NETWORK_BROADCAST)
                    {
                        ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        on_send->status = ZH_NETWORK_SEND_SUCCESS;
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                        }
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_SEARCH_REQUEST)
                    {
                        ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_SEARCH_RESPONSE)
                    {
                        ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to confirmation message waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        zh_network_queue.id = ZH_NETWORK_WAIT_RESPONSE;
                        zh_network_queue.time = esp_timer_get_time() / 1000;
                        if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                        }
                    }
                }
                else
                {
                    if (zh_network_data->message_type == ZH_NETWORK_BROADCAST)
                    {
                        ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_SEARCH_REQUEST)
                    {
                        ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_SEARCH_RESPONSE)
                    {
                        ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                }
            }
            else
            {
                if (memcmp(zh_network_data->original_target_mac, &s_broadcast_mac, 6) != 0)
                {
                    ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X is incorrect.", MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                    for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                    {
                        zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                        if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, 6) == 0)
                        {
                            zh_vector_delete_item(&s_route_vector, i);
                        }
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    zh_network_queue.id = ZH_NETWORK_WAIT_ROUTE;
                    zh_network_queue.time = esp_timer_get_time() / 1000;
                    if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    ESP_LOGI(TAG, "System message for routing request to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue.", MAC2STR(zh_network_queue.data.original_target_mac));
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_SEARCH_REQUEST;
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, 6);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                }
            }
            esp_now_del_peer((uint8_t *)peer->peer_addr);
            heap_caps_free(on_send);
            heap_caps_free(peer);
        ZH_NETWORK_ON_SEND_EXIT:
            break;
        case ZH_NETWORK_ON_RECV:;
            ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processing begin.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
            switch (zh_network_data->message_type)
            {
            case ZH_NETWORK_BROADCAST:;
                ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                zh_network_event_on_recv_t *on_recv = heap_caps_malloc(sizeof(zh_network_event_on_recv_t), MALLOC_CAP_8BIT);
                if (on_recv == NULL)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    heap_caps_free(on_recv);
                    break;
                }
                memset(on_recv, 0, sizeof(zh_network_event_on_recv_t));
                memcpy(on_recv->mac_addr, zh_network_data->original_sender_mac, 6);
                on_recv->data_len = zh_network_data->data_len;
                on_recv->data = heap_caps_malloc(zh_network_data->data_len, MALLOC_CAP_8BIT);
                if (on_recv->data == NULL)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    heap_caps_free(on_recv);
                    heap_caps_free(on_recv->data);
                    break;
                }
                memset(on_recv->data, 0, zh_network_data->data_len);
                memcpy(on_recv->data, zh_network_data->data, zh_network_data->data_len);
                ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for resend to all nodes.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_RECV_EVENT, on_recv, sizeof(zh_network_event_on_recv_t) + on_recv->data_len + sizeof(on_recv->data_len), portTICK_PERIOD_MS) != ESP_OK)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                }
                heap_caps_free(on_recv);
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                }
                break;
            case ZH_NETWORK_UNICAST:;
                ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, 6) == 0)
                {
                    zh_network_event_on_recv_t *on_recv = heap_caps_malloc(sizeof(zh_network_event_on_recv_t), MALLOC_CAP_8BIT);
                    if (on_recv == NULL)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                        heap_caps_free(on_recv);
                        break;
                    }
                    memset(on_recv, 0, sizeof(zh_network_event_on_recv_t));
                    memcpy(on_recv->mac_addr, zh_network_data->original_sender_mac, 6);
                    on_recv->data_len = zh_network_data->data_len;
                    on_recv->data = heap_caps_malloc(zh_network_data->data_len, MALLOC_CAP_8BIT);
                    if (on_recv->data == NULL)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                        heap_caps_free(on_recv);
                        heap_caps_free(on_recv->data);
                        break;
                    }
                    memset(on_recv->data, 0, zh_network_data->data_len);
                    memcpy(on_recv->data, zh_network_data->data, zh_network_data->data_len);
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_RECV_EVENT, on_recv, sizeof(zh_network_event_on_recv_t) + on_recv->data_len + sizeof(on_recv->data_len), portTICK_PERIOD_MS) != ESP_OK)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    heap_caps_free(on_recv);
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_DELIVERY_CONFIRM;
                    memcpy(zh_network_data->original_target_mac, zh_network_data->original_sender_mac, 6);
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, 6);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->confirm_id = zh_network_data->message_id;
                    zh_network_data->message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    break;
                }
                ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for forwarding.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                }
                break;
            case ZH_NETWORK_DELIVERY_CONFIRM:;
                ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, 6) == 0)
                {
                    zh_vector_push_back(&s_response_vector, &zh_network_data->confirm_id);
                    if (zh_vector_get_size(&s_response_vector) > s_zh_network_init_config.queue_size)
                    {
                        zh_vector_delete_item(&s_response_vector, 0);
                    }
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    break;
                }
                ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X fto MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for forwarding.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                }
                break;
            case ZH_NETWORK_SEARCH_REQUEST:;
                ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                {
                    zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                    if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, 6) == 0)
                    {
                        zh_vector_delete_item(&s_route_vector, i);
                    }
                }
                { // Just to avoid the compiler warning.
                    zh_network_routing_table_t routing_table = {0};
                    memcpy(&routing_table.original_target_mac, zh_network_data->original_sender_mac, 6);
                    memcpy(&routing_table.intermediate_target_mac, zh_network_data->sender_mac, 6);
                    zh_vector_push_back(&s_route_vector, &routing_table);
                }
                if (zh_vector_get_size(&s_route_vector) > s_zh_network_init_config.route_vector_size)
                {
                    zh_vector_delete_item(&s_route_vector, 0);
                }
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, 6) == 0)
                {
                    ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to the queue.", MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(zh_network_queue.data.original_sender_mac));
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_SEARCH_RESPONSE;
                    memcpy(zh_network_data->original_target_mac, zh_network_data->original_sender_mac, 6);
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, 6);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    break;
                }
                ESP_LOGI(TAG, "System message for routing request to MAC %02X:%02X:%02X:%02X:%02X:%02X from MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for resend to all nodes.", MAC2STR(zh_network_queue.data.original_target_mac), MAC2STR(zh_network_queue.data.original_sender_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                }
                break;
            case ZH_NETWORK_SEARCH_RESPONSE:;
                ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                {
                    zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                    if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, 6) == 0)
                    {
                        zh_vector_delete_item(&s_route_vector, i);
                    }
                }
                { // Just to avoid the compiler warning.
                    zh_network_routing_table_t routing_table = {0};
                    memcpy(&routing_table.original_target_mac, zh_network_data->original_sender_mac, 6);
                    memcpy(&routing_table.intermediate_target_mac, zh_network_data->sender_mac, 6);
                    zh_vector_push_back(&s_route_vector, &routing_table);
                }
                if (zh_vector_get_size(&s_route_vector) > s_zh_network_init_config.route_vector_size)
                {
                    zh_vector_delete_item(&s_route_vector, 0);
                }
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, 6) != 0)
                {
                    ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for resend to all nodes.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    if (xQueueSendToFront(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    break;
                }
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                break;
            default:
                break;
            }
            break;
        case ZH_NETWORK_WAIT_RESPONSE:;
            for (uint16_t i = 0; i < zh_vector_get_size(&s_response_vector); ++i)
            {
                uint32_t *message_id = zh_vector_get_item(&s_response_vector, i);
                if (memcmp(&zh_network_data->message_id, message_id, sizeof(zh_network_data->message_id)) == 0)
                {
                    zh_vector_delete_item(&s_response_vector, i);
                    zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
                    if (on_send == NULL)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                        heap_caps_free(on_send);
                        break;
                    }
                    memset(on_send, 0, sizeof(zh_network_event_on_send_t));
                    memcpy(on_send->mac_addr, zh_network_data->original_target_mac, 6);
                    on_send->status = ZH_NETWORK_SEND_SUCCESS;
                    ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from confirmation message waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    heap_caps_free(on_send);
                    flag = true;
                    break;
                }
            }
            if (flag == false)
            {
                if ((esp_timer_get_time() / 1000 - zh_network_queue.time) > s_zh_network_init_config.max_waiting_time)
                {
                    ESP_LOGW(TAG, "Time for waiting confirmation message from MAC %02X:%02X:%02X:%02X:%02X:%02X is expired.", MAC2STR(zh_network_queue.data.original_target_mac));
                    if (memcmp(zh_network_data->original_sender_mac, &s_self_mac, 6) == 0)
                    {
                        zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
                        if (on_send == NULL)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                            heap_caps_free(on_send);
                            break;
                        }
                        memset(on_send, 0, sizeof(zh_network_event_on_send_t));
                        memcpy(on_send->mac_addr, zh_network_data->original_target_mac, 6);
                        on_send->status = ZH_NETWORK_SEND_FAIL;
                        ESP_LOGE(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent fail.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from confirmation message waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                        }
                        heap_caps_free(on_send);
                    }
                    break;
                }
                if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                }
            }
            break;
        case ZH_NETWORK_WAIT_ROUTE:;
            for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
            {
                zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, 6) == 0)
                {
                    ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(zh_network_queue.data.original_target_mac));
                    if (zh_network_data->message_type == ZH_NETWORK_UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list and added to queue.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    if (zh_network_data->message_type == ZH_NETWORK_DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list and added to queue.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                    }
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                    }
                    flag = true;
                    break;
                }
            }
            if (flag == false)
            {
                if ((esp_timer_get_time() / 1000 - zh_network_queue.time) > s_zh_network_init_config.max_waiting_time)
                {
                    ESP_LOGW(TAG, "Time for waiting routing to MAC %02X:%02X:%02X:%02X:%02X:%02X is expired.", MAC2STR(zh_network_queue.data.original_target_mac));
                    if (memcmp(zh_network_data->original_sender_mac, &s_self_mac, 6) == 0)
                    {
                        zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
                        if (on_send == NULL)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                            heap_caps_free(on_send);
                            break;
                        }
                        memset(on_send, 0, sizeof(zh_network_event_on_send_t));
                        memcpy(on_send->mac_addr, zh_network_data->original_target_mac, 6);
                        on_send->status = ZH_NETWORK_SEND_FAIL;
                        ESP_LOGE(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent fail.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                        }
                        heap_caps_free(on_send);
                    }
                    else
                    {
                        if (zh_network_data->message_type == ZH_NETWORK_UNICAST)
                        {
                            ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        }
                        if (zh_network_data->message_type == ZH_NETWORK_DELIVERY_CONFIRM)
                        {
                            ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list.", MAC2STR(zh_network_queue.data.original_sender_mac), MAC2STR(zh_network_queue.data.original_target_mac));
                        }
                    }
                    break;
                }
                if (xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error.");
                }
            }
            break;
        default:
            break;
        }
    }
    vTaskDelete(NULL);
}