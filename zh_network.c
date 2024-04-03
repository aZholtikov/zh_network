#include "zh_network.h"

#define ZH_NETWORK_DATA_SEND_SUCCESS BIT0
#define ZH_NETWORK_DATA_SEND_FAIL BIT1

static void s_zh_network_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
#ifdef CONFIG_IDF_TARGET_ESP8266
static void s_zh_network_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int data_len);
#else
static void s_zh_network_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len);
static portMUX_TYPE s_zh_network_spinlock = portMUX_INITIALIZER_UNLOCKED;
#endif
static void s_zh_network_processing(void *pvParameter);

static EventGroupHandle_t s_zh_network_send_cb_status_event_group_handle = {0};
static QueueHandle_t s_zh_network_queue_handle = {0};
static TaskHandle_t s_zh_network_processing_task_handle = {0};
static zh_network_init_config_t s_zh_network_init_config = {0};
static zh_vector_t s_id_vector = {0};
static zh_vector_t s_route_vector = {0};
static zh_vector_t s_response_vector = {0};
static uint8_t s_self_mac[ESP_NOW_ETH_ALEN] = {0};
static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef enum zh_network_message_type_t
{
    ZH_NETWORK_BROADCAST,
    ZH_NETWORK_UNICAST,
    ZH_NETWORK_DELIVERY_CONFIRM,
    ZH_NETWORK_SEARCH_REQUEST,
    ZH_NETWORK_SEARCH_RESPONSE
} __attribute__((packed)) zh_network_message_type_t;

typedef struct zh_network_data_t
{
    zh_network_message_type_t message_type;
    uint32_t network_id;
    uint32_t message_id;
    uint32_t confirm_id;
    uint8_t original_target_mac[ESP_NOW_ETH_ALEN];
    uint8_t original_sender_mac[ESP_NOW_ETH_ALEN];
    uint8_t sender_mac[ESP_NOW_ETH_ALEN];
    uint8_t data[ZH_NETWORK_MAX_MESSAGE_SIZE];
    uint8_t data_len;
} __attribute__((packed)) zh_network_data_t;

typedef struct zh_network_routing_table_t
{
    uint8_t original_target_mac[ESP_NOW_ETH_ALEN];
    uint8_t intermediate_target_mac[ESP_NOW_ETH_ALEN];
} __attribute__((packed)) zh_network_routing_table_t;

typedef enum zh_network_queue_id_t
{
    ZH_NETWORK_ON_SEND,
    ZH_NETWORK_ON_RECV,
    ZH_NETWORK_WAIT_ROUTE,
    ZH_NETWORK_WAIT_RESPONSE,
} __attribute__((packed)) zh_network_queue_id_t;

typedef struct zh_network_queue_t
{
    zh_network_queue_id_t id;
    uint64_t time;
    zh_network_data_t data;
} __attribute__((packed)) zh_network_queue_t;

ESP_EVENT_DEFINE_BASE(ZH_NETWORK);

esp_err_t zh_network_init(zh_network_init_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) == ESP_ERR_WIFI_NOT_INIT)
    {
        return ESP_ERR_WIFI_NOT_INIT;
    }
    if (sizeof(zh_network_data_t) > ESP_NOW_MAX_DATA_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
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
    esp_now_init();
    esp_now_register_send_cb(s_zh_network_send_cb);
    esp_now_register_recv_cb(s_zh_network_recv_cb);
    xTaskCreatePinnedToCore(&s_zh_network_processing, "zh_network_processing", s_zh_network_init_config.stack_size, NULL, s_zh_network_init_config.task_priority, &s_zh_network_processing_task_handle, tskNO_AFFINITY);
    return ESP_OK;
}

void zh_network_deinit(void)
{
    vEventGroupDelete(s_zh_network_send_cb_status_event_group_handle);
    vQueueDelete(s_zh_network_queue_handle);
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    zh_vector_free(&s_id_vector);
    zh_vector_free(&s_route_vector);
    zh_vector_free(&s_response_vector);
    vTaskDelete(s_zh_network_processing_task_handle);
}

esp_err_t zh_network_send(const uint8_t *target, const uint8_t *data, const uint8_t data_len)
{
    if (uxQueueSpacesAvailable(s_zh_network_queue_handle) < s_zh_network_init_config.queue_size / 2)
    {
        return ESP_FAIL;
    }
    if (data_len > ZH_NETWORK_MAX_MESSAGE_SIZE || data_len == 0 || data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    zh_network_queue_t zh_network_queue = {0};
    zh_network_queue.id = ZH_NETWORK_ON_SEND;
    zh_network_data_t *send_data = &zh_network_queue.data;
    send_data->network_id = s_zh_network_init_config.network_id;
    send_data->message_id = abs(esp_random()); // I don't know why, but esp_random() sometimes produces negative values.
    memcpy(send_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN);
    if (target == NULL)
    {
        send_data->message_type = ZH_NETWORK_BROADCAST;
        memcpy(send_data->original_target_mac, &s_broadcast_mac, ESP_NOW_ETH_ALEN);
    }
    else
    {
        if (memcmp(target, &s_broadcast_mac, ESP_NOW_ETH_ALEN) != 0)
        {
            send_data->message_type = ZH_NETWORK_UNICAST;
            memcpy(send_data->original_target_mac, target, ESP_NOW_ETH_ALEN);
        }
        else
        {
            send_data->message_type = ZH_NETWORK_BROADCAST;
            memcpy(send_data->original_target_mac, &s_broadcast_mac, ESP_NOW_ETH_ALEN);
        }
    }
    memcpy(&send_data->data, data, data_len);
    send_data->data_len = data_len;
    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
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
    if (uxQueueSpacesAvailable(s_zh_network_queue_handle) < s_zh_network_init_config.queue_size / 2)
    {
        return;
    }
    if (data_len == sizeof(zh_network_data_t))
    {
        zh_network_data_t *recv_data = (zh_network_data_t *)data;
        if (memcmp(&recv_data->network_id, &s_zh_network_init_config.network_id, sizeof(recv_data->network_id)) != 0)
        {
            return;
        }
        for (uint16_t i = 0; i < zh_vector_get_size(&s_id_vector); ++i)
        {
            uint32_t *message_id = zh_vector_get_item(&s_id_vector, i);
            if (memcmp(&recv_data->message_id, message_id, sizeof(recv_data->message_id)) == 0)
            {
                return;
            }
        }
#ifdef CONFIG_IDF_TARGET_ESP8266
        taskENTER_CRITICAL();
#else
        taskENTER_CRITICAL(&s_zh_network_spinlock);
#endif
        zh_vector_push_back(&s_id_vector, &recv_data->message_id);
        if (zh_vector_get_size(&s_id_vector) > s_zh_network_init_config.id_vector_size)
        {
            zh_vector_delete_item(&s_id_vector, 0);
        }
#ifdef CONFIG_IDF_TARGET_ESP8266
        taskEXIT_CRITICAL();
#else
        taskEXIT_CRITICAL(&s_zh_network_spinlock);
#endif
        zh_network_queue_t zh_network_queue = {0};
        zh_network_queue.id = ZH_NETWORK_ON_RECV;
        recv_data = &zh_network_queue.data;
        memcpy(recv_data, data, data_len);
#ifdef CONFIG_IDF_TARGET_ESP8266
        memcpy(recv_data->sender_mac, mac_addr, ESP_NOW_ETH_ALEN);
#else
        memcpy(recv_data->sender_mac, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);
#endif
        xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
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
            esp_now_peer_info_t *peer = calloc(1, sizeof(esp_now_peer_info_t));
            peer->ifidx = s_zh_network_init_config.wifi_interface;
            if (zh_network_data->message_type == ZH_NETWORK_BROADCAST || zh_network_data->message_type == ZH_NETWORK_SEARCH_REQUEST || zh_network_data->message_type == ZH_NETWORK_SEARCH_RESPONSE)
            {
                memcpy(peer->peer_addr, &s_broadcast_mac, ESP_NOW_ETH_ALEN);
#ifdef CONFIG_IDF_TARGET_ESP8266
                taskENTER_CRITICAL();
#else
                taskENTER_CRITICAL(&s_zh_network_spinlock);
#endif
                zh_vector_push_back(&s_id_vector, &zh_network_data->message_id);
                if (zh_vector_get_size(&s_id_vector) > s_zh_network_init_config.id_vector_size)
                {
                    zh_vector_delete_item(&s_id_vector, 0);
                }
#ifdef CONFIG_IDF_TARGET_ESP8266
                taskEXIT_CRITICAL();
#else
                taskEXIT_CRITICAL(&s_zh_network_spinlock);
#endif
            }
            else
            {
                for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                {
                    zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                    if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, ESP_NOW_ETH_ALEN) == 0)
                    {
                        memcpy(peer->peer_addr, routing_table->intermediate_target_mac, ESP_NOW_ETH_ALEN);
                        flag = true;
                        break;
                    }
                }
                if (flag == false)
                {
                    zh_network_queue.id = ZH_NETWORK_WAIT_ROUTE;
                    zh_network_queue.time = esp_timer_get_time() / 1000;
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_SEARCH_REQUEST;
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->message_id = abs(esp_random()); // I don't know why, but esp_random() sometimes produces negative values.
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                    goto ZH_NETWORK_ON_SEND_EXIT;
                }
            }
            esp_now_add_peer(peer);
            zh_network_event_on_send_t *on_send = calloc(1, sizeof(zh_network_event_on_send_t));
            memcpy(on_send->mac_addr, zh_network_data->original_target_mac, ESP_NOW_ETH_ALEN);
            esp_now_send((uint8_t *)peer->peer_addr, (uint8_t *)zh_network_data, sizeof(zh_network_data_t));
            EventBits_t bit = xEventGroupWaitBits(s_zh_network_send_cb_status_event_group_handle, ZH_NETWORK_DATA_SEND_SUCCESS | ZH_NETWORK_DATA_SEND_FAIL, pdTRUE, pdFALSE, 50 / portTICK_PERIOD_MS);
            if ((bit & ZH_NETWORK_DATA_SEND_SUCCESS) != 0)
            {
                if (memcmp(zh_network_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN) == 0)
                {
                    if (memcmp(zh_network_data->original_target_mac, &s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)
                    {
                        on_send->status = ZH_NETWORK_SEND_SUCCESS;
                        esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portMAX_DELAY);
                    }
                    else if (zh_network_data->message_type == ZH_NETWORK_UNICAST)
                    {
                        zh_network_queue.id = ZH_NETWORK_WAIT_RESPONSE;
                        zh_network_queue.time = esp_timer_get_time() / 1000;
                        xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                    }
                }
            }
            else
            {
                if (memcmp(zh_network_data->original_target_mac, &s_broadcast_mac, ESP_NOW_ETH_ALEN) != 0)
                {
                    for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                    {
                        zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                        if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, ESP_NOW_ETH_ALEN) == 0)
                        {
                            zh_vector_delete_item(&s_route_vector, i);
                        }
                    }
                    zh_network_queue.id = ZH_NETWORK_WAIT_ROUTE;
                    zh_network_queue.time = esp_timer_get_time() / 1000;
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_SEARCH_REQUEST;
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->message_id = abs(esp_random()); // I don't know why, but esp_random() sometimes produces negative values.
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                }
            }
            esp_now_del_peer((uint8_t *)peer->peer_addr);
            free(on_send);
        ZH_NETWORK_ON_SEND_EXIT:
            free(peer);
            break;
        case ZH_NETWORK_ON_RECV:;
            switch (zh_network_data->message_type)
            {
            case ZH_NETWORK_BROADCAST:;
                zh_network_event_on_recv_t *on_recv = calloc(1, sizeof(zh_network_event_on_recv_t));
                memcpy(on_recv->mac_addr, zh_network_data->original_sender_mac, ESP_NOW_ETH_ALEN);
                on_recv->data_len = zh_network_data->data_len;
                on_recv->data = calloc(1, zh_network_data->data_len);
                memcpy(on_recv->data, zh_network_data->data, zh_network_data->data_len);
                esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_RECV_EVENT, on_recv, sizeof(zh_network_event_on_recv_t) + on_recv->data_len + sizeof(on_recv->data_len), portMAX_DELAY);
                free(on_recv);
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                break;
            case ZH_NETWORK_UNICAST:;
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, ESP_NOW_ETH_ALEN) == 0)
                {
                    zh_network_event_on_recv_t *on_recv = calloc(1, sizeof(zh_network_event_on_recv_t));
                    memcpy(on_recv->mac_addr, zh_network_data->original_sender_mac, ESP_NOW_ETH_ALEN);
                    on_recv->data_len = zh_network_data->data_len;
                    on_recv->data = calloc(1, zh_network_data->data_len);
                    memcpy(on_recv->data, zh_network_data->data, zh_network_data->data_len);
                    esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_RECV_EVENT, on_recv, sizeof(zh_network_event_on_recv_t) + on_recv->data_len + sizeof(on_recv->data_len), portMAX_DELAY);
                    free(on_recv);
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_DELIVERY_CONFIRM;
                    memcpy(zh_network_data->original_target_mac, zh_network_data->original_sender_mac, ESP_NOW_ETH_ALEN);
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->confirm_id = zh_network_data->message_id;
                    zh_network_data->message_id = abs(esp_random()); // I don't know why, but esp_random() sometimes produces negative values.
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                    break;
                }
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                break;
            case ZH_NETWORK_DELIVERY_CONFIRM:;
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, ESP_NOW_ETH_ALEN) == 0)
                {
                    zh_vector_push_back(&s_response_vector, &zh_network_data->confirm_id);
                    if (zh_vector_get_size(&s_response_vector) > s_zh_network_init_config.queue_size)
                    {
                        zh_vector_delete_item(&s_response_vector, 0);
                    }
                    break;
                }
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                break;
            case ZH_NETWORK_SEARCH_REQUEST:;
                for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                {
                    zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                    if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, ESP_NOW_ETH_ALEN) == 0)
                    {
                        zh_vector_delete_item(&s_route_vector, i);
                    }
                }
                { // Just to avoid the compiler warning.
                    zh_network_routing_table_t routing_table = {0};
                    memcpy(&routing_table.original_target_mac, zh_network_data->original_sender_mac, ESP_NOW_ETH_ALEN);
                    memcpy(&routing_table.intermediate_target_mac, zh_network_data->sender_mac, ESP_NOW_ETH_ALEN);
                    zh_vector_push_back(&s_route_vector, &routing_table);
                }
                if (zh_vector_get_size(&s_route_vector) > s_zh_network_init_config.route_vector_size)
                {
                    zh_vector_delete_item(&s_route_vector, 0);
                }
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, ESP_NOW_ETH_ALEN) == 0)
                {
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    zh_network_data->message_type = ZH_NETWORK_SEARCH_RESPONSE;
                    memcpy(zh_network_data->original_target_mac, zh_network_data->original_sender_mac, ESP_NOW_ETH_ALEN);
                    memcpy(zh_network_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN);
                    zh_network_data->data_len = 0;
                    memset(zh_network_data->data, 0, ZH_NETWORK_MAX_MESSAGE_SIZE);
                    zh_network_data->message_id = abs(esp_random()); // I don't know why, but esp_random() sometimes produces negative values.
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                    break;
                }
                zh_network_queue.id = ZH_NETWORK_ON_SEND;
                xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                break;
            case ZH_NETWORK_SEARCH_RESPONSE:;
                for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
                {
                    zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                    if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, ESP_NOW_ETH_ALEN) == 0)
                    {
                        zh_vector_delete_item(&s_route_vector, i);
                    }
                }
                { // Just to avoid the compiler warning.
                    zh_network_routing_table_t routing_table = {0};
                    memcpy(&routing_table.original_target_mac, zh_network_data->original_sender_mac, ESP_NOW_ETH_ALEN);
                    memcpy(&routing_table.intermediate_target_mac, zh_network_data->sender_mac, ESP_NOW_ETH_ALEN);
                    zh_vector_push_back(&s_route_vector, &routing_table);
                }
                if (zh_vector_get_size(&s_route_vector) > s_zh_network_init_config.route_vector_size)
                {
                    zh_vector_delete_item(&s_route_vector, 0);
                }
                if (memcmp(zh_network_data->original_target_mac, &s_self_mac, ESP_NOW_ETH_ALEN) != 0)
                {
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                }
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
                    zh_network_event_on_send_t *on_send = calloc(1, sizeof(zh_network_event_on_send_t));
                    memcpy(on_send->mac_addr, zh_network_data->original_target_mac, ESP_NOW_ETH_ALEN);
                    on_send->status = ZH_NETWORK_SEND_SUCCESS;
                    esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portMAX_DELAY);
                    free(on_send);
                    flag = true;
                    break;
                }
            }
            if (flag == false)
            {
                if ((esp_timer_get_time() / 1000 - zh_network_queue.time) > s_zh_network_init_config.max_waiting_time)
                {
                    if (memcmp(zh_network_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN) == 0)
                    {
                        zh_network_event_on_send_t *on_send = calloc(1, sizeof(zh_network_event_on_send_t));
                        memcpy(on_send->mac_addr, zh_network_data->original_target_mac, ESP_NOW_ETH_ALEN);
                        on_send->status = ZH_NETWORK_SEND_FAIL;
                        esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portMAX_DELAY);
                        free(on_send);
                    }
                    break;
                }
                xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
            }
            break;
        case ZH_NETWORK_WAIT_ROUTE:;
            for (uint16_t i = 0; i < zh_vector_get_size(&s_route_vector); ++i)
            {
                zh_network_routing_table_t *routing_table = zh_vector_get_item(&s_route_vector, i);
                if (memcmp(zh_network_data->original_target_mac, routing_table->original_target_mac, ESP_NOW_ETH_ALEN) == 0)
                {
                    zh_network_queue.id = ZH_NETWORK_ON_SEND;
                    xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
                    flag = true;
                    break;
                }
            }
            if (flag == false)
            {
                if ((esp_timer_get_time() / 1000 - zh_network_queue.time) > s_zh_network_init_config.max_waiting_time)
                {
                    if (memcmp(zh_network_data->original_sender_mac, &s_self_mac, ESP_NOW_ETH_ALEN) == 0)
                    {
                        zh_network_event_on_send_t *on_send = calloc(1, sizeof(zh_network_event_on_send_t));
                        memcpy(on_send->mac_addr, zh_network_data->original_target_mac, ESP_NOW_ETH_ALEN);
                        on_send->status = ZH_NETWORK_SEND_FAIL;
                        esp_event_post(ZH_NETWORK, ZH_NETWORK_ON_SEND_EVENT, on_send, sizeof(zh_network_event_on_send_t), portMAX_DELAY);
                        free(on_send);
                    }
                    break;
                }
                xQueueSend(s_zh_network_queue_handle, &zh_network_queue, portMAX_DELAY);
            }
            break;
        default:
            break;
        }
    }
    vTaskDelete(NULL);
}

void *zh_network_get_route(void)
{
    return &s_route_vector;
}