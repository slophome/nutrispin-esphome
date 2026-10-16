# Petoneer NutriSpin PF006 for ESPHome on Raspberry Pi Pico W

An external ESPHome component for local Bluetooth control of two Petoneer
NutriSpin PF006 feeders from one Raspberry Pi Pico W. Each feeder has a
separate GATT client, connection state and credentials. Home Assistant receives
the entities through ESPHome's encrypted native API.

The component supports pairing directly from Home Assistant after a manual
factory reset. No Tuya account, Petoneer account, APK or companion computer is
required for that pairing flow or normal operation. Feeder MAC addresses do
not need to be entered: the Pico discovers and remembers them when pairing.

**Status:** the configuration compiles for Pico W, and the C++ protocol codec
has passed live authentication and read-only tests with a feeder using a Mac
as the Bluetooth adapter. The complete firmware, simultaneous operation of
two feeders, and pairing buttons still require validation on physical Pico W
hardware.

## Contents

```text
components/nutrispin/    External component and protocol codec
pico-nutrispin.yaml     Two-feeder Pico W configuration
secrets.example.yaml    Placeholders; no device credentials
requirements.txt        Tested ESPHome version
```

This directory contains no real
device credentials, Wi-Fi credentials, application binaries, device captures,
private logs or compiled firmware.

## Requirements

- Raspberry Pi Pico W (`rpipicow`).
- ESPHome **2026.9.1**, the version used to validate and compile this project.
  Later versions may work if they retain the same RP2 GATT client API.
- Home Assistant with the ESPHome integration.
- The tested PF006 variant: product ID `66d1tpmo`, Tuya BLE protocol 4.2,
  96-byte device information and flags `0x10`.

Earlier ESPHome releases may not provide the RP2 GATT client API used here.
Other feeder models and protocol variants are not supported by this version.

## Setup

1. Keep `pico-nutrispin.yaml` beside the `components/` directory, or adjust
   the local external-component path in the YAML.
2. Copy `secrets.example.yaml` to `secrets.yaml`. Enter your Wi-Fi credentials,
   generate a unique API encryption key, and choose a long random OTA password.
   The all-zero API key in the example is deliberately invalid and must be
   replaced. Generate a key with:

   ```sh
   openssl rand -base64 32
   ```

3. Leave the `feeder_1` and `feeder_2` secrets as empty strings for pairing
   directly on the Pico. Optional credential imports are described below.
4. Set the time zone in the `time:` section. The example uses `Europe/Warsaw`.
5. Install the pinned ESPHome release, validate, and compile:

   ```sh
   python3 -m venv .venv
   . .venv/bin/activate
   python -m pip install -r requirements.txt
   esphome config pico-nutrispin.yaml
   esphome compile pico-nutrispin.yaml
   ```

6. Flash the Pico. For a first USB installation, hold BOOTSEL while connecting
   it and copy the generated `firmware.uf2` to the `RPI-RP2` drive. With this
   configuration the build output is under
   `.esphome/build/pico-nutrispin/.pioenvs/pico-nutrispin/`.
   You can also use `esphome run pico-nutrispin.yaml` for the normal
   compile/upload/log workflow. Later updates can use OTA.
7. Add the Pico through Home Assistant's ESPHome integration using your API
   encryption key. Home Assistant also supplies the clock.

Do not publish your populated `secrets.yaml`, runtime device data, build
directory or firmware images containing credentials. The included `.gitignore`
excludes common local files.

## Manual factory reset (PF006)

A factory reset may erase the feeding schedule and other settings. Record
your feeding times before resetting a feeder.

1. Unlock the panel by holding **<** and **>** together.
2. Press and hold the middle **MENU / ≡** button until the feeder resets.
   No menu navigation or confirmation is needed.

The panel unlock combination is described on the
[manufacturer's NutriSpin product page](https://www.petoneer.com/cn/NutriSpin?from=nav).

For two feeders, reset only the first one, close the Petoneer app and pair it
using **Feeder 1 Pair** in Home Assistant. Once pairing finishes, reset the
second feeder and use **Feeder 2 Pair**. The detailed pairing flow is below.

## Pairing from Home Assistant

There is one pairing button per configured feeder: **Feeder 1 Pair** and
**Feeder 2 Pair**. Entity labels, button actions, language options, diagnostics
and status messages default to English. Entity names can be overridden in the
YAML.

1. [Manually factory-reset](#manual-factory-reset-pf006) the feeder you want to
   pair. A factory reset may erase its feeding schedule.
2. Close Petoneer or other Bluetooth clients that might occupy its connection.
3. Press **Feeder 1 Pair** or **Feeder 2 Pair** to choose the destination slot.
   The Pico scans for compatible, factory-reset PF006 feeders. Discovery and
   selection remain open for up to 60 seconds. Pairing one slot at a time
   prevents both buttons from claiming the same device.
4. After a short discovery period, a single available feeder is selected
   automatically. If several are found, **Pairing Devices** lists the numbered
   results with their detected addresses and signal strengths. Choose the
   matching **Device 1** through **Device 6** in that slot's **Pairing Device**
   dropdown. No address needs to be typed or copied into YAML. An unavailable
   selection is rejected; pressing Pair again restarts discovery.
5. The Pico decodes the factory UUID from the BLE advertisement and scan
   response, checks the product and reset state, and generates local keys.
6. It persists the address and keys **before** sending the pairing payload, disconnects,
   and verifies authentication on a fresh encrypted connection.

A feeder assigned to the other slot is excluded from discovery results.
For the simplest setup, reset and pair the first feeder, then repeat for the
second. Paired devices that have not been reset are never adopted automatically.
Pressing Pair for an existing slot can replace its assignment with a newly
selected, factory-reset feeder that is not assigned to the other slot.

This flow sends no unbind command and does not reset a feeder automatically.
A device paired to a different controller needs a manual factory reset.
Interrupted attempts retain the same pending keys, including across Pico
restarts. Motor commands are not automatically replayed after a timeout or
disconnect.

Assignments and credentials are stored separately for each slot in ESPHome
preferences on the Pico. Keep the YAML IDs `feeder_1` and `feeder_2` stable
across updates; their display names can change. Normal OTA updates should
preserve the saved assignments. Erasing flash or replacing the
Pico requires pairing again or importing credentials from an existing backup.

## Home Assistant entities

The component creates **48 entities per feeder**, plus three Pico diagnostic
entities in the example configuration.

| Feature | Entities and behavior |
|---|---|
| Pairing | Pair button, Pairing Device dropdown, numbered Pairing Devices results, paired indicator and status text |
| Connection | Connected indicator, BLE RSSI, MAC, firmware/hardware/protocol versions |
| Battery, DP11 | Percentage reported by the feeder; values above 100 become unknown |
| Tray movement, DP2 | Separate next-compartment and previous-compartment buttons |
| Schedule, DP1 | Five entries, each with an enable switch, HH:MM input, weekday mask and raw count field |
| Raw schedule | Full readback and write access as 50 hex characters / 25 bytes |
| Physical buttons, DP104 | Enable or disable manual feeding buttons |
| Sound, DP18 | Numeric value 0–2 from the product's declared range |
| Language, DP101 | Five values from the product's declared range |
| History, DP102 | Latest record timestamp, record code, record count and complete raw history in two parts |
| Rule query, DP103 | Included in refresh and periodic polling |
| Feed report, DP15 | Value if reported; the product schema marks this as unused |
| Clock | Uses Home Assistant time; automatic synchronization every 30 minutes and a Sync Clock button |
| Factory reset, DP9 | Separate reset button, disabled by default in Home Assistant |

Advanced entities such as weekday masks, raw count fields, raw history and
some diagnostics are disabled by default. Enable them in Home Assistant's
entity settings if needed.

Changing an entry's time does not enable it automatically. Use its enable
switch separately. Schedule editing requires a fresh read after connecting.
After reconnecting, the feeder's schedule report populates the editable time,
enable and numeric controls in Home Assistant. Publishing those values does
not write them back to the feeder; a user or automation edit initiates a write.
Each edit sends the complete schedule and waits for matching readback; wait
for confirmation before making another edit. Refreshing only requests data.

### Clock synchronization and time zones

The example refreshes the Pico's clock from Home Assistant every 30 minutes
(`time.update_interval`). Each feeder also has a `clock_sync_interval: 30min`.
When due, the Pico waits for pending commands and schedule confirmation to
finish, then briefly reconnects over Bluetooth so the feeder requests the
current time. The **Sync Clock** button queues the same operation manually.
No Home Assistant automation is required.

If a feeder is disconnected or the Pico has no valid time yet, automatic
synchronization waits until a connection and valid time are available.
Reconnecting normally also allows the feeder to request time; a time reply
restarts its 30-minute interval.

Schedules store local hours and minutes. Synchronizing the clock leaves the
schedule entries unchanged. With `timezone: Europe/Warsaw`, the Pico includes
the current UTC offset, including daylight saving time, in its time replies.
The next synchronization after a seasonal change updates the feeder clock.
To use another time zone, change the YAML and upload the configuration again.
Changing only Home Assistant's time zone does not override this explicit setting.
Feeder behavior for meals in a skipped or repeated hour still needs hardware
verification, as does the automatic synchronization flow on a physical Pico W.

### Recovery after Pico power loss

Removing power from the Pico breaks both Bluetooth connections. The component
flushes each slot's address and credentials to flash when pairing, and reloads
them at startup. It then scans for the saved feeders and attempts to reconnect
without pressing Pair again. Keep the same YAML slot IDs across updates.

ESPHome requests time as soon as Home Assistant's API connection authenticates;
the 30-minute setting controls subsequent refreshes. If a feeder requests time
before the Pico has a valid clock, the component defers the response until time
is available. Deferred requests are discarded when that Bluetooth session ends.
Queued motor commands are not restored after a Pico restart.

Feeders that retain power continue running their own stored schedules while
the Pico is off. Restoring credentials, initiating connections for both slots,
handling delayed time and discarding stale session requests have been checked
in a host simulation of the actual component with substitute hardware services.
A physical Pico power-cycle test is still required to verify radio recovery
and feeder behavior end to end.

The weekday mask is a raw value from 0 to 127; 127 selects every day. The
mapping of individual bits to weekdays has not been confirmed on hardware.
The fourth byte of each schedule record is exposed as a raw count field;
its meaning is not fully verified and it is **not a tray-position sensor**.

History contains up to 40 five-byte records: one code byte followed by a
little-endian 32-bit Unix timestamp. Each raw-history entity holds 200 hex
characters / 20 records, staying below Home Assistant's state-length limit.
The last-feeding entity reports the newest history record's time, with its
code exposed separately. It does not establish the current open compartment
or prove that food was dispensed successfully.

### Custom entity names

Each feeder accepts overrides for the generated entities. For example:

```yaml
nutrispin:
  - id: feeder_1
    name: "Feeder 1"
    time_id: ha_clock
    credentials: !secret feeder_1
    pair:
      name: "Feeder 1 Pair"
    battery:
      name: "Feeder 1 Battery"
```

Merge those entries into the provided two-feeder configuration. Remaining
entity labels and status text keep their current defaults.

## Optional: one credential secret per feeder

The `feeder_1` and `feeder_2` values can remain empty. Pairing buttons generate
and save credentials on the Pico without updating `secrets.yaml`.

If you already have a credential export, put its complete JSON object into
one YAML secret and reference it through `credentials:`. The parser accepts
`version`, `model`, `product_id`, `mac`, `uuid`, `device_id`, `local_key_hex`,
`beacon_key_hex`, `handshake` and `provisioning` fields as produced by the setup
tools. Required identity fields must match the supported model. An imported
MAC is adopted automatically, provided it is not assigned to the other slot.
The factory UUID must be 16 ASCII characters; the virtual device ID may be
up to 22 characters.

`local_key_hex` represents six raw login-key bytes, encoded as 12 hex digits.
Legacy `local_key` imports use their first six ASCII bytes instead. Credentials
already stored on the Pico take precedence so an OTA update cannot silently
restore an older key. Keys are not exposed as Home Assistant entities or
printed in the component log.

## Limitations and validation

All recognized datapoints from the inspected product schema are represented.
Language selection, DP15 and some settings may not be implemented by a
particular feeder firmware. Missing reports leave those entities unknown.
Battery readings while powered by USB have not been verified as a measure of
backup-battery charge.

There is no confirmed current-compartment sensor. Updating the feeder's own
firmware from the manufacturer's servers is not implemented; the configured
OTA service updates the Pico only.

Configuration tests, sanitizer-enabled native C++ protocol and discovery tests,
host recovery simulations, and a Pico W build passed. Discovery tests cover interleaved advertisements
from multiple feeders, UUID isolation, duplicate reports, paired/unsupported
devices, stale results and bounded storage. Live tests using the Mac radio validated the C++ wire codec,
factory UUID decoding, encrypted authentication and status reads. Running the
complete firmware on a Pico, two concurrent links and setting writes still
need hardware verification. Automatic discovery, the Home Assistant candidate
selector and saved slot assignments have not yet been exercised on a physical
Pico W. No device reset or tray movement was performed while developing them.

## References

- [ESPHome RP2 BLE tracker](https://esphome.io/components/rp2_ble_tracker/)
- [ESPHome GATT platform capabilities](https://esphome.io/components/bluetooth_proxy/)
- [Tuya device-side protocol reference](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/apps/common/third_party_profile/tuya_protocol/sdk/src/tuya_ble_data_handler.c)

This is an independent integration for the tested Petoneer product.
