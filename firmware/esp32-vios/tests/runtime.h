/* Native execution of the bridge's real RTOS boundary with bounded fake queues. */
#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#define VIOS_BRIDGE_TEST_RUNTIME 1
#define RTC_NOINIT_ATTR
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xFFFFFFFFu
#define ESP_RST_SW 1
static int reset_reason;
static int esp_reset_reason(){return reset_reason;}
struct FakeMutex {std::timed_mutex lock;std::atomic<bool> held{false};};
using SemaphoreHandle_t=FakeMutex*;
static std::deque<FakeMutex> mutexes;
static SemaphoreHandle_t xSemaphoreCreateMutex(){mutexes.emplace_back();return &mutexes.back();}
static int xSemaphoreTake(SemaphoreHandle_t m,unsigned ticks){
 bool acquired;if(ticks==portMAX_DELAY){m->lock.lock();acquired=true;}else acquired=m->lock.try_lock_for(std::chrono::milliseconds(ticks));
 if(acquired)m->held=true;return acquired;
}
static void xSemaphoreGive(SemaphoreHandle_t m){assert(m->held);m->held=false;m->lock.unlock();}
struct FakeQueue {unsigned capacity=0,size=0;std::deque<std::vector<uint8_t>> records;std::mutex lock;};
using QueueHandle_t=FakeQueue*;
static std::deque<FakeQueue> queues;
static QueueHandle_t xQueueCreate(unsigned count,unsigned size){queues.emplace_back();queues.back().capacity=count;queues.back().size=size;return &queues.back();}
static int xQueueSend(QueueHandle_t q,const void *data,unsigned){std::lock_guard<std::mutex> g(q->lock);if(q->records.size()==q->capacity)return 0;auto bytes=(const uint8_t*)data;q->records.emplace_back(bytes,bytes+q->size);return 1;}
static int xQueueReceive(QueueHandle_t q,void *data,unsigned){std::lock_guard<std::mutex> g(q->lock);if(q->records.empty())return 0;memcpy(data,q->records.front().data(),q->size);q->records.pop_front();return 1;}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q){std::lock_guard<std::mutex> g(q->lock);return q->records.size();}
static void xQueueReset(QueueHandle_t q){std::lock_guard<std::mutex> g(q->lock);q->records.clear();}
static void (*started_task)(void*);
static unsigned task_stack,task_priority,task_core;
static int xTaskCreatePinnedToCore(void(*fn)(void*),const char*,unsigned stack,void*,unsigned priority,void*,unsigned core){started_task=fn;task_stack=stack;task_priority=priority;task_core=core;return 1;}
static void vTaskDelay(unsigned ticks){delay(ticks);}
/* ESP OTA: a fake 8 KB otadata partition and a settable running slot. The
 * esp_ota_* calls follow IDF: state of the active entry, rewrite in place. */
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
typedef enum {ESP_OTA_IMG_NEW=0,ESP_OTA_IMG_PENDING_VERIFY=1,ESP_OTA_IMG_VALID=2,ESP_OTA_IMG_INVALID=3,ESP_OTA_IMG_ABORTED=4,ESP_OTA_IMG_UNDEFINED=-1} esp_ota_img_states_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_MIN 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_MAX 0x20
struct esp_partition_t {char label[17];unsigned subtype;};
static esp_partition_t running_partition={"app0",0x10},otadata_part={"otadata",0};
static uint8_t otadata_flash[8192];
static bool otadata_write_fails,ota_other_valid=true;
static unsigned ota_rollbacks,otadata_erases;
static const esp_partition_t *esp_partition_find_first(int,int,const char*){return &otadata_part;}
static esp_err_t esp_partition_read(const esp_partition_t*,size_t at,void *out,size_t n){memcpy(out,otadata_flash+at,n);return ESP_OK;}
static esp_err_t esp_partition_erase_range(const esp_partition_t*,size_t at,size_t n){memset(otadata_flash+at,0xFF,n);++otadata_erases;return ESP_OK;}
static esp_err_t esp_partition_write(const esp_partition_t*,size_t at,const void *in,size_t n){
    if(otadata_write_fails)return ESP_OK; // Models this board's bootloader: erase lands, program silently does not.
    for(size_t i=0;i<n;++i)otadata_flash[at+i]&=((const uint8_t*)in)[i];return ESP_OK;
}
static const esp_partition_t *esp_ota_get_running_partition(){return &running_partition;}
static int fake_active_sector(){
    int active=-1;uint32_t best=0;
    for(int i=0;i<2;++i){uint32_t seq,state;memcpy(&seq,otadata_flash+i*4096,4);memcpy(&state,otadata_flash+i*4096+24,4);
        if(seq!=0xFFFFFFFFu&&state!=3&&state!=4&&(active<0||seq>best)){active=i;best=seq;}}
    return active;
}
static esp_err_t esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t *state){
    int a=fake_active_sector();if(a<0)return ESP_FAIL;uint32_t v;memcpy(&v,otadata_flash+a*4096+24,4);
    if(v==0xFFFFFFFFu)return ESP_FAIL;*state=(esp_ota_img_states_t)v;return ESP_OK;
}
static esp_err_t esp_ota_mark_app_valid_cancel_rollback(){int a=fake_active_sector();if(a<0)return ESP_FAIL;uint32_t v=2;memcpy(otadata_flash+a*4096+24,&v,4);return ESP_OK;}
static esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(){
    if(!ota_other_valid)return ESP_FAIL;int a=fake_active_sector();if(a<0)return ESP_FAIL;
    uint32_t v=3;memcpy(otadata_flash+a*4096+24,&v,4);++ota_rollbacks;return ESP_OK;
}
