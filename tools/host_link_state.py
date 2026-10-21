#!/usr/bin/env python3
"""Real PCIe link state of the CMP 170HX cards and their root ports, read on the ESXi host (.243).

The guest's config space is VMware's virtual link (5 GT/s x32 whatever the card does), so the
verdict of the Gen3 window comes from the host: this runs `lspci -d` (hex dump of every function's
standard config space) through tools/esxi_link_probe.py (which starts
TSM-SSH on .243 if it is stopped and stops it again), walks each function's capability list to the
PCI Express capability (id 0x10) and decodes LnkCap, LnkSta, LnkCtl2 and LnkSta2. Read-only.

usage: host_link_state.py [--expect-speed N --expect-bdf BDF [--expect-bdf BDF ...]]
                          [--out FILE.json] [BDF ...]
default BDFs: the Gen3 card 0000:02:00.0 (guest 0d:01.0) and its root port 0000:00:01.3, then the
other CMPs and their ports. With --expect-speed, exit 1 unless every --expect-bdf shows that
LnkSta speed (3 = 8 GT/s) and the same width as before (--expect-width).
"""
import argparse
import os
import datetime
import json
import re
import subprocess
import sys

PY = os.environ.get("PROBE_PYTHON", "python3")  # a python with pyVmomi + paramiko
PROBE = str(__import__("pathlib").Path(__file__).resolve().parent / "esxi_link_probe.py")
DEFAULT = ["0000:02:00.0", "0000:00:01.3", "0000:01:00.0", "0000:00:01.1",
           "0000:47:00.0", "0000:40:03.1", "0000:48:00.0", "0000:40:03.3"]
SPEED = {1: "2.5GT/s", 2: "5GT/s", 3: "8GT/s", 4: "16GT/s", 5: "32GT/s"}
GUEST = {"0000:02:00.0": "0d:01.0", "0000:01:00.0": "0d:00.0", "0000:47:00.0": "0c:00.0", "0000:48:00.0": "0c:01.0"}


def fetch_dump():
    r = subprocess.run([PY, PROBE, "lspci -d"], capture_output=True, text=True, timeout=240)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-2000:])
        raise SystemExit("esxi_link_probe.py failed rc=%d" % r.returncode)
    return r.stdout


def parse_dump(text):
    """{bdf: bytes(256)} from the lspci -d output."""
    out, cur, buf = {}, None, {}
    hdr = re.compile(r"^(\d{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7])\s")
    row = re.compile(r"^([0-9a-f]{2}): ((?:[0-9a-f]{2} ?){16})$")
    for line in text.splitlines():
        m = hdr.match(line)
        if m:
            if cur and len(buf) == 16:
                out[cur] = b"".join(buf[k] for k in sorted(buf))
            cur, buf = m.group(1), {}
            continue
        m = row.match(line.strip())
        if m and cur:
            buf[int(m.group(1), 16)] = bytes.fromhex(m.group(2).replace(" ", ""))
    if cur and len(buf) == 16:
        out[cur] = b"".join(buf[k] for k in sorted(buf))
    return out


def le16(b, o):
    return b[o] | (b[o + 1] << 8)


def le32(b, o):
    return le16(b, o) | (le16(b, o + 2) << 16)


def decode(cfg):
    if not (cfg[0x06] & 0x10):
        return {"error": "no capability list"}
    ptr, seen = cfg[0x34], set()
    while ptr and ptr not in seen and ptr + 0x34 <= 256:
        seen.add(ptr)
        cap_id, nxt = cfg[ptr], cfg[ptr + 1]
        if cap_id == 0x10:
            lnkcap, lnkctl, lnksta = le32(cfg, ptr + 0x0c), le16(cfg, ptr + 0x10), le16(cfg, ptr + 0x12)
            lnkcap2, lnkctl2, lnksta2 = le32(cfg, ptr + 0x2c), le16(cfg, ptr + 0x30), le16(cfg, ptr + 0x32)
            return {
                "cap_ptr": ptr, "port_type": (le16(cfg, ptr + 2) >> 4) & 0xf,
                "lnkcap": lnkcap, "lnkcap_max_speed": lnkcap & 0xf, "lnkcap_max_width": (lnkcap >> 4) & 0x3f,
                "lnkcap2": lnkcap2, "lnkcap2_speeds": [g for g in range(1, 8) if lnkcap2 & (1 << g)],
                "lnkctl": lnkctl, "lnksta": lnksta, "lnksta_speed": lnksta & 0xf, "lnksta_width": (lnksta >> 4) & 0x3f,
                "lnkctl2": lnkctl2, "lnkctl2_target": lnkctl2 & 0xf,
                "lnksta2": lnksta2, "eq_complete": bool(lnksta2 & 0x2), "eq_phase1": bool(lnksta2 & 0x4),
                "eq_phase2": bool(lnksta2 & 0x8), "eq_phase3": bool(lnksta2 & 0x10),
            }
        ptr = nxt
    return {"error": "no PCI Express capability"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bdfs", nargs="*", default=DEFAULT)
    ap.add_argument("--expect-speed", type=int)
    ap.add_argument("--expect-width", type=int)
    ap.add_argument("--expect-bdf", action="append", default=[])
    ap.add_argument("--out")
    a = ap.parse_args()
    dump = parse_dump(fetch_dump())
    rec = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "functions": {}}
    ok = True
    for bdf in a.bdfs:
        cfg = dump.get(bdf)
        if cfg is None:
            print("%s: NOT PRESENT in the host's lspci dump" % bdf)
            rec["functions"][bdf] = {"error": "not present"}
            if bdf in a.expect_bdf:
                ok = False
            continue
        ident = "%04x:%04x" % (le16(cfg, 0), le16(cfg, 2))
        d = decode(cfg)
        rec["functions"][bdf] = dict(d, id=ident)
        if "error" in d:
            print("%s [%s]: %s" % (bdf, ident, d["error"]))
            if bdf in a.expect_bdf:
                ok = False
            continue
        guest = (" guest " + GUEST[bdf]) if bdf in GUEST else ""
        print("%s [%s]%s: LnkCap max %s x%u, LnkCap2 speeds %s; LnkSta %s x%u (0x%04x); LnkCtl2 target %s; "
              "LnkSta2 0x%04x eq_complete=%s phase1=%s" % (
                  bdf, ident, guest, SPEED.get(d["lnkcap_max_speed"], "?"), d["lnkcap_max_width"],
                  ",".join(SPEED.get(g, str(g)) for g in d["lnkcap2_speeds"]) or "none",
                  SPEED.get(d["lnksta_speed"], "?"), d["lnksta_width"], d["lnksta"],
                  SPEED.get(d["lnkctl2_target"], "?"), d["lnksta2"], d["eq_complete"], d["eq_phase1"]))
        if bdf in a.expect_bdf:
            if a.expect_speed is not None and d["lnksta_speed"] != a.expect_speed:
                ok = False
            if a.expect_width is not None and d["lnksta_width"] != a.expect_width:
                ok = False
    if a.out:
        with open(a.out, "w") as f:
            json.dump(rec, f, indent=1)
        print("wrote", a.out)
    if a.expect_bdf:
        print("VERDICT:", "PASS" if ok else "FAIL",
              "(expected LnkSta speed %s%s on %s)" % (a.expect_speed, (" width x%d" % a.expect_width) if a.expect_width else "", ",".join(a.expect_bdf)))
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
