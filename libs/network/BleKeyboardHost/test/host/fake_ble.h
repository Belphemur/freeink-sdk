#pragma once

// Test driver for the fake NimBLE stack in stubs/. It drives the real
// BleKeyboardHost (src/BleKeyboardHost.cpp, FREEINK_CAP_BLE_HID_HOST=1):
//
//   fakeble::resetWorld();
//   int map = fakeble::addCharacteristic(0x2A4B, true, false, false);
//   fakeble::setValue(map, reportMap, sizeof reportMap);
//   int in = fakeble::addInputReport();
//   fakeble::beginHost();
//   fakeble::connectTo("AA:BB:CC:DD:EE:01");
//   fakeble::notify(in, report, sizeof report);
//   fakeble::host().popKey(ev);

#include <NimBLEDevice.h>
#include <stddef.h>
#include <stdint.h>

#include "BleKeyboardHost.h"

namespace fakeble {

// Where the connection task can be held. Connect is released by a GAP cancel or
// a disconnect; Security and Discovery wait on an established link, so only a
// disconnect (or the test) releases them, as in NimBLE.
enum class Stage : uint8_t { None, Connect, Security, Discovery };

freeink::BleKeyboardHost& host();

void resetWorld();
bool beginHost();
// Starts a connect and waits (real time) for the link or a failure.
bool connectTo(const char* addr, uint32_t timeoutMs = 2000);
// Waits until the connection task is back at its idle wait.
bool waitForWorkerIdle(uint32_t timeoutMs = 1000);
// The peripheral drops the link (out of range, powered off).
void peerDisconnect();

int addCharacteristic(uint16_t uuid, bool canRead, bool canWrite, bool canNotify);
// A notifiable Report characteristic with an Input Report Reference descriptor.
int addInputReport();
void setValue(int index, const uint8_t* data, size_t len);
bool notify(int index, const uint8_t* data, size_t len);

void advertise(const char* addr, const char* name);
size_t retainedScanResults();

void holdAt(Stage stage);
bool waitUntilHeld(Stage stage, uint32_t timeoutMs = 1000);
// True once vTaskDelete() was called on the connection task while it was held
// inside a NimBLE wait.
bool taskDeletedWhileHeld();

size_t connectCalls();
unsigned long clockMs();
void advanceMillis(uint32_t ms);

}  // namespace fakeble
