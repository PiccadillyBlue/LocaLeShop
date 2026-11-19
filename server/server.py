#!/usr/bin/env python3
"""
LocaLeShop Server Daemon
------------------------
A self-hosted FastAPI server daemon that indexes Wii U titles in:
  1. Compressed Wii U Disc images (.wux) paired with a .key file
  2. Raw Wii U Disc images (.wud or split game_part1.wud) paired with a .key file
  3. Extracted Wii U WUP packages (.app, .h3, title.tmd, title.tik, title.cert)

Streams all WUP installation files over LAN to a paired Wii U homebrew client (.wuhb)
with HTTP 200/206 Byte-Range streaming, official NUS CDN (/ccs/download/...) route
compatibility, CSV metadata enrichment, and UDP LAN auto-discovery.
"""

from __future__ import annotations

import argparse
import array
import asyncio
from contextlib import asynccontextmanager
import hashlib
import json
import logging
import os
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Tuple, Union

import uvicorn
from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import StreamingResponse

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    def aes_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
        cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
        decryptor = cipher.decryptor()
        return decryptor.update(data) + decryptor.finalize()

    def aes_cbc_encrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
        cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
        encryptor = cipher.encryptor()
        return encryptor.update(data) + encryptor.finalize()

except ImportError:
    from Cryptodome.Cipher import AES  # type: ignore

    def aes_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
        return AES.new(key, AES.MODE_CBC, iv=iv).decrypt(data)

    def aes_cbc_encrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
        return AES.new(key, AES.MODE_CBC, iv=iv).encrypt(data)


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
TMD_CONTENTS_OFFSET = 0xB04       # Offset of TMD_CONTENT array (48 bytes each)
TMD_CONTENT_ENTRY_SIZE = 0x30     # 48 bytes per content record
TMD_MIN_HEADER_SIZE = 0x1E0       # Minimum bytes required to read header fields

WUX_HEADER_MAGIC_0 = b"WUX0"
WUX_HEADER_MAGIC_1 = 0x1099D02E
WUX_HEADER_SIZE = 0x20

WUD_SECTOR_SIZE = 0x8000          # 32 KiB Wii U optical disc sector size
WUD_PART_TABLE_OFFSET = 0x18000   # Encrypted partition table offset in WUD
WUD_FST_MAGIC = 0xCCA6E67B        # Big-endian magic of decrypted partition table
WUD_SPLIT_PART_SIZE = 0x80000000  # 2 GiB per game_partX.wud chunk

# Standard retail Wii U Common Key (verified against SHA-1 6a0511622b47f0a342c01f0218c2c53f53175721)
DEFAULT_WIIU_COMMON_KEY = bytes.fromhex("d7b00402659ba2abd2cb0db27fa2b656")
KEYGEN_SECRET = bytes([0xFD, 0x04, 0x01, 0x05, 0x06, 0x0B, 0x11, 0x1C, 0x2D, 0x49])

DEFAULT_CHUNK_SIZE = 64 * 1024    # 64 KiB streaming chunk size for Wii U LAN
DISCOVERY_REQUEST_MAGIC = b"LOCALESHOP_DISCOVER_V1"
DISCOVERY_RESPONSE_MAGIC = "LOCALESHOP_SERVER_V1"


def align_forward(value: int, alignment: int = 16) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def category_from_title_id(title_id: str) -> str:
    tid = title_id.lower().strip()
    if len(tid) < 8:
        return "Unknown"
    high = tid[:8]
    if high == "00050000":
        return "Game"
    if high == "0005000e":
        return "Update"
    if high == "0005000c":
        return "DLC"
    if high == "00050002":
        return "Demo"
    if high in ("00050010", "0005001b", "00050030"):
        return "System"
    return "Other"


# ---------------------------------------------------------------------------
# Virtual Disc Readers (.wux, .wud, split wudump)
# ---------------------------------------------------------------------------

class DiscImageReader:
    """Abstract random-access reader over a Wii U optical disc image (.wux or .wud)."""

    uncompressed_size: int = 0
    source_format: str = "wud"

    def read_at(self, virtual_offset: int, size: int) -> bytes:
        raise NotImplementedError

    def iter_range(
        self,
        virtual_offset: int,
        start: int,
        end: int,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
    ) -> Iterator[bytes]:
        """Yield chunks for byte range [start, end] (inclusive) within a virtual slice."""
        cur = virtual_offset + start
        remaining = end - start + 1
        while remaining > 0:
            step = min(chunk_size, remaining)
            data = self.read_at(cur, step)
            if not data:
                break
            cur += len(data)
            remaining -= len(data)
            yield data


class WudImageReader(DiscImageReader):
    """Direct reader for raw uncompressed .wud disc images."""

    def __init__(self, wud_path: Path) -> None:
        self.wud_path = wud_path.resolve()
        self.uncompressed_size = self.wud_path.stat().st_size
        self.source_format = "wud"

    def read_at(self, virtual_offset: int, size: int) -> bytes:
        if size <= 0 or virtual_offset < 0 or virtual_offset >= self.uncompressed_size:
            return b""
        with self.wud_path.open("rb") as fp:
            fp.seek(virtual_offset)
            return fp.read(min(size, self.uncompressed_size - virtual_offset))

    def iter_range(
        self,
        virtual_offset: int,
        start: int,
        end: int,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
    ) -> Iterator[bytes]:
        abs_start = virtual_offset + start
        bytes_remaining = end - start + 1
        with self.wud_path.open("rb") as fp:
            fp.seek(abs_start)
            while bytes_remaining > 0:
                read_size = min(chunk_size, bytes_remaining)
                data = fp.read(read_size)
                if not data:
                    break
                bytes_remaining -= len(data)
                yield data


class SplitWudImageReader(DiscImageReader):
    """Reader for multi-file wudump folders (game_part1.wud .. game_part12.wud)."""

    def __init__(self, part1_path: Path) -> None:
        self.folder = part1_path.parent.resolve()
        self.parts: List[Path] = []
        idx = 1
        total = 0
        while True:
            candidate = self.folder / f"game_part{idx}.wud"
            if not candidate.is_file():
                break
            self.parts.append(candidate)
            total += candidate.stat().st_size
            idx += 1
        if not self.parts:
            raise FileNotFoundError(f"No game_partX.wud files found in {self.folder}")
        self.uncompressed_size = total
        self.source_format = "wud"

    def read_at(self, virtual_offset: int, size: int) -> bytes:
        if size <= 0 or virtual_offset < 0 or virtual_offset >= self.uncompressed_size:
            return b""
        out = bytearray()
        cur = virtual_offset
        rem = min(size, self.uncompressed_size - virtual_offset)
        while rem > 0:
            part_idx = cur // WUD_SPLIT_PART_SIZE
            if part_idx >= len(self.parts):
                break
            in_part_off = cur % WUD_SPLIT_PART_SIZE
            to_read = min(rem, WUD_SPLIT_PART_SIZE - in_part_off)
            with self.parts[part_idx].open("rb") as fp:
                fp.seek(in_part_off)
                chunk = fp.read(to_read)
            if not chunk:
                break
            out.extend(chunk)
            cur += len(chunk)
            rem -= len(chunk)
        return bytes(out)


class WuxImageReader(DiscImageReader):
    """
    On-the-fly random-access reader for compressed Wii U Disc images (.wux).
    Parses the WUX0 header and 32-bit physical sector lookup table once at index time,
    allowing arbitrary virtual disc byte ranges to be streamed without extraction.
    """

    def __init__(self, wux_path: Path) -> None:
        self.wux_path = wux_path.resolve()
        self.source_format = "wux"
        file_size = self.wux_path.stat().st_size
        if file_size < WUX_HEADER_SIZE:
            raise ValueError(f"WUX file too small ({file_size} bytes): {self.wux_path}")

        with self.wux_path.open("rb") as fp:
            hdr = fp.read(WUX_HEADER_SIZE)
            magic0, magic1, sector_size, _flags, uncompressed_size, _reserved = struct.unpack(
                "<4sIIIQQ", hdr
            )
            if magic0 != WUX_HEADER_MAGIC_0 or magic1 != WUX_HEADER_MAGIC_1:
                raise ValueError(
                    f"Invalid WUX header magic in {self.wux_path}: {magic0!r} / 0x{magic1:08X}"
                )
            if sector_size < 512 or (sector_size & (sector_size - 1)) != 0:
                raise ValueError(f"Invalid WUX sector size ({sector_size}) in {self.wux_path}")

            self.sector_size = sector_size
            self.uncompressed_size = uncompressed_size
            self.sector_count = (uncompressed_size + sector_size - 1) // sector_size

            table_bytes_len = self.sector_count * 4
            raw_table = fp.read(table_bytes_len)
            if len(raw_table) < table_bytes_len:
                raise ValueError(
                    f"Truncated WUX sector index table in {self.wux_path} "
                    f"({len(raw_table)} < {table_bytes_len} bytes)"
                )

            self.index_table = array.array("I")
            self.index_table.frombytes(raw_table)
            if sys.byteorder != "little":
                self.index_table.byteswap()

            table_end = WUX_HEADER_SIZE + table_bytes_len
            self.data_offset = ((table_end + sector_size - 1) // sector_size) * sector_size

    def read_at(self, virtual_offset: int, size: int) -> bytes:
        if size <= 0 or virtual_offset < 0 or virtual_offset >= self.uncompressed_size:
            return b""
        rem = min(size, self.uncompressed_size - virtual_offset)
        out = bytearray()
        cur = virtual_offset
        sec_size = self.sector_size
        idx_tbl = self.index_table
        num_sec = self.sector_count
        data_off = self.data_offset

        with self.wux_path.open("rb") as fp:
            while rem > 0:
                logical_sec = cur // sec_size
                if logical_sec >= num_sec:
                    break
                in_sec_off = cur % sec_size
                phys_sec = idx_tbl[logical_sec]

                # Coalesce contiguous physical sectors for high-throughput reads
                avail_in_first = sec_size - in_sec_off
                coalesced_bytes = min(rem, avail_in_first)
                next_log = logical_sec + 1
                next_phys = phys_sec + 1
                while coalesced_bytes < rem and next_log < num_sec and idx_tbl[next_log] == next_phys:
                    add_bytes = min(rem - coalesced_bytes, sec_size)
                    coalesced_bytes += add_bytes
                    if add_bytes < sec_size:
                        break
                    next_log += 1
                    next_phys += 1

                phys_offset = data_off + (phys_sec * sec_size) + in_sec_off
                fp.seek(phys_offset)
                chunk = fp.read(coalesced_bytes)
                if not chunk:
                    break
                out.extend(chunk)
                cur += len(chunk)
                rem -= len(chunk)

        return bytes(out)

    def iter_range(
        self,
        virtual_offset: int,
        start: int,
        end: int,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
    ) -> Iterator[bytes]:
        cur = virtual_offset + start
        rem = min(end - start + 1, max(0, self.uncompressed_size - cur))
        sec_size = self.sector_size
        idx_tbl = self.index_table
        num_sec = self.sector_count
        data_off = self.data_offset

        with self.wux_path.open("rb") as fp:
            while rem > 0:
                max_step = min(chunk_size, rem)
                logical_sec = cur // sec_size
                if logical_sec >= num_sec:
                    break
                in_sec_off = cur % sec_size
                phys_sec = idx_tbl[logical_sec]

                avail_in_first = sec_size - in_sec_off
                coalesced_bytes = min(max_step, avail_in_first)
                next_log = logical_sec + 1
                next_phys = phys_sec + 1
                while (
                    coalesced_bytes < max_step
                    and next_log < num_sec
                    and idx_tbl[next_log] == next_phys
                ):
                    add_bytes = min(max_step - coalesced_bytes, sec_size)
                    coalesced_bytes += add_bytes
                    if add_bytes < sec_size:
                        break
                    next_log += 1
                    next_phys += 1

                phys_offset = data_off + (phys_sec * sec_size) + in_sec_off
                fp.seek(phys_offset)
                data = fp.read(coalesced_bytes)
                if not data:
                    break
                cur += len(data)
                rem -= len(data)
                yield data


# ---------------------------------------------------------------------------
# Data Models & Virtual File Sources
# ---------------------------------------------------------------------------

@dataclass
class TMDContentRecord:
    """Single content descriptor from a Wii U title.tmd."""
    cid: int
    index: int
    content_type: int
    size: int


@dataclass
class TMDMetadata:
    """Parsed binary metadata from a Wii U title.tmd file."""
    title_id: str
    version: int
    content_count: int
    contents: List[TMDContentRecord] = field(default_factory=list)


@dataclass
class VirtualFileSource:
    """
    Represents a streamable package file backed by either:
      - A physical file on disk (`file_path`)
      - In-memory bytes (`memory_data`, e.g., decrypted TMD/TIK/CERT or .h3 hashes)
      - A virtual slice inside a .wux/.wud disc image (`disc_reader`, `disc_offset`, `size`)
    """
    name: str
    size: int
    file_path: Optional[Path] = None
    memory_data: Optional[bytes] = None
    disc_reader: Optional[DiscImageReader] = None
    disc_offset: int = 0

    def iter_bytes(
        self,
        start: int,
        end: int,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
    ) -> Iterator[bytes]:
        if self.memory_data is not None:
            view = memoryview(self.memory_data)[start : end + 1]
            for offset in range(0, len(view), chunk_size):
                yield bytes(view[offset : offset + chunk_size])
            return

        if self.disc_reader is not None:
            yield from self.disc_reader.iter_range(
                self.disc_offset,
                start,
                end,
                chunk_size=chunk_size,
            )
            return

        if self.file_path is not None:
            yield from stream_file_chunks(
                self.file_path,
                start,
                end,
                chunk_size=chunk_size,
            )
            return

        raise FileNotFoundError(f"VirtualFileSource '{self.name}' has no backing store")


@dataclass
class CSVTitleInfo:
    """Metadata row parsed from WiiU-Titles-Library.csv."""
    title_id: str
    name: str
    product_code: str = ""
    company_code: str = ""
    region: str = "ALL"
    versions: str = ""
    icon_id: str = ""


@dataclass
class TitleEntry:
    """Indexed Wii U package entry (from WUX, WUD, or extracted WUP folder)."""
    title_id: str
    name: str
    version: int
    content_count: int
    total_size_bytes: int
    file_list: List[str]
    folder_path: Path
    region: str = "ALL"
    category: str = "Game"
    product_code: str = ""
    source_format: str = "wup"
    virtual_files: Dict[str, VirtualFileSource] = field(default_factory=dict)

    def to_api_dict(self) -> dict:
        return {
            "title_id": self.title_id,
            "name": self.name,
            "version": self.version,
            "content_count": self.content_count,
            "total_size_bytes": self.total_size_bytes,
            "file_count": len(self.file_list),
            "file_list": self.file_list,
            "files": self.file_list,
            "region": self.region,
            "category": self.category,
            "product_code": self.product_code,
            "source_format": self.source_format,
        }


# ---------------------------------------------------------------------------
# 1. Binary TMD Parser & Fake Ticket/Cert Synthesizer
# ---------------------------------------------------------------------------

def parse_tmd_bytes(raw_tmd: bytes, source_label: str = "<memory>") -> TMDMetadata:
    """Parse a Wii U binary title.tmd buffer into TMDMetadata."""
    if len(raw_tmd) < TMD_MIN_HEADER_SIZE:
        raise ValueError(
            f"Invalid TMD '{source_label}': size ({len(raw_tmd)} bytes) is smaller "
            f"than minimum required header size ({TMD_MIN_HEADER_SIZE} bytes)."
        )

    title_id = raw_tmd[TMD_TITLE_ID_OFFSET : TMD_TITLE_ID_OFFSET + 8].hex().lower()
    version, content_count = struct.unpack_from(">HH", raw_tmd, TMD_TITLE_VERSION_OFFSET)

    contents: List[TMDContentRecord] = []
    if len(raw_tmd) >= TMD_CONTENTS_OFFSET + content_count * TMD_CONTENT_ENTRY_SIZE:
        for idx in range(content_count):
            off = TMD_CONTENTS_OFFSET + idx * TMD_CONTENT_ENTRY_SIZE
            cid, c_index, c_type, c_size = struct.unpack_from(">IHHQ", raw_tmd, off)
            contents.append(
                TMDContentRecord(
                    cid=cid,
                    index=c_index,
                    content_type=c_type,
                    size=c_size,
                )
            )

    return TMDMetadata(
        title_id=title_id,
        version=version,
        content_count=content_count,
        contents=contents,
    )


def parse_tmd(tmd_path: Path) -> TMDMetadata:
    """
    Parse a Wii U binary title.tmd file from disk.

    Offsets read (big-endian):
      - 0x18C (8 bytes): Title ID, formatted as a 16-character hex string
      - 0x1DC (2 bytes, uint16): Title Version
      - 0x1DE (2 bytes, uint16): Content count
    """
    if not tmd_path.is_file():
        raise FileNotFoundError(f"TMD file not found: {tmd_path}")
    return parse_tmd_bytes(tmd_path.read_bytes(), source_label=str(tmd_path))


def derive_nusspli_encrypted_title_key(
    title_id_hex: str,
    common_key: bytes = DEFAULT_WIIU_COMMON_KEY,
    password: str = "mypass",
) -> bytes:
    """
    Derive the encrypted title key for a Wii U Title ID using NUSspli's PBKDF2
    keygen algorithm (src/keygen.c) and encrypt it with the Wii U Common Key.
    """
    tid_bytes = bytes.fromhex(title_id_hex)
    tid_high = struct.unpack(">I", tid_bytes[:4])[0]
    if tid_high == 0x00000007:  # vWii IOS
        part = tid_bytes[3:8]
    else:
        part = tid_bytes[1:8]

    salt = hashlib.md5(KEYGEN_SECRET + part).digest()
    raw_title_key = hashlib.pbkdf2_hmac(
        "sha1",
        password.encode("ascii"),
        salt,
        20,
        dklen=16,
    )
    iv = tid_bytes + (b"\x00" * 8)
    return aes_cbc_encrypt(common_key, iv, raw_title_key)


def synthesize_fake_ticket(
    tmd_meta: TMDMetadata,
    encrypted_title_key: Optional[bytes] = None,
    common_key: bytes = DEFAULT_WIIU_COMMON_KEY,
) -> bytes:
    """
    Generate a Wii U fake ticket (title.tik) matching NUSspli's generateTik() layout
    (sizeof(TICKET) == 0x2B8, plus 0x98 section header for DLC titles).
    """
    tid_bytes = bytes.fromhex(tmd_meta.title_id)
    is_dlc = tmd_meta.title_id.startswith("0005000c")

    if encrypted_title_key is None or len(encrypted_title_key) != 16:
        encrypted_title_key = derive_nusspli_encrypted_title_key(
            tmd_meta.title_id,
            common_key=common_key,
        )

    buf = bytearray(0x2B8)
    # NUS_HEADER (0x000..0x140)
    struct.pack_into(">I", buf, 0x000, 0x00010004)
    buf[0x004:0x00E] = bytes(range(10))
    buf[0x010:0x017] = b"NUSspli"
    buf[0x020:0x025] = b"1.159"
    buf[0x030:0x036] = b"Ticket"
    buf[0x0EF] = 0x01

    issuer = b"Root-CA00000003-XS0000000c"
    buf[0x140 : 0x140 + len(issuer)] = issuer
    buf[0x1BC] = 0x01  # ticket version
    buf[0x1BF:0x1CF] = encrypted_title_key
    struct.pack_into(">Q", buf, 0x1D0, 0x0005000012345678)
    buf[0x1DC:0x1E4] = tid_bytes
    struct.pack_into(">H", buf, 0x1E6, tmd_meta.version & 0xFFFF)
    struct.pack_into(">H", buf, 0x1F2, 0xFFFF)
    struct.pack_into(">H", buf, 0x2A4, 0x0001)

    if not is_dlc:
        struct.pack_into(">I", buf, 0x2A8, 0x00000014)
    else:
        struct.pack_into(">I", buf, 0x2A8, 0x000000AC)
        struct.pack_into(">I", buf, 0x2AC, 0x00000014)
        struct.pack_into(">H", buf, 0x2B0, 0x0001)
        struct.pack_into(">H", buf, 0x2B2, 0x0014)

        sec = bytearray(0x98)
        struct.pack_into(">IIIIH", sec, 0, 0x28, 0x01, 0x84, 0x84, 0x0003)
        for i in range(8):
            struct.pack_into(">I", sec, 0x18 + i * 4, 0xFFFFFFFF)
        buf.extend(sec)

    return bytes(buf)


def synthesize_fake_cert(raw_tmd: Optional[bytes] = None, raw_tik: Optional[bytes] = None) -> bytes:
    """
    Generate a 0xA00-byte title.cert chain matching NUSspli's generateCert() layout.
    Extracts CA3 and CP certs appended to title.tmd when available.
    """
    cetk = bytearray(0xA00)
    tmd_certs_copied = False
    if raw_tmd and len(raw_tmd) >= TMD_MIN_HEADER_SIZE:
        _, content_count = struct.unpack_from(">HH", raw_tmd, TMD_TITLE_VERSION_OFFSET)
        certs_off = TMD_CONTENTS_OFFSET + content_count * TMD_CONTENT_ENTRY_SIZE
        # TMD appends OTHER_PPKI_CERT (0x300, CP0000000b) followed by CA3_PPKI_CERT (0x400, CA00000003)
        if len(raw_tmd) >= certs_off + 0x700:
            cp_cert = raw_tmd[certs_off : certs_off + 0x300]
            ca_cert = raw_tmd[certs_off + 0x300 : certs_off + 0x700]
            cetk[0x000:0x400] = ca_cert
            cetk[0x400:0x700] = cp_cert
            tmd_certs_copied = True

    if not tmd_certs_copied:
        # Fallback synthetic CA3 + CP certificates
        struct.pack_into(">I", cetk, 0x000, 0x00010003)
        cetk[0x004:0x00E] = bytes(range(10))
        cetk[0x010:0x017] = b"NUSspli"
        cetk[0x020:0x025] = b"1.159"
        cetk[0x030:0x03B] = b"Certificate"
        cetk[0x0EF] = 0x01
        cetk[0x240:0x244] = b"Root"
        struct.pack_into(">I", cetk, 0x280, 1)
        cetk[0x284:0x28E] = b"CA00000003"
        struct.pack_into(">I", cetk, 0x3C8, 0x00010001)

        struct.pack_into(">I", cetk, 0x400, 0x00010004)
        cetk[0x540:0x54F] = b"Root-CA00000003"
        struct.pack_into(">I", cetk, 0x580, 1)
        cetk[0x584:0x58E] = b"CP0000000b"
        struct.pack_into(">I", cetk, 0x6C8, 0x00010001)

    # cert3 (XS0000000c at 0x700..0xA00)
    if raw_tik and len(raw_tik) >= 0x350 + 0x300:
        cetk[0x700:0xA00] = raw_tik[0x350 : 0x350 + 0x300]
    else:
        struct.pack_into(">I", cetk, 0x700, 0x00010004)
        cetk[0x840:0x84F] = b"Root-CA00000003"
        struct.pack_into(">I", cetk, 0x880, 1)
        cetk[0x884:0x88E] = b"XS0000000c"
        struct.pack_into(">I", cetk, 0x9C8, 0x00010001)

    return bytes(cetk)


# ---------------------------------------------------------------------------
# Key File & Metadata Database Loaders (WiiU-Titles-Library.csv & keys.txt)
# ---------------------------------------------------------------------------

def parse_key_bytes_from_file(key_path: Path) -> Optional[bytes]:
    """
    Read a 16-byte AES key from a .key file.
    Supports both:
      1. Raw 16-byte binary key files
      2. Text files containing a 32-character hexadecimal key string
    """
    if not key_path or not key_path.is_file():
        return None
    try:
        raw = key_path.read_bytes()
    except OSError as exc:
        logger.warning("Could not read key file %s: %s", key_path, exc)
        return None

    # 1. Exact 16-byte binary key
    if len(raw) == 16:
        return raw

    # 2. Check for 32-character hex key in text representation
    try:
        text = raw.decode("utf-8", errors="ignore")
        for line in text.splitlines():
            clean = line.split("#", 1)[0].strip()
            match = re.search(r"\b([0-9a-fA-F]{32})\b", clean)
            if match:
                return bytes.fromhex(match.group(1))
    except Exception:
        pass

    # 3. Fallback if binary key had trailing CR/LF stripped or padded
    stripped = raw.strip(b"\r\n\t ")
    if len(stripped) == 16:
        return stripped

    return None


def find_companion_key_file(disc_path: Path) -> Optional[Path]:
    """
    Find the companion .key file for a .wux or .wud image in its directory.
    Checks:
      1. <disc_stem>.key (e.g. /wiiu/game_title/game_title.key for game_title.wux)
      2. game.key or disc.key in the same folder
      3. Any other *.key file in the same folder (excluding common.key)
    """
    parent = disc_path.parent
    exact = parent / f"{disc_path.stem}.key"
    if exact.is_file():
        return exact

    for fixed_name in ("game.key", "disc.key", "title.key"):
        cand = parent / fixed_name
        if cand.is_file():
            return cand

    try:
        for f in sorted(parent.iterdir()):
            if f.is_file() and f.suffix.lower() == ".key" and f.name.lower() != "common.key":
                return f
    except OSError:
        pass
    return None


def load_title_aliases(keys_file: Optional[Path]) -> Dict[str, str]:
    """
    Parse a Wii U keys.txt file (if present) to map 16-char hex Title IDs
    to human-readable title names.
    """
    aliases: Dict[str, str] = {}
    keys_map, _ = load_keys_txt(keys_file)
    for tid, (_, display_name) in keys_map.items():
        if display_name:
            aliases[tid] = display_name
    return aliases


def load_keys_txt(keys_file: Optional[Path]) -> Tuple[Dict[str, Tuple[bytes, str]], Optional[bytes]]:
    """
    Parse a Wii U keys.txt file for:
      - Title/Disc keys mapped by 16-char Title ID -> (16-byte key, display_name)
      - Optional Wii U Common Key if listed in keys.txt
    """
    entries: Dict[str, Tuple[bytes, str]] = {}
    common_key: Optional[bytes] = None
    if not keys_file or not keys_file.is_file():
        return entries, common_key

    tid_pattern = re.compile(
        r"^\s*([0-9a-fA-F]{16})\s+([0-9a-fA-F]{32})(?:\s*#\s*([^\t\r\n]+)(?:\t([A-Z]{3}))?(?:\t([^\t\r\n]+))?)?"
    )
    ckey_pattern = re.compile(r"(?:common\s*key|wii\s*u\s*common\s*key)[^0-9a-fA-F]*([0-9a-fA-F]{32})", re.I)

    try:
        with keys_file.open("r", encoding="utf-8", errors="replace") as fp:
            for line in fp:
                ck_match = ckey_pattern.search(line)
                if ck_match and common_key is None:
                    common_key = bytes.fromhex(ck_match.group(1))

                match = tid_pattern.match(line)
                if not match:
                    continue
                tid = match.group(1).lower()
                key_bytes = bytes.fromhex(match.group(2))
                raw_name = (match.group(3) or "").strip()
                region = (match.group(4) or "").strip()
                category = (match.group(5) or "").strip()

                display_name = ""
                if raw_name and "???" not in raw_name:
                    display_name = raw_name
                    suffix_parts = [p for p in (region, category) if p and p != "eShop/Application"]
                    if suffix_parts:
                        display_name = f"{raw_name} ({', '.join(suffix_parts)})"
                    elif region:
                        display_name = f"{raw_name} ({region})"

                entries[tid] = (key_bytes, display_name)
        logger.info("Loaded %d title entry key(s) from %s", len(entries), keys_file)
    except OSError as exc:
        logger.warning("Failed to read keys from %s: %s", keys_file, exc)

    return entries, common_key


def load_json_title_databases(base_dirs: List[Path]) -> Dict[str, str]:
    """Load fallback title names from titledb.json and titles.json if present."""
    db: Dict[str, str] = {}
    for sdir in base_dirs:
        if not sdir or not sdir.is_dir():
            continue
        for json_name in ("titledb.json", "titles.json"):
            p = sdir / json_name
            if not p.is_file():
                continue
            try:
                raw = json.loads(p.read_text(encoding="utf-8", errors="replace"))
                if isinstance(raw, dict):
                    for k, v in raw.items():
                        norm_k = str(k).lower().replace("-", "").replace("'", "").strip()
                        if isinstance(v, str) and v.strip():
                            db[norm_k] = v.strip()
                        elif isinstance(v, dict) and isinstance(v.get("name"), str):
                            db[norm_k] = v["name"].strip()
                elif isinstance(raw, list):
                    for item in raw:
                        if isinstance(item, dict) and "title_id" in item and "name" in item:
                            norm_k = str(item["title_id"]).lower().replace("-", "").replace("'", "").strip()
                            db[norm_k] = str(item["name"]).strip()
                logger.info("Loaded JSON title entries from %s (total JSON aliases: %d)", p, len(db))
            except Exception as exc:
                logger.warning("Failed loading %s: %s", p, exc)
    return db


def load_csv_title_database(csv_path: Optional[Path]) -> Dict[str, CSVTitleInfo]:
    """
    Parse server/WiiU-Titles-Library.csv (semicolon-delimited) to map 16-char
    hex Title IDs to human-readable descriptions, regions, and product codes.
    """
    catalog: Dict[str, CSVTitleInfo] = {}
    if not csv_path or not csv_path.is_file():
        return catalog

    try:
        raw_text = csv_path.read_bytes().decode("utf-8", errors="replace")
        for line in raw_text.splitlines():
            line = line.strip()
            if not line or "Title Id;" in line:
                continue
            parts = [p.strip().strip("'").strip() for p in line.split(";")]
            if len(parts) < 2:
                continue
            tid = parts[0].lower()
            if len(tid) != 16 or not all(c in "0123456789abcdef" for c in tid):
                continue
            name = parts[1].strip()
            if not name or name == "-":
                continue
            product_code = parts[2].upper() if len(parts) > 2 and parts[2] != "-" else ""
            company_code = parts[3].upper() if len(parts) > 3 and parts[3] != "-" else ""
            versions = parts[5] if len(parts) > 5 and parts[5] != "-" else ""
            region_raw = parts[6].upper() if len(parts) > 6 and parts[6] != "-" else "ALL"
            icon_id = parts[8].lower() if len(parts) > 8 and parts[8] != "-" else tid

            if region_raw in ("USA", "EUR", "JPN", "ALL"):
                region = region_raw
            else:
                region = "ALL"

            # Prefer the first descriptive entry or the longer title description
            if tid not in catalog or len(name) > len(catalog[tid].name):
                catalog[tid] = CSVTitleInfo(
                    title_id=tid,
                    name=name,
                    product_code=product_code,
                    company_code=company_code,
                    region=region,
                    versions=versions,
                    icon_id=icon_id,
                )
        logger.info("Loaded %d title metadata rows from %s", len(catalog), csv_path)
    except OSError as exc:
        logger.warning("Failed to load CSV title database from %s: %s", csv_path, exc)

    return catalog


# ---------------------------------------------------------------------------
# WUX / WUD Disc Partition Parser (SI + GM Virtual WUP Extraction)
# ---------------------------------------------------------------------------

def _read_cstring(buf: bytes, offset: int, max_len: int = 256) -> str:
    if offset < 0 or offset >= len(buf):
        return ""
    end = buf.find(b"\x00", offset, min(len(buf), offset + max_len))
    if end == -1:
        end = min(len(buf), offset + max_len)
    return buf[offset:end].decode("ascii", errors="ignore")


def parse_disc_image_titles(
    reader: DiscImageReader,
    disc_key: bytes,
    common_key: bytes,
    folder_path: Path,
    source_label: str,
) -> List[Tuple[TMDMetadata, Dict[str, VirtualFileSource]]]:
    """
    Decrypt a Wii U .wux or .wud disc image's partition table (0x18000),
    extract `title.cert`, `title.tik`, and `title.tmd` from the `SI` partition,
    and build virtual `.app` and `.h3` file mappings from the `GM<title_id>`
    partition(s) without writing gigabytes of files to disk.
    """
    if len(disc_key) != 16:
        raise ValueError(f"Disc key must be 16 bytes (got {len(disc_key)}) for {source_label}")

    part_tbl_enc = reader.read_at(WUD_PART_TABLE_OFFSET, WUD_SECTOR_SIZE)
    if len(part_tbl_enc) < WUD_SECTOR_SIZE:
        raise ValueError(f"Could not read 0x8000 partition table at 0x18000 from {source_label}")

    zero_iv = b"\x00" * 16
    part_tbl = aes_cbc_decrypt(disc_key, zero_iv, part_tbl_enc)
    magic = struct.unpack_from(">I", part_tbl, 0)[0]
    if magic != WUD_FST_MAGIC:
        raise ValueError(
            f"Invalid WUD partition table magic (0x{magic:08X} != 0x{WUD_FST_MAGIC:08X}) in {source_label}. "
            f"Verify that the companion .key file contains the valid 16-byte disc key."
        )

    expected_sha1 = part_tbl[8:28]
    actual_sha1 = hashlib.sha1(part_tbl[0x800:0x8000]).digest()
    if actual_sha1 != expected_sha1:
        raise ValueError(f"Invalid partition table TOC SHA-1 in {source_label}")

    num_partitions = struct.unpack_from(">I", part_tbl, 0x1C)[0]
    if num_partitions <= 0 or num_partitions > 64:
        raise ValueError(f"Invalid partition count ({num_partitions}) in {source_label}")

    partitions: Dict[str, int] = {}
    for idx in range(num_partitions):
        entry_off = 0x800 + idx * 0x80
        if entry_off + 0x24 > len(part_tbl):
            break
        p_name = _read_cstring(part_tbl, entry_off, 0x1F).strip().upper()
        p_sector_off = struct.unpack_from(">I", part_tbl, entry_off + 0x20)[0]
        if p_name:
            byte_off = p_sector_off * WUD_SECTOR_SIZE
            partitions[p_name] = byte_off
            if p_name.startswith("GM") and len(p_name) >= 18:
                partitions[p_name[:18]] = byte_off

    if "SI" not in partitions:
        raise ValueError(f"No SI partition found in {source_label} (partitions: {list(partitions.keys())})")

    si_offset = partitions["SI"]
    si_fst_enc = reader.read_at(si_offset + WUD_SECTOR_SIZE, WUD_SECTOR_SIZE)
    if len(si_fst_enc) < WUD_SECTOR_SIZE:
        raise ValueError(f"Truncated SI FST in {source_label}")

    si_fst = aes_cbc_decrypt(disc_key, zero_iv, si_fst_enc)
    sec_hdr_bytes = struct.unpack_from(">I", si_fst, 8)[0] << 5
    fe_base = 0x20 + sec_hdr_bytes
    if fe_base + 16 > len(si_fst):
        raise ValueError(f"Corrupt SI FST header in {source_label}")

    total_entries = struct.unpack_from(">I", si_fst, fe_base + 8)[0]
    name_table_off = fe_base + (total_entries << 4)
    si_data_base = si_offset + 2 * WUD_SECTOR_SIZE

    cert_bytes: Optional[bytes] = None
    tmds_by_tid: Dict[str, bytes] = {}
    tiks_by_tid: Dict[str, bytes] = {}

    for entry_idx in range(1, total_entries):
        if cert_bytes is not None and tmds_by_tid and tiks_by_tid:
            break
        e_off = fe_base + entry_idx * 16
        if e_off + 16 > len(si_fst):
            break
        type_name, file_offset_raw, file_len, _flags, content_idx = struct.unpack_from(
            ">IIIHH", si_fst, e_off
        )
        entry_type = (type_name >> 24) & 0xFF
        if entry_type & 1:  # Directory entry
            continue
        name_off = type_name & 0x00FFFFFF
        fname = _read_cstring(si_fst, name_table_off + name_off, 128).lower()
        if not fname.startswith("title.") or file_len == 0:
            continue

        # Resolve content base offset from SI FST secondary header when present
        app_off_pos = 0x20 + content_idx * 0x20
        if app_off_pos + 4 <= fe_base:
            sec_rel = struct.unpack_from(">I", si_fst, app_off_pos)[0]
            content_base = si_offset + (sec_rel * WUD_SECTOR_SIZE) if sec_rel >= 2 else si_data_base
        else:
            content_base = si_data_base

        cnt_off = file_offset_raw << 5
        aligned_len = align_forward(file_len, 16)
        enc_blob = reader.read_at(content_base + cnt_off, aligned_len)
        if len(enc_blob) < aligned_len:
            continue

        cnt_iv = (b"\x00" * 8) + struct.pack(">Q", cnt_off >> 16)
        dec_blob = aes_cbc_decrypt(disc_key, cnt_iv, enc_blob)[:file_len]

        if fname == "title.cert" and cert_bytes is None:
            cert_bytes = dec_blob
        elif fname == "title.tik" and len(dec_blob) >= 0x1E4:
            tid_hex = dec_blob[0x1DC:0x1E4].hex().lower()
            if tid_hex.startswith("00050000") and tid_hex not in tiks_by_tid:
                tiks_by_tid[tid_hex] = dec_blob
        elif fname == "title.tmd" and len(dec_blob) >= TMD_MIN_HEADER_SIZE:
            tid_hex = dec_blob[TMD_TITLE_ID_OFFSET : TMD_TITLE_ID_OFFSET + 8].hex().lower()
            if tid_hex.startswith("00050000") and tid_hex not in tmds_by_tid:
                tmds_by_tid[tid_hex] = dec_blob

    if not tmds_by_tid:
        raise ValueError(f"No valid title.tmd found in SI partition of {source_label}")

    results: List[Tuple[TMDMetadata, Dict[str, VirtualFileSource]]] = []

    # Sort so main game (00050000...) is processed first
    for tid_hex in sorted(tmds_by_tid.keys()):
        gm_key = f"GM{tid_hex.upper()}"
        if gm_key not in partitions:
            continue

        raw_tmd = tmds_by_tid[tid_hex]
        tmd_meta = parse_tmd_bytes(raw_tmd, source_label=f"{source_label}:{tid_hex}")
        if not tmd_meta.contents:
            continue

        raw_tik = tiks_by_tid.get(tid_hex)
        if raw_tik is None:
            raw_tik = synthesize_fake_ticket(tmd_meta, common_key=common_key)

        raw_cert = cert_bytes if cert_bytes is not None else synthesize_fake_cert(raw_tmd, raw_tik)

        # Decrypt the title key from title.tik using the Wii U Common Key
        tik_iv = bytes.fromhex(tid_hex) + (b"\x00" * 8)
        enc_title_key = raw_tik[0x1BF:0x1CF]
        title_key = aes_cbc_decrypt(common_key, tik_iv, enc_title_key)

        gm_offset = partitions[gm_key]
        gm_hdr = reader.read_at(gm_offset, WUD_SECTOR_SIZE)
        if len(gm_hdr) < WUD_SECTOR_SIZE:
            raise ValueError(f"Truncated GM partition header for {tid_hex} in {source_label}")

        f_hdr_cnt = struct.unpack_from(">I", gm_hdr, 0x10)[0]
        hash_pos = 0x40 + (f_hdr_cnt * 4)

        # Content 0 is the GM FST at gm_offset + 0x8000
        c0 = tmd_meta.contents[0]
        c0_aligned_size = align_forward(c0.size, 16)
        gm_fst_enc = reader.read_at(gm_offset + WUD_SECTOR_SIZE, c0_aligned_size)
        if len(gm_fst_enc) < c0_aligned_size:
            raise ValueError(f"Truncated GM FST (00000000.app) for {tid_hex} in {source_label}")

        c0_iv = struct.pack(">H", c0.index) + (b"\x00" * 14)
        gm_fst_dec = aes_cbc_decrypt(title_key, c0_iv, gm_fst_enc)

        vfiles: Dict[str, VirtualFileSource] = {
            "title.tmd": VirtualFileSource(name="title.tmd", size=len(raw_tmd), memory_data=raw_tmd),
            "title.tik": VirtualFileSource(name="title.tik", size=len(raw_tik), memory_data=raw_tik),
            "title.cert": VirtualFileSource(name="title.cert", size=len(raw_cert), memory_data=raw_cert),
        }

        # Content 0 (.app) matches wud2app's ALIGN_FORWARD(fstSize, 16) output
        c0_name = f"{c0.cid:08x}.app"
        vfiles[c0_name] = VirtualFileSource(
            name=c0_name,
            size=c0_aligned_size,
            memory_data=gm_fst_enc,
        )

        # Contents 1 .. content_count - 1
        for cur_idx in range(1, tmd_meta.content_count):
            content = tmd_meta.contents[cur_idx]
            app_tbl_off = 0x20 + cur_idx * 0x20
            if app_tbl_off + 4 > len(gm_fst_dec):
                raise ValueError(
                    f"GM FST app_tbl out of bounds for content #{cur_idx} in {source_label}"
                )
            app_sector_off = struct.unpack_from(">I", gm_fst_dec, app_tbl_off)[0]
            total_app_offset = gm_offset + app_sector_off * WUD_SECTOR_SIZE

            app_name = f"{content.cid:08x}.app"
            vfiles[app_name] = VirtualFileSource(
                name=app_name,
                size=content.size,
                disc_reader=reader,
                disc_offset=total_app_offset,
            )

            if content.content_type & 0x0002:
                hash_num = (content.size // 0x10000000) + 1
                h3_size = 0x14 * hash_num
                h3_bytes = gm_hdr[hash_pos : hash_pos + h3_size]
                hash_pos += h3_size
                h3_name = f"{content.cid:08x}.h3"
                vfiles[h3_name] = VirtualFileSource(
                    name=h3_name,
                    size=len(h3_bytes),
                    memory_data=h3_bytes,
                )

        results.append((tmd_meta, vfiles))

    return results


# ---------------------------------------------------------------------------
# 2. Library Scanner
# ---------------------------------------------------------------------------

class LibraryCatalog:
    """
    Scans and indexes Wii U titles in the library root from:
      - Compressed .wux disc images + companion .key files
      - Raw .wud / split game_part1.wud disc images + companion .key files
      - Extracted WUP folders containing title.tmd
    """

    def __init__(
        self,
        library_root: Path,
        keys_file: Optional[Path] = None,
        csv_file: Optional[Path] = None,
        common_key: Optional[bytes] = None,
    ) -> None:
        self.library_root = library_root.resolve()
        self.keys_file = keys_file.resolve() if keys_file and keys_file.is_file() else None

        if csv_file and csv_file.is_file():
            self.csv_file: Optional[Path] = csv_file.resolve()
        else:
            default_csv = Path(__file__).resolve().parent / "WiiU-Titles-Library.csv"
            self.csv_file = default_csv if default_csv.is_file() else None

        self.keys_db, keys_common_key = load_keys_txt(self.keys_file)
        self.aliases: Dict[str, str] = {
            tid: name for tid, (_, name) in self.keys_db.items() if name
        }
        self.json_db: Dict[str, str] = load_json_title_databases(
            [Path(__file__).resolve().parent, Path(__file__).resolve().parent.parent, self.library_root]
        )
        self.csv_db: Dict[str, CSVTitleInfo] = load_csv_title_database(self.csv_file)
        self.common_key: bytes = common_key or keys_common_key or self._find_common_key()
        self._titles: Dict[str, TitleEntry] = {}
        self._disc_cache: Dict[Tuple[Path, int, int], List[TitleEntry]] = {}

        # Discover all library roots to scan (including sibling ROMS/roms/wiiu if present)
        self.library_roots: List[Path] = [self.library_root]
        sibling_wiiu = self.library_root.parent / "ROMS" / "roms" / "wiiu"
        if sibling_wiiu.is_dir() and sibling_wiiu.resolve() not in self.library_roots:
            self.library_roots.append(sibling_wiiu.resolve())
        extra_env = os.environ.get("LOCALESHOP_EXTRA_DIRS", "").strip()
        if extra_env:
            for raw_p in re.split(r"[,;:]", extra_env):
                cand = Path(raw_p.strip())
                if cand.is_dir() and cand.resolve() not in self.library_roots:
                    self.library_roots.append(cand.resolve())

    def _find_common_key(self) -> bytes:
        env_ckey = os.environ.get("LOCALESHOP_COMMON_KEY", "").strip()
        if len(env_ckey) == 32 and all(c in "0123456789abcdefABCDEF" for c in env_ckey):
            return bytes.fromhex(env_ckey)

        for candidate in (
            self.library_root / "common.key",
            Path(__file__).resolve().parent / "common.key",
            Path.cwd() / "common.key",
        ):
            parsed = parse_key_bytes_from_file(candidate)
            if parsed and len(parsed) == 16:
                return parsed

        return DEFAULT_WIIU_COMMON_KEY

    def scan(self) -> List[TitleEntry]:
        """
        Walk the library directory, indexing:
          1. Extracted WUP package folders containing `title.tmd`
          2. Compressed `.wux` disc images + `.key`
          3. Uncompressed `.wud` (or split `game_part1.wud`) disc images + `.key`
        """
        if not self.library_root.exists():
            logger.warning("Library root '%s' does not exist; creating it.", self.library_root)
            self.library_root.mkdir(parents=True, exist_ok=True)

        discovered: Dict[str, TitleEntry] = {}

        for scan_root in self.library_roots:
            for root, dirs, files in os.walk(scan_root):
                dirs.sort()
                files.sort()
                folder_path = Path(root).resolve()

                # A. Check for extracted WUP directory containing title.tmd
                tmd_filename = next((f for f in files if f.lower() == "title.tmd"), None)
                if tmd_filename:
                    self._index_wup_folder(folder_path, tmd_filename, discovered)
                    dirs.clear()
                    continue

                # B. Check for split wudump (game_part1.wud)
                part1_name = next((f for f in files if f.lower() == "game_part1.wud"), None)
                if part1_name:
                    self._index_disc_file(folder_path / part1_name, discovered, is_split_wud=True)
                    continue

                # C. Check for .wux and .wud disc images in this folder
                for fname in files:
                    lower = fname.lower()
                    if lower.endswith(".wux"):
                        self._index_disc_file(folder_path / fname, discovered, is_wux=True)
                    elif lower.endswith(".wud") and not re.match(r"^game_part\d+\.wud$", lower):
                        self._index_disc_file(folder_path / fname, discovered, is_wux=False)

        self._titles = discovered
        return list(self._titles.values())

    def _index_wup_folder(
        self,
        folder_path: Path,
        tmd_filename: str,
        discovered: Dict[str, TitleEntry],
    ) -> None:
        tmd_path = folder_path / tmd_filename
        try:
            raw_tmd = tmd_path.read_bytes()
            tmd_meta = parse_tmd_bytes(raw_tmd, source_label=str(tmd_path))
        except Exception as exc:
            logger.warning("Skipping %s: failed to parse %s (%s)", folder_path, tmd_filename, exc)
            return

        vfiles: Dict[str, VirtualFileSource] = {}
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
                    fsize = full_p.stat().st_size
                    rel_posix = full_p.relative_to(folder_path).as_posix()
                    total_size += fsize
                    rel_files.append(rel_posix)
                    vfiles[rel_posix] = VirtualFileSource(
                        name=rel_posix,
                        size=fsize,
                        file_path=full_p,
                    )
                except OSError as exc:
                    logger.warning("Could not stat file %s: %s", full_p, exc)

        lower_names = {r.lower(): r for r in rel_files}

        # Synthesize fake title.tik if missing from the extracted WUP directory
        raw_tik: Optional[bytes] = None
        if "title.tik" not in lower_names:
            custom_key: Optional[bytes] = None
            comp_key_file = find_companion_key_file(tmd_path)
            if comp_key_file:
                raw_k = parse_key_bytes_from_file(comp_key_file)
                if raw_k and len(raw_k) == 16:
                    iv = bytes.fromhex(tmd_meta.title_id) + (b"\x00" * 8)
                    custom_key = aes_cbc_encrypt(self.common_key, iv, raw_k)
            elif tmd_meta.title_id in self.keys_db:
                raw_k = self.keys_db[tmd_meta.title_id][0]
                iv = bytes.fromhex(tmd_meta.title_id) + (b"\x00" * 8)
                custom_key = aes_cbc_encrypt(self.common_key, iv, raw_k)

            raw_tik = synthesize_fake_ticket(
                tmd_meta,
                encrypted_title_key=custom_key,
                common_key=self.common_key,
            )
            vfiles["title.tik"] = VirtualFileSource(
                name="title.tik",
                size=len(raw_tik),
                memory_data=raw_tik,
            )
            rel_files.append("title.tik")
            total_size += len(raw_tik)
        else:
            tik_rel = lower_names["title.tik"]
            try:
                raw_tik = (folder_path / tik_rel).read_bytes()
            except OSError:
                raw_tik = None

        # Synthesize title.cert if missing from the extracted WUP directory
        if "title.cert" not in lower_names:
            raw_cert = synthesize_fake_cert(raw_tmd=raw_tmd, raw_tik=raw_tik)
            vfiles["title.cert"] = VirtualFileSource(
                name="title.cert",
                size=len(raw_cert),
                memory_data=raw_cert,
            )
            rel_files.append("title.cert")
            total_size += len(raw_cert)

        rel_files.sort()
        name, region, category, product_code = self._resolve_metadata(
            folder_path,
            tmd_meta.title_id,
            stem_hint=folder_path.name,
        )

        entry = TitleEntry(
            title_id=tmd_meta.title_id,
            name=name,
            version=tmd_meta.version,
            content_count=tmd_meta.content_count,
            total_size_bytes=total_size,
            file_list=rel_files,
            folder_path=folder_path,
            region=region,
            category=category,
            product_code=product_code,
            source_format="wup",
            virtual_files=vfiles,
        )
        self._register_entry(entry, discovered, str(folder_path))

    def _index_disc_file(
        self,
        disc_path: Path,
        discovered: Dict[str, TitleEntry],
        is_wux: bool = True,
        is_split_wud: bool = False,
    ) -> None:
        resolved_disc = disc_path.resolve()
        try:
            st = resolved_disc.stat()
            cache_key = (resolved_disc, st.st_mtime_ns, st.st_size)
        except OSError:
            return

        if cache_key in self._disc_cache:
            for cached_entry in self._disc_cache[cache_key]:
                self._register_entry(cached_entry, discovered, str(disc_path), quiet=True)
            return

        folder_path = resolved_disc.parent
        key_file = find_companion_key_file(resolved_disc)
        disc_key = parse_key_bytes_from_file(key_file) if key_file else None

        if disc_key is None:
            logger.warning(
                "Skipping %s: no valid 16-byte companion .key file found in %s",
                disc_path.name,
                folder_path,
            )
            return

        try:
            if is_split_wud:
                reader: DiscImageReader = SplitWudImageReader(resolved_disc)
            elif is_wux:
                reader = WuxImageReader(resolved_disc)
            else:
                reader = WudImageReader(resolved_disc)

            parsed_titles = parse_disc_image_titles(
                reader=reader,
                disc_key=disc_key,
                common_key=self.common_key,
                folder_path=folder_path,
                source_label=str(disc_path),
            )
        except Exception as exc:
            logger.warning("Skipping disc image %s: %s", disc_path, exc)
            return

        stem_hint = disc_path.stem if not is_split_wud else folder_path.name
        cached_entries: List[TitleEntry] = []
        for tmd_meta, vfiles in parsed_titles:
            rel_files = sorted(vfiles.keys())
            total_size = sum(vf.size for vf in vfiles.values())
            name, region, category, product_code = self._resolve_metadata(
                folder_path,
                tmd_meta.title_id,
                stem_hint=stem_hint,
            )
            entry = TitleEntry(
                title_id=tmd_meta.title_id,
                name=name,
                version=tmd_meta.version,
                content_count=tmd_meta.content_count,
                total_size_bytes=total_size,
                file_list=rel_files,
                folder_path=folder_path,
                region=region,
                category=category,
                product_code=product_code,
                source_format=reader.source_format,
                virtual_files=vfiles,
            )
            cached_entries.append(entry)
            self._register_entry(entry, discovered, str(disc_path))

        self._disc_cache[cache_key] = cached_entries

    def _register_entry(
        self,
        entry: TitleEntry,
        discovered: Dict[str, TitleEntry],
        source_desc: str,
        quiet: bool = False,
    ) -> None:
        if entry.title_id in discovered:
            existing = discovered[entry.title_id]
            if entry.version >= existing.version:
                if not quiet:
                    logger.info(
                        "Replacing duplicate Title ID %s (v%d -> v%d) from %s",
                        entry.title_id,
                        existing.version,
                        entry.version,
                        source_desc,
                    )
                discovered[entry.title_id] = entry
        else:
            discovered[entry.title_id] = entry
            if not quiet:
                logger.info(
                    "Indexed [%s] Title ID %s | Name: '%s' (%s, %s) | v%d | Contents: %d | Size: %d bytes",
                    entry.source_format.upper(),
                    entry.title_id,
                    entry.name,
                    entry.region,
                    entry.category,
                    entry.version,
                    entry.content_count,
                    entry.total_size_bytes,
                )

    def _resolve_name(self, folder_path: Path, title_id: str) -> str:
        name, _, _, _ = self._resolve_metadata(folder_path, title_id, stem_hint=folder_path.name)
        return name

    def _resolve_metadata(
        self,
        folder_path: Path,
        title_id: str,
        stem_hint: Optional[str] = None,
    ) -> Tuple[str, str, str, str]:
        tid = title_id.lower().strip()
        category = category_from_title_id(tid)
        region = "ALL"
        product_code = ""

        csv_info = self.csv_db.get(tid)
        if csv_info is None and len(tid) == 16 and tid[:8] in ("0005000e", "0005000c", "00050002"):
            # Fallback to base game's CSV entry for Updates, DLC, and Demos
            base_tid = "00050000" + tid[8:]
            csv_info = self.csv_db.get(base_tid)

        if csv_info:
            region = csv_info.region
            product_code = csv_info.product_code

        # 1. Check for keys.txt alias
        alias = self.aliases.get(tid)
        if alias:
            return alias, region, category, product_code

        # 2. Check CSV description
        if csv_info and csv_info.name:
            display = csv_info.name
            if category in ("Update", "DLC", "Demo") and category.lower() not in display.lower():
                display = f"{display} [{category}]"
            return display, region, category, product_code

        # 3. Check titledb.json / titles.json fallback
        json_name = self.json_db.get(tid)
        if not json_name and len(tid) == 16 and tid[:8] in ("0005000e", "0005000c", "00050002"):
            json_name = self.json_db.get("00050000" + tid[8:])
        if json_name:
            display = json_name
            if category in ("Update", "DLC", "Demo") and category.lower() not in display.lower():
                display = f"{display} [{category}]"
            return display, region, category, product_code

        # 4. Fallback to folder or file stem name
        folder_name = stem_hint or folder_path.name
        if folder_path == self.library_root and stem_hint:
            folder_name = stem_hint
        return folder_name, region, category, product_code

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

    end = min(end, file_size - 1)
    return start, end


def stream_file_chunks(
    file_path: Path,
    start: int,
    end: int,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Iterator[bytes]:
    """Yield binary chunks from `start` to `end` (inclusive) from a physical file."""
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


def build_virtual_file_response(
    vfile: VirtualFileSource,
    request: Request,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Response:
    """Build an HTTP 200 OK or 206 Partial Content response for a VirtualFileSource."""
    file_size = vfile.size
    range_header = request.headers.get("range")
    download_name = Path(vfile.name).name

    common_headers = {
        "Accept-Ranges": "bytes",
        "Content-Disposition": f'attachment; filename="{download_name}"',
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
            vfile.iter_bytes(start, end, chunk_size=chunk_size),
            status_code=206,
            headers=headers,
            media_type="application/octet-stream",
        )

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
        vfile.iter_bytes(0, file_size - 1, chunk_size=chunk_size),
        status_code=200,
        headers=headers,
        media_type="application/octet-stream",
    )


def resolve_entry_virtual_file(entry: TitleEntry, filename: str) -> VirtualFileSource:
    """
    Resolve a requested relative filename inside a TitleEntry (supporting both
    virtual WUX/WUD files and physical WUP folder files with path-traversal protection).
    """
    clean_req = filename.strip().lstrip("/")
    if not clean_req or ".." in Path(clean_req).parts:
        raise HTTPException(status_code=403, detail="Path traversal is not permitted")

    # 1. Exact or case-insensitive lookup in virtual_files
    if clean_req in entry.virtual_files:
        return entry.virtual_files[clean_req]

    lower_req = clean_req.lower()
    for rel_key, vf in entry.virtual_files.items():
        if rel_key.lower() == lower_req:
            return vf

    # 2. Physical disk fallback for extracted WUP directories
    try:
        candidate_path = (entry.folder_path / clean_req).resolve()
        candidate_path.relative_to(entry.folder_path)
    except ValueError as exc:
        raise HTTPException(status_code=403, detail="Path traversal is not permitted") from exc

    if not candidate_path.is_file():
        matched_rel = next(
            (rel for rel in entry.file_list if rel.lower() == lower_req),
            None,
        )
        if matched_rel:
            if matched_rel in entry.virtual_files:
                return entry.virtual_files[matched_rel]
            candidate_path = (entry.folder_path / matched_rel).resolve()

    if not candidate_path.is_file():
        raise HTTPException(
            status_code=404,
            detail=f"File '{filename}' not found in title '{entry.title_id}'",
        )

    return VirtualFileSource(
        name=candidate_path.name,
        size=candidate_path.stat().st_size,
        file_path=candidate_path,
    )


# ---------------------------------------------------------------------------
# UDP LAN Discovery Responder
# ---------------------------------------------------------------------------

class DiscoveryUDPProtocol(asyncio.DatagramProtocol):
    """Answers Wii U LAN broadcast probes ('LOCALESHOP_DISCOVER_V1') over UDP."""

    def __init__(self, app: FastAPI, http_port: int) -> None:
        self.app = app
        self.http_port = http_port
        self.transport: Optional[asyncio.DatagramTransport] = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: Tuple[str, int]) -> None:
        if not self.transport:
            return
        if data.strip().startswith(DISCOVERY_REQUEST_MAGIC):
            catalog: LibraryCatalog = self.app.state.catalog
            payload = json.dumps(
                {
                    "magic": DISCOVERY_RESPONSE_MAGIC,
                    "port": self.http_port,
                    "title_count": len(catalog._titles),
                }
            ).encode("utf-8")
            self.transport.sendto(payload, addr)


# ---------------------------------------------------------------------------
# 4. FastAPI Application Factory & Endpoints
# ---------------------------------------------------------------------------

def _default_library_dir() -> Path:
    env_dir = os.environ.get("LOCALESHOP_LIBRARY_DIR")
    if env_dir:
        return Path(env_dir)
    cwd_lib = Path("./library")
    if cwd_lib.is_dir():
        return cwd_lib
    return Path(__file__).resolve().parent / "library"


def create_app(
    library_dir: Optional[Path] = None,
    keys_file: Optional[Path] = None,
    csv_file: Optional[Path] = None,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    enable_udp_discovery: bool = True,
) -> FastAPI:
    lib_path = library_dir or _default_library_dir()
    default_keys = Path(os.environ.get("LOCALESHOP_KEYS_FILE", "./keys.txt"))
    resolved_keys = keys_file if keys_file is not None else (default_keys if default_keys.is_file() else None)

    env_csv = os.environ.get("LOCALESHOP_CSV_FILE")
    default_csv = Path(env_csv) if env_csv else (Path(__file__).resolve().parent / "WiiU-Titles-Library.csv")
    resolved_csv = csv_file if csv_file is not None else (default_csv if default_csv.is_file() else None)

    catalog = LibraryCatalog(
        library_root=lib_path,
        keys_file=resolved_keys,
        csv_file=resolved_csv,
    )
    catalog.scan()

    @asynccontextmanager
    async def lifespan(app_instance: FastAPI):
        udp_transport = None
        if enable_udp_discovery:
            port = int(os.environ.get("LOCALESHOP_PORT", "8000"))
            try:
                loop = asyncio.get_running_loop()
                udp_transport, _ = await loop.create_datagram_endpoint(
                    lambda: DiscoveryUDPProtocol(app_instance, port),
                    local_addr=("0.0.0.0", port),
                )
                logger.info("UDP LAN auto-discovery responder listening on UDP port %d", port)
            except OSError as exc:
                logger.debug("UDP discovery socket on port %d not bound: %s", port, exc)
        try:
            yield
        finally:
            if udp_transport is not None:
                udp_transport.close()

    app = FastAPI(
        title="LocaLeShop Server Daemon",
        description=(
            "Self-hosted LAN WUX/WUD/WUP package indexer and byte-range streamer "
            "for Wii U homebrew (.wuhb)."
        ),
        version="1.1.0",
        lifespan=lifespan,
    )
    app.state.catalog = catalog
    app.state.chunk_size = chunk_size

    @app.get("/api/v1/status")
    def get_status() -> dict:
        """Return server status and indexed title count."""
        return {
            "status": "ok",
            "version": "1.1.0",
            "title_count": len(app.state.catalog._titles),
        }

    @app.post("/api/v1/rescan")
    def rescan_library() -> dict:
        """Trigger a library rescan and return the updated title list."""
        entries = app.state.catalog.list_titles()
        return {
            "status": "ok",
            "title_count": len(entries),
            "titles": [entry.to_api_dict() for entry in entries],
        }

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
        from WUX, WUD, or extracted WUP folders with HTTP 200 OK and 206 Partial Content support.
        """
        entry: Optional[TitleEntry] = app.state.catalog.get_title(title_id)
        if not entry:
            raise HTTPException(status_code=404, detail=f"Title ID '{title_id}' not found")

        vfile = resolve_entry_virtual_file(entry, filename)
        return build_virtual_file_response(vfile, request, chunk_size=app.state.chunk_size)

    @app.api_route(
        "/ccs/download/{title_id}/{resource:path}",
        methods=["GET", "HEAD"],
    )
    def stream_nus_cdn_resource(title_id: str, resource: str, request: Request) -> Response:
        """
        Official Nintendo Update Server (NUS) CDN compatible endpoint:
          - /ccs/download/{title_id}/tmd (or tmd.{version}) -> title.tmd
          - /ccs/download/{title_id}/cetk -> title.tik
          - /ccs/download/{title_id}/{cid} -> {cid}.app
          - /ccs/download/{title_id}/{cid}.h3 -> {cid}.h3
        """
        entry: Optional[TitleEntry] = app.state.catalog.get_title(title_id)
        if not entry:
            raise HTTPException(status_code=404, detail=f"Title ID '{title_id}' not found")

        res_clean = resource.strip().lower()
        if res_clean == "tmd" or res_clean.startswith("tmd."):
            target_file = "title.tmd"
        elif res_clean in ("cetk", "tik", "title.tik"):
            target_file = "title.tik"
        elif res_clean in ("cert", "title.cert"):
            target_file = "title.cert"
        elif res_clean.endswith(".h3") or res_clean.endswith(".app"):
            target_file = res_clean
        else:
            target_file = f"{res_clean}.app"

        vfile = resolve_entry_virtual_file(entry, target_file)
        return build_virtual_file_response(vfile, request, chunk_size=app.state.chunk_size)

    return app


# Default module-level ASGI app instance for `uvicorn server:app`
app = create_app()


# ---------------------------------------------------------------------------
# CLI Entrypoint
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="LocaLeShop — Self-hosted Wii U WUX/WUD/WUP package server daemon."
    )
    parser.add_argument(
        "--library",
        "-l",
        type=Path,
        default=Path(os.environ.get("LOCALESHOP_LIBRARY_DIR", "./library")),
        help="Root directory containing staged Wii U WUX/WUD/WUP title folders (default: ./library).",
    )
    parser.add_argument(
        "--keys",
        "-k",
        type=Path,
        default=Path(os.environ.get("LOCALESHOP_KEYS_FILE", "./keys.txt")),
        help="Optional path to keys.txt for resolving title keys and names (default: ./keys.txt).",
    )
    parser.add_argument(
        "--csv",
        "-c",
        type=Path,
        default=Path(
            os.environ.get(
                "LOCALESHOP_CSV_FILE",
                str(Path(__file__).resolve().parent / "WiiU-Titles-Library.csv"),
            )
        ),
        help="Optional path to WiiU-Titles-Library.csv for title names, regions, and product codes.",
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
        default=int(os.environ.get("LOCALESHOP_PORT", "8000")),
        help="TCP/UDP port to listen on (default: 8000).",
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
    os.environ["LOCALESHOP_PORT"] = str(args.port)
    keys_path = args.keys if args.keys and args.keys.is_file() else None
    csv_path = args.csv if args.csv and args.csv.is_file() else None
    server_app = create_app(
        library_dir=args.library,
        keys_file=keys_path,
        csv_file=csv_path,
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
