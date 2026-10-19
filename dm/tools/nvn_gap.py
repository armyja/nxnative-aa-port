#!/usr/bin/env python3
"""NVN gap report: which host entry points the game calls but the host does not implement.

Inputs
  --list  FILE  DM_NVN_GAPLIST output: "name<TAB>class" (impl / noop / stub)
  --calls FILE  optional DM_NVN_TSV output: "frame<TAB>name<TAB>count" rows; only known NVN names are used
  --imports FILE optional plain list of symbol names the game imports (e.g. from the NSO dynsym)

Output: stubs / no-ops that were actually called (sorted by call count), then uncalled stubs, then a summary.
The point: fix what the game exercises first, but keep the full list so nothing is discovered only by crashing.
"""
import argparse, collections, sys

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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', required=True)
    ap.add_argument('--calls')
    ap.add_argument('--imports')
    a = ap.parse_args()
    cls = load_list(a.list)
    calls = load_calls(a.calls) if a.calls else collections.Counter()
    imports = set()
    if a.imports:
        with open(a.imports, encoding='utf-8', errors='replace') as f:
            imports = {l.strip() for l in f if l.strip()}

    total = len(cls)
    n_impl = sum(1 for v in cls.values() if v == 'impl')
    n_noop = sum(1 for v in cls.values() if v == 'noop')
    n_stub = sum(1 for v in cls.values() if v == 'stub')
    print(f'NVN 入口 {total}：已实现 {n_impl}，空实现 {n_noop}，报红桩 {n_stub}')
    if imports:
        hit = [n for n in imports if n in cls]
        print(f'游戏导入 {len(imports)} 个符号，其中 {len(hit)} 个是 NVN 入口')

    called_bad = [(calls[n], n, cls[n]) for n in cls if cls[n] in ('stub', 'noop') and calls.get(n)]
    called_bad.sort(reverse=True)
    if a.calls:
        n_called = sum(1 for n in cls if calls.get(n))
        print(f'本次运行实际调用 {n_called} 个入口；其中桩/空实现 {len(called_bad)} 个：')
        for c, n, k in called_bad:
            src = '（游戏导入）' if not imports or n in imports else ''
            print(f'  {c:>9}  {k:<4} {n}{src}')
        if not called_bad:
            print('  （无：调用的入口都有实现）')

    uncalled_stub = sorted(n for n in cls if cls[n] == 'stub' and not calls.get(n))
    if imports:
        imp_stub = [n for n in uncalled_stub if n in imports]
        print(f'\n未被调用但游戏导入了的桩（{len(imp_stub)}）：')
        for n in imp_stub: print('  ' + n)
    print(f'\n未被调用过的桩共 {len(uncalled_stub)} 个（可作为后续优先级较低的候选）')
    return 0

if __name__ == '__main__':
    sys.exit(main())
