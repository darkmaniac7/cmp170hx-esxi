"""Pure offline placement arithmetic. No hardware, I/O, or authorization path.

Input: complete normalized inventory (all memory BARs, bridge windows and
reserved ranges); addresses are integer inclusive start/end pairs. Firmware
collection, inventory completeness, identity freshness and GCD allocation must
be independently verified by the consumer. Even a successful result is ONLY a
proposal. Tests contain historical addresses plus synthetic ownership fixtures.
"""
import re

BAR1_SIZE = 0x1000000000
BAR3_SIZE = 0x02000000
SPAN = BAR1_SIZE + BAR3_SIZE
MAX_ADDRESS = (1 << 48) - 1
TARGET = "0000:01:00.0"
PORT = "0000:00:01.1"
ROOT = "0000:00"


class Refusal(ValueError):
    """Inventory cannot justify a placement proposal."""


def require(condition, reason):
    if not condition:
        raise Refusal(reason)


def number(value, maximum=MAX_ADDRESS):
    require(type(value) is int and 0 <= value <= maximum, "invalid integer/range")
    return value


def interval(row):
    start, end = number(row["start"]), number(row["end"])
    require(start <= end, "inverted interval")
    return start, end


def bdf(value):
    require(isinstance(value, str) and
            re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\.[0-7]", value),
            "invalid BDF")
    return value


def identity(row, expected_bdf, vendor, device):
    require(row["bdf"] == expected_bdf and row["vendor"] == vendor and
            row["device"] == device, "identity mismatch")


def contains(outer, inner):
    return outer[0] <= inner[0] <= inner[1] <= outer[1]


def plan(inventory):
    """Return proposal or raise Refusal; never modifies the supplied inventory."""
    try:
        return _plan(inventory)
    except (KeyError, TypeError, AttributeError, IndexError) as exc:
        raise Refusal("missing or malformed inventory") from exc


def _plan(i):
    require(i["schema"] == 1 and type(i["schema"]) is int, "unknown schema")
    require(i["inventory_complete"] is True and i["gcd_complete"] is True,
            "incomplete inventory")
    require(isinstance(i["provenance"], str) and 0 < len(i["provenance"]) <= 4096,
            "missing provenance")
    identity(i["target"], TARGET, 0x10de, 0x20c2)
    require(i["target"]["root"] == ROOT and i["target"]["port"] == PORT,
            "wrong target route")
    p = i["port"]
    identity(p, PORT, 0x1022, 0x1483)
    for field in ("primary", "secondary", "subordinate"):
        number(p[field], 255)
    require(p["root"] == ROOT and p["primary"] == 0 and p["secondary"] == 1 and
            p["subordinate"] == 1 and p["descendants"] == [TARGET] and
            p["prefetch64"] is True, "port is not exact exclusive 64-bit route")
    for name in ("roots", "occupied", "gcd"):
        require(type(i[name]) is list and 0 < len(i[name]) <= 16384,
                "missing/oversized inventory collection")

    root_ranges = []
    all_roots = []
    for row in i["roots"]:
        extent = interval(row)
        require(type(row["translation"]) is int and row["translation"] == 0,
                "address translation unsupported")
        require(isinstance(row["id"], str) and
                re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}", row["id"]), "bad root ID")
        segment = number(row["segment"], 65535)
        lo, hi = number(row["bus_start"], 255), number(row["bus_end"], 255)
        require(lo <= hi and int(row["id"][:4], 16) == segment and
                int(row["id"][5:], 16) == lo, "root bus identity mismatch")
        for prior in all_roots:
            require(extent[1] < prior[0] or extent[0] > prior[1], "root windows overlap")
        all_roots.append(extent)
        if row["id"] == ROOT:
            require(segment == 0 and lo == 0 and hi >= 1, "target outside root")
            root_ranges.append(extent)
    require(root_ranges, "target root absent")

    occupied = []
    bars = {}
    port_windows = {}
    seen = set()
    for row in i["occupied"]:
        extent = interval(row)
        kind = row["kind"]
        require(kind in ("bar", "bridge", "reserved"), "unknown occupied range")
        if kind == "reserved":
            occupied.append(extent)
            continue
        address = bdf(row["bdf"])
        number(row["vendor"], 65534); number(row["device"], 65534)
        require(type(row["prefetch64"]) is bool, "unknown BAR/window type")
        selector = number(row["index"], 5) if kind == "bar" else row["window"]
        if kind == "bridge":
            require(selector in ("memory", "prefetch"), "unknown bridge window")
        key = (kind, address, selector)
        require(key not in seen, "duplicate resource identity")
        seen.add(key)
        if address == TARGET:
            identity(row, TARGET, 0x10de, 0x20c2)
            require(kind == "bar" and selector in (0, 1, 3), "unexpected target resource")
            bars[selector] = (extent, row["prefetch64"])
            if selector in (1, 3):
                continue
        if address == PORT:
            identity(row, PORT, 0x1022, 0x1483)
            require(kind == "bridge", "unexpected port resource")
            port_windows[selector] = (extent, row["prefetch64"])
            if selector == "prefetch":
                continue
        # Do not excuse any extra function/device on the asserted exclusive bus.
        require(address == TARGET or not address.startswith("0000:01:"),
                "additional occupied device on target bus")
        occupied.append(extent)
    require(set(bars) == {0, 1, 3} and set(port_windows) == {"memory", "prefetch"},
            "target or port resource missing")
    original_bars = sorted(value[0] for value in bars.values())
    require(all(a[1] < b[0] for a, b in zip(original_bars, original_bars[1:])),
            "original target BARs overlap")
    for index, size, pref in ((0, 0x1000000, False), (1, 0x4000000, True),
                              (3, BAR3_SIZE, True)):
        extent, actual_pref = bars[index]
        require(extent[1] - extent[0] + 1 == size and actual_pref is pref and
                extent[0] % size == 0, "unexpected original BAR layout")
    require(port_windows["prefetch"][1] is True and
            port_windows["memory"][1] is False and
            contains(port_windows["prefetch"][0], bars[1][0]) and
            contains(port_windows["prefetch"][0], bars[3][0]) and
            contains(port_windows["memory"][0], bars[0][0]), "original route mismatch")

    gcd_free = []
    gcd_all = []
    for row in i["gcd"]:
        extent = interval(row)
        require(row["type"] in ("mmio", "system", "reserved", "nonexistent",
                                "persistent", "more_reliable", "unaccepted"),
                "unknown GCD type")
        for owner in (row["image_owner"], row["device_owner"]):
            require(owner is None or (isinstance(owner, str) and bool(owner)),
                    "unknown GCD owner")
        gcd_all.append(extent)
        if row["type"] == "mmio" and row["image_owner"] is None and row["device_owner"] is None:
            gcd_free.append(extent)
    gcd_all.sort()
    require(all(a[1] < b[0] for a, b in zip(gcd_all, gcd_all[1:])), "overlapping GCD map")

    # At most 4096 64-GiB slots exist below 48 bits. Search intersection with
    # ONE free GCD descriptor, because merging distinct owners/attributes is
    # not justified by this inventory's reduced schema.
    choices = []
    for root in root_ranges:
        for free in gcd_free:
            low, high = max(root[0], free[0], 1 << 32), min(root[1], free[1])
            base = (low + BAR1_SIZE - 1) & ~(BAR1_SIZE - 1)
            while base <= high and SPAN - 1 <= high - base:
                end = base + SPAN - 1
                collisions = [o for o in occupied if o[0] <= end and base <= o[1]]
                if not collisions:
                    choices.append((base, end))
                    break
                after = max(o[1] for o in collisions) + 1
                base = (after + BAR1_SIZE - 1) & ~(BAR1_SIZE - 1)
    require(choices, "no collision-free, unowned GCD placement within root")
    base, end = min(choices)
    return dict(verdict="PROPOSAL_ONLY_REQUIRES_LIVE_UEFI_VALIDATION",
                hardware_authorized=False, provenance=i["provenance"],
                target=TARGET, port=PORT, root=ROOT,
                bar1=dict(start=base, end=base + BAR1_SIZE - 1),
                bar3=dict(start=base + BAR1_SIZE, end=end),
                bridge=dict(start=base, end=end),
                bar0_unchanged=dict(start=bars[0][0][0], end=bars[0][0][1]),
                gcd_reservation=dict(start=base, length=SPAN, alignment_log2=36),
                excluded_resources=["0000:01:00.0/BAR1", "0000:01:00.0/BAR3",
                                    "0000:00:01.1/prefetch"],
                requires=["fresh complete UEFI inventory and host identity",
                          "successful exact GCD allocation and ownership readback",
                          "separately reviewed guarded PCI transaction",
                          "independent ESXi then guest enumeration evidence"])
