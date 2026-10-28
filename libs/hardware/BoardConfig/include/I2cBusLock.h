#pragma once
// The main loop and the firmware's render task both use the I2C helpers (input polling there,
// the battery gauge here). Two transfers in flight at once fail with ESP_ERR_INVALID_STATE, so
// every helper holds this lock for one complete transfer. Recursive: helpers may nest.
#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace freeink {

#ifdef ESP_PLATFORM
inline SemaphoreHandle_t i2cBusMutex() {
  static StaticSemaphore_t storage;
  static const SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexStatic(&storage);
  return mutex;
}

class I2cBusLock {
 public:
  I2cBusLock() { xSemaphoreTakeRecursive(i2cBusMutex(), portMAX_DELAY); }
  ~I2cBusLock() { xSemaphoreGiveRecursive(i2cBusMutex()); }
  I2cBusLock(const I2cBusLock&) = delete;
  I2cBusLock& operator=(const I2cBusLock&) = delete;
};
#else
// Host tests run single-threaded. User-provided members keep guard variables "used".
class I2cBusLock {
 public:
  I2cBusLock() {}
  ~I2cBusLock() {}
};
#endif

}  // namespace freeink
