/**
 * LocaLeShop Wii U Homebrew Client (.wuhb)
 * ----------------------------------------
 * Connects to the self-hosted LocaLeShop LAN daemon (with UDP auto-discovery
 * and in-app IP/Port settings), renders an eShop-inspired Aero Glass GamePad UI
 * via SDL2 + SDL2_ttf, streams WUX/WUD/WUP package files to the console via
 * libcurl (with 3 MB somemopt socket buffer pool + HTTP 206 resume), and
 * installs titles to playable Wii U Home Screen tiles on USB or NAND via MCP.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

/* Wii U Toolchain (wut) & Mocha headers */
#include <coreinit/filesystem.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <mocha/mocha.h>
#include <nsysnet/_socket.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

/* Portlibs headers */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <jansson.h>

/* --------------------------------------------------------------------------
 * Compatibility Shims for WUT MCP API Variations
 * -------------------------------------------------------------------------- */
#ifndef MCP_INSTALL_TARGET_MLC
#define MCP_INSTALL_TARGET_MLC 0
#endif
#ifndef MCP_INSTALL_TARGET_USB
#define MCP_INSTALL_TARGET_USB 1
#endif

static inline MCPError LocaLeShop_MCP_InstallSetTarget(int mcp_fd, int target, int usb_index)
{
    MCPError err = MCP_InstallSetTargetDevice(mcp_fd, (MCPInstallTarget)target);
    if(err >= 0 && target == MCP_INSTALL_TARGET_USB)
    {
        err = MCP_InstallSetTargetUsb(mcp_fd, (usb_index == 2) ? 2 : 1);
    }
    return err;
}

static inline MCPError LocaLeShop_MCP_InstallTitleAsync(int mcp_fd, const char *path)
{
    static MCPInstallTitleInfo s_installInfo __attribute__((__aligned__(0x40)));
    memset(&s_installInfo, 0, sizeof(s_installInfo));
    return MCP_InstallTitleAsync(mcp_fd, path, &s_installInfo);
}

#define MCP_InstallSetTarget(fd, target, usb_idx) LocaLeShop_MCP_InstallSetTarget((fd), (target), (usb_idx))
#define MCP_InstallTitleAsync(fd, path)           LocaLeShop_MCP_InstallTitleAsync((fd), (path))

/* --------------------------------------------------------------------------
 * Configuration & Constants
 * -------------------------------------------------------------------------- */
#define SCREEN_WIDTH           1280
#define SCREEN_HEIGHT          720

#define DEFAULT_SERVER_IP      "192.168.1.100"
#define DEFAULT_SERVER_PORT    8000
#define CONFIG_DIR_PATH        "/vol/external01/wiiu/apps/LocaLeShop"
#define CONFIG_FILE_PATH       "/vol/external01/wiiu/apps/LocaLeShop/config.json"
#define SD_STAGE_BASE_DIR      "/vol/external01/install"
#define FSA_STAGE_BASE_DIR     "/vol/external01/install"
#define MCP_STAGE_BASE_DIR     "/vol/app_sd/install"

#define ROMFS_FONT_SEMIBOLD    "/vol/content/static/Orbitron-SemiBold.ttf"
#define ROMFS_FONT_REGULAR     "/vol/content/static/Orbitron-Regular.ttf"
#define ROMFS_FONT_VARIABLE    "/vol/content/Orbitron-VariableFont_wght.ttf"
#define SYSTEM_FONT_PRIMARY    "/vol/content/font/CafeStd.ttf"
#define SYSTEM_FONT_FALLBACK_1 "/vol/external01/wiiu/apps/LocaLeShop/font.ttf"
#define SYSTEM_FONT_FALLBACK_2 "/vol/storage_mlc01/sys/title/0005001b/10042400/content/CafeStd.ttf"

#define SOCKET_POOL_SIZE       0x300000 /* 3 MiB somemopt donation for fast TCP window */
#define SOCKET_BUFSIZE         0x40000 /* 256 KiB per-socket receive buffer */
#define FILE_WRITE_BUFFER_SIZE (128 * 1024)
#define MAX_FILE_RETRIES       3

#define MAX_TITLES             512
#define MAX_FILES_PER_TITLE    256
#define MAX_QUEUE_LEN          128
#define MAX_PATH_LEN           512
#define MAX_URL_LEN            768

#define VISIBLE_CARDS          5
#define CARD_MARGIN_X          48
#define CARD_START_Y           138
#define CARD_HEIGHT            78
#define CARD_GAP_Y             10
#define CARD_CORNER_RADIUS     12

#define DISCOVERY_REQ_MAGIC    "LOCALESHOP_DISCOVER_V1"
#define DISCOVERY_RES_MAGIC    "LOCALESHOP_SERVER_V1"

/* --------------------------------------------------------------------------
 * Data Structures
 * -------------------------------------------------------------------------- */

typedef enum
{
    CAT_TAB_ALL = 0,
    CAT_TAB_GAMES,
    CAT_TAB_UPDATES,
    CAT_TAB_DLC,
    CAT_TAB_DEMOS,
    CAT_TAB_COUNT
} CategoryTab;

typedef struct
{
    char title_id[24];
    uint64_t tid_u64;
    char name[128];
    char region[16];
    char category[24];
    char product_code[24];
    char source_format[12];
    uint32_t version;
    uint64_t total_size_bytes;
    int file_count;
    char **file_names;
    char **file_urls;
    bool is_installed;
} TitleMetadata;

typedef enum
{
    APP_STATE_BROWSING = 0,
    APP_STATE_DETAILS_MODAL,
    APP_STATE_SETTINGS_MODAL,
    APP_STATE_DOWNLOADING,
    APP_STATE_INSTALLING,
    APP_STATE_COMPLETED,
    APP_STATE_ERROR,
    APP_STATE_CANCELLED
} AppState;

typedef struct
{
    uint8_t *data;
    size_t size;
    size_t capacity;
} MemoryBuffer;

typedef struct
{
    FILE *fp;
    char file_Buf[FILE_WRITE_BUFFER_SIZE];
    uint64_t bytes_written;
    bool write_error;
} StreamFileContext;

typedef struct
{
    /* Server & target configuration */
    char server_ip[64];
    int server_port;
    int install_target; /* MCP_INSTALL_TARGET_USB or MCP_INSTALL_TARGET_MLC */

    /* In-app IP/Port settings modal state */
    int edit_octets[4];
    int edit_port;
    int edit_field; /* 0..3 = IPv4 octets, 4 = port */

    /* SDL2 rendering resources */
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font_title;
    TTF_Font *font_body;
    TTF_Font *font_small;

    /* Catalog, category filter & selection state */
    TitleMetadata titles[MAX_TITLES];
    int title_count;
    CategoryTab active_tab;
    int filtered_indices[MAX_TITLES];
    int filtered_count;
    int selected_pos;
    int scroll_offset;

    /* Multi-title download & install queue (indices into ctx->titles) */
    int queue[MAX_QUEUE_LEN];
    int queue_count;
    int queue_total_batch;
    int queue_completed_batch;

    /* Operational state machine */
    AppState state;
    char status_message[512];

    /* Non-blocking libcurl download pipeline */
    CURLM *curl_multi;
    CURL *curl_easy;
    StreamFileContext stream_ctx;
    int dl_title_index;
    int dl_file_index;
    int dl_retry_count;
    uint64_t dl_completed_files_bytes;
    uint64_t dl_current_file_bytes;
    uint64_t dl_resume_offset;
    uint64_t dl_total_bytes;
    double dl_speed_bps;
    OSTick dl_last_speed_tick;
    uint64_t dl_last_speed_bytes;
    char dl_current_filename[128];
    char dl_current_filepath[MAX_PATH_LEN];
    char dl_stage_dir[MAX_PATH_LEN];

    /* Non-blocking MCP installation pipeline */
    int mcp_fd;
    MCPInstallProgress mcp_progress __attribute__((__aligned__(0x40)));
    bool mcp_started_flag;

    /* FSA & Mocha unlocked storage management */
    FSAClientHandle fsa_client;
    bool mocha_ready;
    bool fsa_unlocked;
    bool app_sd_mounted;
    bool usb_mounted;
    int usb_index; /* 1 = /vol/storage_usb01/, 2 = /vol/storage_usb02/ */
    char usb_fsa_path[64];
    uint64_t usb_free_bytes;
    uint64_t nand_free_bytes;
    uint64_t sd_free_bytes;
    int iosu_patch_count;
    uint32_t iosu_orig_values[6];
} AppContext;

/* --------------------------------------------------------------------------
 * TCP Socket Tuning Callback
 * -------------------------------------------------------------------------- */

static int curl_sockopt_cb(void *clientp, curl_socket_t curlfd, curlsocktype purpose)
{
    (void)clientp;
    if(purpose != CURLSOCKTYPE_IPCXN)
    {
        return CURL_SOCKOPT_OK;
    }

    int one = 1;
    setsockopt(curlfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return CURL_SOCKOPT_OK;
}

/* --------------------------------------------------------------------------
 * Filesystem, Free Space & Formatting Utilities
 * -------------------------------------------------------------------------- */

static void format_bytes(uint64_t bytes, char *out, size_t out_size)
{
    const double kib = 1024.0;
    const double mib = kib * 1024.0;
    const double gib = mib * 1024.0;

    if(bytes >= (uint64_t)gib)
    {
        snprintf(out, out_size, "%.2f GiB", (double)bytes / gib);
    }
    else if(bytes >= (uint64_t)mib)
    {
        snprintf(out, out_size, "%.1f MiB", (double)bytes / mib);
    }
    else if(bytes >= (uint64_t)kib)
    {
        snprintf(out, out_size, "%.1f KiB", (double)bytes / kib);
    }
    else
    {
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    }
}

static bool fsa_dir_exists(FSAClientHandle fsa_client, const char *fsa_path)
{
    if(fsa_client <= 0)
    {
        return false;
    }
    FSAStat st;
    return FSAGetStat(fsa_client, fsa_path, &st) == FS_ERROR_OK && (st.flags & FS_STAT_DIRECTORY);
}

static void refresh_storage_space(AppContext *ctx)
{
    ctx->usb_mounted = false;
    ctx->usb_index = 1;
    ctx->usb_free_bytes = 0;
    ctx->nand_free_bytes = 0;
    ctx->sd_free_bytes = 0;
    strncpy(ctx->usb_fsa_path, "/vol/storage_usb01/", sizeof(ctx->usb_fsa_path) - 1);

    if(ctx->fsa_client > 0)
    {
        if(fsa_dir_exists(ctx->fsa_client, "/vol/storage_usb01/"))
        {
            ctx->usb_mounted = true;
            ctx->usb_index = 1;
            strncpy(ctx->usb_fsa_path, "/vol/storage_usb01/", sizeof(ctx->usb_fsa_path) - 1);
        }
        else if(fsa_dir_exists(ctx->fsa_client, "/vol/storage_usb02/"))
        {
            ctx->usb_mounted = true;
            ctx->usb_index = 2;
            strncpy(ctx->usb_fsa_path, "/vol/storage_usb02/", sizeof(ctx->usb_fsa_path) - 1);
        }

        if(ctx->usb_mounted)
        {
            uint64_t free_usb = 0;
            if(FSAGetFreeSpaceSize(ctx->fsa_client, ctx->usb_fsa_path, &free_usb) == FS_ERROR_OK)
            {
                ctx->usb_free_bytes = free_usb;
            }
        }

        uint64_t free_mlc = 0;
        if(FSAGetFreeSpaceSize(ctx->fsa_client, "/vol/storage_mlc01/", &free_mlc) == FS_ERROR_OK)
        {
            ctx->nand_free_bytes = free_mlc;
        }

        uint64_t free_sd = 0;
        const char *sd_path = ctx->app_sd_mounted ? "/vol/app_sd/" : "/vol/external01/";
        if(FSAGetFreeSpaceSize(ctx->fsa_client, sd_path, &free_sd) == FS_ERROR_OK)
        {
            ctx->sd_free_bytes = free_sd;
        }
    }

    if(ctx->sd_free_bytes == 0)
    {
        struct statvfs vfs;
        if(statvfs("/vol/external01", &vfs) == 0)
        {
            ctx->sd_free_bytes = (uint64_t)vfs.f_bsize * (uint64_t)vfs.f_bavail;
        }
    }
}

static uint64_t get_active_target_free_bytes(const AppContext *ctx)
{
    if(ctx->install_target == MCP_INSTALL_TARGET_USB && ctx->usb_mounted)
    {
        return ctx->usb_free_bytes;
    }
    return ctx->nand_free_bytes;
}

static FSAClientHandle s_fsaClient = -1;

static void fsa_remove_directory_recursive(FSAClientHandle fsa_client, const char *fsa_dir_path);

static const uint32_t s_iosuCertPatchAddresses[6] = {
    0x05054D6C,
    0x05054D70, /* Cached cert check */
    0x05052A90,
    0x05052A94, /* Cert verification */
    0x05052C44,
    0x05052C48 /* IOSC_VerifyPubkeySign */
};

static void init_mocha_and_fsa(AppContext *ctx)
{
    ctx->fsa_client = -1;
    s_fsaClient = -1;
    ctx->mocha_ready = (Mocha_InitLibrary() == MOCHA_RESULT_SUCCESS);

    if(FSAInit() == FS_ERROR_OK)
    {
        ctx->fsa_client = FSAAddClient(NULL);
        s_fsaClient = ctx->fsa_client;
        if(ctx->fsa_client > 0 && ctx->mocha_ready)
        {
            if(Mocha_UnlockFSClientEx(ctx->fsa_client) == MOCHA_RESULT_SUCCESS)
            {
                ctx->fsa_unlocked = true;
                if(FSAMount(
                       ctx->fsa_client,
                       "/vol/external01",
                       "/vol/app_sd",
                       FSA_MOUNT_FLAG_BIND_MOUNT,
                       NULL,
                       0)
                    == FS_ERROR_OK)
                {
                    ctx->app_sd_mounted = true;
                }
            }
        }
    }

    if(ctx->mocha_ready)
    {
        ctx->iosu_patch_count = 0;
        for(int i = 0; i < 6; i++)
        {
            if(Mocha_IOSUKernelRead32(s_iosuCertPatchAddresses[i], &ctx->iosu_orig_values[i]) != MOCHA_RESULT_SUCCESS)
            {
                break;
            }
            uint32_t patch_val = (i % 2 == 0) ? 0xE3A00000U : 0xE12FFF1EU;
            if(Mocha_IOSUKernelWrite32(s_iosuCertPatchAddresses[i], patch_val) != MOCHA_RESULT_SUCCESS)
            {
                break;
            }
            ctx->iosu_patch_count++;
        }
    }

    refresh_storage_space(ctx);
}

static void deinit_mocha_and_fsa(AppContext *ctx)
{
    if(ctx->fsa_client > 0)
    {
        if(ctx->app_sd_mounted)
        {
            FSAUnmount(ctx->fsa_client, "/vol/app_sd", FSA_UNMOUNT_FLAG_BIND_MOUNT);
            ctx->app_sd_mounted = false;
        }
        FSADelClient(ctx->fsa_client);
        ctx->fsa_client = -1;
        s_fsaClient = -1;
        FSAShutdown();
    }

    if(ctx->mocha_ready)
    {
        for(int i = 0; i < ctx->iosu_patch_count; i++)
        {
            Mocha_IOSUKernelWrite32(s_iosuCertPatchAddresses[i], ctx->iosu_orig_values[i]);
        }
        ctx->iosu_patch_count = 0;
        Mocha_DeInitLibrary();
        ctx->mocha_ready = false;
    }
}

static bool mkdir_recursive(const char *dir_path)
{
    char tmp[MAX_PATH_LEN];
    size_t len = strlen(dir_path);
    if(len == 0 || len >= sizeof(tmp))
    {
        return false;
    }

    memcpy(tmp, dir_path, len + 1);
    if(tmp[len - 1] == '/')
    {
        tmp[len - 1] = '\0';
    }

    /* Skip virtual mount prefix components (/vol/external01/) when creating subdirectories */
    char *start_scan = tmp + 1;
    const char *ext_prefix = "/vol/external01/";
    if(strncmp(tmp, ext_prefix, strlen(ext_prefix)) == 0)
    {
        start_scan = tmp + strlen(ext_prefix);
    }

    for(char *p = start_scan; *p; p++)
    {
        if(*p == '/')
        {
            *p = '\0';
            if(s_fsaClient > 0)
            {
                if(!fsa_dir_exists(s_fsaClient, tmp))
                {
                    FSAMakeDir(s_fsaClient, tmp, 0x660);
                }
            }
            mkdir(tmp, 0777);
            *p = '/';
        }
    }

    if(s_fsaClient > 0)
    {
        if(fsa_dir_exists(s_fsaClient, tmp))
        {
            return true;
        }
        FSError ferr = FSAMakeDir(s_fsaClient, tmp, 0x660);
        if(ferr == FS_ERROR_OK || ferr == FS_ERROR_ALREADY_EXISTS || fsa_dir_exists(s_fsaClient, tmp))
        {
            return true;
        }
    }

    if(mkdir(tmp, 0777) != 0 && errno != EEXIST)
    {
        DIR *d = opendir(tmp);
        if(d)
        {
            closedir(d);
            return true;
        }
        struct stat st;
        if(stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            return false;
        }
    }
    return true;
}

static bool ensure_parent_dir(const char *file_path)
{
    char parent[MAX_PATH_LEN];
    strncpy(parent, file_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';

    char *last_slash = strrchr(parent, '/');
    if(!last_slash || last_slash == parent)
    {
        return true;
    }
    *last_slash = '\0';
    return mkdir_recursive(parent);
}

static void remove_directory_recursive(const char *dir_path)
{
    if(s_fsaClient > 0)
    {
        fsa_remove_directory_recursive(s_fsaClient, dir_path);
    }

    DIR *dir = opendir(dir_path);
    if(!dir)
    {
        return;
    }

    struct dirent *entry;
    char child_path[MAX_PATH_LEN];

    while((entry = readdir(dir)) != NULL)
    {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if(stat(child_path, &st) == 0 && S_ISDIR(st.st_mode))
        {
            remove_directory_recursive(child_path);
        }
        else
        {
            unlink(child_path);
        }
    }
    closedir(dir);
    rmdir(dir_path);
}

static void fsa_remove_directory_recursive(FSAClientHandle fsa_client, const char *fsa_dir_path)
{
    if(fsa_client <= 0)
    {
        return;
    }

    char dir_with_slash[MAX_PATH_LEN];
    snprintf(dir_with_slash, sizeof(dir_with_slash), "%.480s", fsa_dir_path);
    size_t len = strlen(dir_with_slash);
    if(len > 0 && dir_with_slash[len - 1] != '/')
    {
        dir_with_slash[len++] = '/';
        dir_with_slash[len] = '\0';
    }

    FSADirectoryHandle dir_handle;
    if(FSAOpenDir(fsa_client, dir_with_slash, &dir_handle) != FS_ERROR_OK)
    {
        return;
    }

    FSADirectoryEntry entry;
    char child_path[MAX_PATH_LEN + 128];
    while(FSAReadDir(fsa_client, dir_handle, &entry) == FS_ERROR_OK)
    {
        if(strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
        {
            continue;
        }
        snprintf(child_path, sizeof(child_path), "%s%s", dir_with_slash, entry.name);
        if(entry.info.flags & FS_STAT_DIRECTORY)
        {
            fsa_remove_directory_recursive(fsa_client, child_path);
        }
        else
        {
            FSARemove(fsa_client, child_path);
        }
    }
    FSACloseDir(fsa_client, dir_handle);

    if(len > 1 && dir_with_slash[len - 1] == '/')
    {
        dir_with_slash[len - 1] = '\0';
    }
    FSARemove(fsa_client, dir_with_slash);
}

static void cleanup_incomplete_import_dirs(AppContext *ctx)
{
    if(ctx->fsa_client <= 0)
    {
        return;
    }

    const char *base = (ctx->install_target == MCP_INSTALL_TARGET_USB && ctx->usb_mounted)
        ? ctx->usb_fsa_path
        : "/vol/storage_mlc01/";

    char import_root[MAX_PATH_LEN];
    snprintf(import_root, sizeof(import_root), "%susr/import/", base);

    FSADirectoryHandle dir;
    if(FSAOpenDir(ctx->fsa_client, import_root, &dir) != FS_ERROR_OK)
    {
        return;
    }

    FSADirectoryEntry entry;
    char child[MAX_PATH_LEN + 64];
    while(FSAReadDir(ctx->fsa_client, dir, &entry) == FS_ERROR_OK)
    {
        if((entry.info.flags & FS_STAT_DIRECTORY) && strlen(entry.name) == 8)
        {
            snprintf(child, sizeof(child), "%s%.16s", import_root, entry.name);
            fsa_remove_directory_recursive(ctx->fsa_client, child);
        }
    }
    FSACloseDir(ctx->fsa_client, dir);
}

static void parse_ip_to_octets(const char *ip_str, int out_octets[4])
{
    int a = 10, b = 0, c = 0, d = 99;
    if(ip_str && sscanf(ip_str, "%d.%d.%d.%d", &a, &b, &c, &d) == 4)
    {
        out_octets[0] = (a >= 0 && a <= 255) ? a : 10;
        out_octets[1] = (b >= 0 && b <= 255) ? b : 0;
        out_octets[2] = (c >= 0 && c <= 255) ? c : 0;
        out_octets[3] = (d >= 0 && d <= 255) ? d : 99;
    }
    else
    {
        out_octets[0] = 10;
        out_octets[1] = 0;
        out_octets[2] = 0;
        out_octets[3] = 99;
    }
}

static void save_server_config(const AppContext *ctx)
{
    mkdir_recursive(CONFIG_DIR_PATH);

    json_t *root = json_object();
    if(!root)
    {
        return;
    }
    json_object_set_new(root, "server_ip", json_string(ctx->server_ip));
    json_object_set_new(root, "server_port", json_integer(ctx->server_port));
    json_object_set_new(
        root,
        "install_target",
        json_string(ctx->install_target == MCP_INSTALL_TARGET_USB ? "usb" : "nand"));

    char *serialized = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    if(!serialized)
    {
        return;
    }

    FILE *fp = fopen(CONFIG_FILE_PATH, "wb");
    if(fp)
    {
        fwrite(serialized, 1, strlen(serialized), fp);
        fclose(fp);
    }
    free(serialized);
}

static void load_server_config(AppContext *ctx)
{
    strncpy(ctx->server_ip, DEFAULT_SERVER_IP, sizeof(ctx->server_ip) - 1);
    ctx->server_port = DEFAULT_SERVER_PORT;
    ctx->install_target = ctx->usb_mounted
        ? MCP_INSTALL_TARGET_USB
        : MCP_INSTALL_TARGET_MLC;

    json_error_t jerr;
    json_t *root = json_load_file(CONFIG_FILE_PATH, 0, &jerr);
    if(!root)
    {
        return;
    }

    json_t *ip_item = json_object_get(root, "server_ip");
    if(json_is_string(ip_item))
    {
        const char *val = json_string_value(ip_item);
        if(val && val[0])
        {
            strncpy(ctx->server_ip, val, sizeof(ctx->server_ip) - 1);
            ctx->server_ip[sizeof(ctx->server_ip) - 1] = '\0';
        }
    }

    json_t *port_item = json_object_get(root, "server_port");
    if(json_is_integer(port_item))
    {
        int p = (int)json_integer_value(port_item);
        if(p > 0 && p <= 65535)
        {
            ctx->server_port = p;
        }
    }

    json_t *target_item = json_object_get(root, "install_target");
    if(json_is_string(target_item))
    {
        const char *tstr = json_string_value(target_item);
        if(tstr && strcasecmp(tstr, "nand") == 0)
        {
            ctx->install_target = MCP_INSTALL_TARGET_MLC;
        }
        else if(tstr && strcasecmp(tstr, "usb") == 0)
        {
            ctx->install_target = MCP_INSTALL_TARGET_USB;
        }
    }

    json_decref(root);
}

/* --------------------------------------------------------------------------
 * Console Installation Check & Category Filtering
 * -------------------------------------------------------------------------- */

static void refresh_installed_status(AppContext *ctx)
{
    int mcp = MCP_Open();
    if(mcp < 0)
    {
        return;
    }

    MCPTitleListType info __attribute__((__aligned__(0x40)));
    for(int i = 0; i < ctx->title_count; i++)
    {
        TitleMetadata *t = &ctx->titles[i];
        if(t->tid_u64 != 0)
        {
            t->is_installed = (MCP_GetTitleInfo(mcp, t->tid_u64, &info) == 0);
        }
    }
    MCP_Close(mcp);
}

static bool title_matches_tab(const TitleMetadata *t, CategoryTab tab)
{
    if(tab == CAT_TAB_ALL)
    {
        return true;
    }
    if(tab == CAT_TAB_GAMES)
    {
        return strcasecmp(t->category, "Game") == 0;
    }
    if(tab == CAT_TAB_UPDATES)
    {
        return strcasecmp(t->category, "Update") == 0;
    }
    if(tab == CAT_TAB_DLC)
    {
        return strcasecmp(t->category, "DLC") == 0;
    }
    if(tab == CAT_TAB_DEMOS)
    {
        return strcasecmp(t->category, "Demo") == 0;
    }
    return true;
}

static void rebuild_filtered_list(AppContext *ctx)
{
    ctx->filtered_count = 0;
    for(int i = 0; i < ctx->title_count; i++)
    {
        if(title_matches_tab(&ctx->titles[i], ctx->active_tab))
        {
            ctx->filtered_indices[ctx->filtered_count++] = i;
        }
    }
    if(ctx->selected_pos >= ctx->filtered_count)
    {
        ctx->selected_pos = (ctx->filtered_count > 0) ? (ctx->filtered_count - 1) : 0;
    }
    if(ctx->selected_pos < 0)
    {
        ctx->selected_pos = 0;
    }
    if(ctx->scroll_offset > ctx->selected_pos)
    {
        ctx->scroll_offset = ctx->selected_pos;
    }
    if(ctx->selected_pos >= ctx->scroll_offset + VISIBLE_CARDS)
    {
        ctx->scroll_offset = ctx->selected_pos - VISIBLE_CARDS + 1;
    }
}

static int get_selected_title_index(const AppContext *ctx)
{
    if(ctx->filtered_count <= 0 || ctx->selected_pos < 0 || ctx->selected_pos >= ctx->filtered_count)
    {
        return -1;
    }
    return ctx->filtered_indices[ctx->selected_pos];
}

/* --------------------------------------------------------------------------
 * Download Queue Helpers
 * -------------------------------------------------------------------------- */

static int find_in_queue(const AppContext *ctx, int title_idx)
{
    for(int i = 0; i < ctx->queue_count; i++)
    {
        if(ctx->queue[i] == title_idx)
        {
            return i;
        }
    }
    return -1;
}

static bool enqueue_title(AppContext *ctx, int title_idx)
{
    if(title_idx < 0 || title_idx >= ctx->title_count)
    {
        return false;
    }
    if(find_in_queue(ctx, title_idx) >= 0 || ctx->queue_count >= MAX_QUEUE_LEN)
    {
        return false;
    }
    ctx->queue[ctx->queue_count++] = title_idx;
    return true;
}

static void toggle_queue_title(AppContext *ctx, int title_idx)
{
    if(title_idx < 0 || title_idx >= ctx->title_count)
    {
        return;
    }
    int pos = find_in_queue(ctx, title_idx);
    if(pos >= 0)
    {
        for(int i = pos; i < ctx->queue_count - 1; i++)
        {
            ctx->queue[i] = ctx->queue[i + 1];
        }
        ctx->queue_count--;
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Removed '%s' from queue (%d queued).",
            ctx->titles[title_idx].name,
            ctx->queue_count);
    }
    else if(enqueue_title(ctx, title_idx))
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Added '%s' to queue (%d queued). Press A to start!",
            ctx->titles[title_idx].name,
            ctx->queue_count);
    }
}

static int find_companion_title(const AppContext *ctx, uint64_t base_tid, uint32_t target_high)
{
    uint64_t wanted = ((uint64_t)target_high << 32) | (base_tid & 0xFFFFFFFFULL);
    for(int i = 0; i < ctx->title_count; i++)
    {
        if(ctx->titles[i].tid_u64 == wanted)
        {
            return i;
        }
    }
    return -1;
}

static void free_catalog(AppContext *ctx)
{
    for(int i = 0; i < ctx->title_count; i++)
    {
        TitleMetadata *t = &ctx->titles[i];
        if(t->file_names)
        {
            for(int j = 0; j < t->file_count; j++)
            {
                free(t->file_names[j]);
            }
            free(t->file_names);
            t->file_names = NULL;
        }
        if(t->file_urls)
        {
            for(int j = 0; j < t->file_count; j++)
            {
                free(t->file_urls[j]);
            }
            free(t->file_urls);
            t->file_urls = NULL;
        }
        t->file_count = 0;
    }
    ctx->title_count = 0;
    ctx->filtered_count = 0;
    ctx->queue_count = 0;
}

/* --------------------------------------------------------------------------
 * 1. UDP LAN Auto-Discovery & Catalog Ingestion (libcurl & Jansson)
 * -------------------------------------------------------------------------- */

static bool discover_server_on_lan(AppContext *ctx)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock < 0)
    {
        return false;
    }

    int broadcast_on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_on, sizeof(broadcast_on));

    int candidate_ports[3] = { ctx->server_port, 8000, 8080 };
    for(int i = 0; i < 3; i++)
    {
        if(i > 0 && candidate_ports[i] == candidate_ports[0])
        {
            continue;
        }
        if(i == 2 && candidate_ports[2] == candidate_ports[1])
        {
            continue;
        }
        struct sockaddr_in bcast_addr;
        memset(&bcast_addr, 0, sizeof(bcast_addr));
        bcast_addr.sin_family = AF_INET;
        bcast_addr.sin_port = htons((uint16_t)candidate_ports[i]);
        bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        sendto(
            sock,
            DISCOVERY_REQ_MAGIC,
            strlen(DISCOVERY_REQ_MAGIC),
            0,
            (struct sockaddr *)&bcast_addr,
            sizeof(bcast_addr));
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; /* 500 ms discovery window */

    bool found = false;
    if(select(sock + 1, &read_fds, NULL, NULL, &tv) > 0 && FD_ISSET(sock, &read_fds))
    {
        char rx_buf[512];
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);
        ssize_t n = recvfrom(
            sock,
            rx_buf,
            sizeof(rx_buf) - 1,
            0,
            (struct sockaddr *)&sender_addr,
            &sender_len);
        if(n > 0)
        {
            rx_buf[n] = '\0';
            json_error_t jerr;
            json_t *root = json_loads(rx_buf, 0, &jerr);
            if(root)
            {
                json_t *magic = json_object_get(root, "magic");
                json_t *port = json_object_get(root, "port");
                if(json_is_string(magic) && strcmp(json_string_value(magic), DISCOVERY_RES_MAGIC) == 0)
                {
                    const char *sender_ip = inet_ntoa(sender_addr.sin_addr);
                    if(sender_ip && sender_ip[0])
                    {
                        strncpy(ctx->server_ip, sender_ip, sizeof(ctx->server_ip) - 1);
                        ctx->server_ip[sizeof(ctx->server_ip) - 1] = '\0';
                    }
                    if(json_is_integer(port) && json_integer_value(port) > 0)
                    {
                        ctx->server_port = (int)json_integer_value(port);
                    }
                    save_server_config(ctx);
                    found = true;
                }
                json_decref(root);
            }
        }
    }

    close(sock);
    return found;
}

static size_t curl_memory_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    if(mem->size + total + 1 > mem->capacity)
    {
        size_t new_cap = (mem->capacity == 0) ? 16384 : mem->capacity * 2;
        while(new_cap < mem->size + total + 1)
        {
            new_cap *= 2;
        }
        uint8_t *new_data = (uint8_t *)realloc(mem->data, new_cap);
        if(!new_data)
        {
            return 0;
        }
        mem->data = new_data;
        mem->capacity = new_cap;
    }

    memcpy(mem->data + mem->size, contents, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

static size_t curl_file_stream_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    StreamFileContext *sctx = (StreamFileContext *)userdata;
    if(!sctx || !sctx->fp)
    {
        return 0;
    }

    size_t written = fwrite(ptr, 1, total, sctx->fp);
    if(written != total)
    {
        sctx->write_error = true;
        return 0;
    }
    sctx->bytes_written += written;
    return written;
}

static int curl_xferinfo_cb(
    void *clientp,
    curl_off_t dltotal,
    curl_off_t dlnow,
    curl_off_t ul_total,
    curl_off_t ul_now)
{
    (void)dltotal;
    (void)ul_total;
    (void)ul_now;

    AppContext *ctx = (AppContext *)clientp;
    if(!ctx)
    {
        return 0;
    }
    if(dlnow > 0)
    {
        ctx->dl_current_file_bytes = ctx->dl_resume_offset + (uint64_t)dlnow;
    }
    return (ctx->state == APP_STATE_CANCELLED) ? 1 : 0;
}

static void resolve_file_name_and_url(
    const AppContext *ctx,
    const char *title_id,
    const char *raw_entry,
    char *out_name,
    size_t out_name_size,
    char *out_url,
    size_t out_url_size)
{
    if(strncmp(raw_entry, "http://", 7) == 0 || strncmp(raw_entry, "https://", 8) == 0)
    {
        strncpy(out_url, raw_entry, out_url_size - 1);
        out_url[out_url_size - 1] = '\0';

        const char *marker = strstr(raw_entry, "/files/");
        if(marker)
        {
            strncpy(out_name, marker + 7, out_name_size - 1);
        }
        else
        {
            const char *slash = strrchr(raw_entry, '/');
            strncpy(out_name, slash ? (slash + 1) : raw_entry, out_name_size - 1);
        }
        out_name[out_name_size - 1] = '\0';
    }
    else
    {
        const char *rel = (raw_entry[0] == '/') ? (raw_entry + 1) : raw_entry;
        strncpy(out_name, rel, out_name_size - 1);
        out_name[out_name_size - 1] = '\0';

        snprintf(
            out_url,
            out_url_size,
            "http://%s:%d/api/v1/titles/%s/files/%s",
            ctx->server_ip,
            ctx->server_port,
            title_id,
            rel);
    }
}

static void infer_category_from_tid(const char *tid, char *out_cat, size_t out_size)
{
    if(strncasecmp(tid, "00050000", 8) == 0)
    {
        strncpy(out_cat, "Game", out_size - 1);
    }
    else if(strncasecmp(tid, "0005000e", 8) == 0)
    {
        strncpy(out_cat, "Update", out_size - 1);
    }
    else if(strncasecmp(tid, "0005000c", 8) == 0)
    {
        strncpy(out_cat, "DLC", out_size - 1);
    }
    else if(strncasecmp(tid, "00050002", 8) == 0)
    {
        strncpy(out_cat, "Demo", out_size - 1);
    }
    else
    {
        strncpy(out_cat, "Game", out_size - 1);
    }
    out_cat[out_size - 1] = '\0';
}

static bool fetch_titles_catalog(AppContext *ctx, bool allow_autodiscover)
{
    free_catalog(ctx);

    char endpoint_url[MAX_URL_LEN];
    snprintf(
        endpoint_url,
        sizeof(endpoint_url),
        "http://%s:%d/api/v1/titles",
        ctx->server_ip,
        ctx->server_port);

    CURL *curl = curl_easy_init();
    if(!curl)
    {
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Error: Failed to initialize libcurl.");
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    MemoryBuffer response_buf = { 0 };

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_memory_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LocaLeShop-WiiU/1.1");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK || http_code != 200 || !response_buf.data)
    {
        free(response_buf.data);
        if(allow_autodiscover && discover_server_on_lan(ctx))
        {
            return fetch_titles_catalog(ctx, false);
        }
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Cannot reach %s:%d (%s). Press X to Auto-Discover or - for Server Settings.",
            ctx->server_ip,
            ctx->server_port,
            curl_easy_strerror(res));
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    json_error_t jerr;
    json_t *root = json_loads((const char *)response_buf.data, 0, &jerr);
    free(response_buf.data);

    if(!root || !json_is_array(root))
    {
        if(root)
        {
            json_decref(root);
        }
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Invalid JSON catalog from server.");
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    size_t idx = 0;
    json_t *item = NULL;
    json_array_foreach(root, idx, item)
    {
        if(ctx->title_count >= MAX_TITLES)
        {
            break;
        }
        if(!json_is_object(item))
        {
            continue;
        }

        json_t *j_tid = json_object_get(item, "title_id");
        json_t *j_name = json_object_get(item, "name");
        json_t *j_ver = json_object_get(item, "version");
        json_t *j_size = json_object_get(item, "total_size_bytes");
        json_t *j_region = json_object_get(item, "region");
        json_t *j_cat = json_object_get(item, "category");
        json_t *j_pcode = json_object_get(item, "product_code");
        json_t *j_fmt = json_object_get(item, "source_format");
        json_t *j_files = json_object_get(item, "file_list");
        if(!j_files)
        {
            j_files = json_object_get(item, "files");
        }

        if(!json_is_string(j_tid) || !json_string_value(j_tid))
        {
            continue;
        }

        TitleMetadata *meta = &ctx->titles[ctx->title_count];
        memset(meta, 0, sizeof(*meta));

        strncpy(meta->title_id, json_string_value(j_tid), sizeof(meta->title_id) - 1);
        meta->tid_u64 = strtoull(meta->title_id, NULL, 16);

        if(json_is_string(j_name) && json_string_value(j_name))
        {
            strncpy(meta->name, json_string_value(j_name), sizeof(meta->name) - 1);
        }
        else
        {
            strncpy(meta->name, meta->title_id, sizeof(meta->name) - 1);
        }

        if(json_is_string(j_region) && json_string_value(j_region))
        {
            strncpy(meta->region, json_string_value(j_region), sizeof(meta->region) - 1);
        }
        else
        {
            strncpy(meta->region, "ALL", sizeof(meta->region) - 1);
        }

        if(json_is_string(j_cat) && json_string_value(j_cat))
        {
            strncpy(meta->category, json_string_value(j_cat), sizeof(meta->category) - 1);
        }
        else
        {
            infer_category_from_tid(meta->title_id, meta->category, sizeof(meta->category));
        }

        if(json_is_string(j_pcode) && json_string_value(j_pcode))
        {
            strncpy(meta->product_code, json_string_value(j_pcode), sizeof(meta->product_code) - 1);
        }

        if(json_is_string(j_fmt) && json_string_value(j_fmt))
        {
            strncpy(meta->source_format, json_string_value(j_fmt), sizeof(meta->source_format) - 1);
        }
        else
        {
            strncpy(meta->source_format, "wup", sizeof(meta->source_format) - 1);
        }
        for(char *c = meta->source_format; *c; ++c)
        {
            *c = (char)toupper((unsigned char)*c);
        }

        meta->version = json_is_number(j_ver) ? (uint32_t)json_number_value(j_ver) : 0;
        meta->total_size_bytes = json_is_integer(j_size)
            ? (uint64_t)json_integer_value(j_size)
            : (json_is_number(j_size) ? (uint64_t)json_number_value(j_size) : 0);

        if(json_is_array(j_files))
        {
            int arr_len = (int)json_array_size(j_files);
            if(arr_len > MAX_FILES_PER_TITLE)
            {
                arr_len = MAX_FILES_PER_TITLE;
            }
            if(arr_len > 0)
            {
                meta->file_names = (char **)calloc((size_t)arr_len, sizeof(char *));
                meta->file_urls = (char **)calloc((size_t)arr_len, sizeof(char *));

                size_t f_i = 0;
                json_t *f_elem = NULL;
                int f_idx = 0;
                json_array_foreach(j_files, f_i, f_elem)
                {
                    if(f_idx >= arr_len)
                    {
                        break;
                    }
                    if(!json_is_string(f_elem) || !json_string_value(f_elem))
                    {
                        continue;
                    }
                    char resolved_name[MAX_PATH_LEN];
                    char resolved_url[MAX_URL_LEN];
                    resolve_file_name_and_url(
                        ctx,
                        meta->title_id,
                        json_string_value(f_elem),
                        resolved_name,
                        sizeof(resolved_name),
                        resolved_url,
                        sizeof(resolved_url));
                    meta->file_names[f_idx] = strdup(resolved_name);
                    meta->file_urls[f_idx] = strdup(resolved_url);
                    f_idx++;
                }
                meta->file_count = f_idx;
            }
        }

        ctx->title_count++;
    }

    json_decref(root);

    refresh_installed_status(ctx);
    ctx->selected_pos = 0;
    ctx->scroll_offset = 0;
    rebuild_filtered_list(ctx);
    ctx->state = APP_STATE_BROWSING;

    snprintf(
        ctx->status_message,
        sizeof(ctx->status_message),
        "Connected to %s:%d — %d title(s) available. Press A for Details/Install, + to Queue.",
        ctx->server_ip,
        ctx->server_port,
        ctx->title_count);
    return true;
}

/* --------------------------------------------------------------------------
 * Fallback Certificate Synthesizer (Safety Net before MCP Install)
 * -------------------------------------------------------------------------- */

static void ensure_certificate_exists(const char *stage_dir)
{
    char cert_path[MAX_PATH_LEN + 32];
    char tmd_path[MAX_PATH_LEN + 32];
    snprintf(cert_path, sizeof(cert_path), "%.480s/title.cert", stage_dir);
    snprintf(tmd_path, sizeof(tmd_path), "%.480s/title.tmd", stage_dir);

    struct stat st;
    if(stat(cert_path, &st) == 0 && st.st_size >= 0x700)
    {
        return;
    }

    uint8_t cetk[0xA00];
    memset(cetk, 0, sizeof(cetk));
    bool copied_from_tmd = false;

    FILE *tmd_fp = fopen(tmd_path, "rb");
    if(tmd_fp)
    {
        fseek(tmd_fp, 0, SEEK_END);
        long tmd_size = ftell(tmd_fp);
        fseek(tmd_fp, 0, SEEK_SET);
        if(tmd_size > 0xB04)
        {
            uint8_t *tmd_buf = (uint8_t *)malloc((size_t)tmd_size);
            if(tmd_buf && fread(tmd_buf, 1, (size_t)tmd_size, tmd_fp) == (size_t)tmd_size)
            {
                uint16_t num_contents = ((uint16_t)tmd_buf[0x1DE] << 8) | tmd_buf[0x1DF];
                size_t certs_off = 0xB04 + (size_t)num_contents * 0x30;
                if((size_t)tmd_size >= certs_off + 0x700)
                {
                    /* TMD appends CP (0x300) then CA3 (0x400) */
                    memcpy(cetk, tmd_buf + certs_off + 0x300, 0x400);
                    memcpy(cetk + 0x400, tmd_buf + certs_off, 0x300);
                    copied_from_tmd = true;
                }
            }
            free(tmd_buf);
        }
        fclose(tmd_fp);
    }

    if(!copied_from_tmd)
    {
        cetk[0x003] = 0x03;
        cetk[0x001] = 0x01;
        strcpy((char *)(cetk + 0x240), "Root");
        cetk[0x283] = 0x01;
        strcpy((char *)(cetk + 0x284), "CA00000003");

        cetk[0x403] = 0x04;
        cetk[0x401] = 0x01;
        strcpy((char *)(cetk + 0x540), "Root-CA00000003");
        cetk[0x583] = 0x01;
        strcpy((char *)(cetk + 0x584), "CP0000000b");
    }

    cetk[0x703] = 0x04;
    cetk[0x701] = 0x01;
    strcpy((char *)(cetk + 0x840), "Root-CA00000003");
    cetk[0x883] = 0x01;
    strcpy((char *)(cetk + 0x884), "XS0000000c");

    FILE *cfp = fopen(cert_path, "wb");
    if(cfp)
    {
        fwrite(cetk, 1, sizeof(cetk), cfp);
        fclose(cfp);
    }
}

/* --------------------------------------------------------------------------
 * Chunked Direct-to-SD Streaming & Non-Blocking Download State Machine
 * -------------------------------------------------------------------------- */

static void begin_title_download(AppContext *ctx, int title_idx);

static void cleanup_active_download_handles(AppContext *ctx)
{
    if(ctx->curl_easy)
    {
        if(ctx->curl_multi)
        {
            curl_multi_remove_handle(ctx->curl_multi, ctx->curl_easy);
        }
        curl_easy_cleanup(ctx->curl_easy);
        ctx->curl_easy = NULL;
    }
    if(ctx->stream_ctx.fp)
    {
        fflush(ctx->stream_ctx.fp);
        fclose(ctx->stream_ctx.fp);
        ctx->stream_ctx.fp = NULL;
    }
}

static bool start_next_file_download(AppContext *ctx, bool is_retry)
{
    cleanup_active_download_handles(ctx);

    TitleMetadata *title = &ctx->titles[ctx->dl_title_index];
    if(ctx->dl_file_index >= title->file_count)
    {
        return false;
    }

    if(!is_retry)
    {
        ctx->dl_retry_count = 0;
        ctx->dl_resume_offset = 0;
    }

    const char *rel_name = title->file_names[ctx->dl_file_index];
    const char *file_url = title->file_urls[ctx->dl_file_index];

    strncpy(ctx->dl_current_filename, rel_name, sizeof(ctx->dl_current_filename) - 1);
    ctx->dl_current_filename[sizeof(ctx->dl_current_filename) - 1] = '\0';

    snprintf(
        ctx->dl_current_filepath,
        sizeof(ctx->dl_current_filepath),
        "%.240s/%.240s",
        ctx->dl_stage_dir,
        rel_name);

    if(!ensure_parent_dir(ctx->dl_current_filepath))
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "SD Error: Could not create directory for %.240s",
            ctx->dl_current_filepath);
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    memset(&ctx->stream_ctx, 0, sizeof(ctx->stream_ctx));
    if(is_retry)
    {
        struct stat st;
        if(stat(ctx->dl_current_filepath, &st) == 0 && st.st_size > 0)
        {
            ctx->dl_resume_offset = (uint64_t)st.st_size;
            ctx->stream_ctx.fp = fopen(ctx->dl_current_filepath, "ab");
        }
    }
    if(!ctx->stream_ctx.fp)
    {
        ctx->dl_resume_offset = 0;
        ctx->stream_ctx.fp = fopen(ctx->dl_current_filepath, "wb");
    }

    if(!ctx->stream_ctx.fp)
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "SD Error: Cannot open %.200s for writing (%.120s)",
            ctx->dl_current_filepath,
            strerror(errno));
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    setvbuf(ctx->stream_ctx.fp, ctx->stream_ctx.file_Buf, _IOFBF, sizeof(ctx->stream_ctx.file_Buf));

    ctx->curl_easy = curl_easy_init();
    if(!ctx->curl_easy)
    {
        fclose(ctx->stream_ctx.fp);
        ctx->stream_ctx.fp = NULL;
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Error: curl_easy_init failed.");
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    ctx->dl_current_file_bytes = ctx->dl_resume_offset;

    curl_easy_setopt(ctx->curl_easy, CURLOPT_URL, file_url);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_WRITEFUNCTION, curl_file_stream_write_cb);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_WRITEDATA, &ctx->stream_ctx);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_SOCKOPTFUNCTION, curl_sockopt_cb);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_XFERINFOFUNCTION, curl_xferinfo_cb);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_XFERINFODATA, ctx);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_BUFFERSIZE, (long)SOCKET_BUFSIZE);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_USERAGENT, "LocaLeShop-WiiU/1.1");

    if(ctx->dl_resume_offset > 0)
    {
        curl_easy_setopt(ctx->curl_easy, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)ctx->dl_resume_offset);
    }

    curl_multi_add_handle(ctx->curl_multi, ctx->curl_easy);
    return true;
}

static void start_next_queued_download(AppContext *ctx)
{
    if(ctx->queue_count <= 0)
    {
        refresh_installed_status(ctx);
        ctx->state = APP_STATE_COMPLETED;
        return;
    }

    int next_title = ctx->queue[0];
    for(int i = 0; i < ctx->queue_count - 1; i++)
    {
        ctx->queue[i] = ctx->queue[i + 1];
    }
    ctx->queue_count--;

    begin_title_download(ctx, next_title);
}

static void begin_title_download(AppContext *ctx, int title_idx)
{
    if(title_idx < 0 || title_idx >= ctx->title_count)
    {
        return;
    }

    TitleMetadata *title = &ctx->titles[title_idx];
    if(title->file_count <= 0)
    {
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Package '%s' has no files listed.", title->name);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    /* Refresh & verify target drive availability and free space */
    refresh_storage_space(ctx);
    if(ctx->install_target == MCP_INSTALL_TARGET_USB && !ctx->usb_mounted)
    {
        ctx->install_target = MCP_INSTALL_TARGET_MLC;
    }

    uint64_t target_free = get_active_target_free_bytes(ctx);
    if(target_free > 0 && title->total_size_bytes > 0 && target_free < title->total_size_bytes)
    {
        char need_s[32], free_s[32];
        format_bytes(title->total_size_bytes, need_s, sizeof(need_s));
        format_bytes(target_free, free_s, sizeof(free_s));
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Insufficient space on %s: need %s, only %s free (Press Y to switch target).",
            (ctx->install_target == MCP_INSTALL_TARGET_USB) ? "USB" : "NAND",
            need_s,
            free_s);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    ctx->dl_title_index = title_idx;
    ctx->dl_file_index = 0;
    ctx->dl_retry_count = 0;
    ctx->dl_completed_files_bytes = 0;
    ctx->dl_current_file_bytes = 0;
    ctx->dl_resume_offset = 0;
    ctx->dl_total_bytes = title->total_size_bytes;
    ctx->dl_speed_bps = 0.0;
    ctx->dl_last_speed_tick = OSGetTick();
    ctx->dl_last_speed_bytes = 0;

    snprintf(
        ctx->dl_stage_dir,
        sizeof(ctx->dl_stage_dir),
        "%s/%s",
        SD_STAGE_BASE_DIR,
        title->title_id);

    remove_directory_recursive(ctx->dl_stage_dir);
    bool fsa_created = false;
    if(ctx->fsa_client > 0)
    {
        fsa_remove_directory_recursive(ctx->fsa_client, ctx->dl_stage_dir);
        FSAMakeDir(ctx->fsa_client, FSA_STAGE_BASE_DIR, 0x660);
        FSError mk_err = FSAMakeDir(ctx->fsa_client, ctx->dl_stage_dir, 0x660);
        if(mk_err == FS_ERROR_OK || mk_err == FS_ERROR_ALREADY_EXISTS || fsa_dir_exists(ctx->fsa_client, ctx->dl_stage_dir))
        {
            fsa_created = true;
        }
    }
    if(!fsa_created && !mkdir_recursive(ctx->dl_stage_dir))
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Failed to create SD staging path: %.240s",
            ctx->dl_stage_dir);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    ctx->state = APP_STATE_DOWNLOADING;
    start_next_file_download(ctx, false);
}

/* --------------------------------------------------------------------------
 * 2. MCP Installation Pipeline (Async & Non-Blocking to Home Screen Tile)
 * -------------------------------------------------------------------------- */

static void begin_mcp_installation(AppContext *ctx)
{
    ensure_certificate_exists(ctx->dl_stage_dir);

    int mcp_fd = MCP_Open();
    if(mcp_fd < 0)
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "MCP_Open() failed (error %d)",
            mcp_fd);
        remove_directory_recursive(ctx->dl_stage_dir);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    ctx->mcp_fd = mcp_fd;
    memset(&ctx->mcp_progress, 0, sizeof(ctx->mcp_progress));
    ctx->mcp_started_flag = false;

    MCPError target_err = MCP_InstallSetTarget(mcp_fd, ctx->install_target, ctx->usb_index);
    if(target_err < 0 && ctx->install_target == MCP_INSTALL_TARGET_USB)
    {
        /* Fallback to internal NAND if USB target fails */
        ctx->install_target = MCP_INSTALL_TARGET_MLC;
        target_err = MCP_InstallSetTarget(mcp_fd, MCP_INSTALL_TARGET_MLC, ctx->usb_index);
    }

    if(target_err < 0)
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "MCP_InstallSetTarget failed (0x%08X)",
            (unsigned int)target_err);
        MCP_Close(mcp_fd);
        ctx->mcp_fd = -1;
        remove_directory_recursive(ctx->dl_stage_dir);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    /* Resolve MCP stage path (/vol/app_sd/install/<tid> or /vol/external01/install/<tid>) */
    char mcp_stage_path[MAX_PATH_LEN];
    const char *tid_str = ctx->titles[ctx->dl_title_index].title_id;

    MCPInstallInfo info __attribute__((__aligned__(0x40)));
    memset(&info, 0, sizeof(info));
    MCPError info_err = -1;

    const char *mcp_bases[2] = { MCP_STAGE_BASE_DIR, FSA_STAGE_BASE_DIR };
    for(int b_i = 0; b_i < 2; b_i++)
    {
        snprintf(mcp_stage_path, sizeof(mcp_stage_path), "%s/%s", mcp_bases[b_i], tid_str);
        info_err = MCP_InstallGetInfo(mcp_fd, mcp_stage_path, &info);
        if(info_err >= 0)
        {
            break;
        }
    }
    if(info_err < 0)
    {
        strncpy(mcp_stage_path, ctx->dl_stage_dir, sizeof(mcp_stage_path) - 1);
        mcp_stage_path[sizeof(mcp_stage_path) - 1] = '\0';
        info_err = MCP_InstallGetInfo(mcp_fd, mcp_stage_path, &info);
    }
    if(info_err < 0)
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "MCP_InstallGetInfo rejected package (0x%08X) in %.200s",
            (unsigned int)info_err,
            mcp_stage_path);
        MCP_Close(mcp_fd);
        ctx->mcp_fd = -1;
        remove_directory_recursive(ctx->dl_stage_dir);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    MCPError install_err = MCP_InstallTitleAsync(mcp_fd, mcp_stage_path);
    if(install_err < 0)
    {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "MCP_InstallTitleAsync failed (0x%08X) for %.200s",
            (unsigned int)install_err,
            ctx->dl_stage_dir);
        MCP_Close(mcp_fd);
        ctx->mcp_fd = -1;
        remove_directory_recursive(ctx->dl_stage_dir);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    ctx->state = APP_STATE_INSTALLING;
    snprintf(
        ctx->status_message,
        sizeof(ctx->status_message),
        "Installing '%s' to Wii U Home Screen (%s)...",
        ctx->titles[ctx->dl_title_index].name,
        (ctx->install_target == MCP_INSTALL_TARGET_USB) ? "USB" : "NAND");
}

static void step_mcp_installation(AppContext *ctx)
{
    if(ctx->state != APP_STATE_INSTALLING || ctx->mcp_fd < 0)
    {
        return;
    }

    MCPInstallProgress progress __attribute__((__aligned__(0x40)));
    memset(&progress, 0, sizeof(progress));

    MCPError err = MCP_InstallGetProgress(ctx->mcp_fd, &progress);
    if(err >= 0)
    {
        ctx->mcp_progress = progress;
        const char *target_label = (ctx->install_target == MCP_INSTALL_TARGET_USB) ? "USB" : "NAND";

        if(progress.inProgress == 1)
        {
            ctx->mcp_started_flag = true;
            char done_str[32], total_str[32];
            format_bytes(progress.sizeProgress, done_str, sizeof(done_str));
            format_bytes(progress.sizeTotal, total_str, sizeof(total_str));
            snprintf(
                ctx->status_message,
                sizeof(ctx->status_message),
                "Installing to %s Home Screen: content %u/%u (%s / %s)",
                target_label,
                progress.contentsProgress,
                progress.contentsTotal,
                done_str,
                total_str);
            return;
        }

        if(ctx->mcp_started_flag || (progress.sizeTotal > 0 && progress.sizeProgress >= progress.sizeTotal))
        {
            MCP_Close(ctx->mcp_fd);
            ctx->mcp_fd = -1;
            remove_directory_recursive(ctx->dl_stage_dir);

            ctx->titles[ctx->dl_title_index].is_installed = true;
            ctx->queue_completed_batch++;
            refresh_storage_space(ctx);

            if(ctx->queue_count > 0)
            {
                start_next_queued_download(ctx);
                return;
            }

            refresh_installed_status(ctx);
            ctx->state = APP_STATE_COMPLETED;
            snprintf(
                ctx->status_message,
                sizeof(ctx->status_message),
                "Installed '%s' to %s! It is now ready on your Wii U Home Screen.",
                ctx->titles[ctx->dl_title_index].name,
                target_label);
        }
    }
}

static void step_download_pipeline(AppContext *ctx)
{
    if(ctx->state != APP_STATE_DOWNLOADING || !ctx->curl_multi || !ctx->curl_easy)
    {
        return;
    }

    int still_running = 0;
    for(int burst = 0; burst < 12; burst++)
    {
        CURLMcode mc = curl_multi_perform(ctx->curl_multi, &still_running);
        if(mc != CURLM_OK || still_running == 0)
        {
            break;
        }
    }

    ctx->dl_current_file_bytes = ctx->dl_resume_offset + ctx->stream_ctx.bytes_written;
    uint64_t aggregate_done = ctx->dl_completed_files_bytes + ctx->dl_current_file_bytes;

    /* Update smoothed transfer rate every ~250 ms */
    OSTick now_tick = OSGetTick();
    uint64_t elapsed_ms = OSTicksToMilliseconds(now_tick - ctx->dl_last_speed_tick);
    if(elapsed_ms >= 250)
    {
        uint64_t delta_bytes = (aggregate_done >= ctx->dl_last_speed_bytes)
            ? (aggregate_done - ctx->dl_last_speed_bytes)
            : 0;
        double inst_bps = ((double)delta_bytes * 1000.0) / (double)elapsed_ms;
        if(ctx->dl_speed_bps <= 1.0)
        {
            ctx->dl_speed_bps = inst_bps;
        }
        else
        {
            ctx->dl_speed_bps = ctx->dl_speed_bps * 0.75 + inst_bps * 0.25;
        }
        ctx->dl_last_speed_tick = now_tick;
        ctx->dl_last_speed_bytes = aggregate_done;
    }

    char done_str[32], total_str[32], speed_str[32], eta_str[32];
    format_bytes(aggregate_done, done_str, sizeof(done_str));
    format_bytes(ctx->dl_total_bytes, total_str, sizeof(total_str));
    format_bytes((uint64_t)ctx->dl_speed_bps, speed_str, sizeof(speed_str));

    if(ctx->dl_speed_bps > 1024.0 && ctx->dl_total_bytes > aggregate_done)
    {
        uint64_t rem_sec = (uint64_t)((double)(ctx->dl_total_bytes - aggregate_done) / ctx->dl_speed_bps);
        snprintf(eta_str, sizeof(eta_str), "ETA %02llu:%02llu", rem_sec / 60ULL, rem_sec % 60ULL);
    }
    else
    {
        strncpy(eta_str, "ETA --:--", sizeof(eta_str) - 1);
        eta_str[sizeof(eta_str) - 1] = '\0';
    }

    TitleMetadata *title = &ctx->titles[ctx->dl_title_index];
    snprintf(
        ctx->status_message,
        sizeof(ctx->status_message),
        "Downloading [%d/%d] %s — %s / %s (%s/s, %s)",
        ctx->dl_file_index + 1,
        title->file_count,
        ctx->dl_current_filename,
        done_str,
        total_str,
        speed_str,
        eta_str);

    int msgs_left = 0;
    CURLMsg *msg = NULL;
    while((msg = curl_multi_info_read(ctx->curl_multi, &msgs_left)) != NULL)
    {
        if(msg->msg == CURLMSG_DONE)
        {
            CURLcode result = msg->data.result;
            long http_code = 0;
            curl_easy_getinfo(ctx->curl_easy, CURLINFO_RESPONSE_CODE, &http_code);

            bool write_err = ctx->stream_ctx.write_error;
            uint64_t file_bytes = ctx->dl_resume_offset + ctx->stream_ctx.bytes_written;

            cleanup_active_download_handles(ctx);

            if(result != CURLE_OK || (http_code != 200 && http_code != 206) || write_err)
            {
                if(!write_err && ctx->dl_retry_count < MAX_FILE_RETRIES)
                {
                    ctx->dl_retry_count++;
                    start_next_file_download(ctx, true);
                    return;
                }

                remove_directory_recursive(ctx->dl_stage_dir);
                snprintf(
                    ctx->status_message,
                    sizeof(ctx->status_message),
                    "Download failed on %s (%s, HTTP %ld). Cleaned up SD staging.",
                    ctx->dl_current_filename,
                    curl_easy_strerror(result),
                    http_code);
                ctx->state = APP_STATE_ERROR;
                return;
            }

            ctx->dl_completed_files_bytes += file_bytes;
            ctx->dl_current_file_bytes = 0;
            ctx->dl_resume_offset = 0;
            ctx->dl_file_index++;

            if(ctx->dl_file_index < title->file_count)
            {
                start_next_file_download(ctx, false);
            }
            else
            {
                begin_mcp_installation(ctx);
            }
            break;
        }
    }
}

static void cancel_active_operation(AppContext *ctx)
{
    if(ctx->state == APP_STATE_DOWNLOADING)
    {
        ctx->state = APP_STATE_CANCELLED;
        cleanup_active_download_handles(ctx);
        remove_directory_recursive(ctx->dl_stage_dir);
        cleanup_incomplete_import_dirs(ctx);
        refresh_storage_space(ctx);
        ctx->queue_count = 0;
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Download cancelled. Removed staged files from SD card.");
        ctx->state = APP_STATE_BROWSING;
    }
}

/* --------------------------------------------------------------------------
 * 3. Visual Design (eShop Aero Glass Aesthetic via SDL2)
 * -------------------------------------------------------------------------- */

static void draw_filled_rounded_rect(
    SDL_Renderer *renderer,
    int x,
    int y,
    int w,
    int h,
    int r,
    SDL_Color color)
{
    if(r <= 0 || w < 2 * r || h < 2 * r)
    {
        SDL_Rect rect = { x, y, w, h };
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    SDL_Rect center_rect = { x, y + r, w, h - 2 * r };
    SDL_RenderFillRect(renderer, &center_rect);

    for(int dy = 0; dy < r; dy++)
    {
        int cy = r - dy;
        int dx = (int)(sqrt((double)(r * r - cy * cy)) + 0.5);
        int line_x = x + r - dx;
        int line_w = w - 2 * (r - dx);

        SDL_Rect top_span = { line_x, y + dy, line_w, 1 };
        SDL_Rect bot_span = { line_x, y + h - 1 - dy, line_w, 1 };
        SDL_RenderFillRect(renderer, &top_span);
        SDL_RenderFillRect(renderer, &bot_span);
    }
}

static void draw_rounded_rect_outline(
    SDL_Renderer *renderer,
    int x,
    int y,
    int w,
    int h,
    int r,
    SDL_Color color)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    SDL_RenderDrawLine(renderer, x + r, y, x + w - r - 1, y);
    SDL_RenderDrawLine(renderer, x + r, y + h - 1, x + w - r - 1, y + h - 1);
    SDL_RenderDrawLine(renderer, x, y + r, x, y + h - r - 1);
    SDL_RenderDrawLine(renderer, x + w - 1, y + r, x + w - 1, y + h - r - 1);

    for(int dy = 0; dy < r; dy++)
    {
        int cy = r - dy;
        int dx = (int)(sqrt((double)(r * r - cy * cy)) + 0.5);

        SDL_RenderDrawPoint(renderer, x + r - dx, y + dy);
        SDL_RenderDrawPoint(renderer, x + w - 1 - r + dx, y + dy);
        SDL_RenderDrawPoint(renderer, x + r - dx, y + h - 1 - dy);
        SDL_RenderDrawPoint(renderer, x + w - 1 - r + dx, y + h - 1 - dy);
    }
}

static void draw_aero_glass_card(
    SDL_Renderer *renderer,
    int x,
    int y,
    int w,
    int h,
    int radius,
    uint8_t fill_alpha,
    bool is_selected)
{
    SDL_Color shadow_outer = { 0, 0, 0, 55 };
    SDL_Color shadow_inner = { 0, 0, 0, 95 };
    draw_filled_rounded_rect(renderer, x + 4, y + 6, w, h, radius, shadow_outer);
    draw_filled_rounded_rect(renderer, x + 2, y + 3, w, h, radius, shadow_inner);

    SDL_Color glass_fill = { 255, 255, 255, fill_alpha };
    draw_filled_rounded_rect(renderer, x, y, w, h, radius, glass_fill);

    SDL_Color gloss_band = { 255, 255, 255, 14 };
    SDL_Rect gloss_rect = { x + radius / 2, y + 2, w - radius, h / 2 - 2 };
    SDL_SetRenderDrawColor(renderer, gloss_band.r, gloss_band.g, gloss_band.b, gloss_band.a);
    SDL_RenderFillRect(renderer, &gloss_rect);

    SDL_Color rim_color = { 180, 210, 240, 45 };
    draw_rounded_rect_outline(renderer, x, y, w, h, radius, rim_color);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
    SDL_RenderDrawLine(renderer, x + radius, y, x + w - radius - 1, y);
    SDL_RenderDrawLine(renderer, x, y + radius, x, y + h - radius - 1);
    for(int dy = 0; dy < radius; dy++)
    {
        int cy = radius - dy;
        int dx = (int)(sqrt((double)(radius * radius - cy * cy)) + 0.5);
        SDL_RenderDrawPoint(renderer, x + radius - dx, y + dy);
    }

    if(is_selected)
    {
        SDL_Color glow_outer = { 0, 195, 255, 70 };
        SDL_Color glow_mid = { 0, 215, 255, 140 };
        SDL_Color glow_core = { 80, 235, 255, 235 };
        draw_rounded_rect_outline(renderer, x - 2, y - 2, w + 4, h + 4, radius + 2, glow_outer);
        draw_rounded_rect_outline(renderer, x - 1, y - 1, w + 2, h + 2, radius + 1, glow_mid);
        draw_rounded_rect_outline(renderer, x, y, w, h, radius, glow_core);
    }
}

static void draw_aero_background(SDL_Renderer *renderer)
{
    for(int y = 0; y < SCREEN_HEIGHT; y += 2)
    {
        float t = (float)y / (float)SCREEN_HEIGHT;
        uint8_t r = (uint8_t)(12.0f + t * 10.0f);
        uint8_t g = (uint8_t)(20.0f + t * 22.0f);
        uint8_t b = (uint8_t)(34.0f + t * 38.0f);
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_Rect band = { 0, y, SCREEN_WIDTH, 2 };
        SDL_RenderFillRect(renderer, &band);
    }

    SDL_Color bloom1 = { 0, 180, 255, 12 };
    SDL_Color bloom2 = { 120, 210, 255, 10 };
    draw_filled_rounded_rect(renderer, -80, -40, 620, 260, 80, bloom1);
    draw_filled_rounded_rect(renderer, 740, 420, 620, 320, 90, bloom2);
}

static void render_text(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const char *text,
    int x,
    int y,
    SDL_Color color,
    bool align_right)
{
    if(!font || !text || !text[0])
    {
        return;
    }

    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if(!surf)
    {
        return;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    if(tex)
    {
        SDL_Rect dst = {
            align_right ? (x - surf->w) : x,
            y,
            surf->w,
            surf->h
        };
        SDL_RenderCopy(renderer, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void render_details_modal(AppContext *ctx)
{
    int title_idx = get_selected_title_index(ctx);
    if(title_idx < 0)
    {
        return;
    }
    TitleMetadata *t = &ctx->titles[title_idx];
    SDL_Renderer *renderer = ctx->renderer;

    /* Dark backdrop */
    SDL_Rect full = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
    SDL_SetRenderDrawColor(renderer, 4, 8, 16, 195);
    SDL_RenderFillRect(renderer, &full);

    int mx = 160, my = 100, mw = SCREEN_WIDTH - 320, mh = 500;
    draw_aero_glass_card(renderer, mx, my, mw, mh, 18, 55, true);

    SDL_Color white_primary = { 245, 250, 255, 255 };
    SDL_Color white_secondary = { 180, 205, 230, 225 };
    SDL_Color cyan_accent = { 70, 225, 255, 255 };
    SDL_Color green_accent = { 80, 245, 165, 255 };

    render_text(renderer, ctx->font_title, t->name, mx + 36, my + 28, white_primary, false);

    char subline[256];
    snprintf(
        subline,
        sizeof(subline),
        "Title ID: %s   |   Category: %s   |   Region: %s   |   Format: %s",
        t->title_id,
        t->category,
        t->region,
        t->source_format);
    render_text(renderer, ctx->font_small, subline, mx + 36, my + 78, cyan_accent, false);

    char size_s[32];
    format_bytes(t->total_size_bytes, size_s, sizeof(size_s));
    snprintf(
        subline,
        sizeof(subline),
        "Version: v%u   |   Package Files: %d   |   Total Size: %s   |   Product: %s",
        t->version,
        t->file_count,
        size_s,
        t->product_code[0] ? t->product_code : "N/A");
    render_text(renderer, ctx->font_body, subline, mx + 36, my + 125, white_primary, false);

    render_text(
        renderer,
        ctx->font_body,
        t->is_installed
            ? "Console Status: INSTALLED on Wii U Home Screen"
            : "Console Status: Not Currently Installed",
        mx + 36,
        my + 170,
        t->is_installed ? green_accent : white_secondary,
        false);

    char target_line[256];
    char free_s[32];
    format_bytes(get_active_target_free_bytes(ctx), free_s, sizeof(free_s));
    snprintf(
        target_line,
        sizeof(target_line),
        "Install Destination: %s (%s available) — Press Y to switch USB/NAND",
        (ctx->install_target == MCP_INSTALL_TARGET_USB) ? "USB Storage" : "System Memory (NAND)",
        free_s);
    render_text(renderer, ctx->font_body, target_line, mx + 36, my + 215, cyan_accent, false);

    /* Companion Update / DLC detection for base games */
    int upd_idx = -1, dlc_idx = -1;
    if(strcasecmp(t->category, "Game") == 0)
    {
        upd_idx = find_companion_title(ctx, t->tid_u64, 0x0005000E);
        dlc_idx = find_companion_title(ctx, t->tid_u64, 0x0005000C);
    }

    if(upd_idx >= 0 || dlc_idx >= 0)
    {
        char bundle_line[256];
        snprintf(
            bundle_line,
            sizeof(bundle_line),
            "Companion Content Available on Server: %s%s%s",
            (upd_idx >= 0) ? "Update " : "",
            (upd_idx >= 0 && dlc_idx >= 0) ? "+ " : "",
            (dlc_idx >= 0) ? "DLC" : "");
        render_text(renderer, ctx->font_body, bundle_line, mx + 36, my + 265, green_accent, false);
        render_text(
            renderer,
            ctx->font_small,
            "Press X to Download & Install Game + Update + DLC Bundle to Home Screen!",
            mx + 36,
            my + 300,
            white_primary,
            false);
    }

    /* Action buttons footer inside modal */
    draw_aero_glass_card(renderer, mx + 28, my + mh - 86, mw - 56, 58, 12, 40, false);
    render_text(
        renderer,
        ctx->font_small,
        "A: Download & Install Now   |   +: Add/Remove Queue   |   Y: Toggle USB/NAND   |   B: Back",
        mx + 48,
        my + mh - 66,
        white_primary,
        false);
}

static void render_settings_modal(AppContext *ctx)
{
    SDL_Renderer *renderer = ctx->renderer;

    SDL_Rect full = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
    SDL_SetRenderDrawColor(renderer, 4, 8, 16, 195);
    SDL_RenderFillRect(renderer, &full);

    int mx = 220, my = 140, mw = SCREEN_WIDTH - 440, mh = 420;
    draw_aero_glass_card(renderer, mx, my, mw, mh, 18, 55, true);

    SDL_Color white_primary = { 245, 250, 255, 255 };
    SDL_Color white_secondary = { 180, 205, 230, 225 };
    SDL_Color cyan_accent = { 70, 225, 255, 255 };

    render_text(renderer, ctx->font_title, "LocaLeShop Server Settings", mx + 36, my + 28, cyan_accent, false);
    render_text(
        renderer,
        ctx->font_small,
        "Use D-Pad Left/Right to select octet/port, Up/Down (+/-1), L/R (+/-10), or X to Auto-Discover.",
        mx + 36,
        my + 76,
        white_secondary,
        false);

    const char *labels[5] = { "IP 1", "IP 2", "IP 3", "IP 4", "Port" };
    int vals[5] = {
        ctx->edit_octets[0],
        ctx->edit_octets[1],
        ctx->edit_octets[2],
        ctx->edit_octets[3],
        ctx->edit_port
    };

    for(int i = 0; i < 5; i++)
    {
        int bx = mx + 36 + i * 155;
        int by = my + 145;
        int bw = (i == 4) ? 145 : 130;
        int bh = 95;
        bool sel = (ctx->edit_field == i);

        draw_aero_glass_card(renderer, bx, by, bw, bh, 12, sel ? 60 : 30, sel);
        render_text(renderer, ctx->font_small, labels[i], bx + 18, by + 14, white_secondary, false);

        char vstr[32];
        snprintf(vstr, sizeof(vstr), "%d", vals[i]);
        render_text(renderer, ctx->font_title, vstr, bx + 18, by + 44, sel ? cyan_accent : white_primary, false);
    }

    draw_aero_glass_card(renderer, mx + 28, my + mh - 86, mw - 56, 58, 12, 40, false);
    render_text(
        renderer,
        ctx->font_small,
        "A: Save & Connect   |   X: Auto-Discover Server on LAN   |   B: Cancel",
        mx + 56,
        my + mh - 66,
        white_primary,
        false);
}

static void render_ui(AppContext *ctx)
{
    SDL_Renderer *renderer = ctx->renderer;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    draw_aero_background(renderer);

    SDL_Color white_primary = { 245, 250, 255, 255 };
    SDL_Color white_secondary = { 180, 205, 230, 225 };
    SDL_Color cyan_accent = { 70, 225, 255, 255 };
    SDL_Color green_accent = { 80, 245, 165, 255 };
    SDL_Color gold_accent = { 255, 215, 85, 255 };

    /* 1. Top Aero Glass Header Bar */
    draw_aero_glass_card(renderer, 36, 14, SCREEN_WIDTH - 72, 66, 14, 34, false);
    render_text(renderer, ctx->font_title, "LocaLeShop", 60, 26, cyan_accent, false);

    int logo_w = 240;
    if(ctx->font_title)
    {
        TTF_SizeUTF8(ctx->font_title, "LocaLeShop", &logo_w, NULL);
    }

    char target_badge[192];
    char usb_free_str[32], nand_free_str[32];
    format_bytes(ctx->usb_free_bytes, usb_free_str, sizeof(usb_free_str));
    format_bytes(ctx->nand_free_bytes, nand_free_str, sizeof(nand_free_str));
    if(ctx->usb_mounted)
    {
        snprintf(
            target_badge,
            sizeof(target_badge),
            "Target: %s [Y]  |  USB: %s  |  NAND: %s  |  Queue: %d [+]",
            (ctx->install_target == MCP_INSTALL_TARGET_USB) ? "USB" : "NAND",
            usb_free_str,
            nand_free_str,
            ctx->queue_count);
    }
    else
    {
        snprintf(
            target_badge,
            sizeof(target_badge),
            "Target: NAND (%s free) [Y]   |   Queue: %d [+]",
            nand_free_str,
            ctx->queue_count);
    }
    render_text(renderer, ctx->font_small, target_badge, 60 + logo_w + 38, 36, white_primary, false);

    char server_badge[128];
    snprintf(server_badge, sizeof(server_badge), "Server: %s:%d [-]", ctx->server_ip, ctx->server_port);
    render_text(renderer, ctx->font_small, server_badge, SCREEN_WIDTH - 58, 36, white_secondary, true);

    /* 2. Category Filter Tabs Bar (L / R) */
    const char *tab_names[CAT_TAB_COUNT] = {
        "All Titles",
        "Games",
        "Updates",
        "DLC",
        "Demos"
    };
    int tab_w = 224;
    int tab_gap = 16;
    int tab_start_x = CARD_MARGIN_X;
    int tab_y = 88;
    for(int t_i = 0; t_i < CAT_TAB_COUNT; t_i++)
    {
        bool is_active = ((int)ctx->active_tab == t_i);
        int tx = tab_start_x + t_i * (tab_w + tab_gap);
        draw_aero_glass_card(renderer, tx, tab_y, tab_w, 38, 10, is_active ? 50 : 22, is_active);
        render_text(
            renderer,
            ctx->font_small,
            tab_names[t_i],
            tx + 22,
            tab_y + 9,
            is_active ? cyan_accent : white_secondary,
            false);
    }

    /* 3. Title List Cards */
    if(ctx->filtered_count == 0)
    {
        draw_aero_glass_card(renderer, CARD_MARGIN_X, 210, SCREEN_WIDTH - 2 * CARD_MARGIN_X, 170, 16, 30, false);
        render_text(
            renderer,
            ctx->font_body,
            (ctx->title_count == 0)
                ? "No Wii U titles loaded from LocaLeShop server."
                : "No titles in this category tab.",
            CARD_MARGIN_X + 36,
            248,
            white_primary,
            false);
        render_text(
            renderer,
            ctx->font_small,
            "Press A to refresh, X to Auto-Discover server on LAN, - for Server IP Settings, or L/R to switch tabs.",
            CARD_MARGIN_X + 36,
            296,
            white_secondary,
            false);
    }
    else
    {
        int end_pos = ctx->scroll_offset + VISIBLE_CARDS;
        if(end_pos > ctx->filtered_count)
        {
            end_pos = ctx->filtered_count;
        }

        for(int pos = ctx->scroll_offset; pos < end_pos; pos++)
        {
            int row = pos - ctx->scroll_offset;
            int card_x = CARD_MARGIN_X;
            int card_y = CARD_START_Y + row * (CARD_HEIGHT + CARD_GAP_Y);
            int card_w = SCREEN_WIDTH - 2 * CARD_MARGIN_X;
            bool is_sel = (pos == ctx->selected_pos);

            int title_idx = ctx->filtered_indices[pos];
            TitleMetadata *t = &ctx->titles[title_idx];
            int q_pos = find_in_queue(ctx, title_idx);

            uint8_t alpha = is_sel ? 46 : 25;
            draw_aero_glass_card(renderer, card_x, card_y, card_w, CARD_HEIGHT, CARD_CORNER_RADIUS, alpha, is_sel);

            if(is_sel)
            {
                SDL_Color pill_color = { 0, 215, 255, 230 };
                draw_filled_rounded_rect(renderer, card_x + 12, card_y + 16, 6, CARD_HEIGHT - 32, 3, pill_color);
            }

            render_text(
                renderer,
                ctx->font_body,
                t->name,
                card_x + 30,
                card_y + 13,
                is_sel ? white_primary : white_secondary,
                false);

            char meta_line[256];
            snprintf(
                meta_line,
                sizeof(meta_line),
                "[%s]  %s  |  %s  |  Title ID: %s  |  v%u  |  %d file(s)",
                t->source_format,
                t->category,
                t->region,
                t->title_id,
                t->version,
                t->file_count);
            render_text(
                renderer,
                ctx->font_small,
                meta_line,
                card_x + 30,
                card_y + 47,
                is_sel ? cyan_accent : white_secondary,
                false);

            /* Right-side status badges (INSTALLED / QUEUED) + Size */
            char size_badge[64];
            format_bytes(t->total_size_bytes, size_badge, sizeof(size_badge));
            render_text(
                renderer,
                ctx->font_body,
                size_badge,
                card_x + card_w - 28,
                card_y + 13,
                is_sel ? cyan_accent : white_primary,
                true);

            if(q_pos >= 0)
            {
                char q_badge[48];
                snprintf(q_badge, sizeof(q_badge), "[QUEUED #%d]", q_pos + 1);
                render_text(
                    renderer,
                    ctx->font_small,
                    q_badge,
                    card_x + card_w - 28,
                    card_y + 47,
                    gold_accent,
                    true);
            }
            else if(t->is_installed)
            {
                render_text(
                    renderer,
                    ctx->font_small,
                    "[INSTALLED ON HOME SCREEN]",
                    card_x + card_w - 28,
                    card_y + 47,
                    green_accent,
                    true);
            }
        }
    }

    /* 4. Bottom Translucent Glass Status Bar & Progress Meter */
    int status_x = 36;
    int status_y = SCREEN_HEIGHT - 126;
    int status_w = SCREEN_WIDTH - 72;
    int status_h = 110;
    draw_aero_glass_card(renderer, status_x, status_y, status_w, status_h, 14, 38, false);

    /* Line 1: Active status message */
    render_text(renderer, ctx->font_small, ctx->status_message, status_x + 24, status_y + 12, white_primary, false);

    /* Line 2: Dedicated button controls reminder bar */
    const char *controls_hint = "A: Details/Install   |   +: Queue   |   Y: USB/NAND   |   L/R: Tabs   |   X: Find Server   |   -: IP Setup   |   HOME: Exit";
    render_text(
        renderer,
        ctx->font_small,
        controls_hint,
        status_x + 24,
        status_y + 40,
        white_secondary,
        false);

    /* Line 3: Progress bar track */
    int bar_x = status_x + 24;
    int bar_y = status_y + 72;
    int bar_w = status_w - 48;
    int bar_h = 22;

    SDL_Color track_fill = { 10, 18, 30, 140 };
    SDL_Color track_rim = { 255, 255, 255, 70 };
    draw_filled_rounded_rect(renderer, bar_x, bar_y, bar_w, bar_h, 8, track_fill);
    draw_rounded_rect_outline(renderer, bar_x, bar_y, bar_w, bar_h, 8, track_rim);

    double progress_ratio = 0.0;
    if(ctx->state == APP_STATE_DOWNLOADING)
    {
        uint64_t done = ctx->dl_completed_files_bytes + ctx->dl_current_file_bytes;
        if(ctx->dl_total_bytes > 0)
        {
            progress_ratio = (double)done / (double)ctx->dl_total_bytes;
        }
        else if(ctx->titles[ctx->dl_title_index].file_count > 0)
        {
            progress_ratio = (double)ctx->dl_file_index / (double)ctx->titles[ctx->dl_title_index].file_count;
        }
    }
    else if(ctx->state == APP_STATE_INSTALLING)
    {
        if(ctx->mcp_progress.sizeTotal > 0)
        {
            progress_ratio = (double)ctx->mcp_progress.sizeProgress / (double)ctx->mcp_progress.sizeTotal;
        }
    }
    else if(ctx->state == APP_STATE_COMPLETED)
    {
        progress_ratio = 1.0;
    }

    if(progress_ratio < 0.0)
        progress_ratio = 0.0;
    if(progress_ratio > 1.0)
        progress_ratio = 1.0;

    int fill_w = (int)((double)bar_w * progress_ratio);
    if(fill_w >= 16)
    {
        SDL_Color fill_color = (ctx->state == APP_STATE_INSTALLING)
            ? (SDL_Color) { 60, 240, 180, 210 }
            : (SDL_Color) { 0, 205, 255, 215 };
        draw_filled_rounded_rect(renderer, bar_x, bar_y, fill_w, bar_h, 8, fill_color);

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 130);
        SDL_RenderDrawLine(renderer, bar_x + 6, bar_y + 2, bar_x + fill_w - 6, bar_y + 2);
    }

    /* 5. Modal Overlays */
    if(ctx->state == APP_STATE_DETAILS_MODAL)
    {
        render_details_modal(ctx);
    }
    else if(ctx->state == APP_STATE_SETTINGS_MODAL)
    {
        render_settings_modal(ctx);
    }

    SDL_RenderPresent(renderer);
}

/* --------------------------------------------------------------------------
 * Input Handling (Wii U GamePad VPAD + SDL2 Events)
 * -------------------------------------------------------------------------- */

static void move_selection(AppContext *ctx, int delta)
{
    if(ctx->filtered_count <= 0)
    {
        return;
    }
    ctx->selected_pos += delta;
    if(ctx->selected_pos < 0)
    {
        ctx->selected_pos = 0;
    }
    else if(ctx->selected_pos >= ctx->filtered_count)
    {
        ctx->selected_pos = ctx->filtered_count - 1;
    }

    if(ctx->selected_pos < ctx->scroll_offset)
    {
        ctx->scroll_offset = ctx->selected_pos;
    }
    else if(ctx->selected_pos >= ctx->scroll_offset + VISIBLE_CARDS)
    {
        ctx->scroll_offset = ctx->selected_pos - VISIBLE_CARDS + 1;
    }
}

static void toggle_install_target(AppContext *ctx)
{
    refresh_storage_space(ctx);
    if(ctx->install_target == MCP_INSTALL_TARGET_USB)
    {
        ctx->install_target = MCP_INSTALL_TARGET_MLC;
        char free_s[32];
        format_bytes(ctx->nand_free_bytes, free_s, sizeof(free_s));
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Install destination set to System Memory (NAND — %s free).",
            free_s);
    }
    else
    {
        if(!ctx->usb_mounted)
        {
            snprintf(
                ctx->status_message,
                sizeof(ctx->status_message),
                "No Wii U USB storage device detected; keeping System Memory (NAND).");
            ctx->install_target = MCP_INSTALL_TARGET_MLC;
        }
        else
        {
            ctx->install_target = MCP_INSTALL_TARGET_USB;
            char free_s[32];
            format_bytes(ctx->usb_free_bytes, free_s, sizeof(free_s));
            snprintf(
                ctx->status_message,
                sizeof(ctx->status_message),
                "Install destination set to USB Storage (%s free).",
                free_s);
        }
    }
    save_server_config(ctx);
}

static void adjust_settings_field(AppContext *ctx, int step)
{
    if(ctx->edit_field >= 0 && ctx->edit_field < 4)
    {
        int v = ctx->edit_octets[ctx->edit_field] + step;
        if(v < 0)
            v = 255;
        if(v > 255)
            v = 0;
        ctx->edit_octets[ctx->edit_field] = v;
    }
    else if(ctx->edit_field == 4)
    {
        int p = ctx->edit_port + step;
        if(p < 1)
            p = 65535;
        if(p > 65535)
            p = 1;
        ctx->edit_port = p;
    }
}

static bool handle_input(AppContext *ctx)
{
    bool nav_up = false;
    bool nav_down = false;
    bool nav_left = false;
    bool nav_right = false;
    bool press_a = false;
    bool press_b = false;
    bool press_x = false;
    bool press_y = false;
    bool press_l = false;
    bool press_r = false;
    bool press_plus = false;
    bool press_minus = false;
    bool press_home = false;

    VPADStatus vpad_data;
    VPADReadError vpad_err = VPAD_READ_UNINITIALIZED;
    VPADRead(VPAD_CHAN_0, &vpad_data, 1, &vpad_err);
    if(vpad_err == VPAD_READ_SUCCESS)
    {
        uint32_t trig = vpad_data.trigger;
        if(trig & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP))
            nav_up = true;
        if(trig & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN))
            nav_down = true;
        if(trig & (VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT))
            nav_left = true;
        if(trig & (VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT))
            nav_right = true;
        if(trig & VPAD_BUTTON_A)
            press_a = true;
        if(trig & VPAD_BUTTON_B)
            press_b = true;
        if(trig & VPAD_BUTTON_X)
            press_x = true;
        if(trig & VPAD_BUTTON_Y)
            press_y = true;
        if(trig & (VPAD_BUTTON_L | VPAD_BUTTON_ZL))
            press_l = true;
        if(trig & (VPAD_BUTTON_R | VPAD_BUTTON_ZR))
            press_r = true;
        if(trig & VPAD_BUTTON_PLUS)
            press_plus = true;
        if(trig & VPAD_BUTTON_MINUS)
            press_minus = true;
        if(trig & VPAD_BUTTON_HOME)
            press_home = true;
    }

    SDL_Event ev;
    while(SDL_PollEvent(&ev))
    {
        if(ev.type == SDL_QUIT)
        {
            return false;
        }
        if(ev.type == SDL_KEYDOWN)
        {
            switch(ev.key.keysym.sym)
            {
                case SDLK_UP:
                    nav_up = true;
                    break;
                case SDLK_DOWN:
                    nav_down = true;
                    break;
                case SDLK_LEFT:
                    nav_left = true;
                    break;
                case SDLK_RIGHT:
                    nav_right = true;
                    break;
                case SDLK_RETURN:
                    press_a = true;
                    break;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE:
                    press_b = true;
                    break;
                case SDLK_x:
                    press_x = true;
                    break;
                case SDLK_y:
                    press_y = true;
                    break;
                case SDLK_q:
                    press_l = true;
                    break;
                case SDLK_e:
                    press_r = true;
                    break;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                    press_plus = true;
                    break;
                case SDLK_MINUS:
                    press_minus = true;
                    break;
                case SDLK_HOME:
                    press_home = true;
                    break;
                default:
                    break;
            }
        }
    }

    if(press_home)
    {
        if(ctx->state == APP_STATE_DOWNLOADING)
        {
            cancel_active_operation(ctx);
        }
        SYSLaunchMenu();
        return false;
    }

    /* 1. Server Settings Modal Input */
    if(ctx->state == APP_STATE_SETTINGS_MODAL)
    {
        if(nav_left)
        {
            ctx->edit_field = (ctx->edit_field + 4) % 5;
        }
        else if(nav_right)
        {
            ctx->edit_field = (ctx->edit_field + 1) % 5;
        }
        else if(nav_up)
        {
            adjust_settings_field(ctx, 1);
        }
        else if(nav_down)
        {
            adjust_settings_field(ctx, -1);
        }
        else if(press_r)
        {
            adjust_settings_field(ctx, 10);
        }
        else if(press_l)
        {
            adjust_settings_field(ctx, -10);
        }
        else if(press_x)
        {
            snprintf(ctx->status_message, sizeof(ctx->status_message), "Searching LAN for LocaLeShop server...");
            render_ui(ctx);
            if(discover_server_on_lan(ctx))
            {
                parse_ip_to_octets(ctx->server_ip, ctx->edit_octets);
                ctx->edit_port = ctx->server_port;
                fetch_titles_catalog(ctx, false);
            }
            else
            {
                snprintf(ctx->status_message, sizeof(ctx->status_message), "No server responded to UDP LAN discovery.");
            }
        }
        else if(press_a)
        {
            snprintf(
                ctx->server_ip,
                sizeof(ctx->server_ip),
                "%d.%d.%d.%d",
                ctx->edit_octets[0],
                ctx->edit_octets[1],
                ctx->edit_octets[2],
                ctx->edit_octets[3]);
            ctx->server_port = ctx->edit_port;
            save_server_config(ctx);
            ctx->state = APP_STATE_BROWSING;
            fetch_titles_catalog(ctx, false);
        }
        else if(press_b || press_minus)
        {
            ctx->state = APP_STATE_BROWSING;
        }
        return true;
    }

    /* 2. Title Details Modal Input */
    if(ctx->state == APP_STATE_DETAILS_MODAL)
    {
        int title_idx = get_selected_title_index(ctx);
        if(press_b)
        {
            ctx->state = APP_STATE_BROWSING;
        }
        else if(press_y)
        {
            toggle_install_target(ctx);
        }
        else if(press_plus && title_idx >= 0)
        {
            toggle_queue_title(ctx, title_idx);
        }
        else if(press_x && title_idx >= 0)
        {
            /* Bundle Game + Update + DLC into queue and start */
            TitleMetadata *t = &ctx->titles[title_idx];
            enqueue_title(ctx, title_idx);
            int upd_idx = find_companion_title(ctx, t->tid_u64, 0x0005000E);
            int dlc_idx = find_companion_title(ctx, t->tid_u64, 0x0005000C);
            if(upd_idx >= 0)
                enqueue_title(ctx, upd_idx);
            if(dlc_idx >= 0)
                enqueue_title(ctx, dlc_idx);
            ctx->queue_total_batch = ctx->queue_count;
            ctx->queue_completed_batch = 0;
            start_next_queued_download(ctx);
        }
        else if(press_a && title_idx >= 0)
        {
            if(ctx->queue_count > 0)
            {
                if(find_in_queue(ctx, title_idx) < 0)
                {
                    enqueue_title(ctx, title_idx);
                }
                ctx->queue_total_batch = ctx->queue_count;
                ctx->queue_completed_batch = 0;
                start_next_queued_download(ctx);
            }
            else
            {
                begin_title_download(ctx, title_idx);
            }
        }
        return true;
    }

    /* 3. Main Catalog Browsing Input */
    if(ctx->state == APP_STATE_BROWSING || ctx->state == APP_STATE_COMPLETED || ctx->state == APP_STATE_ERROR)
    {
        if(nav_up)
        {
            move_selection(ctx, -1);
        }
        else if(nav_down)
        {
            move_selection(ctx, 1);
        }
        else if(nav_left)
        {
            move_selection(ctx, -VISIBLE_CARDS);
        }
        else if(nav_right)
        {
            move_selection(ctx, VISIBLE_CARDS);
        }
        else if(press_l)
        {
            ctx->active_tab = (CategoryTab)(((int)ctx->active_tab + CAT_TAB_COUNT - 1) % CAT_TAB_COUNT);
            rebuild_filtered_list(ctx);
        }
        else if(press_r)
        {
            ctx->active_tab = (CategoryTab)(((int)ctx->active_tab + 1) % CAT_TAB_COUNT);
            rebuild_filtered_list(ctx);
        }
        else if(press_y)
        {
            toggle_install_target(ctx);
        }
        else if(press_minus)
        {
            parse_ip_to_octets(ctx->server_ip, ctx->edit_octets);
            ctx->edit_port = ctx->server_port;
            ctx->edit_field = 3;
            ctx->state = APP_STATE_SETTINGS_MODAL;
        }
        else if(press_x)
        {
            snprintf(ctx->status_message, sizeof(ctx->status_message), "Auto-discovering LocaLeShop server on LAN...");
            render_ui(ctx);
            discover_server_on_lan(ctx);
            fetch_titles_catalog(ctx, false);
        }
        else if(press_plus)
        {
            int title_idx = get_selected_title_index(ctx);
            if(title_idx >= 0)
            {
                toggle_queue_title(ctx, title_idx);
            }
        }
        else if(press_a)
        {
            if(ctx->title_count == 0)
            {
                fetch_titles_catalog(ctx, true);
            }
            else if(ctx->filtered_count > 0)
            {
                ctx->state = APP_STATE_DETAILS_MODAL;
            }
        }
    }
    else if(ctx->state == APP_STATE_DOWNLOADING)
    {
        if(press_b)
        {
            cancel_active_operation(ctx);
        }
    }

    return true;
}

/* --------------------------------------------------------------------------
 * Initialization & Teardown
 * -------------------------------------------------------------------------- */

static TTF_Font *open_ui_font(int pt_size, bool prefer_display_font)
{
    if(prefer_display_font)
    {
        const char *display_candidates[] = {
            ROMFS_FONT_SEMIBOLD,
            ROMFS_FONT_REGULAR,
            ROMFS_FONT_VARIABLE
        };
        for(size_t i = 0; i < sizeof(display_candidates) / sizeof(display_candidates[0]); i++)
        {
            TTF_Font *f = TTF_OpenFont(display_candidates[i], pt_size);
            if(f)
            {
                return f;
            }
        }
    }

    /* Load Wii U built-in shared CafeStd system font directly from OS memory */
    void *shared_ttf = NULL;
    uint32_t shared_size = 0;
    if(OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &shared_ttf, &shared_size) && shared_ttf && shared_size > 0)
    {
        SDL_RWops *rw = SDL_RWFromConstMem(shared_ttf, (int)shared_size);
        if(rw)
        {
            TTF_Font *f = TTF_OpenFontRW(rw, 1, pt_size);
            if(f)
            {
                return f;
            }
        }
    }

    const char *file_candidates[] = {
        SYSTEM_FONT_PRIMARY,
        ROMFS_FONT_REGULAR,
        ROMFS_FONT_VARIABLE,
        SYSTEM_FONT_FALLBACK_1,
        SYSTEM_FONT_FALLBACK_2
    };
    for(size_t i = 0; i < sizeof(file_candidates) / sizeof(file_candidates[0]); i++)
    {
        TTF_Font *f = TTF_OpenFont(file_candidates[i], pt_size);
        if(f)
        {
            return f;
        }
    }
    return NULL;
}

static bool init_application(AppContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->mcp_fd = -1;
    ctx->fsa_client = -1;

    WHBProcInit();
    WHBLogCafeInit();
    WHBLogUdpInit();
    VPADInit();
    init_mocha_and_fsa(ctx);
    socket_lib_init();

    if(curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
    {
        WHBLogPrintf("Failed to initialize libcurl");
        return false;
    }

    ctx->curl_multi = curl_multi_init();
    if(!ctx->curl_multi)
    {
        WHBLogPrintf("Failed to initialize curl_multi");
        return false;
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0)
    {
        WHBLogPrintf("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    if(TTF_Init() < 0)
    {
        WHBLogPrintf("TTF_Init failed: %s", TTF_GetError());
        return false;
    }

    ctx->window = SDL_CreateWindow(
        "LocaLeShop",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN);
    if(!ctx->window)
    {
        WHBLogPrintf("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    ctx->renderer = SDL_CreateRenderer(
        ctx->window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if(!ctx->renderer)
    {
        WHBLogPrintf("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);

    ctx->font_title = open_ui_font(28, true);
    ctx->font_body = open_ui_font(22, false);
    ctx->font_small = open_ui_font(17, false);

    load_server_config(ctx);
    return true;
}

static void shutdown_application(AppContext *ctx)
{
    cleanup_active_download_handles(ctx);

    if(ctx->mcp_fd >= 0)
    {
        MCP_Close(ctx->mcp_fd);
        ctx->mcp_fd = -1;
    }

    if(ctx->curl_multi)
    {
        curl_multi_cleanup(ctx->curl_multi);
        ctx->curl_multi = NULL;
    }

    free_catalog(ctx);

    if(ctx->font_small)
        TTF_CloseFont(ctx->font_small);
    if(ctx->font_body)
        TTF_CloseFont(ctx->font_body);
    if(ctx->font_title)
        TTF_CloseFont(ctx->font_title);

    if(ctx->renderer)
        SDL_DestroyRenderer(ctx->renderer);
    if(ctx->window)
        SDL_DestroyWindow(ctx->window);

    TTF_Quit();
    SDL_Quit();
    curl_global_cleanup();
    deinit_mocha_and_fsa(ctx);

    VPADShutdown();
    WHBLogUdpDeinit();
    WHBLogCafeDeinit();
    WHBProcShutdown();
}

/* --------------------------------------------------------------------------
 * Entry Point
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AppContext *ctx = (AppContext *)calloc(1, sizeof(AppContext));
    if(!ctx)
    {
        return EXIT_FAILURE;
    }

    if(!init_application(ctx))
    {
        shutdown_application(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    snprintf(
        ctx->status_message,
        sizeof(ctx->status_message),
        "Connecting to LocaLeShop server at %s:%d...",
        ctx->server_ip,
        ctx->server_port);
    render_ui(ctx);
    fetch_titles_catalog(ctx, true);

    while(WHBProcIsRunning())
    {
        if(!handle_input(ctx))
        {
            continue;
        }

        if(ctx->state == APP_STATE_DOWNLOADING)
        {
            step_download_pipeline(ctx);
        }
        else if(ctx->state == APP_STATE_INSTALLING)
        {
            step_mcp_installation(ctx);
        }

        render_ui(ctx);
    }

    shutdown_application(ctx);
    free(ctx);
    return EXIT_SUCCESS;
}
