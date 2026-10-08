import sqlite3
import shutil
from datetime import datetime, timezone

from .config import ServerConfig
from .schemas import CheckRequest, RegisterRequest, ReportRequest
from .service import ALLOWED_STAGES, build_package_url, compare_versions, normalize_release_package_path, validate_release_id


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


class OTAStorage:
    def __init__(self, config: ServerConfig):
        self.config = config
        self.config.database_path.parent.mkdir(parents=True, exist_ok=True)
        self.config.package_root.mkdir(parents=True, exist_ok=True)
        self.config.log_dir.mkdir(parents=True, exist_ok=True)

    def connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.config.database_path)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        return conn

    def init_db(self) -> None:
        with self.connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    board_type TEXT NOT NULL,
                    hardware_version TEXT NOT NULL,
                    mac TEXT NOT NULL,
                    current_version TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    last_seen_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS releases (
                    release_id TEXT PRIMARY KEY,
                    board_type TEXT NOT NULL,
                    sys_version TEXT NOT NULL,
                    rollback_index INTEGER NOT NULL,
                    package_relpath TEXT NOT NULL,
                    sha256 TEXT NOT NULL,
                    signing_key_id TEXT,
                    mandatory INTEGER NOT NULL DEFAULT 0,
                    release_note TEXT,
                    created_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS ota_reports (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL,
                    release_id TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    stage TEXT NOT NULL,
                    result TEXT NOT NULL,
                    current_version TEXT NOT NULL,
                    target_version TEXT NOT NULL,
                    reason_code TEXT,
                    reason_message TEXT,
                    reported_at TEXT NOT NULL,
                    FOREIGN KEY(device_id) REFERENCES devices(device_id),
                    FOREIGN KEY(release_id) REFERENCES releases(release_id)
                );

                CREATE INDEX IF NOT EXISTS idx_releases_board_type ON releases(board_type);
                CREATE INDEX IF NOT EXISTS idx_reports_device_id ON ota_reports(device_id);
                CREATE INDEX IF NOT EXISTS idx_reports_release_id ON ota_reports(release_id);
                """
            )

    def upsert_device(self, request: RegisterRequest) -> None:
        timestamp = utc_now()
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO devices (
                    device_id,
                    board_type,
                    hardware_version,
                    mac,
                    current_version,
                    created_at,
                    updated_at,
                    last_seen_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    board_type = excluded.board_type,
                    hardware_version = excluded.hardware_version,
                    mac = excluded.mac,
                    current_version = excluded.current_version,
                    updated_at = excluded.updated_at,
                    last_seen_at = excluded.last_seen_at
                """,
                (
                    request.device_id,
                    request.board_type,
                    request.hardware_version,
                    request.mac,
                    request.current_version,
                    timestamp,
                    timestamp,
                    timestamp,
                ),
            )

    def get_device(self, device_id: str) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                "SELECT * FROM devices WHERE device_id = ?",
                (device_id,),
            ).fetchone()

    def list_devices(self, limit: int = 100) -> list[sqlite3.Row]:
        with self.connect() as conn:
            return conn.execute(
                """
                SELECT * FROM devices
                ORDER BY updated_at DESC
                LIMIT ?
                """,
                (limit,),
            ).fetchall()

    def upsert_release(
        self,
        release_id: str,
        board_type: str,
        sys_version: str,
        rollback_index: int,
        sha256: str,
        signing_key_id: str | None = None,
        mandatory: bool = False,
        release_note: str | None = None,
        package_relpath: str | None = None,
    ) -> None:
        if not validate_release_id(release_id):
            raise ValueError(f"invalid release_id: {release_id}")
        timestamp = utc_now()
        relpath = package_relpath or normalize_release_package_path(release_id)
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO releases (
                    release_id,
                    board_type,
                    sys_version,
                    rollback_index,
                    package_relpath,
                    sha256,
                    signing_key_id,
                    mandatory,
                    release_note,
                    created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(release_id) DO UPDATE SET
                    board_type = excluded.board_type,
                    sys_version = excluded.sys_version,
                    rollback_index = excluded.rollback_index,
                    package_relpath = excluded.package_relpath,
                    sha256 = excluded.sha256,
                    signing_key_id = excluded.signing_key_id,
                    mandatory = excluded.mandatory,
                    release_note = excluded.release_note
                """,
                (
                    release_id,
                    board_type,
                    sys_version,
                    rollback_index,
                    relpath,
                    sha256,
                    signing_key_id,
                    int(mandatory),
                    release_note,
                    timestamp,
                ),
            )

    def get_release(self, release_id: str) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                "SELECT * FROM releases WHERE release_id = ?",
                (release_id,),
            ).fetchone()

    def delete_release(self, release_id: str, delete_files: bool = True) -> sqlite3.Row | None:
        release = self.get_release(release_id)
        if release is None:
            return None

        with self.connect() as conn:
            conn.execute("DELETE FROM ota_reports WHERE release_id = ?", (release_id,))
            conn.execute("DELETE FROM releases WHERE release_id = ?", (release_id,))

        if delete_files:
            release_dir = self.config.package_root / release_id
            if release_dir.is_dir():
                shutil.rmtree(release_dir)

        return release

    def list_releases(self, limit: int = 100) -> list[sqlite3.Row]:
        with self.connect() as conn:
            return conn.execute(
                """
                SELECT * FROM releases
                ORDER BY created_at DESC
                LIMIT ?
                """,
                (limit,),
            ).fetchall()

    def release_archive_path(self, package_relpath: str) -> str:
        return str((self.config.package_root / package_relpath).resolve())

    def find_update(self, request: CheckRequest) -> dict | None:
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT * FROM releases
                WHERE board_type = ?
                ORDER BY created_at DESC, release_id DESC
                """,
                (request.board_type,),
            ).fetchall()

        best_row = None
        for row in rows:
            archive_path = self.config.package_root / row["package_relpath"]
            if not archive_path.is_file():
                continue
            if compare_versions(row["sys_version"], request.current_version) < 0:
                continue
            if best_row is None:
                best_row = row
                break

        if best_row is None:
            return None

        return {
            "release_id": best_row["release_id"],
            "board_type": best_row["board_type"],
            "sys_version": best_row["sys_version"],
            "rollback_index": best_row["rollback_index"],
            "package_url": build_package_url(self.config, best_row["package_relpath"]),
            "sha256": best_row["sha256"],
            "signing_key_id": best_row["signing_key_id"],
            "mandatory": bool(best_row["mandatory"]),
            "release_note": best_row["release_note"],
        }

    def insert_report(self, request: ReportRequest) -> None:
        if request.stage not in ALLOWED_STAGES:
            raise ValueError(f"invalid stage: {request.stage}")

        if self.get_release(request.release_id) is None:
            raise LookupError(f"unknown release_id: {request.release_id}")

        if self.get_device(request.device_id) is None:
            raise LookupError(f"unknown device_id: {request.device_id}")

        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO ota_reports (
                    device_id,
                    release_id,
                    session_id,
                    stage,
                    result,
                    current_version,
                    target_version,
                    reason_code,
                    reason_message,
                    reported_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    request.device_id,
                    request.release_id,
                    request.session_id,
                    request.stage,
                    request.result,
                    request.current_version,
                    request.target_version,
                    request.reason_code,
                    request.reason_message,
                    utc_now(),
                ),
            )

    def list_reports(self, device_id: str) -> list[sqlite3.Row]:
        with self.connect() as conn:
            return conn.execute(
                """
                SELECT * FROM ota_reports
                WHERE device_id = ?
                ORDER BY id ASC
                """,
                (device_id,),
            ).fetchall()

    def list_all_reports(self, limit: int = 100) -> list[sqlite3.Row]:
        with self.connect() as conn:
            return conn.execute(
                """
                SELECT * FROM ota_reports
                ORDER BY id DESC
                LIMIT ?
                """,
                (limit,),
            ).fetchall()

    def list_reports_filtered(
        self,
        release_id: str = "",
        device_id: str = "",
        result: str = "",
        limit: int = 500,
    ) -> list[sqlite3.Row]:
        clauses = []
        params: list[str | int] = []
        if release_id:
            clauses.append("release_id = ?")
            params.append(release_id)
        if device_id:
            clauses.append("device_id = ?")
            params.append(device_id)
        if result:
            clauses.append("result = ?")
            params.append(result)

        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(limit)
        with self.connect() as conn:
            return conn.execute(
                f"""
                SELECT * FROM ota_reports
                {where_sql}
                ORDER BY id DESC
                LIMIT ?
                """,
                params,
            ).fetchall()

    def list_report_filter_options(self) -> dict:
        with self.connect() as conn:
            return {
                "release_ids": [
                    row[0] for row in conn.execute(
                        "SELECT DISTINCT release_id FROM ota_reports ORDER BY release_id DESC"
                    ).fetchall()
                ],
                "device_ids": [
                    row[0] for row in conn.execute(
                        "SELECT DISTINCT device_id FROM ota_reports ORDER BY device_id"
                    ).fetchall()
                ],
                "results": [
                    row[0] for row in conn.execute(
                        "SELECT DISTINCT result FROM ota_reports ORDER BY result"
                    ).fetchall()
                ],
            }

    def count_devices(self) -> int:
        with self.connect() as conn:
            return conn.execute("SELECT COUNT(*) FROM devices").fetchone()[0]

    def count_releases(self) -> int:
        with self.connect() as conn:
            return conn.execute("SELECT COUNT(*) FROM releases").fetchone()[0]

    def count_reports(self) -> int:
        with self.connect() as conn:
            return conn.execute("SELECT COUNT(*) FROM ota_reports").fetchone()[0]
