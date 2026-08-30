#!/usr/bin/env python3
"""Validate the VisionFive 2 NOR layout and generate derived artifacts."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
CONFIG_KEYS = {
    "layout_version", "flash_size", "sector_size", "erase_size",
    "page_size", "alignment", "update_policies", "partitions",
    "reserved_regions",
}
PARTITION_KEYS = {"name", "offset", "size", "update_policy"}
RESERVED_KEYS = {"name", "offset", "size"}


class LayoutError(ValueError):
    """Raised when layout data or an assigned image is invalid."""


@dataclass(frozen=True)
class Region:
    name: str
    offset: int
    size: int
    update_policy: str | None = None
    reserved: bool = False

    @property
    def end(self) -> int:
        return self.offset + self.size


@dataclass(frozen=True)
class Layout:
    version: int
    flash_size: int
    sector_size: int
    erase_size: int
    page_size: int
    alignment: int
    update_policies: Mapping[str, int]
    partitions: tuple[Region, ...]
    reserved_regions: tuple[Region, ...]

    def partition(self, name: str) -> Region:
        for partition in self.partitions:
            if partition.name == name:
                return partition
        raise LayoutError(f"unknown partition for image: {name}")


def parse_int(value: object, field: str, *, allow_zero: bool = False) -> int:
    if isinstance(value, bool):
        raise LayoutError(f"{field} must be an integer, not a boolean")
    if isinstance(value, int):
        result = value
    elif isinstance(value, str):
        try:
            result = int(value, 0)
        except ValueError as exc:
            raise LayoutError(f"{field} has invalid integer value {value!r}") from exc
    else:
        raise LayoutError(f"{field} must be an integer or base-prefixed string")
    if result < 0 or (result == 0 and not allow_zero):
        requirement = "zero or greater" if allow_zero else "greater than zero"
        raise LayoutError(f"{field} must be {requirement}")
    return result


def _require_keys(data: Mapping[str, object], expected: set[str], context: str) -> None:
    missing = expected - data.keys()
    extra = data.keys() - expected
    if missing:
        raise LayoutError(f"{context} missing keys: {', '.join(sorted(missing))}")
    if extra:
        raise LayoutError(f"{context} has unknown keys: {', '.join(sorted(extra))}")


def _parse_name(value: object, field: str) -> str:
    if not isinstance(value, str) or NAME_RE.fullmatch(value) is None:
        raise LayoutError(f"{field} must match {NAME_RE.pattern}")
    return value


def _parse_regions(
    raw_regions: object,
    *,
    context: str,
    policies: Mapping[str, int],
    reserved: bool,
) -> tuple[Region, ...]:
    if not isinstance(raw_regions, list) or not raw_regions:
        raise LayoutError(f"{context} must be a non-empty array")

    regions: list[Region] = []
    expected_keys = RESERVED_KEYS if reserved else PARTITION_KEYS
    for index, raw_region in enumerate(raw_regions):
        item_context = f"{context}[{index}]"
        if not isinstance(raw_region, dict):
            raise LayoutError(f"{item_context} must be an object")
        _require_keys(raw_region, expected_keys, item_context)
        name = _parse_name(raw_region["name"], f"{item_context}.name")
        policy = None if reserved else raw_region["update_policy"]
        if policy is not None and (not isinstance(policy, str) or policy not in policies):
            raise LayoutError(f"{item_context}.update_policy is not declared")
        regions.append(Region(
            name=name,
            offset=parse_int(
                raw_region["offset"], f"{item_context}.offset", allow_zero=True
            ),
            size=parse_int(raw_region["size"], f"{item_context}.size"),
            update_policy=policy,
            reserved=reserved,
        ))
    return tuple(regions)


def _validate_geometry(layout: Layout) -> None:
    if layout.version != 1:
        raise LayoutError(f"unsupported layout_version: {layout.version}")
    if layout.flash_size % layout.alignment != 0:
        raise LayoutError("flash_size must be aligned to alignment")
    if layout.alignment % layout.erase_size != 0:
        raise LayoutError("alignment must be a multiple of erase_size")
    if layout.erase_size % layout.page_size != 0:
        raise LayoutError("erase_size must be a multiple of page_size")
    if layout.erase_size % layout.sector_size != 0:
        raise LayoutError("erase_size must be a multiple of sector_size")


def _validate_regions(layout: Layout) -> None:
    all_regions = sorted(
        (*layout.partitions, *layout.reserved_regions), key=lambda region: region.offset
    )
    names: set[str] = set()
    expected_offset = 0
    for region in all_regions:
        if region.name in names:
            raise LayoutError(f"duplicate region name: {region.name}")
        names.add(region.name)
        if region.offset % layout.alignment != 0:
            raise LayoutError(
                f"region {region.name} offset 0x{region.offset:x} is not aligned "
                f"to 0x{layout.alignment:x}"
            )
        if region.size % layout.alignment != 0:
            raise LayoutError(
                f"region {region.name} size 0x{region.size:x} is not aligned "
                f"to 0x{layout.alignment:x}"
            )
        if region.offset < expected_offset:
            raise LayoutError(f"region {region.name} overlaps the previous region")
        if region.offset > expected_offset:
            raise LayoutError(
                f"unassigned gap 0x{expected_offset:x}-0x{region.offset - 1:x}; "
                "declare it as a reserved region"
            )
        if region.end > layout.flash_size:
            raise LayoutError(f"region {region.name} exceeds flash_size")
        expected_offset = region.end
    if expected_offset != layout.flash_size:
        raise LayoutError(
            f"layout ends at 0x{expected_offset:x}, expected flash_size "
            f"0x{layout.flash_size:x}"
        )


def load_layout(path: str | Path) -> Layout:
    config_path = Path(path)
    try:
        raw = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LayoutError(f"cannot read {config_path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise LayoutError("layout root must be an object")
    _require_keys(raw, CONFIG_KEYS, "layout")

    raw_policies = raw["update_policies"]
    if not isinstance(raw_policies, dict) or not raw_policies:
        raise LayoutError("update_policies must be a non-empty object")
    policies: dict[str, int] = {}
    policy_values: set[int] = set()
    for raw_name, raw_value in raw_policies.items():
        name = _parse_name(raw_name, "update policy name")
        value = parse_int(raw_value, f"update_policies.{name}")
        if value in policy_values:
            raise LayoutError(f"duplicate update policy value: {value}")
        policies[name] = value
        policy_values.add(value)

    partitions = _parse_regions(
        raw["partitions"], context="partitions", policies=policies, reserved=False
    )
    reserved_regions = _parse_regions(
        raw["reserved_regions"], context="reserved_regions",
        policies=policies, reserved=True,
    )
    layout = Layout(
        version=parse_int(raw["layout_version"], "layout_version"),
        flash_size=parse_int(raw["flash_size"], "flash_size"),
        sector_size=parse_int(raw["sector_size"], "sector_size"),
        erase_size=parse_int(raw["erase_size"], "erase_size"),
        page_size=parse_int(raw["page_size"], "page_size"),
        alignment=parse_int(raw["alignment"], "alignment"),
        update_policies=policies,
        partitions=partitions,
        reserved_regions=reserved_regions,
    )
    _validate_geometry(layout)
    _validate_regions(layout)
    return layout


def parse_image_assignments(values: Iterable[str]) -> dict[str, Path]:
    assignments: dict[str, Path] = {}
    for value in values:
        name, separator, raw_path = value.partition("=")
        if not separator or not name or not raw_path:
            raise LayoutError(f"image assignment must be PARTITION=PATH: {value!r}")
        if name in assignments:
            raise LayoutError(f"duplicate image assignment: {name}")
        assignments[name] = Path(raw_path)
    return assignments


def check_images(layout: Layout, assignments: Mapping[str, Path]) -> dict[str, int]:
    sizes: dict[str, int] = {}
    for name, path in assignments.items():
        partition = layout.partition(name)
        try:
            size = path.stat().st_size
        except OSError as exc:
            raise LayoutError(f"cannot stat image for {name}: {path}: {exc}") from exc
        if not path.is_file():
            raise LayoutError(f"image for {name} is not a regular file: {path}")
        if size > partition.size:
            raise LayoutError(
                f"image {path} for {name} is 0x{size:x} bytes, exceeds "
                f"partition size 0x{partition.size:x}"
            )
        sizes[name] = size
    return sizes


def _macro_name(name: str) -> str:
    return re.sub(r"[^A-Z0-9]", "_", name.upper())


def render_header(layout: Layout) -> str:
    lines = [
        "/* Auto-generated by tools/gpt/nor_layout.py. Do not edit. */",
        "#ifndef QUARD_NOR_LAYOUT_H",
        "#define QUARD_NOR_LAYOUT_H",
        "",
        f"#define QUARD_NOR_LAYOUT_VERSION {layout.version}U",
        f"#define QUARD_NOR_FLASH_SIZE 0x{layout.flash_size:08X}U",
        f"#define QUARD_NOR_SECTOR_SIZE 0x{layout.sector_size:08X}U",
        f"#define QUARD_NOR_ERASE_SIZE 0x{layout.erase_size:08X}U",
        f"#define QUARD_NOR_PAGE_SIZE 0x{layout.page_size:08X}U",
        f"#define QUARD_NOR_ALIGNMENT 0x{layout.alignment:08X}U",
        f"#define QUARD_NOR_PARTITION_COUNT {len(layout.partitions)}U",
        "",
    ]
    for name, value in sorted(layout.update_policies.items(), key=lambda item: item[1]):
        lines.append(f"#define QUARD_NOR_UPDATE_{_macro_name(name)} {value}U")
    lines.append("")
    for partition in layout.partitions:
        macro = _macro_name(partition.name)
        policy = _macro_name(partition.update_policy or "")
        lines.extend([
            f'#define QUARD_NOR_{macro}_NAME "{partition.name}"',
            f"#define QUARD_NOR_{macro}_OFFSET 0x{partition.offset:08X}U",
            f"#define QUARD_NOR_{macro}_SIZE 0x{partition.size:08X}U",
            f"#define QUARD_NOR_{macro}_LIMIT 0x{partition.end:08X}U",
            f"#define QUARD_NOR_{macro}_UPDATE_POLICY QUARD_NOR_UPDATE_{policy}",
            "",
        ])
    for region in layout.reserved_regions:
        macro = _macro_name(region.name)
        lines.extend([
            f"#define QUARD_NOR_{macro}_OFFSET 0x{region.offset:08X}U",
            f"#define QUARD_NOR_{macro}_SIZE 0x{region.size:08X}U",
            f"#define QUARD_NOR_{macro}_LIMIT 0x{region.end:08X}U",
            "",
        ])
    lines.extend(["#endif /* QUARD_NOR_LAYOUT_H */", ""])
    return "\n".join(lines)


def render_report(layout: Layout, image_sizes: Mapping[str, int]) -> str:
    lines = [
        "VisionFive 2 SPI NOR layout",
        f"layout_version: {layout.version}",
        f"flash_size: 0x{layout.flash_size:08x} ({layout.flash_size} bytes)",
        f"sector_size: 0x{layout.sector_size:x}",
        f"erase_size: 0x{layout.erase_size:x}",
        f"page_size: 0x{layout.page_size:x}",
        f"alignment: 0x{layout.alignment:x}",
        "",
        "name             offset      end         size        policy               image_size",
    ]
    for region in sorted(
        (*layout.partitions, *layout.reserved_regions), key=lambda item: item.offset
    ):
        policy = region.update_policy or "reserved"
        image_size = image_sizes.get(region.name)
        image_text = "-" if image_size is None else f"0x{image_size:x}"
        lines.append(
            f"{region.name:<16} 0x{region.offset:08x} 0x{region.end - 1:08x} "
            f"0x{region.size:08x} {policy:<20} {image_text}"
        )
    lines.append("")
    return "\n".join(lines)


def write_text(path: str | Path, content: str) -> None:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp")
    temporary.write_text(content, encoding="utf-8", newline="\n")
    temporary.replace(output)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        default=str(Path(__file__).resolve().parents[2] / "conf/visionfive2-nor-layout.json"),
        help="canonical NOR layout JSON",
    )
    parser.add_argument("--check", action="store_true", help="validate without requiring output")
    parser.add_argument("--report", help="write a normalized text layout report")
    parser.add_argument("--header", help="write an auto-generated C constants header")
    parser.add_argument(
        "--image", action="append", default=[], metavar="PARTITION=PATH",
        help="validate an image against its partition capacity",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        layout = load_layout(args.config)
        assignments = parse_image_assignments(args.image)
        image_sizes = check_images(layout, assignments)
        if args.header:
            write_text(args.header, render_header(layout))
        if args.report:
            write_text(args.report, render_report(layout, image_sizes))
        if not (args.check or args.header or args.report or args.image):
            raise LayoutError("select --check, --report, --header, or --image")
    except LayoutError as exc:
        print(f"NOR layout error: {exc}", file=sys.stderr)
        return 2
    print(
        f"NOR layout OK: {len(layout.partitions)} partitions, "
        f"flash_size=0x{layout.flash_size:x}"
    )
    for name, size in image_sizes.items():
        print(f"Image {name}: 0x{size:x} bytes (limit 0x{layout.partition(name).size:x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
