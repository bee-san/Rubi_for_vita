#!/usr/bin/env python3
"""Check actual native jump-register spans against the compiled PNG buffer."""
import argparse
import json
import re
import struct
import subprocess


def disassemble(objdump, elf, symbol):
    output = subprocess.check_output([
        objdump, "-d", "--no-show-raw-insn", "--disassemble=" + symbol, elf
    ], text=True)
    marker = re.search(r"^[0-9a-f]+ <" + re.escape(symbol) + r">:\n", output, re.M)
    if not marker:
        raise ValueError("Missing linked native symbol: " + symbol)
    lines = []
    for line in output[marker.end():].splitlines():
        match = re.match(r"\s*[0-9a-f]+:\s+(.+)", line)
        if not match:
            break
        lines.append(match.group(1).split("@", 1)[0].strip())
    return lines


def register_count(registers):
    count = 0
    for item in registers.split(","):
        item = item.strip()
        span = re.fullmatch(r"([rd])(\d+)-\1(\d+)", item)
        if span:
            count += int(span[3]) - int(span[2]) + 1
        elif re.fullmatch(r"r\d+|d\d+|sl|fp|ip|lr|sp", item):
            count += 1
        else:
            raise ValueError("Unknown jump register: " + item)
    return count


def jump_span(lines, store):
    # Fail closed for an unrecognized implementation: this gate understands
    # the linked ARM newlib multiple-register save/restore sequence.
    core = "stmia" if store else "ldmia"
    vfp = "vstmia" if store else "vldmia"
    offset = extent = transfers = 0
    terminal = returned = False
    for line in lines:
        match = re.fullmatch(r"(" + core + r"(?:\.w)?|" + vfp +
                             r")\s+r0(!?),\s*\{([^}]+)\}", line)
        if match:
            if terminal or returned or (transfers == 0 and match[1].startswith("v")):
                raise ValueError("Unexpected jump-register ordering: " + line)
            if transfers > 1 or (transfers == 1 and not match[1].startswith("v")):
                raise ValueError("Unexpected additional jump-register transfer: " + line)
            if bool(match[2]) == match[1].startswith("v"):
                raise ValueError("Unexpected jump-buffer pointer update: " + line)
            size = register_count(match[3]) * (8 if match[1].startswith("v") else 4)
            extent = max(extent, offset + size)
            if match[2]:
                offset += size
            transfers += 1
        elif returned:
            raise ValueError("Unexpected instruction after jump return: " + line)
        elif store and not transfers and re.fullmatch(r"mov(?:\.w)?\s+ip,\s*sp", line):
            pass
        elif not store and transfers == 1 and not terminal and re.fullmatch(r"mov(?:\.w)?\s+sp,\s*ip", line):
            pass
        elif transfers and ((store and re.fullmatch(r"mov(?:\.w)?\s+r0,\s*#0", line)) or
                            (not store and re.fullmatch(r"movs\s+r0,\s*r1|it\s+eq|moveq\s+r0,\s*#1", line))):
            terminal = True
        elif terminal and re.fullmatch(r"bx\s+lr", line):
            returned = True
        else:
            raise ValueError("Unrecognized jump instruction: " + line)
    if not transfers or not returned:
        raise ValueError("No linked jump-buffer transfers decoded")
    return extent


def geometry(objdump, obj):
    output = subprocess.check_output([objdump, "-s", "-j", ".rodata", obj], text=True)
    data = bytearray()
    for line in output.splitlines():
        match = re.match(r"^\s*[0-9a-f]+\s+((?:[0-9a-f]{8}\s+){1,4})", line)
        if match:
            data.extend(bytes.fromhex(match[1]))
    if len(data) != 16:
        raise ValueError("Unexpected compiled PNG geometry payload")
    return struct.unpack("<4I", data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--geometry-object", required=True)
    parser.add_argument("elf")
    args = parser.parse_args()
    try:
        size, fn_offset, ptr_offset, size_offset = geometry(args.objdump, args.geometry_object)
        saved = jump_span(disassemble(args.objdump, args.elf, "setjmp"), True)
        restored = jump_span(disassemble(args.objdump, args.elf, "longjmp"), False)
        bounds = disassemble(args.objdump, args.elf, "png_set_longjmp_fn")
        compiled_limits = [int(m[1], 0) for line in bounds
                           if (m := re.fullmatch(r"cmp(?:\.w)?\s+r2,\s*#(0x[0-9a-f]+|\d+)", line))]
        if size not in compiled_limits:
            raise ValueError("Linked libpng buffer limit does not match its compiled geometry")
        if saved != restored or saved > size or min(fn_offset, ptr_offset, size_offset) < saved:
            raise ValueError("Linked jump register spans overlap libpng metadata")
    except (ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, "Settings PNG ABI failed: " + str(exc) + "\n")
    print(json.dumps({"settings_png_abi": "PASS", "setjmp_bytes": saved,
                      "longjmp_bytes": restored, "png_local_bytes": size,
                      "png_metadata_offsets": [fn_offset, ptr_offset, size_offset]}))


if __name__ == "__main__":
    main()
