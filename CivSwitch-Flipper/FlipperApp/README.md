# CivSwitch for Flipper Zero

This is a native Flipper Zero FAP port of CivSwitch's Pico macro. It exposes the same composite USB device as the Pico build (Switch gamepad plus boot keyboard) and reuses the repository's generated `PicoSwitch/payload.txt` lines and established timings.

## Build

Install Node.js, npm, and uFBT. From this folder, run:

```powershell
./build.ps1
```

The resulting FAP is `dist/CivSwitch.fap`. This source was built with official Flipper firmware 1.5.1 (API 88.2) and launches on Unleashed unlshd-094 (API 88.19); no firmware change is needed for that release. Rebuild with uFBT for your firmware if a future loader reports an API mismatch.

## Use

1. Copy the FAP to the Flipper SD card under `apps/USB/`.
2. On the Switch, set the USB keyboard layout to English (US), open the Civ7 multiplayer lobby chat, and leave the chat input focused.
3. Connect the Flipper by USB and launch **Apps → USB → CivSwitch**.
4. Wait for `USB: composite HID connected`, then press OK. The macro waits one second, sends the five A-button preamble presses, and types each payload line with the original PicoSwitch delays.
5. Back cancels an active macro and releases both HID reports. The FAP restores the previous USB profile when it exits.

The application embeds the payload at build time. Re-run `compress.js`, `prepare_payload.mjs`, and `ufbt build` after changing `core.js`.

## Port notes

- USB descriptors and the `SwitchReport` layout are adapted from `PicoSwitch/include/SwitchConsts.h` and `PicoSwitch/src/switchController/SwitchUsb.cpp`.
- The Flipper IN endpoints are primed with zero-length reports after configuration. Each interface has its own initially available completion semaphore; endpoint callbacks gate subsequent reports, matching the official Flipper HID driver.
- The generated payload content, character map, report cadence, and line ordering come from CivSwitch's `PicoSwitch/compress.js` and `PicoSwitch/tools/payload_to_macro.py`.
- This is an external FAP, not a replacement Flipper firmware. It owns the USB profile only while running.
- Tested timings are copied as constants from CivSwitch; the Flipper's USB endpoint scheduling still needs validation against the console.

## License and attribution

CivSwitch's root `LICENSE` retains the MIT notice from Gezine's upstream repository. This does not automatically license third-party material under the same terms; review the source and licensing notes in `../README.md` before redistributing this port.
