#!/usr/bin/env python3

"""Build a signed VisionFive2 AMP A/B OTA package."""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


FORMAT_VERSION = 1
SIGNING_ALGORITHM = "RSA2048-SHA256"
MANIFEST_NAME = "data.json"
MANIFEST_HASH_NAME = "manifest.hash"
SIGNATURE_NAME = "signature.sig"
TRIM_SCAN_CHUNK = 1024 * 1024


def fail(message):
    raise SystemExit(f"Error: {message}")


def sha256_hex(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_payload_arg(raw):
    if "=" not in raw or ":" not in raw.split("=", 1)[0]:
        fail(f"invalid payload '{raw}', expected partition:stage=source")

    left, source = raw.split("=", 1)
    partition, stage_text = left.split(":", 1)
    partition = partition.strip()
    source = source.strip()
    if not partition or not source:
        fail(f"invalid payload '{raw}', partition and source must not be empty")

    try:
        stage = int(stage_text)
    except ValueError:
        fail(f"invalid stage in payload '{raw}'")
    if stage not in (1, 2):
        fail(f"invalid stage in payload '{raw}', expected 1 or 2")
    return partition, stage, Path(source)


def opposite_bank(bank):
    return "a" if bank == "b" else "b"


def payloads_for_bank(repo, bank, stage):
    target_dir = repo / "work/target/amp"
    return [
        (f"fw_payload_{bank}", stage, target_dir / "visionfive2_fw_payload_amp.img"),
        (f"boot_{bank}", stage, target_dir / "starfive-visionfive2-vfat.part"),
        (f"rootfs_{bank}", stage, target_dir / "rootfs.ext4"),
        (f"spl_{bank}", stage, target_dir / "u-boot-amp-spl.bin.normal.out"),
    ]


def automatic_payloads(repo, target_bank, include_stage2):
    specs = payloads_for_bank(repo, target_bank, 1)
    if include_stage2:
        specs.extend(payloads_for_bank(repo, opposite_bank(target_bank), 2))
    return specs


def find_trimmed_size(path):
    end = path.stat().st_size
    with path.open("rb") as stream:
        while end > 0:
            chunk_size = min(TRIM_SCAN_CHUNK, end)
            stream.seek(end - chunk_size)
            chunk = stream.read(chunk_size)
            for index in range(chunk_size - 1, -1, -1):
                if chunk[index] != 0:
                    return end - chunk_size + index + 1
            end -= chunk_size
    return 0


def copy_payload(source, destination, partition):
    partition_size = source.stat().st_size
    if partition_size <= 0:
        fail(f"payload source is empty: {source}")

    # Only rootfs is a raw filesystem image whose zero tail may be restored by writer.
    if partition.startswith("rootfs_"):
        payload_size = find_trimmed_size(source)
        if payload_size == 0:
            fail(f"rootfs payload contains only zero bytes: {source}")
        with source.open("rb") as src, destination.open("wb") as dst:
            remaining = payload_size
            while remaining:
                chunk = src.read(min(TRIM_SCAN_CHUNK, remaining))
                if not chunk:
                    fail(f"unexpected EOF while copying {source}")
                dst.write(chunk)
                remaining -= len(chunk)
        return payload_size, partition_size

    shutil.copyfile(source, destination)
    return partition_size, partition_size


def write_json(path, value):
    with path.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=True, indent=2)
        stream.write("\n")


def signature_input(package_dir, manifest_path, payload_names):
    names = sorted([manifest_path.name] + payload_names)
    data = bytearray()
    for name in names:
        data.extend(bytes.fromhex(sha256_hex(package_dir / name)))
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description="Build a signed VisionFive2 AMP A/B OTA package")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--private-key", required=True)
    parser.add_argument("--sys-version", required=True)
    parser.add_argument("--rollback-index", required=True, type=int)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--key-id", default="ota-dev-01")
    parser.add_argument("--payload", action="append", default=[],
                        help="custom payload partition:stage=source")
    parser.add_argument("--auto-from-target", action="store_true",
                        help="use the AMP SDK images under work/target/amp")
    parser.add_argument("--project-root", help="repository root for --auto-from-target")
    parser.add_argument("--target-bank", choices=("a", "b"), default="b")
    parser.add_argument("--include-stage2", action="store_true")
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    private_key = Path(args.private_key).resolve()
    if not private_key.is_file():
        fail(f"private key not found: {private_key}")
    if args.rollback_index < 0 or args.rollback_index > 0xFFFFFFFF:
        fail("rollback index must be a uint32 value")

    specs = [parse_payload_arg(item) for item in args.payload]
    if args.auto_from_target:
        if specs:
            fail("--auto-from-target and --payload cannot be used together")
        repo = Path(args.project_root).resolve() if args.project_root else Path(__file__).resolve().parents[2]
        specs = automatic_payloads(repo, args.target_bank, args.include_stage2)
    if not specs:
        fail("at least one --payload or --auto-from-target is required")

    partitions = [partition for partition, _, _ in specs]
    if len(partitions) != len(set(partitions)):
        fail("duplicate payload partition detected")

    for _, _, source in specs:
        if not source.resolve().is_file():
            fail(f"required payload source not found: {source.resolve()}")

    if output_dir.exists():
        fail(f"output directory already exists: {output_dir}")
    output_dir.mkdir(parents=True)

    manifest_payloads = []
    payload_names = []
    for partition, stage, source in specs:
        source = source.resolve()
        suffix = ".vfat.ota" if partition.startswith("boot_") else ".img.ota"
        target_name = f"{partition}{suffix}"
        target = output_dir / target_name
        size, partition_size = copy_payload(source, target, partition)
        manifest_payloads.append({
            "partition": partition,
            "file": target_name,
            "sha256": sha256_hex(target),
            "size": size,
            "partition_size": partition_size,
            "stage": stage,
        })
        payload_names.append(target_name)

    manifest = {
        "format_version": FORMAT_VERSION,
        "sys_version": args.sys_version,
        "session_id": args.session_id,
        "rollback_index": args.rollback_index,
        "signing": {"algorithm": SIGNING_ALGORITHM, "key_id": args.key_id},
        "payloads": manifest_payloads,
    }
    manifest_path = output_dir / MANIFEST_NAME
    write_json(manifest_path, manifest)
    (output_dir / MANIFEST_HASH_NAME).write_text(sha256_hex(manifest_path) + "\n", encoding="ascii")

    sign_input = output_dir / ".signature-input.bin"
    sign_input.write_bytes(signature_input(output_dir, manifest_path, payload_names))
    try:
        subprocess.run([
            "openssl", "dgst", "-sha256", "-sign", str(private_key),
            "-out", str(output_dir / SIGNATURE_NAME), str(sign_input),
        ], check=True)
    finally:
        sign_input.unlink(missing_ok=True)

    print(f"OTA package created at {output_dir}")


if __name__ == "__main__":
    main()
