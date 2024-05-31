# ESP32 ESP-IDF and ESP8266 RTOS SDK component for ESP-NOW based mesh network

## Tested on

1. ESP8266 RTOS_SDK v3.4
2. ESP32 ESP-IDF v5.2

## Features

1. The maximum size of transmitted data is up to 218 bytes.
2. Support of any data types.
3. All nodes are not visible to the network scanner.
4. Not required a pre-pairings for data transfer.
5. Broadcast or unicast data transmissions.
6. There are no periodic/synchronous messages on the network. All devices are in "silent mode" and do not "hum" into the air.
7. Each node has its own independent routing table, updated only as needed.
8. Each node will receive/send a message if it "sees" at least one device on the network.
9. The number of devices on the network and the area of use is not limited.
10. Possibility uses WiFi AP or STA modes at the same time with ESP-NOW.

## Attention

1. The definition of ZH_NETWORK_MAX_MESSAGE_SIZE in the zh_network.h can be changed between 1 and 218. Smaller size - higher transmission speed. All devices on the network must have the same ZH_NETWORK_MAX_MESSAGE_SIZE.
2. For correct work at ESP-NOW + STA mode your WiFi router must be set on channel 1.
3. The ZHNetwork and the zh_network are incompatible.

## Testing

1. Program 2 receivers and 1 transmitter (specify the MAC of the 1st receiver in the code).
2. Connect the 1st receiver to the computer. Switch on serial port monitor. Turn transmitter on. Receiver will start receiving data.
3. Move transmitter as far away from receiver as possible until receiver is able to receive data (shield module if necessary).
4. Turn on the 2nd receiver and place it between the 1st receiver and transmitter (preferably in the middle). The 1st receiver will resume data reception (with relaying through the 2nd receiver). P.S. You can use a transmitter instead of the 2nd receiver - makes no difference.

## Arduino library

1. For using zh_network component on Arduino download and copy (with extract) zh_network.zip file to your folder for Arduino libraries. See README.md in this folder for using example.
2. Please pay attention - library tested on VSCode + PlatformIO. Not on Arduino IDE.

## Dependencies

1. [zh_vector](https://github.com/aZholtikov/zh_vector.git)

## [Function description](http://zh-network.zh.com.ru)

## Using

In an existing project, run the following command to install the component:

```text
cd ../your_project/components
git clone https://github.com/aZholtikov/zh_vector.git
git clone https://github.com/aZholtikov/zh_network.git
```

In the application, add the component:

```c
#include "zh_network.h"
```

## Example

Sending and receiving messages:

```c
#include "nvs_flash.h"
#include "esp_netif.h"
#include "zh_network.h"

#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

void zh_network_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

uint8_t target[6] = {0x58, 0xBF, 0x25, 0x18, 0xC8, 0x04};

typedef struct
{
    char char_value[30];
    int int_value;
    float float_value;
    bool bool_value;
} example_message_t;

void app_main(void)
{
    esp_log_level_set("zh_vector", ESP_LOG_NONE);
    esp_log_level_set("zh_network", ESP_LOG_NONE);
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_init_config);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_max_tx_power(8); // Power reduction is for example and testing purposes only. Do not use in your own programs!
    zh_network_init_config_t zh_network_init_config = ZH_NETWORK_INIT_CONFIG_DEFAULT();
    zh_network_init(&zh_network_init_config);
#ifdef CONFIG_IDF_TARGET_ESP8266
    esp_event_handler_register(ZH_NETWORK, ESP_EVENT_ANY_ID, &zh_network_event_handler, NULL);
#else
    esp_event_handler_instance_register(ZH_NETWORK, ESP_EVENT_ANY_ID, &zh_network_event_handler, NULL, NULL);
#endif
    example_message_t send_message = {0};
    strcpy(send_message.char_value, "THIS IS A CHAR");
    send_message.float_value = 1.234;
    send_message.bool_value = false;
    for (;;)
    {
        send_message.int_value = esp_random();
        zh_network_send(NULL, (uint8_t *)&send_message, sizeof(send_message));
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        zh_network_send(target, (uint8_t *)&send_message, sizeof(send_message));
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void zh_network_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case ZH_NETWORK_ON_RECV_EVENT:;
        zh_network_event_on_recv_t *recv_data = event_data;
        printf("Message from MAC %02X:%02X:%02X:%02X:%02X:%02X is received. Data lenght %d bytes.\n", MAC2STR(recv_data->mac_addr), recv_data->data_len);
        example_message_t *recv_message = (example_message_t *)recv_data->data;
        printf("Char %s\n", recv_message->char_value);
        printf("Int %d\n", recv_message->int_value);
        printf("Float %f\n", recv_message->float_value);
        printf("Bool %d\n", recv_message->bool_value);
        free(recv_data->data); // Do not delete to avoid memory leaks!
        break;
    case ZH_NETWORK_ON_SEND_EVENT:;
        zh_network_event_on_send_t *send_data = event_data;
        if (send_data->status == ZH_NETWORK_SEND_SUCCESS)
        {
            printf("Message to MAC %02X:%02X:%02X:%02X:%02X:%02X sent success.\n", MAC2STR(send_data->mac_addr));
        }
        else
        {
            printf("Message to MAC %02X:%02X:%02X:%02X:%02X:%02X sent fail.\n", MAC2STR(send_data->mac_addr));
        }
        break;
    default:
        break;
    }
}
```

Thanks to [Marton Larrosa](mailto:marton@mail.com) for participating in the testing.

Any [feedback](mailto:github@azholtikov.ru) will be gladly accepted.
