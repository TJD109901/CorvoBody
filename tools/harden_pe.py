"""harden_pe.py <dll> - finishing step for the zig-linked dinput8.dll (run by build.ps1; needs Python 3 + pefile).

Antivirus engines read a DLL without version information, with a COFF symbol table appended after its sections and
with a zero PE checksum as suspicious (CorvoBody 3.0's files were flagged by ML/generic detections for that kind of
profile). zig/lld can't leave those out at link time without stripping code, so this edits the linked file instead.
It never moves or changes a code or data byte:
  1. nulls the debug data directory (no PDB reference),
  2. zeroes the .buildid section (the CodeView / build-id record),
  3. drops the trailing "/4" DWARF section and the COFF symbol table after it (debug data only),
  4. zeroes PointerToSymbolTable / NumberOfSymbols and recomputes SizeOfImage,
  5. sets the PE checksum.
The version resource itself is linked in by build.ps1 (src\\dinput8.rc).
"""
import struct
import sys

import pefile


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


def harden(path):
    pe = pefile.PE(path, fast_load=False)
    buf = bytearray(open(path, 'rb').read())
    e = pe.DOS_HEADER.e_lfanew
    opt = e + 24
    soo = pe.FILE_HEADER.SizeOfOptionalHeader
    sects = list(pe.sections)

    dbg = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']]
    if dbg.VirtualAddress and dbg.Size:
        off = pe.get_offset_from_rva(dbg.VirtualAddress)
        buf[off:off + dbg.Size] = bytes(dbg.Size)
        struct.pack_into('<II', buf, opt + 96 + pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG'] * 8, 0, 0)
        print(f'  debug directory nulled ({dbg.Size} bytes)')

    for s in sects:
        if s.Name.rstrip(b'\0') == b'.buildid':
            buf[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData] = bytes(s.SizeOfRawData)
            print(f'  .buildid zeroed ({s.SizeOfRawData} bytes)')

    dwarf = next((s for s in sects if s.Name.rstrip(b'\0') == b'/4'), None)
    remaining = sects
    if dwarf is not None:
        last = max(sects, key=lambda s: s.PointerToRawData)
        if dwarf is not last:
            sys.exit('harden_pe: the "/4" debug section is not the last section - layout not as expected, file left as is')
        removed = len(buf) - dwarf.PointerToRawData
        hdr = dwarf.get_file_offset()
        buf = buf[:dwarf.PointerToRawData]
        buf[hdr:hdr + 40] = bytes(40)
        struct.pack_into('<H', buf, e + 6, pe.FILE_HEADER.NumberOfSections - 1)
        remaining = [s for s in sects if s is not dwarf]
        print(f'  "/4" debug section and COFF symbol table removed ({removed} bytes)')
    struct.pack_into('<II', buf, e + 12, 0, 0)   # PointerToSymbolTable, NumberOfSymbols
    size_img = align_up(max(s.VirtualAddress + s.Misc_VirtualSize for s in remaining), pe.OPTIONAL_HEADER.SectionAlignment)
    struct.pack_into('<I', buf, opt + 56, size_img)
    pe.close()

    struct.pack_into('<I', buf, opt + 64, 0)
    checksum = pefile.PE(data=bytes(buf), fast_load=True).generate_checksum()
    struct.pack_into('<I', buf, opt + 64, checksum)
    with open(path, 'wb') as f:
        f.write(bytes(buf))
    print(f'  PE checksum set: 0x{checksum:08x}')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    harden(sys.argv[1])
