#!/usr/bin/env python3
"""
LocaLeShop Server Daemon
------------------------
A lightweight, self-hosted FastAPI server daemon that indexes extracted Wii U
WUP packages (.app, .h3, title.tmd, title.tik, title.cert) and serves them over
LAN to a paired Wii U homebrew client (.wuhb) with HTTP 200/206 Range streaming.
"""

from __future__ import annotations

import argparse
import logging
import os
import re
import struct
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Tuple

import uvicorn
from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import StreamingResponse

# ---------------------------------------------------------------------------
# Logging & Constants
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("localeshop")

TMD_TITLE_ID_OFFSET = 0x18C       # 8 bytes (unsigned 64-bit big-endian)
TMD_TITLE_VERSION_OFFSET = 0x1DC  # 2 bytes (unsigned 16-bit big-endian)
TMD_CONTENT_COUNT_OFFSET = 0x1DE  # 2 bytes (unsigned 16-bit big-endian)
TMD_MIN_HEADER_SIZE = 0x1E0       # Minimum bytes required to read header fields

DEFAULT_CHUNK_SIZE = 64 * 1024    # 64 KiB streaming chunk size for Wii U LAN


# ---------------------------------------------------------------------------
# Data Models
# ---------------------------------------------------------------------------

@dataclass
class TMDMetadata:
    """Parsed binary metadata from a Wii U title.tmd file."""
    title_id: str
    version: int
    content_count: int


@dataclass
class TitleEntry:
    """Indexed Wii U package entry."""
    title_id: str
    name: str
    version: int
    content_count: int
    total_size_bytes: int
    file_list: List[str]
    folder_path: Path

    def to_api_dict(self) -> dict:
        return {
            "title_id": self.title_id,
            "name": self.name,
            "version": self.version,
            "content_count": self.content_count,
            "total_size_bytes": self.total_size_bytes,
            "file_list": self.file_list,
        }


# ---------------------------------------------------------------------------
# 1. Binary TMD Parser
# ---------------------------------------------------------------------------

def parse_tmd(tmd_path: Path) -> TMDMetadata:
    """
    Parse a Wii U binary title.tmd file.

    Offsets read (big-endian):
      - 0x18C (8 bytes): Title ID, formatted as a 16-character hex string
      - 0x1DC (2 bytes, uint16): Title Version
      - 0x1DE (2 bytes, uint16): Content count
    """
    if not tmd_path.is_file():
        raise FileNotFoundError(f"TMD file not found: {tmd_path}")

    file_size = tmd_path.stat().st_size
    if file_size < TMD_MIN_HEADER_SIZE:
        raise ValueError(
            f"Invalid TMD file '{tmd_path}': size ({file_size} bytes) is smaller "
            f"than minimum required header size ({TMD_MIN_HEADER_SIZE} bytes)."
        )

    with tmd_path.open("rb") as fp:
        fp.seek(TMD_TITLE_ID_OFFSET)
        raw_title_id = fp.read(8)
        title_id = raw_title_id.hex().lower()

        fp.seek(TMD_TITLE_VERSION_OFFSET)
        version, content_count = struct.unpack(">HH", fp.read(4))

    return TMDMetadata(
        title_id=title_id,
        version=version,
        content_count=content_count,
    )


# ---------------------------------------------------------------------------
# Optional Title Alias Lookup (keys.txt / titles database)
# ---------------------------------------------------------------------------

def load_title_aliases(keys_file: Optional[Path]) -> Dict[str, str]:
    """
    Parse a Wii U keys.txt file (if present) to map 16-char hex Title IDs
    to human-readable title names.
    """
    aliases: Dict[str, str] = {}
    if not keys_file or not keys_file.is_file():
        return aliases

    # Matches lines starting with a 16-char hex Title ID followed by a key and comment/name
    pattern = re.compile(
        r"^\s*([0-9a-fA-F]{16})\s+[0-9a-fA-F]{32}\s*#\s*([^\t\r\n]+)(?:\t([A-Z]{3}))?(?:\t([^\t\r\n]+))?"
    )

    try:
        with keys_file.open("r", encoding="utf-8", errors="replace") as fp:
            for line in fp:
                match = pattern.match(line)
                if not match:
                    continue
                tid = match.group(1).lower()
                raw_name = match.group(2).strip()
                region = (match.group(3) or "").strip()
                category = (match.group(4) or "").strip()

                if not raw_name or "?" * 3 in raw_name:
                    continue

                display_name = raw_name
                suffix_parts = [p for p in (region, category) if p and p != "eShop/Application"]
                if suffix_parts:
                    display_name = f"{raw_name} ({', '.join(suffix_parts)})"
                elif region:
                    display_name = f"{raw_name} ({region})"

                aliases[tid] = display_name
        logger.info("Loaded %d human-readable title aliases from %s", len(aliases), keys_file)
    except OSError as exc:
        logger.warning("Failed to read aliases from %s: %s", keys_file, exc)

    return aliases


# ---------------------------------------------------------------------------
# 2. Library Scanner
# ---------------------------------------------------------------------------

class LibraryCatalog:
    """Scans and indexes staged Wii U WUP package directories containing title.tmd."""

    def __init__(self, library_root: Path, keys_file: Optional[Path] = None) -> None:
        self.library_root = library_root.resolve()
        self.keys_file = keys_file.resolve() if keys_file else None
        self.aliases: Dict[str, str] = load_title_aliases(self.keys_file)
        self._titles: Dict[str, TitleEntry] = {}

    def scan(self) -> List[TitleEntry]:
        """
        Walk the library directory, detect folders containing a `title.tmd`
        (case-insensitive check supported), and index their metadata and files.
        """
        if not self.library_root.exists():
            logger.warning("Library root '%s' does not exist; creating it.", self.library_root)
            self.library_root.mkdir(parents=True, exist_ok=True)

        discovered: Dict[str, TitleEntry] = {}

        for root, dirs, files in os.walk(self.library_root):
            # Sort for deterministic traversal
            dirs.sort()
            files.sort()

            tmd_filename = next((f for f in files if f.lower() == "title.tmd"), None)
            if not tmd_filename:
                continue

            folder_path = Path(root).resolve()
            tmd_path = folder_path / tmd_filename

            try:
                tmd_meta = parse_tmd(tmd_path)
            except Exception as exc:
                logger.warning("Skipping %s: failed to parse %s (%s)", folder_path, tmd_filename, exc)
                continue

            # Gather all files inside this package folder (including any subfiles)
            rel_files: List[str] = []
            total_size = 0
            for pkg_root, pkg_dirs, pkg_files in os.walk(folder_path):
                pkg_dirs.sort()
                pkg_files.sort()
                for f in pkg_files:
                    full_p = Path(pkg_root) / f
                    if not full_p.is_file():
                        continue
                    try:
                        total_size += full_p.stat().st_size
                        rel_files.append(full_p.relative_to(folder_path).as_posix())
                    except OSError as exc:
                        logger.warning("Could not stat file %s: %s", full_p, exc)

            # Prevent descending into nested subdirectories as separate packages if already indexed
            dirs.clear()

            # Resolve human-readable name:
            # 1. Optional name.txt / alias.txt inside package folder
            # 2. Alias from keys.txt if folder is named after raw hex Title ID
            # 3. Folder name (or Folder name + alias if distinct)
            name = self._resolve_name(folder_path, tmd_meta.title_id)

            entry = TitleEntry(
                title_id=tmd_meta.title_id,
                name=name,
                version=tmd_meta.version,
                content_count=tmd_meta.content_count,
                total_size_bytes=total_size,
                file_list=rel_files,
                folder_path=folder_path,
            )

            if entry.title_id in discovered:
                existing = discovered[entry.title_id]
                if entry.version >= existing.version:
                    logger.info(
                        "Replacing duplicate Title ID %s (v%d -> v%d) from %s",
                        entry.title_id,
                        existing.version,
                        entry.version,
                        folder_path,
                    )
                    discovered[entry.title_id] = entry
            else:
                discovered[entry.title_id] = entry
                logger.info(
                    "Indexed Title ID %s | Name: '%s' | v%d | Contents: %d | Size: %d bytes",
                    entry.title_id,
                    entry.name,
                    entry.version,
                    entry.content_count,
                    entry.total_size_bytes,
                )

        self._titles = discovered
        return list(self._titles.values())

    def _resolve_name(self, folder_path: Path, title_id: str) -> str:
        alias = self.aliases.get(title_id.lower())
        folder_name = folder_path.name

        # If folder name is just the hex title_id (or [title_id]), prefer human-readable alias
        if alias and folder_name.lower().strip("[]") == title_id.lower():
            return alias
        if alias and alias.lower() not in folder_name.lower():
            return alias
        return folder_name

    def list_titles(self) -> List[TitleEntry]:
        self.scan()
        return sorted(self._titles.values(), key=lambda t: (t.name.lower(), t.title_id))

    def get_title(self, title_id: str) -> Optional[TitleEntry]:
        normalized = title_id.lower().strip()
        if normalized not in self._titles:
            self.scan()
        return self._titles.get(normalized)


# ---------------------------------------------------------------------------
# 3. HTTP Range Parsing & Chunk Streaming Helpers
# ---------------------------------------------------------------------------

def parse_range_header(range_header: str, file_size: int) -> Tuple[int, int]:
    """
    Parse an HTTP Range header ('bytes=start-end', 'bytes=start-', or 'bytes=-suffix')
    and return an inclusive (start, end) byte tuple.
    Raises HTTPException(416) if the range is malformed or out of bounds.
    """
    if file_size == 0:
        raise HTTPException(
            status_code=416,
            detail="Requested Range Not Satisfiable (empty file)",
            headers={"Content-Range": "bytes */0"},
        )

    if not range_header or not range_header.strip().lower().startswith("bytes="):
        raise HTTPException(
            status_code=416,
            detail="Invalid Range header unit (only 'bytes' is supported)",
            headers={"Content-Range": f"bytes */{file_size}"},
        )

    range_spec = range_header.strip()[6:].strip()
    # Reject multi-part ranges to keep Wii U client streaming simple and predictable
    if "," in range_spec or "-" not in range_spec:
        raise HTTPException(
            status_code=416,
            detail="Multiple or malformed byte ranges are not supported",
            headers={"Content-Range": f"bytes */{file_size}"},
        )

    start_str, end_str = range_spec.split("-", 1)
    start_str = start_str.strip()
    end_str = end_str.strip()

    try:
        if start_str == "":
            # Suffix byte-range-spec: bytes=-500 (last 500 bytes)
            if not end_str:
                raise ValueError("Empty range bounds")
            suffix_length = int(end_str)
            if suffix_length <= 0:
                raise ValueError("Invalid suffix length")
            start = max(0, file_size - suffix_length)
            end = file_size - 1
        else:
            start = int(start_str)
            end = int(end_str) if end_str != "" else file_size - 1
    except ValueError as exc:
        raise HTTPException(
            status_code=416,
            detail="Malformed Range header values",
            headers={"Content-Range": f"bytes */{file_size}"},
        ) from exc

    if start < 0 or start >= file_size or end < start:
        raise HTTPException(
            status_code=416,
            detail=f"Requested Range {start}-{end} Not Satisfiable for size {file_size}",
            headers={"Content-Range": f"bytes */{file_size}"},
        )

    # Clamp end to file_size - 1 per RFC 9110 Section 14.1.2
    end = min(end, file_size - 1)
    return start, end


def stream_file_chunks(
    file_path: Path,
    start: int,
    end: int,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Iterator[bytes]:
    """Yield binary chunks from `start` to `end` (inclusive)."""
    bytes_remaining = end - start + 1
    with file_path.open("rb") as fp:
        fp.seek(start)
        while bytes_remaining > 0:
            read_size = min(chunk_size, bytes_remaining)
            data = fp.read(read_size)
            if not data:
                break
            bytes_remaining -= len(data)
            yield data


# ---------------------------------------------------------------------------
# 4. FastAPI Application Factory & Endpoints
# ---------------------------------------------------------------------------

def create_app(
    library_dir: Optional[Path] = None,
    keys_file: Optional[Path] = None,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> FastAPI:
    lib_path = library_dir or Path(os.environ.get("LOCALESHOP_LIBRARY_DIR", "./library"))
    default_keys = Path(os.environ.get("LOCALESHOP_KEYS_FILE", "./keys.txt"))
    resolved_keys = keys_file if keys_file is not None else (default_keys if default_keys.is_file() else None)

    catalog = LibraryCatalog(library_root=lib_path, keys_file=resolved_keys)
    catalog.scan()

    app = FastAPI(
        title="LocaLeShop Server Daemon",
        description="Self-hosted LAN WUP package indexer and byte-range streamer for Wii U homebrew (.wuhb).",
        version="1.0.0",
    )
    app.state.catalog = catalog
    app.state.chunk_size = chunk_size

    @app.get("/api/v1/titles")
    def get_titles() -> List[dict]:
        """Return a JSON array of all detected Wii U titles in the library."""
        entries = app.state.catalog.list_titles()
        return [entry.to_api_dict() for entry in entries]

    @app.api_route(
        "/api/v1/titles/{title_id}/files/{filename:path}",
        methods=["GET", "HEAD"],
    )
    def stream_title_file(title_id: str, filename: str, request: Request) -> Response:
        """
        Stream raw binary package content (.app, .h3, title.tmd, title.tik, title.cert)
        with full HTTP 200 OK and HTTP 206 Partial Content Range header support.
        """
        entry: Optional[TitleEntry] = app.state.catalog.get_title(title_id)
        if not entry:
            raise HTTPException(status_code=404, detail=f"Title ID '{title_id}' not found")

        # Resolve target file and guard against path traversal attacks
        try:
            candidate_path = (entry.folder_path / filename).resolve()
            candidate_path.relative_to(entry.folder_path)
        except ValueError as exc:
            raise HTTPException(status_code=403, detail="Path traversal is not permitted") from exc

        if not candidate_path.is_file():
            # Case-insensitive fallback for FAT32/WUP filename variations (e.g. 0000000A.app vs 0000000a.app)
            matched_rel = next(
                (rel for rel in entry.file_list if rel.lower() == filename.lower()),
                None,
            )
            if matched_rel:
                candidate_path = (entry.folder_path / matched_rel).resolve()

        if not candidate_path.is_file():
            raise HTTPException(
                status_code=404,
                detail=f"File '{filename}' not found in title '{title_id}'",
            )

        file_size = candidate_path.stat().st_size
        range_header = request.headers.get("range")

        common_headers = {
            "Accept-Ranges": "bytes",
            "Content-Disposition": f'attachment; filename="{candidate_path.name}"',
            "Cache-Control": "no-transform",
        }

        if range_header:
            start, end = parse_range_header(range_header, file_size)
            content_length = end - start + 1
            headers = {
                **common_headers,
                "Content-Range": f"bytes {start}-{end}/{file_size}",
                "Content-Length": str(content_length),
            }
            if request.method == "HEAD":
                return Response(
                    status_code=206,
                    headers=headers,
                    media_type="application/octet-stream",
                )
            return StreamingResponse(
                stream_file_chunks(candidate_path, start, end, chunk_size=app.state.chunk_size),
                status_code=206,
                headers=headers,
                media_type="application/octet-stream",
            )

        # Full file response (HTTP 200 OK)
        headers = {
            **common_headers,
            "Content-Length": str(file_size),
        }
        if request.method == "HEAD" or file_size == 0:
            return Response(
                status_code=200,
                headers=headers,
                media_type="application/octet-stream",
            )

        return StreamingResponse(
            stream_file_chunks(candidate_path, 0, file_size - 1, chunk_size=app.state.chunk_size),
            status_code=200,
            headers=headers,
            media_type="application/octet-stream",
        )

    return app


# Default module-level ASGI app instance for `uvicorn server:app`
app = create_app()


# ---------------------------------------------------------------------------
# CLI Entrypoint
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="LocaLeShop — Self-hosted Wii U WUP package server daemon."
    )
    parser.add_argument(
        "--library",
        "-l",
        type=Path,
        default=Path(os.environ.get("LOCALESHOP_LIBRARY_DIR", "./library")),
        help="Root directory containing staged Wii U title folders (default: ./library or $LOCALESHOP_LIBRARY_DIR).",
    )
    parser.add_argument(
        "--keys",
        "-k",
        type=Path,
        default=Path(os.environ.get("LOCALESHOP_KEYS_FILE", "./keys.txt")),
        help="Optional path to keys.txt for resolving human-readable title names (default: ./keys.txt).",
    )
    parser.add_argument(
        "--host",
        "-H",
        default=os.environ.get("LOCALESHOP_HOST", "0.0.0.0"),
        help="Host interface to bind (default: 0.0.0.0).",
    )
    parser.add_argument(
        "--port",
        "-p",
        type=int,
        default=int(os.environ.get("LOCALESHOP_PORT", "8080")),
        help="TCP port to listen on (default: 8080).",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=int(os.environ.get("LOCALESHOP_CHUNK_SIZE", str(DEFAULT_CHUNK_SIZE))),
        help=f"Streaming chunk size in bytes (default: {DEFAULT_CHUNK_SIZE}).",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    keys_path = args.keys if args.keys and args.keys.is_file() else None
    server_app = create_app(
        library_dir=args.library,
        keys_file=keys_path,
        chunk_size=args.chunk_size,
    )
    logger.info(
        "Starting LocaLeShop daemon on http://%s:%d (library: %s)",
        args.host,
        args.port,
        args.library.resolve(),
    )
    uvicorn.run(server_app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()