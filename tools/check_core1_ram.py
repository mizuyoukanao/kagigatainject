"""Checks that core1's command path never touches flash.

core0 erases/programs flash (log and config saves) while core1 keeps
answering the SD bus, so everything core1 runs or reads must be in RAM.
Walks the call graph from core1_main in the disassembly and fails if any
reachable function lives in flash or loads a flash address from its literal
pool.

  python tools/check_core1_ram.py build/kagigatainject.elf [path/to/objdump]
"""
import re
import subprocess
import sys

ROOT = "core1_main"
FLASH = re.compile(r"^1[0-9a-f]{7}$")


def main():
    elf = sys.argv[1]
    objdump = sys.argv[2] if len(sys.argv) > 2 else "arm-none-eabi-objdump"
    text = subprocess.run([objdump, "-d", elf], capture_output=True, text=True,
                          check=True).stdout

    funcs = {}
    for chunk in re.split(r"\n(?=[0-9a-f]{8} <)", text):
        m = re.match(r"([0-9a-f]{8}) <([^>]+)>:", chunk)
        if m:
            funcs[m.group(2)] = (m.group(1), chunk)

    if ROOT not in funcs:
        sys.exit(f"{ROOT} not found in {elf}")

    bad = []
    seen = set()
    todo = [ROOT]
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        addr, body = funcs[name]
        if FLASH.match(addr):
            bad.append(f"{name} is in flash ({addr})")
            continue
        for lit in re.findall(r"\.word\s+0x([0-9a-f]{8})", body):
            if FLASH.match(lit):
                bad.append(f"{name} references flash address 0x{lit}")
        for target in re.findall(r"\bb(?:l|lx)?(?:\.w)?\s+[0-9a-f]+ <([^>+]+)>",
                                 body):
            if target in funcs and target != name:
                todo.append(target)

    print("core1 reaches:", ", ".join(sorted(seen)))
    if bad:
        print("FAIL:\n  " + "\n  ".join(bad))
        sys.exit(1)
    print("OK: core1 path is RAM-only")


if __name__ == "__main__":
    main()
