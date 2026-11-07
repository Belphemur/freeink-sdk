#pragma once

// FreeInk SDK — boot-time recovery hatch.
//
// The stock Xteink (and most ESP32) second-stage bootloader can't read buttons —
// it just boots whatever otadata selects. So "hold a combo at reset to fall back
// to the recovery firmware" can only be honoured by the firmware that actually
// boots. This is that check, made shareable: call recovery::checkBootCombo() as
// the VERY FIRST thing in setup() of every firmware you want to be escapable
// (the recovery flasher itself, the editor, the reader, ...).
//
// Convention: the recovery / "escape hatch" firmware lives in OTA slot 0 (ota_0,
// the default upload offset 0x10000). Held at reset, the combo repoints otadata
// at slot 0 and reboots into it.
//
// The combo is board-specific: boardCombo() resolves the ACTIVE profile's pair —
// Back + Up on ADC-ladder boards (the classic Xteink layout), Power + Next on
// the X4 Pro / X4 Classic (their Up key is GPIO0, a boot strap that must not be
// held at reset; on the Pro Back/Confirm are the GT911's capacitive Home key +
// touch), and Up + Down on the EEGO A4 (its Back key is touch-only). Pass a
// pair explicitly through SdUpdateOptions to override it for an SD update.
//
// Call order: on multi-device binaries, call BoardConfig::selectDevice() BEFORE
// either checkBootCombo() overload — the combo resolves from the ACTIVE
// profile, which defaults to DEFAULT_DEVICE until selection.
//
// It is always safe to call unconditionally and early — it does nothing unless
// ALL of these hold: the combo is pressed, slot 0 contains a valid app image, and
// the caller isn't already running from slot 0 (so inside the recovery firmware
// it's a no-op). When it does act it reboots and never returns.
//
// The SdUpdateOptions overload layers an SD-card flash on the same combo: when
// the combo is held AND an update image sits at the configured SD path (default
// /update.bin), it is validated and flashed into the next OTA slot, otadata is
// repointed, and the device reboots into it — no menu, no per-app UI code. Only
// when no update file is present (or the flash fails) does the combo fall back
// to the plain slot-0 hatch above, so a bad .bin can't strand the user.
//
// What it can't do: escape a firmware that crashes in ROM / early SDK init before
// this call is reached. (A corrupt app *image* is still caught for free — the
// bootloader falls back to the other OTA slot on its own.) A truly unconditional
// GPIO recovery would require a custom second-stage bootloader, which the recovery
// firmware deliberately never reflashes.

#include <InputManager.h>
#include <esp_partition.h>

#include "FirmwareFlasher.h"

namespace freeink {
namespace recovery {

// Read the recovery combo and, if held, switch to OTA slot 0 and reboot. Returns
// immediately (no reboot) in every other case. On multi-device binaries, call
// BoardConfig::selectDevice() first (see the file comment above).
void checkBootCombo();

// The board's recovery combo, resolved from the ACTIVE BoardConfig profile.
// X4 Pro / X4 Classic: Power + Next (BTN_POWER + BTN_DOWN). EEGO A4: Up + Down
// (BTN_UP + BTN_DOWN; Back is touch-only there). Every other profile: Back + Up.
// On multi-device binaries, resolve only after BoardConfig::selectDevice() —
// ACTIVE.board defaults to DEFAULT_DEVICE until then.
struct RecoveryCombo {
  int8_t button1;
  int8_t button2;  // -1 = single-button latch
};
RecoveryCombo boardCombo();

struct SdUpdateOptions {
  // SD path checked for a firmware image while the combo is held.
  const char* path = "/update.bin";
  // Optional flash progress hook (e.g. to drive a display). When unset,
  // progress is logged to Serial every 10%.
  firmware::ProgressCb onProgress = nullptr;
  void* progressCtx = nullptr;
  // Rename the image to "<path>.flashed" after a successful flash so holding
  // the combo through the post-flash reset can't flash the same file again.
  bool renameOnSuccess = false;
  // The combo, as InputManager button indices. The default kAutoCombo resolves
  // that slot to the ACTIVE board's boardCombo() (Back + Up on ADC-ladder
  // boards, Power + Next on the X4 Pro / X4 Classic, Up + Down on the EEGO A4);
  // overriding one slot keeps the board's other key, so an explicit single key
  // still needs the board's partner held. Pass button2 = -1 for an explicit
  // single-button latch; button1 = -1 disables the check.
  static constexpr int8_t kAutoCombo = -128;  // INT8_MIN: resolve from boardCombo()
  int8_t button1 = kAutoCombo;
  int8_t button2 = kAutoCombo;
};

// Combo-latched SD update: when the combo is held at reset, look for
// `options.path` on the SD card and flash it into the next OTA slot, then
// reboot into it (never returns). With no update file present — or if the
// flash fails — falls back to the plain slot-0 hatch of checkBootCombo(),
// using the same (possibly overridden) combo. Returns immediately when the
// combo isn't held.
//
// Ordering: call this before bringing up the display or app state, but after
// any SD power prerequisite — SDCardManager::setPowerHook() for PMIC-gated
// rails, and BoardConfig::selectDevice() on multi-device binaries. It latches
// the board's power rails itself (BoardConfig::holdPowerRails()) so a
// released power button can't cut power mid-flash on battery-latched boards.
void checkBootCombo(const SdUpdateOptions& options);

// Point the bootloader at `dest` by writing a fresh otadata entry into the
// inactive selection slot. Bypasses esp_ota_set_boot_partition's
// esp_image_verify (which rejects patched vendor images). Does not reboot.
bool switchBootPartition(const esp_partition_t* dest);

}  // namespace recovery
}  // namespace freeink
