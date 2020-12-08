// Copyright 2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>

#include "esp_libc.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_wifi_osi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_log.h"

#include "nvs.h"

void *__wifi_task_create(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio)
{
    //ESP_LOGV("FUNC", "*__wifi_task_create");

    portBASE_TYPE ret;
    xTaskHandle handle;
    ret = xTaskCreate(task_func, name, stack_depth, param, prio, &handle);

    return ret == pdPASS ? handle : NULL;
}

void __wifi_task_delete(void *task)
{
    //ESP_LOGV("FUNC", "__wifi_task_delete");

    vTaskDelete(task);
}

void __wifi_task_yield_from_isr(void)
{
    //ESP_LOGV("FUNC", "__wifi_task_yield_from_isr");

    portYIELD_FROM_ISR();
}

void __wifi_task_delay(uint32_t tick)
{
    //ESP_LOGV("FUNC", "__wifi_task_delay");

    vTaskDelay(tick);
}

uint32_t __wifi_task_get_max_priority(void)
{
    //ESP_LOGV("FUNC", "__wifi_task_get_max_priority");

    return (uint32_t)(configMAX_PRIORITIES);
}

uint32_t __wifi_task_ms_to_ticks(uint32_t ms)
{
    //ESP_LOGV("FUNC", "__wifi_task_ms_to_ticks");

    return (uint32_t)(ms / portTICK_RATE_MS);
}

void __wifi_task_suspend_all(void)
{
    //ESP_LOGV("FUNC", "__wifi_task_suspend_all");

    vTaskSuspendAll();
}

void __wifi_task_resume_all(void)
{
    //ESP_LOGV("FUNC", "__wifi_task_resume_all");

    xTaskResumeAll();
}

void *__wifi_queue_create(uint32_t queue_len, uint32_t item_size)
{
    //ESP_LOGV("FUNC", "*__wifi_queue_create");

    return (void *)xQueueCreate(queue_len, item_size);
}

void __wifi_queue_delete(void *queue)
{
    //ESP_LOGV("FUNC", "__wifi_queue_delete");

    vQueueDelete(queue);
}

int __wifi_queue_send(void *queue, void *item, uint32_t block_time_tick, uint32_t pos)
{
    //ESP_LOGV("FUNC", "__wifi_queue_send");

    signed portBASE_TYPE ret;
    BaseType_t os_pos;

    if (pos == OSI_QUEUE_SEND_BACK)
        os_pos = queueSEND_TO_BACK;
    else if (pos == OSI_QUEUE_SEND_FRONT)
        os_pos = queueSEND_TO_FRONT;
    else
        os_pos = queueOVERWRITE;

    if (block_time_tick == OSI_FUNCS_TIME_BLOCKING) {
        ret = xQueueGenericSend(queue, item, portMAX_DELAY, os_pos);
    } else {
        ret = xQueueGenericSend(queue, item, block_time_tick, os_pos);
    }

    return ret == pdPASS ? true : false;
}

int __wifi_queue_send_from_isr(void *queue, void *item, int *hptw)
{
    //ESP_LOGV("FUNC", "__wifi_queue_send_from_isr");

    signed portBASE_TYPE ret;

    ret = xQueueSendFromISR(queue, item, (signed portBASE_TYPE *)hptw);

    return ret == pdPASS ? true : false;
}

int __wifi_queue_recv(void *queue, void *item, uint32_t block_time_tick)
{
    //ESP_LOGV("FUNC", "__wifi_queue_recv");

    signed portBASE_TYPE ret;

    if (block_time_tick == OSI_FUNCS_TIME_BLOCKING) {
        ret = xQueueReceive(queue, item, portMAX_DELAY);
    } else {
        ret = xQueueReceive(queue, item, block_time_tick);
    }

    return ret == pdPASS ? true : false;
}

uint32_t __wifi_queue_msg_num(void *queue)
{
    //ESP_LOGV("FUNC", "__wifi_queue_msg_num");

    return (uint32_t)uxQueueMessagesWaiting((const QueueHandle_t)queue);
}

void *__wifi_timer_create(const char *name, uint32_t period_ticks, bool auto_load, void *arg, void (*cb)(void *timer))
{
    //ESP_LOGV("FUNC", "(*cb)");

    return xTimerCreate(name, period_ticks, auto_load, arg, (tmrTIMER_CALLBACK)cb);
}

int __wifi_timer_reset(void *timer, uint32_t ticks)
{
    //ESP_LOGV("FUNC", "__wifi_timer_reset");

    return xTimerReset(timer, ticks);
}

int __wifi_timer_stop(void *timer, uint32_t ticks)
{
    //ESP_LOGV("FUNC", "__wifi_timer_stop");

    return xTimerStop(timer, ticks);
}

void *__wifi_task_top_sp(void)
{
    //ESP_LOGV("FUNC", "*__wifi_task_top_sp");

    extern uint32_t **pxCurrentTCB;

    return pxCurrentTCB[0];
}

void* __wifi_semphr_create(uint32_t max, uint32_t init)
{
    //ESP_LOGV("FUNC", "__wifi_semphr_create");

    return (void*)xSemaphoreCreateCounting(max, init);
}

void __wifi_semphr_delete(void* semphr)
{
    //ESP_LOGV("FUNC", "__wifi_semphr_delete");

    vSemaphoreDelete(semphr);
}

int32_t __wifi_semphr_take(void* semphr, uint32_t block_time_tick)
{
    //ESP_LOGV("FUNC", "__wifi_semphr_take");

    if (block_time_tick == OSI_FUNCS_TIME_BLOCKING) {
        return (int32_t)xSemaphoreTake(semphr, portMAX_DELAY);
    } else {
        return (int32_t)xSemaphoreTake(semphr, block_time_tick);
    }
}

int32_t __wifi_semphr_give(void* semphr)
{
    //ESP_LOGV("FUNC", "__wifi_semphr_give");

    return (int32_t)xSemaphoreGive(semphr);
}
