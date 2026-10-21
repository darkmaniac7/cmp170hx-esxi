# CMP 170HX patches for NVIDIA open GPU kernel modules 580.178.04

Four patches against NVIDIA's open-gpu-kernel-modules, tag `580.178.04`. They
let the CMP 170HX boot GSP-RM with its compute and memory configuration
unlocked, retrain its PCIe link, and add two opt-in experiments: an ECC
hardware recipe and an SM reconfiguration override.

All CMP code is gated on PCI device IDs 0x20C2 (CMP 170HX 8 GB) and 0x2082
(CMP 170HX 10 GB). Every other GPU takes the stock path. Only the 8 GB card
(10de:20c2) has been tested; the 0x2082 path is coded but untested.

## Patches, in order

1. `0001-Backport-minimal-CMP-compute-and-memory-core-to-580..patch`
   Minimal CMP compute and memory core. During GSP boot it opens the FEAT,
   FBPA, WPR and WPR_CFG PLMs through a SEC2 post-Booter payload, writes the
   SS0/SS1 overrides and the per-SKU FBPA_CFG1 / MMU LMR memory geometry,
   checks that result, rebuilds the stock signature and refreshes the WPR
   metadata. It validates the FB region table GSP reports before extending
   its reserved tail, keeps the BAR0 PRAMIN window at the stock offset, and
   runs CeUtils and the scrubber without virtual mode or compressible PTE
   kinds. No parameter: always active on the CMP device IDs. Logs as
   `SEC2_DEBUG` lines.

2. `0002-Port-the-CMP-170HX-PCIe-link-speed-retrain-with-a-ta.patch`
   PCIe link-speed retrain inside the unlock window. Module parameter
   `NVreg_CmpPcieTargetGen`: PCIe generation to request, 1 to 4, default 2.
   0 disables the retrain; values above 4 also disable it and are logged.
   Logs as `CMP580: pcie retrain` lines.

3. `0003-Add-the-CMP-170HX-ECC-hardware-recipe-experiment-beh.patch`
   ECC hardware recipe, experimental. Registry DWORD `CmpEccRecipe`, default
   0 (nothing is written). 1 applies the 14 register writes from the hardware
   half of cmpunlocker v0.5 `ecc-enable.patch`, followed by its WPR/VPR/lock
   teardown. No ECC reporting code is changed: RM, NVML and nvidia-smi report
   whatever the hardware returns. Logs as `CMP580: ecc recipe` lines.

4. `0004-Port-cmpunlocker-PR-55-SM-reconfiguration-override-b.patch`
   SM reconfiguration override, experimental, a port of cmpunlocker PR #55.
   Registry DWORD `CmpSmReconfig`, default 0 (nothing is written or logged).
   1 opens RECONFIG_PLM (0x8200d8) and writes 0xff to the per-GPC TPC
   reconfiguration overrides (0x820a40 + 4*gpc). Only TPCs held back by the
   RECONFIG mask can come back; defective and floor-swept TPCs stay off.
   Upstream measured 70 to 74 SMs on a 0x20C2 card. Runs after the PCIe
   retrain and before the ECC recipe. Logs as `CMP580: sm reconfig` lines.

## Module parameters

`CmpEccRecipe` and `CmpSmReconfig` are meant to be set per device through
`NVreg_RegistryDwordsPerDevice` (they are also exposed as the global module
parameters `NVreg_CmpEccRecipe` and `NVreg_CmpSmReconfig`). Example
`/etc/modprobe.d/nvidia-cmp.conf`, one `pci=` block per card:

```
options nvidia NVreg_CmpPcieTargetGen=2
options nvidia NVreg_RegistryDwordsPerDevice="pci=0000:XX:00.0;CmpEccRecipe=1;CmpSmReconfig=1;pci=0000:YY:00.0;CmpEccRecipe=1;CmpSmReconfig=1"
```

If the nvidia module is loaded from the initramfs, run `update-initramfs -u`
after changing these options. Check the result with
`dmesg | grep -E 'CMP580|SEC2_DEBUG'`.

## Applying

```
git clone https://github.com/NVIDIA/open-gpu-kernel-modules.git
cd open-gpu-kernel-modules
git checkout 580.178.04
git am /path/to/000*.patch
make modules -j"$(nproc)"
```

`git am` prints one whitespace warning for patch 1 (a blank line added at the
end of `mem_mgr.h`); it is harmless.

Install the five `kernel-open/*.ko` modules in place of the ones the system
loads (`modinfo -n nvidia` shows the path; on Ubuntu with
`nvidia-dkms-580-open` it is `/lib/modules/$(uname -r)/updates/dkms/`, stored
zstd-compressed), then run `depmod -a`. The userspace driver and GSP firmware
must be the same version, 580.178.04.

## Test environment

- Ubuntu 24.04 LTS, kernel 6.8.0-146-generic
- 4x CMP 170HX 8 GB (10de:20c2), PCI passthrough under VMware ESXi 8.0.3
- Ubuntu NVIDIA 580 packages, version 580.178.04-1ubuntu1, with the
  `nvidia-dkms-580-open` modules replaced by the patched build
- The test machine also restores a 64 GiB BAR1 vendor override before the
  nvidia module loads. That step is not part of these patches.

## Upgrades

A kernel upgrade or an NVIDIA package upgrade replaces these modules: DKMS
builds stock modules for a new kernel, and a new `nvidia-dkms-580-open`
overwrites the patched ones. Hold the packages, for example:

```
sudo apt-mark hold linux-generic linux-image-generic linux-headers-generic \
  nvidia-dkms-580-open nvidia-kernel-source-580-open nvidia-kernel-common-580 \
  nvidia-firmware-580 nvidia-compute-utils-580 libnvidia-compute-580
```

Rebuild and reinstall the patched modules before taking any held upgrade.

## Provenance and license

These patches are derived from cmpunlocker
(https://github.com/amoghmunikote/cmpunlocker), release v0.5, including PR #55
"Add +4 more SMs" by Amogh Munikote (commit 6c442ee). The PCIe retrain in
patch 2 follows `_cmpRetrainGen2` from the bayley/cmpunlocker fork
(https://github.com/bayley/cmpunlocker). cmpunlocker is GPL-2.0, so these
patches are GPL-2.0 as well; see `LICENSE`. NVIDIA's open-gpu-kernel-modules
itself is dual MIT/GPL-2.0.

The ECC recipe and the SM override write undocumented registers. Both are off
by default. Use them at your own risk.
