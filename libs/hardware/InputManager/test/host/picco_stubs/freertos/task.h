#pragma once
#include "FreeRTOS.h"
// Host fake: update() in async mode only runs its real path when
// xTaskGetCurrentTaskHandle() == _asyncTask, so tests that fake the poll task
// set fakeCurrentTask to the same cookie they store in _asyncTask.
inline TaskHandle_t fakeCurrentTask = nullptr;
inline void vTaskDelay(int) {}
inline int xTaskCreate(void (*)(void*),const char*,int,void*,int,void**) { return 0; }
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return fakeCurrentTask; }
