#!/usr/bin/env python3
"""One explicitly selected CUDA trial with bounded, retained evidence.

No installs, service changes, retries, resets, or power changes. A failure is a
stop condition: an unkillable child requires recovery, never another GPU probe.
Only the optional vram mode tests addresses beyond 8 GiB; neither mode is a soak.
Requires the unchanged gpu_probe.py beside this script for bounded CPU commands.
"""
import argparse
import csv
import datetime
import gc
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from gpu_probe import UUID_RE, GPU_ERROR, run_command, signal_group

STAGES = ['import', 'init', 'device', 'capability', 'alloc', 'fill', 'sync', 'readback', 'free', 'exit']
CMP_UUIDS = {
    'GPU-00000000-0000-0000-0000-000000000000', 'GPU-00000000-0000-0000-0000-000000000000',
    'GPU-00000000-0000-0000-0000-000000000000', 'GPU-00000000-0000-0000-0000-000000000000'}
GIB = 1024 ** 3


class Failed(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise Failed(message)


def emit(stage, phase, **details):
    print(json.dumps(dict(stage=stage, phase=phase, monotonic=time.monotonic(), **details)), flush=True)


def pattern_pass(torch, allocation, chunk_elements, salt, verify, pass_index):
    """Touch every int64 word; keep temporary GPU storage bounded to one chunk."""
    for begin in range(0, allocation.numel(), chunk_elements):
        end = min(begin + chunk_elements, allocation.numel())
        expected = torch.arange(begin, end, dtype=torch.int64, device='cuda:0')
        expected.bitwise_xor_(salt)
        view = allocation[begin:end]
        if verify:
            require(torch.equal(view, expected), 'VRAM address-dependent comparison failed at byte ' + str(begin * 8))
        else:
            view.copy_(expected)
        torch.cuda.synchronize()
        del view, expected
        emit('stripe', 'verify' if verify else 'write', pass_index=pass_index,
             offset_bytes=begin * 8, length_bytes=(end - begin) * 8)


def cuda_child(config):
    emit('import', 'begin')
    import torch
    torch.set_num_threads(1)
    emit('import', 'end', torch_version=str(torch.__version__))
    emit('init', 'begin')
    torch.cuda.init()
    emit('init', 'end')
    emit('device', 'begin')
    require(torch.cuda.device_count() == 1, 'expected exactly one UUID-selected CUDA device')
    torch.cuda.set_device(0)
    properties = torch.cuda.get_device_properties(0)
    emit('device', 'end', name=properties.name, total_memory=properties.total_memory)
    emit('capability', 'begin')
    capability = list(torch.cuda.get_device_capability(0))
    require(len(capability) == 2 and capability[0] > 0, 'invalid CUDA capability')
    emit('capability', 'end', capability=capability)
    emit('alloc', 'begin')
    total_bytes = config['gib'] * GIB if config['mode'] == 'vram' else 8192
    if config['mode'] == 'vram':
        free_bytes, _ = torch.cuda.mem_get_info()
        require(free_bytes >= total_bytes + 512 * 1024 ** 2, 'insufficient free VRAM plus 512 MiB reserve')
    allocation = torch.empty(total_bytes // 8, dtype=torch.int64, device='cuda:0')
    emit('alloc', 'end', allocated_bytes=total_bytes)
    emit('fill', 'begin')
    if config['mode'] == 'vram':
        pattern_pass(torch, allocation, config['chunk_mib'] * 1024 ** 2 // 8,
                     0x13579BDF2468ACE, False, 0)
    else:
        allocation.fill_(3)
        allocation.mul_(7).add_(2)
    emit('fill', 'end')
    emit('sync', 'begin')
    torch.cuda.synchronize()
    emit('sync', 'end')
    emit('readback', 'begin')
    if config['mode'] == 'vram':
        chunk = config['chunk_mib'] * 1024 ** 2 // 8
        pattern_pass(torch, allocation, chunk, 0x13579BDF2468ACE, True, 0)
        pattern_pass(torch, allocation, chunk, 0x7ECA86420DB97531, False, 1)
        pattern_pass(torch, allocation, chunk, 0x7ECA86420DB97531, True, 1)
    else:
        require(allocation.sum().item() == 23552 and allocation[0].item() == 23
                and allocation[-1].item() == 23, 'small CUDA arithmetic mismatch')
    emit('readback', 'end')
    emit('free', 'begin')
    del allocation
    gc.collect()
    torch.cuda.empty_cache()
    torch.cuda.synchronize()
    emit('free', 'end')
    # These markers are insufficient by themselves: the parent requires exit 0.
    emit('exit', 'begin')
    emit('exit', 'end')


def start_owned(argv, directory, name, env=None):
    record = {'name': name, 'argv': argv, 'returncode': None, 'unreaped': False,
              'stdout': str(directory / (name + '.stdout')),
              'stderr': str(directory / (name + '.stderr'))}
    with open(record['stdout'], 'xb') as out, open(record['stderr'], 'xb') as err:
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   env=env, close_fds=True, start_new_session=True)
    record['pid'] = process.pid
    return process, record


def stop_owned(process, record, deadline):
    """Signal only this harness's process group; never wait on an unkillable PID."""
    if process.poll() is None:
        record['term_error'] = signal_group(process.pid, signal.SIGTERM)
        end = min(deadline, time.monotonic() + 1)
        while process.poll() is None and time.monotonic() < end:
            time.sleep(.01)
    if process.poll() is None:
        record['kill_error'] = signal_group(process.pid, signal.SIGKILL)
        end = min(deadline, time.monotonic() + 1)
        while process.poll() is None and time.monotonic() < end:
            time.sleep(.01)
    record['returncode'] = process.poll()
    record['unreaped'] = record['returncode'] is None


def read_small(path, limit=2 * 1024 ** 2):
    require(path.stat().st_size <= limit, 'evidence output exceeded size bound: ' + path.name)
    return path.read_text(encoding='utf-8', errors='strict')


def process_rows(text):
    rows = []
    seen = set()
    for line in text.splitlines():
        fields = line.split(None, 4)
        require(len(fields) == 5 and fields[0].isdigit() and int(fields[0]) > 0
                and fields[1].isdigit() and fields[0] not in seen
                and re.fullmatch(r'[RSDZTtWXIP][<NLsl+]*', fields[2]), 'malformed CPU process evidence')
        seen.add(fields[0])
        rows.append(dict(zip(('pid', 'ppid', 'state', 'wchan', 'comm'), fields)))
    require(rows, 'empty CPU process evidence')
    return rows


def usb_worker(row):
    # Generic mutex/io waits cannot be proven unrelated to the GPU. Only this
    # observed USB path is eligible for a bounded CPU-only resolution check.
    return (row['ppid'] == '2' and row['comm'].startswith('kworker/')
            and not re.search(r'nvidia|uvm|gpu', row['comm'], re.I)
            and re.fullmatch(r'(?:usb_kill_urb(?:\.part\.[0-9]+)?|usb_start_wait_urb)', row['wchan']))


def gpu_wait(row):
    return bool(re.search(r'nvidia|uvm|gpu|nv_alloc|nv_free|rm_init|rm_shutdown',
                          row['wchan'] + ' ' + row['comm'], re.I))


def cpu_evidence(summary, directory, label, args, deadline):
    def snapshot(suffix):
        outputs = []
        for kind, argv in [('dmesg', ['dmesg', '--color=never']),
                           ('ps', ['ps', '-eo', 'pid=,ppid=,stat=,wchan:40=,comm='])]:
            result = run_command(argv, directory, suffix + '-' + kind, args.command_timeout, deadline)
            summary['commands'].append(result)
            require(not result.get('error') and result['returncode'] == 0
                    and not result['timed_out'] and not result['unreaped'], suffix + ': CPU evidence command failed')
            outputs.append(read_small(Path(result['stdout'])))
        kernel, processes = outputs
        require(kernel.strip() and '\x00' not in kernel, 'empty or malformed kernel evidence')
        rows = process_rows(processes)
        errors = [line for line in kernel.splitlines() if GPU_ERROR.search(line)
                  or re.search(r'NVRM:.*RmInitAdapter.*fail|UVM.*(?:fatal|error|fault)', line, re.I)]
        observed = {'label': suffix, 'monotonic': time.monotonic(), 'kernel_gpu_errors': errors,
                    'd_state': [row for row in rows if row['state'].startswith('D')]}
        summary['cpu_observations'].append(observed)
        require(not errors, suffix + ': kernel GPU error')
        return rows, observed['d_state']

    rows, stuck = snapshot(label)
    if not stuck:
        return
    require(not any(gpu_wait(row) for row in stuck), label + ': GPU-linked D-state process')
    original = list(stuck)
    for index in range(2):
        require(time.monotonic() + args.d_state_grace / 2 < deadline, 'deadline prevents CPU D-state recheck')
        time.sleep(args.d_state_grace / 2)
        rows, stuck = snapshot(label + '-recheck-' + str(index))
        require(not any(gpu_wait(row) for row in stuck), label + ': GPU-linked D state appeared in recheck')
        if not stuck:
            by_pid = {row['pid']: row for row in rows}
            summary['resolved_d_state'].extend({'original': row, 'observed': by_pid.get(row['pid']),
                'resolution': 'absent' if row['pid'] not in by_pid else 'no-longer-D',
                'recheck': label + '-recheck-' + str(index)} for row in original)
            return
    raise Failed(label + ': D state remained after bounded CPU rechecks; cause unverified')


def telemetry(record, state, args):
    text = read_small(Path(record['stdout']))
    lines = text.splitlines(keepends=True)
    complete = [line for line in lines if line.endswith('\n')]
    require(len(complete) >= state['count'], 'telemetry evidence was truncated')
    for line in complete[state['count']:]:
        fields = next(csv.reader([line]))
        require(len(fields) == 4 and fields[0].strip().lower() == args.gpu.lower(), 'invalid/wrong-UUID telemetry')
        memory, temperature = int(fields[2].strip()), float(fields[3].strip())
        require(memory > 0 and math.isfinite(temperature) and 0 <= temperature <= 120, 'invalid telemetry values')
        row = {'uuid': fields[0].strip(), 'name': fields[1].strip(), 'memory_mib': memory,
               'temperature_c': temperature, 'observed_monotonic': time.monotonic()}
        state['samples'].append(row)
        state['last_seen'] = time.monotonic()
        require(temperature < args.max_temp, 'temperature reached limit: ' + str(temperature))
        if args.expect_memory_mib is not None:
            require(memory == args.expect_memory_mib, 'GPU memory differs from explicit expectation')
        if args.mode == 'vram':
            require(memory >= args.gib * 1024 + 512, 'reported VRAM cannot hold requested test plus reserve')
    state['count'] = len(complete)
    require(time.monotonic() - state['last_seen'] < args.telemetry_stale, 'temperature telemetry missing/stale')


def stages_from(record):
    stages = [json.loads(line) for line in read_small(Path(record['stdout'])).splitlines() if line.strip()]
    core = [(row['stage'], row['phase']) for row in stages if row['stage'] in STAGES]
    require(core == [(stage, phase) for stage in STAGES for phase in ('begin', 'end')],
            'CUDA stage markers missing, duplicated or out of order')
    return stages


def bounded_float(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('must be finite and positive')
    return number


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', '--uuid', dest='gpu', required=True)
    parser.add_argument('--mode', choices=('small', 'vram'), default='small')
    parser.add_argument('--python', default='python3')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--gib', type=int, default=9)
    parser.add_argument('--chunk-mib', type=int, default=32)
    parser.add_argument('--expect-memory-mib', type=int)
    parser.add_argument('--max-temp', type=bounded_float, default=78)
    parser.add_argument('--timeout', type=bounded_float, default=60)
    parser.add_argument('--deadline', type=bounded_float, default=90)
    parser.add_argument('--command-timeout', type=bounded_float, default=1)
    parser.add_argument('--telemetry-stale', type=bounded_float, default=6)
    parser.add_argument('--d-state-grace', type=bounded_float, default=1)
    args = parser.parse_args(argv)
    if not UUID_RE.fullmatch(args.gpu): parser.error('--gpu requires one full GPU UUID')
    if not 9 <= args.gib <= 60: parser.error('--gib must be 9..60')
    if args.mode == 'vram' and args.gpu.lower() not in {v.lower() for v in CMP_UUIDS}:
        parser.error('vram mode is restricted to the four known CMP UUIDs')
    if not 1 <= args.chunk_mib <= 64: parser.error('--chunk-mib must be 1..64')
    if args.max_temp > 78 or args.timeout > 300 or args.deadline > 360:
        parser.error('limits: temperature <=78, child timeout <=300, deadline <=360')
    if args.command_timeout > 5 or args.telemetry_stale > 10 or args.d_state_grace > 2:
        parser.error('limits: CPU command <=5, stale telemetry <=10, D-state grace <=2 seconds')
    return args


def main(argv=None):
    args = arguments(argv)
    os.umask(0o077)
    args.output_dir.mkdir(parents=False, exist_ok=False)
    directory = args.output_dir.resolve()
    start = time.monotonic()
    deadline = start + args.deadline
    work_deadline = deadline - min(5, args.deadline / 4)
    summary = {'verdict': 'FAIL', 'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        'gpu_uuid': args.gpu, 'mode': args.mode, 'child_normal_exit': False,
        'vram_bytes': args.gib * GIB if args.mode == 'vram' else 8192,
        'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'commands': [], 'cpu_observations': [], 'resolved_d_state': [], 'stages': [], 'failures': [],
        'limitation': 'A bounded selected-device trial is not a thermal soak or a whole-host stability guarantee.'}
    observer = child = None
    monitor = {'count': 0, 'last_seen': start, 'samples': []}
    def interrupted(signum, frame):
        raise KeyboardInterrupt('controller received signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    try:
        cpu_evidence(summary, directory, 'before', args, work_deadline)
        observer, summary['nvml_observer'] = start_owned(['nvidia-smi', '-i', args.gpu,
            '--query-gpu=uuid,name,memory.total,temperature.gpu', '--format=csv,noheader,nounits',
            '--loop-ms=2000'], directory, 'temperature')
        monitor['last_seen'] = time.monotonic()
        while not monitor['samples']:
            require(observer.poll() is None, 'temperature observer exited unexpectedly')
            require(time.monotonic() < work_deadline, 'overall work deadline exhausted')
            telemetry(summary['nvml_observer'], monitor, args)
            if not monitor['samples']: time.sleep(.025)
        config = {'mode': args.mode, 'gib': args.gib, 'chunk_mib': args.chunk_mib}
        env = dict(os.environ, CUDA_DEVICE_ORDER='PCI_BUS_ID', CUDA_VISIBLE_DEVICES=args.gpu,
                   PYTHONUNBUFFERED='1', OMP_NUM_THREADS='1', MKL_NUM_THREADS='1')
        child, summary['cuda_child'] = start_owned([args.python, str(Path(__file__).resolve()),
            '--_child', json.dumps(config)], directory, 'cuda', env)
        child_deadline = min(time.monotonic() + args.timeout, work_deadline)
        next_cpu = time.monotonic() + 2
        cycle = 0
        while child.poll() is None:
            require(time.monotonic() < child_deadline, 'CUDA child deadline exceeded')
            require(observer.poll() is None, 'temperature observer exited unexpectedly')
            telemetry(summary['nvml_observer'], monitor, args)
            require(Path(summary['cuda_child']['stdout']).stat().st_size <= 2 * 1024 ** 2,
                    'CUDA output exceeded size bound')
            if time.monotonic() >= next_cpu:
                cpu_evidence(summary, directory, 'during-' + str(cycle), args, child_deadline)
                cycle += 1
                next_cpu = time.monotonic() + 2
            time.sleep(.025)
        summary['cuda_child']['returncode'] = child.poll()
        summary['child_normal_exit'] = child.returncode == 0
        require(summary['child_normal_exit'], 'CUDA child did not exit normally with status 0')
        summary['stages'] = stages_from(summary['cuda_child'])
        require(observer.poll() is None, 'temperature observer exited unexpectedly')
        telemetry(summary['nvml_observer'], monitor, args)
    except (Exception, KeyboardInterrupt) as exc:
        summary['failures'].append(type(exc).__name__ + ': ' + str(exc))
    finally:
        if child is not None:
            stop_owned(child, summary['cuda_child'], deadline)
            try:
                if not summary['stages']:
                    summary['stages'] = [json.loads(line) for line in read_small(Path(summary['cuda_child']['stdout'])).splitlines() if line.strip()]
            except (Exception, KeyboardInterrupt):
                pass
        if observer is not None:
            stop_owned(observer, summary['nvml_observer'], deadline)
        if child is not None and time.monotonic() < deadline:
            try:
                cpu_evidence(summary, directory, 'after', args, deadline)
            except (Exception, KeyboardInterrupt) as exc:
                summary['failures'].append(type(exc).__name__ + ': ' + str(exc))
    for key in ('cuda_child', 'nvml_observer'):
        if summary.get(key, {}).get('unreaped'):
            summary['failures'].append(key + ': unkillable child recorded; stop GPU work and recover')
    if time.monotonic() >= deadline:
        summary['failures'].append('overall deadline exhausted')
    summary['telemetry'] = monitor['samples']
    summary['max_temperature_c'] = max((row['temperature_c'] for row in monitor['samples']), default=None)
    summary['elapsed_seconds'] = round(time.monotonic() - start, 3)
    summary['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    summary['verdict'] = 'PASS' if not summary['failures'] and summary['child_normal_exit'] else 'FAIL'
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(summary['verdict'] + ': ' + str(directory / 'summary.json'))
    for failure in summary['failures']: print('- ' + failure)
    return 0 if summary['verdict'] == 'PASS' else 1


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--_child':
        cuda_child(json.loads(sys.argv[2]))
    else:
        sys.exit(main())
