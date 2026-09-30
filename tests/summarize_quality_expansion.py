#!/usr/bin/env python3
"""Repeat secondary top-100 quality changes and independently validate at 160 dps."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import statistics

import mpmath as mp

from bench_quality_expansion import aggregate, evaluate, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--repeat-secondary', action='store_true')
    options = parser.parse_args()
    directory = options.input.resolve().parent
    report = json.loads(options.input.read_text(encoding='utf-8'))
    if not report['complete']:
        raise RuntimeError('Wait for the primary comparison to finish')
    manifest = json.loads(Path(report['manifest']).read_text(encoding='utf-8'))
    for label, binary in manifest['binaries'].items():
        if hashlib.sha256(Path(binary).read_bytes()).hexdigest() != manifest['sha256'][label]:
            raise RuntimeError(f'Binary changed during comparison: {label}')
    # Secondary validation selects EVERY case where the count <=1e-12 or <=1e-14
    # changed, in either direction. It does not change the initial sample set.
    selected = [entry for entry in report['entries'].values()
                if any(entry['comparison']['hit_count_changes'][threshold]
                       for threshold in ('1e-12', '1e-14'))
                and len(entry['samples']['old']) == 1]
    supplement = {'selection': [entry['key'] for entry in selected],
                  'rule': 'All remaining cases with nonzero top100 count changes at 1e-12 or 1e-14, either sign'}
    (directory / 'secondary-manifest.json').write_text(json.dumps(supplement, indent=2), encoding='utf-8')
    if options.repeat_secondary:
        for repeat in (2, 3):
            for entry in selected:
                for label in (('new', 'old') if repeat == 2 else ('old', 'new')):
                    print(f"SECONDARY {repeat}/3 {entry['key']} {label}", flush=True)
                    sample = run(Path(manifest['binaries'][label]), entry,
                                 directory / f"{entry['key']}--{label}--{repeat}.json", 240)
                    if sample['fingerprint'] != entry['samples'][label][0]['fingerprint']:
                        raise RuntimeError(f"non-deterministic secondary case {entry['key']} {label}")
                    entry['samples'][label].append(sample)
    report['aggregate'] = aggregate(report['entries'])
    (directory / 'combined-results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    mp.mp.dps = 160
    evaluate.cache_clear()
    validations = []
    rows = []
    for entry in report['entries'].values():
        row = {key: entry[key] for key in ('key', 'id', 'profile', 'group', 'source', 'target')}
        row['comparison'] = entry['comparison']
        row['samples_per_binary'] = {label: len(entry['samples'][label]) for label in ('old', 'new')}
        row['median_seconds'] = {label: statistics.median(s['wall_seconds'] for s in entry['samples'][label])
                                 for label in ('old', 'new')}
        row['stage_median_seconds'] = {
            label: {stage: statistics.median(s['stats']['stage_seconds'][stage] for s in entry['samples'][label])
                    for stage in ('deterministic', 'mitm', 'inverse', 'deep')}
            for label in ('old', 'new')}
        row['initial_time_ratio'] = entry['samples']['new'][0]['wall_seconds'] / entry['samples']['old'][0]['wall_seconds']
        row['median_time_ratio'] = row['median_seconds']['new'] / row['median_seconds']['old']
        previous = {v['expression'] for v in entry['samples']['old'][0]['quality']['verified']}
        novel = [v for v in entry['samples']['new'][0]['quality']['verified']
                 if v['expression'] not in previous and mp.mpf(v['mp_normalized_error']) <= mp.mpf('1e-12')]
        row['new_top100_with_normalized_error_at_most_1e_minus_12'] = novel
        row['cheapest_hit_cost'] = {}
        for label in ('old', 'new'):
            quality = entry['samples'][label][0]['quality']
            limit = int(entry['args'][entry['args'].index('--max-cost') + 1])
            for result in quality['verified']:
                if 'cos(' in result['expression'] or result['cost'] > limit:
                    raise RuntimeError(f'Operator/cost constraint violation: {entry["key"]} {label}')
            costs = [v['cost'] for v in quality['verified'] if mp.mpf(v['mp_normalized_error']) <= mp.mpf('1e-14')]
            row['cheapest_hit_cost'][label] = min(costs, default=None)
            check = [quality['best'], *novel] if label == 'new' else [quality['best']]
            for result in check:
                error160 = abs(evaluate(result['expression']) - mp.mpf(entry['target']))
                error80 = mp.mpf(result['mp_error'])
                difference = abs(error160 - error80)
                consistent = difference <= max(mp.mpf('1e-65'), abs(error160) * mp.mpf('1e-12'))
                validations.append({'key': entry['key'], 'binary': label, 'expression': result['expression'],
                                    'error80': result['mp_error'], 'error160': mp.nstr(error160, 110),
                                    'consistent': bool(consistent)})
                if not consistent:
                    raise RuntimeError(f"80/160 digit check changed materially: {entry['key']} {label}")
        rows.append(row)
    by_profile = {}
    for profile in ('screen', 'original-budget'):
        subset = [row for row in rows if row['profile'] == profile]
        changes = lambda t: [row['comparison']['hit_count_changes'][t] for row in subset]
        by_profile[profile] = {
            'cases': len(subset), 'status': dict(Counter(row['comparison']['status'] for row in subset)),
            'geomean_initial_time_ratio': statistics.geometric_mean(row['initial_time_ratio'] for row in subset),
            'geomean_median_time_ratio': statistics.geometric_mean(row['median_time_ratio'] for row in subset),
            'counts': {t: {'cases_increased': sum(n > 0 for n in changes(t)),
                           'cases_decreased': sum(n < 0 for n in changes(t)),
                           'net_additional_expressions': sum(changes(t))}
                       for t in ('1e-10', '1e-12', '1e-14', '1e-50')},
            'fully_repeated_cases': sum(min(row['samples_per_binary'].values()) == 3 for row in subset),
            'sum_of_case_stage_medians': {
                label: {stage: sum(row['stage_median_seconds'][label][stage] for row in subset)
                        for stage in ('deterministic', 'mitm', 'inverse', 'deep')}
                for label in ('old', 'new')},
        }
    analysis = {'profiles': by_profile, 'rows': rows, 'validation_160_digits': validations,
                'total_searches': sum(sum(row['samples_per_binary'].values()) for row in rows)}
    (directory / 'analysis.json').write_text(json.dumps(analysis, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps({'profiles': by_profile, 'total_searches': analysis['total_searches'],
                      'independent_160_digit_checks': len(validations)}, indent=2), flush=True)


if __name__ == '__main__':
    main()
