import re
from itertools import zip_longest

from .config import ServerConfig


RELEASE_ID_PATTERN = re.compile(r"^\d{8}-[a-z0-9]+(?:-[a-z0-9]+)*-r\d+$")
ALLOWED_STAGES = {
    "download",
    "unpack",
    "stage1_start",
    "stage1_end",
    "stage2_start",
    "stage2_end",
    "boot_success",
}


def validate_release_id(release_id: str) -> bool:
    return bool(RELEASE_ID_PATTERN.fullmatch(release_id))


def normalize_release_package_path(release_id: str) -> str:
    return f"{release_id}/{release_id}.tar.gz"


def build_package_url(config: ServerConfig, package_relpath: str) -> str:
    relpath = package_relpath.lstrip("/")
    return f"{config.public_base_url}{config.package_url_prefix}/{relpath}"


def _version_tokens(version: str) -> list[int | str]:
    tokens = re.findall(r"\d+|[a-zA-Z]+", version)
    normalized: list[int | str] = []
    for token in tokens:
        if token.isdigit():
            normalized.append(int(token))
        else:
            normalized.append(token.lower())
    return normalized


def compare_versions(left: str, right: str) -> int:
    left_tokens = _version_tokens(left)
    right_tokens = _version_tokens(right)
    for left_item, right_item in zip_longest(left_tokens, right_tokens, fillvalue=0):
        if left_item == right_item:
            continue
        if isinstance(left_item, int) and isinstance(right_item, int):
            return 1 if left_item > right_item else -1
        return 1 if str(left_item) > str(right_item) else -1
    return 0
