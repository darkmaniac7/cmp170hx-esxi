# CMP 170HX on ESXi 8: the whole story

Four CMP 170HX (the 8 GB 10de:20c2 cards, Hynix HBM, VBIOS 92.00.67.00.01) passed through from VMware ESXi 8.0.3 into one Ubuntu 24.04 VM, next to two RTX PRO 6000 Blackwells and an L4 on the same guest. ASRock Rack ROMED8-2T, EPYC 73F3, 512 GB, slots bifurcated x8/x8 so every card sits behind its own AMD root port. Everything below was measured on that box in October 2026. Where I could not find a public report of something I say so, I don't claim firsts.

This is a dossier, not a tool. The tools and the driver patches that came out of it are in this repo (see the end). All credit for the unlock itself goes to amoghmunikote/cmpunlocker. I just dragged it into a hypervisor and kept notes.

## TL;DR

| What | Status on ESXi 8 passthrough | Notes |
|---|---|---|
| 64 GB VRAM unlock, inside the guest | works | cmpunlocker v0.5 core ported onto open modules 580.178.04. Does not need a big BAR1. |
| 64 GiB BAR1 | works, but ESXi will never do it for you | needs a tiny UEFI app that runs before ESXi on every host boot, plus a guest-side hook on every VM power-on |
| PCIe Gen2 | works, guest-side retrain is enough | cards come up Gen1 x8 stock |
| PCIe Gen3 | fused off on these 8 GB cards | the SW fuse override path is itself fused off. One attempt via the XP/XVE registers left the card at Gen1 until a power cycle. Don't. |
| 74 SMs | works | port of upstream PR #55 onto 580 |
| ECC | the hardware recipe does take, with logs | readout bit flips, master enable reads 0x41, pattern tests pass. Still no counters, no scrub, no proof of correction. |
| P2P between CMPs in the VM | no | static BAR1 P2P init fails or wedges UVM in the guest. Same cards do 12/12 pairs on bare metal. Blackwell to Blackwell P2P in the same VM is fine. |
| Serving | 4 cards run GLM-5.3-Flash W4A16 at recipe-book parity in PP4, about 0.75x in TP4 | numbers in section 7 |

## 1. The unlock itself, inside an ESXi guest

Upstream's "Virtual Passthrough" route does the unlock on the host, because under QEMU/VFIO the in-guest Booter path comes back with `0x15` (ACR_ERROR_FLCN_REG_ACCESS) and the guest ends up on a stock driver. Under ESXi I never saw that. The whole v0.5 core runs inside the guest and all four cards report 65,536 MiB. You do get 48 `Booter failed ... 0x31` lines per boot. They are noise, not a failure.

Why 580.178.04 and not the pinned 610/615: this VM also holds two Blackwells. With nvidia-open 615.71.09 plus cmpunlocker the Blackwells hit `krcWatchdog: GPU is probably locked` about 14 s after a second CUDA app initialised on a card that was mid-prefill, and then UVM wedged (every new GPU process hangs at exit in `uvm_va_space_destroy`, dozens of nvidia-smi pollers in D state, no Xid, only a reboot clears it). NVIDIA's 615.71.09 notes list that symptom as a known issue. 580 was what the rest of the fleet already ran, so I ported the unlock down instead of moving everything up.

What the port is: the six v0.5 core patches (`sec2-postbl-plm-ss-cfg`, `booter-verify`, `late-pma`, `bar0-pramin-clamp`, `ce-scrub-workarounds`, `persistent-sw-state`) re-applied by hand onto pristine 580.178.04 sources, plus lifecycle guards. The SEC2 post-Booter payload is gated on the two CMP device IDs and opens the four protection domains with required read-back after each write (WPR_CFG `0x1fa7cc`, FBPA `0x9a0148`, WPR `0x1fa7c4`, FEAT `0x823804`). The signature buffer is exactly `0xf800` bytes, filled only after the descriptor size checks out, and swapped back to the saved stock signature for the normal GSP boot. The phase-1 port deliberately left out BAR1 resize, Gen2, ECC, the SKU/name patches and the extra-SM loop, so the first boot only had the memory and compute unlock to blame if anything went wrong. Those came back one at a time afterwards (sections 3 to 5) as module parameters, each default off.

Acceptance: small CUDA arithmetic on all four, then 60 GiB allocations written and verified word by word with two address-dependent patterns, on every card. The VRAM unlock works with the 64 MiB BAR1 ESXi gives you. BAR1 only matters for P2P and static mappings.

One boring but important thing: the patched modules sit over the DKMS build. A kernel or nvidia package upgrade silently puts stock back and the cards drop to 8 GB. Hold the kernel and every nvidia package, and identify modules by sha256, not srcversion.

## 2. 64 GiB BAR1 under ESXi

### Why ESXi can't do it

At every host power-on the card advertises a 64 MiB BAR1: ReBAR CAP at config `0xbbc` reads `0x400` (only the 64 MiB bit) and CTRL at `0xbc0` reads `0x601`. Firmware sizes the BAR to that, ESXi enumerates whatever firmware assigned, and ESXi never writes the vendor override. Broadcom's VMDirectPath notes say resizable BAR is unsupported for passthrough and that devices get reset at VM power-on. Raising `pciPassthru.64bitMMIOSizeGB` only reserves guest address space; it cannot grow the physical BAR. Upstream's `bar1-resize-unlock.patch` (write the two override DWORDs, then let Linux resize with `NVreg_EnableResizableBar`) is a Linux-host path. In a VM the BAR is fixed by the hypervisor before your driver exists.

### What worked: resize it before ESXi boots

A small UEFI application, booted once from USB (IP-KVM virtual media, BMC one-time boot override), that runs after firmware PCI enumeration and before the ESXi loader. Per card, through `EFI_PCI_IO_PROTOCOL.Pci.Write`, each store read back:

1. COMMAND on the card and on its parent port: MEM and BME off (and no transactions pending)
2. CYA `0x724 = 0x30`
3. CFG `0xdcc = 0x8000000a`. After this, CAP `0xbbc` goes `0x400 -> 0x1ffc00`
4. ReBAR CTRL `0xbc0`: size code 16 (64 GiB), result `0x1001`
5. BAR1 low/high, then BAR3 low/high (BAR3 is 32 MiB at base + 64 GiB)
6. the parent port's prefetchable window base/limit plus the upper 32 bits
7. restore the parent port's COMMAND. The card itself is left with MEM/BME off for the OS to enable.

Before the first store the 64 GiB + 32 MiB span (`0x1002000000`) is reserved in the firmware GCD at 64 GiB alignment, inside the root bridge aperture taken from the DSDT `_CRS` windows. Then the app does `LoadImage`/`StartImage` on the existing ESXi loader (`BOOTX64.EFI` is VMware's safeboot, which loads `mboot64.efi`) with the original load options, no firmware reset. The whole thing adds about 3.5 to 4 minutes to a host boot.

No retries, no rollback, no NVRAM writes inside the app. Any failure after a possible store is a cold recovery: detach media, clear the override, power off, wait ten seconds, power on, verify four small BARs, then try again.

Two gotchas that cost me boots:

- The first writer version compared CTRL to its original whole value after the CFG write and stopped at write 4. The CYA store alone already moves CTRL `0x801 -> 0x1001` and the CFG store expands CAP to `0x1ffc00`. Expect those exact values (or mask the size field `0x1f00`), don't expect "unchanged".
- The extended capability chain on these cards (`100 -> 250 -> 258 -> 128 -> 420 -> 600 -> 900 -> bb0`) has a null capability (ID 0) at `0x900`. A strict decoder rejects the walk and never finds the ReBAR block at `0xbb0`.

Result: first two cards, then all four, with physical BAR1 at `0x64000000000`, `0x66000000000`, `0x48000000000` and `0x4a000000000`, BAR3 32 MiB right behind each. ESXi's own view (`vsish ... BARInfo`) says `Size:68719476736`, the VM log says `PCIPassthru: Device 0000:01:00.0 barIndex 1 realaddr 0x64000000000 size 68719476736`, guest `resource1` is 64 GiB, NVML says 65536 MiB BAR1.

### VM config

`pciPassthru.use64bitMMIO = TRUE`, and `pciPassthru.64bitMMIOSizeGB` had to go from 384 to 1024. With one 64 GiB card the guest EFI already refused 67,239,936 KiB of MMIO at 384 (the arithmetic fit, the placement didn't). Four cards need about 544 GiB of 64-bit BAR payload with the Blackwells' 128 GiB BAR1s next to them, so 512 can't work either. 1024 is just the next power of two.

### The part that bit me in the guest: 256 MiB

After the host had 64 GiB BAR1s, the first VM boot showed `resource1` = 64 GiB in sysfs but NVML said BAR1 Total 256 MiB. Both guest config space and the BAR0+`0x88000` config mirror read CAP `0x400`, CTRL `0x801` (size 8 = 256 MiB, which CAP doesn't even advertise), CYA `0x26`, CFG `0x80000000`. Every VM power-on resets the vendor override while the host allocation survives. GSP then reports what the ReBAR block says, and RM's BAR1 window follows GSP.

Fix: a modprobe `install nvidia` hook that runs before nvidia.ko loads. It mmaps one 4 KiB page of `resource0` at `0x88000`, checks it is looking at the reset profile (ID `0x20c210de`, ReBAR header `0x00010015`, CAP `0x400`, CTRL `0x601` or `0x801`, CYA `0x26`, CFG `0x80000000`), writes `0x724 = 0x30`, re-reads, writes `0xdcc = 0x8000000a`, and requires the restored profile (CAP `0x1ffc00`, CTRL `0x1001`) on read-back. Guest config-space writes are virtualised and don't reach the device, which is why it goes through the mirror. Three things it learned the hard way:

- a cold VM boot leaves memory decode off, so the mirror reads `0xffffffff`. Write `1` to sysfs `enable` first (MEM only, never bus master)
- udev runs the install hook once per NVIDIA function, concurrently. Take a lock or you get a race on the override
- it always exits 0 so the driver still loads, and it never retries. A wrong read-back after a write means a VM power cycle, not a second attempt

Since then every VM boot logs four repairs at first load and NVML says 65536 MiB on all four.

### Persistence

Every host reboot comes back cold, so the writer has to run on every host boot. What I ended up with: leave the writer image attached read-only on the IP-KVM, set the BMC to boot UEFI/USB first (Continuous), and let it fall through to the ESXi entry when no USB target exists. Set the override back to Disabled and detach the media before patching ESXi, because a changed loader makes the writer refuse. Copying the writer onto the ESXi ESP with its own boot entry would be cleaner, but ESXi patches rewrite the ESP and I haven't rehearsed that.

## 3. PCIe Gen2, and why Gen3 is dead on the 8 GB cards

Stock, these cards train Gen1 x8 here: LnkCap advertises 2.5 GT/s only, LnkCap2 lists only Gen1, while the AMD root ports are Gen4 x8 and already target Gen4. cmpunlocker's Gen2 retrain (`_cmpRetrainGen2`: XP_CFG0 `0x8c040`, XP_LCTRL2 `0x8c1c0`, done inside the PLM unlock window) ported onto 580 as a module parameter `NVreg_CmpPcieTargetGen` (default 2, 0 disables) brings all four to Gen2 x8 inside the VM. The root-port LnkCtl2 write is a no-op in a VM (you are writing VMware's virtual bridge) and turns out to be unnecessary. Guest dmesg: `pcie retrain done gen=2 ... speed=2 width=x8`; the host's lspci afterwards shows LnkCap 5 GT/s x16, LnkSta 5 GT/s x8.

Gen3. Read-only fuse dump on all four cards, identical on every one:

| Register | Value | Meaning |
|---|---|---|
| `0x820084` DISABLE_SW_OVERRIDE_STATUS | 1 | software fuse override permanently disabled |
| `0x820040` EN_SW_OVERRIDE | 0 | |
| `0x820250` OPT_DISABLE_GEN3_SPEED | 1 | |
| `0x820580` OPT_PCIE_BOOT_GEN3_DIS | 1 | |
| `0x82057c` GEN23_DIS | 1 | |
| `0x820520` | Gen4 disable bit set | |
| `0x820228` OPT_ECC_EN | 0 | |

The Gen3 recipes going around (the probe-time software fuse override that clears `0x820250` and `0x820580` after setting EN_SW_OVERRIDE) check `0x820084` bit 0 first and skip when it is set. On these 8 GB cards it is set, so that path is closed before it starts. That matches the reports that only the 10 GB 2082 cards take the Gen3 unlock.

I tried the other route once, on one card: the XP/XVE registers (`XVE_CYA_SPD 0x8841c`, `XP_CYA0 0x8c2c0`, XVE LnkCtl2 `0x880a8`, XP_LCTRL2) plus patching the LnkCap value RM caches for GSP (`0x00456101 -> 0x00456103`) and `RmForceEnableGen2=1`, then the RM speed sequence. Eight of nine writes took (the XVE LnkCtl2 target 3 was refused), the CYA_SPD write clipped the host-visible LnkCap to 2.5 GT/s max, `SET_PCIE_SPEED(1)` succeeded, `SET_PCIE_SPEED(2)` failed with status `0x40`, and the card sat at Gen1 x8 (`LnkSta 0x1081`) until a VM power cycle re-ran the Gen2 retrain. No Xid, memory pattern test still passed. Gen2 x8 is the ceiling on 20c2. If you need more between cards, that is x16 slots and bare metal, not registers.

## 4. 74 SMs

Upstream PR #55 ("Add +4 more SMs") ported onto 580 as a per-device registry DWORD `CmpSmReconfig`. It opens RECONFIG_PLM (`0x8200d8`) through the SEC2 post-Booter payload while the unlock window is open, writes `0xff` to the eight per-GPC TPC reconfiguration overrides (`0x820a40 + 4*gpc`) and logs the per-GPC TPC status (`0x820c38 + 4*gpc`) with every write and read-back. Only TPCs that RECONFIG held back come back; defective and floor-swept ones stay off. Every card here logs `taken=8 mismatched=0 ... changed=2 TPC 35->37` and CUDA reports 74 SMs.

One thing from the field: torch.compile/inductor and Triton caches are keyed to the SM count. Move them aside when the count changes or you get a confusing first boot. The first boot per layout then costs about ten extra minutes of compiles.

Effect on serving: decode-neutral, plus 2 to 5 percent on pipeline-parallel prefill. Don't buy the card for it, but it's free.

## 5. ECC: what actually takes, and what still isn't shown

I applied only the hardware half of upstream's ECC patch, no reporting override, as a per-device DWORD `CmpEccRecipe=1`, inside the SEC2 window: `0x823800 = 0xffffffff` (the feature-override PLM), `0x82380c = 0x00999999`, `0x823810 = 0x003ffebf`, `0x82382c = 0xf`, then `FBPA_ECC_CTRL 0x9a0470 |= 0x41`, `0x9a1004 |= 0x200`, `0x9a033c |= 0x3`, `0x9a0084 |= 1`, and the two LTC bits `0x17e318 |= 1`, `0x17e21c |= 1`. Fourteen protected writes in all, each read back. First on one card with the other three as same-boot controls, then on all four.

What the logs say, on every card: `ecc recipe done: taken=14 mismatched=0 invalid=0 of 14` and `readout enablement before=disabled after=enabled`. `FEATURE_READOUT 0x823814` goes from `0x00000233` to `0xec8ff233` (bit 16, the ECC_DRAM input GSP consumes, 0 to 1). `FBPA_ECC_CTRL 0x9a0470` reads `0x41` afterwards (stock CMP reads 0, a real A100 reads `0x41`). Control cards on the same boot are unchanged. 60 GiB two-pattern tests pass on all four with the recipe active, no init failure, no Xid. So the "readout is POR-latched and can't change at runtime" argument is wrong for these cards. A fresh read shows the bit set.

What is still not shown, and I want to be clear about this: NVML `nvmlDeviceGetEccMode` still returns "not supported" because the `OPT_ECC_EN` fuse (`0x820228`) is 0 and there is no InfoROM ECC object. No counters, no scrubbing, no page retirement, and no demonstration that an HBM error gets corrected or even detected. The nvidia-smi "ECC Enabled" screenshots you see around come from the reporting patch, which prints that regardless of hardware state. The honest summary is: the enable machinery takes, the proof of correction doesn't exist yet. If someone knows the real GA100 DED counter registers, that's the next test.

## 6. P2P between CMPs inside the VM: no

Stock: GSP reports P2P not supported for the CMP IDs, `cudaDeviceCanAccessPeer` is false on all 12 directed pairs, and the mailbox path moves zero bytes. The bare-metal route that works is static BAR1 P2P (`RMForceStaticBar1=1; RMPcieP2PType=1`), and the same four cards, same host booted into native Linux, passed 12/12 directed pairs with real remote loads and stores at 4 KiB, 1 MiB and 16 MiB. So the cards can.

In the ESXi guest it goes like this:

- Attempt 1 never armed. The VMware opt-in in my peer branch was gated on RM's own hypervisor detection. RM only scans the CPUID leaves `0x40000000..` for `VMwareVMware`, no DMI fallback on x86, and this VM runs with `hypervisor.cpuid.v0 = FALSE`, which hides exactly those leaves. So `bDetected` stays false, the opt-in refuses, and because the refusal sat inside the init path after GSP was already booted, every later open re-ran Booter against a running GSP. Lesson: don't gate anything on hypervisor detection in a VM that hides it, and don't flip `hypervisor.cpuid.v0` casually, it changes RM behaviour for every GPU in the VM.
- `RMForceStaticBar1=1` alone fails init: the static map gets placed at offset 512 MiB (there is a mailbox carve-out in front of it) and a 63.6 GiB framebuffer (`0xfe7600000`) from `0x20000000` overruns the 64 GiB BAR1 VA by a few hundred MiB. `kbusEnableStaticBar1Mapping_TU102: Failed to create the static bar1 mapping`, `RmInitAdapter failed`.
- `RMForceStaticBar1=1;RMPcieP2PType=1`: the mailbox is skipped, the static map lands at offset 0, init is clean, NVML shows 64 GiB. Then the first plain CUDA context (an 8 KiB probe, no peer step at all) never exits, and UVM wedges with nvidia-smi stuck in `uvm_va_space_destroy`. Hard VM reset. Twice.

Meanwhile the two Blackwells in the same VM do P2P with each other just fine (`pciPassthru.allowP2P` and `relaxACSforP2P` set, NCCL reports `P2P/CUMEM` between them), so ESXi's peer transport is not the blocker. My reading is that it's the ESXi mapping of a 64 GiB BAR1 under CUDA use, not the cards, but that is a reading, not a root cause. Untested: the TRAP31 mailbox route inside a VM, a static window shrunk to the client's size, and `hypervisor.cpuid.v0 = TRUE`. I could not find any public report of CMP P2P working inside any hypervisor, and a few bare-metal writeups say outright that their setup is incompatible with passthrough.

Serving lives with it: NCCL with `NCCL_P2P_DISABLE=1` and the fork's host-staged all-reduce. That is the whole TP4 gap in section 7.

## 7. What it serves

Engine: the Morrowmake `vllm-cmp170hx` fork (engine `819146023f`, recipe 1.8.0) with GLM-5.3-Flash W4A16 (int4 g128 on the routed experts, everything else BF16) and the DFlash2 drafter, adaptive depth by load (7/5/3), prefix caching on, cards capped at 180 W. Protocol is the recipe book's: structured / code / prose prompts, 400 tokens, temperature 0, one user = median of 5, multi-user = median of 3 aggregates, cold prefill on synthetic 8k and 32k prompts. Numbers are tokens per second, structured / code / prose, with the 74 SM driver.

| Layout | 1 user | 2 users agg | 4 users agg | 8 users agg | 32k cold prefill | KV pool at 262k ctx |
|---|---|---|---|---|---|---|
| 4x CMP, PP4 | 201 / 149 / 97 | 309 / 223 / 183 | 386 / 353 / 300 | 664 / 548 / 477 | 4.7 to 4.8 s (6.8 to 7.0k tok/s) | 2.08M tokens |
| 4x CMP, TP4 | 361 / 284 / 199 | 441 / 371 / 273 | 545 / 468 / 401 | 697 / 587 / 529 | 19 to 20 s (1.6 to 1.75k tok/s) | 1.2M tokens |
| recipe book, PP4 (x16, P2P on) | 232 / 210 / 100 | 344 / 273 / 196 | 409 / 383 / 314 | 664 / 609 / 469 | 7.4k tok/s | 2.09M |
| recipe book, TP4 (x16, P2P on) | 483 / 442 / 232 | 607 / 460 / 315 | 718 / 619 / 501 | 944 / 835 / 667 | 3.2k tok/s | |
| 2x CMP + 2x Blackwell, PP2 x TP2 | 349 / 261 / 180 | 379 / 328 / 239 | 497 / 432 / 355 | 895 / 703 / 611 | 5.95 s | 1.9M tokens |

So: PP4 lands on the book within noise on decode and 6 to 8 percent under on prefill, which means PP4 is not link-bound at Gen2 x8. TP4 sits at about 0.75x on decode and 0.55x on prefill, and that is entirely the all-reduce going through host memory over Gen2 x8 instead of P2P over x16. Drafter acceptance is in the book's range (95 to 99 percent structured, 74 to 89 code, 41 to 48 prose at one user), so the drafter is not the gap. Without the drafter the raw step is 9.9 ms on TP4 (101 tok/s) and 20.8 ms on PP4 (48 tok/s).

The pair-of-pairs line is two CMPs as pipeline stage 0 (12 layers) and two Blackwells as stage 1 (33 layers), mixed sm_80 and sm_120 in one job. It is the best 8-user throughput I measured and leaves two CMPs idle. A 996,784-token prompt prefilled in 206 s on that layout, 211k tokens in 40.6 s. Mixing the two architectures in one vLLM job needed three small Python patches (resolve device capability per rank instead of from device 0, pick the MLA backend on sm_120 without the fp8 KV path, build the host-staged all-reduce only on sm_80 TP groups) and a few launch flags (`kda_prefill_backend: triton`, `CUTE_DSL_ARCH=sm_120a`, FlashInfer autotune off, every pipeline stage must hold at least one MLA layer, Blackwells last).

Deeper speculative decoding on the CMPs doesn't pay: every extra draft token costs about 1.1 ms of verify time per step (21.2 / 23.4 / 25.6 ms at depth 7 / 8 / 10), so depth 7 stays. The same depth 10 runs at 20.0 ms on the Blackwell pair. Memory-bound cards, memory-bound verify.

## 8. Traps, in the order they bit

- One nvidia.ko drives every GPU in the VM. A CMP module swap or wedge takes the Blackwells and the L4 down with it.
- `nvidia-persistenced` is down after a VM power cycle; the NVIDIA container runtime then fails on `/run/nvidia-persistenced/socket`. Start it before any `docker run`.
- Power caps reset to 250 W on every module load. Set 180 W after, not before.
- Only one `NVreg_RegistryDwordsPerDevice` line counts. A second file silently replaces the first.
- During a UVM wedge don't run nvidia-smi or a bug report. Every new caller blocks in D on the RM lock, and `timeout` cannot kill a D-state process. A guest poweroff won't finish either; hard reset the VM, after restoring the known-good modules on disk.
- A sub-0.15 s D state on `os_acquire_rwlock` during KV allocation is benign. A watchdog that aborts on the first sample will kill healthy runs; require persistence.
- Keep nvidia.ko out of the initramfs, and never `insmod` or `modprobe --ignore-install` by hand, or the BAR1 hook is bypassed.
- Never follow a successful 64 GiB host boot with a "normal" reboot to make some baseline check green. You lose the allocation and have to run the window again.

## 9. What's in this repo

- `writer/`: the UEFI BAR writer (two-card and four-card profiles), the placement planner, and the window driver that talks to the IP-KVM and the BMC. Addresses are parameters; nothing in here knows my network.
- `guest/cmp-bar1-repair`: the modprobe install hook, Python 3, stdlib only, with its README.
- `tools/`: the read-only fuse dump, the ECC register verifier with pattern test, the host link-state decoder, and the RM speed-change helper (query mode is safe; do not run the Gen3 sequence on a 20c2).
- `driver/`: the 580.178.04 work as a `git format-patch` series against NVIDIA's pristine 580.178.04 sources: 0001 the v0.5 core port, 0002 the Gen2 retrain parameter, 0003 the ECC recipe parameter, 0004 the SM reconfig parameter. Apply in order with `git am`; 0002 to 0004 are independent features behind parameters that default off. GPL-2.0 like cmpunlocker.

## Credits

amoghmunikote/cmpunlocker for the unlock, PR #55 for the SM reconfig, bayley's driver for the Gen2 retrain reference, Morrowmake for the vLLM fork and the recipe book I benchmarked against, the DFlash2 authors for the drafter, and the 170hx_tips Gen3 notes for documenting the fuse-override gate clearly enough that I could show why it doesn't apply here.
