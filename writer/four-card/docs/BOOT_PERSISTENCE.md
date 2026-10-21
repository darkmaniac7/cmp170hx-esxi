# Running the writer on every host boot (design only; nothing executed)

Why: firmware re-enumerates all four cards cold on every power-on and reboot, so a 64 GiB layout written for 01/02 is lost at the next host boot. The writer is a cold-boot-only tool (it refuses an already enlarged layout), so it must run first on every boot and chainload the unchanged VMware loader. With the fail-open branch, a boot on which the writer cannot prove its preconditions degrades to a normal ESXi boot with 64 MiB BARs instead of a hang, except for the pre-PREPARE cases listed under "Shared failure modes".

Facts both options rest on: the EFI pins Boot0003 (`HD(1,GPT,f5db35a8-468f-e04f-87d0-b8016b3649b2)/\EFI\BOOT\BOOTX64.EFI`, variable bytes pinned) and the loader bytes it chainloads (115,000 B, SHA256 `48eadedc...`, the VMware mboot at that path on the ESXi system partition). The ESP partition GUID, the Boot0003 bytes and the loader hash/size are this host's values and must be regenerated for another host. The current boot block observed on this host was `BootSourceOverrideEnabled=Disabled`, `BootSourceOverrideTarget=None`, `Mode=UEFI`, `BootOrder=[Boot0003,Boot0004,Boot0002]`. A read-only KVM mass-storage device (MSD) carrying a 64 MiB FAT image (`rw=false`, `cdrom=false`) has been observed attached through a complete ESXi boot once, attached before the boot and detached afterward without incident. Console evidence is the BOOTINV lines on the firmware console captured by the remote console recorder.

## Option A: KVM MSD image left attached + BMC boot source USB-first (recommended)

Mechanism: the four-card FAT image stays uploaded and connected on the KVM MSD (read-only), and the BMC makes the firmware try USB before `Boot0003` on every boot. Two ways to express "USB first", in order of preference:

1. Redfish `PATCH /redfish/v1/Systems/Self {"Boot":{"BootSourceOverrideEnabled":"Continuous","BootSourceOverrideTarget":"Usb","BootSourceOverrideMode":"UEFI"}}`. `Continuous` must appear in `BootSourceOverrideEnabled@Redfish.AllowableValues`; check it with a Redfish GET before patching. `Continuous` re-applies the override at every boot and falls through to the normal order when no USB boot target exists.
2. If `Continuous` is not offered: BIOS setup over the BMC remote console, boot option priorities with the "UEFI: USB" class first (AMI creates the removable-media option dynamically from `\EFI\BOOT\BOOTX64.EFI`; the priority class setting persists even though the entry itself is transient).

Exact steps (one supervised window, then unattended):
1. Build the four-card EFI (see the Makefile), obtain the parent's fresh activation binding, place `BOOTX64.EFI` at `\EFI\BOOT\BOOTX64.EFI` on a 64 MiB FAT image, upload it to the KVM MSD, read it back (SHA256 match), then `set_params image=... cdrom=false rw=false`.
2. First four-card run with all VMs off and the host in maintenance: `set_connected true`, boot override `Once/UEFI/Usb`, one host reboot, console recording, eight `vsish` BAR reads now expecting 47 BAR1 `0x48000000000` / 48 BAR1 `0x4a000000000`, guest checks. Do not enable persistence before this run is green.
3. Enable persistence: leave the MSD connected and enabled (skip the detach step); `PATCH` `Continuous/Usb/UEFI`; read back and record the Redfish object and the MSD state.
4. Verify unattended behaviour with one more supervised reboot (same quiesce rules): console must show `WRITER_RUN ... target_count=0x4` then `START_IMAGE`, ESXi returns, BARs 64 GiB on all four.
5. Add a post-boot check on the management host (ESXi API or an equivalent collector) that alerts when any of the four BAR1s is not 64 GiB after a host boot; this is the only way to notice a silent fallback.

Risks and the recovery path for each:
- KVM down, MSD not presented, image missing: firmware falls through to `Boot0003`; ESXi boots with 64 MiB BARs; no hang. Recovery: fix the KVM, reboot in a window. The post-boot check catches it.
- Writer falls open (`HANDOFF_FALLBACK reason=...`): ESXi boots with 64 MiB BARs; read the reason from the console recording; typical causes are an inventory change (new/moved device, firmware update changing AML hashes or the EFI memory map layout) or a GCD surprise. Recovery: investigate, rebuild pins or policy, reboot in a window.
- Writer refuses before PREPARE (wrong host identity, BootCurrent, Boot0003 bytes, loader bytes after an ESXi patch replaced mboot, media drift): terminal `HANDOFF_REFUSED cold_recovery_required=0`, host parked in the EFI. Recovery = the recovery runbook (detach `set_connected false`, `set_enabled false`, Redfish override `Disabled`, journaled IPMI power off, 10 s, verified power on): ESXi boots normally. To avoid this, set the override to `Disabled` before any ESXi patch and re-verify the loader/Boot0003 pins before re-enabling.
- Terminal after a possible write (`write_attempted=1` without `status=4`, `cold_recovery_required=1`, returned loader): cold recovery only, no retry; the override must be set to `Disabled` first so the recovery power cycle boots ESXi.
- Unplanned reboot (power event, PSOD) while the MSD is attached: the writer runs unattended with VMs off by definition of a cold boot; the guest BAR hook on the management host must be installed and `--check` verified so a GPU-passthrough guest comes up correctly without a controller.
- Every boot now takes about 3.5 extra minutes (reboot to `START_IMAGE`), and the ESXi host sees a USB disk; it has run with it attached.
- Fallback for the whole option: detach (`set_connected false`, `set_enabled false`) and `Disabled/None` on the BMC. Nothing on the host changes.

## Option B: BOOTX64.EFI on the ESXi boot ESP as a chain entry

Mechanism: copy the writer onto the ESXi system partition (GPT partition 1, the 100 MiB FAT "BOOT" volume that holds `\EFI\BOOT\BOOTX64.EFI`) under a new path, add a UEFI boot entry for it and put that entry first in `BootOrder`. The writer must NOT replace `\EFI\BOOT\BOOTX64.EFI`: that file is the pinned loader the writer chainloads, so replacing it makes the writer load itself, fail the loader hash and park the host. Install as `\EFI\CMP\WRITER4.EFI`; `Boot0003` stays byte-identical so the pins hold.

Exact steps:
1. Same supervised first run as A, steps 1-2, from the KVM MSD.
2. In the same maintenance window, over ESXi SSH (enabled temporarily): confirm the partition (`partedUtil getptbl` on the boot disk; partition 1 GUID `f5db35a8-...`), locate its mount (the FAT system partition is the one containing `EFI/BOOT/BOOTX64.EFI` with SHA256 `48eadedc...`), `mkdir EFI/CMP`, copy `BOOTX64.EFI` from the build as `EFI/CMP/WRITER4.EFI`, read back and compare SHA256, restore the SSH policy.
3. Create the boot entry. ESXi has no `efibootmgr`; use the firmware setup over the BMC remote console ("Add New Boot Option": file browser on the BOOT partition, `\EFI\CMP\WRITER4.EFI`, name `CMP four-card writer`), then move it to the top of the boot order; or, if this AMI build exposes it, Redfish `Boot.BootOrder` with the new `Boot000X` first. Read `BootOrder` back from Redfish and record it.
4. One supervised reboot as in A step 4.

Risks and recovery:
- ESXi upgrades and patches rewrite the system partition (mboot and the boot variable are maintained by the installer): the loader bytes change, the writer refuses pre-PREPARE and parks the host on the next boot; the installer may also remove `EFI/CMP` or reset the boot order. Recovery is a BMC console session: firmware setup, boot override to `Boot0003` (or Redfish `Once/Hdd`), then delete or fix the entry. Mitigation: before any ESXi patch, move `Boot0003` first again.
- The new `Boot000X` becomes stale if the file disappears; AMI skips missing entries and falls through to `Boot0003`, so this case is safe.
- Fail-open and post-write terminal cases are identical to A, but recovery from a parked host requires changing the boot order from the console instead of detaching media.
- Writing to the production boot disk and editing NVRAM boot variables by hand are new operations without a rehearsal; the KVM path has been rehearsed.
- Advantage: no dependency on the KVM being up; the writer lives on the host.

## Recommendation

Option A. It reuses the exact mechanism that has already run physically (two-card), its persistence is a single reversible BMC field plus "leave the image connected", every recovery branch is the already-written recovery runbook with a remote detach as the first step, and it does not touch the ESXi system partition or NVRAM boot variables. Option B should wait until the four-card writer has run unattended several times from the MSD and until a rehearsal of the ESP/boot-entry procedure exists, and even then only with the loader-pin refresh procedure for ESXi patches in place. Independent of the option: install and `--check` the guest BAR hook on the management host, add the post-boot BAR alert, and keep the override `Disabled` around ESXi patching.

Open questions for the parent: whether `Continuous` is in this BMC's allowable values (one Redfish GET); whether to extend fail-open to pre-PREPARE refusals by chainloading the Boot0003 path without the byte pin (a design change outside this brief: such a refusal currently parks the host); whether the four-card writer should refuse or fall open when exactly two cards already show the enlarged layout (it currently refuses, by design, and with the fallback that refusal now degrades to a plain boot rather than a hang).
