#!/usr/bin/env python3
"""Move regress.c bookkeeping arrays to external BSS without touching DSP state."""
import re
import subprocess
import sys


def main():
    objdump, objcopy, archive = sys.argv[1:4]
    sections = subprocess.check_output([objdump, "-h", archive], text=True).splitlines()
    wanted = {"J": False, "gold": False, "cpu": False, "cpu_parts": False}
    args = [objcopy]
    for line in sections:
        fields = line.split()
        if len(fields) < 2:
            continue
        section = fields[1]
        match = re.fullmatch(r"\.bss\.(J|gold|cpu|cpu_parts)(?:\.\d+)?", section)
        if not match:
            continue
        name = match.group(1)
        wanted[name] = True
        args.extend(["--rename-section", f"{section}=.ext_ram.bss.{name}"])
    missing = [name for name, found in wanted.items() if not found]
    if missing:
        raise RuntimeError(f"missing regress sections: {', '.join(missing)}")
    subprocess.run(args + [archive], check=True)


if __name__ == "__main__":
    main()
