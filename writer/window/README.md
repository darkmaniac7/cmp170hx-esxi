# Two-card EFI writer window driver (`two_card_window.py`)

Lean, journaled driver for the maintenance window in which an ESXi host boots the UEFI BAR
writer once from USB. The writer image is served by an IP-KVM's mass-storage drive
(PiKVM-style `/api/msd/*` HTTP API), the one-time UEFI USB boot is set through the BMC's
Redfish boot override, and chassis power goes through IPMI. The hardware guards from the
window runbook are kept; the guest return gate, XML collectors, CPU readiness and coexistence
scripts are not used.

Every subcommand observes first and refuses (exit 2, `REFUSED-*.json`) when its guard is
not met. Without `--execute` it prints observations and the exact actions it would take.
Executed steps journal to `<root>/two-card-window-<WINDOW>/<step>-<utc>.json` (mode 0600;
`<root>` is `--root`, default `./artifacts`). Passwords are read from environment variables
and never written. `reboot --execute` additionally needs `--i-confirm-single-boot`; a second
reboot in the same window directory is refused by the exclusive `LIVE_ATTEMPT.json`.

Run as root on a control host that reaches the ESXi API, the BMC, the IP-KVM and the guest:
`python3 -B two_card_window.py <step> --window <WINDOW> [--execute] [settings]`
with `WINDOW` like `bar64-two-card-20261018-2300` (regex `[a-z0-9][a-z0-9-]{0,47}`).

Requirements: `pyvmomi` (put a vendored copy on `PYTHONPATH` if it is not installed),
`requests`, `paramiko`, `ipmitool`; `playwright` with Chromium only if the KVM rejects the
plain `/api/auth/login` POST. Guest access is `ssh` with key auth (`BatchMode=yes`).

## Configuration

Every setting is a flag, and each flag except the `--*-password-env` ones also reads the
environment variable shown when the flag is absent. Nothing defaults to a real host; a step
whose settings are missing refuses with `missing configuration: ...`. The `record` step passes
its KVM settings on to the background recorder.

| Flag | Env | Used for |
|---|---|---|
| `--esxi-host` | `ESXI_HOST` | vSphere API and the temporary SSH session of `physical-check` |
| `--esxi-user` | `ESXI_USER` | default `root`; the credential guard accepts `root` only (vsish BAR reads) |
| `--esxi-password-env` | | name of the env var holding the ESXi password (default `ESXI_PASSWORD`) |
| `--esxi-uuid` | `ESXI_UUID` | expected host hardware UUID; checked on the ESXi API and on the Redfish system |
| `--esxi-build` | `ESXI_BUILD` | expected ESXi build number |
| `--guest-vm` | `GUEST_VM` | managed object id of the GPU guest VM; must be one of `--vms-on` |
| `--vms-on` | `VMS_ON` | comma-separated VM moids running before the window (shut down, later started) |
| `--vms-off` | `VMS_OFF` | comma-separated VM moids that stay off; `--vms-on` + `--vms-off` must be the whole host inventory |
| `--bmc-url` | `BMC_URL` | Redfish base URL, e.g. `https://bmc.example` (system path `/redfish/v1/Systems/Self`) |
| `--ipmi-host` | `IPMI_HOST` | IPMI lanplus address; default is the host part of `--bmc-url` |
| `--bmc-user` | `BMC_USER` | BMC account for Redfish and IPMI |
| `--bmc-password-env` | | name of the env var holding the BMC password (default `BMC_PASSWORD`) |
| `--boot-orders` | `BMC_BOOT_ORDERS` | accepted permanent Redfish `BootOrder` lists, `;` between lists and `,` between entries, e.g. `Boot0003,Boot0002;Boot0003,Boot0004,Boot0002` |
| `--kvm-url` | `KVM_URL` | IP-KVM base URL, e.g. `https://kvm.example` |
| `--kvm-user` | `KVM_USER` | IP-KVM account (default `admin`) |
| `--kvm-password-env` | | name of the env var holding the IP-KVM password (default `KVM_PASSWORD`) |
| `--chromium` | `PLAYWRIGHT_CHROMIUM` | Chromium binary for the browser-login fallback (default: Playwright-managed) |
| `--guest-host` | `GUEST_HOST` | guest IP address for SSH; `start-vm` compares it with the IP VMware Tools reports, so use the IP, not a name |
| `--guest-user` | `GUEST_USER` | guest SSH account (default `root`) |
| `--image-src` | `WRITER_IMAGE` | local writer disk image (`upload`) |
| `--image-name` | `WRITER_IMAGE_NAME` | image name on the IP-KVM |
| `--image-sha256` | `WRITER_IMAGE_SHA256` | sha256 of the built image; every KVM read-back must match it |
| `--image-size` | `WRITER_IMAGE_SIZE` | image size in bytes (default 67108864) |
| `--gpu-units` | `GUEST_GPU_UNITS` | comma-separated guest systemd units (no `.service`) that hold the GPUs |
| `--start-order` | `GUEST_START_ORDER` | `start-services` order: `;` between groups, `,` between units started together |
| `--gpu-container` | `GUEST_GPU_CONTAINER` | guest docker container that holds a GPU |
| `--health-url` | `GUEST_HEALTH_URL` | URL fetched with curl on the guest after `start-services`; must return 200 |
| `--target-gpus` | `TARGET_GPUS` | comma-separated NVML GPU UUIDs for `target-probes` (labelled `target0`, `target1`, ...) |
| `--probe-script` | `GUEST_PROBE` | path of `tools/cmp580_trial_probe.py` on the guest |
| `--probe-python` | `GUEST_PROBE_PYTHON` | guest Python with torch + CUDA, passed to the probe as `--python` |
| `--guest-evidence-root` | `GUEST_EVIDENCE_ROOT` | guest directory for probe evidence (`<root>/<WINDOW>/evidence-<utc>/`) |

`--profile two-card|four-card` (default `two-card`) selects the expected post-writer BAR
layout read back by `physical-check` (bus, BAR index, address, size), the guest PCI addresses
that must show 64 GiB in `guest-check`, and the verdict name. The tables encode the layout the
writer builds produce on the reference host; edit `PROFILES` if your writer build places the
BARs elsewhere.

Example (passwords only in the environment):

```
export ESXI_PASSWORD=... BMC_PASSWORD=... KVM_PASSWORD=...
export ESXI_HOST=esxi.example ESXI_UUID=<host uuid> ESXI_BUILD=<build> BMC_URL=https://bmc.example BMC_USER=<user> \
       BMC_BOOT_ORDERS='Boot0003,Boot0002' KVM_URL=https://kvm.example GUEST_HOST=<guest ip> GUEST_VM=<moid> \
       VMS_ON=<guest moid>,<moid> VMS_OFF=<moid>,<moid>
python3 -B two_card_window.py preflight --window bar64-two-card-20261018-2300
```

## Ordered command list

| # | Step | Guard / what it does |
|---|---|---|
| 1 | `preflight` | `--vms-on` VMs on, `--vms-off` VMs off, maintenance false, IPMI on, KVM streamer + snapshot, `/api/msd`, guest `cmp-bar1-repair --check` and modprobe hook present. Verdict PASS required |
| 2 | `upload --execute` | set_enabled, write `--image-src` as `--image-name`, read back and compare with `--image-sha256`, set_params cdrom=false rw=false, verify not connected. Writes `MEDIA_VERIFIED.json` |
| 3 | `quiesce --execute` | guest: `docker stop` the `--gpu-container`, `systemctl stop` every `--gpu-units` unit, wait <=240 s, require no `/dev/nvidia*` holders |
| 4 | `shutdown-vms --execute` | ShutdownGuest each `--vms-on` VM once; poll 20 s up to 600 s until every listed VM is poweredOff; never resends |
| 5 | `enter-maintenance --execute` | requires all listed VMs off |
| 6 | `attach --execute` | requires all off + maintenance + selected read-only image; set_connected true, readback |
| 7 | `bootusb --execute` | requires all off + maintenance; Redfish Once/UEFI/Usb with readback; permanent order unchanged |
| 8 | `record --execute --seconds 900` | background KVM snapshot poller into `console-kvm/` (dedups identical frames, `frames.jsonl`); `recorder.pid` |
| 9 | `reboot --execute --i-confirm-single-boot` | guards: all off, maintenance, medium connected rw=false, override Once/Usb read back, recorder alive, no `LIVE_ATTEMPT.json`; RebootHost(force=False) once |
| 10 | `wait-esxi --execute` | polls the API up to 900 s for a newer host boot than `LIVE_ATTEMPT.json` |
| 11 | `physical-check --execute` | all off + maintenance; TSM-SSH started, one `vsish ... BARInfo` read per expected BAR (eight for two-card), SSH/firewall restored exactly; `PHYSICAL_RESULT-*.json` verdict `PHYSICAL_TWO_CARD_64GIB_ESXI_CONFIRMED` |
| 12 | `detach-clear --execute` | set_connected false, image sha unchanged, set_enabled false, Redfish clear + readback (idempotent) |
| 13 | `exit-maintenance --execute` | |
| 14 | `start-vm --vm <guest moid> --execute` | PowerOn, waits for tools + IP equal to `--guest-host` + ssh |
| 15 | `guest-check` | boot_id, resource1 sizes, hook journal/check, NVML BAR1 per GPU; PASS = 0d:00.0 and 0d:01.0 at 65536 MiB |
| 16 | `target-probes --execute` | small then 60 GiB probe on every `--target-gpus` UUID into `<--guest-evidence-root>/<WINDOW>/evidence-<utc>/` |
| 17 | `start-services --execute` | `--start-order` groups, then `docker start` the `--gpu-container`; waits for every `--gpu-units` unit active + `--health-url` 200 |
| 18 | `start-vm --vm <moid> --execute` for each remaining `--vms-on` VM | |
| 19 | `status` | any time: census, maintenance, boot, IPMI, BMC boot, MSD, recorder, LIVE_ATTEMPT |

`ipmi-off --execute` and `ipmi-on --execute` are used only in the recovery branches below.

Console success signature (runbook 2.9): `WRITER_RUN execute=1 target_count=2`, two
`WRITER_REBAR_TRANSITION` lines (cap 0x400->0x1ffc00, ctrl 0x601->0x1001), `WRITER_STATE
status=4 applied_mask=3`, `START_IMAGE attempt=1 pci_mutation=1`, VMware loader banner.

## Recovery branches

| Branch | Condition | Driver steps |
|---|---|---|
| R0 | refusal before the reboot | fix the input; `exit-maintenance`, `start-vm` (guest first, then the other `--vms-on` VMs), `start-services` |
| R1 | `HANDOFF_REFUSED ... cold_recovery_required=0`, no `write_attempted=1` (host parked in the EFI, no PCI write) | `detach-clear --execute` (clears override too), confirm `status` shows IPMI on and no ESXi API, `ipmi-off --execute` (refuses while media connected), `ipmi-on --execute` (requires 10 s stable off + cleared override), `wait-esxi --previous-boot <old>`, `exit-maintenance`, `start-vm` the guest, `guest-check` (all cards small, hook skips), `start-services`, `start-vm` the other VMs |
| R2 | any `write_attempted=1` without `status=4`, `cold_recovery_required=1`, returned loader, lost console | follow the writer's cold-recovery procedure: no rerun; `detach-clear`, `ipmi-off`, dwell, `ipmi-on`; fresh cold 4x64 MiB `physical-check` (expect verdict DIFFERS with all small; keep the raw rows) before any `start-vm` |
| R3 | ESXi back but the guest VM fails to start | keep VMs off, read `vmware.log` on the host; never reboot the host (loses the allocation) |
| R4 | `guest-check` shows 256 MiB on a big-BAR card | stop GPU holders (`quiesce`), `modprobe -r nvidia_drm nvidia_modeset nvidia_uvm nvidia`, `/usr/local/sbin/cmp-bar1-repair`, `modprobe nvidia`, `guest-check` |
| R5 | a target probe fails | no production impact; `start-services` proceeds; read the summary and dmesg before any retry |

The cold-recovery rule in one line: after any possible PCI store without a success status, detach and disable the media, clear the boot override, power the host off, confirm it stays off for at least ten seconds, power it on, and verify four small BARs before starting any VM. Never re-run the writer on a possibly written card.

Never: a second `reboot` in one window, clearing a STOP to retry.
