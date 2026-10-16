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
