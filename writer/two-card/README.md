# Two-card BAR1-to-64 GiB UEFI writer

A UEFI application (gnu-efi, x86_64) that resizes the BAR1 of two NVIDIA CMP 170HX
cards to 64 GiB before the ESXi loader runs, then chain-loads that loader. It is a
cold-boot-only tool: both targets must present their small cold layout, so an already
enlarged, partial or changed layout is refused and there is no resume path.

The two cards are fixed, in transaction order (`coordinator/pins/two_card_policy.h`):

| Slot | Endpoint / port | BAR1 (64 GiB) | BAR3 (32 MiB) |
|---|---|---|---|
| 0 | 01:00.0 / 00:01.1 | `0x64000000000` | `0x65000000000` |
| 1 | 02:00.0 / 00:01.3 | `0x66000000000` | `0x67000000000` |

Each reservation is `0x1002000000` bytes (64 GiB BAR1 + 32 MiB BAR3).

## What it does, in order

Before any write it captures the full resource picture and runs the pre-write gates
(see below). It then takes two GCD reservations in order, each of 64 GiB + 32 MiB at
64 GiB alignment, with cumulative overlap checks, before the first store. For each
card it performs the same store sequence, and every store is read back before the
next:

1. COMMAND cleared (memory/BME off) on both the endpoint and its parent bridge.
2. CYA register `0x724` = `0x30`.
3. CFG register `0xdcc` = `0x8000000a`.
4. ReBAR control set to size 16 (the 64 GiB selector) in the BAR1 ReBAR entry.
5. BAR1 address words (low/high) written to the planned 64 GiB-aligned base.
6. BAR3 address words (low/high) written to the next 64 GiB boundary.
7. The parent bridge prefetchable window set to enclose the reservation.
8. The parent bridge COMMAND restored.

The endpoint stays decode/BME-disabled; only the parent port's original COMMAND is
restored. After both cards verify, the app does one `LoadImage`/`StartImage` of the
existing pinned loader with its original load options.

## Pre-write gates

The app refuses before the first store unless all hold: the live SMBIOS system UUID
matches the host pin; BootCurrent and the Boot variable match; the loader bytes at the
pinned ESP path match the pinned size and SHA256; each endpoint's cold BAR0/BAR1/BAR3
and parent bridge windows match the retained values; the ReBAR capability offers the
64 GiB size; the UEFI root bus descriptors and aperture match; and every reservation
misses all retained memory, bridge and query intervals.

## No fallback (this variant)

This variant has no fail-open branch. A possible mutation failure retains both
reservations and requires cold recovery. A refusal or any failure is terminal: the
host is parked and needs a cold power cycle.

## Per-host values to regenerate

The host SMBIOS system UUID (placeholder in `src/build_pins.h`), the ESP partition
GUID, the Boot variable bytes, the loader SHA256 and size, and the BAR plan and
aperture addresses in `coordinator/pins/two_card_policy.h` and
`coordinator/pins/resource_types.h` are all specific to one host. Regenerate them for
any other host; the writer refuses on any mismatch by design.

## Build

```sh
make GNUEFI=/path/to/gnu-efi/usr two-card
```

Output: `../build/two-card/BOOTX64.EFI`. See `../Makefile` for flags and the
`WRITER_EXECUTE`/`WRITER_POLICY_BOUND` policy defines.

## Warning

There is no rollback, retry or reset inside the app.
