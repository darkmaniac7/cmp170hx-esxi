#!/usr/bin/env python3
"""ECC verifier for the CMP 170HX ECC hardware-recipe experiment (read-only except --pattern).

Sources of truth, none of which the cmpunlocker reporting patch can forge:
  1. raw BAR0 registers through the sysfs resource0 mmap (same pattern as fuse_readout.py);
  2. NVML library calls (nvmlDeviceGetEccMode, nvmlDeviceGetTotalEccErrors) and nvidia-smi -q -d ECC;
  3. optional full-memory pattern test through the existing cmp580_trial_probe.py (--pattern).
Writes one JSON report per run; --diff compares two reports.
"""
import os, argparse, ctypes, datetime, json, mmap, os, re, struct, subprocess, sys

REGS = [
    (0x823000, 0x814, "FEATURE_READOUT"), (0x823000, 0x800, "FEAT_OVR_ECC_PLM"), (0x823000, 0x80c, "FEAT_OVR_ECC"),
    (0x823000, 0x810, "FEAT_OVR_ECC_1"), (0x823000, 0x82c, "FEAT_OVR_ECC_2"),
    (0x9a0000, 0x470, "FBPA_ECC_CTRL"), (0x9a0000, 0x33c, "FBPA_ECC_B"), (0x9a0000, 0x084, "FBPA_ECC_C"),
    (0x9a1000, 0x004, "FBPA_MRS_ECC"), (0x17e000, 0x318, "LTC_ECC_A"), (0x17e000, 0x21c, "LTC_ECC_B"),
    (0x820000, 0x228, "OPT_ECC_EN"),
]
PROBE = str(__import__("pathlib").Path(__file__).resolve().parent / "cmp580_trial_probe.py")


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def pci_path(bdf):
    return f"/sys/bus/pci/devices/0000:{bdf}"


def rd32(fd, page, off):
    m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=page)
    try:
        return struct.unpack_from("<I", m, off)[0]
    finally:
        m.close()


def raw_registers(bdf):
    fd = os.open(pci_path(bdf) + "/resource0", os.O_RDONLY | os.O_SYNC)
    try:
        out = {}
        for page, off, name in REGS:
            v = rd32(fd, page, off)
            out[name] = {"addr": hex(page + off), "value": hex(v)}
        out["FEATURE_READOUT"]["ecc_dram_bit16"] = (int(out["FEATURE_READOUT"]["value"], 16) >> 16) & 1
        # DED counters: 0x900488 + subp*4 + fbpa*0x4000, 24 FBPAs x 2 sub-partitions (survey memo).
        ded = {}
        for fbpa in range(24):
            base = 0x900488 + fbpa * 0x4000
            page = base & ~0xfff
            for subp in range(2):
                addr = base + subp * 4
                ded[f"fbpa{fbpa:02d}_subp{subp}"] = hex(rd32(fd, page, addr - page))
        out["ded_counters"] = ded
        return out
    finally:
        os.close(fd)


def nvml(bdf):
    rec = {}
    try:
        lib = ctypes.CDLL("libnvidia-ml.so.1")
        if lib.nvmlInit_v2() != 0:
            return {"error": "nvmlInit failed"}
        try:
            h = ctypes.c_void_p()
            r = lib.nvmlDeviceGetHandleByPciBusId_v2(("0000:" + bdf).encode(), ctypes.byref(h))
            if r != 0:
                return {"error": f"handle lookup rc={r}"}
            cur, pend = ctypes.c_uint(), ctypes.c_uint()
            r = lib.nvmlDeviceGetEccMode(h, ctypes.byref(cur), ctypes.byref(pend))
            rec["ecc_mode"] = {"rc": r, "current": cur.value, "pending": pend.value} if r == 0 else {"rc": r}
            counts = {}
            for et, etn in ((0, "corrected"), (1, "uncorrected")):
                for ct, ctn in ((0, "volatile"), (1, "aggregate")):
                    n = ctypes.c_ulonglong()
                    r = lib.nvmlDeviceGetTotalEccErrors(h, et, ct, ctypes.byref(n))
                    counts[f"{etn}_{ctn}"] = {"rc": r, "count": n.value} if r == 0 else {"rc": r}
            rec["total_ecc_errors"] = counts
            rec["note"] = "rc 3 = NVML_ERROR_NOT_SUPPORTED (what the stock cards report today)"
        finally:
            lib.nvmlShutdown()
    except OSError as exc:
        rec["error"] = f"libnvidia-ml: {exc}"
    try:
        smi = subprocess.run(["timeout", "30", "nvidia-smi", "-q", "-d", "ECC", "-i", "0000:" + bdf],
                             capture_output=True, text=True, timeout=40)
        rec["nvidia_smi_ecc"] = smi.stdout[-3000:] if smi.returncode == 0 else {"rc": smi.returncode, "err": smi.stderr[-500:]}
    except Exception as exc:  # noqa: BLE001
        rec["nvidia_smi_ecc"] = {"error": str(exc)}
    return rec


def pattern(bdf, uuid, outdir):
    cmd = ["python3", PROBE, "--gpu", uuid, "--mode", "vram", "--gib", "60", "--chunk-mib", "32",
           "--python", os.environ.get("CUDA_PYTHON", "python3"), "--expect-memory-mib", "65536",
           "--timeout", "60", "--deadline", "90", "--max-temp", "78", "--output-dir", outdir]
    r = subprocess.run(["timeout", "150"] + cmd, capture_output=True, text=True)
    rec = {"argv": cmd, "rc": r.returncode, "stderr_tail": r.stderr[-800:]}
    try:
        s = json.load(open(os.path.join(outdir, "summary.json")))
        rec["verdict"] = s.get("verdict"); rec["failures"] = s.get("failures"); rec["boot_id"] = s.get("boot_id")
        rec["summary"] = s
    except Exception as exc:  # noqa: BLE001
        rec["summary_error"] = str(exc)
    return rec


def report(args):
    rec = {"utc": utc(), "boot_id": open("/proc/sys/kernel/random/boot_id").read().strip(), "card": args.card,
           "uuid": args.uuid, "driver_srcversion": open("/sys/module/nvidia/srcversion").read().strip()
           if os.path.exists("/sys/module/nvidia/srcversion") else None,
           "label": args.label}
    rec["registers"] = raw_registers(args.card)
    rec["nvml"] = nvml(args.card)
    rec["dmesg_ecc_recipe"] = subprocess.run("dmesg | grep -E 'CMP580: ecc recipe' | cut -c1-240 | tail -60",
                                             shell=True, capture_output=True, text=True).stdout.splitlines()
    if args.pattern:
        outdir = os.path.join(os.path.dirname(args.out) or ".", "pattern-" + rec["utc"].replace(":", "").replace("+00:00", "Z"))
        rec["pattern"] = pattern(args.card, args.uuid, outdir)
    with open(args.out, "w") as f:
        json.dump(rec, f, indent=1)
    regs = rec["registers"]
    print(f"{args.label}: ECC_DRAM bit16={regs['FEATURE_READOUT']['ecc_dram_bit16']} FBPA_ECC_CTRL={regs['FBPA_ECC_CTRL']['value']} "
          f"FEAT_OVR_ECC={regs['FEAT_OVR_ECC']['value']} nvml_ecc_mode={rec['nvml'].get('ecc_mode')} "
          f"ded_nonzero={sum(1 for v in regs['ded_counters'].values() if int(v,16) != 0 and (int(v,16) & 0xfff00000) != 0xbad00000)} ded_pri_error={sum(1 for v in regs['ded_counters'].values() if (int(v,16) & 0xfff00000) == 0xbad00000)} written={args.out}")


def diff(a, b):
    A, B = json.load(open(a)), json.load(open(b))
    print(f"control {A['label']} ({A['boot_id'][:8]}) vs recipe {B['label']} ({B['boot_id'][:8]})")
    for name in A["registers"]:
        if name == "ded_counters":
            continue
        va, vb = A["registers"][name]["value"], B["registers"][name]["value"]
        print(f"  {name:18s} {va:>12} -> {vb:>12} {'CHANGED' if va != vb else ''}")
    da, db = A["registers"]["ded_counters"], B["registers"]["ded_counters"]
    changed = [k for k in da if da[k] != db.get(k)]
    print(f"  ded_counters changed: {len(changed)} {changed[:6]}")
    print(f"  nvml ecc_mode: {A['nvml'].get('ecc_mode')} -> {B['nvml'].get('ecc_mode')}")
    print(f"  nvml counters: {A['nvml'].get('total_ecc_errors')} -> {B['nvml'].get('total_ecc_errors')}")
    for lbl, R in (("control", A), ("recipe", B)):
        p = R.get("pattern")
        if p:
            print(f"  pattern {lbl}: verdict={p.get('verdict')} failures={p.get('failures')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--card", required=True, help="guest PCI BDF, e.g. 0d:00.0"); ap.add_argument("--uuid", required=True, help="GPU UUID (nvidia-smi -L)")
    ap.add_argument("--label", default="run"); ap.add_argument("--out"); ap.add_argument("--pattern", action="store_true")
    ap.add_argument("--diff", nargs=2, metavar=("CONTROL", "RECIPE"))
    a = ap.parse_args()
    if a.diff:
        return diff(*a.diff)
    if not a.out:
        ap.error("--out required")
    report(a)


if __name__ == "__main__":
    main()
