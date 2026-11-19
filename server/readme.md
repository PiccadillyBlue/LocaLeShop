# LocaLeShop Server Daemon

A lightweight, self-hosted Python 3 LAN server daemon for **LocaLeShop** that indexes Wii U titles in **`.wux`** (compressed Wii U Disc image + `.key`), **`.wud`** (raw or split `wudump` disc image + `.key`), and **extracted WUP package folders** (`.app`, `.h3`, `title.tmd`, `title.tik`, `title.cert`), streaming installable WUP files directly over HTTP (`200 OK` / `206 Partial Content` byte-range support) to a paired Wii U homebrew client (`.wuhb`).

## Features

- **Zero-Extraction On-the-Fly `.wux` & `.wud` Streaming**:
  - Parses compressed `WUX0` sector lookup tables directly in memory.
  - Decrypts the disc partition table (`0x18000`) and `SI` partition using the companion `.key` file (supports both raw 16-byte binary `.key` files and 32-character hex text `.key` files).
  - Extracts `title.tmd`, `title.tik`, and `title.cert` into memory and maps every `.app` and `.h3` content file inside the `GM<title_id>` partition so the Wii U can stream and install `.wux` titles directly to a Home Screen tile without needing 23 GB of temporary server extraction space.
- **Automatic Fake Ticket & Certificate Synthesis**:
  - If an extracted WUP folder is missing `title.tik` or `title.cert`, automatically synthesizes a valid ticket and certificate chain so `MCP_InstallTitleAsync` can install it cleanly.
- **Rich Metadata via `WiiU-Titles-Library.csv`**:
  - Resolves human-readable title names, regions (`USA`, `EUR`, `JPN`, `ALL`), categories (`Game`, `Update`, `DLC`, `Demo`, `System`), and product codes automatically.
- **Official Nintendo NUS CDN Endpoint Compatibility (`/ccs/download/...`)**:
  - In addition to `/api/v1/titles`, exposes `/ccs/download/{title_id}/tmd`, `/ccs/download/{title_id}/cetk`, and `/ccs/download/{title_id}/{cid}` for native `nn::nim` / NUS compatibility.
- **UDP LAN Auto-Discovery**:
  - Listens for UDP broadcast probes (`LOCALESHOP_DISCOVER_V1`) on the server port so the Wii U client can find the server on your local network automatically.

## Directory Layout

Place `.wux` + `.key` folders, `.wud` + `.key` folders, or extracted WUP folders inside your library directory:

```text
library/
├── The Legend of Zelda Breath of the Wild/
│   ├── botw.wux
│   └── botw.key
├── Mario Kart 8/
│   ├── mk8.wud
│   └── game.key
└── Super Mario 3D World [0005000010145c00]/
    ├── title.tmd
    ├── title.tik
    ├── title.cert
    ├── 00000000.app
    └── 00000000.h3
```

## Installation

1. Ensure **Python 3.9+** is installed.
2. Install dependencies from `requirements.txt`:

```bash
pip install -r requirements.txt
```

## Running the Server

```bash
python3 server.py --library ./library --host 0.0.0.0 --port 8080
```

| CLI Flag | Environment Variable | Default | Description |
| :--- | :--- | :--- | :--- |
| `--library`, `-l` | `LOCALESHOP_LIBRARY_DIR` | `./library` | Root folder containing `.wux`, `.wud`, or extracted WUP title directories |
| `--csv`, `-c` | `LOCALESHOP_CSV_FILE` | `./WiiU-Titles-Library.csv` | Title metadata CSV database |
| `--keys`, `-k` | `LOCALESHOP_KEYS_FILE` | `./keys.txt` | Optional `keys.txt` file for additional title keys and aliases |
| `--host`, `-H` | `LOCALESHOP_HOST` | `0.0.0.0` | Network interface to bind |
| `--port`, `-p` | `LOCALESHOP_PORT` | `8080` | TCP & UDP port to listen on |
| `--chunk-size` | `LOCALESHOP_CHUNK_SIZE` | `65536` | Streaming chunk size in bytes (`64 KiB`) |

## API Reference

- `GET /api/v1/titles` — Returns JSON array of all indexed `.wux`, `.wud`, and `.wup` titles.
- `GET /api/v1/status` — Health check and indexed title count.
- `POST /api/v1/rescan` — Rescans the library directory.
- `GET /api/v1/titles/{title_id}/files/{filename}` — Streams any package file (`200 OK` or `206 Partial Content`).
- `GET /ccs/download/{title_id}/{tmd|cetk|<cid>}` — Official Nintendo Update Server (NUS) CDN compatible endpoints.
