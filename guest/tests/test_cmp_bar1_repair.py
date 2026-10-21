#!/usr/bin/python3
"""Unit tests for cmp-bar1-repair against a fake sysfs tree (no hardware).

Run:  nice -n 19 python3 tests/test_cmp_bar1_repair.py -v
The fake device is a sparse regular file; the GPU's reaction to the second
store (CAP/CTRL re-derived) and the write-1 failure are emulated by wrapping
MirrorPage.write32 here, so the production tool has no test-only code path.
"""
import contextlib
import fcntl
import threading
import time
import importlib.machinery
import importlib.util
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), 'cmp-bar1-repair')


def load_tool():
    loader = importlib.machinery.SourceFileLoader('cmp_bar1_repair', TOOL)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


tool = load_tool()
OFFSETS = tool.OFFSETS
RES0 = tool.RES0_SIZE
RES1_64G = tool.RES1_SIZE
RES1_64M = 64 * 1024 * 1024
BDF = '0000:0d:00.0'

RESET_WORDS = [0x20c210de, 0x00100002, 0x00020010, 0x00002930, 0xf3000000, 0xc, 0x640,
               0xc, 0x650, 0x10015, 0x460, 0x400, 0x801, 0x26, 0x80000000]
RESTORED_WORDS = RESET_WORDS[:11] + [0x1ffc00, 0x1001, 0x30, 0x8000000a]
RESET_WORDS_601 = RESET_WORDS[:11] + [0x400, 0x601, 0x26, 0x80000000]   # firmware-time CTRL
SMALL_WORDS = RESET_WORDS_601                                           # 64 MiB host BAR1 card
ALL_ONES = [0xffffffff] * 15                                            # decode off at cold boot
GEN2_DEVCTL = 0x0002c810                                                # word 078 after the Gen2 driver's retrain


def make_card(devdir, bdf, words, res1_size=RES1_64G, driver=None, command=0x0002,
              devsta=0, device=0x20c2, vendor=0x10de):
    d = os.path.join(devdir, bdf)
    os.makedirs(d)
    with open(os.path.join(d, 'vendor'), 'w') as f:
        f.write('0x%04x\n' % vendor)
    with open(os.path.join(d, 'device'), 'w') as f:
        f.write('0x%04x\n' % device)
    with open(os.path.join(d, 'class'), 'w') as f:
        f.write('0x030200\n')
    with open(os.path.join(d, 'enable'), 'w') as f:
        f.write('0\n')
    with open(os.path.join(d, 'resource0'), 'wb') as f:
        f.truncate(RES0)
        for off, w in zip(OFFSETS, words):
            f.seek(tool.PAGE_OFFSET + off)
            f.write(struct.pack('<I', w))
    with open(os.path.join(d, 'resource1'), 'wb') as f:
        f.truncate(res1_size)
    cfg = bytearray(4096)
    struct.pack_into('<HHHH', cfg, 0, vendor, device, command, 0x0010)
    cfg[0x34] = 0x60
    cfg[0x60], cfg[0x61] = 0x01, 0x68          # PM cap -> MSI
    cfg[0x68], cfg[0x69] = 0x05, 0x78          # MSI cap -> PCIe
    cfg[0x78], cfg[0x79] = 0x10, 0x00          # PCIe cap, end of list
    struct.pack_into('<H', cfg, 0x7a, 0x0002)
    struct.pack_into('<HH', cfg, 0x80, 0x2930, devsta)
    with open(os.path.join(d, 'config'), 'wb') as f:
        f.write(bytes(cfg))
    if driver:
        os.symlink('../../../bus/pci/drivers/%s' % driver, os.path.join(d, 'driver'))
    return d


def words_in(devdir, bdf):
    with open(os.path.join(devdir, bdf, 'resource0'), 'rb') as f:
        out = []
        for off in OFFSETS:
            f.seek(tool.PAGE_OFFSET + off)
            out.append(struct.unpack('<I', f.read(4))[0])
        return out


def set_command(devdir, bdf, command):
    p = os.path.join(devdir, bdf, 'config')
    with open(p, 'r+b') as f:
        f.seek(4)
        f.write(struct.pack('<H', command))


def set_words(devdir, bdf, words):
    with open(os.path.join(devdir, bdf, 'resource0'), 'r+b') as f:
        for off, w in zip(OFFSETS, words):
            f.seek(tool.PAGE_OFFSET + off)
            f.write(struct.pack('<I', w))


def enable_text(devdir, bdf):
    with open(os.path.join(devdir, bdf, 'enable')) as f:
        return f.read()


def poke(page, off, value):
    """Test-side emulation of the GPU changing a word (writes the fake file)."""
    struct.pack_into('<I', page.mm, off, value)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cmp-bar1-test-')
        self.devdir = os.path.join(self.tmp.name, 'devices')
        os.makedirs(self.devdir)
        self.jdir = os.path.join(self.tmp.name, 'journal')
        self.rdir = os.path.join(self.tmp.name, 'run')
        self.lock = os.path.join(self.tmp.name, 'lock')
        self.moddir = os.path.join(self.tmp.name, 'module')
        os.makedirs(self.moddir)
        self.events = []
        self._orig_enable = tool.enable_decode
        self._orig_write32 = tool.MirrorPage.write32
        self._orig_snapshot = tool.MirrorPage.snapshot
        self._orig_init = tool.MirrorPage.__init__
        self.maps = []
        events, maps = self.events, self.maps
        orig_snapshot, orig_init = self._orig_snapshot, self._orig_init

        def snapshot(page):
            events.append('snapshot')
            return orig_snapshot(page)

        def init(page, resource0, writable=False):
            maps.append(writable)
            orig_init(page, resource0, writable)

        tool.MirrorPage.snapshot = snapshot
        tool.MirrorPage.__init__ = init
        self.set_hw(None)
        self.set_enable_hw(None)

    def reset(self):
        self.tearDown()
        self.setUp()

    def tearDown(self):
        tool.enable_decode = self._orig_enable
        tool.MirrorPage.write32 = self._orig_write32
        tool.MirrorPage.snapshot = self._orig_snapshot
        tool.MirrorPage.__init__ = self._orig_init
        self.tmp.cleanup()

    def set_hw(self, reaction):
        """reaction(page, off, value) runs after a real store, emulating the GPU."""
        events, orig = self.events, self._orig_write32

        def write32(page, off, value):
            orig(page, off, value)
            events.append('write:0x%x=0x%x' % (off, value))
            if reaction:
                reaction(page, off, value)
        tool.MirrorPage.write32 = write32

    def set_enable_hw(self, reaction):
        """reaction(devpath) runs after a real write to the fake `enable` file."""
        events, orig = self.events, self._orig_enable

        def enable_decode(devpath):
            orig(devpath)
            events.append('enable')
            if reaction:
                reaction(devpath)
        tool.enable_decode = enable_decode

    def argv(self, *mode):
        return list(mode) + ['--no-syslog', '--sysfs-devices', self.devdir, '--journal-dir', self.jdir,
                             '--run-dir', self.rdir, '--lock-file', self.lock, '--modules-dir', self.moddir]

    def load_nvidia_module(self, state='live'):
        os.makedirs(os.path.join(self.moddir, 'nvidia'))
        with open(os.path.join(self.moddir, 'nvidia', 'initstate'), 'w') as f:
            f.write(state + '\n')

    def run_tool(self, *mode):
        argv = self.argv(*mode)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = tool.main(argv)
        return rc, out.getvalue(), self.journal()

    def journal(self):
        if not os.path.isdir(self.jdir):
            return None
        names = sorted(n for n in os.listdir(self.jdir) if n.endswith('.json'))
        self.assertEqual(len(names), 1, names)
        self.assertRegex(names[0], r'^\d{8}T\d{6}\.\d{6}Z-[0-9a-f-]+\.json$')
        with open(os.path.join(self.jdir, names[0])) as f:
            return json.load(f)

    def writes(self):
        return [e for e in self.events if e.startswith('write:')]


def hw_restores_on_cfg(page, off, value):
    if off == 0xdcc and value == 0x8000000a:
        poke(page, 0xbbc, 0x1ffc00)
        poke(page, 0xbc0, 0x1001)


class TestRepair(Base):
    def test_reset_profile_two_writes_in_order_with_intermediate_check(self):
        make_card(self.devdir, BDF, RESET_WORDS)
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertEqual(self.writes(), ['write:0x724=0x30', 'write:0xdcc=0x8000000a'])
        # read-only read, journal intent, RW re-read, store 1, readback, store 2, readback
        self.assertEqual(self.events, ['snapshot', 'snapshot', 'write:0x724=0x30', 'snapshot',
                                       'write:0xdcc=0x8000000a', 'snapshot'])
        self.assertEqual(self.maps, [False, True])
        self.assertEqual(words_in(self.devdir, BDF), RESTORED_WORDS)
        self.assertIn('%s REPAIRED' % BDF, out)
        self.assertIn('REPAIR intent journaled', out)
        c = j['cards'][0]
        self.assertEqual(c['action'], 'REPAIRED')
        self.assertEqual(c['words_before'], RESET_WORDS)
        self.assertEqual(c['words_after'], RESTORED_WORDS)
        self.assertEqual([w['offset'] for w in c['writes']], [0x724, 0xdcc])
        self.assertEqual([w['value'] for w in c['writes']], [0x30, 0x8000000a])
        self.assertEqual(j['summary'], {'repaired': 1})
        self.assertEqual(j['exit_code'], 0)

    def test_reset_profile_ctrl_0x601_two_writes_in_order(self):
        make_card(self.devdir, BDF, RESET_WORDS_601)
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertEqual(self.events, ['snapshot', 'snapshot', 'write:0x724=0x30', 'snapshot',
                                       'write:0xdcc=0x8000000a', 'snapshot'])
        self.assertEqual(words_in(self.devdir, BDF), RESTORED_WORDS)
        c = j['cards'][0]
        self.assertEqual(c['action'], 'REPAIRED')
        self.assertEqual(c['words_before'], RESET_WORDS_601)
        self.assertEqual(c['words_after_write1'][12], 0x601)   # CTRL untouched by store 1 here
        self.assertEqual([w['offset'] for w in c['writes']], [0x724, 0xdcc])
        self.assertIn('%s REPAIRED' % BDF, out)

    def test_intermediate_cap_ctrl_change_after_write1_is_accepted(self):
        make_card(self.devdir, BDF, RESET_WORDS)

        def hw(page, off, value):
            if off == 0x724:
                poke(page, 0xbbc, 0x1ffc00)      # CAP may already widen after CYA
                poke(page, 0xbc0, 0x1001)
        self.set_hw(hw)
        rc, out, j = self.run_tool()
        self.assertEqual(j['cards'][0]['action'], 'REPAIRED')
        self.assertEqual(len(self.writes()), 2)

    def test_restored_profile_zero_writes(self):
        make_card(self.devdir, BDF, RESTORED_WORDS)
        rc, out, j = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertEqual(self.writes(), [])
        self.assertEqual(self.maps, [False])
        self.assertIn('%s OK already restored' % BDF, out)
        self.assertEqual(j['cards'][0]['action'], 'OK')
        self.assertEqual(words_in(self.devdir, BDF), RESTORED_WORDS)

    def test_small_resource1_skips_but_logs_words(self):
        make_card(self.devdir, BDF, SMALL_WORDS, res1_size=RES1_64M)
        rc, out, j = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertEqual(self.writes(), [])
        self.assertIn('%s SKIP host BAR1 small' % BDF, out)
        self.assertRegex(out, r'(?m)^%s words(?: [0-9a-f]{3}=[0-9a-f]{8}){15}$' % re.escape(BDF))
        self.assertEqual(j['cards'][0]['action'], 'SKIP')
        self.assertEqual(j['cards'][0]['words_before'], SMALL_WORDS)
        self.assertFalse(j['cards'][0]['host_bar1_64g'])

    def test_small_resource1_with_reset_profile_still_skips(self):
        make_card(self.devdir, BDF, RESET_WORDS, res1_size=256 * 1024 * 1024)
        rc, out, j = self.run_tool()
        self.assertEqual(self.writes(), [])
        self.assertEqual(j['cards'][0]['action'], 'SKIP')

    def test_unexpected_words_refuse_zero_writes(self):
        variants = {
            'half_applied': RESET_WORDS[:13] + [0x30, 0x80000000],
            'cap_wide_ctrl_small': RESET_WORDS[:11] + [0x1ffc00, 0x801, 0x26, 0x80000000],
            'ctrl_0x401': RESET_WORDS[:11] + [0x400, 0x401, 0x26, 0x80000000],
            'ctrl_0x600_bar_index': RESET_WORDS[:11] + [0x400, 0x600, 0x26, 0x80000000],
            'ctrl_0xa01': RESET_WORDS[:11] + [0x400, 0xa01, 0x26, 0x80000000],
            'ctrl_0x1001_with_reset_cya_cfg': RESET_WORDS[:11] + [0x400, 0x1001, 0x26, 0x80000000],
            'ctrl_0x1401': RESET_WORDS[:11] + [0x400, 0x1401, 0x26, 0x80000000],
            'wrong_id': [0x20b010de] + RESET_WORDS[1:],
            'all_ones_bar1': RESET_WORDS[:5] + [0xffffffff] + RESET_WORDS[6:],
            'bad_marker': RESET_WORDS[:14] + [0xbad00200],
            'rebar_header_moved': RESET_WORDS[:9] + [0x00010010] + RESET_WORDS[10:],
            'bar0_ctrl_differs': RESET_WORDS[:10] + [0x461] + RESET_WORDS[11:],
        }
        for name, words in variants.items():
            with self.subTest(name):
                self.reset()
                try:
                    make_card(self.devdir, BDF, words)
                    self.set_hw(hw_restores_on_cfg)
                    rc, out, j = self.run_tool()
                    self.assertEqual(rc, 0)
                    self.assertEqual(self.writes(), [], name)
                    self.assertIn('%s REFUSE unexpected profile' % BDF, out, name)
                    self.assertEqual(j['cards'][0]['action'], 'REFUSE', name)
                    self.assertEqual(words_in(self.devdir, BDF), words, name)
                finally:
                    self.tearDown()

    def test_write1_readback_failure_stops_before_write2(self):
        cases = {
            'ctrl_unexpected': lambda page, off, value: off == 0x724 and poke(page, 0xbc0, 0x1401),
            'cap_unexpected': lambda page, off, value: off == 0x724 and poke(page, 0xbbc, 0x3ffc00),
            'bar1_lo_changed': lambda page, off, value: off == 0x724 and poke(page, 0x14, 0x1c),
            'cfg_changed_early': lambda page, off, value: off == 0x724 and poke(page, 0xdcc, 0x8000000a),
            'cya_not_taken': lambda page, off, value: off == 0x724 and poke(page, 0x724, 0x26),
            'bus_master_appeared': lambda page, off, value: off == 0x724 and poke(page, 0x4, 0x00100006),
            'transactions_pending': lambda page, off, value: off == 0x724 and poke(page, 0x80, 0x00202930),
        }
        for name, hw in cases.items():
            with self.subTest(name):
                self.reset()
                try:
                    make_card(self.devdir, BDF, RESET_WORDS)
                    self.set_hw(hw)
                    rc, out, j = self.run_tool()
                    self.assertEqual(rc, 0)
                    self.assertEqual(self.writes(), ['write:0x724=0x30'], name)
                    self.assertIn('FAILED after write 1', out, name)
                    self.assertIn('stopped before write 2', out, name)
                    c = j['cards'][0]
                    self.assertEqual(c['action'], 'FAILED_AFTER_WRITE1', name)
                    self.assertEqual(len(c['writes']), 1, name)
                    self.assertEqual(len(c['words_after_write1']), 15, name)
                    if name != 'cfg_changed_early':
                        self.assertEqual(words_in(self.devdir, BDF)[14], 0x80000000, name)
                finally:
                    self.tearDown()

    def test_write2_readback_failure_reports_cold_recovery(self):
        make_card(self.devdir, BDF, RESET_WORDS)   # plain file: CAP/CTRL never widen
        rc, out, j = self.run_tool()
        self.assertEqual(len(self.writes()), 2)
        self.assertEqual(j['cards'][0]['action'], 'FAILED_AFTER_WRITE2')
        self.assertIn('COLD RECOVERY', out)
        self.assertEqual(rc, 0)

    def test_driver_bound_live_run_skips_without_mapping(self):
        make_card(self.devdir, BDF, RESET_WORDS, driver='nvidia', command=0x0406)
        rc, out, j = self.run_tool()
        self.assertEqual(self.maps, [])
        self.assertEqual(self.writes(), [])
        self.assertIn('%s SKIP driver nvidia bound' % BDF, out)
        self.assertEqual(j['cards'][0]['action'], 'SKIP')

    def test_not_idle_refuses(self):
        cases = {
            'config_bus_master_on': dict(command=0x0006),
            'config_memory_off': dict(command=0x0000),
            'config_transactions_pending': dict(devsta=0x0020),
            'config_fatal_error': dict(devsta=0x0004),
        }
        for name, kw in cases.items():
            with self.subTest(name):
                self.reset()
                try:
                    make_card(self.devdir, BDF, RESET_WORDS, **kw)
                    self.set_hw(hw_restores_on_cfg)
                    rc, out, j = self.run_tool()
                    self.assertEqual(self.writes(), [], name)
                    if name == 'config_memory_off':
                        # decode off takes the sysfs-enable path first; the fake stays off
                        self.assertIn('%s REFUSE memory decode still off after sysfs enable' % BDF, out, name)
                    else:
                        self.assertIn('%s REFUSE reset profile but not repairable' % BDF, out, name)
                    self.assertEqual(j['cards'][0]['action'], 'REFUSE', name)
                finally:
                    self.tearDown()
        for name, words in {'mirror_bus_master_on': RESET_WORDS[:1] + [0x00100006] + RESET_WORDS[2:],
                            'mirror_transactions_pending': RESET_WORDS[:3] + [0x00202930] + RESET_WORDS[4:]}.items():
            with self.subTest(name):
                self.reset()
                try:
                    make_card(self.devdir, BDF, words)
                    self.set_hw(hw_restores_on_cfg)
                    rc, out, j = self.run_tool()
                    self.assertEqual(self.writes(), [], name)
                    self.assertEqual(j['cards'][0]['action'], 'REFUSE', name)
                    self.assertIn('not repairable', out)
                finally:
                    self.tearDown()

    def test_restored_with_gen2_devctl_is_ok(self):
        words = RESTORED_WORDS[:2] + [GEN2_DEVCTL] + RESTORED_WORDS[3:]
        make_card(self.devdir, BDF, words, driver='nvidia', command=0x0406)
        for mode in (('--check',), ()):
            rc, out, j = self.run_tool(*mode)
            self.assertEqual(self.writes(), [])
            self.assertEqual(j['cards'][0]['action'], 'OK' if mode else 'SKIP')   # live: driver bound
            for n in os.listdir(self.jdir):
                os.unlink(os.path.join(self.jdir, n))
        os.unlink(os.path.join(self.devdir, BDF, 'driver'))
        set_command(self.devdir, BDF, 0x0002)
        rc, out, j = self.run_tool()
        self.assertEqual(self.writes(), [])
        self.assertIn('%s OK already restored' % BDF, out)
        self.assertEqual(words_in(self.devdir, BDF), words)

    def test_reset_with_gen2_devctl_two_stores(self):
        words = RESET_WORDS[:2] + [GEN2_DEVCTL] + RESET_WORDS[3:]
        make_card(self.devdir, BDF, words)
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(self.writes(), ['write:0x724=0x30', 'write:0xdcc=0x8000000a'])
        self.assertEqual(j['cards'][0]['action'], 'REPAIRED')
        self.assertEqual(words_in(self.devdir, BDF), words[:11] + list(RESTORED_WORDS[11:]))
        self.assertEqual(j['cards'][0]['words_after'][2], GEN2_DEVCTL)

    def test_id_or_rebar_header_changed_refuses(self):
        variants = {
            'id_wrong_device': [0x20b110de] + RESET_WORDS[1:],
            'id_wrong_vendor': [0x20c21002] + RESET_WORDS[1:],
            'id_2082_on_20c2_card': [0x208210de] + RESET_WORDS[1:],
            'rebar_header_version': RESET_WORDS[:9] + [0x00020015] + RESET_WORDS[10:],
            'rebar_header_id': RESET_WORDS[:9] + [0x00010016] + RESET_WORDS[10:],
            'rebar_header_next_ptr': RESET_WORDS[:9] + [0x10010015] + RESET_WORDS[10:],
            'bar0_ctrl': RESET_WORDS[:10] + [0x0460 | 0x100] + RESET_WORDS[11:],
            'with_gen2_devctl_and_bad_header': RESET_WORDS[:2] + [GEN2_DEVCTL] + RESET_WORDS[3:9] + [0x00010016] + RESET_WORDS[10:],
        }
        for name, words in variants.items():
            with self.subTest(name):
                self.reset()
                try:
                    make_card(self.devdir, BDF, words)
                    self.set_hw(hw_restores_on_cfg)
                    rc, out, j = self.run_tool()
                    self.assertEqual(self.writes(), [], name)
                    self.assertIn('%s REFUSE unexpected profile' % BDF, out, name)
                    self.assertEqual(words_in(self.devdir, BDF), words, name)
                finally:
                    self.tearDown()

    def test_informational_words_are_not_pinned(self):
        # COMMAND/STATUS with extra status bits, odd DEVCTL, and different physical BAR0/BAR3
        words = list(RESET_WORDS)
        words[1] = 0x00100002 | 0x08000000       # STATUS: master data parity error latched (informational)
        words[2] = 0x0002c810
        words[4] = 0xa0000000                    # BAR0 elsewhere
        words[7], words[8] = 0x0000000c, 0x00000700   # BAR3 elsewhere
        make_card(self.devdir, BDF, words)
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(j['cards'][0]['action'], 'REPAIRED')
        self.assertEqual(len(self.writes()), 2)

    def test_bar1_not_64g_aligned_refuses(self):
        variants = {
            'low_bits_set': RESET_WORDS[:5] + [0x4000000c] + RESET_WORDS[6:],      # 1 GiB offset
            'high_nibble_set': RESET_WORDS[:6] + [0x00000641] + RESET_WORDS[7:],   # 4 GiB offset
            'not_prefetchable_64bit': RESET_WORDS[:5] + [0x00000004] + RESET_WORDS[6:],
        }
        for name, words in variants.items():
            with self.subTest(name):
                self.reset()
                try:
                    make_card(self.devdir, BDF, words)
                    self.set_hw(hw_restores_on_cfg)
                    rc, out, j = self.run_tool()
                    self.assertEqual(self.writes(), [], name)
                    self.assertIn('%s REFUSE reset profile but not repairable' % BDF, out, name)
                    self.assertIn('BAR1', out, name)
                    self.assertEqual(j['cards'][0]['action'], 'REFUSE', name)
                finally:
                    self.tearDown()

    def test_mixed_fleet_repairs_only_the_reset_64g_card(self):
        make_card(self.devdir, '0000:0c:00.0', SMALL_WORDS, res1_size=RES1_64M)
        make_card(self.devdir, '0000:0c:01.0', RESTORED_WORDS)
        make_card(self.devdir, '0000:0d:00.0', RESET_WORDS)
        make_card(self.devdir, '0000:0d:01.0', SMALL_WORDS, res1_size=RES1_64M)
        make_card(self.devdir, '0000:0e:00.0', RESET_WORDS_601)             # second enlarged card
        make_card(self.devdir, '0000:04:00.0', RESET_WORDS, device=0x27b8)   # not a CMP
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        actions = {c['bdf']: c['action'] for c in j['cards']}
        self.assertEqual(actions, {'0000:0c:00.0': 'SKIP', '0000:0c:01.0': 'OK',
                                   '0000:0d:00.0': 'REPAIRED', '0000:0d:01.0': 'SKIP',
                                   '0000:0e:00.0': 'REPAIRED'})
        self.assertEqual(j['ignored_nvidia_devices'], 1)
        self.assertEqual(self.writes(), ['write:0x724=0x30', 'write:0xdcc=0x8000000a'] * 2)
        self.assertEqual(words_in(self.devdir, '0000:0d:00.0'), RESTORED_WORDS)
        self.assertEqual(words_in(self.devdir, '0000:0e:00.0'), RESTORED_WORDS)
        self.assertEqual(words_in(self.devdir, '0000:0c:00.0'), SMALL_WORDS)
        self.assertEqual(words_in(self.devdir, '0000:04:00.0'), RESET_WORDS)

    def test_journal_unavailable_refuses_before_any_write(self):
        make_card(self.devdir, BDF, RESET_WORDS)
        self.set_hw(hw_restores_on_cfg)
        with open(os.path.join(self.tmp.name, 'blocker'), 'w') as f:
            f.write('x')
        os.makedirs(self.rdir)
        with open(os.path.join(self.rdir, 'journal'), 'w') as f:   # fallback dir is a file
            f.write('x')
        self.jdir = os.path.join(self.tmp.name, 'blocker', 'journal')  # primary under a file
        argv = self.argv()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = tool.main(argv)
        self.assertEqual(rc, 0)
        self.assertEqual(self.writes(), [])
        self.assertIn('REFUSE journal unavailable', out.getvalue())
        self.assertEqual(words_in(self.devdir, BDF), RESET_WORDS)

    def test_no_cards_is_harmless(self):
        rc, out, j = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertIn('no CMP 170HX', out)
        self.assertEqual(j['cards'], [])


class TestColdBootDecode(Base):
    """Cold boot: driverless function has COMMAND.MEM=0 and BAR0 reads all-ones."""

    def hw_enable_works(self, bdf=BDF, words=RESET_WORDS, command=0x0002):
        devdir = self.devdir

        def reaction(devpath):
            set_command(devdir, bdf, command)
            set_words(devdir, bdf, words)
        return reaction

    def test_all_ones_then_enable_then_two_stores(self):
        make_card(self.devdir, BDF, ALL_ONES, command=0x0000)
        self.set_enable_hw(self.hw_enable_works())
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertEqual(self.events, ['enable', 'snapshot', 'snapshot', 'write:0x724=0x30', 'snapshot',
                                       'write:0xdcc=0x8000000a', 'snapshot'])
        self.assertEqual(enable_text(self.devdir, BDF), '1\n')   # '1' written in place over '0\n'
        self.assertEqual(words_in(self.devdir, BDF), RESTORED_WORDS)
        c = j['cards'][0]
        self.assertEqual(c['action'], 'REPAIRED')
        self.assertTrue(c['decode_enabled_by_tool'])
        self.assertEqual(c['config_before_enable']['command'], 0)
        self.assertEqual(c['config']['command'], 0x2)
        self.assertEqual(c['enable_count_before'], '0')
        self.assertEqual(c['words_before'], RESET_WORDS)
        self.assertIn('%s enabled memory decode via sysfs enable (config COMMAND 0x0000 -> 0x0002' % BDF, out)
        self.assertIn('%s REPAIRED' % BDF, out)

    def test_enable_write_fails_refuses_without_mapping(self):
        d = make_card(self.devdir, BDF, ALL_ONES, command=0x0000)
        os.unlink(os.path.join(d, 'enable'))
        os.makedirs(os.path.join(d, 'enable'))          # open(O_WRONLY) -> EISDIR
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertEqual(self.maps, [])
        self.assertEqual(self.writes(), [])
        self.assertIn('%s REFUSE cannot enable memory decode via sysfs enable' % BDF, out)
        self.assertEqual(j['cards'][0]['action'], 'REFUSE')
        self.assertFalse(j['cards'][0]['decode_enabled_by_tool'])

    def test_enable_leaves_decode_off_refuses(self):
        make_card(self.devdir, BDF, ALL_ONES, command=0x0000)
        self.set_enable_hw(None)                          # written, but COMMAND stays 0
        rc, out, j = self.run_tool()
        self.assertEqual(self.events, ['enable'])
        self.assertEqual(self.maps, [])
        self.assertIn('%s REFUSE memory decode still off after sysfs enable' % BDF, out)
        self.assertEqual(j['cards'][0]['action'], 'REFUSE')

    def test_enable_sets_bus_master_refuses(self):
        make_card(self.devdir, BDF, ALL_ONES, command=0x0000)
        self.set_enable_hw(self.hw_enable_works(command=0x0006))
        rc, out, j = self.run_tool()
        self.assertEqual(self.events, ['enable'])
        self.assertEqual(self.maps, [])
        self.assertIn('%s REFUSE BUS_MASTER on after sysfs enable' % BDF, out)

    def test_all_ones_after_enable_refuses_zero_writes(self):
        make_card(self.devdir, BDF, ALL_ONES, command=0x0000)
        self.set_enable_hw(self.hw_enable_works(words=ALL_ONES))
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(self.events, ['enable', 'snapshot'])
        self.assertEqual(self.writes(), [])
        self.assertIn('%s REFUSE unexpected profile (unreadable word(s)' % BDF, out)
        self.assertEqual(j['cards'][0]['action'], 'REFUSE')
        self.assertTrue(j['cards'][0]['decode_enabled_by_tool'])
        self.assertEqual(words_in(self.devdir, BDF), ALL_ONES)

    def test_check_and_dry_run_report_decode_off_without_enable_write(self):
        for mode in ('--check', '--dry-run'):
            with self.subTest(mode):
                self.reset()
                try:
                    make_card(self.devdir, BDF, ALL_ONES, command=0x0000)
                    rc, out, j = self.run_tool(mode)
                    self.assertEqual(rc, 0)
                    self.assertEqual(self.events, [])
                    self.assertEqual(self.maps, [])
                    self.assertEqual(enable_text(self.devdir, BDF), '0\n')
                    self.assertIn('%s decode off: memory decode off (config COMMAND=0x0000)' % BDF, out)
                    self.assertEqual(j['cards'][0]['action'], 'DECODE_OFF')
                    self.assertFalse(j['cards'][0]['decode_enabled_by_tool'])
                finally:
                    self.tearDown()
        self.setUp()
        make_card(self.devdir, BDF, ALL_ONES, command=0x0000)
        rc, out, j = self.run_tool('--check', '--strict')
        self.assertEqual(rc, 1)

    def test_small_card_with_decode_off_skips_without_enable(self):
        make_card(self.devdir, BDF, ALL_ONES, res1_size=RES1_64M, command=0x0000)
        for mode in ((), ('--check',)):
            rc, out, j = self.run_tool(*mode)
            self.assertEqual(self.events, [])
            self.assertEqual(enable_text(self.devdir, BDF), '0\n')
            self.assertIn('%s SKIP host BAR1 small' % BDF, out)
            self.assertIn('page not read', out)
            for n in os.listdir(self.jdir):
                os.unlink(os.path.join(self.jdir, n))

    def test_nvidia_module_loaded_guard(self):
        make_card(self.devdir, BDF, RESET_WORDS)
        self.load_nvidia_module('live')
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool()
        self.assertEqual(self.maps, [])
        self.assertEqual(self.writes(), [])
        self.assertIn('%s SKIP nvidia module loaded (initstate live)' % BDF, out)
        self.assertEqual(j['cards'][0]['nvidia_module_state'], 'live')
        for n in os.listdir(self.jdir):
            os.unlink(os.path.join(self.jdir, n))
        rc, out, j = self.run_tool('--check')            # check is unaffected
        self.assertEqual(j['cards'][0]['action'], 'WOULD_REPAIR')
        for n in os.listdir(self.jdir):
            os.unlink(os.path.join(self.jdir, n))
        rc, out, j = self.run_tool('--allow-module-loaded')
        self.assertEqual(j['cards'][0]['action'], 'REPAIRED')
        self.assertEqual(self.writes(), ['write:0x724=0x30', 'write:0xdcc=0x8000000a'])


class TestSerialisation(Base):
    def test_two_concurrent_runs_one_repairs_other_finds_restored(self):
        make_card(self.devdir, BDF, RESET_WORDS)
        events = self.events

        def slow_hw(page, off, value):
            hw_restores_on_cfg(page, off, value)
            if off == 0x724:
                time.sleep(0.6)                           # hold the critical section open
        self.set_hw(slow_hw)
        results = {}

        def runner(name, delay):
            time.sleep(delay)
            results[name] = tool.main(self.argv())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            a = threading.Thread(target=runner, args=('a', 0.0))
            b = threading.Thread(target=runner, args=('b', 0.2))
            a.start()
            b.start()
            a.join(30)
            b.join(30)
        self.assertEqual(results, {'a': 0, 'b': 0})
        self.assertEqual(self.writes(), ['write:0x724=0x30', 'write:0xdcc=0x8000000a'])
        # run b's single snapshot comes after run a's complete sequence
        self.assertEqual(events, ['snapshot', 'snapshot', 'write:0x724=0x30', 'snapshot',
                                  'write:0xdcc=0x8000000a', 'snapshot', 'snapshot'])
        journals = []
        for n in sorted(os.listdir(self.jdir)):
            with open(os.path.join(self.jdir, n)) as f:
                journals.append(json.load(f))
        self.assertEqual(len(journals), 2)
        actions = sorted(j['cards'][0]['action'] for j in journals)
        self.assertEqual(actions, ['OK', 'REPAIRED'])
        waited = {j['cards'][0]['action']: j['lock']['waited_s'] for j in journals}
        self.assertGreaterEqual(waited['OK'], 0.3)
        self.assertEqual(waited['REPAIRED'], 0.0)
        self.assertIn('%s OK already restored' % BDF, out.getvalue())
        self.assertEqual(words_in(self.devdir, BDF), RESTORED_WORDS)

    def test_lock_timeout_refuses_whole_run(self):
        make_card(self.devdir, BDF, RESET_WORDS)
        self.set_hw(hw_restores_on_cfg)
        fd = os.open(self.lock, os.O_RDWR | os.O_CREAT, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX)
        try:
            t0 = time.monotonic()
            rc, out, j = self.run_tool('--lock-timeout', '0.3')
            self.assertGreaterEqual(time.monotonic() - t0, 0.3)
            self.assertEqual(rc, 0)
            self.assertEqual(self.maps, [])
            self.assertEqual(self.writes(), [])
            self.assertIn('REFUSE lock %s still busy after 0 s' % self.lock, out)
            self.assertEqual(j['cards'], [])
            self.assertFalse(j['lock']['acquired'])
            self.assertEqual(words_in(self.devdir, BDF), RESET_WORDS)
            for n in os.listdir(self.jdir):
                os.unlink(os.path.join(self.jdir, n))
            rc, out, j = self.run_tool('--check', '--strict', '--lock-timeout', '0.3')
            self.assertEqual(rc, 1)
        finally:
            os.close(fd)


class TestCheckAndDryRun(Base):
    def test_check_with_driver_bound_is_read_only(self):
        make_card(self.devdir, '0000:0c:00.0', SMALL_WORDS, res1_size=RES1_64M, driver='nvidia', command=0x0406)
        make_card(self.devdir, '0000:0d:00.0', RESTORED_WORDS, driver='nvidia', command=0x0406)
        make_card(self.devdir, '0000:0d:01.0', RESET_WORDS, driver='nvidia', command=0x0406)
        rc, out, j = self.run_tool('--check')
        self.assertEqual(rc, 0)
        self.assertEqual(self.writes(), [])
        self.assertEqual(self.maps, [False, False, False])
        actions = {c['bdf']: c['action'] for c in j['cards']}
        self.assertEqual(actions, {'0000:0c:00.0': 'SKIP', '0000:0d:00.0': 'OK', '0000:0d:01.0': 'WOULD_REPAIR'})
        self.assertIn('0000:0d:00.0 OK already restored', out)
        self.assertIn('0000:0c:00.0 SKIP host BAR1 small', out)
        self.assertIn('0000:0d:01.0 WOULD REPAIR', out)
        for bdf in actions:
            self.assertRegex(out, r'(?m)^%s words(?: [0-9a-f]{3}=[0-9a-f]{8}){15}$' % re.escape(bdf))
        self.assertEqual(j['mode'], 'check')

    def test_check_strict_exit_codes(self):
        make_card(self.devdir, '0000:0c:00.0', SMALL_WORDS, res1_size=RES1_64M, driver='nvidia', command=0x0406)
        make_card(self.devdir, '0000:0d:00.0', RESTORED_WORDS, driver='nvidia', command=0x0406)
        rc, out, j = self.run_tool('--check', '--strict')
        self.assertEqual(rc, 0)
        self.assertEqual(j['exit_code'], 0)
        make_card(self.devdir, '0000:0d:01.0', RESET_WORDS, driver='nvidia', command=0x0406)
        for n in os.listdir(self.jdir):
            os.unlink(os.path.join(self.jdir, n))
        rc, out, j = self.run_tool('--check', '--strict')
        self.assertEqual(rc, 1)
        self.assertEqual(j['exit_code'], 1)
        self.assertEqual(self.writes(), [])

    def test_dry_run_applies_live_gates_without_writing(self):
        make_card(self.devdir, '0000:0d:00.0', RESET_WORDS)
        make_card(self.devdir, '0000:0d:01.0', RESET_WORDS, driver='nvidia', command=0x0406)
        self.set_hw(hw_restores_on_cfg)
        rc, out, j = self.run_tool('--dry-run')
        self.assertEqual(rc, 0)
        self.assertEqual(self.writes(), [])
        self.assertEqual(self.maps, [False])
        actions = {c['bdf']: c['action'] for c in j['cards']}
        self.assertEqual(actions, {'0000:0d:00.0': 'WOULD_REPAIR', '0000:0d:01.0': 'SKIP'})
        self.assertEqual(words_in(self.devdir, '0000:0d:00.0'), RESET_WORDS)

    def test_live_strict_is_ignored(self):
        make_card(self.devdir, BDF, RESET_WORDS, driver='nvidia', command=0x0406)
        rc, out, j = self.run_tool('--strict')
        self.assertEqual(rc, 0)
        self.assertIn('--strict is only honoured', out)


class TestAccessLayer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cmp-bar1-mmio-')
        self.devdir = os.path.join(self.tmp.name, 'devices')
        os.makedirs(self.devdir)
        self.res0 = os.path.join(make_card(self.devdir, BDF, RESET_WORDS), 'resource0')

    def tearDown(self):
        self.tmp.cleanup()

    def test_whitelist_order_and_count(self):
        with tool.MirrorPage(self.res0, writable=True) as p:
            self.assertEqual(p.snapshot(), RESET_WORDS)
            with self.assertRaises(RuntimeError):
                p.write32(0xdcc, 0x8000000a)        # second store first
            with self.assertRaises(RuntimeError):
                p.write32(0x724, 0x31)              # wrong value
            with self.assertRaises(RuntimeError):
                p.write32(0x728, 0x30)              # wrong offset
            p.write32(0x724, 0x30)
            with self.assertRaises(RuntimeError):
                p.write32(0x724, 0x30)              # retry
            p.write32(0xdcc, 0x8000000a)
            with self.assertRaises(RuntimeError):
                p.write32(0xdcc, 0x8000000a)        # third store
            self.assertEqual(p.stores, [(0x724, 0x30), (0xdcc, 0x8000000a)])
        self.assertEqual(words_in(self.devdir, BDF)[13:], [0x30, 0x8000000a])

    def test_read_only_page_cannot_store(self):
        with tool.MirrorPage(self.res0) as p:
            with self.assertRaises(RuntimeError):
                p.write32(0x724, 0x30)
        self.assertEqual(words_in(self.devdir, BDF), RESET_WORDS)

    def test_wrong_resource0_size_rejected(self):
        with open(self.res0, 'r+b') as f:
            f.truncate(64 * 1024 * 1024)
        with self.assertRaises(OSError):
            tool.MirrorPage(self.res0)

    def test_config_parser(self):
        cfg = tool.read_config(os.path.join(self.devdir, BDF, 'config'))
        self.assertEqual((cfg['vendor'], cfg['device'], cfg['command'], cfg['status']), (0x10de, 0x20c2, 0x2, 0x10))
        self.assertEqual((cfg['pcie_cap'], cfg['devctl'], cfg['devsta']), (0x78, 0x2930, 0))
        self.assertEqual(tool.idle_reasons(cfg, RESET_WORDS), [])

    def test_classify(self):
        self.assertEqual(tool.classify(RESET_WORDS, 0x20c2)[0], 'RESET')
        self.assertEqual(tool.classify(RESTORED_WORDS, 0x20c2)[0], 'RESTORED')
        self.assertEqual(tool.classify(RESET_WORDS, 0x2082)[0], 'UNEXPECTED')
        self.assertEqual(tool.classify(RESET_WORDS_601, 0x20c2)[0], 'RESET')
        self.assertEqual(tool.classify(RESET_WORDS[:11] + [0x400, 0x401, 0x26, 0x80000000], 0x20c2)[0], 'UNEXPECTED')
        self.assertEqual(tool.classify([0x208210de] + RESET_WORDS[1:], 0x2082)[0], 'RESET')
        self.assertEqual(tool.classify(RESET_WORDS[:13] + [0x30, 0x80000000], 0x20c2)[0], 'UNEXPECTED')
        self.assertEqual(tool.classify(RESET_WORDS[:14] + [0xffffffff], 0x20c2)[0], 'BAD')
        self.assertEqual(tool.classify(RESET_WORDS[:2] + [GEN2_DEVCTL] + RESET_WORDS[3:], 0x20c2)[0], 'RESET')
        self.assertEqual(tool.classify(RESTORED_WORDS[:2] + [GEN2_DEVCTL] + RESTORED_WORDS[3:], 0x20c2)[0], 'RESTORED')
        self.assertEqual(tool.STRUCTURE, {9: 0x00010015, 10: 0x00000460})
        self.assertEqual(tool.bar1_alignment_reasons(RESET_WORDS), [])
        self.assertEqual(tool.bar1_alignment_reasons(RESET_WORDS_601), [])
        self.assertEqual(len(tool.bar1_alignment_reasons(RESET_WORDS[:5] + [0xf800000c] + RESET_WORDS[6:])), 1)


class TestCli(unittest.TestCase):
    def test_script_runs_under_isolated_python(self):
        with tempfile.TemporaryDirectory(prefix='cmp-bar1-cli-') as tmp:
            devdir = os.path.join(tmp, 'devices')
            os.makedirs(devdir)
            make_card(devdir, '0000:0c:00.0', SMALL_WORDS, res1_size=RES1_64M, driver='nvidia', command=0x0406)
            make_card(devdir, '0000:0d:00.0', RESTORED_WORDS, driver='nvidia', command=0x0406)
            base = [sys.executable, '-I', TOOL, '--check', '--strict', '--no-syslog', '--json',
                    '--sysfs-devices', devdir, '--journal-dir', os.path.join(tmp, 'j'), '--run-dir', os.path.join(tmp, 'r'),
                    '--lock-file', os.path.join(tmp, 'lock'), '--modules-dir', os.path.join(tmp, 'mod')]
            p = subprocess.run(base, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn('0000:0d:00.0 OK already restored', p.stdout)
            self.assertIn('"mode": "check"', p.stdout)
            make_card(devdir, '0000:0d:01.0', RESET_WORDS, driver='nvidia', command=0x0406)
            p = subprocess.run(base, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
            self.assertIn('0000:0d:01.0 WOULD REPAIR', p.stdout)
            self.assertEqual(words_in(devdir, '0000:0d:01.0'), RESET_WORDS)


if __name__ == '__main__':
    unittest.main()
