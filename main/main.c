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
// #include "Arduino.h"
// #include "WiFi.h"

#define ESPNOW_MAXDELAY 512
#define DEFAULT_DELAY 5000
#define CYCLE_NUM 10
#define MAX_AP_NUM 10

#define TAG "MESH"
#define TAG_RIRQ "<RIRQ>" // Root Node Info Request
#define TAG_RIRP "<RIRP>" // Root Node Info Response
#define TAG_RV "<RVOTE>"  // Root Vote

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static xQueueHandle queue;
static bool is_root = false;
static bool is_root_defined = false;
static bool is_voting = false;
static u_int8_t root_mac[ESP_NOW_ETH_ALEN];
static u_int8_t candidate_root_mac[ESP_NOW_ETH_ALEN];
static char router_ssid[] = "FiberHGW_ZTDGZ4_2.4GHz";
static int8_t router_rssi = -1000;

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

static void example_espnow_deinit()
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
  // packet_t packet;

  // if (mac_addr == NULL)
  // {
  //   ESP_LOGE(TAG, "Send cb arg error");
  //   return;
  // }
  // memcpy(packet.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  // packet.status = status;
  // packet.packet_type = SEND;
  // if (xQueueSend(queue, &packet, ESPNOW_MAXDELAY) != pdTRUE)
  // {
  //   ESP_LOGW(TAG, "Send send queue fail");
  // }
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

static void send_advertisement(uint8_t *mac)
{
  data_t data;
  data.type = ADVERTISEMENT;
  ESP_ERROR_CHECK(esp_now_send(mac, (u_int8_t *)&data, sizeof(data_t)));
}

static void get_rssi()
{
  wifi_scan_config_t config;
  config.ssid = (uint8_t *)router_ssid;
  // memcpy(config.ssid, router_ssid, strlen(router_ssid));
  config.bssid = 0;
  config.channel = 0;
  config.show_hidden = false;
  ESP_ERROR_CHECK(esp_wifi_scan_start(&config, true));
  uint16_t number;
  wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * MAX_AP_NUM);
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&number));
  if (number < 1)
  {
    ESP_LOGE(TAG, "No APs found");
  }
  else
  {
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
    for (size_t i = 0; i < number; i++)
    {
      ESP_LOGI("<RSSI>", "RSSI: %d", ap_info[i].rssi);
      if (memcmp(ap_info[i].ssid, router_ssid, strlen(router_ssid)) == 0)
      {
        router_rssi = ap_info[i].rssi;
        break;
      }
    }
  }
  free(ap_info);
}

static void rootnode_vote_task(void *pvParameter)
{
  for (size_t i = 0; i < CYCLE_NUM; i++)
  {
    data_t data;
    data.data_p = malloc(sizeof(root_node_t));
    data.type = ROOT_VOTE;
    root_node_t root_vote;
    memset(root_vote.mac_addr, candidate_root_mac, ESP_NOW_ETH_ALEN);
    data.data_p = &root_vote;

    ESP_ERROR_CHECK(esp_now_send(broadcast_mac, (u_int8_t *)&(data), sizeof(data_t)));
    ESP_LOGI(TAG_RV, "Root vote sent to: " MACSTR, MAC2STR(broadcast_mac));
    free(data.data_p);
    vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
  }
  memset(root_mac, candidate_root_mac, ESP_NOW_ETH_ALEN);
  is_root_defined = true;
  vTaskDelay(DEFAULT_DELAY * CYCLE_NUM / portTICK_RATE_MS);
  is_voting = false;
  vTaskDelete(NULL);
}

static void advertise_task(void *pvParameter)
{
  data_t data;
  data.type = ADVERTISEMENT;
  while (true)
  {
    // vTaskDelay(DEFAULT_DELAY * 2 / portTICK_RATE_MS);
    vTaskDelay((DEFAULT_DELAY / 2) / portTICK_RATE_MS);
    ESP_ERROR_CHECK(esp_now_send(broadcast_mac, (u_int8_t *)&data, sizeof(data_t)));
  }
}

static void handle_queue_task(void *pvParameter)
{
  packet_t packet;
  while (xQueueReceive(queue, &packet, portMAX_DELAY) == pdTRUE)
  {
    switch (packet.packet_type)
    {
    case SEND:
      // if (packet.status == ESP_NOW_SEND_SUCCESS)
      // {
      //   // ESP_LOGI(data_types_strings[packet.packet_type], "Send success to:" MACSTR "packet type: %s", MAC2STR(packet.mac_addr), data_types_strings[packet.packet_type]);
      //   ESP_LOGI(TAG, "Send success to:" MACSTR, MAC2STR(packet.mac_addr));
      // }
      // else if (packet.status == ESP_NOW_SEND_FAIL)
      // {
      //   ESP_LOGE(TAG, "Send fail to: " MACSTR, MAC2STR(packet.mac_addr));
      // }
      break;
    case RECEIVE:
      ESP_LOGI(TAG, "Receive from: " MACSTR, MAC2STR(packet.mac_addr));
      if (esp_now_is_peer_exist(packet.mac_addr) == false)
      {
        add_peer(packet.mac_addr);
        send_advertisement(packet.mac_addr);
      }
      switch (packet.data.type)
      {
      case ROOTNODE_INFO_REQUEST:
        if (is_root_defined)
        {
          data_t data;
          data.type = ROOTNODE_INFO_RESPONSE;
          memset(data.data_p, root_mac, ESP_NOW_ETH_ALEN);
          ESP_ERROR_CHECK(esp_now_send(packet.mac_addr, (u_int8_t *)&(data), sizeof(data_t)));
          ESP_LOGI(TAG_RIRQ, "Root node info sent to: " MACSTR, MAC2STR(packet.mac_addr));
        }
        ESP_LOGI(TAG_RIRQ, "Root node info request taken");
        break;
      case ROOTNODE_INFO_RESPONSE:
        if (is_root_defined && memcmp(packet.mac_addr, root_mac, ESP_NOW_ETH_ALEN) != 0)
        {
          ESP_LOGE(TAG_RIRP, "Multiple root node info !!  Current Root: " MACSTR "Received Root: " MACSTR, MAC2STR(root_mac), MAC2STR(packet.mac_addr));
          break;
        }
        memcpy(root_mac, packet.data.data_p, ESP_NOW_ETH_ALEN);
        is_root_defined = true;
        ESP_LOGI(TAG_RIRP, "Root node info received from: " MACSTR, MAC2STR(packet.mac_addr));
        break;
      case ROOT_VOTE:
        if (!is_voting)
        {
          is_voting = true;
          xTaskCreate(rootnode_vote_task, "rootnode_vote_task", 2048, NULL, 4, NULL);
        }
        root_node_t *received_vote = (root_node_t *)packet.data.data_p;

        // Add if RSSI is bigger condition
        get_rssi();
        if (router_rssi > received_vote->rssi)
        {
          memset(candidate_root_mac, received_vote->mac_addr, ESP_NOW_ETH_ALEN);
        }

        ESP_LOGI(TAG_RV, "Root vote taken from: " MACSTR, MAC2STR(received_vote->mac_addr));
        break;
      default:
        break;
      }

      break;
    default:
      ESP_LOGE(TAG, "Unknown packet type");
      break;
    }
  }
}

static void test_task(void *pvParameter)
{
  wifi_scan_config_t config;
  config.ssid = 0;
  config.bssid = 0;
  config.channel = 0;
  config.show_hidden = false;
  while (true)
  {
    if (esp_wifi_scan_start(&config, true) != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to start scan");
      vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
      continue;
    }
    uint16_t number;
    wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * 10);
    if (esp_wifi_scan_get_ap_records(&number, ap_info) != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to get scan result");
      vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
      continue;
    }
    esp_wifi_scan_get_ap_num(&number);
    if (number < 1)
    {
      ESP_LOGE(TAG, "No AP found");
      vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
      continue;
    }
    ESP_LOGI(TAG, "Number of APs: %d", number);
    for (size_t i = 0; i < number; i++)
    {
      ESP_LOGI(TAG, "Ap name:%s RSSI: %d", (ap_info + i)->ssid, (ap_info + i)->rssi);
      // ESP_LOGI(TAG, "Ap name: RSSI: %d", (ap_info + i)->rssi);
      vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
    }

    vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
  }
}

static void search_rootnode_task(void *pvParameter)
{
  data_t data;
  data.type = ROOTNODE_INFO_REQUEST;
  for (size_t i = 0; i < CYCLE_NUM; i++)
  {
    if (is_root_defined)
    {
      break;
    }
    esp_err_t err = esp_now_send(broadcast_mac, (u_int8_t *)&data, sizeof(data_t));
    ESP_ERROR_CHECK(err);
    if (err == ESP_OK)
    {
      ESP_LOGI(TAG_RIRQ, "Root node info request sent");
    }
    vTaskDelay(DEFAULT_DELAY / portTICK_RATE_MS);
  }
  if (!is_root_defined)
  {
    ESP_LOGE(TAG_RIRQ, "Root node not found starting voting");
    xTaskCreate(rootnode_vote_task, "rootnode_vote_task", 2048, NULL, 4, NULL);
  }
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

  /* Create Tasks */
  xTaskCreate(handle_queue_task, "handle_queue", 2048, NULL, 4, NULL);
  xTaskCreate(advertise_task, "advertise_task", 2048, NULL, 4, NULL);
  xTaskCreate(search_rootnode_task, "search_rootnode_task", 2048, NULL, 4, NULL);

  return ESP_OK;
}

void app_main(void)
{
  // initArduino();
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