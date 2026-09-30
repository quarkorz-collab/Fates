#!/usr/bin/env python3
"""Bounded completion regression: budgets, costs, constraints and determinism."""

import argparse
import ast
from collections import Counter
import json
from pathlib import Path
import subprocess

from bench_search_stages import fingerprint


def structure(expression, costs):
    """Independently count rendered AST costs/leaves, rejecting unknown ops."""
    tree = ast.parse(expression.replace('^', '**').replace('×', '*'), mode='eval')
    leaves = []
    def walk(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            leaves.append(str(node.value))
            return len(str(node.value))
        if isinstance(node, ast.BinOp):
            op = {ast.Add: '+', ast.Sub: '-', ast.Mult: '*', ast.Div: '/', ast.Pow: '^'}[type(node.op)]
            return costs[op] + walk(node.left) + walk(node.right)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            return costs['neg'] + walk(node.operand)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and len(node.args) == 1:
            return costs[node.func.id] + walk(node.args[0])
        raise AssertionError(ast.dump(node))
    return walk(tree.body), leaves


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    options = parser.parse_args()
    binary = options.bin.resolve(strict=True)
    common = ['--max-cost', '11', '--beam', '128', '--pairs', '5000', '--deep-rounds', '3',
              '--deep-beam', '48', '--pareto-slots', '2', '--pareto-extra', '32',
              '--inverse-depth', '2', '--inverse-budget', '2000', '--no-stop', '--json']
    cases = [
        ('ordinary', ['0.731', *common]),
        ('cheap-hit', ['1', *common]),
        ('constraint-cost', ['4.7320508075688772935', *common, '--mode', 'pareto', '--digits', '123', '--max-literal-len', '1',
                             '--constants', 'none', '--ops', '+:2,-:3,*:2,/:3,sqrt:2',
                             '--symbol-count', '1=1', '--symbol-count', '2=1', '--symbol-count', '3=1']),
        ('ordered', ['3.731', *common, '--digits', '123', '--max-literal-len', '1',
                     '--constants', 'none', '--ops', '+,-,*,/,sqrt', '--symbol-order', '1,2,3']),
        ('sparse', ['17', *common, '--digits', '2', '--constants', 'none', '--ops', '+']),
        ('multi-ops', ['0.731', *common, '--ops', '+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,tan,asin',
                       '--task-chunks', '1', '--inverse-neighbors', '1']),
    ]
    report = []
    for name, arguments in cases:
        for mode, budget in [('off', 0), ('full', 0), ('auto', 0), ('auto', 1), ('auto', 10000)]:
            expected = None
            for threads in (1, 4, 16):
                command = [str(binary), *arguments, '--completion-mode', mode,
                           '--completion-budget', str(budget), '--threads', str(threads)]
                process = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=120)
                assert process.returncode == 0, process.stderr
                payload = json.loads(process.stdout)
                stats = payload['stats']
                assert payload['completed_cost'] == 11, payload
                assert stats['completion_work'] <= stats['completion_limit'] or mode == 'full'
                if mode == 'auto' and budget:
                    assert stats['completion_limit'] == budget or name == 'sparse'
                if mode == 'off' or name == 'sparse' or (name == 'cheap-hit' and mode == 'auto'):
                    assert stats['completion_work'] == stats['completion_index_entries'] == 0
                if name in ('constraint-cost', 'ordered'):
                    if name == 'constraint-cost' and mode == 'auto' and budget == 0:
                        assert stats['completion_evaluated'] > 0 and stats['completion_candidates'] > 0
                    costs = {'+': 2, '-': 3, '*': 2, '/': 3, 'sqrt': 2} if name == 'constraint-cost' else {
                        '+': 1, '-': 1, '*': 1, '/': 1, 'sqrt': 1}
                    assert payload['results'], (name, mode)
                    for result in payload['results']:
                        cost, leaves = structure(result['expression'], costs)
                        assert cost == result['cost'] <= 11, result
                        assert Counter(leaves) == Counter(['1', '2', '3']), result
                        if name == 'ordered':
                            assert leaves == ['1', '2', '3'], result
                current = fingerprint(payload)
                assert expected is None or expected == current, (name, mode, budget, threads)
                expected = current
                report.append({'case': name, 'mode': mode, 'budget': budget, 'threads': threads,
                               'fingerprint': current, 'stats': stats})
            print(f'{name}: {mode} budget={budget}, 1/4/16 threads passed', flush=True)
    if options.out:
        options.out.write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
