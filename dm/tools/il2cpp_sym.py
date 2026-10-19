#!/usr/bin/env python3
"""把 guest 模块里的地址翻成 IL2CPP 托管方法名（御剑精选集 / 一切 Unity IL2CPP 游戏）。

用法：
    il2cpp_sym.py <main.so> <script.json> [地址...]      # 地址是模块内偏移（main+0x31100ec → 0x31100ec）
    il2cpp_sym.py <main.so> <script.json> --dis <起始> <结束>

`script.json` 由 Il2CppDumper 生成（Perfare/Il2CppDumper）：它的 NSO 读取器假定「文件偏移 == 虚拟地址」，
本作这种 16 字节段表、模块头按虚拟地址存（text+4 是模块头的 VA）的 NSO 需要改一行：
    Position = MapVATR(modOffset) + 4;
然后 `dotnet build -c Release` 并把 bin/Release/net6.0 整个目录拷出来跑。输出 dump.cs / script.json
就在当前目录。反汇编时给每个 bl/b 目标标上方法名，崩溃现场（pc + lr + 栈扫描）就能直接读懂。
"""
import bisect, json, struct, sys

def image(path):
    """NSO（16 字节段表：file, mem, size, 保留）→ 按虚拟地址铺平的镜像"""
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        return bytearray(d)
    import lz4.block
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(max(mo + sz for fo, mo, sz in segs))
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        if flags >> i & 1:
            raw = lz4.block.decompress(raw, uncompressed_size=sz)
        img[mo:mo + sz] = raw
    return img

def load_symbols(script):
    j = json.load(open(script))
    ms = sorted(((m['Address'], m['Name']) for m in j['ScriptMethod']), key=lambda x: x[0])
    return [m[0] for m in ms], ms

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    img = image(sys.argv[1])
    addrs, ms = load_symbols(sys.argv[2])
    def sym(a, span=0x4000):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0 or a - ms[i][0] > span: return None
        return ms[i]
    if sys.argv[3] == '--dis':
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        start, end = int(sys.argv[4], 0), int(sys.argv[5], 0)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        for ins in md.disasm(bytes(img[start:end]), start):
            note = ''
            if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#'):
                s = sym(int(ins.op_str[1:], 16))
                if s: note = f'   ; {s[1]}'
            print(f'{ins.address:#08x}  {ins.mnemonic:8s} {ins.op_str}{note}')
        return
    for arg in sys.argv[3:]:
        a = int(arg, 0); s = sym(a)
        print(f'{a:#x}: ' + (f'{s[1]} +{a - s[0]:#x}' if s else '?'))

if __name__ == '__main__':
    main()
