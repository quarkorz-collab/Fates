#!/usr/bin/env python3
"""Audit saved completion-policy runs; never infer quality from engine zeroes."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import statistics

from bench_quality_expansion import compare
from bench_search_stages import stable_payload


def legacy_payload(payload):
    result = stable_payload(payload)
    result = {key: value for key, value in result.items() if not key.startswith('completion_')}
    result['stats'] = {key: value for key, value in result['stats'].items()
                       if not key.startswith('completion_') and key != 'inverse_requests_scored'}
    return result


def summarize(suite, previous=None):
    rows = []
    for key, entry in suite['cases'].items():
        if not all(entry['samples'].values()):
            continue
        samples = entry['samples']
        row = {'key': key, 'profile': entry['case'].get('profile'),
               'best': {label: values[0]['quality']['best'] for label, values in samples.items()}}
        if previous:
            prior = previous['entries'][key]['samples']
            row['auto_vs_original'] = compare(prior['old'][0]['quality'], samples['auto'][0]['quality'])
            row['auto_vs_broad'] = compare(prior['new'][0]['quality'], samples['auto'][0]['quality'])
            if 'full' in samples:
                raw = json.loads(Path(prior['new'][0]['raw_json']).read_text(encoding='utf-8'))
                row['full_legacy_identical'] = legacy_payload(raw) == legacy_payload(samples['full'][0]['payload'])
        elif 'reference' in samples:
            row['auto_vs_original'] = compare(samples['reference'][0]['quality'], samples['auto'][0]['quality'])
        if 'broad_reference' in samples:
            row['auto_vs_broad'] = compare(samples['broad_reference'][0]['quality'], samples['auto'][0]['quality'])
        if 'auto' in samples and 'full' in samples:
            row['auto_vs_full'] = compare(samples['full'][0]['quality'], samples['auto'][0]['quality'])
        row['samples'] = {label: len(values) for label, values in samples.items()}
        row['minimum_hit_cost'] = {
            label: min((value['cost'] for value in values[0]['quality']['verified']
                        if float(value['mp_normalized_error']) <= 1e-14), default=None)
            for label, values in samples.items()}
        row['median_seconds'] = {label: statistics.median(s['wall_seconds'] for s in values)
                                  for label, values in samples.items()}
        row['time_range'] = {label: [min(s['wall_seconds'] for s in values), max(s['wall_seconds'] for s in values)]
                             for label, values in samples.items()}
        row['median_peak_working_set'] = {label: statistics.median(s['memory']['peak_working_set'] for s in values)
                                          for label, values in samples.items() if values[0]['memory']}
        row['auto_work'] = samples['auto'][0]['payload']['stats']['completion_work']
        row['auto_limit'] = samples['auto'][0]['payload']['stats']['completion_limit']
        assert row['auto_work'] <= row['auto_limit'], key
        rows.append(row)
    aggregate = {}
    for profile in sorted({row['profile'] for row in rows} | {'all'}):
        selected = [r for r in rows if profile == 'all' or r['profile'] == profile]
        item = {'configurations': len(selected)}
        for comparison in ('auto_vs_original', 'auto_vs_broad', 'auto_vs_full'):
            relevant = [r[comparison] for r in selected if comparison in r]
            if relevant:
                item[comparison] = dict(Counter(r['status'] for r in relevant))
                item[comparison + '_hits'] = {'before': sum(r['old_hit'] for r in relevant),
                                              'after': sum(r['new_hit'] for r in relevant)}
        for label in ('reference', 'full', 'broad_reference'):
            relevant = [r for r in selected if label in r['median_seconds']]
            if relevant:
                item['auto_time_ratio_vs_' + label] = math.exp(statistics.mean(
                    math.log(r['median_seconds']['auto'] / r['median_seconds'][label]) for r in relevant))
                if all(label in r['median_peak_working_set'] for r in relevant):
                    item['auto_memory_ratio_vs_' + label] = math.exp(statistics.mean(
                        math.log(r['median_peak_working_set']['auto'] / r['median_peak_working_set'][label])
                        for r in relevant))
        aggregate[profile] = item
    return {'complete': suite['complete'], 'rows': rows, 'aggregate': aggregate,
            'full_compatibility_failures': [r['key'] for r in rows if r.get('full_legacy_identical') is False],
            'auto_hit_cost_regressions_vs_full': [r['key'] for r in rows
                if r['minimum_hit_cost'].get('full') is not None and
                (r['minimum_hit_cost']['auto'] is None or
                 r['minimum_hit_cost']['auto'] > r['minimum_hit_cost']['full'])],
            'unverified_records': sum(len(s['quality']['unverified']) for entry in suite['cases'].values()
                                       for values in entry['samples'].values() for s in values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', type=Path, required=True)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    options = parser.parse_args()
    suite = json.loads(options.suite.read_text(encoding='utf-8'))
    previous = json.loads(options.previous.read_text(encoding='utf-8')) if options.previous else None
    result = summarize(suite, previous)
    options.out.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'rows'}, indent=2))
    if result['full_compatibility_failures']:
        raise SystemExit('Full differs from previous broad-enhancement payload/counters')


if __name__ == '__main__':
    main()
