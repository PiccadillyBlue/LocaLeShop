# My contributions to this forked software are 100% cuck-coded, and thus will never reach a full 1.0 release because I have no idea how to code. You may treat this fork as a base from which a real coder can come in and refine this functionality.
So just, y'know...keep that in mind. I'm not gonna sit here and lie to you like I'm some big-shot software engineer.

#LocaLeShop
Yeah so basically the idea is that you can host your own extracted WUXes on your server and have your Wii U console directly pull from there so you don't have to deal with file management yourself, it just works like an eShop.

# LocaLeShop Server Daemon

A lightweight, self-hosted Python 3 LAN server daemon for **LocaLeShop** that indexes extracted Wii U WUP packages (`.app`, `.h3`, `title.tmd`, `title.tik`, `title.cert`) and streams them over HTTP (with RFC 9110 `200 OK` / `206 Partial Content` byte-range support) to a paired Wii U homebrew client (`.wuhb`).

## Features

- **Binary TMD Parser**: Reads big-endian Wii U `title.tmd` headers directly:
  - `Title ID` at offset `0x18C` (`8` bytes, formatted as a 16-character lowercase hex string)
  - `Title Version` at offset `0x1DC` (`2` bytes, unsigned short `>H`)
  - `Content Count` at offset `0x1DE` (`2` bytes, unsigned short `>H`)
- **Automatic Library Scanner**: Recursively scans a staging directory (default `./library`) for folders containing `title.tmd`, computes total package byte size, and indexes all relative file paths. Optionally resolves human-readable game titles from a `keys.txt` database if present.
- **Chunked Byte-Range Streaming (`200` / `206`)**: Streams `.app`, `.h3`, `.tmd`, `.tik`, and `.cert` files in configurable chunks with `Accept-Ranges: bytes`, `Content-Range`, and `Content-Length` headers, plus path-traversal protection and case-insensitive filename fallback for FAT32/WUP compatibility.

## Directory Layout

Place extracted WUP packages inside your library folder so each package folder contains a `title.tmd`:

```text
library/
├── The Legend of Zelda Breath of the Wild [00050000101c9400]/
│   ├── title.tmd
│   ├── title.tik
│   ├── title.cert
│   ├── 00000000.app
│   └── 00000000.h3
└── Mario Kart 8 USA/
    ├── title.tmd
    ├── title.tik
    ├── title.cert
    └── 00000000.app
```

## Installation

1. Ensure **Python 3.9+** is installed.
2. Install dependencies from `requirements.txt`:

```bash
pip install -r requirements.txt
```

## Running the Server

### Using the CLI

```bash
python3 server.py --library ./library --host 0.0.0.0 --port 8080
```

Optional flags and environment variables:

| CLI Flag | Environment Variable | Default | Description |
| :--- | :--- | :--- | :--- |
| `--library`, `-l` | `LOCALESHOP_LIBRARY_DIR` | `./library` | Root folder containing staged WUP title directories |
| `--keys`, `-k` | `LOCALESHOP_KEYS_FILE` | `./keys.txt` | Optional `keys.txt` file to resolve Title IDs to human-readable game names |
| `--host`, `-H` | `LOCALESHOP_HOST` | `0.0.0.0` | Network interface to bind |
| `--port`, `-p` | `LOCALESHOP_PORT` | `8080` | TCP port to listen on |
| `--chunk-size` | `LOCALESHOP_CHUNK_SIZE` | `65536` | Streaming chunk size in bytes (`64 KiB`) |

### Using Uvicorn Directly

```bash
LOCALESHOP_LIBRARY_DIR=./library uvicorn server:app --host 0.0.0.0 --port 8080
```

## API Reference

### 1. List Indexed Titles

- **Endpoint**: `GET /api/v1/titles`
- **Response (`200 OK`)**:

```json
[
  {
    "title_id": "00050000101c9400",
    "name": "The Legend of Zelda Breath of the Wild",
    "version": 208,
    "content_count": 29,
    "total_size_bytes": 2147485920,
    "file_list": [
      "00000000.app",
      "00000000.h3",
      "title.cert",
      "title.tik",
      "title.tmd"
    ]
  }
]
```

### 2. Stream Package File (Full or Byte Range)

- **Endpoint**: `GET /api/v1/titles/{title_id}/files/{filename}`
- **Full Download**:

```bash
curl -O http://localhost:8080/api/v1/titles/00050000101c9400/files/title.tmd
```

- **Partial Chunk Request (`206 Partial Content`)**:

```bash
curl -i -H "Range: bytes=0-1023" \
  http://localhost:8080/api/v1/titles/00050000101c9400/files/00000000.app
```
