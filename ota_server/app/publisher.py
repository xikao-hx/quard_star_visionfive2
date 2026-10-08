import hashlib
import json
import tarfile
import tempfile
from pathlib import Path

from .config import ServerConfig
from .service import normalize_release_package_path, validate_release_id
from .storage import OTAStorage


REQUIRED_PACKAGE_FILES = {
    "data.json",
    "manifest.hash",
    "signature.sig",
}


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file_obj:
        for chunk in iter(lambda: file_obj.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_extract_tar_gz(archive_path: Path, target_dir: Path) -> None:
    with tarfile.open(archive_path, "r:gz") as tar_obj:
        for member in tar_obj.getmembers():
            member_path = (target_dir / member.name).resolve()
            if target_dir.resolve() not in member_path.parents and member_path != target_dir.resolve():
                raise ValueError("unsafe archive path detected")
        tar_obj.extractall(target_dir)


def load_package_metadata(package_dir: Path) -> dict:
    metadata_path = package_dir / "data.json"
    try:
        return json.loads(metadata_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"missing package metadata: {metadata_path}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid package metadata json: {metadata_path}") from exc


def validate_package_dir(package_dir: Path) -> dict:
    if not package_dir.is_dir():
        raise ValueError(f"package dir not found: {package_dir}")

    missing = sorted(name for name in REQUIRED_PACKAGE_FILES if not (package_dir / name).is_file())
    if missing:
        raise ValueError(f"missing package files: {', '.join(missing)}")

    metadata = load_package_metadata(package_dir)
    for key in ("sys_version", "rollback_index", "signing", "payloads"):
        if key not in metadata:
            raise ValueError(f"missing metadata key: {key}")

    signing = metadata.get("signing", {})
    if not isinstance(signing, dict) or not signing.get("key_id"):
        raise ValueError("missing signing.key_id in data.json")

    payloads = metadata.get("payloads", [])
    if not isinstance(payloads, list) or not payloads:
        raise ValueError("payloads must be a non-empty list")

    for payload in payloads:
        payload_file = payload.get("file")
        if not payload_file:
            raise ValueError("payload file missing in data.json")
        if not (package_dir / payload_file).is_file():
            raise ValueError(f"missing payload file: {payload_file}")

    return metadata


def build_release_audit(
    release_id: str,
    board_type: str,
    metadata: dict,
    archive_path: Path,
    sha256_value: str,
    mandatory: bool,
    release_note: str,
) -> dict:
    return {
        "release_id": release_id,
        "board_type": board_type,
        "sys_version": metadata["sys_version"],
        "rollback_index": metadata["rollback_index"],
        "archive_path": str(archive_path),
        "archive_name": archive_path.name,
        "archive_size": archive_path.stat().st_size,
        "sha256": sha256_value,
        "signing": metadata["signing"],
        "mandatory": mandatory,
        "release_note": release_note,
    }


def publish_release(
    config: ServerConfig,
    storage: OTAStorage,
    source_dir: Path,
    release_id: str,
    board_type: str,
    mandatory: bool = False,
    release_note: str = "",
) -> dict:
    if not validate_release_id(release_id):
        raise ValueError(f"invalid release_id: {release_id}")

    metadata = validate_package_dir(source_dir)
    release_dir = config.package_root / release_id
    release_dir.mkdir(parents=True, exist_ok=True)

    package_relpath = normalize_release_package_path(release_id)
    archive_path = config.package_root / package_relpath

    with tarfile.open(archive_path, "w:gz") as tar_obj:
        tar_obj.add(source_dir, arcname="ota_package")

    sha256_value = _sha256_file(archive_path)
    audit = build_release_audit(
        release_id=release_id,
        board_type=board_type,
        metadata=metadata,
        archive_path=archive_path,
        sha256_value=sha256_value,
        mandatory=mandatory,
        release_note=release_note,
    )

    (release_dir / "release.json").write_text(
        json.dumps(audit, ensure_ascii=True, indent=2) + "\n",
        encoding="utf-8",
    )

    storage.upsert_release(
        release_id=release_id,
        board_type=board_type,
        sys_version=metadata["sys_version"],
        rollback_index=int(metadata["rollback_index"]),
        sha256=sha256_value,
        signing_key_id=metadata["signing"]["key_id"],
        mandatory=mandatory,
        release_note=release_note,
        package_relpath=package_relpath,
    )
    return audit


def publish_release_archive(
    config: ServerConfig,
    storage: OTAStorage,
    archive_path: Path,
    release_id: str,
    board_type: str,
    mandatory: bool = False,
    release_note: str = "",
) -> dict:
    with tempfile.TemporaryDirectory() as tempdir:
        temp_root = Path(tempdir)
        _safe_extract_tar_gz(archive_path, temp_root)
        package_dir = temp_root / "ota_package"
        if not package_dir.is_dir():
            raise ValueError("archive must contain top-level ota_package directory")
        return publish_release(
            config=config,
            storage=storage,
            source_dir=package_dir,
            release_id=release_id,
            board_type=board_type,
            mandatory=mandatory,
            release_note=release_note,
        )
