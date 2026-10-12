#!/usr/bin/env python3
import sys
import hashlib
import zlib
from pathlib import Path

def main():
    if len(sys.argv) != 2:
        print("Uso:")
        print('  py rom_info.py "MinhaROM.z64"')
        return 1

    rom = Path(sys.argv[1]).expanduser().resolve()

    if not rom.is_file():
        print(f"ERRO: ROM não encontrada: {rom}")
        return 1

    data = rom.read_bytes()

    print(f"ROM: {rom.name}")
    print(f"Path: {rom}")
    print(f"Size: {len(data)} bytes")
    print(f"Size HEX: {hex(len(data))}")
    print(f"CRC32: {zlib.crc32(data) & 0xFFFFFFFF:08X}")
    print(f"SHA-1: {hashlib.sha1(data).hexdigest()}")
    print(f"MD5: {hashlib.md5(data).hexdigest()}")

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
