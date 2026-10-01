#pragma once
#include "FreeRTOS.h"
inline void vTaskDelay(int) {}
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, void**) { return 0; }
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void**, int) { return 0; }
inline void vTaskDelete(void*) {}
