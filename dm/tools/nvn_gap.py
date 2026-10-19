#!/usr/bin/env python3
"""Two reports from one game run:

1) NVN gap list - which host entry points the game calls but the host treats as a no-op or a stub.
   Inputs: DM_NVN_GAPLIST=<file> (entry classification) and optionally DM_NVN_TSV=<file> (per-frame call table).
2) Red-line summary - every 【红】 line the host printed during the run, deduplicated by shape with counts,
   so "implemented but wrong" behaviour is visible in one place instead of scrolling the log.
   Input: the run log (-–log).

Both sections are optional; give at least one input.
Usage:
  dm/tools/nvn_gap.py --list gap.tsv --calls calls.tsv --log run.log
"""
import argparse, collections, re, sys

def load_list(path):
    cls = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 2 and p[1] in ('impl', 'noop', 'stub'):
                cls[p[0]] = p[1]
    return cls

def load_calls(path):
    calls = collections.Counter()
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 3 and p[0].isdigit():
                try: calls[p[1]] += int(p[2])
                except ValueError: pass
    return calls

HEX = re.compile(r'0x[0-9a-fA-F]+')
NUM = re.compile(r'\b\d+\b')
def shape(line):
    """Normalise a red line so repeats of the same message group together."""
    s = line.strip()
    s = HEX.sub('0x…', s)
    s = NUM.sub('#', s)
    s = re.sub(r'\s+', ' ', s)
    return s

def red_summary(path):
    counts = collections.Counter(); first = {}; total = 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln, line in enumerate(f, 1):
            if '【红】' not in line: continue
            total += 1
            k = shape(line)
            counts[k] += 1
            first.setdefault(k, ln)
    return counts, first, total

def gap_section(a):
    cls = load_list(a.list)
    calls = load_calls(a.calls) if a.calls else collections.Counter()
    n_impl = sum(1 for v in cls.values() if v == 'impl')
    n_noop = sum(1 for v in cls.values() if v == 'noop')
    n_stub = sum(1 for v in cls.values() if v == 'stub')
    print('=== 缺口清单（NVN 入口）===')
    print(f'入口 {len(cls)}：已实现 {n_impl}，空实现 {n_noop}，报红桩 {n_stub}')
    if a.calls:
        bad = [(calls[n], n, cls[n]) for n in cls if cls[n] in ('stub', 'noop') and calls.get(n)]
        bad.sort(reverse=True)
        n_called = sum(1 for n in cls if calls.get(n))
        print(f'本次实际调用 {n_called} 个入口；其中桩/空实现 {len(bad)} 个：')
        for c, n, k in bad: print(f'  {c:>9}  {k:<4} {n}')
        if not bad: print('  （无：调用的入口都有实现）')
        uncalled = sorted(n for n in cls if cls[n] == 'stub' and not calls.get(n))
        print(f'未被调用过的桩 {len(uncalled)} 个（后续低频候选，暂不用补）')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list'); ap.add_argument('--calls'); ap.add_argument('--log')
    a = ap.parse_args()
    if not (a.list or a.log):
        ap.error('至少给 --list（缺口清单）或 --log（红色告警汇总）之一')
    if a.list: gap_section(a)
    if a.log:
        counts, first, total = red_summary(a.log)
        print('\n=== 红色告警汇总（本次运行）===')
        print(f'{len(counts)} 类 / {total} 条')
        if not counts: print('  （没有红色告警）')
        for k, c in sorted(counts.items(), key=lambda kv: (-kv[1], first[kv[0]])):
            print(f'  {c:>6} 次  行{first[k]:<6}  {k[:160]}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
