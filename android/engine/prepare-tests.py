"""Generate ELF assembly from upstream's Mach-O test fixtures."""
from pathlib import Path
import re
import sys
root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / 'build'
out.mkdir(parents=True, exist_ok=True)
for name in ['classify_cases.s', 'operand_cases.s', 'harness.S', 'jit_programs.S', 'mem_programs.S', 'exc_programs.S', 'harness.h', 'devices_programs.S']:
    s = (root / 'tests' / name).read_text()
    s = re.sub(r'(?<![\w)])_([A-Za-z][A-Za-z0-9_]*)', r'\1', s)
    s = s.replace('_\\name', '\\name')
    s = re.sub(r'(\w+)@PAGEOFF', r':lo12:\1', s)
    s = re.sub(r'(\w+)@PAGE', r'\1', s)
    s = re.sub(r'^\s*\.subsections_via_symbols.*$', '', s, flags=re.M)
    if name == 'harness.S':
        s = s.replace('#-160', '#-176').replace('[sp], #160', '[sp], #176')
        s = s.replace('    br x1', '    str x18, [sp, #160]\n    br x1').replace('    ldp x29, x30, [sp], #176', '    ldr x18, [sp, #160]\n    ldp x29, x30, [sp], #176')
    # C code in harness.h needs its original double-underscore names.
    if name == 'harness.h':
        original = (root / 'tests' / name).read_text()
        s = s[:s.index('#else')] + original[original.index('#else'):]
    (out / (name + '.S' if name.endswith('.s') else name)).write_text(s)

for p in (root / 'tests').glob('test_*.c'):
    s = p.read_text()
    if p.name == 'test_exc.c':
        s = s.replace('#include <mach/mach_time.h>', '#include "platform.h"')
        a = s.index('    mach_timebase_info_data_t tb;')
        b = s.index('\n\n', a)
        s = s[:a] + '    check("host", "24 MHz counter advances", hv_host_counter() > 0, 1);' + s[b:]
    (out / p.name).write_text(s)
