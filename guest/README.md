# cmp-bar1-repair: CMP 170HX 64 GiB BAR1 repair hook (ESXi guest)

ESXi passthrough resets the GPU-internal ReBAR vendor override of every CMP
170HX at VM power-on. The host keeps the 64 GiB BAR1 allocation and the guest
sees a 64 GiB `resource1`, but the GPU's config mirror page (BAR0 + 0x88000)
returns to the reset profile and NVML reports 256 MiB BAR1 after the driver
loads. The proven repair (guest `0000:0d:00.0`) is two 32-bit
stores into that page while the nvidia driver is **not** bound:
`0x724 = 0x30`, then `0xdcc = 0x8000000a`. A normal driver load afterwards
gives 65536 MiB BAR1 (60 GiB CUDA tests passed).

`cmp-bar1-repair` does exactly that, for every CMP that needs it, before every
nvidia module load, idempotently and fail-safe.

## Files

| Path | Purpose |
|---|---|
| `/usr/local/sbin/cmp-bar1-repair` | the tool (Python 3, stdlib only); installed copy of `cmp-bar1-repair` in this directory |
| `/etc/modprobe.d/zz-cmp-bar1-repair.conf` | `install nvidia /usr/local/sbin/cmp-bar1-repair; /sbin/modprobe --ignore-install nvidia $CMDLINE_OPTS` |
| `/var/lib/cmp-bar1-repair/<utc>-<bootid>.json` | one JSON journal per run: mode, boot id, who invoked it, per card the 15 words before/after, action, reason, stores issued |
| `/run/cmp-bar1-repair.lock` | exclusive flock held for the whole run; udev coldplug fires the hook once per NVIDIA function (five within two seconds on the 2026-10-18 cold boot). Blocking wait, 60 s timeout, expiry = REFUSE the whole run |
| `/run/cmp-bar1-repair/journal/` | fallback journal dir, used only if `/var/lib` is not writable (read-only root at early coldplug) |
| journald, tag `cmp-bar1-repair` | every line the tool prints: `journalctl -t cmp-bar1-repair` |
| `tests/test_cmp_bar1_repair.py` | fake-sysfs unit tests (no hardware) |

This directory is the plain-file source of
truth; it is not a git checkout. After editing, reinstall with
`install -m 0755 cmp-bar1-repair /usr/local/sbin/cmp-bar1-repair` and compare
`sha256sum` of both copies.

## What it does on every nvidia load

Every path that loads `nvidia` through libkmod's probe (`modprobe`, udev's
kmod builtin at coldplug, `systemd-modules-load`, the kernel's `request_module`
via `/sbin/modprobe`, `nvidia-modprobe` used by nvidia-persistenced / NVML /
the CDI refresh) runs the `install` command first. `insmod` and
`modprobe --ignore-install` bypass it by definition. When `nvidia` is already
loaded, libkmod does not run the install command at all.

For each PCI function with vendor `0x10de` and device `0x20c2` or `0x2082`:

1. `resource0` must be a 16 MiB regular file. `resource1` size is read from
   sysfs; if it is not exactly 64 GiB (68719476736 bytes) the card is
   `SKIP host BAR1 small` (words are still read and logged, nothing written).
2. In a live run the `driver` link must be absent, otherwise
   `SKIP driver <name> bound` (nothing is even mapped), and the nvidia module
   must not be loaded (`/sys/module/nvidia/initstate` absent), otherwise
   `SKIP nvidia module loaded` unless `--allow-module-loaded` is given (manual
   unbind-one-card flows). `--check` allows both.
3. PCI COMMAND / PCIe DEVSTA are read from the sysfs `config` file. On a cold
   boot the kernel leaves a driverless function with memory decode off
   (COMMAND.MEM = 0) and every BAR0 read returns `0xffffffff` (seen on boot
   `90968ea6`, all four cards; the tool refused correctly but could not
   repair). A live run on a 64 GiB card therefore writes `1` to the sysfs
   `enable` attribute (`pci_enable_device`: MEM decode only, never bus
   master), re-reads COMMAND and requires MEM=1, BME=0 before mapping; it
   journals `decode_enabled_by_tool` and leaves decode on (the driver enables
   it anyway). If the enable write fails, decode stays off or BME comes on:
   `REFUSE`, nothing mapped. Small-BAR1 cards with decode off are skipped
   without an enable write. `--check` / `--dry-run` never write `enable`;
   they report `decode off:` (`DECODE_OFF`, counts as not-OK for `--strict`).
   `setpci` is never used.
4. Exactly one 4 KiB page, `resource0` offset `0x88000`, is mmapped
   `PROT_READ` and the 15 words are read with single aligned 32-bit loads and
   logged.
5. Decision on words 11..14 (CAP, CTRL, CYA, CFG):
   * restored profile: `OK already restored`, zero writes;
   * reset profile (CAP `0x400`, CTRL `0x601` or `0x801`, CYA `0x26`, CFG `0x80000000`)
     **and** word 0 == `0x20c210de` (or `0x208210de`) **and**
     the structure words match (`bb0=00010015`, `bb8=00000460`; words `004`,
     `078` and `010..020` are informational, see below) **and** the physical
     BAR1 words `014/018` describe a 64-bit prefetchable BAR on a 64 GiB-aligned
     base **and** COMMAND has MEMORY on and BUS_MASTER off in the `config` file and
     in mirror word 1 **and** no CorrErr/NonFatal/Fatal/UnsupReq/TransPend bit
     in `config` DEVSTA or mirror word 3: repair (below);
   * anything else (half-applied, unknown ID, `0xffffffff` / `0xbadxxxxx`,
     wrong structure, not idle): `REFUSE ...` with the raw words, zero writes,
     next card.
6. Repair: the run journal is written and fsynced first (intent); if that is
   impossible the card is refused. The page is remapped
   `PROT_READ|PROT_WRITE`, re-read and must be unchanged. Store
   `0x724 = 0x30`; re-read; require CYA == 0x30, CAP in {0x400, 0x1ffc00},
   CTRL in {0x601, 0x801, 0x1001}, every other word unchanged, still idle.
   Store `0xdcc = 0x8000000a`; re-read; require exactly the restored profile
   with all other words unchanged and still idle. Log `REPAIRED`.
   If the readback after store 1 is wrong the tool stops **before** store 2
   (`FAILED after write 1`); a wrong readback after store 2 is
   `FAILED after write 2`. Both mean cold recovery (VM power cycle); the tool
   never retries.

The access class enforces the store whitelist itself: only `(0x724, 0x30)`
then `(0xdcc, 0x8000000a)`, at most once each, in that order, on a writable
map. COMMAND, ReBAR CTRL, BAR address registers and every other offset are
never written. Nothing is written in `--check` / `--dry-run`.

Exit status is 0 in every case (the driver must load so the box stays
usable). Only `--check --strict` / `--dry-run --strict` exit 1 (any card not
`OK` or `SKIP`, no CMP found, or lock timeout).

Serialisation: every run (live, `--check`, `--dry-run`) takes an exclusive
flock on `/run/cmp-bar1-repair.lock` and holds it until exit; a second
concurrent invocation waits and then finds `OK already restored`. If the lock
is still busy after 60 s (`--lock-timeout`) the run REFUSES without touching
any card. Note the lock covers the tool only, not the `modprobe` that follows
it in the install line; the module-loaded guard above closes most of that
window (a second run that starts while the first run's modprobe is loading
nvidia sees the module and skips).

## The 15 monitored words

Page offsets, in order: `000 004 078 080 010 014 018 01c 020 bb0 bb8 bbc bc0 724 dcc`
= ID, COMMAND/STATUS, PCIe cap header, DEVCTL/DEVSTA, BAR0, BAR1 lo, BAR1 hi,
BAR3 lo, BAR3 hi, ReBAR ext-cap header, ReBAR ctrl (BAR0), ReBAR CAP (BAR1),
ReBAR CTRL (BAR1), CYA, CFG.

Words 4..8 are the **physical (host)** BAR address words of that card. They
differ per card and from the guest sysfs addresses; the tool never compares
them with guest addresses, only requires them unchanged across the two stores
and, before a repair, that `014/018` form a 64-bit prefetchable BAR on a
64 GiB-aligned base (a real 64 GiB host allocation cannot be anything else).

Pinned structure words: `000` (device ID per device), `bb0` (ReBAR extended
capability header `0x00010015`), `bb8` (BAR0 ReBAR control `0x00000460`).
Informational (logged, never pinned): `004` COMMAND/STATUS (checked only via the
explicit MEM/BME/status-error rules), `078` PCIe DEVCTL/DEVSTA (the driver
programs max payload / read request / relaxed ordering / no-snoop; `0x00020010`
before, `0x0002c810` after the Gen2 driver's retrain on a later cold VM boot),
`010..020` physical BAR words. All of them must still be unchanged across the
two stores.

| profile | CAP (`bbc`) | CTRL (`bc0`) | CYA (`724`) | CFG (`dcc`) |
|---|---|---|---|---|
| reset (device thinks small; repaired only when host BAR1 is 64 GiB) | `0x400` | `0x601` or `0x801` | `0x26` | `0x80000000` |
| restored (after the two stores) | `0x1ffc00` | `0x1001` | `0x30` | `0x8000000a` |

`0x601` is the firmware-time CTRL the EFI capture showed before any write (and
what the three 64 MiB-host CMPs show today); `0x801` is what ESXi's power-on
reset left on the first repaired card. Any other CTRL with reset CAP/CYA/CFG is
refused. (1.0.1.)

Measured with the driver bound:
`0d:00.0` restored (`010=f3000000 014=0000000c 018=00000640 01c=0000000c
020=00000650`), the other three at the 64 MiB native profile. Words
`bb0=00010015 bb8=00000460` are identical on all four cards.

## Running it by hand

```sh
cmp-bar1-repair --check            # read-only report, fine with the driver bound and services running
cmp-bar1-repair --check --strict   # same, exit 1 unless every CMP is OK or SKIP (for monitoring)
cmp-bar1-repair --check --json     # also dumps the run record
cmp-bar1-repair --dry-run          # read-only, but applies the live gates (driver unbound, idle)
cmp-bar1-repair                    # live run: what the modprobe hook executes
cmp-bar1-repair --allow-module-loaded   # live run with nvidia loaded but the card unbound (manual flows only)
cmp-bar1-repair --lock-file F --lock-timeout S --modules-dir D   # test / special-case overrides
journalctl -t cmp-bar1-repair      # all lines of all runs
ls -la /var/lib/cmp-bar1-repair/   # one JSON per run
```

Expected `--check` today: `0000:0d:00.0 OK already restored (...)` and three
`SKIP host BAR1 small (resource1=67108864 bytes, not 64 GiB)` lines, each
preceded by its `words` line with 15 `off=value` pairs.

## Verifying the hook

```sh
modprobe -c | grep -i "install nvidia"
modprobe --show-depends nvidia          # first line must be the install command
modprobe --show-depends nvidia_uvm      # dependency path goes through it too
lsinitramfs /boot/initrd.img-$(uname -r) | grep -i nvidia   # no nvidia.ko: no early-boot bypass
```

`update-initramfs` copies `/etc/modprobe.d/*` into the initramfs; that is
harmless as long as `nvidia.ko` is not in the initramfs (it is not today).
If it ever is, the hook cannot run there (no python, no tool) and the install
command would fail inside the initramfs; report that rather than editing the
initramfs.

## Removing it

```sh
rm /etc/modprobe.d/zz-cmp-bar1-repair.conf      # hook gone; next nvidia load is a plain modprobe
modprobe -c | grep -i "install nvidia"           # must print nothing
rm -f /usr/local/sbin/cmp-bar1-repair            # optional
rm -rf /var/lib/cmp-bar1-repair /run/cmp-bar1-repair   # optional, deletes the journals
```

No module, service or initramfs change is needed to remove it.

## Tests

```sh
nice -n 19 python3 tests/test_cmp_bar1_repair.py -v
```

39 tests against a fake sysfs tree (sparse `resource0`/`resource1`, synthetic
`config` with a PCIe capability at 0x78, optional `driver` symlink). The
GPU's response to the stores is emulated by wrapping `MirrorPage.write32` in
the tests. Covered: reset profile -> two stores in order with the intermediate
readback check (CTRL 0x801 and 0x601); restored -> zero writes; small `resource1` -> skip; unexpected
words (12 variants, every other CTRL value) -> refuse, zero writes; write-1 readback failure (7
variants) -> stop before write 2; write-2 failure -> cold-recovery report;
driver bound -> skip without mapping; not idle (config and mirror) -> refuse;
mixed fleet; journal unavailable -> refuse before any store; `--check` is
read-only with the driver bound; `--strict` exit codes; `--dry-run`; the
store whitelist in the access class; the CLI under `python3 -I`. 1.1.0 adds:
all-ones page + decode off -> sysfs enable -> two stores (event order
asserted); enable write fails / decode stays off / BME on after enable / page
still all-ones after enable -> refuse without stores; `--check` and
`--dry-run` report decode off without writing `enable`; small card with decode
off skips without enable; nvidia-module-loaded guard and
`--allow-module-loaded`; two concurrent runs (threads, delayed first store):
one repairs, the other waits on the lock and finds restored, exactly two stores;
lock timeout refuses the whole run (rc 1 under `--check --strict`). 1.1.1
adds: restored and reset profiles with `078=0x0002c810` (OK / two stores),
word `000` or `bb0`/`bb8` changed -> refuse, informational words differing ->
still repaired, BAR1 not 64 GiB-aligned -> refuse.

## History

* 1.0.0 (2026-10-18 22:54Z): initial install; `--check` showed `0d:00.0` restored, three cards SKIP.
* 1.0.1: reset profile accepts CTRL 0x601 as well as 0x801 .
* 1.1.1: word `078` (PCIe DEVCTL/DEVSTA) no longer pinned after the Gen2
  driver (srcversion CB9E8798742A0DE871DFF4E, a later cold VM boot) changed it to
  `0x0002c810` and `--check` refused restored cards; pins are now `000`,
  `bb0`, `bb8`; BAR1 64 GiB-alignment gate on `014/018` added.
* 1.1.0: cold-boot fix (sysfs `enable` when decode is off), single flock
  `/run/cmp-bar1-repair.lock` held for the whole run with 60 s REFUSE, nvidia
  module-loaded guard. Triggered by a cold VM boot where
  all four cards read `0xffffffff` at the hook, zero writes, driver came up
  with 256 MiB on the two enlarged cards (`0d:00.0`, `0d:01.0`).

