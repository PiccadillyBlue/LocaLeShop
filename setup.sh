#!/usr/bin/env bash
set -e

# --- Terminal Styling ---
BOLD='\033[1m'
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="${ROOT_DIR}/server"
CLIENT_DIR="${ROOT_DIR}/client"

clear
echo -e "${CYAN}${BOLD}"
echo "==========================================================="
echo "   LocaLeShop Automated Setup & Deployment Wizard          "
echo "==========================================================="
echo -e "${NC}"

# -----------------------------------------------------------------------------
# 1. Gather Configuration
# -----------------------------------------------------------------------------

# Detect host LAN IP
HOST_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}')
if [ -z "$HOST_IP" ]; then
    HOST_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
fi
HOST_IP=${HOST_IP:-"127.0.0.1"}

# Prompt: Server Host
echo -e "${BOLD}[1/4] Server Host Configuration${NC}"
read -rp "Is the LocaLeShop server hosted on this machine? [Y/n]: " IS_LOCAL
IS_LOCAL=${IS_LOCAL:-"y"}

if [[ "$IS_LOCAL" =~ ^[Yy]$ ]]; then
    SERVER_IP="$HOST_IP"
    echo -e "      Using local host IP: ${GREEN}${SERVER_IP}${NC}"
else
    read -rp "Enter remote LocaLeShop server IP: " SERVER_IP
fi

read -rp "Enter server port [8000]: " SERVER_PORT
SERVER_PORT=${SERVER_PORT:-8000}

# Prompt: WUP Packages Storage Location (if running locally)
WUP_DIR=""
if [[ "$IS_LOCAL" =~ ^[Yy]$ ]]; then
    echo -e "\n${BOLD}[2/4] WUP Storage Directory${NC}"
    DEFAULT_WUP_DIR="${SERVER_DIR}/packages"
    while true; do
        read -rp "Enter absolute path to your WUP titles directory [${DEFAULT_WUP_DIR}]: " USER_WUP_DIR
        USER_WUP_DIR=${USER_WUP_DIR:-$DEFAULT_WUP_DIR}
        
        # Expand tilde if present
        USER_WUP_DIR="${USER_WUP_DIR/#\~/$HOME}"

        if [ -d "$USER_WUP_DIR" ]; then
            WUP_DIR="$USER_WUP_DIR"
            echo -e "      Found valid directory: ${GREEN}${WUP_DIR}${NC}"
            break
        else
            echo -e "      ${YELLOW}Directory does not exist.${NC}"
            read -rp "      Would you like to create it now? [y/N]: " MKDIR_CONFIRM
            if [[ "$MKDIR_CONFIRM" =~ ^[Yy]$ ]]; then
                mkdir -p "$USER_WUP_DIR"
                WUP_DIR="$USER_WUP_DIR"
                echo -e "      Created directory: ${GREEN}${WUP_DIR}${NC}"
                break
            fi
        fi
    done
fi

# Prompt: Target Wii U IP
echo -e "\n${BOLD}[3/4] Target Console Configuration${NC}"
read -rp "Enter Wii U network IP address [10.0.0.77]: " WIIU_IP
WIIU_IP=${WIIU_IP:-"10.0.0.77"}
read -rp "Enter Wii U FTP port [21]: " WIIU_FTP_PORT
WIIU_FTP_PORT=${WIIU_FTP_PORT:-21}

echo -e "\n${CYAN}Configuration Summary:${NC}"
echo -e "  Server Endpoint : ${BOLD}http://${SERVER_IP}:${SERVER_PORT}${NC}"
if [ -n "$WUP_DIR" ]; then
    echo -e "  WUP Repository  : ${BOLD}${WUP_DIR}${NC}"
fi
echo -e "  Target Console  : ${BOLD}${WIIU_IP}:${WIIU_FTP_PORT}${NC}"
echo ""
read -rp "Proceed with automated configuration and deployment? [Y/n]: " PROCEED
if [[ ! "$PROCEED" =~ ^[Yy]?$ ]]; then
    echo -e "${RED}Aborted by user.${NC}"
    exit 0
fi

# -----------------------------------------------------------------------------
# 2. Local Server Setup & Systemd Provisioning
# -----------------------------------------------------------------------------
if [[ "$IS_LOCAL" =~ ^[Yy]$ ]]; then
    echo -e "\n${CYAN}[+] Provisioning Local Server & Systemd Service...${NC}"
    
    # Symlink packages folder into server root if non-default
    if [ "$WUP_DIR" != "${SERVER_DIR}/packages" ]; then
        ln -sfn "$WUP_DIR" "${SERVER_DIR}/packages"
        echo "    Linked ${WUP_DIR} -> ${SERVER_DIR}/packages"
    fi

    # Create systemd service unit
    SERVICE_FILE="/etc/systemd/system/localeshop.service"
    echo "    Writing ${SERVICE_FILE}..."
    sudo tee "$SERVICE_FILE" > /dev/null << SYSTEMD_EOF
[Unit]
Description=LocaLeShop LAN WUP Catalog & Streamer
After=network.target

[Service]
Type=simple
User=$USER
WorkingDirectory=${SERVER_DIR}
ExecStart=/usr/bin/python3 ${SERVER_DIR}/server.py
Restart=always
RestartSec=3
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
SYSTEMD_EOF

    echo "    Reloading and restarting localeshop.service..."
    sudo systemctl daemon-reload
    sudo systemctl enable localeshop.service
    sudo systemctl restart localeshop.service

    # Health check
    sleep 1
    if curl -s -f "http://127.0.0.1:${SERVER_PORT}/api/v1/titles" > /dev/null; then
        echo -e "    ${GREEN}Server running and responding at http://127.0.0.1:${SERVER_PORT}${NC}"
    else
        echo -e "    ${YELLOW}Warning: Server service started, but health check failed at http://127.0.0.1:${SERVER_PORT}.${NC}"
    fi
fi

# -----------------------------------------------------------------------------
# 3. Patch Client Source with Target Server IP
# -----------------------------------------------------------------------------
echo -e "\n${CYAN}[+] Updating Client Configuration...${NC}"

python3 - << PY_EOF
import re

main_c = "${CLIENT_DIR}/src/main.c"
with open(main_c, "r", encoding="utf-8") as f:
    code = f.read()

# Update DEFAULT_SERVER_IP define if present
code = re.sub(
    r'(#define\s+DEFAULT_SERVER_IP\s+")[^"]*(")',
    r'\g<1>${SERVER_IP}\g<2>',
    code
)

# Update hardcoded fallback IP in init/fetch if present
code = re.sub(
    r'(strncpy\(\s*ctx->server_ip,\s*")[^"]*(")',
    r'\g<1>${SERVER_IP}\g<2>',
    code
)

# Update DEFAULT_SERVER_PORT if defined
code = re.sub(
    r'(#define\s+DEFAULT_SERVER_PORT\s+)\d+',
    r'\g<1>${SERVER_PORT}',
    code
)

with open(main_c, "w", encoding="utf-8") as f:
    f.write(code)

print("    Applied Server IP (${SERVER_IP}) and Port (${SERVER_PORT}) to src/main.c")
PY_EOF

# -----------------------------------------------------------------------------
# 4. Build Client via Docker devkitPPC
# -----------------------------------------------------------------------------
echo -e "\n${CYAN}[+] Building Client Binary via Docker...${NC}"
mkdir -p "${CLIENT_DIR}/build"

sudo docker run --rm -v "${CLIENT_DIR}:/source" -w /source/build devkitpro/devkitppc:latest /bin/bash -c "
    set -e
    if [ ! -f Makefile ]; then
        cmake -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/wut/share/wut.toolchain.cmake ..
    fi
    make -j\$(nproc)
"

if [ ! -f "${CLIENT_DIR}/build/LocaLeShop.wuhb" ]; then
    echo -e "${RED}[!] Build failed: LocaLeShop.wuhb not found.${NC}"
    exit 1
fi
echo -e "    ${GREEN}Client build succeeded: build/LocaLeShop.wuhb${NC}"

# -----------------------------------------------------------------------------
# 5. Push Binary to Wii U over FTP
# -----------------------------------------------------------------------------
echo -e "\n${CYAN}[+] Deploying to Wii U over FTP (${WIIU_IP}:${WIIU_FTP_PORT})...${NC}"

WUHB_PATH="${CLIENT_DIR}/build/LocaLeShop.wuhb"
TARGET_FTP_URL="ftp://${WIIU_IP}:${WIIU_FTP_PORT}/fs/vol/external01/wiiu/apps/LocaLeShop/LocaLeShop.wuhb"

# Ensure the destination directory exists over FTP
curl -s --connect-timeout 5 "ftp://${WIIU_IP}:${WIIU_FTP_PORT}/fs/vol/external01/wiiu/apps/" \
    -Q "MKD /fs/vol/external01/wiiu/apps/LocaLeShop" > /dev/null 2>&1 || true

if curl --connect-timeout 8 -T "$WUHB_PATH" "$TARGET_FTP_URL"; then
    echo -e "\n${GREEN}${BOLD}==========================================================="
    echo "   Installation & Deployment Complete!                    "
    echo "===========================================================${NC}"
    echo -e "  Server Status : ${GREEN}Active${NC} (http://${SERVER_IP}:${SERVER_PORT})"
    echo -e "  Client Binary : Uploaded to ${CYAN}/wiiu/apps/LocaLeShop/LocaLeShop.wuhb${NC}"
    echo -e "\nYou can now launch ${BOLD}LocaLeShop${NC} on your Wii U GamePad."
else
    echo -e "\n${RED}[!] FTP upload failed. Verify the Wii U is powered on and running Mocha/Aroma FTP.${NC}"
    echo "    Manual upload target:"
    echo "    curl -T ${WUHB_PATH} ${TARGET_FTP_URL}"
fi
