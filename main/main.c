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
#define DEFAULT_DELAY 5000
#define CYCLE_NUM 3
#define MAX_AP_NUM 10

#define TAG "MESH"
#define TAG_RIRQ "<RIRQ>" // Root Node Info Request
#define TAG_RIRP "<RIRP>" // Root Node Info Response
#define TAG_RV "<RVOTE>"  // Root Vote

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static xQueueHandle queue;
static bool is_root_defined = false;
static bool is_voting = false;
static char router_ssid[] = "FiberHGW_ZTDGZ4_2.4GHz";
static node_info_t this_node;
static node_info_t root_node;

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

static void espnow_deinit()
{
  vSemaphoreDelete(queue);
  esp_now_deinit();
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
  if (status != ESP_NOW_SEND_SUCCESS)
  {
    ESP_LOGE(TAG, "Send a packet to " MACSTR " fail", MAC2STR(mac_addr));
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

  memcpy(&packet, data, len);
  memcpy(packet.sender_mac, mac_addr, ESP_NOW_ETH_ALEN);
  if (xQueueSend(queue, &packet, ESPNOW_MAXDELAY) != pdTRUE)
  {
    ESP_LOGW(TAG, "Send receive queue fail");
  }
}

static void send_advertisement(uint8_t *mac)
{
  packet_t packet;
  packet.type = ADVERTISEMENT;
  packet.data_size = 0;
  memcpy(packet.src_mac, this_node.mac_addr, ESP_NOW_ETH_ALEN);
  memcpy(packet.dst_mac, broadcast_mac, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_send(mac, (u_int8_t *)&packet, sizeof(packet_t)));
}

static void advertise_task(void *pvParameter)
{
  packet_t packet;
  packet.type = ADVERTISEMENT;
  packet.data_size = 0;
  memcpy(packet.src_mac, this_node.mac_addr, ESP_NOW_ETH_ALEN);
  memcpy(packet.dst_mac, broadcast_mac, ESP_NOW_ETH_ALEN);
  while (true)
  {
    vTaskDelay((DEFAULT_DELAY * 2) / portTICK_RATE_MS);
    ESP_ERROR_CHECK(esp_now_send(broadcast_mac, (u_int8_t *)&packet, sizeof(packet_t)));
  }
}

static int8_t get_rssi()
{
  wifi_scan_config_t config;
  config.ssid = (uint8_t *)router_ssid;
  config.bssid = 0;
  config.channel = 0;
  config.show_hidden = false;
  ESP_ERROR_CHECK(esp_wifi_scan_start(&config, true));
  uint16_t number;
  wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * MAX_AP_NUM);
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&number));
  if (number > 0)
  {
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
    for (size_t i = 0; i < number; i++)
    {
      if (memcmp(ap_info[i].ssid, router_ssid, strlen(router_ssid)) == 0)
      {
        return ap_info[i].rssi;
      }
    }
  }
  ESP_LOGE("<RSSI>", "No APs found");
  free(ap_info);
  return INT8_MIN;
}

static void rootnode_vote_task(void *pvParameter)
{
  is_root_defined = false;
  for (size_t i = 0; i < CYCLE_NUM; i++)
  {
    packet_t packet;
    packet.type = ROOT_VOTE;
    packet.data_p = &root_node;
    packet.data_size = sizeof(node_info_t);
    memcpy(packet.src_mac, this_node.mac_addr, ESP_NOW_ETH_ALEN);
    memcpy(packet.dst_mac, broadcast_mac, ESP_NOW_ETH_ALEN);

    ESP_ERROR_CHECK(esp_now_send(broadcast_mac, (u_int8_t *)&packet, sizeof(packet_t)));
    ESP_LOGI(TAG_RV, "Root vote sent | root_mac :" MACSTR " RSSI: %d", MAC2STR(root_node.mac_addr), root_node.rssi);
    vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
  }
  is_root_defined = true;
  ESP_LOGI(TAG, "Root node: " MACSTR, MAC2STR(root_node.mac_addr));
  vTaskDelay(DEFAULT_DELAY * CYCLE_NUM / portTICK_RATE_MS);
  is_voting = false;
  vTaskDelete(NULL);
}

static void search_rootnode_task(void *pvParameter)
{
  packet_t packet;
  packet.type = ROOTNODE_INFO_REQUEST;
  packet.data_size = 0;
  memcpy(packet.src_mac, this_node.mac_addr, ESP_NOW_ETH_ALEN);
  memcpy(packet.dst_mac, broadcast_mac, ESP_NOW_ETH_ALEN);
  for (size_t i = 0; i < CYCLE_NUM; i++)
  {
    if (is_root_defined || is_voting)
    {
      break;
    }
    esp_err_t err = esp_now_send(broadcast_mac, (u_int8_t *)&packet, sizeof(packet_t));
    ESP_ERROR_CHECK(err);
    if (err == ESP_OK)
    {
      ESP_LOGI(TAG_RIRQ, "Root node info request sent");
    }
    vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
  }
  if (!is_root_defined)
  {
    ESP_LOGW(TAG_RIRQ, "Root node not found starting voting");
    xTaskCreate(rootnode_vote_task, "rootnode_vote_task", 2048, NULL, 4, NULL);
  }
  vTaskDelete(NULL);
}

static void handle_queue_task(void *pvParameter)
{
  packet_t packet;
  node_info_t root_info;
  while (xQueueReceive(queue, &packet, portMAX_DELAY) == pdTRUE)
  {
    memset(&root_info, 0, sizeof(node_info_t));
    if (esp_now_is_peer_exist(packet.sender_mac) == false)
    {
      add_peer(packet.sender_mac);
      send_advertisement(packet.sender_mac);
    }
    switch (packet.type)
    {
    case ROOTNODE_INFO_REQUEST:
      ESP_LOGI(TAG_RIRQ, "Root node info request taken");
      if (is_root_defined)
      {
        packet_t packet;
        packet.type = ROOTNODE_INFO_RESPONSE;
        packet.data_p = &root_node;
        ESP_ERROR_CHECK(esp_now_send(packet.sender_mac, (u_int8_t *)&packet, sizeof(packet_t)));
        ESP_LOGI(TAG_RIRQ, "Root node info sent to: " MACSTR, MAC2STR(packet.sender_mac));
      }
      break;
    case ROOTNODE_INFO_RESPONSE:
      if (is_voting)
      {
        ESP_LOGE(TAG_RIRP, "VOTING!!!");
      }
      ESP_LOGI(TAG_RIRP, "Root node info received from: " MACSTR, MAC2STR(packet.sender_mac));
      root_info = *(node_info_t *)packet.data_p;
      if (is_root_defined && memcmp(root_info.mac_addr, root_node.mac_addr, ESP_NOW_ETH_ALEN) != 0)
      {
        ESP_LOGE(TAG_RIRP, "Multiple root node info !!  Current Root: " MACSTR "Received Root: " MACSTR, MAC2STR(root_node.mac_addr), MAC2STR(root_info.mac_addr));
        break;
      }
      memcpy(root_node.mac_addr, root_info.mac_addr, ESP_NOW_ETH_ALEN);
      is_root_defined = true;
      break;
    case ROOT_VOTE:
      if (!is_voting)
      {
        is_voting = true;
        xTaskCreate(rootnode_vote_task, "rootnode_vote_task", 2048, NULL, 4, NULL);
      }
      memset(&root_info, 0, sizeof(node_info_t));
      root_info = *(node_info_t *)packet.data_p;
      ESP_LOGI(TAG_RV, "Root vote taken from: " MACSTR " candidate root: " MACSTR " RSSI: %d", MAC2STR(packet.sender_mac), MAC2STR(root_info.mac_addr), root_info.rssi);

      if (root_node.rssi < root_info.rssi)
      {
        ESP_LOGI(TAG_RV, "Root node info received from: " MACSTR, MAC2STR(packet.sender_mac));
        memcpy(root_node.mac_addr, root_info.mac_addr, ESP_NOW_ETH_ALEN);
      }
      else
      {
        ESP_LOGI(TAG_RV, "Current root RSSI: %d | Candidate root RSSI: %d", root_node.rssi, root_info.rssi);
      }
      break;
    case ADVERTISEMENT:
      if (esp_now_is_peer_exist(packet.sender_mac) == false)
      {
        add_peer(packet.sender_mac);
        send_advertisement(packet.sender_mac);
      }
      break;
    default:
      ESP_LOGE(TAG, "Unknown packet type: %s", data_types_strings[packet.type]);
      break;
    }
  }
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

  /* Set this node's and root's informations */
  this_node.rssi = get_rssi();
  esp_wifi_get_mac(WIFI_IF_STA, this_node.mac_addr);
  ESP_LOGI(TAG, "This node: " MACSTR " RSSI: %d", MAC2STR(this_node.mac_addr), this_node.rssi);
  root_node.rssi = this_node.rssi;
  memcpy(root_node.mac_addr, this_node.mac_addr, ESP_NOW_ETH_ALEN);

  /* Create Tasks */
  xTaskCreate(handle_queue_task, "handle_queue", 2048, NULL, 4, NULL);
  xTaskCreate(advertise_task, "advertise_task", 2048, NULL, 4, NULL);
  xTaskCreate(search_rootnode_task, "search_rootnode_task", 2048, NULL, 4, NULL);

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