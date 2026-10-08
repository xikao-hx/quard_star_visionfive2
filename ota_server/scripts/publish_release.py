#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from ota_server.app.config import load_config
from ota_server.app.publisher import publish_release
from ota_server.app.storage import OTAStorage


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Publish OTA package to static package dir")
    parser.add_argument("--config", default=None, help="ota server config path")
    parser.add_argument("--source-dir", required=True, help="source ota_package dir")
    parser.add_argument("--release-id", required=True, help="release id like 20260601-quard-star-r1")
    parser.add_argument("--board-type", required=True, help="target board type")
    parser.add_argument("--release-note", default="", help="release note text")
    parser.add_argument("--mandatory", action="store_true", help="mark as mandatory release")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    config = load_config(args.config)
    storage = OTAStorage(config)
    storage.init_db()
    audit = publish_release(
        config=config,
        storage=storage,
        source_dir=Path(args.source_dir).resolve(),
        release_id=args.release_id,
        board_type=args.board_type,
        mandatory=args.mandatory,
        release_note=args.release_note,
    )
    print(json.dumps(audit, ensure_ascii=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
