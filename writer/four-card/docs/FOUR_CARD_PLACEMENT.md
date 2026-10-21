# Four-card placement: 47:00.0 and 48:00.0 inside `\_SB.S0D2`

Scope: interval arithmetic over retained evidence plus the physically confirmed two-card layout. It authorizes nothing; the writer re-validates every interval live (112 identities, 568 slots, 26 bridges, root descriptors, EFI memory map, seven AML pins) before any GCD reservation or PCI store. Checked with an offline placement checker (verdict `CANDIDATE_PLACEMENT_CLEAR`, 608 intervals, 8 sources). All addresses below are this host's measured values and must be regenerated for another host.

## Inputs

| Input | Value | Source |
|---|---|---|
| S0D2 declared buses | 40-7f (`BRB 0x40`, `BRL 0x40`) | DSDT `Device (S0D2)`, `_UID 1` |
| S0D2 declared high memory | `0x47e20200000-0x63cffffffff` (`MABL 0x47e20200000`, `MALL 0x1bedfe00000`) | same; matches the ACPI root findings table |
| PCI0 declared high memory | `0x63d20200000-0x7fbffffffff` | same source (used by 01/02) |
| retained GCD, S0D2 part | unowned MMIO `0x47e20200000-0x63cefffffff`; owned `0x63cf0000000-0x63cfecfffff` (root 40 current prefetch window); unowned `0x63cfed00000-0x63cffffffff` | retained inventory, `inventory.gcd` |
| root 40 current allocations | `0xb2000000-0xb6cfffff` (32-bit), `0x63cf0000000-0x63cfecfffff` (64-bit), buses 40-4c | `root_current_allocations` |
| 47:00.0 cold BARs | BAR0 `0xb5000000` 16 MiB, BAR1 `0x63cf8000000` 64 MiB, BAR3 `0x63cfc000000` 32 MiB; port 40:03.1 windows `0xb5f0b500 0xfdf1f801 0x63c 0x63c` | retained CONFIG32 owner 48 / port owner 47 |
| 48:00.0 cold BARs | BAR0 `0xb4000000` 16 MiB, BAR1 `0x63cf0000000` 64 MiB, BAR3 `0x63cf4000000` 32 MiB; port 40:03.3 windows `0xb4f0b400 0xf5f1f001 0x63c 0x63c` | retained CONFIG32 owner 50 / port owner 49 |
| 47/48 ReBAR | header `0xbb0`, three entries, BAR1 capability `0x400` control `0x601` (64 MiB selected), same as 01/02 cold | `validated_rebar`, EXT_CONFIG32 owners 0x30/0x32 |
| confirmed two-card layout | 01 BAR1 `0x64000000000` BAR3 `0x65000000000`; 02 BAR1 `0x66000000000` BAR3 `0x67000000000`; 47/48 unchanged 64 MiB | the confirmed two-card result |

## Rule (the root00 pattern, applied to S0D2)

For each card, in transaction order: BAR1 at the lowest 64 GiB-aligned base at or above the root's declared aperture start, BAR3 at the next 64 GiB boundary (BAR1 + 64 GiB), one enclosing reservation of `0x1002000000` bytes (BAR1 64 GiB + BAR3 32 MiB) that is also the parent port's prefetchable window, and a 128 GiB stride to the next card. Applied to PCI0 this rule reproduces exactly the `0x64000000000/0x66000000000` layout (`rule_reproduces_root00: true` in the record), so the S0D2 placements are the same construction, not a new one.

## Placement table

| Target | Port | Root | BAR1 (64 GiB) | BAR3 (32 MiB) | Reservation / bridge window | GCD container |
|---|---|---|---|---|---|---|
| 01:00.0 (already applied) | 00:01.1 | PCI0 | `0x64000000000` | `0x65000000000` | `0x64000000000-0x65001ffffff` | `0x63d20200000-0x7fbefffffff` |
| 02:00.0 (already applied) | 00:01.3 | PCI0 | `0x66000000000` | `0x67000000000` | `0x66000000000-0x67001ffffff` | same |
| **47:00.0** | 40:03.1 | S0D2 | **`0x48000000000`** | **`0x49000000000`** | `0x48000000000-0x49001ffffff` | `0x47e20200000-0x63cefffffff` |
| **48:00.0** | 40:03.3 | S0D2 | **`0x4a000000000`** | **`0x4b000000000`** | `0x4a000000000-0x4b001ffffff` | same |

Register images the 13-store transaction writes (computed live from the plan, same formulas as for 01/02): 47 port `0x24` = `0x01f10001` (`((base>>16)&0xfff0)|1 | ((((limit>>16)&0xfff0)|1)<<16)` with base `0x48000000000`, limit `0x49001ffffff`), `0x28` = `0x480`, `0x2c` = `0x490`; endpoint BAR1 `0x14/0x18` = `0x0000000c/0x480`, BAR3 `0x1c/0x20` = `0x0000000c/0x490`. For 48: port `0x24` = `0x01f10001`, `0x28` = `0x4a0`, `0x2c` = `0x4b0`; BAR1 high `0x4a0`, BAR3 high `0x4b0`. (01: `0x01f10001/0x640/0x650`.)

## Overlap check

Every reservation was intersected with all 608 retained intervals: `inventory.occupied` (bars, bridge windows, reserved unbound descriptors), owned or non-MMIO `inventory.gcd` descriptors, `root_current_allocations`, `unbound_resources` memory descriptors, the 57 `unknown_bars` with conservative positive bounds (32-bit space, or the enclosing parent prefetch window/root allocation for 64-bit BARs), all 149 `firmware_facts.MEMORY` descriptors, the two existing reservations, and the three other DSDT root apertures (PCI0, S0D1, S0D0). Result: zero collisions for both cards; both reservations lie entirely inside the single unowned GCD MMIO descriptor `0x47e20200000-0x63cefffffff`; neither touches the cold 47/48 windows at `0x63cf0000000-0x63cfdffffff` or the root 40 current window; the two reservations do not overlap each other; all bases are 64 GiB aligned and BAR3 sits at the next 64 GiB boundary.

An interrupted earlier attempt had pinned `0x60000000000/0x61000000000` and `0x62000000000/0x63000000000`. That alternative is also collision-free (`alternative_for_record.clear: true`) but does not follow the root00 construction (it sits 51.7 GiB below the existing root 40 window instead of at the aperture floor); it was replaced, and the offline placement checker in alternative mode keeps it evaluable.

## What the writer enforces live (unchanged checks, now per root)

`coordinator/pins/resource_validate.h`: the port must be on bus `0x40` with secondary = subordinate = the card's bus; the port's bridge windows must equal the cold images above; the endpoint's cold BAR0/BAR1/BAR3 must equal the retained values and sit inside the port's windows; the UEFI root bus descriptor must start at BRB `0x40` and stay within 40-7f; the reservation must lie inside the S0D2 declared aperture; the two S0D2 targets must resolve to the same UEFI root domain and to a different one than 01/02; and every memory/root/query/bridge interval of all 112 functions must miss all four reservations. The four GCD reservations are taken in order 01, 02, 47, 48 with cumulative map comparison (masks 1, 3, 7, 15) and released in reverse only when nothing was stored.

## Limits

Trusted-firmware aperture (DSDT declarations), not measured DF decode. No fresh GCD/EFI map after the last boot: the writer's own live capture is the authority. ESXi retention of S0D2 placements is unproven until a physical run (01/02 retention on PCI0 is proven).
