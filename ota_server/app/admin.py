from pathlib import Path

from .config import ServerConfig
from .service import build_package_url
from .storage import OTAStorage


def build_report_sessions(reports: list[dict]) -> list[dict]:
    sessions: dict[tuple[str, str, str], dict] = {}
    for report in reversed(reports):
        key = (report["device_id"], report["release_id"], report["session_id"])
        session = sessions.setdefault(
            key,
            {
                "device_id": report["device_id"],
                "release_id": report["release_id"],
                "session_id": report["session_id"],
                "source_version": report["current_version"],
                "target_version": report["target_version"],
                "started_at": report["reported_at"],
                "updated_at": report["reported_at"],
                "status": report["result"],
                "events": [],
            },
        )
        session["events"].append(report)
        session["updated_at"] = report["reported_at"]
        session["status"] = report["result"]
        if report["result"] == "failed":
            session["status"] = "failed"

    return sorted(sessions.values(), key=lambda item: item["updated_at"], reverse=True)


def build_dashboard(store: OTAStorage, config: ServerConfig) -> dict:
    releases = [dict(row) for row in store.list_releases(limit=5)]
    for release in releases:
        release["package_url"] = build_package_url(config, release["package_relpath"])

    return {
        "device_count": store.count_devices(),
        "release_count": store.count_releases(),
        "report_count": store.count_reports(),
        "recent_devices": [dict(row) for row in store.list_devices(limit=5)],
        "recent_releases": releases,
        "recent_reports": [dict(row) for row in store.list_all_reports(limit=5)],
    }


def build_release_rows(store: OTAStorage, config: ServerConfig) -> list[dict]:
    rows = []
    for row in store.list_releases(limit=100):
        item = dict(row)
        item["package_url"] = build_package_url(config, item["package_relpath"])
        item["archive_exists"] = Path(store.release_archive_path(item["package_relpath"])).is_file()
        rows.append(item)
    return rows
