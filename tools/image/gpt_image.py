#!/usr/bin/env python3
"""Generate and validate the relocated VisionFive 2 NOR GPT image."""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from nor_layout import Layout, LayoutError, Region, load_layout


GPT_SIGNATURE = b"EFI PART"
GPT_REVISION = 0x00010000
GPT_HEADER_SIZE = 92
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_HEADER_LBA = 1
GPT_ENTRY_LBA = 2
GPT_PROTECTIVE_TYPE = 0xEE
GPT_BASIC_DATA_GUID = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
GPT_UUID_NAMESPACE = uuid.UUID("635e8669-365b-54ba-82d0-e0905bc1918c")

HEADER_STRUCT = struct.Struct("<8sIIIIQQQQ16sQIII")
ENTRY_STRUCT = struct.Struct("<16s16sQQQ72s")
MBR_ENTRY_STRUCT = struct.Struct("<BBBBBBBBII")


class GptError(ValueError):
    """Raised when a generated or supplied NOR GPT image is invalid."""


@dataclass(frozen=True)
class GptPartition:
    name: str
    first_lba: int
    last_lba: int

    @property
    def sector_count(self) -> int:
        return self.last_lba - self.first_lba + 1


@dataclass(frozen=True)
class GptImage:
    disk_guid: uuid.UUID
    partitions: tuple[GptPartition, ...]


def _crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def _disk_guid(layout: Layout) -> uuid.UUID:
    identity = (
        f"visionfive2-nor-layout-v{layout.version}:"
        f"{layout.flash_size}:{layout.sector_size}"
    )
    return uuid.uuid5(GPT_UUID_NAMESPACE, identity)


def _partition_guid(disk_guid: uuid.UUID, partition: Region) -> uuid.UUID:
    return uuid.uuid5(
        disk_guid, f"{partition.name}:{partition.offset}:{partition.size}"
    )


def _encode_name(name: str) -> bytes:
    encoded = name.encode("utf-16-le")
    if len(encoded) > 72:
        raise GptError(f"partition name is too long for GPT: {name}")
    return encoded.ljust(72, b"\0")


def _require_gpt_geometry(layout: Layout) -> Region:
    if layout.sector_size != 512:
        raise GptError(
            f"GPT requires 512-byte sectors, layout has {layout.sector_size}"
        )
    gpt = layout.partition("gpt")
    minimum_size = (GPT_ENTRY_LBA * layout.sector_size
                    + GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    if gpt.size < minimum_size:
        raise GptError(
            f"gpt partition is 0x{gpt.size:x}, needs at least 0x{minimum_size:x}"
        )
    if layout.flash_size % layout.sector_size:
        raise GptError("flash_size must be a whole number of GPT sectors")
    return gpt


def generate_gpt_image(layout: Layout) -> bytes:
    """Return a deterministic, full-partition-size relocated GPT image."""
    gpt_region = _require_gpt_geometry(layout)
    sector_size = layout.sector_size
    flash_lbas = layout.flash_size // sector_size
    disk_guid = _disk_guid(layout)

    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    for index, partition in enumerate(layout.partitions):
        if index >= GPT_ENTRY_COUNT:
            raise GptError("layout contains more GPT partitions than entry slots")
        if partition.offset % sector_size or partition.size % sector_size:
            raise GptError(f"partition {partition.name} is not sector aligned")
        first_lba = partition.offset // sector_size
        last_lba = partition.end // sector_size - 1
        ENTRY_STRUCT.pack_into(
            entries,
            index * GPT_ENTRY_SIZE,
            GPT_BASIC_DATA_GUID.bytes_le,
            _partition_guid(disk_guid, partition).bytes_le,
            first_lba,
            last_lba,
            0,
            _encode_name(partition.name),
        )

    entry_crc = _crc32(entries)
    header = bytearray(sector_size)
    HEADER_STRUCT.pack_into(
        header,
        0,
        GPT_SIGNATURE,
        GPT_REVISION,
        GPT_HEADER_SIZE,
        0,
        0,
        GPT_HEADER_LBA,
        flash_lbas - 1,
        0,
        flash_lbas - 1,
        disk_guid.bytes_le,
        GPT_ENTRY_LBA,
        GPT_ENTRY_COUNT,
        GPT_ENTRY_SIZE,
        entry_crc,
    )
    struct.pack_into("<I", header, 16, _crc32(header[:GPT_HEADER_SIZE]))

    mbr = bytearray(sector_size)
    MBR_ENTRY_STRUCT.pack_into(
        mbr,
        446,
        0,
        0,
        2,
        0,
        GPT_PROTECTIVE_TYPE,
        0xFF,
        0xFF,
        0xFF,
        1,
        min(flash_lbas - 1, 0xFFFFFFFF),
    )
    struct.pack_into("<H", mbr, 510, 0xAA55)

    image = bytearray(b"\xFF" * gpt_region.size)
    image[0:sector_size] = mbr
    image[sector_size:2 * sector_size] = header
    entries_offset = GPT_ENTRY_LBA * sector_size
    image[entries_offset:entries_offset + len(entries)] = entries

    result = bytes(image)
    validate_gpt_image(layout, result)
    return result


def _decode_name(raw: bytes, index: int) -> str:
    try:
        text = raw.decode("utf-16-le")
    except UnicodeDecodeError as exc:
        raise GptError(f"entry {index} has invalid UTF-16LE name") from exc
    name = text.split("\0", 1)[0]
    if not name:
        raise GptError(f"entry {index} has an empty name")
    if any(ord(character) > 0x7F for character in name):
        raise GptError(f"entry {index} name is not ASCII-compatible")
    return name


def validate_gpt_image(layout: Layout, image: bytes) -> GptImage:
    """Validate an image and require exact agreement with the canonical layout."""
    gpt_region = _require_gpt_geometry(layout)
    sector_size = layout.sector_size
    flash_lbas = layout.flash_size // sector_size
    if len(image) != gpt_region.size:
        raise GptError(
            f"GPT image size is 0x{len(image):x}, expected 0x{gpt_region.size:x}"
        )

    if struct.unpack_from("<H", image, 510)[0] != 0xAA55:
        raise GptError("protective MBR signature is invalid")
    mbr_entry = MBR_ENTRY_STRUCT.unpack_from(image, 446)
    if mbr_entry[4] != GPT_PROTECTIVE_TYPE:
        raise GptError("protective MBR partition type is not 0xEE")
    if mbr_entry[8] != 1 or mbr_entry[9] != min(flash_lbas - 1, 0xFFFFFFFF):
        raise GptError("protective MBR does not describe the whole NOR")

    header_offset = GPT_HEADER_LBA * sector_size
    fields = HEADER_STRUCT.unpack_from(image, header_offset)
    (
        signature,
        revision,
        header_size,
        stored_header_crc,
        reserved,
        current_lba,
        alternate_lba,
        first_usable_lba,
        last_usable_lba,
        raw_disk_guid,
        entry_lba,
        entry_count,
        entry_size,
        stored_entry_crc,
    ) = fields
    if signature != GPT_SIGNATURE:
        raise GptError("GPT header signature is invalid")
    if revision != GPT_REVISION:
        raise GptError(f"unsupported GPT revision: 0x{revision:08x}")
    if header_size != GPT_HEADER_SIZE or header_size > sector_size:
        raise GptError(f"invalid GPT header size: {header_size}")

    header_bytes = bytearray(
        image[header_offset:header_offset + header_size]
    )
    struct.pack_into("<I", header_bytes, 16, 0)
    if _crc32(header_bytes) != stored_header_crc:
        raise GptError("GPT header CRC32 mismatch")

    if reserved != 0:
        raise GptError("GPT header reserved field is nonzero")
    if current_lba != GPT_HEADER_LBA or entry_lba != GPT_ENTRY_LBA:
        raise GptError("GPT header local LBA fields are invalid")
    if (alternate_lba != flash_lbas - 1 or first_usable_lba != 0
            or last_usable_lba != flash_lbas - 1):
        raise GptError("GPT header absolute NOR LBA range is invalid")
    if entry_count != GPT_ENTRY_COUNT or entry_size != GPT_ENTRY_SIZE:
        raise GptError("GPT entry geometry is invalid")

    entries_offset = entry_lba * sector_size
    entries_size = entry_count * entry_size
    entries_end = entries_offset + entries_size
    if entries_end > len(image):
        raise GptError("GPT entry array exceeds image")
    entries = image[entries_offset:entries_end]
    if _crc32(entries) != stored_entry_crc:
        raise GptError("GPT entry array CRC32 mismatch")

    disk_guid = uuid.UUID(bytes_le=raw_disk_guid)
    if disk_guid != _disk_guid(layout):
        raise GptError("GPT disk GUID does not match the canonical layout")

    parsed: list[GptPartition] = []
    for index in range(entry_count):
        raw_entry = entries[index * entry_size:(index + 1) * entry_size]
        if raw_entry[:16] == b"\0" * 16:
            if raw_entry != b"\0" * entry_size:
                raise GptError(f"unused GPT entry {index} is not zero-filled")
            continue
        raw_type, raw_unique, first_lba, last_lba, attributes, raw_name = (
            ENTRY_STRUCT.unpack(raw_entry)
        )
        if uuid.UUID(bytes_le=raw_type) != GPT_BASIC_DATA_GUID:
            raise GptError(f"entry {index} has an unexpected type GUID")
        if first_lba > last_lba or last_lba >= flash_lbas:
            raise GptError(f"entry {index} has an invalid absolute LBA range")
        if attributes != 0:
            raise GptError(f"entry {index} has unsupported attributes")
        name = _decode_name(raw_name, index)
        try:
            expected = layout.partitions[len(parsed)]
        except IndexError as exc:
            raise GptError("GPT contains more partitions than the layout") from exc
        if name != expected.name:
            raise GptError(
                f"entry {index} name is {name!r}, expected {expected.name!r}"
            )
        expected_first = expected.offset // sector_size
        expected_last = expected.end // sector_size - 1
        if (first_lba, last_lba) != (expected_first, expected_last):
            raise GptError(
                f"entry {name} LBA range is {first_lba}-{last_lba}, expected "
                f"{expected_first}-{expected_last}"
            )
        if uuid.UUID(bytes_le=raw_unique) != _partition_guid(disk_guid, expected):
            raise GptError(f"entry {name} unique GUID is not deterministic")
        parsed.append(GptPartition(name, first_lba, last_lba))

    if len(parsed) != len(layout.partitions):
        raise GptError(
            f"GPT has {len(parsed)} partitions, expected {len(layout.partitions)}"
        )
    if image[entries_end:] != b"\xFF" * (len(image) - entries_end):
        raise GptError("unused GPT image area is not filled with 0xFF")
    return GptImage(disk_guid=disk_guid, partitions=tuple(parsed))


def render_gpt_report(layout: Layout, parsed: GptImage) -> str:
    lines = [
        "VisionFive 2 relocated NOR GPT",
        f"flash_offset: 0x{layout.partition('gpt').offset:08x}",
        f"image_size: 0x{layout.partition('gpt').size:08x}",
        f"disk_guid: {parsed.disk_guid}",
        "",
        "name             first_lba   last_lba    offset      size",
    ]
    for partition in parsed.partitions:
        offset = partition.first_lba * layout.sector_size
        size = partition.sector_count * layout.sector_size
        lines.append(
            f"{partition.name:<16} {partition.first_lba:<11} "
            f"{partition.last_lba:<11} 0x{offset:08x} 0x{size:08x}"
        )
    lines.append("")
    return "\n".join(lines)


def _write(path: Path, data: bytes | str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8")
    else:
        path.write_bytes(data)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--generate", metavar="IMAGE", type=Path)
    action.add_argument("--check", metavar="IMAGE", type=Path)
    parser.add_argument("--report", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        layout = load_layout(args.config)
        if args.generate is not None:
            image = generate_gpt_image(layout)
            _write(args.generate, image)
        else:
            image = args.check.read_bytes()
        parsed = validate_gpt_image(layout, image)
        report = render_gpt_report(layout, parsed)
        if args.report is not None:
            _write(args.report, report)
        else:
            print(report, end="")
    except (GptError, LayoutError, OSError) as exc:
        print(f"NOR GPT error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
