#!/usr/bin/env python3
"""
LocaLeShop Server Daemon
-------------------------
A lightweight, self-hosted FastAPI server daemon that indexes extracted Wii U
WUP packages (.app, .h3, title.tmd, title.tik, title.cert) and serves them over
LAN to a paired Wii U homebrew client (.wuhb) with HTTP 200/206 Range streaming.
"""

import argparse
import csv
import json
import logging
import os
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import StreamingResponse
import uvicorn

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] [%(levelname)s] %(name)s: %(message)s"
)
logger = logging.getLogger("localeshop")

# ---------------------------------------------------------------------------
# Constants & TMD Parsing Offsets
# ---------------------------------------------------------------------------
TMD_TITLE_ID_OFFSET = 0x18C
TMD_TITLE_VERSION_OFFSET = 0x1DC
TMD_CONTENT_COUNT_OFFSET = 0x1DE
TMD_MIN_HEADER_SIZE = 0x200
DEFAULT_CHUNK_SIZE = 64 * 1024

# ---------------------------------------------------------------------------
# Global Title Database Loader (CSV, titledb.json & titles.json)
# ---------------------------------------------------------------------------
TITLE_DB: Dict[str, str] = {}

def load_title_databases() -> Dict[str, str]:
    db: Dict[str, str] = {}
    base_dir = Path(__file__).parent.resolve()
    search_dirs = [base_dir, base_dir.parent]

    # 1. Parse JSON databases (base fallback)
    for sdir in search_dirs:
        for json_name in ["titledb.json", "titles.json"]:
            p = sdir / json_name
            if p.is_file():
                try:
                    with p.open("r", encoding="utf-8") as f:
                        raw = json.load(f)
                        if isinstance(raw, dict):
                            for k, v in raw.items():
                                norm_key = k.lower().replace("-", "").replace("'", "").strip()
                                if isinstance(v, str):
                                    db[norm_key] = v
                                elif isinstance(v, dict) and "name" in v:
                                    db[norm_key] = v["name"]
                        elif isinstance(raw, list):
                            for item in raw:
                                if isinstance(item, dict) and "title_id" in item and "name" in item:
                                    norm_key = str(item["title_id"]).lower().replace("-", "").replace("'", "").strip()
                                    db[norm_key] = item["name"]
                    logger.info("Loaded JSON title entries from %s (Total now: %d)", p.name, len(db))
                except Exception as e:
                    logger.warning("Failed loading %s: %s", p, e)

    # 2. Parse CSV databases (semicolon-delimited WiiU-Titles-Library.csv with higher precedence)
    for sdir in search_dirs:
        for csv_path in sdir.glob("*.csv"):
            try:
                count = 0
                with csv_path.open("r", encoding="utf-8", errors="ignore") as f:
                    for line in f:
                        line = line.strip()
                        if not line or line.startswith("#"):
                            continue
                        delimiter = ";" if ";" in line else ","
                        parts = [p.replace("'", "").replace('"', "").strip() for p in line.split(delimiter)]
                        if len(parts) >= 2:
                            tid = parts[0].lower().replace("-", "").strip()
                            name = parts[1].strip()
                            if len(tid) == 16 and all(c in "0123456789abcdef" for c in tid) and name:
                                region = parts[6].upper() if len(parts) > 6 and parts[6] not in ("-", "") else None
                                db[tid] = f"{name} ({region})" if region else name
                                count += 1
                logger.info("Loaded %d CSV title entries from %s (Total now: %d)", count, csv_path.name, len(db))
            except Exception as e:
                logger.warning("Failed parsing CSV %s: %s", csv_path.name, e)

    return db

TITLE_DB = load_title_databases()

# ---------------------------------------------------------------------------
# TMD Metadata & Parser
# ---------------------------------------------------------------------------
@dataclass
class TMDMetadata:
    title_id: str
    version: int
    content_count: int

def parse_tmd(tmd_path: Path) -> TMDMetadata:
    if not tmd_path.is_file():
        raise FileNotFoundError(f"TMD file not found: {tmd_path}")

    file_size = tmd_path.stat().st_size
    if file_size < TMD_MIN_HEADER_SIZE:
        raise ValueError(f"Invalid TMD file '{tmd_path}': size {file_size} < {TMD_MIN_HEADER_SIZE}")

    with tmd_path.open("rb") as fp:
        fp.seek(TMD_TITLE_ID_OFFSET)
        raw_title_id = fp.read(8)
        title_id = raw_title_id.hex().lower()

        fp.seek(TMD_TITLE_VERSION_OFFSET)
        version = struct.unpack(">H", fp.read(2))[0]

        fp.seek(TMD_CONTENT_COUNT_OFFSET)
        content_count = struct.unpack(">H", fp.read(2))[0]

    return TMDMetadata(
        title_id=title_id,
        version=version,
        content_count=content_count
    )

# ---------------------------------------------------------------------------
# Catalog & Data Models
# ---------------------------------------------------------------------------
@dataclass
class TitleEntry:
    title_id: str
    name: str
    version: int
    content_count: int
    total_size_bytes: int
    file_list: List[str]
    folder_path: Path

    def to_api_dict(self) -> dict:
        tid_norm = self.title_id.lower().replace("-", "").strip()
        display_name = TITLE_DB.get(tid_norm, self.name)
        return {
            "title_id": self.title_id,
            "name": display_name,
            "version": self.version,
            "content_count": self.content_count,
            "total_size_bytes": self.total_size_bytes,
            "file_count": len(self.file_list),
            "files": self.file_list,
        }

class LibraryCatalog:
    def __init__(self, library_root: Path, keys_file: Optional[Path] = None) -> None:
        self.library_root = library_root.resolve()
        self.aliases: Dict[str, str] = {}
        if keys_file and keys_file.is_file():
            self._load_keys(keys_file)
        self._titles: Dict[str, TitleEntry] = {}

    def _load_keys(self, keys_file: Path) -> None:
        try:
            with keys_file.open("r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split("=", 1) if "=" in line else line.split(None, 1)
                    if len(parts) == 2:
                        k, v = parts[0].strip().lower().replace("-", ""), parts[1].strip()
                        if len(k) == 16:
                            self.aliases[k] = v
        except Exception as e:
            logger.warning("Could not read keys file %s: %s", keys_file, e)

    def _resolve_name(self, folder_path: Path, title_id: str) -> str:
        tid_norm = title_id.lower().replace("-", "").strip()

        # 1. Title DB (CSV / JSON)
        if tid_norm in TITLE_DB:
            return TITLE_DB[tid_norm]

        # 2. Local name.txt or alias.txt override
        for fname in ["name.txt", "alias.txt"]:
            fpath = folder_path / fname
            if fpath.is_file():
                try:
                    val = fpath.read_text(encoding="utf-8").strip()
                    if val:
                        return val
                except OSError:
                    pass

        # 3. keys.txt aliases
        if tid_norm in self.aliases:
            return self.aliases[tid_norm]

        # 4. Parent directory fallback if folder name is just the hex Title ID
        folder_name = folder_path.name
        if folder_name.lower().strip("[]") == tid_norm:
            parent_name = folder_path.parent.name
            if folder_path.parent != self.library_root and parent_name:
                return parent_name

        # 5. Immediate folder name
        return folder_name

    def scan(self) -> List[TitleEntry]:
        if not self.library_root.exists():
            logger.warning("Library root '%s' does not exist; creating it.", self.library_root)
            self.library_root.mkdir(parents=True, exist_ok=True)

        discovered: Dict[str, TitleEntry] = {}

        for root, dirs, files in os.walk(self.library_root):
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

            rel_files: List[str] = []
            total_size = 0
            for pkg_root, pkg_dirs, pkg_files in os.walk(folder_path):
                pkg_dirs.sort()
                pkg_files.sort()
                for f in pkg_files:
                    full_p = Path(pkg_root) / f
                    if full_p.is_symlink() and not full_p.exists():
                        continue
                    try:
                        total_size += full_p.stat().st_size
                        rel_files.append(full_p.relative_to(folder_path).as_posix())
                    except OSError as exc:
                        logger.warning("Could not stat file %s: %s", full_p, exc)

            # Prevent descending into nested subdirectories of this package
            dirs.clear()

            resolved_name = self._resolve_name(folder_path, tmd_meta.title_id)
            entry = TitleEntry(
                title_id=tmd_meta.title_id,
                name=resolved_name,
                version=tmd_meta.version,
                content_count=tmd_meta.content_count,
                total_size_bytes=total_size,
                file_list=rel_files,
                folder_path=folder_path,
            )

            if entry.title_id in discovered:
                existing = discovered[entry.title_id]
                if entry.version >= existing.version:
                    discovered[entry.title_id] = entry
            else:
                discovered[entry.title_id] = entry

        self._titles = discovered
        logger.info("Scan complete: indexed %d title(s)", len(self._titles))
        return list(self._titles.values())

    def get_title(self, title_id: str) -> Optional[TitleEntry]:
        return self._titles.get(title_id.lower().replace("-", "").strip())

# ---------------------------------------------------------------------------
# Range Streaming
# ---------------------------------------------------------------------------
def stream_file_chunks(
    file_path: Path,
    start: int,
    end: int,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Iterator[bytes]:
    bytes_remaining = end - start + 1
    with file_path.open("rb") as fp:
        fp.seek(start)
        while bytes_remaining > 0:
            read_size = min(chunk_size, bytes_remaining)
            chunk = fp.read(read_size)
            if not chunk:
                break
            bytes_remaining -= len(chunk)
            yield chunk

# ---------------------------------------------------------------------------
# Application Factory
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

    app = FastAPI(title="LocaLeShop Server", version="1.0.0")
    app.state.catalog = catalog
    app.state.chunk_size = chunk_size

    @app.get("/api/v1/titles")
    def list_titles():
        entries = catalog.scan()
        return [entry.to_api_dict() for entry in entries]

    @app.api_route(
        "/api/v1/titles/{title_id}/files/{filename:path}",
        methods=["GET", "HEAD"],
    )
    def stream_title_file(title_id: str, filename: str, request: Request) -> Response:
        entry = catalog.get_title(title_id)
        if not entry:
            raise HTTPException(status_code=404, detail=f"Title ID '{title_id}' not found")

        try:
            candidate_path = (entry.folder_path / filename).resolve()
            candidate_path.relative_to(entry.folder_path)
        except ValueError as exc:
            raise HTTPException(status_code=403, detail="Path traversal is not permitted") from exc

        if not candidate_path.is_file():
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
            m = re.match(r"^bytes=(\d+)-(\d*)$", range_header.strip())
            if not m:
                raise HTTPException(status_code=416, detail="Invalid Range header format")

            start = int(m.group(1))
            end = int(m.group(2)) if m.group(2) else file_size - 1

            if start >= file_size or end >= file_size or start > end:
                raise HTTPException(
                    status_code=416,
                    detail="Requested Range Not Satisfiable",
                    headers={"Content-Range": f"bytes */{file_size}"},
                )

            length = end - start + 1
            headers = {
                **common_headers,
                "Content-Range": f"bytes {start}-{end}/{file_size}",
                "Content-Length": str(length),
            }

            if request.method == "HEAD":
                return Response(status_code=206, headers=headers, media_type="application/octet-stream")

            return StreamingResponse(
                stream_file_chunks(candidate_path, start, end, chunk_size=app.state.chunk_size),
                status_code=206,
                headers=headers,
                media_type="application/octet-stream",
            )

        headers = {
            **common_headers,
            "Content-Length": str(file_size),
        }

        if request.method == "HEAD":
            return Response(status_code=200, headers=headers, media_type="application/octet-stream")

        return StreamingResponse(
            stream_file_chunks(candidate_path, 0, file_size - 1, chunk_size=app.state.chunk_size),
            status_code=200,
            headers=headers,
            media_type="application/octet-stream",
        )

    return app

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LocaLeShop LAN WUP Catalog & Streamer")
    parser.add_argument(
        "--library",
        "-l",
        type=Path,
        default=Path(os.environ.get("LOCALESHOP_LIBRARY_DIR", "./library")),
        help="Root directory containing staged Wii U title folders.",
    )
    parser.add_argument(
        "--keys",
        "-k",
        type=Path,
        default=Path(os.environ.get("LOCALESHOP_KEYS_FILE", "./keys.txt")),
        help="Optional path to keys.txt.",
    )
    parser.add_argument(
        "--host",
        type=str,
        default="0.0.0.0",
        help="Host address to bind to.",
    )
    parser.add_argument(
        "--port",
        "-p",
        type=int,
        default=8000,
        help="Port to listen on.",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK_SIZE,
        help="Streaming chunk size in bytes.",
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
    logger.info("Starting LocaLeShop server on %s:%d", args.host, args.port)
    uvicorn.run(server_app, host=args.host, port=args.port, log_level="info")

if __name__ == "__main__":
    main()
