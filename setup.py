#!/usr/bin/env python3
"""
LocaLeShop Cross-Platform Interactive Setup Wizard
--------------------------------------------------
Sets up the LocaLeShop server and deploys the Wii U client (.wuhb) and
configuration (config.json) on Windows, macOS, and Linux without requiring
end-users to manually compile C code.
"""

from __future__ import annotations

import argparse
import ftplib
import io
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import List, Optional, Tuple

ROOT_DIR = Path(__file__).resolve().parent
SERVER_DIR = ROOT_DIR / "server"
CLIENT_DIR = ROOT_DIR / "client"

GITHUB_REPO = "PiccadillyBlue/LocaLeShop"
GITHUB_RELEASES_API = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"


# ---------------------------------------------------------------------------
# Cross-Platform Terminal Styling & Input Helpers
# ---------------------------------------------------------------------------

def _enable_windows_ansi() -> bool:
    if platform.system() != "Windows":
        return True
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_ulong()
        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)
            return True
    except Exception:
        pass
    return "WT_SESSION" in os.environ or "TERM" in os.environ


USE_COLOR = sys.stdout.isatty() and _enable_windows_ansi() and "NO_COLOR" not in os.environ

BOLD = "\033[1m" if USE_COLOR else ""
CYAN = "\033[0;36m" if USE_COLOR else ""
GREEN = "\033[0;32m" if USE_COLOR else ""
YELLOW = "\033[1;33m" if USE_COLOR else ""
RED = "\033[0;31m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""


def ask(prompt_text: str, default: str = "") -> str:
    suffix = f" [{default}]" if default != "" else ""
    try:
        raw = input(f"{prompt_text}{suffix}: ").strip()
    except (EOFError, KeyboardInterrupt):
        print(f"\n{RED}Setup cancelled by user.{NC}")
        sys.exit(0)
    return raw if raw else default


def ask_yes_no(prompt_text: str, default_yes: bool = True) -> bool:
    hint = "Y/n" if default_yes else "y/N"
    try:
        raw = input(f"{prompt_text} [{hint}]: ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        print(f"\n{RED}Setup cancelled by user.{NC}")
        sys.exit(0)
    if not raw:
        return default_yes
    return raw.startswith("y")


def clean_user_path(raw_path: str) -> Path:
    """
    Normalize a path typed or dragged-and-dropped into the terminal across
    Windows, macOS, and Linux.
    """
    cleaned = raw_path.strip()
    if (cleaned.startswith('"') and cleaned.endswith('"')) or (
        cleaned.startswith("'") and cleaned.endswith("'")
    ):
        cleaned = cleaned[1:-1].strip()
    if platform.system() != "Windows":
        cleaned = cleaned.replace(r"\ ", " ")
    return Path(os.path.expanduser(os.path.expandvars(cleaned))).resolve()


def detect_lan_ip() -> str:
    """Detect the primary LAN IPv4 address on Windows, macOS, and Linux."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("1.1.1.1", 80))
            ip = s.getsockname()[0]
            if ip and not ip.startswith("127."):
                return ip
    except OSError:
        pass

    try:
        hostname = socket.gethostname()
        for addr in socket.gethostbyname_ex(hostname)[2]:
            if not addr.startswith("127."):
                return addr
    except OSError:
        pass

    return "127.0.0.1"


def preview_library_titles(directories: List[Path]) -> Tuple[int, int, int]:
    """Count .wux, .wud, and extracted WUP (title.tmd) packages in the given directories."""
    wux_count = 0
    wud_count = 0
    wup_count = 0
    for d in directories:
        if not d.is_dir():
            continue
        for _root, dirs, files in os.walk(d):
            lower_files = [f.lower() for f in files]
            if "title.tmd" in lower_files:
                wup_count += 1
                dirs.clear()
                continue
            if "game_part1.wud" in lower_files:
                wud_count += 1
                continue
            for lf in lower_files:
                if lf.endswith(".wux"):
                    wux_count += 1
                elif lf.endswith(".wud") and not re.match(r"^game_part\d+\.wud$", lf):
                    wud_count += 1
    return wux_count, wud_count, wup_count


# ---------------------------------------------------------------------------
# Server Virtualenv & Startup Provisioning
# ---------------------------------------------------------------------------

def get_venv_python(venv_dir: Path) -> Path:
    if platform.system() == "Windows":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python"


def setup_server_environment() -> Path:
    """Ensure server/.venv exists and has required dependencies installed."""
    venv_dir = SERVER_DIR / ".venv"
    venv_python = get_venv_python(venv_dir)

    if not venv_python.is_file():
        print(f"    Creating Python virtual environment at {venv_dir}...")
        subprocess.run([sys.executable, "-m", "venv", str(venv_dir)], check=True)

    req_file = SERVER_DIR / "requirements.txt"
    print("    Installing server dependencies (fastapi, uvicorn, cryptography)...")
    subprocess.run(
        [str(venv_python), "-m", "pip", "install", "--quiet", "--upgrade", "-r", str(req_file)],
        check=True,
    )
    return venv_python


def create_server_launcher_scripts(
    venv_python: Path,
    primary_lib: Path,
    extra_libs: List[Path],
    server_port: int,
) -> Path:
    """Create a one-click server launcher script for the current OS."""
    extra_env_val = ";".join(str(p) for p in extra_libs) if extra_libs else ""

    if platform.system() == "Windows":
        launcher = ROOT_DIR / "start_server.bat"
        lines = [
            "@echo off",
            "title LocaLeShop Server Daemon",
            f'cd /d "{SERVER_DIR}"',
        ]
        if extra_env_val:
            lines.append(f'set "LOCALESHOP_EXTRA_DIRS={extra_env_val}"')
        lines.append(
            f'"{venv_python}" "{SERVER_DIR / "server.py"}" --library "{primary_lib}" --host 0.0.0.0 --port {server_port}'
        )
        lines.append("pause")
        launcher.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
    else:
        launcher = ROOT_DIR / "start_server.sh"
        lines = [
            "#!/usr/bin/env bash",
            "set -e",
            f'cd "{SERVER_DIR}"',
        ]
        if extra_env_val:
            lines.append(f'export LOCALESHOP_EXTRA_DIRS="{extra_env_val}"')
        lines.append(
            f'exec "{venv_python}" "{SERVER_DIR / "server.py"}" --library "{primary_lib}" --host 0.0.0.0 --port {server_port}'
        )
        launcher.write_text("\n".join(lines) + "\n", encoding="utf-8")
        try:
            launcher.chmod(0o755)
        except OSError:
            pass

    return launcher


def wait_for_server_health(port: int, timeout_sec: float = 8.0) -> Optional[dict]:
    deadline = time.time() + timeout_sec
    url = f"http://127.0.0.1:{port}/api/v1/status"
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=1.5) as resp:
                if resp.status == 200:
                    return json.loads(resp.read().decode("utf-8"))
        except Exception:
            time.sleep(0.5)
    return None


def start_or_install_server(
    venv_python: Path,
    primary_lib: Path,
    extra_libs: List[Path],
    server_port: int,
) -> None:
    os_name = platform.system()
    extra_env_val = ":".join(str(p) for p in extra_libs) if extra_libs else ""

    if os_name == "Linux" and shutil.which("systemctl"):
        if ask_yes_no("    Install and start LocaLeShop as a background systemd service?", default_yes=True):
            user_name = os.environ.get("SUDO_USER") or os.environ.get("USER") or "root"
            env_line = f'Environment="LOCALESHOP_EXTRA_DIRS={extra_env_val}"\n' if extra_env_val else ""
            unit_content = (
                "[Unit]\n"
                "Description=LocaLeShop LAN WUX/WUD/WUP Catalog & Streamer\n"
                "After=network.target\n\n"
                "[Service]\n"
                "Type=simple\n"
                f"User={user_name}\n"
                f"WorkingDirectory={SERVER_DIR}\n"
                f'ExecStart="{venv_python}" "{SERVER_DIR / "server.py"}" --library "{primary_lib}" --host 0.0.0.0 --port {server_port}\n'
                "Restart=always\n"
                "RestartSec=3\n"
                "Environment=PYTHONUNBUFFERED=1\n"
                f"{env_line}\n"
                "[Install]\n"
                "WantedBy=multi-user.target\n"
            )
            service_path = "/etc/systemd/system/localeshop.service"
            try:
                proc = subprocess.run(
                    ["sudo", "tee", service_path],
                    input=unit_content.encode("utf-8"),
                    stdout=subprocess.DEVNULL,
                    check=True,
                )
                if proc.returncode == 0:
                    subprocess.run(["sudo", "systemctl", "daemon-reload"], check=True)
                    subprocess.run(["sudo", "systemctl", "enable", "localeshop.service"], check=True)
                    subprocess.run(["sudo", "systemctl", "restart", "localeshop.service"], check=True)
                    status = wait_for_server_health(server_port)
                    if status:
                        print(
                            f"    {GREEN}systemd service active! Indexed {status.get('title_count', 0)} title(s).{NC}"
                        )
                        return
            except Exception as exc:
                print(f"    {YELLOW}Could not install systemd service ({exc}); starting in background instead.{NC}")

    elif os_name == "Darwin":
        if ask_yes_no("    Install LocaLeShop as a macOS background LaunchAgent?", default_yes=True):
            plist_dir = Path.home() / "Library" / "LaunchAgents"
            plist_dir.mkdir(parents=True, exist_ok=True)
            plist_path = plist_dir / "com.localeshop.server.plist"
            plist_content = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.localeshop.server</string>
    <key>ProgramArguments</key>
    <array>
        <string>{venv_python}</string>
        <string>{SERVER_DIR / "server.py"}</string>
        <string>--library</string>
        <string>{primary_lib}</string>
        <string>--host</string>
        <string>0.0.0.0</string>
        <string>--port</string>
        <string>{server_port}</string>
    </array>
    <key>WorkingDirectory</key>
    <string>{SERVER_DIR}</string>
    <key>EnvironmentVariables</key>
    <dict>
        <key>LOCALESHOP_EXTRA_DIRS</key>
        <string>{extra_env_val}</string>
    </dict>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
</dict>
</plist>
"""
            plist_path.write_text(plist_content, encoding="utf-8")
            subprocess.run(["launchctl", "unload", str(plist_path)], stderr=subprocess.DEVNULL)
            subprocess.run(["launchctl", "load", str(plist_path)], check=False)
            status = wait_for_server_health(server_port)
            if status:
                print(
                    f"    {GREEN}macOS LaunchAgent active! Indexed {status.get('title_count', 0)} title(s).{NC}"
                )
                return

    # Universal background process launch (Windows, macOS, Linux fallback)
    if ask_yes_no("    Start the LocaLeShop server in the background now?", default_yes=True):
        env = os.environ.copy()
        if extra_libs:
            sep = ";" if os_name == "Windows" else ":"
            env["LOCALESHOP_EXTRA_DIRS"] = sep.join(str(p) for p in extra_libs)

        cmd = [
            str(venv_python),
            str(SERVER_DIR / "server.py"),
            "--library",
            str(primary_lib),
            "--host",
            "0.0.0.0",
            "--port",
            str(server_port),
        ]
        log_file = SERVER_DIR / "localeshop_server.log"
        log_fp = log_file.open("ab")
        if os_name == "Windows":
            creation_flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0x00000200) | getattr(
                subprocess, "DETACHED_PROCESS", 0x00000008
            )
            subprocess.Popen(
                cmd,
                cwd=str(SERVER_DIR),
                env=env,
                stdout=log_fp,
                stderr=log_fp,
                creationflags=creation_flags,
            )
        else:
            subprocess.Popen(
                cmd,
                cwd=str(SERVER_DIR),
                env=env,
                stdout=log_fp,
                stderr=log_fp,
                start_new_session=True,
            )

        status = wait_for_server_health(server_port)
        if status:
            print(
                f"    {GREEN}Server is running at http://127.0.0.1:{server_port} "
                f"({status.get('title_count', 0)} title(s) indexed)!{NC}"
            )
        else:
            print(f"    {YELLOW}Server started in background (logs: {log_file}).{NC}")


# ---------------------------------------------------------------------------
# Pre-built Client (.wuhb) Resolution & Optional Builder
# ---------------------------------------------------------------------------

def locate_or_obtain_wuhb(force_rebuild: bool = False) -> Optional[Path]:
    """
    Find LocaLeShop.wuhb without requiring end-users to compile C code:
      1. Existing local pre-built binary (client/build/LocaLeShop.wuhb, client/LocaLeShop.wuhb, ./LocaLeShop.wuhb)
      2. Pre-built release asset from GitHub Releases
      3. Optional automatic Docker or devkitPro build if requested
    """
    candidates = [
        CLIENT_DIR / "build" / "LocaLeShop.wuhb",
        CLIENT_DIR / "LocaLeShop.wuhb",
        ROOT_DIR / "LocaLeShop.wuhb",
        ROOT_DIR / "out" / "Aroma-DEBUG" / "WiiU" / "apps" / "LocaLeShop.wuhb",
    ]

    if not force_rebuild:
        for cand in candidates:
            if cand.is_file() and cand.stat().st_size > 100_000:
                print(f"    Found pre-built client binary: {GREEN}{cand}{NC}")
                return cand

        # Try fetching pre-compiled LocaLeShop.wuhb from GitHub Releases
        print("    Checking GitHub Releases for pre-built LocaLeShop.wuhb...")
        try:
            req = urllib.request.Request(
                GITHUB_RELEASES_API,
                headers={"User-Agent": "LocaLeShop-Setup/1.1"},
            )
            with urllib.request.urlopen(req, timeout=6) as resp:
                release_data = json.loads(resp.read().decode("utf-8"))
                for asset in release_data.get("assets", []):
                    name = asset.get("name", "")
                    dl_url = asset.get("browser_download_url", "")
                    if name.lower().endswith(".wuhb") and dl_url:
                        dest = CLIENT_DIR / "build" / "LocaLeShop.wuhb"
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        print(f"    Downloading {name} from GitHub Releases...")
                        urllib.request.urlretrieve(dl_url, str(dest))
                        if dest.is_file() and dest.stat().st_size > 100_000:
                            print(f"    {GREEN}Downloaded pre-built LocaLeShop.wuhb!{NC}")
                            return dest
        except Exception:
            pass

    # Fallback: build via Docker (native or WSL on Windows) if available
    print("    No pre-built LocaLeShop.wuhb found locally; checking for Docker to build automatically...")
    out_wuhb = CLIENT_DIR / "build" / "LocaLeShop.wuhb"
    out_wuhb.parent.mkdir(parents=True, exist_ok=True)

    dockerfile_inline = (
        "FROM devkitpro/devkitppc:20260503\n"
        "COPY --from=ghcr.io/wiiu-env/libmocha:20260331 /artifacts /opt/devkitpro\n"
    )

    if shutil.which("docker"):
        try:
            subprocess.run(
                ["docker", "build", "-t", "localeshop-builder", "-"],
                input=dockerfile_inline.encode("utf-8"),
                check=True,
            )
            subprocess.run(
                [
                    "docker",
                    "run",
                    "--rm",
                    "-v",
                    f"{CLIENT_DIR}:/client",
                    "localeshop-builder",
                    "sh",
                    "-c",
                    "mkdir -p /tmp/build && cd /tmp/build && cmake /client && make -j4 && mkdir -p /client/build && cp LocaLeShop.wuhb /client/build/LocaLeShop.wuhb",
                ],
                check=True,
            )
            if out_wuhb.is_file():
                print(f"    {GREEN}Built client binary: {out_wuhb}{NC}")
                return out_wuhb
        except Exception as exc:
            print(f"    {YELLOW}Docker build failed: {exc}{NC}")

    elif platform.system() == "Windows" and shutil.which("wsl"):
        try:
            wsl_client = subprocess.check_output(
                ["wsl", "-e", "wslpath", "-a", str(CLIENT_DIR)],
                text=True,
            ).strip()
            subprocess.run(
                [
                    "wsl",
                    "-e",
                    "sh",
                    "-c",
                    f"printf '{dockerfile_inline}' | docker build -t localeshop-builder - && "
                    f"docker run --rm -v '{wsl_client}:/client' localeshop-builder sh -c "
                    "'mkdir -p /tmp/build && cd /tmp/build && cmake /client && make -j4 && mkdir -p /client/build && cp LocaLeShop.wuhb /client/build/LocaLeShop.wuhb'",
                ],
                check=True,
            )
            if out_wuhb.is_file():
                print(f"    {GREEN}Built client binary via WSL Docker: {out_wuhb}{NC}")
                return out_wuhb
        except Exception as exc:
            print(f"    {YELLOW}WSL Docker build failed: {exc}{NC}")

    return None


# ---------------------------------------------------------------------------
# Wii U Deployment (Network FTP or Local SD Card)
# ---------------------------------------------------------------------------

def deploy_to_wiiu_ftp(
    wiiu_ip: str,
    wiiu_port: int,
    wuhb_path: Path,
    config_bytes: bytes,
) -> bool:
    print(f"    Connecting to Wii U FTP at {wiiu_ip}:{wiiu_port}...")
    ftp = ftplib.FTP()
    try:
        ftp.connect(wiiu_ip, wiiu_port, timeout=10)
        ftp.login()

        # Detect whether ftpiiu uses /fs/vol/external01 or /vol/external01
        sd_root = "/fs/vol/external01"
        try:
            ftp.cwd("/fs/vol/external01")
        except ftplib.error_perm:
            sd_root = "/vol/external01"
            ftp.cwd(sd_root)

        for sub in ("wiiu", "wiiu/apps", "wiiu/apps/LocaLeShop"):
            target_dir = f"{sd_root}/{sub}"
            try:
                ftp.mkd(target_dir)
            except ftplib.error_perm:
                pass

        app_dir = f"{sd_root}/wiiu/apps/LocaLeShop"
        ftp.cwd(app_dir)

        print(f"    Uploading config.json to {app_dir}/config.json...")
        ftp.storbinary("STOR config.json", io.BytesIO(config_bytes))

        print(f"    Uploading LocaLeShop.wuhb ({wuhb_path.stat().st_size:,} bytes)...")
        with wuhb_path.open("rb") as fp:
            ftp.storbinary("STOR LocaLeShop.wuhb", fp, blocksize=65536)

        ftp.quit()
        return True
    except Exception as exc:
        print(f"    {RED}FTP upload failed: {exc}{NC}")
        return False


def deploy_to_sdcard_path(
    sd_root: Path,
    wuhb_path: Path,
    config_bytes: bytes,
) -> bool:
    try:
        app_dir = sd_root / "wiiu" / "apps" / "LocaLeShop"
        app_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(wuhb_path, app_dir / "LocaLeShop.wuhb")
        (app_dir / "config.json").write_bytes(config_bytes)
        print(f"    {GREEN}Copied LocaLeShop.wuhb and config.json to {app_dir}{NC}")
        return True
    except OSError as exc:
        print(f"    {RED}Failed to copy files to SD card: {exc}{NC}")
        return False


# ---------------------------------------------------------------------------
# Main Interactive Wizard
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="LocaLeShop Cross-Platform Setup Wizard (Windows, macOS, Linux)."
    )
    parser.add_argument(
        "--rebuild-client",
        action="store_true",
        help="Force rebuilding LocaLeShop.wuhb from source via Docker.",
    )
    args = parser.parse_args()

    print(f"{CYAN}{BOLD}")
    print("===========================================================")
    print("   LocaLeShop Easy Setup Wizard (Windows / macOS / Linux)  ")
    print("===========================================================")
    print(f"{NC}")

    # 1. Server Host & Port
    detected_ip = detect_lan_ip()
    print(f"{BOLD}[1/3] Server Configuration{NC}")
    is_local = ask_yes_no("Will this computer host your Wii U games (LocaLeShop server)?", default_yes=True)
    if is_local:
        server_ip = ask("Confirm this computer's LAN IP address for your Wii U", default=detected_ip)
    else:
        server_ip = ask("Enter the LAN IP address of your LocaLeShop server", default="10.0.0.99")

    port_str = ask("Enter LocaLeShop server port", default="8000")
    try:
        server_port = int(port_str)
    except ValueError:
        server_port = 8000

    # 2. Wii U Titles Directory
    primary_lib: Optional[Path] = None
    extra_libs: List[Path] = []
    if is_local:
        print(f"\n{BOLD}[2/3] Wii U Game Library Location{NC}")
        print("  Point LocaLeShop to the folder containing your .wux + .key files,")
        print("  .wud + .key files, or extracted WUP folders (you can drag & drop the folder here).")
        default_lib = str(SERVER_DIR / "library")
        while True:
            raw_dir = ask("Path to your Wii U titles folder", default=default_lib)
            candidate = clean_user_path(raw_dir)
            if candidate.is_dir():
                primary_lib = candidate
                break
            print(f"  {YELLOW}Folder '{candidate}' does not exist.{NC}")
            if ask_yes_no("  Create this folder now?", default_yes=True):
                candidate.mkdir(parents=True, exist_ok=True)
                primary_lib = candidate
                break

        if ask_yes_no("Do you have a second folder of Wii U titles/updates to include?", default_yes=False):
            raw_extra = ask("Path to additional Wii U titles folder")
            if raw_extra:
                extra_cand = clean_user_path(raw_extra)
                if extra_cand.is_dir() and extra_cand != primary_lib:
                    extra_libs.append(extra_cand)

        all_dirs = [primary_lib] + extra_libs
        wux_n, wud_n, wup_n = preview_library_titles(all_dirs)
        print(
            f"  {GREEN}Library scan preview: {wux_n} WUX disc(s), "
            f"{wud_n} WUD disc(s), {wup_n} extracted WUP package(s).{NC}"
        )

    # 3. Wii U Console Deployment Method
    print(f"\n{BOLD}[3/3] Wii U Console Setup{NC}")
    print("  How would you like to install the LocaLeShop app onto your Wii U?")
    print("    [1] Over Wi-Fi / LAN (Wii U is powered on with Aroma ftpiiu running)")
    print("    [2] Directly to an SD card plugged into this computer")
    print("    [3] Skip Wii U deployment (only set up the server)")
    deploy_choice = ask("Choose an option (1/2/3)", default="1").strip()

    wiiu_ip = ""
    wiiu_ftp_port = 21
    sd_card_path: Optional[Path] = None

    if deploy_choice == "1":
        wiiu_ip = ask("Enter your Wii U console's IP address", default="10.0.0.218")
        ftp_port_str = ask("Enter your Wii U FTP port", default="21")
        try:
            wiiu_ftp_port = int(ftp_port_str)
        except ValueError:
            wiiu_ftp_port = 21
    elif deploy_choice == "2":
        while True:
            raw_sd = ask("Enter the path/drive letter of your Wii U SD card (e.g. E:\\ or /Volumes/WIIU)")
            if raw_sd:
                cand_sd = clean_user_path(raw_sd)
                if cand_sd.is_dir():
                    sd_card_path = cand_sd
                    break
                print(f"  {YELLOW}Path '{cand_sd}' is not a valid directory.{NC}")

    # Summary
    print(f"\n{CYAN}Setup Summary:{NC}")
    print(f"  Server Address  : {BOLD}http://{server_ip}:{server_port}{NC}")
    if primary_lib:
        print(f"  Primary Library : {BOLD}{primary_lib}{NC}")
    if extra_libs:
        print(f"  Extra Libraries : {BOLD}{', '.join(str(p) for p in extra_libs)}{NC}")
    if deploy_choice == "1":
        print(f"  Wii U Target    : {BOLD}FTP -> {wiiu_ip}:{wiiu_ftp_port}{NC}")
    elif deploy_choice == "2" and sd_card_path:
        print(f"  Wii U Target    : {BOLD}SD Card -> {sd_card_path}{NC}")
    else:
        print(f"  Wii U Target    : {BOLD}Skipped (Server Only){NC}")
    print("")

    if not ask_yes_no("Proceed with setup?", default_yes=True):
        print(f"{YELLOW}Setup cancelled.{NC}")
        return

    # Execute Server Setup
    if is_local and primary_lib:
        print(f"\n{CYAN}[+] Setting up LocaLeShop Server...{NC}")
        venv_python = setup_server_environment()
        launcher = create_server_launcher_scripts(venv_python, primary_lib, extra_libs, server_port)
        print(f"    Created quick-start launcher: {GREEN}{launcher.name}{NC}")
        start_or_install_server(venv_python, primary_lib, extra_libs, server_port)

    # Execute Wii U Client Deployment
    if deploy_choice in ("1", "2"):
        print(f"\n{CYAN}[+] Preparing Wii U Client & Configuration...{NC}")
        config_payload = json.dumps(
            {
                "server_ip": server_ip,
                "server_port": server_port,
                "install_target": "usb",
            },
            indent=2,
        ).encode("utf-8")

        wuhb_path = locate_or_obtain_wuhb(force_rebuild=args.rebuild_client)
        if not wuhb_path:
            print(
                f"    {RED}Could not find or build LocaLeShop.wuhb.{NC}\n"
                f"    Place LocaLeShop.wuhb in '{CLIENT_DIR / 'build' / 'LocaLeShop.wuhb'}' and re-run setup."
            )
            sys.exit(1)

        if deploy_choice == "1":
            if deploy_to_wiiu_ftp(wiiu_ip, wiiu_ftp_port, wuhb_path, config_payload):
                print(f"\n{GREEN}{BOLD}===========================================================")
                print("   LocaLeShop Setup Complete!                              ")
                print(f"==========================================================={NC}")
                print(f"  Server URL : {GREEN}http://{server_ip}:{server_port}{NC}")
                print(f"  Wii U App  : {CYAN}sd:/wiiu/apps/LocaLeShop/LocaLeShop.wuhb{NC}")
                print("\nYou can now launch LocaLeShop directly from your Wii U Menu!")
            else:
                local_cfg = CLIENT_DIR / "build" / "config.json"
                local_cfg.write_bytes(config_payload)
                print(f"\n{YELLOW}Saved your ready-to-copy files locally:{NC}")
                print(f"  1. {wuhb_path}")
                print(f"  2. {local_cfg}")
                print("Copy both files into sd:/wiiu/apps/LocaLeShop/ on your Wii U SD card.")
        elif deploy_choice == "2" and sd_card_path:
            if deploy_to_sdcard_path(sd_card_path, wuhb_path, config_payload):
                print(f"\n{GREEN}{BOLD}===========================================================")
                print("   LocaLeShop Setup Complete!                              ")
                print(f"==========================================================={NC}")
                print("Insert the SD card back into your Wii U and launch LocaLeShop from the Wii U Menu!")


if __name__ == "__main__":
    main()
