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

typedef enum
{
  ACK,
  DATA,
  ROOT_VOTE,
  ADVERTISEMENT,
  ROUTING_TABLE,
} data_type_t;

typedef enum
{
  SEND,
  RECEIVE
} packet_type_t;

typedef struct
{
  unsigned int size;
  data_type_t type;
  void *data_p;
} data;

typedef struct
{
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  data data;
  packet_type_t packet_type;
  esp_now_send_status_t status;
} packet;

#endif