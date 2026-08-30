#!/usr/bin/env python3
"""Generate and validate the VisionFive 2 recovery v2 NOR image."""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from nor_layout import Layout, LayoutError, load_layout


RECOVERY_MAGIC = 0x424150A5
RECOVERY_STRUCT_VERSION = 2
RECOVERY_BANK_A = 0
RECOVERY_BANK_B = 1
RECOVERY_BANK_MASK_A = 1 << RECOVERY_BANK_A
RECOVERY_BANK_MASK_B = 1 << RECOVERY_BANK_B
RECOVERY_BANK_MASK_ALL = RECOVERY_BANK_MASK_A | RECOVERY_BANK_MASK_B
RECOVERY_STATE_IDLE = 0
RECOVERY_VALID_STATES = {0, 6, 11, 12, 13, 14, 15, 16, 17}
RECOVERY_ERASE_BLOCK_SIZE = 4096
RECOVERY_SESSION_ID_SIZE = 64
RECOVERY_SYS_VERSION_SIZE = 32
RECOVERY_U32_SIZE = 4

RECOVERY_STRUCT = struct.Struct("<17I64s32s32sI")
RECOVERY_CRC_OFFSET = RECOVERY_STRUCT.size - RECOVERY_U32_SIZE


class RecoveryError(ValueError):
    """Raised when a recovery image does not satisfy the v2 ABI."""


@dataclass(frozen=True)
class RecoveryRecord:
    magic: int
    struct_version: int
    usable_bank: int
    current_bank: int
    target_bank: int
    ota_reboot_cnt: int
    ota_update: int
    ota_state: int
    session_id_crc32: int
    successful_bank_mask: int
    boot_success: int
    rollback_index: int
    pending_rollback_index: int
    current_image_version: int
    pending_image_version: int
    reserved0: int
    reserved1: int
    session_id: str
    current_sys_version: str
    pending_sys_version: str
    header_crc32: int


def _crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def _decode_string(raw: bytes, field: str) -> str:
    terminator = raw.find(b"\0")
    if terminator < 0:
        raise RecoveryError(f"{field} is not NUL-terminated")
    if raw[terminator:] != b"\0" * (len(raw) - terminator):
        raise RecoveryError(f"{field} has nonzero bytes after its terminator")
    try:
        return raw[:terminator].decode("ascii")
    except UnicodeDecodeError as exc:
        raise RecoveryError(f"{field} is not ASCII") from exc


def _recovery_region(layout: Layout):
    region = layout.partition("recovery")
    if layout.erase_size != RECOVERY_ERASE_BLOCK_SIZE:
        raise RecoveryError(
            f"recovery v2 requires 0x{RECOVERY_ERASE_BLOCK_SIZE:x}-byte "
            f"erase blocks, layout has 0x{layout.erase_size:x}"
        )
    if region.size < RECOVERY_ERASE_BLOCK_SIZE:
        raise RecoveryError("recovery partition is smaller than one erase block")
    return region


def _pack_initial_record() -> bytes:
    values = [
        RECOVERY_MAGIC,
        RECOVERY_STRUCT_VERSION,
        RECOVERY_BANK_MASK_ALL,
        RECOVERY_BANK_A,
        RECOVERY_BANK_A,
        0,
        0,
        RECOVERY_STATE_IDLE,
        0,
        RECOVERY_BANK_MASK_A,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        b"\0" * RECOVERY_SESSION_ID_SIZE,
        b"\0" * RECOVERY_SYS_VERSION_SIZE,
        b"\0" * RECOVERY_SYS_VERSION_SIZE,
        0,
    ]
    record = bytearray(RECOVERY_STRUCT.pack(*values))
    struct.pack_into("<I", record, RECOVERY_CRC_OFFSET,
                     _crc32(record[:RECOVERY_CRC_OFFSET]))
    return bytes(record)


def generate_recovery_image(layout: Layout) -> bytes:
    """Return the deterministic full-partition initial recovery image."""
    region = _recovery_region(layout)
    image = bytearray(b"\xFF" * region.size)
    image[:RECOVERY_STRUCT.size] = _pack_initial_record()
    result = bytes(image)
    record = validate_recovery_image(layout, result)
    if not is_initial_record(record):
        raise RecoveryError("generated recovery record is not the initial state")
    return result


def validate_recovery_image(layout: Layout, image: bytes) -> RecoveryRecord:
    """Validate one full recovery partition image and return its record."""
    region = _recovery_region(layout)
    if len(image) != region.size:
        raise RecoveryError(
            f"recovery image size is 0x{len(image):x}, expected 0x{region.size:x}"
        )
    if image[RECOVERY_STRUCT.size:] != b"\xFF" * (
            len(image) - RECOVERY_STRUCT.size):
        raise RecoveryError("recovery image padding is not filled with 0xFF")

    fields = RECOVERY_STRUCT.unpack_from(image)
    stored_crc = fields[-1]
    calculated_crc = _crc32(image[:RECOVERY_CRC_OFFSET])
    if stored_crc != calculated_crc:
        raise RecoveryError(
            f"recovery CRC32 mismatch: stored 0x{stored_crc:08x}, "
            f"calculated 0x{calculated_crc:08x}"
        )

    record = RecoveryRecord(
        *fields[:17],
        _decode_string(fields[17], "session_id"),
        _decode_string(fields[18], "current_sys_version"),
        _decode_string(fields[19], "pending_sys_version"),
        stored_crc,
    )
    if record.magic != RECOVERY_MAGIC:
        raise RecoveryError(f"invalid recovery magic 0x{record.magic:08x}")
    if record.struct_version != RECOVERY_STRUCT_VERSION:
        raise RecoveryError(
            f"unsupported recovery version {record.struct_version}"
        )
    if record.current_bank > RECOVERY_BANK_B or record.target_bank > RECOVERY_BANK_B:
        raise RecoveryError("recovery bank index is invalid")
    if record.usable_bank & ~RECOVERY_BANK_MASK_ALL:
        raise RecoveryError("usable bank mask is invalid")
    if record.successful_bank_mask & ~RECOVERY_BANK_MASK_ALL:
        raise RecoveryError("successful bank mask is invalid")
    if record.successful_bank_mask & ~record.usable_bank:
        raise RecoveryError("successful bank mask is not a subset of usable banks")
    if record.boot_success not in (0, 1):
        raise RecoveryError("boot_success is not boolean")
    if record.ota_state not in RECOVERY_VALID_STATES:
        raise RecoveryError(f"unsupported OTA state {record.ota_state}")
    if record.reserved0 or record.reserved1:
        raise RecoveryError("recovery reserved fields are nonzero")
    return record


def is_initial_record(record: RecoveryRecord) -> bool:
    return (
        record.usable_bank == RECOVERY_BANK_MASK_ALL
        and record.current_bank == RECOVERY_BANK_A
        and record.target_bank == RECOVERY_BANK_A
        and record.ota_reboot_cnt == 0
        and record.ota_update == 0
        and record.ota_state == RECOVERY_STATE_IDLE
        and record.session_id_crc32 == 0
        and record.successful_bank_mask == RECOVERY_BANK_MASK_A
        and record.boot_success == 1
        and record.rollback_index == 0
        and record.pending_rollback_index == 0
        and record.current_image_version == 0
        and record.pending_image_version == 0
        and not record.session_id
        and not record.current_sys_version
        and not record.pending_sys_version
    )


def render_recovery_report(layout: Layout, record: RecoveryRecord) -> str:
    region = _recovery_region(layout)
    return "\n".join([
        "VisionFive 2 recovery v2",
        f"flash_offset: 0x{region.offset:08x}",
        f"image_size: 0x{region.size:08x}",
        f"record_size: 0x{RECOVERY_STRUCT.size:x}",
        f"header_crc32: 0x{record.header_crc32:08x}",
        f"usable_bank: 0x{record.usable_bank:x}",
        f"current_bank: {record.current_bank}",
        f"target_bank: {record.target_bank}",
        f"successful_bank_mask: 0x{record.successful_bank_mask:x}",
        f"boot_success: {record.boot_success}",
        f"ota_state: {record.ota_state}",
        f"ota_reboot_cnt: {record.ota_reboot_cnt}",
        "",
    ])


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
            image = generate_recovery_image(layout)
            _write(args.generate, image)
        else:
            image = args.check.read_bytes()
        record = validate_recovery_image(layout, image)
        report = render_recovery_report(layout, record)
        if args.report is not None:
            _write(args.report, report)
        else:
            print(report, end="")
    except (RecoveryError, LayoutError, OSError) as exc:
        print(f"recovery image error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
