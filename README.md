# LocaLeShop

**LocaLeShop** is a self-hosted LAN eShop server and Wii U homebrew client (`.wuhb`) that lets you host your Wii U games on a local server—directly in **`.wux`** (compressed Wii U Disc image + `.key`), **`.wud`** (raw or split `wudump` disc image + `.key`), or **extracted WUP package** format—and browse, download, and install them on your Wii U console to playable Home Screen tiles.

## Features

- **Zero-Extraction `.wux` & `.wud` Streaming (`server/server.py`)**:
  - Parses compressed `WUX0` sector lookup tables and decrypts the disc partition table (`0x18000`) and `SI` partition in memory using your companion `.key` file (e.g., `/wiiu/game_title/game_title.wux` and `game_title.key`).
  - Streams virtual `.app`, `.h3`, `title.tmd`, `title.tik`, and `title.cert` files over HTTP (`200 OK` / `206 Partial Content` byte-range support) directly out of `.wux` and `.wud` containers without requiring 23 GB of temporary server extraction space.
  - Synthesizes missing `title.tik` and `title.cert` files automatically for extracted WUP folders.
  - Enriches titles with official names, regions (`USA`, `EUR`, `JPN`), categories (`Game`, `Update`, `DLC`, `Demo`), and product codes via `server/WiiU-Titles-Library.csv`.
  - Also exposes official Nintendo Update Server (NUS) CDN compatible endpoints (`/ccs/download/{title_id}/...`) and a UDP LAN auto-discovery responder.
- **Aero Glass eShop Wii U Client (`client/src/main.c`)**:
  - **Playable Home Screen Tile Installation**: Installs downloaded titles via `MCP_InstallTitleAsync` to **USB Storage** or **System Memory (NAND)** so they appear as official tiles on the Wii U Menu.
  - **Installed Title Detection**: Queries the console's MCP title database (`MCP_GetTitleInfo`) to badge titles already installed on your Wii U Home Screen.
  - **Zero-Config LAN Auto-Discovery & In-App IP Settings**: Automatically finds your LocaLeShop server on the local network via UDP broadcast (`X`), or lets you configure the server IP and port directly on the GamePad (`-`).
  - **Category Tabs & Multi-Title Queue**: Filter by *All Titles*, *Games*, *Updates*, *DLC*, or *Demos* (`L` / `R`), queue multiple titles (`+`), or bundle a base Game + its Update + DLC in one click (`X` in the Title Details view).
  - **High-Speed LAN Transfers**: Uses NUSspli's 3 MiB `somemopt` socket buffer donation, 256 KiB TCP receive windows, live download speed/ETA reporting, and automatic HTTP `206` byte-range resume on network hiccups.

## Quick Start

### 1. Running the Server

```bash
cd server
pip install -r requirements.txt
python3 server.py --library ./library --host 0.0.0.0 --port 8080
```

Place your `.wux` + `.key` folders, `.wud` + `.key` folders, or extracted WUP folders inside `server/library/`:

```text
server/library/
├── The Legend of Zelda Breath of the Wild/
│   ├── botw.wux
│   └── botw.key
└── Mario Kart 8 [000500001010ec00]/
    ├── title.tmd
    ├── title.tik
    ├── title.cert
    ├── 00000000.app
    └── 00000000.h3
```

### 2. Building the Wii U Client (`.wuhb`)

Using the devkitPro `devkitppc` Docker image:

```bash
docker run --rm -v "${PWD}/client:/client" devkitpro/devkitppc:20260503 sh -c \
  "mkdir -p /client/build && cd /client/build && cmake /client && make -j$(nproc)"
```

Copy the resulting `LocaLeShop.wuhb` to `sd:/wiiu/apps/LocaLeShop/LocaLeShop.wuhb` on your console's SD card.

### 3. GamePad Controls

| Button | Action |
| :--- | :--- |
| **D-Pad Up / Down** | Navigate title list |
| **D-Pad Left / Right** | Page Up / Page Down |
| **L / R (or ZL / ZR)** | Switch category tab (*All*, *Games*, *Updates*, *DLC*, *Demos*) |
| **A** | Open Title Details & Download/Install to Home Screen |
| **+ (PLUS)** | Add / Remove selected title from the download queue |
| **Y** | Toggle installation destination between **USB Storage** and **System Memory (NAND)** |
| **X** | Auto-Discover LocaLeShop server on LAN (or bundle *Game + Update + DLC* inside Title Details) |
| **- (MINUS)** | Open in-app Server IP & Port Settings |
| **B** | Close modal / Cancel active download |
| **HOME** | Return to Wii U Menu |

## Credits

LocaLeShop is based on [NUSspli](https://github.com/V10lator/NUSspli) by V10lator, which itself is based on [WUPDownloader](https://github.com/Pokes303/WUPDownloader) by Pokes303, and incorporates disc partition layout research from [wud2app](https://github.com/FIX94/wud2app) by FIX94.
