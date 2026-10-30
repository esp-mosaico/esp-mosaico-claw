/* Tests execute the production parser, refresh and selected-city persistence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "../../../components/weather_service/weather_service.c"
struct host_mutex {bool held;};
static void (*after_unlock)(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void) {return calloc(1,sizeof(struct host_mutex));}
int xSemaphoreTake(SemaphoreHandle_t m,TickType_t t) {assert(m&&!m->held);m->held=true;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t m) {
 assert(m&&m->held);m->held=false;
 if(after_unlock){void (*f)(void)=after_unlock;after_unlock=NULL;f();}
 return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t m) {free(m);}
int xTaskCreate(void (*fn)(void *),const char *n,unsigned s,void *c,unsigned p,void *o) {return pdFALSE;}
void vTaskDelete(void *p) {}
void xTaskNotifyGive(void *p) {}
unsigned ulTaskNotifyTake(int a,unsigned b) {return 0;}
int64_t esp_timer_get_time(void) {return 1000000;}
uint32_t esp_random(void) {return 1;}
void esp_crt_bundle_attach(void) {}
const char *esp_err_to_name(esp_err_t e) {return "test";}
esp_err_t wifi_manager_register_event_callback(wifi_test_callback_t c,void *x) {return ESP_OK;}
esp_err_t wifi_manager_unregister_event_callback(wifi_test_callback_t c,void *x) {return ESP_OK;}
gzip_miniz_handle_t gzip_miniz_init(gzip_miniz_cfg_t *c) {return NULL;}
int gzip_miniz_read(gzip_miniz_handle_t h,uint8_t *b,int s) {return -1;}
void gzip_miniz_deinit(gzip_miniz_handle_t h) {}
static uint8_t blobs[2][sizeof(weather_cache_record_t)];static size_t sizes[2];static esp_err_t commit_error;
static unsigned key_index(const char *key) {return strcmp(key,WEATHER_LOCATION_KEY)==0;}
esp_err_t nvs_open(const char *n,int mode,nvs_handle_t *out) {*out=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t n,const char *key,void *out,size_t *size) {
 unsigned i=key_index(key);if(!sizes[i])return ESP_ERR_NVS_NOT_FOUND;
 if(*size<sizes[i])return ESP_ERR_INVALID_SIZE;
 memcpy(out,blobs[i],sizes[i]);*size=sizes[i];return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t n,const char *key,const void *data,size_t size) {
 unsigned i=key_index(key);assert(size<=sizeof(blobs[i]));memcpy(blobs[i],data,size);sizes[i]=size;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t n) {return commit_error;}
esp_err_t nvs_erase_all(nvs_handle_t n) {memset(sizes,0,sizeof(sizes));return ESP_OK;}
void nvs_close(nvs_handle_t n) {}
struct http_test_client {esp_http_client_config_t config;int status;};
static const char *ip_json="{\"status\":\"success\",\"lat\":31.2,\"lon\":121.5,\"city\":\"Shanghai\",\"timezone\":\"Asia/Shanghai\"}";
static const char *forecast_json="{\"properties\":{\"timeseries\":[{\"data\":{\"instant\":{\"details\":{\"air_temperature\":23.5}},\"next_1_hours\":{\"summary\":{\"symbol_code\":\"clearsky_day\"}}}}]}}";
static const char *cities_json="{}";static esp_err_t ip_error;static unsigned ip_calls,forecast_calls;
static void (*inflight)(void);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
 struct http_test_client *c=calloc(1,sizeof(*c));assert(c);c->config=*config;c->status=200;return c;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *k,const char *v) {return ESP_OK;}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
 const char *json;
 if(strstr(c->config.url,"ip-api")){++ip_calls;if(ip_error)return ip_error;json=ip_json;}
 else if(strstr(c->config.url,"api.met.no")){++forecast_calls;json=forecast_json;if(inflight){void (*f)(void)=inflight;inflight=NULL;f();}}
 else {json=cities_json;assert(strstr(c->config.url,"%")!=NULL);}
 esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_DATA,.user_data=c->config.user_data,.data=(void *)json,.data_len=(int)strlen(json)};
 return c->config.event_handler(&event);
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {return c->status;}
void esp_http_client_cleanup(esp_http_client_handle_t c) {free(c);}
static void reboot_service(void) {
 if(s_service.lock)vSemaphoreDelete(s_service.lock);
 memset(&s_service,0,sizeof(s_service));
 const weather_service_config_t config={.user_agent="test (test@example.invalid)"};
 assert(weather_service_init(&config)==ESP_OK);s_service.connected=true;
}
static weather_city_t beijing={.latitude=39.9,.longitude=116.4,.city="Beijing",.region="Beijing",.country="CN",.timezone="Asia/Shanghai"};
static void change_city_during_http(void) {assert(weather_service_set_city(&beijing)==ESP_OK);}
static weather_service_snapshot_t concurrent_refresh;
static uint32_t concurrent_revision;
static void begin_concurrent_refresh(void) {
 /* A worker can acquire its next refresh snapshot at the first unlocked
  * point of set_city(), then finish HTTP after set_city() returns. */
 xSemaphoreTake(s_service.lock,portMAX_DELAY);
 concurrent_refresh=s_service.snapshot;
 concurrent_revision=s_service.location_revision;
 xSemaphoreGive(s_service.lock);
}
static void finish_concurrent_refresh(void) {
 weather_service_snapshot_t fresh=s_service.snapshot;
 fresh.temperature_c=30;fresh.weather_valid=true;
 assert(weather_publish(&fresh,NULL,s_service.location_revision));
}
int main(void) {
 reboot_service();assert(weather_refresh_once()==ESP_OK);assert(s_service.snapshot.weather_valid&&s_service.snapshot.temperature_c==24);
 after_unlock=begin_concurrent_refresh;
 assert(weather_service_set_city(&beijing)==ESP_OK);
 assert(weather_publish(&concurrent_refresh,NULL,concurrent_revision));
 assert(s_service.snapshot.location_source==WEATHER_LOCATION_MANUAL&&!strcmp(s_service.snapshot.city,"Beijing"));
 puts("PASS selected city and refresh revision become visible atomically to the worker");
 assert(weather_service_use_network_location()==ESP_OK);assert(weather_refresh_once()==ESP_OK);
 after_unlock=finish_concurrent_refresh;
 const wifi_manager_event_t disconnected={.sta_connected=false};
 weather_wifi_event(&disconnected,NULL);
 assert(s_service.snapshot.temperature_c==30&&!s_service.snapshot.online);
 puts("PASS Wi-Fi changes cannot republish an older forecast over a completed refresh");
 s_service.connected=true;
 ip_error=ESP_ERR_TIMEOUT;assert(weather_refresh_once()==ESP_ERR_TIMEOUT);
 assert(s_service.snapshot.weather_valid&&s_service.snapshot.location_stale&&s_service.snapshot.stale&&s_service.snapshot.last_error==ESP_ERR_TIMEOUT);
 puts("PASS forecast success cannot hide IP-location failure; refresh retries");
 unsigned previous_ip=ip_calls;assert(weather_service_set_city(&beijing)==ESP_OK);assert(!s_service.snapshot.weather_valid);
 assert(weather_refresh_once()==ESP_OK);assert(ip_calls==previous_ip&&!s_service.snapshot.location_stale);
 reboot_service();assert(s_service.snapshot.location_source==WEATHER_LOCATION_MANUAL&&!strcmp(s_service.snapshot.city,"Beijing"));
 assert(s_service.snapshot.weather_valid);puts("PASS manual city survives reboot, keeps matching forecast and bypasses IP");
 assert(weather_service_use_network_location()==ESP_OK);reboot_service();
 assert(s_service.snapshot.location_source==WEATHER_LOCATION_IP&&!s_service.snapshot.location_valid&&!s_service.snapshot.weather_valid);
 puts("PASS network-mode reset survives reboot before first refresh, overriding old manual cache");
 ip_error=ESP_OK;inflight=change_city_during_http;assert(weather_refresh_once()==ESP_ERR_INVALID_STATE);
 assert(!strcmp(s_service.snapshot.city,"Beijing")&&!s_service.snapshot.weather_valid);
 puts("PASS delayed IP/forecast response cannot replace a newer manual city");
 commit_error=ESP_FAIL;weather_service_snapshot_t before=s_service.snapshot;
 assert(weather_service_use_network_location()==ESP_FAIL);assert(!memcmp(&before,&s_service.snapshot,sizeof(before)));commit_error=ESP_OK;
 puts("PASS failed persistence leaves the selected city and visible weather unchanged");
 weather_city_search_t result={0};cities_json="{\"results\":[{\"name\":\"Springfield\",\"latitude\":39.8,\"longitude\":-89.6,\"admin1\":\"Illinois\",\"country_code\":\"US\"},{\"name\":\"bad\",\"latitude\":200,\"longitude\":2}]}";
 assert(fetch_cities("Springfield, US",&result)==ESP_OK&&result.count==1&&!strcmp(result.cities[0].region,"Illinois"));
 cities_json="{}";result=(weather_city_search_t){0};assert(fetch_cities("nothing",&result)==ESP_OK&&!result.count);
 cities_json="{\"results\":false}";assert(fetch_cities("bad",&result)==ESP_ERR_INVALID_RESPONSE);
 cities_json="[]";assert(fetch_cities("bad",&result)==ESP_ERR_INVALID_RESPONSE);
 puts("PASS city parsing filters invalid coordinates and distinguishes empty from malformed replies");
 assert(weather_service_search_city("x")==ESP_ERR_INVALID_ARG);assert(weather_service_search_city("Shanghai")==ESP_OK);
 weather_city_search_t pending;assert(weather_service_get_city_search(&pending)==ESP_OK&&pending.state==WEATHER_SEARCH_PENDING);
 puts("PASS city search starts asynchronously and rejects incomplete input");
 sizes[1]=0;weather_cache_record_t old={.magic=WEATHER_CACHE_MAGIC,.version=3};old.snapshot.location_valid=true;old.snapshot.latitude=1;old.snapshot.longitude=2;
 nvs_set_blob(1,WEATHER_NVS_KEY,&old,sizeof(old));reboot_service();assert(!s_service.snapshot.weather_valid&&!s_service.snapshot.location_valid);
 assert(weather_service_erase_cache()==ESP_OK&&!sizes[0]&&!sizes[1]);puts("PASS old cache rejected and factory reset erases selected city");
 vSemaphoreDelete(s_service.lock);return 0;
}
