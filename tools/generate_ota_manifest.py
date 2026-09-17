#!/usr/bin/env python3
"""根据已构建固件生成边界明确的 OTA Manifest。"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import urlparse


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="生成 ESP32 OTA Manifest")
    parser.add_argument("--binary", required=True, type=Path, help="固件二进制路径")
    parser.add_argument("--output", required=True, type=Path, help="Manifest 输出路径")
    parser.add_argument("--product", required=True, help="产品/工程标识")
    parser.add_argument("--hardware", required=True, help="硬件兼容标识")
    parser.add_argument("--version", required=True, help="固件展示版本")
    parser.add_argument(
        "--release-sequence",
        required=True,
        type=int,
        help="必须单调递增的发布序号",
    )
    parser.add_argument("--image-url", required=True, help="设备下载固件的 URL")
    return parser.parse_args()


def validate_arguments(arguments: argparse.Namespace) -> None:
    if not arguments.binary.is_file():
        raise ValueError(f"固件不存在：{arguments.binary}")
    if arguments.binary.stat().st_size <= 0:
        raise ValueError("固件不能为空")
    if not 1 <= arguments.release_sequence <= 0x7FFFFFFF:
        raise ValueError("发布序号必须位于 1..2147483647")
    for name in ("product", "hardware", "version"):
        value = getattr(arguments, name)
        if not value or len(value.encode("utf-8")) > 32:
            raise ValueError(f"{name} 必须为 1..32 字节")

    parsed_url = urlparse(arguments.image_url)
    if parsed_url.scheme not in {"http", "https"} or not parsed_url.netloc:
        raise ValueError("image-url 必须是完整的 HTTP/HTTPS URL")
    if len(arguments.image_url.encode("utf-8")) > 256:
        raise ValueError("image-url 不能超过 256 字节")


def calculate_sha256(binary_path: Path) -> str:
    digest = hashlib.sha256()
    with binary_path.open("rb") as binary_file:
        for block in iter(lambda: binary_file.read(64 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    arguments = parse_arguments()
    validate_arguments(arguments)

    manifest = {
        "schema_version": 1,
        "product": arguments.product,
        "hardware": arguments.hardware,
        "version": arguments.version,
        "release_sequence": arguments.release_sequence,
        "image_url": arguments.image_url,
        "image_size": arguments.binary.stat().st_size,
        "sha256": calculate_sha256(arguments.binary),
    }

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = arguments.output.with_suffix(arguments.output.suffix + ".tmp")
    temporary_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary_path.replace(arguments.output)
    print(f"Manifest 已生成：{arguments.output}")
    print(f"固件大小：{manifest['image_size']} 字节")
    print(f"SHA-256：{manifest['sha256']}")


if __name__ == "__main__":
    main()
