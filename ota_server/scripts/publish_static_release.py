#!/usr/bin/env python3
import argparse
import json
import os
import sys
import tarfile
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OTA_ROOT = REPO_ROOT / "ota_server" / "static_ota"
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from ota_server.app.publisher import (
    _safe_extract_tar_gz,
    _sha256_file,
    build_release_audit,
    validate_package_dir,
)
from ota_server.app.config import load_config
from ota_server.app.service import validate_release_id
from ota_server.app.storage import OTAStorage


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Publish OTA package to nginx static OTA root")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--source-dir", help="source ota_package dir")
    source.add_argument("--archive", help="existing ota_package tar.gz archive")
    parser.add_argument("--release-id", required=True, help="release id like 20260629-quard-star-r1")
    parser.add_argument("--board-type", required=True, help="target board type")
    parser.add_argument("--public-base-url", required=True, help="public base URL, e.g. http://127.0.0.1:18080")
    parser.add_argument("--ota-root", default=str(DEFAULT_OTA_ROOT), help="static OTA root")
    parser.add_argument("--url-prefix", default="/ota", help="public URL prefix")
    parser.add_argument("--config", default=None, help="ota server config path for admin/report database")
    parser.add_argument("--no-db", action="store_true", help="do not update FastAPI admin database")
    parser.add_argument("--release-note", default="", help="release note text")
    parser.add_argument("--mandatory", action="store_true", help="mark as mandatory release")
    return parser.parse_args()


def normalize_url_prefix(prefix: str) -> str:
    if not prefix.startswith("/"):
        raise ValueError("--url-prefix must start with /")
    return prefix.rstrip("/")


def prepare_source_dir(args: argparse.Namespace, temp_root: Path | None) -> Path:
    if args.source_dir:
        return Path(args.source_dir).resolve()

    if temp_root is None:
        raise ValueError("internal error: temp root missing")

    _safe_extract_tar_gz(Path(args.archive).resolve(), temp_root)
    source_dir = temp_root / "ota_package"
    if not source_dir.is_dir():
        raise ValueError("archive must contain top-level ota_package directory")
    return source_dir


def write_json_atomic(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    text = json.dumps(payload, ensure_ascii=True, indent=2) + "\n"
    json.loads(text)
    temp_path.write_text(text, encoding="utf-8")
    os.replace(temp_path, path)


def sync_release_db(args: argparse.Namespace, metadata: dict, sha256_value: str) -> None:
    if args.no_db:
        return

    config = load_config(args.config)
    storage = OTAStorage(config)
    storage.init_db()
    storage.upsert_release(
        release_id=args.release_id,
        board_type=args.board_type,
        sys_version=metadata["sys_version"],
        rollback_index=int(metadata["rollback_index"]),
        sha256=sha256_value,
        signing_key_id=metadata.get("signing", {}).get("key_id"),
        mandatory=args.mandatory,
        release_note=args.release_note,
        package_relpath=f"{args.release_id}/{args.release_id}.tar.gz",
    )


def publish(args: argparse.Namespace, source_dir: Path) -> dict:
    if not validate_release_id(args.release_id):
        raise ValueError(f"invalid release_id: {args.release_id}")
    if not args.board_type.strip():
        raise ValueError("board_type must not be empty")

    url_prefix = normalize_url_prefix(args.url_prefix)
    ota_root = Path(args.ota_root).resolve()
    release_dir = ota_root / "packages" / args.release_id
    archive_path = release_dir / f"{args.release_id}.tar.gz"
    manifest_path = ota_root / "manifests" / f"{args.board_type}.json"

    metadata = validate_package_dir(source_dir)
    release_dir.mkdir(parents=True, exist_ok=True)

    with tarfile.open(archive_path, "w:gz") as tar_obj:
        tar_obj.add(source_dir, arcname="ota_package")

    sha256_value = _sha256_file(archive_path)
    audit = build_release_audit(
        release_id=args.release_id,
        board_type=args.board_type,
        metadata=metadata,
        archive_path=archive_path,
        sha256_value=sha256_value,
        mandatory=args.mandatory,
        release_note=args.release_note,
    )
    write_json_atomic(release_dir / "release.json", audit)

    public_base_url = args.public_base_url.rstrip("/")
    package_url = (
        f"{public_base_url}{url_prefix}/packages/"
        f"{args.release_id}/{args.release_id}.tar.gz"
    )
    manifest = {
        "format_version": 1,
        "board_type": args.board_type,
        "release_id": args.release_id,
        "sys_version": metadata["sys_version"],
        "rollback_index": int(metadata["rollback_index"]),
        "package_url": package_url,
        "package_sha256": sha256_value,
        "package_size": archive_path.stat().st_size,
        "mandatory": bool(args.mandatory),
        "release_note": args.release_note,
    }
    write_json_atomic(manifest_path, manifest)
    sync_release_db(args, metadata, sha256_value)
    return {
        "release_dir": str(release_dir),
        "archive_path": str(archive_path),
        "release_json": str(release_dir / "release.json"),
        "manifest_path": str(manifest_path),
        "sha256": sha256_value,
        "package_size": archive_path.stat().st_size,
        "package_url": package_url,
        "db_synced": not args.no_db,
    }


def main() -> int:
    args = parse_args()
    try:
        with tempfile.TemporaryDirectory() as tempdir:
            temp_root = Path(tempdir)
            source_dir = prepare_source_dir(args, temp_root)
            result = publish(args, source_dir)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"publish package: {result['archive_path']}")
    print(f"sha256: {result['sha256']}")
    print(f"package_size: {result['package_size']}")
    print(f"release.json: {result['release_json']}")
    print(f"manifest updated: {result['manifest_path']}")
    print(f"package_url: {result['package_url']}")
    print(f"db_synced: {str(result['db_synced']).lower()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
