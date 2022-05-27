#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
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
int counter = 0;

void wifi_init()
{
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
  ESP_ERROR_CHECK(esp_wifi_start());
}

static void example_espnow_deinit(packet *p)
{
  // free(p->data.data_p);
  // free(p);
  vSemaphoreDelete(queue);
  esp_now_deinit();
}

void add_peerx(uint8_t *mac)
{
  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL)
  {
    ESP_LOGE(TAG, "Malloc peer information fail");
    vSemaphoreDelete(queue);
    esp_now_deinit();
    return ESP_FAIL;
  }
  memset(peer, 0, sizeof(esp_now_peer_info_t));
  peer->channel = CONFIG_ESPNOW_CHANNEL;
  peer->ifidx = ESPNOW_WIFI_IF;
  peer->encrypt = false;
  memcpy(peer->peer_addr, mac, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(peer));
  free(peer);
  return ESP_OK;
}

void add_peer(uint8_t *mac)
{
  esp_now_peer_info_t peer;
  peer.channel = CONFIG_ESPNOW_CHANNEL;
  peer.ifidx = ESPNOW_WIFI_IF;
  peer.encrypt = false;
  memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));
  return ESP_OK;
}

static void send_callback(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  packet packet;

  if (mac_addr == NULL)
  {
    ESP_LOGE(TAG, "Send cb arg error");
    return;
  }
  u_int8_t *casted_data = (u_int8_t *)&packet.data;
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
  packet packet;

  if (mac_addr == NULL || data == NULL || len <= 0)
  {
    ESP_LOGE(TAG, "Receive cb arg error");
    return;
  }

  packet.packet_type = RECEIVE;
  memcpy(packet.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  // packet = malloc(len);
  // if (packet.data == NULL)
  // {
  //   ESP_LOGE(TAG, "Malloc receive data fail");
  //   return;
  // }
  memcpy(&packet, data, len);
  // packet.data.size = len;
  if (xQueueSend(queue, &packet, ESPNOW_MAXDELAY) != pdTRUE)
  {
    ESP_LOGW(TAG, "Send receive queue fail");
    // free(&packet);
  }
}

static void task(void *pvParameter)
{
  packet *send_packet = (packet *)pvParameter;
  uint8_t *casted_packet = (uint8_t *)send_packet;
  packet queue_packet;

  vTaskDelay(5000 / portTICK_RATE_MS);
  ESP_LOGI(TAG, "Start sending broadcast data");
  ESP_LOGI(TAG, MACSTR, MAC2STR(broadcast_mac));
  esp_err_t err = esp_now_send(broadcast_mac, casted_packet, sizeof(packet));
  /* Start sending broadcast ESPNOW data. */
  // if (esp_now_send(broadcast_mac, send_packet, sizeof(packet)) != ESP_OK)
  // {
  //   ESP_LOGE(TAG, "Send error");
  //   // example_espnow_deinit(send_param);
  //   vTaskDelete(NULL);
  // }

  switch (err)
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

  while (xQueueReceive(queue, &queue_packet, portMAX_DELAY) == pdTRUE)
  {
    switch (queue_packet.packet_type)
    {
    case SEND:
    {
      ESP_LOGD(TAG, "Send data to " MACSTR ", status1: %d", MAC2STR(queue_packet.mac_addr), queue_packet.status);

      /* Delay a while before sending the next data. */
      vTaskDelay(ESPNOW_MAXDELAY / portTICK_RATE_MS);

      ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(queue_packet.mac_addr));

      /* Send the next data after the previous data is sent. */
      if (esp_now_send(send_packet->mac_addr, casted_packet, sizeof(packet)) != ESP_OK)
      {
        ESP_LOGE(TAG, "Send error");
        example_espnow_deinit(&queue_packet);
        vTaskDelete(NULL);
      }
      break;
    }
    case RECEIVE:
    {
      ESP_LOGI(TAG, "Receive broadcast data from: " MACSTR ", len: %d", MAC2STR(queue_packet.mac_addr), queue_packet.data.size);

      /* If MAC address does not exist in peer list, add it to peer list. */
      if (esp_now_is_peer_exist(queue_packet.mac_addr) == false)
      {
        add_peer(queue_packet.mac_addr);
      }
      break;
    }
    default:
      ESP_LOGE(TAG, "Callback type error");
      break;
    }
  }
}

static esp_err_t espnow_init(void)
{
  queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(packet));
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
  packet *packet = malloc(sizeof(packet));
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
  xTaskCreate(task, "example_espnow_task", 2048, packet, 4, NULL);

  return ESP_OK;
}

void app_main(void)
{
  ESP_LOGI(TAG, "SAAAA");
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