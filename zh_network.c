/**
 * @file
 * The main code of the zh_network component.
 */

#include "zh_network.h"

/// \cond
#define DATA_SEND_SUCCESS BIT0
#define DATA_SEND_FAIL BIT1
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
/// \endcond

static void _send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
#if defined CONFIG_IDF_TARGET_ESP8266 || ESP_IDF_VERSION_MAJOR == 4
static void _recv_cb(const uint8_t *mac_addr, const uint8_t *data, int data_len);
#else
static void _recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len);
#endif
static void _processing(void *pvParameter);

static const char *TAG = "zh_network";

static EventGroupHandle_t _event_group_handle = {0};
static QueueHandle_t _queue_handle = {0};
static TaskHandle_t _processing_task_handle = {0};
static SemaphoreHandle_t _id_vector_mutex = {0};
static zh_network_init_config_t _init_config = {0};
static zh_vector_t _id_vector = {0};
static zh_vector_t _route_vector = {0};
static zh_vector_t _response_vector = {0};
static uint8_t _self_mac[6] = {0};
static const uint8_t _broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool _is_initialized = false;
static uint8_t _attempts = 0;

/// \cond
typedef struct
{
    uint8_t original_target_mac[6];
    uint8_t intermediate_target_mac[6];
} _routing_table_t;

typedef struct
{
    uint64_t time;
    enum
    {
        TO_SEND,
        ON_RECV,
        WAIT_ROUTE,
        WAIT_RESPONSE,
    } id;
    struct
    {
        enum
        {
            BROADCAST,
            UNICAST,
            DELIVERY_CONFIRM,
            SEARCH_REQUEST,
            SEARCH_RESPONSE
        } __attribute__((packed)) message_type;
        uint32_t network_id;
        uint32_t message_id;
        uint32_t confirm_id;
        uint8_t original_target_mac[6];
        uint8_t original_sender_mac[6];
        uint8_t sender_mac[6];
        uint8_t payload[ZH_NETWORK_MAX_MESSAGE_SIZE];
        uint8_t payload_len;
    } __attribute__((packed)) data;
} _queue_t;

ESP_EVENT_DEFINE_BASE(ZH_NETWORK);
/// \endcond

esp_err_t zh_network_init(const zh_network_init_config_t *config)
{
    ESP_LOGI(TAG, "ESP-NOW initialization begin.");
    if (config == NULL)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. Invalid argument.");
        return ESP_ERR_INVALID_ARG;
    }
    _init_config = *config;
    if (_init_config.wifi_channel < 1 || _init_config.wifi_channel > 14)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. WiFi channel.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_wifi_set_channel(_init_config.wifi_channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. WiFi not initialized.");
        return ESP_ERR_WIFI_NOT_INIT;
    }
    else if (err == ESP_FAIL)
    {
        uint8_t prim = 0;
        wifi_second_chan_t sec = 0;
        esp_wifi_get_channel(&prim, &sec);
        if (prim != _init_config.wifi_channel)
        {
            ESP_LOGW(TAG, "ESP-NOW initialization warning. The device is connected to the router. Channel %d will be used for ESP-NOW.", prim);
        }
    }
    if ((sizeof(_queue_t) - 14) > ESP_NOW_MAX_DATA_LEN)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. The maximum value of the transmitted data size is incorrect.");
        return ESP_ERR_INVALID_ARG;
    }
    if (_init_config.wifi_interface == WIFI_IF_STA)
    {
        esp_read_mac(_self_mac, ESP_MAC_WIFI_STA);
    }
    else
    {
        esp_read_mac(_self_mac, ESP_MAC_WIFI_SOFTAP);
    }
    _event_group_handle = xEventGroupCreate();
    _queue_handle = xQueueCreate(_init_config.queue_size, sizeof(_queue_t));
    zh_vector_init(&_id_vector, sizeof(uint32_t), false);
    zh_vector_init(&_route_vector, sizeof(_routing_table_t), false);
    zh_vector_init(&_response_vector, sizeof(uint32_t), false);
    _id_vector_mutex = xSemaphoreCreateMutex();
    if (esp_now_init() != ESP_OK || esp_now_register_send_cb(_send_cb) != ESP_OK || esp_now_register_recv_cb(_recv_cb) != ESP_OK)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. Internal error.");
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(&_processing, "NULL", _init_config.stack_size, NULL, _init_config.task_priority, &_processing_task_handle, tskNO_AFFINITY) != pdPASS)
    {
        ESP_LOGE(TAG, "ESP-NOW initialization fail. Internal error.");
        return ESP_FAIL;
    }
    _is_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW initialization success.");
    return ESP_OK;
}

esp_err_t zh_network_deinit(void)
{
    ESP_LOGI(TAG, "ESP-NOW deinitialization begin.");
    if (_is_initialized == false)
    {
        ESP_LOGE(TAG, "ESP-NOW deinitialization fail. ESP-NOW not initialized.");
        return ESP_FAIL;
    }
    vEventGroupDelete(_event_group_handle);
    vQueueDelete(_queue_handle);
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    zh_vector_free(&_id_vector);
    zh_vector_free(&_route_vector);
    zh_vector_free(&_response_vector);
    vTaskDelete(_processing_task_handle);
    _is_initialized = false;
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
    if (_is_initialized == false)
    {
        ESP_LOGE(TAG, "Adding outgoing ESP-NOW data to queue fail. ESP-NOW not initialized.");
        return ESP_FAIL;
    }
    if (data_len == 0 || data == NULL || data_len > ZH_NETWORK_MAX_MESSAGE_SIZE)
    {
        ESP_LOGE(TAG, "Adding outgoing ESP-NOW data to queue fail. Invalid argument.");
        return ESP_ERR_INVALID_ARG;
    }
    if (uxQueueSpacesAvailable(_queue_handle) < _init_config.queue_size / 2)
    {
        ESP_LOGW(TAG, "Adding outgoing ESP-NOW data to queue fail. Queue is almost full.");
        return ESP_ERR_INVALID_STATE;
    }
    _queue_t queue = {0};
    queue.id = TO_SEND;
    queue.data.network_id = _init_config.network_id;
    queue.data.message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
    memcpy(queue.data.original_sender_mac, _self_mac, 6);
    if (target == NULL)
    {
        queue.data.message_type = BROADCAST;
        memcpy(queue.data.original_target_mac, _broadcast_mac, 6);
    }
    else
    {
        if (memcmp(target, _broadcast_mac, 6) != 0)
        {
            queue.data.message_type = UNICAST;
            memcpy(queue.data.original_target_mac, target, 6);
        }
        else
        {
            queue.data.message_type = BROADCAST;
            memcpy(queue.data.original_target_mac, _broadcast_mac, 6);
        }
    }
    memcpy(queue.data.payload, data, data_len);
    queue.data.payload_len = data_len;
    if (target == NULL)
    {
        ESP_LOGI(TAG, "Adding outgoing ESP-NOW data to MAC FF:FF:FF:FF:FF:FF to queue success.");
    }
    else
    {
        ESP_LOGI(TAG, "Adding outgoing ESP-NOW data to MAC %02X:%02X:%02X:%02X:%02X:%02X to queue success.", MAC2STR(target));
    }
    if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
    {
        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void _send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        xEventGroupSetBits(_event_group_handle, DATA_SEND_SUCCESS);
    }
    else
    {
        xEventGroupSetBits(_event_group_handle, DATA_SEND_FAIL);
    }
}

#if defined CONFIG_IDF_TARGET_ESP8266 || ESP_IDF_VERSION_MAJOR == 4
static void _recv_cb(const uint8_t *mac_addr, const uint8_t *data, int data_len)
#else
static void _recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len)
#endif
{
#if defined CONFIG_IDF_TARGET_ESP8266 || ESP_IDF_VERSION_MAJOR == 4
    ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue begin.", MAC2STR(mac_addr));
#else
    ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue begin.", MAC2STR(esp_now_info->src_addr));
#endif
    if (uxQueueSpacesAvailable(_queue_handle) < (_init_config.queue_size - 2))
    {
        ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Queue is almost full.");
        return;
    }
    if (data_len == sizeof(_queue_t) - 14)
    {
        _queue_t queue = {0};
        queue.id = ON_RECV;
        memcpy(&queue.data, data, data_len);
        if (memcmp(&queue.data.network_id, &_init_config.network_id, sizeof(queue.data.network_id)) != 0)
        {
            ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Incorrect mesh network ID.");
            return;
        }
        for (uint16_t i = 0; i < zh_vector_get_size(&_id_vector); ++i)
        {
            uint32_t *message_id = zh_vector_get_item(&_id_vector, i);
            if (memcmp(&queue.data.message_id, message_id, sizeof(queue.data.message_id)) == 0)
            {
                ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Repeat message received.");
                return;
            }
        }
        if (xSemaphoreTake(_id_vector_mutex, portTICK_PERIOD_MS) == pdTRUE)
        {
            zh_vector_push_back(&_id_vector, &queue.data.message_id);
            if (zh_vector_get_size(&_id_vector) > _init_config.id_vector_size)
            {
                zh_vector_delete_item(&_id_vector, 0);
            }
            xSemaphoreGive(_id_vector_mutex);
        }
#if defined CONFIG_IDF_TARGET_ESP8266 || ESP_IDF_VERSION_MAJOR == 4
        memcpy(queue.data.sender_mac, mac_addr, 6);
#else
        memcpy(queue.data.sender_mac, esp_now_info->src_addr, 6);
#endif
#if defined CONFIG_IDF_TARGET_ESP8266 || ESP_IDF_VERSION_MAJOR == 4
        ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue success.", MAC2STR(mac_addr));
#else
        ESP_LOGI(TAG, "Adding incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to queue success.", MAC2STR(esp_now_info->src_addr));
#endif
        if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
        {
            ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
        }
    }
    else
    {
        ESP_LOGW(TAG, "Adding incoming ESP-NOW data to queue fail. Incorrect ESP-NOW data size.");
    }
}

static void _processing(void *pvParameter)
{
    _queue_t queue = {0};
    while (xQueueReceive(_queue_handle, &queue, portMAX_DELAY) == pdTRUE)
    {
        bool flag = false;
        switch (queue.id)
        {
        case TO_SEND:
            ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processing begin.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
            esp_now_peer_info_t *peer = heap_caps_malloc(sizeof(esp_now_peer_info_t), MALLOC_CAP_8BIT);
            if (peer == NULL)
            {
                ESP_LOGE(TAG, "Outgoing ESP-NOW data processing fail. Memory allocation fail or no free memory in the heap.");
                heap_caps_free(peer);
                break;
            }
            memset(peer, 0, sizeof(esp_now_peer_info_t));
            peer->ifidx = _init_config.wifi_interface;
            if (queue.data.message_type == BROADCAST || queue.data.message_type == SEARCH_REQUEST || queue.data.message_type == SEARCH_RESPONSE)
            {
                memcpy(peer->peer_addr, _broadcast_mac, 6);
                if (memcmp(queue.data.original_sender_mac, _self_mac, 6) == 0)
                {
                    if (xSemaphoreTake(_id_vector_mutex, portTICK_PERIOD_MS) == pdTRUE)
                    {
                        zh_vector_push_back(&_id_vector, &queue.data.message_id);
                        if (zh_vector_get_size(&_id_vector) > _init_config.id_vector_size)
                        {
                            zh_vector_delete_item(&_id_vector, 0);
                        }
                        xSemaphoreGive(_id_vector_mutex);
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Checking routing table to MAC %02X:%02X:%02X:%02X:%02X:%02X.", MAC2STR(queue.data.original_target_mac));
                for (uint16_t i = 0; i < zh_vector_get_size(&_route_vector); ++i)
                {
                    _routing_table_t *routing_table = zh_vector_get_item(&_route_vector, i);
                    if (memcmp(queue.data.original_target_mac, routing_table->original_target_mac, 6) == 0)
                    {
                        memcpy(peer->peer_addr, routing_table->intermediate_target_mac, 6);
                        flag = true;
                        ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X is found. Forwarding via MAC %02X:%02X:%02X:%02X:%02X:%02X.", MAC2STR(queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        break;
                    }
                }
                if (flag == false)
                {
                    ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X not found.", MAC2STR(queue.data.original_target_mac));
                    if (queue.data.message_type == UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    else
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    queue.id = WAIT_ROUTE;
                    queue.time = esp_timer_get_time() / 1000;
                    if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    queue.id = TO_SEND;
                    queue.data.message_type = SEARCH_REQUEST;
                    memcpy(queue.data.original_sender_mac, _self_mac, 6);
                    queue.data.payload_len = 0;
                    memset(queue.data.payload, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    queue.data.message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    heap_caps_free(peer);
                    break;
                }
            }
            if (esp_now_add_peer(peer) != ESP_OK)
            {
                ESP_LOGE(TAG, "Outgoing ESP-NOW data processing fail. Internal error with adding peer.");
                heap_caps_free(peer);
                break;
            }
            zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
            if (on_send == NULL)
            {
                ESP_LOGE(TAG, "Outgoing ESP-NOW data processing fail. Memory allocation fail or no free memory in the heap.");
                heap_caps_free(peer);
                heap_caps_free(on_send);
                break;
            }
            memset(on_send, 0, sizeof(zh_network_event_on_send_t));
            memcpy(on_send->mac_addr, queue.data.original_target_mac, 6);
        SEND:
            ++_attempts;
            if (esp_now_send((uint8_t *)peer->peer_addr, (uint8_t *)&queue.data, sizeof(_queue_t) - 14) != ESP_OK)
            {
                ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                heap_caps_free(peer);
                heap_caps_free(on_send);
                break;
            }
            EventBits_t bit = xEventGroupWaitBits(_event_group_handle, DATA_SEND_SUCCESS | DATA_SEND_FAIL, pdTRUE, pdFALSE, 50 / portTICK_PERIOD_MS);
            if ((bit & DATA_SEND_SUCCESS) != 0)
            {
                _attempts = 0;
                if (memcmp(queue.data.original_sender_mac, _self_mac, 6) == 0)
                {
                    if (queue.data.message_type == BROADCAST)
                    {
                        ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        on_send->status = ZH_NETWORK_SEND_SUCCESS;
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                        }
                    }
                    if (queue.data.message_type == SEARCH_REQUEST)
                    {
                        ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == SEARCH_RESPONSE)
                    {
                        ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to confirmation message waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        queue.id = WAIT_RESPONSE;
                        queue.time = esp_timer_get_time() / 1000;
                        if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                        }
                    }
                }
                else
                {
                    if (queue.data.message_type == BROADCAST)
                    {
                        ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == SEARCH_REQUEST)
                    {
                        ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == SEARCH_RESPONSE)
                    {
                        ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                        ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                }
            }
            else
            {
                if (_attempts < _init_config.attempts)
                {
                    goto SEND;
                }
                _attempts = 0;
                if (memcmp(queue.data.original_target_mac, _broadcast_mac, 6) != 0)
                {
                    ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X via MAC %02X:%02X:%02X:%02X:%02X:%02X is incorrect.", MAC2STR(queue.data.original_target_mac), MAC2STR(peer->peer_addr));
                    for (uint16_t i = 0; i < zh_vector_get_size(&_route_vector); ++i)
                    {
                        _routing_table_t *routing_table = zh_vector_get_item(&_route_vector, i);
                        if (memcmp(queue.data.original_target_mac, routing_table->original_target_mac, 6) == 0)
                        {
                            zh_vector_delete_item(&_route_vector, i);
                        }
                    }
                    if (queue.data.message_type == UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X transferred to routing waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    queue.id = WAIT_ROUTE;
                    queue.time = esp_timer_get_time() / 1000;
                    if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    ESP_LOGI(TAG, "System message for routing request to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue.", MAC2STR(queue.data.original_target_mac));
                    queue.id = TO_SEND;
                    queue.data.message_type = SEARCH_REQUEST;
                    memcpy(queue.data.original_sender_mac, _self_mac, 6);
                    queue.data.payload_len = 0;
                    memset(queue.data.payload, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    queue.data.message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    ESP_LOGI(TAG, "Outgoing ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                }
            }
            esp_now_del_peer(peer->peer_addr);
            heap_caps_free(on_send);
            heap_caps_free(peer);
            break;
        case ON_RECV:
            ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processing begin.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
            switch (queue.data.message_type)
            {
            case BROADCAST:
                ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                zh_network_event_on_recv_t *on_recv = heap_caps_malloc(sizeof(zh_network_event_on_recv_t), MALLOC_CAP_8BIT);
                if (on_recv == NULL)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    heap_caps_free(on_recv);
                    break;
                }
                memset(on_recv, 0, sizeof(zh_network_event_on_recv_t));
                memcpy(on_recv->mac_addr, queue.data.original_sender_mac, 6);
                on_recv->data_len = queue.data.payload_len;
                on_recv->data = heap_caps_malloc(queue.data.payload_len, MALLOC_CAP_8BIT);
                if (on_recv->data == NULL)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    heap_caps_free(on_recv);
                    heap_caps_free(on_recv->data);
                    break;
                }
                memset(on_recv->data, 0, queue.data.payload_len);
                memcpy(on_recv->data, queue.data.payload, queue.data.payload_len);
                ESP_LOGI(TAG, "Broadcast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for resend to all nodes.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_RECV_EVENT, on_recv, sizeof(zh_network_event_on_recv_t) + on_recv->data_len + sizeof(on_recv->data_len), portTICK_PERIOD_MS) != ESP_OK)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                }
                heap_caps_free(on_recv);
                queue.id = TO_SEND;
                if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                }
                break;
            case UNICAST:
                ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                if (memcmp(queue.data.original_target_mac, _self_mac, 6) == 0)
                {
                    zh_network_event_on_recv_t *on_recv = heap_caps_malloc(sizeof(zh_network_event_on_recv_t), MALLOC_CAP_8BIT);
                    if (on_recv == NULL)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                        heap_caps_free(on_recv);
                        break;
                    }
                    memset(on_recv, 0, sizeof(zh_network_event_on_recv_t));
                    memcpy(on_recv->mac_addr, queue.data.original_sender_mac, 6);
                    on_recv->data_len = queue.data.payload_len;
                    on_recv->data = heap_caps_malloc(queue.data.payload_len, MALLOC_CAP_8BIT);
                    if (on_recv->data == NULL)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                        heap_caps_free(on_recv);
                        heap_caps_free(on_recv->data);
                        break;
                    }
                    memset(on_recv->data, 0, queue.data.payload_len);
                    memcpy(on_recv->data, queue.data.payload, queue.data.payload_len);
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_RECV_EVENT, on_recv, sizeof(zh_network_event_on_recv_t) + on_recv->data_len + sizeof(on_recv->data_len), portTICK_PERIOD_MS) != ESP_OK)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    heap_caps_free(on_recv);
                    queue.id = TO_SEND;
                    queue.data.message_type = DELIVERY_CONFIRM;
                    memcpy(queue.data.original_target_mac, queue.data.original_sender_mac, 6);
                    memcpy(queue.data.original_sender_mac, _self_mac, 6);
                    queue.data.payload_len = 0;
                    memset(queue.data.payload, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    queue.data.confirm_id = queue.data.message_id;
                    queue.data.message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    break;
                }
                ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for forwarding.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                queue.id = TO_SEND;
                if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                }
                break;
            case DELIVERY_CONFIRM:
                ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                if (memcmp(queue.data.original_target_mac, _self_mac, 6) == 0)
                {
                    zh_vector_push_back(&_response_vector, &queue.data.confirm_id);
                    if (zh_vector_get_size(&_response_vector) > _init_config.queue_size)
                    {
                        zh_vector_delete_item(&_response_vector, 0);
                    }
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    break;
                }
                ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X fto MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for forwarding.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                queue.id = TO_SEND;
                if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                }
                break;
            case SEARCH_REQUEST:
                ESP_LOGI(TAG, "System message for routing request from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                for (uint16_t i = 0; i < zh_vector_get_size(&_route_vector); ++i)
                {
                    _routing_table_t *routing_table = zh_vector_get_item(&_route_vector, i);
                    if (memcmp(queue.data.original_target_mac, routing_table->original_target_mac, 6) == 0)
                    {
                        zh_vector_delete_item(&_route_vector, i);
                    }
                }
                { // Just to avoid the compiler warning.
                    _routing_table_t routing_table = {0};
                    memcpy(routing_table.original_target_mac, queue.data.original_sender_mac, 6);
                    memcpy(routing_table.intermediate_target_mac, queue.data.sender_mac, 6);
                    zh_vector_push_back(&_route_vector, &routing_table);
                }
                if (zh_vector_get_size(&_route_vector) > _init_config.route_vector_size)
                {
                    zh_vector_delete_item(&_route_vector, 0);
                }
                if (memcmp(queue.data.original_target_mac, _self_mac, 6) == 0)
                {
                    ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to the queue.", MAC2STR(queue.data.original_target_mac), MAC2STR(queue.data.original_sender_mac));
                    queue.id = TO_SEND;
                    queue.data.message_type = SEARCH_RESPONSE;
                    memcpy(queue.data.original_target_mac, queue.data.original_sender_mac, 6);
                    memcpy(queue.data.original_sender_mac, _self_mac, 6);
                    queue.data.payload_len = 0;
                    memset(queue.data.payload, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    queue.data.message_id = abs(esp_random()); // It is not clear why esp_random() sometimes gives negative values.
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    break;
                }
                ESP_LOGI(TAG, "System message for routing request to MAC %02X:%02X:%02X:%02X:%02X:%02X from MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for resend to all nodes.", MAC2STR(queue.data.original_target_mac), MAC2STR(queue.data.original_sender_mac));
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                queue.id = TO_SEND;
                if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                }
                break;
            case SEARCH_RESPONSE:
                ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                for (uint16_t i = 0; i < zh_vector_get_size(&_route_vector); ++i)
                {
                    _routing_table_t *routing_table = zh_vector_get_item(&_route_vector, i);
                    if (memcmp(queue.data.original_target_mac, routing_table->original_target_mac, 6) == 0)
                    {
                        zh_vector_delete_item(&_route_vector, i);
                    }
                }
                { // Just to avoid the compiler warning.
                    _routing_table_t routing_table = {0};
                    memcpy(routing_table.original_target_mac, queue.data.original_sender_mac, 6);
                    memcpy(routing_table.intermediate_target_mac, queue.data.sender_mac, 6);
                    zh_vector_push_back(&_route_vector, &routing_table);
                }
                if (zh_vector_get_size(&_route_vector) > _init_config.route_vector_size)
                {
                    zh_vector_delete_item(&_route_vector, 0);
                }
                if (memcmp(queue.data.original_target_mac, _self_mac, 6) != 0)
                {
                    ESP_LOGI(TAG, "System message for routing response from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X added to queue for resend to all nodes.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    queue.id = TO_SEND;
                    if (xQueueSendToFront(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    break;
                }
                ESP_LOGI(TAG, "Incoming ESP-NOW data from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X processed success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                break;
            default:
                break;
            }
            break;
        case WAIT_RESPONSE:
            for (uint16_t i = 0; i < zh_vector_get_size(&_response_vector); ++i)
            {
                uint32_t *message_id = zh_vector_get_item(&_response_vector, i);
                if (memcmp(&queue.data.message_id, message_id, sizeof(queue.data.message_id)) == 0)
                {
                    zh_vector_delete_item(&_response_vector, i);
                    zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
                    if (on_send == NULL)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                        heap_caps_free(on_send);
                        break;
                    }
                    memset(on_send, 0, sizeof(zh_network_event_on_send_t));
                    memcpy(on_send->mac_addr, queue.data.original_target_mac, 6);
                    on_send->status = ZH_NETWORK_SEND_SUCCESS;
                    ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from confirmation message waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    heap_caps_free(on_send);
                    flag = true;
                    break;
                }
            }
            if (flag == false)
            {
                if ((esp_timer_get_time() / 1000 - queue.time) > _init_config.max_waiting_time)
                {
                    ESP_LOGW(TAG, "Time for waiting confirmation message from MAC %02X:%02X:%02X:%02X:%02X:%02X is expired.", MAC2STR(queue.data.original_target_mac));
                    if (memcmp(queue.data.original_sender_mac, _self_mac, 6) == 0)
                    {
                        zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
                        if (on_send == NULL)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                            heap_caps_free(on_send);
                            break;
                        }
                        memset(on_send, 0, sizeof(zh_network_event_on_send_t));
                        memcpy(on_send->mac_addr, queue.data.original_target_mac, 6);
                        on_send->status = ZH_NETWORK_SEND_FAIL;
                        ESP_LOGE(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent fail.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from confirmation message waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                        }
                        heap_caps_free(on_send);
                    }
                    break;
                }
                if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                }
            }
            break;
        case WAIT_ROUTE:
            for (uint16_t i = 0; i < zh_vector_get_size(&_route_vector); ++i)
            {
                _routing_table_t *routing_table = zh_vector_get_item(&_route_vector, i);
                if (memcmp(queue.data.original_target_mac, routing_table->original_target_mac, 6) == 0)
                {
                    ESP_LOGI(TAG, "Routing to MAC %02X:%02X:%02X:%02X:%02X:%02X is received.", MAC2STR(queue.data.original_target_mac));
                    if (queue.data.message_type == UNICAST)
                    {
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list and added to queue.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    if (queue.data.message_type == DELIVERY_CONFIRM)
                    {
                        ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list and added to queue.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                    }
                    queue.id = TO_SEND;
                    if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                    {
                        ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                    }
                    flag = true;
                    break;
                }
            }
            if (flag == false)
            {
                if ((esp_timer_get_time() / 1000 - queue.time) > _init_config.max_waiting_time)
                {
                    ESP_LOGW(TAG, "Time for waiting routing to MAC %02X:%02X:%02X:%02X:%02X:%02X is expired.", MAC2STR(queue.data.original_target_mac));
                    if (memcmp(queue.data.original_sender_mac, _self_mac, 6) == 0)
                    {
                        zh_network_event_on_send_t *on_send = heap_caps_malloc(sizeof(zh_network_event_on_send_t), MALLOC_CAP_8BIT);
                        if (on_send == NULL)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                            heap_caps_free(on_send);
                            break;
                        }
                        memset(on_send, 0, sizeof(zh_network_event_on_send_t));
                        memcpy(on_send->mac_addr, queue.data.original_target_mac, 6);
                        on_send->status = ZH_NETWORK_SEND_FAIL;
                        ESP_LOGE(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X sent fail.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        if (esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portTICK_PERIOD_MS) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                        }
                        heap_caps_free(on_send);
                    }
                    else
                    {
                        if (queue.data.message_type == UNICAST)
                        {
                            ESP_LOGI(TAG, "Unicast message from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        }
                        if (queue.data.message_type == DELIVERY_CONFIRM)
                        {
                            ESP_LOGI(TAG, "System message for message receiving confirmation from MAC %02X:%02X:%02X:%02X:%02X:%02X to MAC %02X:%02X:%02X:%02X:%02X:%02X removed from routing waiting list.", MAC2STR(queue.data.original_sender_mac), MAC2STR(queue.data.original_target_mac));
                        }
                    }
                    break;
                }
                if (xQueueSend(_queue_handle, &queue, portTICK_PERIOD_MS) != pdTRUE)
                {
                    ESP_LOGE(TAG, "ESP-NOW message processing task internal error at line %d.", __LINE__);
                }
            }
            break;
        default:
            break;
        }
    }
    vTaskDelete(NULL);
}