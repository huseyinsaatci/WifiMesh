#include "esp_check.h"

#ifndef ESPNOW_EXAMPLE_H
#define ESPNOW_EXAMPLE_H

/* ESPNOW can work in both station and softap mode. It is configured in menuconfig. */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF ESP_IF_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF ESP_IF_WIFI_AP
#endif

#define ESPNOW_QUEUE_SIZE 6

char *data_types_strings[] = {"ACK", "DATA", "ROOT_VOTE", "ADVERTISEMENT", "ROUTING_TABLE", "ROOTNODE_INFO_REQUEST", "ROOTNODE_INFO_RESPONSE"};

typedef enum
{
  ACK,
  DATA,
  ROOT_VOTE,
  ADVERTISEMENT,
  ROUTING_TABLE,
  ROOTNODE_INFO_REQUEST,
  ROOTNODE_INFO_RESPONSE,
} data_type_t;

typedef struct
{
  uint8_t src_mac[ESP_NOW_ETH_ALEN];
  uint8_t dst_mac[ESP_NOW_ETH_ALEN];
  uint8_t sender_mac[ESP_NOW_ETH_ALEN];
  uint8_t data_size;
  data_type_t type;
  void *data_p;
} packet_t;

typedef struct
{
  int8_t rssi;
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
} node_info_t;

#endif