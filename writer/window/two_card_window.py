#!/usr/bin/env python3
"""Lean, journaled driver for the two-card CMP 170HX EFI writer host window.

Every subcommand observes first and refuses when its hardware guard is not met.
Without --execute a subcommand only prints what it observes and would do.
Site settings (hosts, VM ids, image, guest units) come from flags or environment
variables; passwords come only from the environment variables named by the
--*-password-env flags and are never journaled.
"""
import os
import argparse, datetime as dt, hashlib, json, os, re, subprocess, sys, time
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import urlparse

# Site configuration: main() fills these from flags / environment variables (see SETTINGS).
TW = Path.cwd()  # default journal root is ./artifacts; --root overrides it
ESXI_HOST, ESXI_USER, ESXI_UUID, ESXI_BUILD = '', 'root', '', ''
BMC, IPMI_HOST, BMC_USER, KVM, KVM_USER, CHROMIUM = '', '', '', '', 'admin', ''
GUEST, GUEST_USER, GUEST_VM = '', 'root', ''
ESXI_PASSWORD_ENV, BMC_PASSWORD_ENV, KVM_PASSWORD_ENV = 'ESXI_PASSWORD', 'BMC_PASSWORD', 'KVM_PASSWORD'
VM_ON, VM_OFF = (), ()
IMAGE_SRC, IMAGE_NAME, IMAGE_SHA, IMAGE_SIZE = None, '', '', 67108864
UNITS, START_ORDER = [], []
TEI, ROUTER_HEALTH = '', ''
TARGETS = []
PROBE, PROBE_PYTHON, GUEST_EVIDENCE = '', '', ''
NORMAL_ORDERS = ()
BIG_GUEST, SMALL_GUEST = ('0d:00.0', '0d:01.0'), ('0c:00.0', '0c:01.0')
# (bus, bar index) -> (address, size): the exact post-writer physical layout.
EXPECTED = {(0x01, 0): (0xf3000000, 16 << 20), (0x01, 1): (0x64000000000, 64 << 30), (0x01, 3): (0x65000000000, 32 << 20),
            (0x02, 0): (0xf2000000, 16 << 20), (0x02, 1): (0x66000000000, 64 << 30), (0x02, 3): (0x67000000000, 32 << 20),
            (0x47, 1): (0x63cf8000000, 64 << 20), (0x48, 1): (0x63cf0000000, 64 << 20)}
PROFILES = {
    'two-card': dict(EXPECTED=EXPECTED, VERDICT='PHYSICAL_TWO_CARD_64GIB_ESXI_CONFIRMED', BIG_GUEST=('0d:00.0', '0d:01.0'), SMALL_GUEST=('0c:00.0', '0c:01.0')),
    'four-card': dict(EXPECTED={(0x01, 0): (0xf3000000, 16 << 20), (0x01, 1): (0x64000000000, 64 << 30), (0x01, 3): (0x65000000000, 32 << 20),
                                (0x02, 0): (0xf2000000, 16 << 20), (0x02, 1): (0x66000000000, 64 << 30), (0x02, 3): (0x67000000000, 32 << 20),
                                (0x47, 0): (0xb5000000, 16 << 20), (0x47, 1): (0x48000000000, 64 << 30), (0x47, 3): (0x49000000000, 32 << 20),
                                (0x48, 0): (0xb4000000, 16 << 20), (0x48, 1): (0x4a000000000, 64 << 30), (0x48, 3): (0x4b000000000, 32 << 20)},
                      VERDICT='PHYSICAL_FOUR_CARD_64GIB_ESXI_CONFIRMED', BIG_GUEST=('0d:00.0', '0d:01.0', '0c:00.0', '0c:01.0'), SMALL_GUEST=()),
}


def apply_profile(name):
    prof = PROFILES[name]
    g = globals()
    for k in ('EXPECTED', 'BIG_GUEST', 'SMALL_GUEST'):
        g[k] = prof[k]
    g['PHYSICAL_OK_VERDICT'] = prof['VERDICT']


PHYSICAL_OK_VERDICT = 'PHYSICAL_TWO_CARD_64GIB_ESXI_CONFIRMED'
BAR_RE = re.compile(r'PCI BAR Info \{\s*Address:(0x[0-9a-fA-F]+)\s+Size:([0-9]+)\s+Flags:(0x[0-9a-fA-F]+)\s+Type:(0x[0-9a-fA-F]+)\s*\}\s*')


class Refused(Exception):
    pass


def need(ok, why):
    if not ok:
        raise Refused(why)


def utc():
    return dt.datetime.now(dt.timezone.utc)


def stamp():
    return utc().strftime('%Y%m%dT%H%M%S.%fZ')


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def _csv(v): return tuple(x.strip() for x in v.split(',') if x.strip())
def _list(v): return list(_csv(v))
def _groups(v): return [list(_csv(g)) for g in v.split(';') if _csv(g)]
def _orders(v): return tuple(list(_csv(g)) for g in v.split(';') if _csv(g))
def _targets(v): return [(u, f'target{i}') for i, u in enumerate(_csv(v))]
def _lower(v): return v.strip().lower()
def _path(v): return Path(v) if v else None


# (flag, environment variable or None, global, parser, default, help). No default names a real host.
SETTINGS = [
    ('--esxi-host', 'ESXI_HOST', 'ESXI_HOST', str, '', 'ESXi host address for the vSphere API and the temporary SSH session'),
    ('--esxi-user', 'ESXI_USER', 'ESXI_USER', str, 'root', 'ESXi account; the credential guard accepts root only (vsish BAR reads)'),
    ('--esxi-password-env', None, 'ESXI_PASSWORD_ENV', str, 'ESXI_PASSWORD', 'name of the environment variable holding the ESXi password'),
    ('--esxi-uuid', 'ESXI_UUID', 'ESXI_UUID', _lower, '', 'expected host hardware UUID (checked on the ESXi API and the Redfish system)'),
    ('--esxi-build', 'ESXI_BUILD', 'ESXI_BUILD', str, '', 'expected ESXi build number'),
    ('--guest-vm', 'GUEST_VM', 'GUEST_VM', str, '', 'managed object id of the GPU guest VM (one of --vms-on)'),
    ('--vms-on', 'VMS_ON', 'VM_ON', _csv, '', 'comma-separated VM moids running before the window; shut down, later started again'),
    ('--vms-off', 'VMS_OFF', 'VM_OFF', _csv, '', 'comma-separated VM moids that stay off; --vms-on plus --vms-off must be the whole host inventory'),
    ('--bmc-url', 'BMC_URL', 'BMC', str, '', 'BMC base URL for Redfish, e.g. https://bmc.example'),
    ('--ipmi-host', 'IPMI_HOST', 'IPMI_HOST', str, '', 'IPMI lanplus address (default: host part of --bmc-url)'),
    ('--bmc-user', 'BMC_USER', 'BMC_USER', str, '', 'BMC account for Redfish and IPMI'),
    ('--bmc-password-env', None, 'BMC_PASSWORD_ENV', str, 'BMC_PASSWORD', 'name of the environment variable holding the BMC password'),
    ('--boot-orders', 'BMC_BOOT_ORDERS', 'NORMAL_ORDERS', _orders, '', 'accepted permanent Redfish BootOrder lists: ";" between lists, "," between entries'),
    ('--kvm-url', 'KVM_URL', 'KVM', str, '', 'IP-KVM base URL (PiKVM-style /api), e.g. https://kvm.example'),
    ('--kvm-user', 'KVM_USER', 'KVM_USER', str, 'admin', 'IP-KVM account'),
    ('--kvm-password-env', None, 'KVM_PASSWORD_ENV', str, 'KVM_PASSWORD', 'name of the environment variable holding the IP-KVM password'),
    ('--chromium', 'PLAYWRIGHT_CHROMIUM', 'CHROMIUM', str, '', 'Chromium binary for the KVM browser-login fallback (default: Playwright-managed)'),
    ('--guest-host', 'GUEST_HOST', 'GUEST', str, '', 'guest IP address for SSH; start-vm compares it with the IP VMware Tools reports'),
    ('--guest-user', 'GUEST_USER', 'GUEST_USER', str, 'root', 'guest SSH account (key auth, BatchMode)'),
    ('--image-src', 'WRITER_IMAGE', 'IMAGE_SRC', _path, '', 'local writer disk image to upload'),
    ('--image-name', 'WRITER_IMAGE_NAME', 'IMAGE_NAME', str, '', 'image name on the IP-KVM'),
    ('--image-sha256', 'WRITER_IMAGE_SHA256', 'IMAGE_SHA', _lower, '', 'sha256 of the built image; KVM read-backs must match it'),
    ('--image-size', 'WRITER_IMAGE_SIZE', 'IMAGE_SIZE', int, '67108864', 'image size in bytes'),
    ('--gpu-units', 'GUEST_GPU_UNITS', 'UNITS', _list, '', 'comma-separated guest systemd units (no .service suffix) that hold the GPUs'),
    ('--start-order', 'GUEST_START_ORDER', 'START_ORDER', _groups, '', 'start-services order: ";" between groups, "," between units started together'),
    ('--gpu-container', 'GUEST_GPU_CONTAINER', 'TEI', str, '', 'guest docker container holding a GPU (stopped by quiesce, started by start-services)'),
    ('--health-url', 'GUEST_HEALTH_URL', 'ROUTER_HEALTH', str, '', 'URL fetched with curl on the guest after start-services; must return 200'),
    ('--target-gpus', 'TARGET_GPUS', 'TARGETS', _targets, '', 'comma-separated NVML GPU UUIDs for target-probes (labelled target0, target1, ...)'),
    ('--probe-script', 'GUEST_PROBE', 'PROBE', str, '', 'path of cmp580_trial_probe.py on the guest'),
    ('--probe-python', 'GUEST_PROBE_PYTHON', 'PROBE_PYTHON', str, '', 'guest Python with torch + CUDA, passed to the probe as --python'),
    ('--guest-evidence-root', 'GUEST_EVIDENCE_ROOT', 'GUEST_EVIDENCE', str, '', 'guest directory for target-probe evidence (<root>/<window>/evidence-<utc>)'),
]
# Configuration each step needs beyond its transports (transports check their own in __init__).
STEP_NEEDS = {'upload': ('IMAGE_SRC', 'IMAGE_NAME', 'IMAGE_SHA'), 'attach': ('IMAGE_NAME',), 'reboot': ('IMAGE_NAME', 'IMAGE_SHA'),
              'detach-clear': ('IMAGE_NAME', 'IMAGE_SHA'), 'quiesce': ('UNITS', 'TEI'), 'start-services': ('UNITS', 'START_ORDER', 'TEI', 'ROUTER_HEALTH'),
              'target-probes': ('TARGETS', 'PROBE', 'PROBE_PYTHON', 'GUEST_EVIDENCE')}


def add_settings(ap):
    for flag, env, name, kind, default, text in SETTINGS:
        ap.add_argument(flag, default=(os.environ.get(env) if env else None) or default, help=text + (f' [env {env}]' if env else ''))


def apply_config(a):
    g = globals()
    for flag, env, name, kind, default, text in SETTINGS:
        g[name] = kind(getattr(a, flag[2:].replace('-', '_')))
    if not g['IPMI_HOST'] and g['BMC']:
        g['IPMI_HOST'] = urlparse(g['BMC']).hostname or ''


def require(*names):
    label = {s[2]: s[0] + (f' / {s[1]}' if s[1] else '') for s in SETTINGS}
    missing = [label.get(n, n) for n in names if not globals().get(n)]
    need(not missing, 'missing configuration: ' + ', '.join(missing))


def env_secret(var):
    value = os.environ.get(var, '') if var else ''
    need(value != '', f'password environment variable {var} is empty or unset')
    return value


def esxi_creds():
    require('ESXI_HOST', 'ESXI_USER')
    return {'host': ESXI_HOST, 'user': ESXI_USER, 'password': env_secret(ESXI_PASSWORD_ENV)}


def bmc_creds():
    require('BMC_USER')
    return {'user': BMC_USER, 'password': env_secret(BMC_PASSWORD_ENV)}


def kvm_creds():
    require('KVM_USER')
    return {'user': KVM_USER, 'password': env_secret(KVM_PASSWORD_ENV)}


def vsish_cmd(bus, index):
    return f'timeout -t 10 -s TERM vsish -e get /hardware/pci/seg/0/bus/{bus}/slot/0/func/0/BARInfo/{index}'


def parse_bar(raw):
    need(isinstance(raw, bytes) and 0 < len(raw) <= 4096, 'BAR output size')
    m = BAR_RE.fullmatch(raw.decode('ascii', 'replace'))
    need(m is not None, 'BAR output schema')
    return int(m.group(1), 16), int(m.group(2))


def physical_verdict(observed):
    """observed: {(bus,index): (address,size)}. Returns (verdict, rows)."""
    rows = []
    ok = set(observed) == set(EXPECTED)
    for key in sorted(EXPECTED):
        exp, got = EXPECTED[key], observed.get(key)
        match = got == exp
        ok = ok and match
        rows.append({'bus': f'{key[0]:02x}', 'bar': key[1], 'expected': [hex(exp[0]), exp[1]],
                     'observed': [hex(got[0]), got[1]] if got else None, 'match': match})
    return (PHYSICAL_OK_VERDICT if ok else 'PHYSICAL_LAYOUT_DIFFERS'), rows


def reboot_guard(census, maintenance, msd, bmc_boot, recorder_alive, live_attempt_exists):
    need(all(v == 'poweredOff' for v in census.values()), 'all listed VMs must be poweredOff')
    need(set(census) == set(VM_ON + VM_OFF), 'VM census differs from --vms-on/--vms-off')
    need(maintenance is True, 'host must be in maintenance mode')
    drive = msd.get('drive') or {}
    need(msd.get('enabled') is True and drive.get('connected') is True and drive.get('rw') is False
         and (drive.get('image') or {}).get('name') == IMAGE_NAME and (drive.get('image') or {}).get('size') == IMAGE_SIZE,
         'writer medium must be connected read-only')
    need((msd.get('drive_partition') or {}).get('connected') in (False, None), 'KVM partition must be disconnected')
    need(bmc_boot.get('BootSourceOverrideEnabled') == 'Once' and bmc_boot.get('BootSourceOverrideTarget') == 'Usb'
         and bmc_boot.get('BootSourceOverrideMode') == 'UEFI', 'one-time UEFI USB override must be read back')
    need(bmc_boot.get('BootOrder') in NORMAL_ORDERS, 'permanent boot order differs')
    need(recorder_alive is True, 'a console recorder must be running')
    need(not live_attempt_exists, 'LIVE_ATTEMPT.json exists: one writer boot per window')


def attach_guard(census, maintenance, msd):
    need(all(v == 'poweredOff' for v in census.values()) and set(census) == set(VM_ON + VM_OFF), 'all listed VMs must be poweredOff before media attach')
    need(maintenance is True, 'host must be in maintenance mode before media attach')
    drive = msd.get('drive') or {}
    need(msd.get('enabled') is True and (drive.get('image') or {}).get('name') == IMAGE_NAME
         and (drive.get('image') or {}).get('size') == IMAGE_SIZE and drive.get('rw') is False, 'uploaded read-only image must be selected')


def upload_verify(readback_sha):
    need(readback_sha == IMAGE_SHA, 'KVM read-back sha256 differs from the built image; refusing set_params')


class Journal:
    def __init__(self, window, root=None):
        need(re.fullmatch(r'[a-z0-9][a-z0-9-]{0,47}', window), 'invalid window name')
        self.window = window
        self.dir = (root or TW / 'artifacts') / f'two-card-window-{window}'
        self.dir.mkdir(mode=0o700, parents=True, exist_ok=True)

    def write(self, step, record, name=None, exclusive=False):
        record = dict(record, window=self.window, utc=utc().isoformat(), step=step)
        path = self.dir / (name or f'{step}-{stamp()}.json')
        flags = os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | (os.O_EXCL if exclusive else os.O_TRUNC)
        fd = os.open(path, flags, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write(json.dumps(record, indent=2, default=str) + '\n'); f.flush(); os.fsync(f.fileno())
        return path


# ---------------------------------------------------------------- transports
class Esxi:
    def __init__(self):
        require('ESXI_HOST', 'ESXI_UUID', 'ESXI_BUILD', 'GUEST_VM', 'VM_ON')
        from pyVim.connect import SmartConnect, Disconnect  # pyvmomi; put a vendored copy on PYTHONPATH if needed
        from pyVmomi import vim
        a = esxi_creds()
        # The ESXi API user is pinned to root on purpose: the host actions below assume it.
        need(a['host'] == ESXI_HOST and a['user'] == 'root', 'unexpected ESXi credential target')
        self.si = SmartConnect(host=a['host'], user=a['user'], pwd=a['password'], disableSslCertValidation=True, httpConnectionTimeout=12)
        del a
        self.vim, self._disconnect = vim, Disconnect
        self.host = vim.VirtualMachine(GUEST_VM, self.si._stub).runtime.host
        need(self.host.hardware.systemInfo.uuid.lower() == ESXI_UUID and self.host.config.product.build == ESXI_BUILD, 'ESXi identity differs')

    def close(self):
        self._disconnect(self.si)

    def vms(self):
        live = {v._moId: v for v in self.host.vm}
        need(set(live) == set(VM_ON + VM_OFF), 'VM inventory differs from --vms-on/--vms-off')
        return live

    def census(self):
        return {i: str(v.runtime.powerState) for i, v in self.vms().items()}

    def maintenance(self):
        return bool(self.host.runtime.inMaintenanceMode)

    def boot_time(self):
        return self.host.runtime.bootTime.isoformat()

    def wait_task(self, task, seconds=180):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            state = str(task.info.state)
            if state == 'success':
                return 'success'
            if state == 'error':
                raise RuntimeError('ESXi task error: ' + type(task.info.error).__name__)
            time.sleep(2)
        raise TimeoutError('task still pending; inspect before any retry')

    def shutdown_guest(self, moid):
        vm = self.vms()[moid]
        need(str(vm.runtime.powerState) == 'poweredOn' and str(vm.guest.toolsRunningStatus) == 'guestToolsRunning', f'VM {moid} not running with tools')
        vm.ShutdownGuest()

    def power_on(self, moid):
        vm = self.vms()[moid]
        need(str(vm.runtime.powerState) == 'poweredOff', f'VM {moid} not poweredOff')
        return self.wait_task(vm.PowerOnVM_Task())

    def guest_status(self, moid):
        vm = self.vms()[moid]
        return {'power': str(vm.runtime.powerState), 'tools': str(vm.guest.toolsRunningStatus), 'ip': vm.guest.ipAddress, 'boot': vm.runtime.bootTime.isoformat() if vm.runtime.bootTime else None}

    def enter_maintenance(self):
        return self.wait_task(self.host.EnterMaintenanceMode_Task(timeout=120))

    def exit_maintenance(self):
        return self.wait_task(self.host.ExitMaintenanceMode_Task(timeout=120))

    def reboot(self):
        task = self.host.RebootHost_Task(force=False)
        return task._moId

    def ssh_state(self):
        svc, fw = self.host.configManager.serviceSystem, self.host.configManager.firewallSystem
        svc.RefreshServices()
        s = [x for x in svc.serviceInfo.service if x.key == 'TSM-SSH']; r = [x for x in fw.firewallInfo.ruleset if x.key == 'sshServer']
        need(len(s) == 1 and len(r) == 1, 'SSH service/ruleset unknown')
        return {'service': {'running': bool(s[0].running), 'policy': str(s[0].policy)}, 'firewall': {'enabled': bool(r[0].enabled)}}

    def ssh_start(self):
        self.host.configManager.serviceSystem.StartService(id='TSM-SSH')

    def ssh_restore(self, original):
        svc, fw = self.host.configManager.serviceSystem, self.host.configManager.firewallSystem
        if self.ssh_state()['service']['running'] and not original['service']['running']:
            svc.StopService(id='TSM-SSH')
        if self.ssh_state()['firewall'] != original['firewall']:
            (fw.EnableRuleset if original['firewall']['enabled'] else fw.DisableRuleset)(id='sshServer')
        now = self.ssh_state(); need(now == original, 'SSH/firewall restoration differs'); return now


class EsxiShell:
    """Temporary paramiko session on the ESXi host while TSM-SSH is up."""
    def __init__(self):
        import paramiko
        a = esxi_creds()
        self.c = paramiko.SSHClient(); self.c.load_host_keys(os.path.expanduser('~/.ssh/known_hosts')); self.c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self.c.connect(a['host'], 22, a['user'], a['password'], allow_agent=False,  # (hostname, port, username, password)
                       look_for_keys=False, timeout=8, auth_timeout=8, banner_timeout=8)
        del a

    def run(self, cmd, timeout=20):
        _, o, e = self.c.exec_command(cmd, timeout=timeout)
        out, err = o.read(), e.read(); rc = o.channel.recv_exit_status()
        return rc, out, err

    def close(self):
        self.c.close()


class Redfish:
    def __init__(self):
        require('BMC', 'ESXI_UUID', 'NORMAL_ORDERS')
        self.URL = BMC + '/redfish/v1/Systems/Self'
        import requests, urllib3
        urllib3.disable_warnings()
        a = bmc_creds()
        self.s = requests.Session(); self.s.auth = (a['user'], a['password']); self.s.verify = False; del a

    def get(self):
        r = self.s.get(self.URL, timeout=15, allow_redirects=False); r.raise_for_status(); d = r.json()
        need(d['UUID'].lower() == ESXI_UUID, 'BMC system UUID differs'); need(d['Boot']['BootOrder'] in NORMAL_ORDERS, 'permanent boot order differs')
        return d

    def boot(self):
        d = self.get(); return {k: d['Boot'].get(k) for k in ('BootSourceOverrideEnabled', 'BootSourceOverrideTarget', 'BootSourceOverrideMode', 'BootOrder')} | {'PowerState': d.get('PowerState')}

    def patch(self, payload):
        d = self.get()
        r = self.s.patch(self.URL, json=payload, headers={'If-Match': d['@odata.etag']}, timeout=15); r.raise_for_status()
        now = self.get()['Boot']
        for k, v in payload['Boot'].items():
            need(now.get(k) == v, f'BMC readback {k} differs')
        return r.status_code

    def set_usb_once(self):
        d = self.get(); need('Usb' in d['Boot']['BootSourceOverrideTarget@Redfish.AllowableValues'], 'Usb override not allowed')
        return self.patch({'Boot': {'BootSourceOverrideEnabled': 'Once', 'BootSourceOverrideMode': 'UEFI', 'BootSourceOverrideTarget': 'Usb'}})

    def clear(self):
        return self.patch({'Boot': {'BootSourceOverrideEnabled': 'Disabled', 'BootSourceOverrideTarget': 'None'}})


class Ipmi:
    def __init__(self):
        require('IPMI_HOST')
        a = bmc_creds()
        self.env = dict(os.environ, IPMI_PASSWORD=a['password']); self.user = a['user']; del a

    def run(self, action):
        need(action in ('status', 'on', 'off'), 'IPMI action not permitted')
        r = subprocess.run(['ipmitool', '-I', 'lanplus', '-H', IPMI_HOST, '-U', self.user, '-E', 'chassis', 'power', action],
                           env=self.env, capture_output=True, text=True, timeout=20)
        need(r.returncode == 0 and not r.stderr.strip(), 'IPMI request failed')
        return r.stdout.strip()


class Kvm:
    def __init__(self):
        require('KVM')
        import requests, urllib3
        urllib3.disable_warnings()
        self.s = requests.Session(); self.s.verify = False
        a = kvm_creds()
        r = self.s.post(KVM + '/api/auth/login', data={'user': a['user'], 'passwd': a['password']}, timeout=20)
        if r.status_code != 200:
            self._browser_login(a['password'])
        del a
        chk = self.s.get(KVM + '/api/auth/check', timeout=15); need(chk.status_code == 200, 'KVM session not authenticated')

    def _browser_login(self, password):
        from playwright.sync_api import sync_playwright
        with sync_playwright() as p:
            b = p.chromium.launch(headless=True, executable_path=CHROMIUM or None, args=['--no-sandbox'])
            c = b.new_context(ignore_https_errors=True); page = c.new_page()
            try:
                page.goto(KVM + '/', wait_until='domcontentloaded', timeout=15000)
                page.locator('#form_item_passwd').wait_for(timeout=10000); page.locator('#form_item_passwd').fill(password)
                page.get_by_role('button', name='Log In', exact=True).click(timeout=5000); page.wait_for_timeout(5000)
                for ck in c.cookies():
                    self.s.cookies.set(ck['name'], ck['value'], domain=ck.get('domain') or urlparse(KVM).hostname)
            finally:
                b.close()

    def json(self, path, method='GET', **kw):
        r = self.s.request(method, KVM + path, timeout=kw.pop('timeout', 30), **kw); r.raise_for_status(); d = r.json()
        need(d.get('ok') is True, f'KVM {path} not ok: {str(d)[:200]}'); return d['result']

    def msd(self):
        return self.json('/api/msd')

    def streamer(self):
        return self.json('/api/streamer')

    def snapshot(self):
        r = self.s.get(KVM + '/api/streamer/snapshot', timeout=15); r.raise_for_status()
        need(r.headers.get('content-type', '').startswith('image/'), 'snapshot is not an image'); return r.content

    def set_enabled(self, value):
        return self.json('/api/msd/set_enabled', 'POST', params={'enabled': 'true' if value else 'false'}, timeout=60)

    def write(self, name, raw):
        return self.json('/api/msd/write', 'POST', params={'image': name}, data=raw, headers={'Content-Type': 'application/octet-stream'}, timeout=900)

    def read(self, name):
        r = self.s.get(KVM + '/api/msd/read', params={'image': name}, timeout=900); r.raise_for_status(); return r.content

    def set_params(self, name):
        return self.json('/api/msd/set_params', 'POST', params={'image': name, 'cdrom': 'false', 'rw': 'false'}, timeout=60)

    def set_connected(self, value):
        return self.json('/api/msd/set_connected', 'POST', params={'connected': 'true' if value else 'false'}, timeout=60)


class Guest:
    def __init__(self):
        require('GUEST', 'GUEST_USER')

    def run(self, cmd, timeout=60):
        r = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', GUEST_USER + '@' + GUEST, cmd], capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr

    def reachable(self):
        try:
            return self.run('hostname', timeout=15)[0] == 0
        except subprocess.TimeoutExpired:
            return False


# ------------------------------------------------------------------- driver
class Driver:
    def __init__(self, window, execute, root=None, transports=None):
        self.j = Journal(window, root); self.execute = execute; self.window = window
        self.t = transports or {}

    def get(self, name, factory):
        if name not in self.t:
            self.t[name] = factory()
        return self.t[name]

    def esxi(self): return self.get('esxi', Esxi)
    def redfish(self): return self.get('redfish', Redfish)
    def ipmi(self): return self.get('ipmi', Ipmi)
    def kvm(self): return self.get('kvm', Kvm)
    def guest(self): return self.get('guest', Guest)

    def say(self, msg):
        print(json.dumps(msg, indent=2, default=str), flush=True)

    def host_facts(self):
        e = self.esxi(); return {'census': e.census(), 'maintenance': e.maintenance(), 'host_boot': e.boot_time()}

    def recorder_alive(self):
        pid_file = self.j.dir / 'recorder.pid'
        if not pid_file.exists():
            return False
        try:
            pid = int(pid_file.read_text().strip()); os.kill(pid, 0)
        except (ValueError, ProcessLookupError, PermissionError):
            return False
        frames = self.j.dir / 'console-kvm' / 'frames.jsonl'
        return frames.exists() and time.time() - frames.stat().st_mtime < 10

    # -- read-only steps
    def preflight(self):
        f = self.host_facts(); f['ipmi'] = self.ipmi().run('status')
        k = self.kvm(); f['streamer'] = k.streamer(); f['msd'] = k.msd()
        jpg = k.snapshot(); p = self.j.dir / f'preflight-snapshot-{stamp()}.jpg'; p.write_bytes(jpg); f['snapshot'] = {'path': str(p), 'sha256': sha(jpg), 'bytes': len(jpg)}
        g = self.guest()
        f['guest_hook_check'] = g.run('/usr/local/sbin/cmp-bar1-repair --check 2>&1; echo rc=$?', 60)[1]
        f['guest_modprobe_hook'] = g.run('modprobe -c | grep "install nvidia"', 20)[1].strip()
        checks = {'vms_on': all(f['census'].get(v) == 'poweredOn' for v in VM_ON), 'vms_off': all(f['census'].get(v) == 'poweredOff' for v in VM_OFF),
                  'maintenance_false': f['maintenance'] is False, 'ipmi_on': f['ipmi'] == 'Chassis Power is on',
                  'streamer_online': bool(f['streamer']), 'hook_installed': 'install nvidia' in f['guest_modprobe_hook'],
                  'hook_check_ok': 'rc=0' in f['guest_hook_check']}
        f['checks'] = checks; f['verdict'] = 'PASS' if all(checks.values()) else 'FAIL'
        self.j.write('preflight', f); self.say(f); return f['verdict'] == 'PASS'

    def status(self):
        f = self.host_facts(); f['ipmi'] = self.ipmi().run('status'); f['bmc_boot'] = self.redfish().boot(); f['msd'] = self.kvm().msd()
        f['recorder_alive'] = self.recorder_alive(); f['live_attempt'] = (self.j.dir / 'LIVE_ATTEMPT.json').exists()
        self.j.write('status', f); self.say(f); return True

    # -- media
    def upload(self):
        raw = IMAGE_SRC.read_bytes(); need(sha(raw) == IMAGE_SHA and len(raw) == IMAGE_SIZE, 'local image bytes differ from the built image')
        k = self.kvm(); before = k.msd(); rec = {'image': IMAGE_NAME, 'sha256': IMAGE_SHA, 'before': before}
        self.say({'would': ['set_enabled true', f'write {IMAGE_NAME}', 'read back + sha256', 'set_params cdrom=false rw=false'], 'before': before})
        if not self.execute:
            return True
        if before.get('enabled') is not True:
            k.set_enabled(True)
        need(k.msd().get('enabled') is True, 'KVM MSD not enabled')
        need((k.msd().get('drive') or {}).get('connected') is not True, 'a medium is still connected; detach first')
        rec['write'] = k.write(IMAGE_NAME, raw); back = k.read(IMAGE_NAME); rec['readback_sha256'] = sha(back); rec['readback_bytes'] = len(back)
        upload_verify(rec['readback_sha256'])
        k.set_params(IMAGE_NAME); after = k.msd(); rec['after'] = after; drive = after.get('drive') or {}
        need(drive.get('connected') is False and drive.get('rw') is False and (drive.get('image') or {}).get('name') == IMAGE_NAME
             and (drive.get('image') or {}).get('size') == IMAGE_SIZE, 'selected medium readback differs')
        rec['verdict'] = 'MEDIA_VERIFIED'; self.j.write('upload', rec, name='MEDIA_VERIFIED.json'); self.say(rec); return True

    def attach(self):
        f = self.host_facts(); msd = self.kvm().msd(); attach_guard(f['census'], f['maintenance'], msd)
        self.say({'would': 'set_connected true', 'facts': f, 'msd': msd})
        if not self.execute:
            return True
        self.kvm().set_connected(True); after = self.kvm().msd(); drive = after.get('drive') or {}
        need(drive.get('connected') is True and drive.get('rw') is False and (after.get('drive_partition') or {}).get('connected') is not True, 'attach readback differs')
        self.j.write('attach', {'after': after}, name='MEDIA_ATTACHED.json'); self.say(after); return True

    def detach_clear(self):
        k = self.kvm(); rec = {'before': k.msd(), 'bmc_before': self.redfish().boot()}
        self.say({'would': ['set_connected false', 'read back sha', 'set_enabled false', 'Redfish clear override'], **rec})
        if not self.execute:
            return True
        if (rec['before'].get('drive') or {}).get('connected'):
            k.set_connected(False)
        if rec['before'].get('enabled'):
            back = k.read(IMAGE_NAME); rec['readback_sha256'] = sha(back); need(rec['readback_sha256'] == IMAGE_SHA, 'medium changed after boot')
            k.set_enabled(False)
        after = k.msd(); need(after.get('enabled') is False and not after.get('drive'), 'KVM media not disabled'); rec['after'] = after
        b = rec['bmc_before']
        if not (b['BootSourceOverrideEnabled'] == 'Disabled' and b['BootSourceOverrideTarget'] == 'None'):
            rec['bmc_patch_status'] = self.redfish().clear()
        rec['bmc_after'] = self.redfish().boot()
        need(rec['bmc_after']['BootSourceOverrideEnabled'] == 'Disabled' and rec['bmc_after']['BootSourceOverrideTarget'] == 'None' and rec['bmc_after']['BootSourceOverrideMode'] == 'UEFI', 'override not cleared')
        self.j.write('detach-clear', rec, name=f'MEDIA_DISABLED-{stamp()}.json'); self.say(rec); return True

    def bootusb(self):
        f = self.host_facts(); need(all(v == 'poweredOff' for v in f['census'].values()) and f['maintenance'], 'all-off + maintenance required before boot override')
        rec = {'before': self.redfish().boot()}; self.say({'would': 'Redfish Once/UEFI/Usb', **rec})
        if not self.execute:
            return True
        rec['http_status'] = self.redfish().set_usb_once(); rec['after'] = self.redfish().boot(); self.j.write('bootusb', rec); self.say(rec); return True

    # -- guest service control
    def quiesce(self):
        g = self.guest(); self.say({'would': [f'docker stop -t 20 {TEI}', 'systemctl stop ' + ' '.join(UNITS), 'wait inactive <=240 s', 'require no /dev/nvidia* holders']})
        if not self.execute:
            self.say({'active': g.run('systemctl is-active ' + ' '.join(u + '.service' for u in UNITS), 20)[1].split()}); return True
        rec = {}
        rec['tei'] = g.run(f'docker stop -t 20 {TEI}; docker inspect -f "{{{{.State.Running}}}}" {TEI}', 60)[1].strip()
        g.run('systemctl stop --no-block ' + ' '.join(u + '.service' for u in UNITS), 20)
        end = time.monotonic() + 240
        while time.monotonic() < end:
            vals = g.run('systemctl is-active ' + ' '.join(u + '.service' for u in UNITS), 20)[1].split()
            if len(vals) == len(UNITS) and all(v in ('inactive', 'failed') for v in vals):
                break
            time.sleep(5)
        else:
            rec['units'] = vals; self.j.write('quiesce', rec | {'verdict': 'TIMEOUT'}); raise Refused('units still stopping; inspect, do not blindly retry')
        rec['units'] = dict(zip(UNITS, vals))
        holders = []
        for _ in range(2):
            holders.append(g.run('lsof -t /dev/nvidia* 2>/dev/null | sort -u | tr "\\n" " "', 30)[1].split()); time.sleep(3)
        persistent = sorted(set(holders[0]) & set(holders[1])); rec['nvidia_holders'] = persistent
        need(rec['tei'].split()[-1:] == ['false'] and not persistent, f'GPU still held by PIDs {persistent} or TEI running')
        rec['verdict'] = 'QUIESCED'; self.j.write('quiesce', rec); self.say(rec); return True

    def start_services(self):
        g = self.guest(); self.say({'would': START_ORDER + [[f'docker start {TEI}'], ['wait active', ROUTER_HEALTH]]})
        if not self.execute:
            return True
        rec = {'started': []}
        for group in START_ORDER:
            g.run('systemctl start ' + ' '.join(u + '.service' for u in group), 120); rec['started'].append(group)
        rec['tei'] = g.run(f'docker start {TEI} && docker inspect -f "{{{{.State.Running}}}}" {TEI}', 60)[1].strip()
        end = time.monotonic() + 400; health = None
        while time.monotonic() < end:
            vals = g.run('systemctl is-active ' + ' '.join(u + '.service' for u in UNITS), 20)[1].split()
            health = g.run(f'curl -fsS -m 5 -o /dev/null -w "%{{http_code}}" {ROUTER_HEALTH}', 15)[1].strip()
            if len(vals) == len(UNITS) and all(v == 'active' for v in vals) and health == '200':
                break
            time.sleep(10)
        rec['units'] = dict(zip(UNITS, vals)); rec['router_health'] = health
        rec['verdict'] = 'PASS' if all(v == 'active' for v in vals) and health == '200' and 'true' in rec['tei'] else 'INCOMPLETE'
        self.j.write('start-services', rec); self.say(rec); return rec['verdict'] == 'PASS'

    # -- VM and host power
    def shutdown_vms(self):
        e = self.esxi(); f = self.host_facts(); self.say({'would': f'ShutdownGuest {", ".join(VM_ON)} (one request each)', 'facts': f})
        if not self.execute:
            return True
        rec = {'requests': []}
        for moid in VM_ON:
            if f['census'][moid] == 'poweredOn':
                rec['requests'].append({'vm': moid, 'utc': utc().isoformat()}); self.j.write('shutdown-vms', rec | {'phase': 'REQUESTING'})
                e.shutdown_guest(moid)
        end = time.monotonic() + 600
        while time.monotonic() < end:
            c = e.census(); rec['census'] = c
            if all(v == 'poweredOff' for v in c.values()):
                rec['verdict'] = 'ALL_SIX_OFF'; self.j.write('shutdown-vms', rec); self.say(rec); return True
            time.sleep(20)
        rec['verdict'] = 'TIMEOUT'; self.j.write('shutdown-vms', rec); raise Refused('VMs still on after 600 s; inspect, never resend ShutdownGuest blindly')

    def enter_maintenance(self):
        f = self.host_facts(); need(all(v == 'poweredOff' for v in f['census'].values()), 'all listed VMs must be off'); need(not f['maintenance'], 'already in maintenance')
        self.say({'would': 'EnterMaintenanceMode', 'facts': f})
        if self.execute:
            r = self.esxi().enter_maintenance(); self.j.write('enter-maintenance', {'task': r, 'maintenance': self.esxi().maintenance()})
        return True

    def exit_maintenance(self):
        f = self.host_facts(); need(f['maintenance'], 'not in maintenance'); self.say({'would': 'ExitMaintenanceMode', 'facts': f})
        if self.execute:
            r = self.esxi().exit_maintenance(); self.j.write('exit-maintenance', {'task': r, 'maintenance': self.esxi().maintenance()})
        return True

    def record(self, seconds):
        need(0 < seconds <= 3600, 'seconds out of range')
        out = self.j.dir / 'console-kvm'; self.say({'would': f'poll {KVM}/api/streamer/snapshot every 0.75 s for {seconds} s into {out}'})
        if not self.execute:
            return True
        need(not self.recorder_alive(), 'a recorder is already running')
        log = open(self.j.dir / 'recorder.log', 'ab')
        p = subprocess.Popen([sys.executable, '-B', __file__, 'record-worker', '--window', self.window, '--seconds', str(seconds), '--root', str(self.j.dir.parent),
                              f'--kvm-url={KVM}', f'--kvm-user={KVM_USER}', f'--kvm-password-env={KVM_PASSWORD_ENV}', f'--chromium={CHROMIUM}'],
                             stdout=log, stderr=log, start_new_session=True)
        (self.j.dir / 'recorder.pid').write_text(str(p.pid)); time.sleep(4)
        need(self.recorder_alive(), 'recorder did not start'); self.j.write('record', {'pid': p.pid, 'seconds': seconds, 'output': str(out)}); return True

    def record_worker(self, seconds):
        k = self.kvm(); out = self.j.dir / 'console-kvm'; out.mkdir(mode=0o700, exist_ok=True)
        start = time.monotonic(); count = len(list(out.glob('*.jpg'))); last = None
        with (out / 'frames.jsonl').open('a') as log:
            while time.monotonic() - start < seconds and not (out / 'STOP').exists():
                try:
                    raw = k.snapshot(); h = sha(raw)
                    if h != last:
                        (out / f'{count:05d}.jpg').write_bytes(raw); count += 1
                    log.write(json.dumps({'utc': utc().isoformat(), 'frame': count - 1, 'sha256': h, 'bytes': len(raw), 'new': h != last}) + '\n'); log.flush(); last = h
                except Exception as exc:
                    log.write(json.dumps({'utc': utc().isoformat(), 'error': type(exc).__name__}) + '\n'); log.flush()
                time.sleep(0.75)
        return True

    def reboot(self, confirm):
        f = self.host_facts(); msd = self.kvm().msd(); boot = self.redfish().boot(); alive = self.recorder_alive(); attempt = self.j.dir / 'LIVE_ATTEMPT.json'
        reboot_guard(f['census'], f['maintenance'], msd, boot, alive, attempt.exists())
        rec = {'host_boot_before': f['host_boot'], 'image': IMAGE_NAME, 'image_sha256': IMAGE_SHA, 'census': f['census'], 'maintenance': f['maintenance'], 'bmc_boot': boot,
               'msd_drive': msd.get('drive'), 'ipmi': self.ipmi().run('status'), 'outcome': 'PENDING'}
        self.say({'would': 'RebootHost(force=False) exactly once', **rec})
        if not self.execute:
            return True
        need(confirm, '--i-confirm-single-boot required')
        self.j.write('reboot', rec, name='LIVE_ATTEMPT.json', exclusive=True)
        rec['task_id'] = self.esxi().reboot(); rec['submitted_utc'] = utc().isoformat(); rec['outcome'] = 'SUBMITTED'
        self.j.write('reboot', rec, name='LIVE_ATTEMPT.json'); self.say(rec); return True

    def wait_esxi(self, previous_boot):
        if previous_boot is None:
            p = self.j.dir / 'LIVE_ATTEMPT.json'; need(p.exists(), 'no LIVE_ATTEMPT.json; pass --previous-boot'); previous_boot = json.loads(p.read_text())['host_boot_before']
        end = time.monotonic() + 900; rec = {'previous_boot': previous_boot, 'polls': 0}
        while time.monotonic() < end:
            rec['polls'] += 1
            try:
                self.t.pop('esxi', None); e = self.esxi(); boot = e.boot_time()
                if boot > previous_boot:
                    rec.update(host_boot=boot, maintenance=e.maintenance(), census=e.census(), verdict='ESXI_RETURNED'); self.j.write('wait-esxi', rec); self.say(rec); return True
                rec['last_seen_boot'] = boot
            except Exception as exc:
                rec['last_error'] = type(exc).__name__
            time.sleep(15)
        rec['verdict'] = 'TIMEOUT'; self.j.write('wait-esxi', rec); raise Refused('ESXi API did not return with a newer boot in 900 s; check the console frames')

    def physical_check(self):
        e = self.esxi(); f = self.host_facts(); need(all(v == 'poweredOff' for v in f['census'].values()) and f['maintenance'], 'all-off + maintenance required for physical reads')
        original = e.ssh_state(); rec = {'host_boot': f['host_boot'], 'ssh_before': original, 'commands': []}
        self.say({'would': [vsish_cmd(b, i) for b, i in sorted(EXPECTED)], 'ssh_before': original})
        if not self.execute:
            return True
        need(original['service']['running'] is False, 'TSM-SSH already running; refusing to own its restoration')
        observed = {}
        try:
            e.ssh_start(); need(e.ssh_state()['service']['running'], 'SSH start not confirmed'); sh = EsxiShell()
            try:
                for key in sorted(EXPECTED):
                    cmd = vsish_cmd(*key); rc, out, err = sh.run(cmd); raw_dir = self.j.dir / 'physical-raw'; raw_dir.mkdir(exist_ok=True)
                    (raw_dir / f'{key[0]:02x}-{key[1]}.stdout').write_bytes(out)
                    rec['commands'].append({'command': cmd, 'exit_code': rc, 'stdout_sha256': sha(out), 'stderr_bytes': len(err)})
                    need(rc == 0 and not err, f'BAR read failed for {key}'); observed[key] = parse_bar(out)
            finally:
                sh.close()
        finally:
            rec['ssh_restored'] = e.ssh_restore(original); rec['restoration'] = 'PASS'
        rec['verdict'], rec['rows'] = physical_verdict(observed); self.j.write('physical-check', rec, name=f'PHYSICAL_RESULT-{stamp()}.json'); self.say(rec)
        return rec['verdict'] == PHYSICAL_OK_VERDICT

    def start_vm(self, moid):
        need(moid in VM_ON, f'only {", ".join(VM_ON)} may be started'); f = self.host_facts(); need(not f['maintenance'], 'exit maintenance first')
        self.say({'would': f'PowerOnVM {moid}', 'facts': f})
        if not self.execute:
            return True
        e = self.esxi(); rec = {'vm': moid, 'task': e.power_on(moid)}
        end = time.monotonic() + 600
        while time.monotonic() < end:
            s = e.guest_status(moid); rec['guest'] = s
            if s['tools'] == 'guestToolsRunning' and s['ip'] and (moid != GUEST_VM or (s['ip'] == GUEST and self.guest().reachable())):
                rec['verdict'] = 'UP'; self.j.write(f'start-vm-{moid}', rec); self.say(rec); return True
            time.sleep(15)
        rec['verdict'] = 'NOT_READY'; self.j.write(f'start-vm-{moid}', rec); raise Refused(f'VM {moid} not ready in 600 s')

    def guest_check(self):
        g = self.guest(); rec = {}
        rec['boot_id'] = g.run('cat /proc/sys/kernel/random/boot_id', 15)[1].strip()
        rec['resource1'] = {d: int(g.run(f'stat -c %s /sys/bus/pci/devices/0000:{d}/resource1', 15)[1].strip() or 0) for d in BIG_GUEST + SMALL_GUEST}
        rec['hook_journal'] = g.run('journalctl -b -t cmp-bar1-repair --no-pager 2>&1 | tail -40', 30)[1]
        rec['hook_check'] = g.run('/usr/local/sbin/cmp-bar1-repair --check 2>&1', 60)[1]
        xml = g.run('nvidia-smi -q -x', 60)[1]; rec['bar1_mib'] = {}
        try:
            for gpu in ET.fromstring(xml).iter('gpu'):
                bus = gpu.get('id', '').lower()[-7:]; total = gpu.findtext('bar1_memory_usage/total') or ''
                rec['bar1_mib'][bus] = int(total.split()[0]) if total.split() and total.split()[0].isdigit() else total
        except ET.ParseError:
            rec['bar1_mib'] = {'parse_error': xml[:300]}
        rec['verdict'] = 'PASS' if all(rec['resource1'].get(d) == 64 << 30 and rec['bar1_mib'].get(d) == 65536 for d in BIG_GUEST) else 'FAIL'
        self.j.write('guest-check', rec); self.say(rec); return rec['verdict'] == 'PASS'

    def target_probes(self):
        g = self.guest(); evidence = f'{GUEST_EVIDENCE}/{self.window}/evidence-{stamp()}'; rec = {'stages': [], 'evidence_dir': evidence}
        plan = []
        for uuid, label in TARGETS:
            base = f'python3 {PROBE} --gpu {uuid} --python {PROBE_PYTHON} --expect-memory-mib 65536 --timeout 60 --deadline 90 --max-temp 78'
            plan.append((f'{label}-small', f'{base} --mode small --output-dir {evidence}/{label}-small'))
            plan.append((f'{label}-vram60', f'{base} --mode vram --gib 60 --chunk-mib 32 --output-dir {evidence}/{label}-vram60'))
        self.say({'would': [c for _, c in plan]})
        if not self.execute:
            return True
        boot = g.run('cat /proc/sys/kernel/random/boot_id', 15)[1].strip(); g.run(f'mkdir -p {evidence}', 15)
        for name, cmd in plan:
            rc, out, err = g.run(cmd, 300); summ = g.run(f'cat {evidence}/{name}/summary.json 2>/dev/null', 15)[1]
            try:
                s = json.loads(summ)
            except json.JSONDecodeError:
                s = {}
            ok = rc == 0 and s.get('verdict') == 'PASS' and s.get('boot_id') == boot and s.get('failures') == []
            rec['stages'].append({'stage': name, 'exit_code': rc, 'verdict': s.get('verdict'), 'boot_match': s.get('boot_id') == boot, 'failures': s.get('failures'), 'ok': ok, 'stderr_tail': err[-400:]})
            if not ok:
                rec['verdict'] = 'FAIL'; self.j.write('target-probes', rec); self.say(rec); return False
        rec['verdict'] = 'PASS'; self.j.write('target-probes', rec); self.say(rec); return True

    def ipmi_power(self, action):
        before = self.ipmi().run('status'); rec = {'action': action, 'before': before}
        self.say({'would': f'ipmitool chassis power {action}', **rec, 'bmc': self.redfish().boot()})
        if not self.execute:
            return True
        if action == 'off':
            need(before == 'Chassis Power is on', 'host already off'); need(not (self.kvm().msd().get('drive') or {}).get('connected'), 'detach media before a power-off')
        else:
            need(before == 'Chassis Power is off', 'fresh IPMI off required before on')
            time.sleep(10); need(self.ipmi().run('status') == 'Chassis Power is off', 'off not stable over 10 s dwell')
            b = self.redfish().boot(); need(b['BootSourceOverrideEnabled'] == 'Disabled' and b['BootOrder'] in NORMAL_ORDERS, 'clear the override before a normal power-on')
        self.j.write(f'ipmi-{action}', rec | {'phase': 'SUBMITTING'}); rec['result'] = self.ipmi().run(action); time.sleep(3); rec['after'] = self.ipmi().run('status')
        self.j.write(f'ipmi-{action}', rec); self.say(rec); return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('step', choices=['preflight', 'status', 'upload', 'quiesce', 'shutdown-vms', 'enter-maintenance', 'attach', 'bootusb', 'record', 'record-worker', 'reboot',
                                     'wait-esxi', 'physical-check', 'detach-clear', 'exit-maintenance', 'start-vm', 'guest-check', 'target-probes', 'start-services', 'ipmi-off', 'ipmi-on'])
    ap.add_argument('--window', required=True); ap.add_argument('--execute', action='store_true'); ap.add_argument('--i-confirm-single-boot', action='store_true'); ap.add_argument('--profile', choices=list(PROFILES), default='two-card')
    ap.add_argument('--vm', help='VM moid for start-vm (one of --vms-on)'); ap.add_argument('--seconds', type=int, default=900); ap.add_argument('--previous-boot'); ap.add_argument('--root', type=Path)
    add_settings(ap)
    a = ap.parse_args(argv); os.umask(0o077); sys.dont_write_bytecode = True; apply_profile(a.profile); apply_config(a)
    if a.vm is not None and a.vm not in VM_ON:
        ap.error(f'--vm must be one of --vms-on {list(VM_ON)}')
    if GUEST_VM and VM_ON and GUEST_VM not in VM_ON:
        ap.error('--guest-vm must be one of --vms-on')
    d = Driver(a.window, a.execute, a.root)
    try:
        require(*STEP_NEEDS.get(a.step, ()))
        ok = {'preflight': d.preflight, 'status': d.status, 'upload': d.upload, 'quiesce': d.quiesce, 'shutdown-vms': d.shutdown_vms, 'enter-maintenance': d.enter_maintenance,
              'attach': d.attach, 'bootusb': d.bootusb, 'record': lambda: d.record(a.seconds), 'record-worker': lambda: d.record_worker(a.seconds),
              'reboot': lambda: d.reboot(a.i_confirm_single_boot), 'wait-esxi': lambda: d.wait_esxi(a.previous_boot), 'physical-check': d.physical_check,
              'detach-clear': d.detach_clear, 'exit-maintenance': d.exit_maintenance, 'start-vm': lambda: d.start_vm(a.vm), 'guest-check': d.guest_check,
              'target-probes': d.target_probes, 'start-services': d.start_services, 'ipmi-off': lambda: d.ipmi_power('off'), 'ipmi-on': lambda: d.ipmi_power('on')}[a.step]()
        return 0 if ok else 1
    except Refused as exc:
        d.j.write(a.step, {'refused': str(exc), 'execute': a.execute}, name=f'REFUSED-{a.step}-{stamp()}.json'); print('REFUSED: ' + str(exc), file=sys.stderr); return 2
    finally:
        if 'esxi' in d.t and hasattr(d.t['esxi'], 'close'):
            d.t['esxi'].close()


if __name__ == '__main__':
    raise SystemExit(main())
