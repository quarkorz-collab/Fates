#!/usr/bin/env python3
"""Compare completion policies using a frozen quality manifest, with peak memory.

Requires mpmath. Writes raw payloads, independent precision checks and timings;
single-repeat output is screening evidence, not a final performance benchmark.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

from bench_quality_expansion import quality
from bench_search_stages import fingerprint


def peak_memory(process):
    if os.name != 'nt':
        return None
    class Counters(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_ulong), ('faults', ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in (
                'peak_working_set', 'working_set', 'peak_paged', 'paged',
                'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_commit', 'private')]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    query = ctypes.WinDLL('psapi', use_last_error=True).GetProcessMemoryInfo
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
    query.restype = ctypes.c_int
    if not query(int(process._handle), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return {'peak_working_set': counters.peak_working_set, 'peak_commit': counters.peak_commit}


def run(binary, arguments, timeout):
    started = time.perf_counter()
    with subprocess.Popen([str(binary), *arguments], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding='utf-8') as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        elapsed = time.perf_counter() - started
        memory = peak_memory(process)
        if process.returncode:
            raise RuntimeError(stderr)
    payload = json.loads(stdout)
    return {'wall_seconds': elapsed, 'memory': memory, 'payload': payload,
            'fingerprint': fingerprint(payload)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--case', action='append')
    parser.add_argument('--modes', nargs='+', default=['off', 'auto', 'full'], choices=['off', 'auto', 'full'])
    parser.add_argument('--compare', type=Path, help='Additional unchanged reference executable')
    parser.add_argument('--broad-reference', type=Path,
                        help='Previous broad-enhancement release (or manifest broad_reference field)')
    parser.add_argument('--threads', type=int, default=16)
    parser.add_argument('--repeats', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=240)
    options = parser.parse_args()
    binary = options.bin.resolve(strict=True)
    manifest = json.loads(options.manifest.read_text(encoding='utf-8'))
    selected = [case for case in manifest['workloads'] if not options.case or case['key'] in options.case]
    if not selected:
        raise ValueError('No selected cases')
    binaries = {mode: binary for mode in options.modes}
    if options.compare:
        binaries['reference'] = options.compare.resolve(strict=True)
    broad_reference = options.broad_reference or manifest.get('broad_reference')
    if broad_reference:
        binaries['broad_reference'] = Path(broad_reference).resolve(strict=True)
    report = {'complete': False, 'binaries': {key: str(path) for key, path in binaries.items()},
              'sha256': {key: hashlib.sha256(path.read_bytes()).hexdigest() for key, path in binaries.items()},
              'threads': options.threads, 'repeats': options.repeats, 'cases': {}}
    options.out.parent.mkdir(parents=True, exist_ok=True)
    def save():
        options.out.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    for case_index, case in enumerate(selected):
        arguments = list(case['args'])
        arguments[arguments.index('--threads') + 1] = str(options.threads)
        entry = {'case': case, 'samples': {label: [] for label in binaries}}
        report['cases'][case['key']] = entry
        for repeat in range(options.repeats):
            order = list(binaries)
            offset = (case_index + repeat) % len(order)
            for label in order[offset:] + order[:offset]:
                print(f"{case['key']} {label} {repeat+1}/{options.repeats}", flush=True)
                sample = run(binaries[label], arguments + (['--completion-mode', label] if label in options.modes else []), options.timeout)
                if entry['samples'][label] and sample['fingerprint'] != entry['samples'][label][0]['fingerprint']:
                    raise RuntimeError(f"Non-deterministic repeat {case['key']} {label}")
                sample['quality'] = quality(sample['payload'], case['target'])
                entry['samples'][label].append(sample)
                save()
                stats = sample['payload']['stats']
                print(f"  {sample['wall_seconds']:.3f}s; err={sample['quality']['best']['mp_error']}; "
                      f"work={stats.get('completion_work')}; limited={stats.get('completion_limited_tasks')}; "
                      f"memory={sample['memory']}", flush=True)
        entry['median_seconds'] = {label: statistics.median(s['wall_seconds'] for s in samples)
                                    for label, samples in entry['samples'].items()}
        save()
    report['complete'] = True
    save()


if __name__ == '__main__':
    main()
