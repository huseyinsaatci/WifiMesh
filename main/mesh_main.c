#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "mesh_headers.h"
#include "esp_check.h"

#define ESPNOW_MAXDELAY 512

#define TAG "MESH"

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static xQueueHandle queue;
static bool is_root = false;
static bool is_root_defined = false;
static u_int8_t root_mac[ESP_NOW_ETH_ALEN];

static void wifi_init()
{
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
  ESP_ERROR_CHECK(esp_wifi_start());
}

static void example_espnow_deinit(packet_t *p)
{
  // free(p->data.data_p);
  // free(p);
  vSemaphoreDelete(queue);
  esp_now_deinit();
}

static void send_error_check(esp_err_t *err)
{
  esp_err_t error = *err;
  switch (error)
  {
  case ESP_ERR_ESPNOW_NOT_FOUND:
    ESP_LOGE(TAG, "Peer not found");
    break;
  case ESP_ERR_ESPNOW_ARG:
    ESP_LOGE(TAG, "Some error with the passed arguments");
    break;
  case ESP_ERR_ESPNOW_NO_MEM:
    ESP_LOGE(TAG, "out of memory");
    break;
  case ESP_ERR_ESPNOW_IF:
    ESP_LOGE(TAG, "WiFi interface doesnt match that of peer");
    break;
  case ESP_ERR_ESPNOW_INTERNAL:
    ESP_LOGE(TAG, "internal error");
    break;
  case ESP_ERR_ESPNOW_NOT_INIT:
    ESP_LOGE(TAG, "ESPNOW is not initialized");
    break;
  }
}

esp_err_t add_peer(uint8_t *mac)
{
  esp_now_peer_info_t peer;
  peer.channel = CONFIG_ESPNOW_CHANNEL;
  peer.ifidx = ESPNOW_WIFI_IF;
  peer.encrypt = false;
  memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);

  esp_err_t err = esp_now_add_peer(&peer);
  ESP_ERROR_CHECK(err);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to add peer: " MACSTR, MAC2STR(peer.peer_addr));
    return ESP_FAIL;
  }
  else
  {
    ESP_LOGI(TAG, "Added peer: " MACSTR, MAC2STR(peer.peer_addr));
  }
  return ESP_OK;
}

static void send_callback(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  packet_t packet;

  if (mac_addr == NULL)
  {
    ESP_LOGE(TAG, "Send cb arg error");
    return;
  }
  memcpy(packet.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  packet.status = status;
  packet.packet_type = SEND;
  if (xQueueSend(queue, &packet, ESPNOW_MAXDELAY) != pdTRUE)
  {
    ESP_LOGW(TAG, "Send send queue fail");
  }
}

static void receive_callback(const uint8_t *mac_addr, const uint8_t *data, int len)
{
  packet_t packet;

  if (mac_addr == NULL || data == NULL || len <= 0)
  {
    ESP_LOGE(TAG, "Receive cb arg error");
    return;
  }

  memcpy(&(packet.data), data, len);
  memcpy(packet.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  packet.packet_type = RECEIVE;
  if (xQueueSend(queue, &packet, ESPNOW_MAXDELAY) != pdTRUE)
  {
    ESP_LOGW(TAG, "Send receive queue fail");
  }
}

static void advertise_task(void *pvParameter)
{
  packet_t packet;
  packet.packet_type = SEND;
  memset(packet.mac_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
  packet.data.type = ADVERTISEMENT;
  int delay = 10000;
  u_int8_t *data = (u_int8_t *)&(packet.data);
  while (true)
  {
    vTaskDelay(delay / portTICK_RATE_MS);
    esp_err_t err = esp_now_send(broadcast_mac, data, sizeof(data_t));
    send_error_check(&err);
  }
}

static void handle_queue_task(void *pvParameter)
{
  packet_t packet;
  // data_t p_data = packet.data;
  while (xQueueReceive(queue, &packet, portMAX_DELAY) == pdTRUE)
  {
    switch (packet.packet_type)
    {
    case SEND:
      if (packet.status == ESP_NOW_SEND_SUCCESS)
      {
        ESP_LOGI(TAG, "Send success to: " MACSTR, MAC2STR(packet.mac_addr));
      }
      else if (packet.status == ESP_NOW_SEND_FAIL)
      {
        ESP_LOGE(TAG, "Send fail to: " MACSTR, MAC2STR(packet.mac_addr));
      }
      break;
    case RECEIVE:
      ESP_LOGI(TAG, "Receive from: " MACSTR, MAC2STR(packet.mac_addr));
      if (esp_now_is_peer_exist(packet.mac_addr) == false)
      {
        add_peer(packet.mac_addr);
      }
      // char *str = (char *)packet.data.data_p;
      // ESP_LOGI(TAG, "Data: %s", str);
      break;
    default:
      ESP_LOGE(TAG, "Unknown packet type");
      break;
    }
  }
}

static void task(void *pvParameter)
{
  packet_t *send_packet = (packet_t *)pvParameter;
  uint8_t *casted_packet = (uint8_t *)send_packet;
  packet_t queue_packet;

  ESP_LOGI(TAG, "Start sending broadcast data");
  ESP_LOGI(TAG, MACSTR, MAC2STR(broadcast_mac));
  while (1)
  {
    vTaskDelay(5000 / portTICK_RATE_MS);
    esp_err_t err = esp_now_send(broadcast_mac, casted_packet, sizeof(packet_t));
    send_error_check(&err);
  }
  /* Start sending broadcast ESPNOW data. */
  // if (esp_now_send(broadcast_mac, send_packet, sizeof(packet)) != ESP_OK)
  // {
  //   ESP_LOGE(TAG, "Send error");
  //   // example_espnow_deinit(send_param);
  //   vTaskDelete(NULL);
  // }

  // while (xQueueReceive(queue, &queue_packet, portMAX_DELAY) == pdTRUE)
  // {
  //   switch (queue_packet.packet_type)
  //   {
  //   case SEND:
  //   {
  //     ESP_LOGD(TAG, "Send data to " MACSTR ", status1: %d", MAC2STR(queue_packet.mac_addr), queue_packet.status);

  //     /* Delay a while before sending the next data. */
  //     vTaskDelay(ESPNOW_MAXDELAY / portTICK_RATE_MS);

  //     ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(queue_packet.mac_addr));

  //     /* Send the next data after the previous data is sent. */
  //     if (esp_now_send(send_packet->mac_addr, casted_packet, sizeof(packet_t)) != ESP_OK)
  //     {
  //       ESP_LOGE(TAG, "Send error");
  //       example_espnow_deinit(&queue_packet);
  //       vTaskDelete(NULL);
  //     }
  //     break;
  //   }
  //   case RECEIVE:
  //   {
  //     ESP_LOGI(TAG, "Receive broadcast data from: " MACSTR ", len: %d", MAC2STR(queue_packet.mac_addr), queue_packet.data.size);

  //     /* If MAC address does not exist in peer list, add it to peer list. */
  //     if (esp_now_is_peer_exist(queue_packet.mac_addr) == false)
  //     {
  //       add_peer(queue_packet.mac_addr);
  //     }
  //     break;
  //   }
  //   default:
  //     ESP_LOGE(TAG, "Callback type error");
  //     ESP_LOGI(TAG, "Packet type: %u", queue_packet.packet_type);
  //     break;
  //   }
  // }
  vTaskDelete(NULL);
}

static esp_err_t espnow_init(void)
{
  queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(packet_t));
  if (queue == NULL)
  {
    ESP_LOGE(TAG, "Create mutex fail");
    return ESP_FAIL;
  }

  /* Initialize ESPNOW and register sending and receiving callback function. */
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(send_callback));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(receive_callback));

  /* Set primary master key. */
  ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK));

  /* Add broadcast peer information to peer list. */
  add_peer(broadcast_mac);

  if (esp_now_is_peer_exist(broadcast_mac))
  {
    ESP_LOGI(TAG, "Broadcast peer is in peer list");
  }
  else
  {
    ESP_LOGE(TAG, "Broadcast peer is not in peer list");
  }

  /* Initialize sending parameters. */
  packet_t *packet = malloc(sizeof(packet_t));
  // memset(packet, 0, sizeof(struct packet));
  if (packet == NULL)
  {
    ESP_LOGE(TAG, "Malloc send parameter fail");
    vSemaphoreDelete(queue);
    esp_now_deinit();
    return ESP_FAIL;
  }

  packet->data.data_p = malloc(CONFIG_ESPNOW_SEND_LEN);
  if (packet->data.data_p == NULL)
  {
    ESP_LOGE(TAG, "Malloc send buffer fail");
    // free(packet);
    vSemaphoreDelete(queue);
    esp_now_deinit();
    return ESP_FAIL;
  }
  memcpy(packet->mac_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
  packet->data.size = CONFIG_ESPNOW_SEND_LEN;
  packet->status = 0;
  char *d = "hello";
  packet->data.data_p = d;
  packet->data.size = sizeof(5);
  packet->data.type = SEND;
  // xTaskCreate(task, "example_espnow_task", 2048, packet, 4, NULL);
  xTaskCreate(handle_queue_task, "handle_queue", 2048, NULL, 4, NULL);
  xTaskCreate(advertise_task, "advertise_task", 2048, NULL, 4, NULL);

  return ESP_OK;
}

void app_main(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init();
  espnow_init();
}