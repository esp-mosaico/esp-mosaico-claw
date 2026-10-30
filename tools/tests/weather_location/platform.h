#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
typedef void *TaskHandle_t;
int xTaskCreate(void (*fn)(void *),const char *,unsigned,void *,unsigned,void *);
void vTaskDelete(void *);
void xTaskNotifyGive(void *);
unsigned ulTaskNotifyTake(int,unsigned);
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_malloc(n,c) malloc(n)
#define heap_caps_realloc(p,n,c) realloc(p,n)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
void esp_crt_bundle_attach(void);
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_all(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef struct {bool sta_connected;} wifi_manager_event_t;
typedef void (*wifi_test_callback_t)(const wifi_manager_event_t *,void *);
esp_err_t wifi_manager_register_event_callback(wifi_test_callback_t,void *);
esp_err_t wifi_manager_unregister_event_callback(wifi_test_callback_t,void *);
#define HTTP_METHOD_GET 0
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_EVENT_ON_DATA 2
typedef struct {void *user_data;int event_id;const char *header_key,*header_value;void *data;int data_len;} esp_http_client_event_t;
typedef struct {const char *url;int method;esp_err_t (*event_handler)(esp_http_client_event_t *);void *user_data;const char *user_agent;int timeout_ms,buffer_size,buffer_size_tx;void (*crt_bundle_attach)(void);bool keep_alive_enable;} esp_http_client_config_t;
typedef struct http_test_client *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
void esp_http_client_cleanup(esp_http_client_handle_t);
typedef struct {int (*read_cb)(uint8_t *,int,void *);int chunk_size;void *ctx;} gzip_miniz_cfg_t;
typedef void *gzip_miniz_handle_t;
gzip_miniz_handle_t gzip_miniz_init(gzip_miniz_cfg_t *);
int gzip_miniz_read(gzip_miniz_handle_t,uint8_t *,int);
void gzip_miniz_deinit(gzip_miniz_handle_t);
