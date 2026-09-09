# Grayscale capabilities

Query `display.grayscaleCapabilities(mode)` before choosing a grayscale upload
path. The descriptor is returned by value and allocates no memory. Querying does
not select a waveform, prepare the panel, or change uploaded data.

```cpp
const auto gray = display.grayscaleCapabilities(freeink::GrayscaleMode::Overlay);
if (!gray.supported()) {
  // Render the page in B/W.
} else if (gray.stripUploads) {
  // The existing writeGrayscalePlaneStrip() API is available.
} else {
  // Use the existing complete-plane upload calls.
}
```

## Modes and encoding

`Overlay` describes the existing B/W-base plus LSB/MSB-mask pipeline used by
CrossPoint. The `encoding` is `OverlayMasks`: in (LSB, MSB) order, dark gray is
(1,1), light gray is (0,1), and B/W pixels are (0,0), with black versus white
supplied by the base frame. This is the host input encoding; a driver may convert
it internally before sending controller RAM.

`Absolute` reserves the complete four-tone plane contract: black (0,0), dark
(1,0), light (0,1), white (1,1). Every pixel, including text and background,
would need to be supplied. **No driver currently advertises this mode.** Existing
`factoryMode`/calibration entry points do not imply this capability. The #40/#95
integrations must establish encoding, preparation, and next-refresh behavior
before opting in. Absolute pixel encoding does not promise that a waveform can
start from arbitrary previous ink without conditioning.

An unsupported mode returns the default descriptor: `supported()` is false and
all upload/overlap flags are false. Do not silently fall back from Absolute to
Overlay with the same input bytes.

## Execution properties

| Field | Meaning |
|---|---|
| `base` | `Separate`: the base is activated separately. `Combined`: `displayGrayscaleBase()` retains it and the gray pass activates both together. |
| `stripUploads` | `writeGrayscalePlaneStrip()` accepts this mode's encoding. False means use complete-plane uploads, not that grayscale is unsupported. |
| `asyncBase` | An ordinary asynchronous B/W refresh can supply the base. Dedicated X3 grayscale-base waveforms do not qualify. |
| `stagingWhileBusy` | Strip uploads and `prepareGrayscaleTarget()` stage host data without SPI access while a B/W waveform is pending. This does not authorize arbitrary display calls while BUSY. |

The facade reports no grayscale while inverted or without a driver. A pending
inversion transition disables `asyncBase`. CrossPoint's renderer additionally
disables `asyncBase` when its fading fix is active. Re-query after changing
controller or inversion settings; the descriptor is a snapshot, not an object
to cache for the display's lifetime. Support describes the implemented path,
not a guarantee that later buffer allocations cannot fail.

## Compatibility and migration

Drivers implement one `grayscaleCapabilities(mode)` override. The default
`PanelDriver` implementation advertises no grayscale. Existing boolean methods
remain compatibility wrappers for Overlay mode:

- `supportsStripGrayscale()` -> `stripUploads`
- `supportsAsyncGrayscaleBase()` -> `asyncBase`
- `supportsBusyGrayscaleStaging()` -> `stagingWhileBusy`
- `combinesGrayscaleBase()` -> `base == GrayscaleBase::Combined`

Custom PanelDriver subclasses should move their old boolean overrides into the
new descriptor; the facade now reads that descriptor directly. The public
FreeInkDisplay/EInkDisplay compatibility calls remain available.

CrossPoint exposes the same descriptor through HalDisplay and GfxRenderer.
EPUB/XTC and shared reader helpers use that query. Existing upload, preparation,
activation, and cleanup calls are retained; this change consolidates capability
selection, not the entire refresh lifecycle. CrossPoint must build against an
SDK containing this header and query (the local development configuration points
to the sibling SDK checkout).

## Validation

The display host tests cover driver availability, unsupported modes, inversion,
async readiness, compatibility wrappers, and query side effects, alongside the
existing X3/Pro transfer and lifecycle checks. On hardware, compare AA page turns,
image pages, inverted reading, and the first page after wake. Include Paper Mono
for combined-base behavior; the capability refactor changes no waveform tables.
