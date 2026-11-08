#!/usr/bin/env python3
"""Read-only x86_64 Mach-O / MH_FILESET ABI probe for Tahoe IOUSBHostFamily."""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

CPU_TYPE_X86_64 = 0x01000007
MH_MAGIC_64 = 0xFEEDFACF
MH_FILESET = 0xC
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_UUID = 0x1B
LC_FILESET_ENTRY = 0x80000035
NLIST64_SIZE = 16
MAX_LOAD_COMMANDS = 8192
MAX_SYMBOLS = 4_000_000


class ProbeError(RuntimeError):
    pass


@dataclass(frozen=True)
class Header64:
    offset: int
    cputype: int
    filetype: int
    ncmds: int
    sizeofcmds: int


@dataclass(frozen=True)
class Segment64:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int


@dataclass(frozen=True)
class ImageInfo:
    header: Header64
    uuid: bytes
    segments: tuple[Segment64, ...]
    symoff: int
    nsyms: int
    stroff: int
    strsize: int


def _need(data: bytes, off: int, size: int, what: str) -> None:
    if off < 0 or size < 0 or off > len(data) or size > len(data) - off:
        raise ProbeError(f"{what} is out of bounds")


def _slice_x86_64(raw: bytes) -> bytes:
    if len(raw) < 4:
        raise ProbeError("input is too small for Mach-O magic")
    le = struct.unpack_from("<I", raw, 0)[0]
    if le == MH_MAGIC_64:
        return raw

    be = struct.unpack_from(">I", raw, 0)[0]
    if be not in (FAT_MAGIC, FAT_MAGIC_64):
        raise ProbeError("unsupported Mach-O/fat magic")
    _need(raw, 0, 8, "fat header")
    nfat = struct.unpack_from(">I", raw, 4)[0]
    if nfat == 0 or nfat > 64:
        raise ProbeError("invalid fat architecture count")
    off = 8
    arch_size = 32 if be == FAT_MAGIC_64 else 20
    _need(raw, off, nfat * arch_size, "fat architecture table")
    matches = []
    for _ in range(nfat):
        if be == FAT_MAGIC_64:
            cputype, _sub, slice_off, slice_size, _align, _reserved = struct.unpack_from(">IIQQII", raw, off)
        else:
            cputype, _sub, slice_off, slice_size, _align = struct.unpack_from(">IIIII", raw, off)
        if cputype == CPU_TYPE_X86_64:
            _need(raw, int(slice_off), int(slice_size), "x86_64 fat slice")
            matches.append(raw[int(slice_off):int(slice_off + slice_size)])
        off += arch_size
    if len(matches) != 1:
        raise ProbeError(f"expected exactly one x86_64 fat slice, found {len(matches)}")
    return matches[0]


def _parse_header(data: bytes, off: int) -> Header64:
    _need(data, off, 32, "Mach-O 64 header")
    magic, cputype, _sub, filetype, ncmds, sizeofcmds, _flags, _reserved = struct.unpack_from("<IIIIIIII", data, off)
    if magic != MH_MAGIC_64:
        raise ProbeError("expected little-endian MH_MAGIC_64 Mach-O")
    if cputype != CPU_TYPE_X86_64:
        raise ProbeError(f"expected x86_64 Mach-O, got cputype 0x{cputype:08x}")
    if ncmds > MAX_LOAD_COMMANDS:
        raise ProbeError("unreasonable Mach-O load-command count")
    _need(data, off + 32, sizeofcmds, "Mach-O load commands")
    return Header64(off, cputype, filetype, ncmds, sizeofcmds)


def _iter_commands(data: bytes, header: Header64) -> Iterable[tuple[int, int, int]]:
    cur = header.offset + 32
    end = cur + header.sizeofcmds
    for _ in range(header.ncmds):
        _need(data, cur, 8, "Mach-O load command")
        cmd, cmdsize = struct.unpack_from("<II", data, cur)
        if cmdsize < 8 or cur + cmdsize > end:
            raise ProbeError("malformed Mach-O load command")
        yield cur, cmd, cmdsize
        cur += cmdsize
    if cur > end:
        raise ProbeError("Mach-O load commands exceed sizeofcmds")


def _cstring_in_command(data: bytes, cmd_off: int, cmdsize: int, relative: int) -> str:
    if relative < 0 or relative >= cmdsize:
        raise ProbeError("fileset entry string offset is invalid")
    start = cmd_off + relative
    end = cmd_off + cmdsize
    nul = data.find(b"\0", start, end)
    if nul < 0:
        raise ProbeError("unterminated fileset entry identifier")
    try:
        return data[start:nul].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProbeError("fileset entry identifier is not UTF-8") from exc


def _find_fileset_entry(data: bytes, outer: Header64, wanted: str) -> tuple[int, int]:
    matches = []
    for cmd_off, cmd, cmdsize in _iter_commands(data, outer):
        if cmd != LC_FILESET_ENTRY:
            continue
        if cmdsize < 32:
            raise ProbeError("short LC_FILESET_ENTRY")
        _cmd, _sz, vmaddr, fileoff, string_off, _reserved = struct.unpack_from("<IIQQII", data, cmd_off)
        entry_id = _cstring_in_command(data, cmd_off, cmdsize, string_off)
        if entry_id == wanted:
            matches.append((int(fileoff), int(vmaddr)))
    if len(matches) != 1:
        raise ProbeError(f"expected exactly one fileset entry {wanted!r}, found {len(matches)}")
    return matches[0]


def _parse_image(data: bytes, header_off: int) -> ImageInfo:
    header = _parse_header(data, header_off)
    uuid = None
    segments = []
    symtab = None
    for cmd_off, cmd, cmdsize in _iter_commands(data, header):
        if cmd == LC_SEGMENT_64:
            if cmdsize < 72:
                raise ProbeError("short LC_SEGMENT_64")
            (_cmd, _sz, raw_name, vmaddr, vmsize, fileoff, filesize,
             _maxprot, _initprot, _nsects, _flags) = struct.unpack_from("<II16sQQQQiiII", data, cmd_off)
            name = raw_name.split(b"\0", 1)[0].decode("ascii", "replace")
            _need(data, int(fileoff), int(filesize), f"segment {name}")
            segments.append(Segment64(name, int(vmaddr), int(vmsize), int(fileoff), int(filesize)))
        elif cmd == LC_UUID:
            if cmdsize < 24:
                raise ProbeError("short LC_UUID")
            if uuid is not None:
                raise ProbeError("multiple LC_UUID commands")
            uuid = bytes(data[cmd_off + 8:cmd_off + 24])
        elif cmd == LC_SYMTAB:
            if cmdsize < 24:
                raise ProbeError("short LC_SYMTAB")
            if symtab is not None:
                raise ProbeError("multiple LC_SYMTAB commands")
            _cmd, _sz, symoff, nsyms, stroff, strsize = struct.unpack_from("<IIIIII", data, cmd_off)
            if nsyms > MAX_SYMBOLS:
                raise ProbeError("unreasonable symbol count")
            _need(data, int(symoff), int(nsyms) * NLIST64_SIZE, "symbol table")
            _need(data, int(stroff), int(strsize), "string table")
            symtab = (int(symoff), int(nsyms), int(stroff), int(strsize))
    if uuid is None:
        raise ProbeError("LC_UUID is missing")
    if not segments:
        raise ProbeError("no LC_SEGMENT_64 commands")
    if symtab is None:
        raise ProbeError("LC_SYMTAB is missing")
    return ImageInfo(header, uuid, tuple(segments), *symtab)


def _symbol_matches(data: bytes, image: ImageInfo, wanted: str) -> list[int]:
    out = []
    str_end = image.stroff + image.strsize
    for i in range(image.nsyms):
        p = image.symoff + i * NLIST64_SIZE
        strx, _typ, _sect, _desc, value = struct.unpack_from("<IbbHQ", data, p)
        if strx == 0 or strx >= image.strsize:
            continue
        start = image.stroff + strx
        nul = data.find(b"\0", start, str_end)
        if nul < 0:
            raise ProbeError("unterminated Mach-O symbol name")
        try:
            name = data[start:nul].decode("utf-8")
        except UnicodeDecodeError:
            continue
        if name == wanted:
            out.append(int(value))
    return out


def _symbol_file_offset(data: bytes, image: ImageInfo, address: int) -> int:
    matches = []
    for seg in image.segments:
        if seg.filesize == 0:
            continue
        if seg.vmaddr <= address < seg.vmaddr + seg.filesize:
            off = seg.fileoff + (address - seg.vmaddr)
            _need(data, off, 32, "symbol prologue")
            matches.append(off)
    if len(matches) != 1:
        raise ProbeError(f"symbol address maps to {len(matches)} file-backed segments")
    return matches[0]


def _image_hash(data: bytes, image: ImageInfo) -> str:
    h = hashlib.sha256()
    for seg in image.segments:
        if seg.filesize == 0:
            continue
        name = seg.name.encode("utf-8")
        h.update(struct.pack("<I", len(name)))
        h.update(name)
        h.update(struct.pack("<QQQQ", seg.vmaddr, seg.vmsize, seg.fileoff, seg.filesize))
        h.update(data[seg.fileoff:seg.fileoff + seg.filesize])
    return h.hexdigest()


def inspect(path: Path, entry: Optional[str], symbol: str) -> dict:
    # Kernel Collections are large. Map them read-only instead of copying the
    # whole file into Python heap; the parser remains bounded/read-only and the
    # OS can page data as the symbol/hash walk needs it.
    try:
        file_size = path.stat().st_size
    except OSError as exc:
        raise ProbeError(f"cannot stat input: {exc}") from exc
    if file_size < 4:
        raise ProbeError("input is too small for Mach-O magic")

    with path.open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as raw:
            data = _slice_x86_64(raw)
            outer = _parse_header(data, 0)

            entry_off = None
            entry_vmaddr = None
            if outer.filetype == MH_FILESET:
                if not entry:
                    raise ProbeError("--entry is required for MH_FILESET kernel collections")
                entry_off, entry_vmaddr = _find_fileset_entry(data, outer, entry)
                image = _parse_image(data, entry_off)
                source_kind = "kernel-collection-fileset"
            else:
                if entry:
                    raise ProbeError("--entry was supplied for a standalone Mach-O")
                image = _parse_image(data, 0)
                source_kind = "standalone-macho"

            matches = _symbol_matches(data, image, symbol)
            if len(matches) != 1:
                raise ProbeError(f"exact symbol count for {symbol!r} is {len(matches)}, expected 1")
            address = matches[0]
            fileoff = _symbol_file_offset(data, image, address)
            prologue = bytes(data[fileoff:fileoff + 32])

            result = {
                "sourceKind": source_kind,
                "sourcePath": str(path),
                "sourceSHA256": hashlib.sha256(data).hexdigest(),
                "imageSHA256": _image_hash(data, image),
                "uuidHex": image.uuid.hex(),
                "mangledSymbol": symbol,
                "symbolAddress": f"{address:016x}",
                "symbolFileOffset": f"{fileoff:x}",
                "prologueHex32": prologue.hex(),
                "prologueSHA256": hashlib.sha256(prologue).hexdigest(),
            }
            if entry is not None:
                result.update({
                    "filesetEntryID": entry,
                    "filesetEntryOffset": f"{entry_off:x}",
                    "filesetEntryVMAddress": f"{entry_vmaddr:016x}",
                })
            return result


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--entry")
    ap.add_argument("--symbol", required=True)
    ap.add_argument("paths", nargs="+")
    ns = ap.parse_args(argv)

    successes = []
    errors = []
    for text in ns.paths:
        path = Path(text)
        if not path.is_file():
            errors.append(f"{path}: not a file")
            continue
        try:
            successes.append(inspect(path, ns.entry, ns.symbol))
        except (OSError, ProbeError) as exc:
            errors.append(f"{path}: {exc}")

    if len(successes) != 1:
        for err in errors:
            print(err, file=sys.stderr)
        print(f"expected exactly one usable ABI source, found {len(successes)}", file=sys.stderr)
        return 2
    print(json.dumps(successes[0], sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
