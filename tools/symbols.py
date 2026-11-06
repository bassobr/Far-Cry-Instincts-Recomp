# Turns the output of XbSymbolDatabase's CLI (run with -e) into config/symbols.txt: one symbol per line,
#   fun <Name> = 0x<address>     a function (an extra entry point for the recompiler)
#   var <Name> = 0x<address>     data, or an offset into a structure (names ending in _OFFSET)
# Hand-found symbols live in config/symbols_fci.txt in the same format and are appended after these.
#   python tools/symbols.py config/xdk_symbols.txt config/symbols.txt
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
seen = {}
lines = []
for raw in open(src, encoding='utf-8'):
    raw = raw.strip()
    if not raw or raw.startswith('#'):
        continue
    m = re.match(r'([A-Z0-9]+)__(FUN|VAR|UNK)__(?:(?:stdcall|fastcall|thiscall|cdecl|unknown)__)?([A-Za-z0-9_]+).* = 0x([0-9a-fA-F]+)$', raw)
    if not m:
        print('skipped:', raw, file=sys.stderr)
        continue
    lib, kind, name, addr = m.group(1), m.group(2), m.group(3), int(m.group(4), 16)
    if name in seen:
        if seen[name] != addr:
            print(f'conflict: {name} {seen[name]:08X} {addr:08X}', file=sys.stderr)
        continue
    seen[name] = addr
    lines.append(f"{'fun' if kind == 'FUN' else 'var'} {name} = 0x{addr:08X}  # {lib}")
with open(dst, 'w', encoding='utf-8', newline='\n') as f:
    f.write('# XDK 5849 symbols of default.xbe, from XbSymbolDatabase (MIT, Cxbx-Reloaded); see tools/symbols.py\n')
    f.write('\n'.join(sorted(lines, key=lambda l: l.split()[1])) + '\n')
print(f'{len(lines)} symbols')
