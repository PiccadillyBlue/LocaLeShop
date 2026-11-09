/**
 * LocaLeShop Wii U Homebrew Client (.wuhb)
 * ----------------------------------------
 * Connects to the self-hosted LocaLeShop LAN daemon, renders an Aero Glass
 * GamePad UI via SDL2 + SDL2_ttf, streams WUP package files directly to the
 * SD card via libcurl (non-blocking curl_multi + direct fwrite), and installs
 * packages asynchronously to USB via Wii U MCP APIs.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

/* Wii U Toolchain (wut) headers */
#include <coreinit/filesystem.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysnet/socket.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

/* Portlibs headers */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "cJSON.h"
#include <curl/curl.h>

/* --------------------------------------------------------------------------
 * Compatibility Shims for WUT MCP API Variations
 * -------------------------------------------------------------------------- */
#ifndef MCP_INSTALL_TARGET_USB
#define MCP_INSTALL_TARGET_USB 1
#endif

/*
 * Provide thin inline wrappers matching the exact MCP_InstallSetTarget and
 * 2-argument MCP_InstallTitleAsync signatures while delegating to the
 * underlying wut <coreinit/mcp.h> symbols.
 */
static inline MCPError LocaLeShop_MCP_InstallSetTarget(int mcp_fd, int target) {
    MCPError err = MCP_InstallSetTargetDevice(mcp_fd, (MCPInstallTarget)target);
    if (err >= 0 && target == MCP_INSTALL_TARGET_USB) {
        MCP_InstallSetTargetUsb(mcp_fd, 1);
    }
    return err;
}

static inline MCPError LocaLeShop_MCP_InstallTitleAsync(int mcp_fd, const char *path) {
    static MCPInstallTitleInfo s_installInfo;
    memset(&s_installInfo, 0, sizeof(s_installInfo));

    char mcp_path[512];
    if (strncmp(path, "fs:/vol/external01", 18) == 0) {
        snprintf(mcp_path, sizeof(mcp_path), "/vol/storage_sdcard%s", path + 18);
    } else {
        snprintf(mcp_path, sizeof(mcp_path), "%s", path);
    }

    return MCP_InstallTitleAsync(mcp_fd, mcp_path, &s_installInfo);
}

#define MCP_InstallSetTarget(fd, target) LocaLeShop_MCP_InstallSetTarget((fd), (target))
#define MCP_InstallTitleAsync(fd, path)  LocaLeShop_MCP_InstallTitleAsync((fd), (path))

/* --------------------------------------------------------------------------
 * Configuration & Constants
 * -------------------------------------------------------------------------- */
#define SCREEN_WIDTH            1280
#define SCREEN_HEIGHT           720

#define DEFAULT_SERVER_IP       "192.168.1.100"
#define DEFAULT_SERVER_PORT     8000
#define CONFIG_FILE_PATH        "fs:/vol/external01/wiiu/apps/LocaLeShop/config.json"
#define SD_STAGE_BASE_DIR       "fs:/vol/external01/install"

#define SYSTEM_FONT_PRIMARY     "/vol/content/font/CafeStd.ttf"
#define SYSTEM_FONT_FALLBACK_1  "fs:/vol/external01/wiiu/apps/LocaLeShop/font.ttf"
#define SYSTEM_FONT_FALLBACK_2  "/vol/storage_mlc01/sys/title/0005001b/10042400/content/CafeStd.ttf"

#define MAX_TITLES              512
#define MAX_FILES_PER_TITLE     256
#define MAX_PATH_LEN            512
#define MAX_URL_LEN             768
#define FILE_WRITE_BUFFER_SIZE  (128 * 1024) /* 128 KiB stdio buffer for SD writes */

#define VISIBLE_CARDS           6
#define CARD_MARGIN_X           56
#define CARD_START_Y            116
#define CARD_HEIGHT             76
#define CARD_GAP_Y              12
#define CARD_CORNER_RADIUS      12

/* --------------------------------------------------------------------------
 * Data Structures
 * -------------------------------------------------------------------------- */

typedef struct {
    char     title_id[24];
    char     name[128];
    uint32_t version;
    uint64_t total_size_bytes;
    int      file_count;
    char   **file_names; /* Relative file paths inside package */
    char   **file_urls;  /* Resolved HTTP download URLs */
} TitleMetadata;

typedef enum {
    APP_STATE_BROWSING = 0,
    APP_STATE_DOWNLOADING,
    APP_STATE_INSTALLING,
    APP_STATE_COMPLETED,
    APP_STATE_ERROR,
    APP_STATE_CANCELLED
} AppState;

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} MemoryBuffer;

typedef struct {
    FILE    *fp;
    char     file_Buf[FILE_WRITE_BUFFER_SIZE];
    uint64_t bytes_written;
    bool     write_error;
} StreamFileContext;

typedef struct {
    /* Server configuration */
    char server_ip[64];
    int  server_port;

    /* SDL2 rendering resources */
    SDL_Window   *window;
    SDL_Renderer *renderer;
    TTF_Font     *font_title;
    TTF_Font     *font_body;
    TTF_Font     *font_small;

    /* Catalog & selection state */
    TitleMetadata titles[MAX_TITLES];
    int           title_count;
    int           selected_index;
    int           scroll_offset;

    /* Operational state machine */
    AppState      state;
    char          status_message[256];

    /* Non-blocking libcurl download pipeline */
    CURLM             *curl_multi;
    CURL              *curl_easy;
    StreamFileContext  stream_ctx;
    int                dl_title_index;
    int                dl_file_index;
    uint64_t           dl_completed_files_bytes;
    uint64_t           dl_current_file_bytes;
    uint64_t           dl_total_bytes;
    char               dl_current_filename[128];
    char               dl_current_filepath[MAX_PATH_LEN];
    char               dl_stage_dir[MAX_PATH_LEN];

    /* Non-blocking MCP installation pipeline */
    int                mcp_fd;
    MCPInstallProgress mcp_progress;
    bool               mcp_started_flag;
} AppContext;

/* --------------------------------------------------------------------------
 * Filesystem & Formatting Utilities
 * -------------------------------------------------------------------------- */

static void format_bytes(uint64_t bytes, char *out, size_t out_size) {
    const double kib = 1024.0;
    const double mib = kib * 1024.0;
    const double gib = mib * 1024.0;

    if (bytes >= (uint64_t)gib) {
        snprintf(out, out_size, "%.2f GiB", (double)bytes / gib);
    } else if (bytes >= (uint64_t)mib) {
        snprintf(out, out_size, "%.2f MiB", (double)bytes / mib);
    } else if (bytes >= (uint64_t)kib) {
        snprintf(out, out_size, "%.1f KiB", (double)bytes / kib);
    } else {
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    }
}

static bool mkdir_recursive(const char *dir_path) {
    char tmp[MAX_PATH_LEN];
    size_t len = strlen(dir_path);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }

    memcpy(tmp, dir_path, len + 1);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                /* Ignore root mount points like /vol or /vol/storage_sdcard */
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return false;
        }
    }
    return true;
}

static bool ensure_parent_dir(const char *file_path) {
    char parent[MAX_PATH_LEN];
    strncpy(parent, file_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';

    char *last_slash = strrchr(parent, '/');
    if (!last_slash || last_slash == parent) {
        return true;
    }
    *last_slash = '\0';
    return mkdir_recursive(parent);
}

static void remove_directory_recursive(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    char child_path[MAX_PATH_LEN];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(child_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_directory_recursive(child_path);
        } else {
            unlink(child_path);
        }
    }
    closedir(dir);
    rmdir(dir_path);
}

static void load_server_config(AppContext *ctx) {
    strncpy(ctx->server_ip, DEFAULT_SERVER_IP, sizeof(ctx->server_ip) - 1);
    ctx->server_port = DEFAULT_SERVER_PORT;

    FILE *fp = fopen(CONFIG_FILE_PATH, "rb");
    if (!fp) {
        return;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 16384) {
        fclose(fp);
        return;
    }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(fp);
        return;
    }
    size_t read_bytes = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);
    buf[read_bytes] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return;
    }

    const cJSON *ip_item = cJSON_GetObjectItemCaseSensitive(root, "server_ip");
    if (cJSON_IsString(ip_item) && ip_item->valuestring && ip_item->valuestring[0]) {
        strncpy(ctx->server_ip, ip_item->valuestring, sizeof(ctx->server_ip) - 1);
        ctx->server_ip[sizeof(ctx->server_ip) - 1] = '\0';
    }

    const cJSON *port_item = cJSON_GetObjectItemCaseSensitive(root, "server_port");
    if (cJSON_IsNumber(port_item) && port_item->valueint > 0 && port_item->valueint <= 65535) {
        ctx->server_port = port_item->valueint;
    }

    cJSON_Delete(root);
}

static void free_catalog(AppContext *ctx) {
    for (int i = 0; i < ctx->title_count; i++) {
        TitleMetadata *t = &ctx->titles[i];
        if (t->file_names) {
            for (int j = 0; j < t->file_count; j++) {
                free(t->file_names[j]);
            }
            free(t->file_names);
            t->file_names = NULL;
        }
        if (t->file_urls) {
            for (int j = 0; j < t->file_count; j++) {
                free(t->file_urls[j]);
            }
            free(t->file_urls);
            t->file_urls = NULL;
        }
        t->file_count = 0;
    }
    ctx->title_count = 0;
}

/* --------------------------------------------------------------------------
 * 1. Networking & Data Ingestion (libcurl & cJSON)
 * -------------------------------------------------------------------------- */

static size_t curl_memory_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    if (mem->size + total + 1 > mem->capacity) {
        size_t new_cap = (mem->capacity == 0) ? 16384 : mem->capacity * 2;
        while (new_cap < mem->size + total + 1) {
            new_cap *= 2;
        }
        uint8_t *new_data = (uint8_t *)realloc(mem->data, new_cap);
        if (!new_data) {
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

static size_t curl_file_stream_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    StreamFileContext *sctx = (StreamFileContext *)userdata;
    if (!sctx || !sctx->fp) {
        return 0;
    }

    size_t written = fwrite(ptr, 1, total, sctx->fp);
    if (written != total) {
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
    curl_off_t ul_now
) {
    (void)dltotal;
    (void)ul_total;
    (void)ul_now;

    AppContext *ctx = (AppContext *)clientp;
    if (!ctx) {
        return 0;
    }
    if (dlnow > 0) {
        ctx->dl_current_file_bytes = (uint64_t)dlnow;
    }
    return (ctx->state == APP_STATE_CANCELLED) ? 1 : 0;
}

/**
 * Extract filename from either a relative filename ("00000000.app") or a full URL
 * ("http://.../api/v1/titles/<id>/files/00000000.app").
 */
static void resolve_file_name_and_url(
    const AppContext *ctx,
    const char *title_id,
    const char *raw_entry,
    char *out_name,
    size_t out_name_size,
    char *out_url,
    size_t out_url_size
) {
    if (strncmp(raw_entry, "http://", 7) == 0 || strncmp(raw_entry, "https://", 8) == 0) {
        strncpy(out_url, raw_entry, out_url_size - 1);
        out_url[out_url_size - 1] = '\0';

        const char *marker = strstr(raw_entry, "/files/");
        if (marker) {
            strncpy(out_name, marker + 7, out_name_size - 1);
        } else {
            const char *slash = strrchr(raw_entry, '/');
            strncpy(out_name, slash ? (slash + 1) : raw_entry, out_name_size - 1);
        }
        out_name[out_name_size - 1] = '\0';
    } else {
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
            rel
        );
    }
}

static bool fetch_titles_catalog(AppContext *ctx) {
    free_catalog(ctx);

    char endpoint_url[MAX_URL_LEN];
    snprintf(
        endpoint_url,
        sizeof(endpoint_url),
        "http://%s:%d/api/v1/titles",
        ctx->server_ip,
        ctx->server_port
    );

    snprintf(ctx->status_message, sizeof(ctx->status_message), "Connecting to %s...", endpoint_url);

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Error: Failed to initialize libcurl.");
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    MemoryBuffer response_buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_memory_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LocaLeShop-WiiU/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200 || !response_buf.data) {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Failed to fetch catalog (%s, HTTP %ld) from %s:%d",
            curl_easy_strerror(res),
            http_code,
            ctx->server_ip,
            ctx->server_port
        );
        free(response_buf.data);
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    cJSON *root = cJSON_Parse((const char *)response_buf.data);
    free(response_buf.data);

    if (!root || !cJSON_IsArray(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Invalid JSON array returned by server.");
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (ctx->title_count >= MAX_TITLES) {
            break;
        }
        if (!cJSON_IsObject(item)) {
            continue;
        }

        const cJSON *j_tid   = cJSON_GetObjectItemCaseSensitive(item, "title_id");
        const cJSON *j_name  = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *j_ver   = cJSON_GetObjectItemCaseSensitive(item, "version");
        const cJSON *j_size  = cJSON_GetObjectItemCaseSensitive(item, "total_size_bytes");
        const cJSON *j_files = cJSON_GetObjectItemCaseSensitive(item, "file_list");
        if (!j_files) {
            j_files = cJSON_GetObjectItemCaseSensitive(item, "files");
        }
        if (!j_files) {
            j_files = cJSON_GetObjectItemCaseSensitive(item, "file_urls");
        }

        if (!cJSON_IsString(j_tid) || !j_tid->valuestring) {
            continue;
        }

        TitleMetadata *meta = &ctx->titles[ctx->title_count];
        memset(meta, 0, sizeof(*meta));

        strncpy(meta->title_id, j_tid->valuestring, sizeof(meta->title_id) - 1);
        if (cJSON_IsString(j_name) && j_name->valuestring) {
            strncpy(meta->name, j_name->valuestring, sizeof(meta->name) - 1);
        } else {
            strncpy(meta->name, meta->title_id, sizeof(meta->name) - 1);
        }

        meta->version = cJSON_IsNumber(j_ver) ? (uint32_t)j_ver->valuedouble : 0;
        meta->total_size_bytes = cJSON_IsNumber(j_size) ? (uint64_t)j_size->valuedouble : 0;

        if (cJSON_IsArray(j_files)) {
            int arr_len = cJSON_GetArraySize(j_files);
            if (arr_len > MAX_FILES_PER_TITLE) {
                arr_len = MAX_FILES_PER_TITLE;
            }
            if (arr_len > 0) {
                meta->file_names = (char **)calloc((size_t)arr_len, sizeof(char *));
                meta->file_urls  = (char **)calloc((size_t)arr_len, sizeof(char *));

                cJSON *f_elem = NULL;
                int f_idx = 0;
                cJSON_ArrayForEach(f_elem, j_files) {
                    if (f_idx >= arr_len) {
                        break;
                    }
                    if (!cJSON_IsString(f_elem) || !f_elem->valuestring) {
                        continue;
                    }
                    char resolved_name[MAX_PATH_LEN];
                    char resolved_url[MAX_URL_LEN];
                    resolve_file_name_and_url(
                        ctx,
                        meta->title_id,
                        f_elem->valuestring,
                        resolved_name,
                        sizeof(resolved_name),
                        resolved_url,
                        sizeof(resolved_url)
                    );
                    meta->file_names[f_idx] = strdup(resolved_name);
                    meta->file_urls[f_idx]  = strdup(resolved_url);
                    f_idx++;
                }
                meta->file_count = f_idx;
            }
        }

        ctx->title_count++;
    }

    cJSON_Delete(root);

    ctx->selected_index = 0;
    ctx->scroll_offset = 0;
    ctx->state = APP_STATE_BROWSING;

    snprintf(
        ctx->status_message,
        sizeof(ctx->status_message),
        "Ready — %d title(s) indexed from %s:%d. Press A to Download & Install.",
        ctx->title_count,
        ctx->server_ip,
        ctx->server_port
    );
    return true;
}

/* --------------------------------------------------------------------------
 * Chunked Direct-to-SD Streaming & Non-Blocking Download State Machine
 * -------------------------------------------------------------------------- */

static void cleanup_active_download_handles(AppContext *ctx) {
    if (ctx->curl_easy) {
        if (ctx->curl_multi) {
            curl_multi_remove_handle(ctx->curl_multi, ctx->curl_easy);
        }
        curl_easy_cleanup(ctx->curl_easy);
        ctx->curl_easy = NULL;
    }
    if (ctx->stream_ctx.fp) {
        fflush(ctx->stream_ctx.fp);
        fclose(ctx->stream_ctx.fp);
        ctx->stream_ctx.fp = NULL;
    }
}

static bool start_next_file_download(AppContext *ctx) {
    cleanup_active_download_handles(ctx);

    TitleMetadata *title = &ctx->titles[ctx->dl_title_index];

    if (ctx->dl_file_index >= title->file_count) {
        /* All files in the WUP package have been staged to SD card */
        return false;
    }

    const char *rel_name = title->file_names[ctx->dl_file_index];
    const char *file_url = title->file_urls[ctx->dl_file_index];

    strncpy(ctx->dl_current_filename, rel_name, sizeof(ctx->dl_current_filename) - 1);
    ctx->dl_current_filename[sizeof(ctx->dl_current_filename) - 1] = '\0';

    snprintf(
        ctx->dl_current_filepath,
        sizeof(ctx->dl_current_filepath),
        "%s/%s",
        ctx->dl_stage_dir,
        rel_name
    );

    if (!ensure_parent_dir(ctx->dl_current_filepath)) {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "SD Error: Could not create directory for %s",
            ctx->dl_current_filepath
        );
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    memset(&ctx->stream_ctx, 0, sizeof(ctx->stream_ctx));
    ctx->stream_ctx.fp = fopen(ctx->dl_current_filepath, "wb");
    if (!ctx->stream_ctx.fp) {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "SD Error: Cannot open %s for writing (%s)",
            ctx->dl_current_filepath,
            strerror(errno)
        );
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    /* Attach a 128 KiB stdio buffer to minimize SD card syscall overhead */
    setvbuf(ctx->stream_ctx.fp, ctx->stream_ctx.file_Buf, _IOFBF, sizeof(ctx->stream_ctx.file_Buf));

    ctx->curl_easy = curl_easy_init();
    if (!ctx->curl_easy) {
        fclose(ctx->stream_ctx.fp);
        ctx->stream_ctx.fp = NULL;
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Error: curl_easy_init failed.");
        ctx->state = APP_STATE_ERROR;
        return false;
    }

    ctx->dl_current_file_bytes = 0;

    curl_easy_setopt(ctx->curl_easy, CURLOPT_URL, file_url);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_WRITEFUNCTION, curl_file_stream_write_cb);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_WRITEDATA, &ctx->stream_ctx);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_XFERINFOFUNCTION, curl_xferinfo_cb);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_XFERINFODATA, ctx);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ctx->curl_easy, CURLOPT_USERAGENT, "LocaLeShop-WiiU/1.0");

    curl_multi_add_handle(ctx->curl_multi, ctx->curl_easy);

    snprintf(
        ctx->status_message,
        sizeof(ctx->status_message),
        "Downloading [%d/%d]: %s",
        ctx->dl_file_index + 1,
        title->file_count,
        ctx->dl_current_filename
    );
    return true;
}

static void begin_title_download(AppContext *ctx, int title_idx) {
    if (title_idx < 0 || title_idx >= ctx->title_count) {
        return;
    }

    TitleMetadata *title = &ctx->titles[title_idx];
    if (title->file_count <= 0) {
        snprintf(ctx->status_message, sizeof(ctx->status_message), "Package '%s' has no files listed.", title->name);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    ctx->dl_title_index = title_idx;
    ctx->dl_file_index = 0;
    ctx->dl_completed_files_bytes = 0;
    ctx->dl_current_file_bytes = 0;
    ctx->dl_total_bytes = title->total_size_bytes;

    snprintf(
        ctx->dl_stage_dir,
        sizeof(ctx->dl_stage_dir),
        "%s/%s",
        SD_STAGE_BASE_DIR,
        title->title_id
    );

    if (!mkdir_recursive(ctx->dl_stage_dir)) {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Failed to create SD staging path: %s",
            ctx->dl_stage_dir
        );
        ctx->state = APP_STATE_ERROR;
        return;
    }

    ctx->state = APP_STATE_DOWNLOADING;
    start_next_file_download(ctx);
}

/* --------------------------------------------------------------------------
 * 2. MCP Installation Pipeline (Async & Non-Blocking)
 * -------------------------------------------------------------------------- */

static void begin_mcp_installation(AppContext *ctx) {
    int mcp_fd = MCP_Open();
    if (mcp_fd < 0) {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "MCP_Open() failed (error %d)",
            mcp_fd
        );
        remove_directory_recursive(ctx->dl_stage_dir);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    ctx->mcp_fd = mcp_fd;
    memset(&ctx->mcp_progress, 0, sizeof(ctx->mcp_progress));
    ctx->mcp_started_flag = false;

    MCPError target_err = MCP_InstallSetTarget(mcp_fd, MCP_INSTALL_TARGET_USB);
    if (target_err < 0) {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "MCP_InstallSetTarget(USB) failed (0x%08X)",
            (unsigned int)target_err
        );
        MCP_Close(mcp_fd);
        ctx->mcp_fd = -1;
        remove_directory_recursive(ctx->dl_stage_dir);
        ctx->state = APP_STATE_ERROR;
        return;
    }

    MCPError install_err = MCP_InstallTitleAsync(mcp_fd, ctx->dl_stage_dir);
    if (install_err < 0) {
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "MCP_InstallTitleAsync failed (0x%08X) for %s",
            (unsigned int)install_err,
            ctx->dl_stage_dir
        );
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
        "Installing %s to USB via MCP...",
        ctx->titles[ctx->dl_title_index].name
    );
}

static void step_mcp_installation(AppContext *ctx) {
    if (ctx->state != APP_STATE_INSTALLING || ctx->mcp_fd < 0) {
        return;
    }

    MCPInstallProgress progress;
    memset(&progress, 0, sizeof(progress));

    MCPError err = MCP_InstallGetProgress(ctx->mcp_fd, &progress);
    if (err >= 0) {
        ctx->mcp_progress = progress;
        if (progress.inProgress == 1) {
            ctx->mcp_started_flag = true;
            char done_str[32], total_str[32];
            format_bytes(progress.sizeProgress, done_str, sizeof(done_str));
            format_bytes(progress.sizeTotal, total_str, sizeof(total_str));
            snprintf(
                ctx->status_message,
                sizeof(ctx->status_message),
                "Installing to USB: content %u/%u (%s / %s)",
                progress.contentsProgress,
                progress.contentsTotal,
                done_str,
                total_str
            );
            return;
        }

        /* When inProgress transitions to 0 (or sizeTotal > 0 and sizeProgress >= sizeTotal), install is complete */
        if (ctx->mcp_started_flag || (progress.sizeTotal > 0 && progress.sizeProgress >= progress.sizeTotal)) {
            MCP_Close(ctx->mcp_fd);
            ctx->mcp_fd = -1;

            /* Clean up staged SD directory */
            remove_directory_recursive(ctx->dl_stage_dir);

            ctx->state = APP_STATE_COMPLETED;
            snprintf(
                ctx->status_message,
                sizeof(ctx->status_message),
                "Installed '%s' to USB! Staged SD files cleaned up.",
                ctx->titles[ctx->dl_title_index].name
            );
        }
    }
}

static void step_download_pipeline(AppContext *ctx) {
    if (ctx->state != APP_STATE_DOWNLOADING || !ctx->curl_multi || !ctx->curl_easy) {
        return;
    }

    int still_running = 0;
    /* Perform non-blocking network I/O bursts for this frame */
    for (int burst = 0; burst < 8; burst++) {
        CURLMcode mc = curl_multi_perform(ctx->curl_multi, &still_running);
        if (mc != CURLM_OK || still_running == 0) {
            break;
        }
    }

    ctx->dl_current_file_bytes = ctx->stream_ctx.bytes_written;

    char done_str[32], total_str[32];
    uint64_t aggregate_done = ctx->dl_completed_files_bytes + ctx->dl_current_file_bytes;
    format_bytes(aggregate_done, done_str, sizeof(done_str));
    format_bytes(ctx->dl_total_bytes, total_str, sizeof(total_str));

    TitleMetadata *title = &ctx->titles[ctx->dl_title_index];
    snprintf(
        ctx->status_message,
        sizeof(ctx->status_message),
        "Downloading [%d/%d] %s — %s / %s (Press B to Cancel)",
        ctx->dl_file_index + 1,
        title->file_count,
        ctx->dl_current_filename,
        done_str,
        total_str
    );

    int msgs_left = 0;
    CURLMsg *msg = NULL;
    while ((msg = curl_multi_info_read(ctx->curl_multi, &msgs_left)) != NULL) {
        if (msg->msg == CURLMSG_DONE) {
            CURLcode result = msg->data.result;
            long http_code = 0;
            curl_easy_getinfo(ctx->curl_easy, CURLINFO_RESPONSE_CODE, &http_code);

            bool write_err = ctx->stream_ctx.write_error;
            uint64_t file_bytes = ctx->stream_ctx.bytes_written;

            cleanup_active_download_handles(ctx);

            if (result != CURLE_OK || (http_code != 200 && http_code != 206) || write_err) {
                remove_directory_recursive(ctx->dl_stage_dir);
                snprintf(
                    ctx->status_message,
                    sizeof(ctx->status_message),
                    "Download failed on %s (%s, HTTP %ld). Cleaned up SD staging.",
                    ctx->dl_current_filename,
                    curl_easy_strerror(result),
                    http_code
                );
                ctx->state = APP_STATE_ERROR;
                return;
            }

            ctx->dl_completed_files_bytes += file_bytes;
            ctx->dl_current_file_bytes = 0;
            ctx->dl_file_index++;

            if (ctx->dl_file_index < title->file_count) {
                start_next_file_download(ctx);
            } else {
                /* All package files staged on SD; transition to async MCP USB installation */
                begin_mcp_installation(ctx);
            }
            break;
        }
    }
}

static void cancel_active_operation(AppContext *ctx) {
    if (ctx->state == APP_STATE_DOWNLOADING) {
        ctx->state = APP_STATE_CANCELLED;
        cleanup_active_download_handles(ctx);
        remove_directory_recursive(ctx->dl_stage_dir);
        snprintf(
            ctx->status_message,
            sizeof(ctx->status_message),
            "Download cancelled. Removed staged files from SD card."
        );
        ctx->state = APP_STATE_BROWSING;
    }
}

/* --------------------------------------------------------------------------
 * 3. Visual Design (Aero Glass Aesthetic via SDL2)
 * -------------------------------------------------------------------------- */

static void draw_filled_rounded_rect(
    SDL_Renderer *renderer,
    int x,
    int y,
    int w,
    int h,
    int r,
    SDL_Color color
) {
    if (r <= 0 || w < 2 * r || h < 2 * r) {
        SDL_Rect rect = {x, y, w, h};
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    /* Center body */
    SDL_Rect center_rect = {x, y + r, w, h - 2 * r};
    SDL_RenderFillRect(renderer, &center_rect);

    /* Top and bottom rounded scanlines without overlapping rows (crucial for alpha blending) */
    for (int dy = 0; dy < r; dy++) {
        int cy = r - dy;
        int dx = (int)(sqrt((double)(r * r - cy * cy)) + 0.5);
        int line_x = x + r - dx;
        int line_w = w - 2 * (r - dx);

        SDL_Rect top_span = {line_x, y + dy, line_w, 1};
        SDL_Rect bot_span = {line_x, y + h - 1 - dy, line_w, 1};
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
    SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    /* Straight edges */
    SDL_RenderDrawLine(renderer, x + r, y, x + w - r - 1, y);
    SDL_RenderDrawLine(renderer, x + r, y + h - 1, x + w - r - 1, y + h - 1);
    SDL_RenderDrawLine(renderer, x, y + r, x, y + h - r - 1);
    SDL_RenderDrawLine(renderer, x + w - 1, y + r, x + w - 1, y + h - r - 1);

    /* Corner arcs */
    for (int dy = 0; dy < r; dy++) {
        int cy = r - dy;
        int dx = (int)(sqrt((double)(r * r - cy * cy)) + 0.5);

        SDL_RenderDrawPoint(renderer, x + r - dx, y + dy);
        SDL_RenderDrawPoint(renderer, x + w - 1 - r + dx, y + dy);
        SDL_RenderDrawPoint(renderer, x + r - dx, y + h - 1 - dy);
        SDL_RenderDrawPoint(renderer, x + w - 1 - r + dx, y + h - 1 - dy);
    }
}

/**
 * Render an Aero Glass panel with:
 * - Soft offset drop shadow
 * - Frosted translucent fill (rgba(255, 255, 255, fill_alpha))
 * - Upper glossy reflection band
 * - 1px top/left specular highlight border in soft white (rgba(255, 255, 255, 120))
 * - Optional electric cyan accent glow for active/selected cards
 */
static void draw_aero_glass_card(
    SDL_Renderer *renderer,
    int x,
    int y,
    int w,
    int h,
    int radius,
    uint8_t fill_alpha,
    bool is_selected
) {
    /* 1. Multi-layer soft drop shadow */
    SDL_Color shadow_outer = {0, 0, 0, 55};
    SDL_Color shadow_inner = {0, 0, 0, 95};
    draw_filled_rounded_rect(renderer, x + 4, y + 6, w, h, radius, shadow_outer);
    draw_filled_rounded_rect(renderer, x + 2, y + 3, w, h, radius, shadow_inner);

    /* 2. Frosted semi-transparent base fill (rgba(255, 255, 255, 25..45)) */
    SDL_Color glass_fill = {255, 255, 255, fill_alpha};
    draw_filled_rounded_rect(renderer, x, y, w, h, radius, glass_fill);

    /* 3. Top half subtle glass reflection sheen */
    SDL_Color gloss_band = {255, 255, 255, 14};
    SDL_Rect gloss_rect = {x + radius / 2, y + 2, w - radius, h / 2 - 2};
    SDL_SetRenderDrawColor(renderer, gloss_band.r, gloss_band.g, gloss_band.b, gloss_band.a);
    SDL_RenderFillRect(renderer, &gloss_rect);

    /* 4. Subtle dark bottom/right rim for depth */
    SDL_Color rim_color = {180, 210, 240, 45};
    draw_rounded_rect_outline(renderer, x, y, w, h, radius, rim_color);

    /* 5. 1px top and left specular highlight border in soft white rgba(255, 255, 255, 120) */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
    SDL_RenderDrawLine(renderer, x + radius, y, x + w - radius - 1, y);
    SDL_RenderDrawLine(renderer, x, y + radius, x, y + h - radius - 1);
    for (int dy = 0; dy < radius; dy++) {
        int cy = radius - dy;
        int dx = (int)(sqrt((double)(radius * radius - cy * cy)) + 0.5);
        SDL_RenderDrawPoint(renderer, x + radius - dx, y + dy);
    }

    /* 6. Electric cyan/blue accent glow when selected */
    if (is_selected) {
        SDL_Color glow_outer = {0, 195, 255, 70};
        SDL_Color glow_mid   = {0, 215, 255, 140};
        SDL_Color glow_core  = {80, 235, 255, 235};
        draw_rounded_rect_outline(renderer, x - 2, y - 2, w + 4, h + 4, radius + 2, glow_outer);
        draw_rounded_rect_outline(renderer, x - 1, y - 1, w + 2, h + 2, radius + 1, glow_mid);
        draw_rounded_rect_outline(renderer, x, y, w, h, radius, glow_core);
    }
}

static void draw_aero_background(SDL_Renderer *renderer) {
    /* Deep slate / translucent dark gradient background */
    for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
        float t = (float)y / (float)SCREEN_HEIGHT;
        uint8_t r = (uint8_t)(12.0f + t * 10.0f);
        uint8_t g = (uint8_t)(20.0f + t * 22.0f);
        uint8_t b = (uint8_t)(34.0f + t * 38.0f);
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_Rect band = {0, y, SCREEN_WIDTH, 2};
        SDL_RenderFillRect(renderer, &band);
    }

    /* Subtle Aero diagonal light blooms */
    SDL_Color bloom1 = {0, 180, 255, 12};
    SDL_Color bloom2 = {120, 210, 255, 10};
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
    bool align_right
) {
    if (!font || !text || !text[0]) {
        return;
    }

    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) {
        return;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
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

static void render_ui(AppContext *ctx) {
    SDL_Renderer *renderer = ctx->renderer;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* 1. Deep slate gradient background */
    draw_aero_background(renderer);

    SDL_Color white_primary   = {245, 250, 255, 255};
    SDL_Color white_secondary = {180, 205, 230, 225};
    SDL_Color cyan_accent     = {70, 225, 255, 255};

    /* 2. Top Aero Glass Header Bar */
    draw_aero_glass_card(renderer, 36, 20, SCREEN_WIDTH - 72, 74, 14, 32, false);
    render_text(renderer, ctx->font_title, "LocaLeShop", 64, 34, cyan_accent, false);
    render_text(
        renderer,
        ctx->font_small,
        "Wii U LAN WUP Package Client  |  Aero Glass Edition",
        245,
        43,
        white_secondary,
        false
    );

    char server_badge[128];
    snprintf(server_badge, sizeof(server_badge), "Server: %s:%d", ctx->server_ip, ctx->server_port);
    render_text(renderer, ctx->font_small, server_badge, SCREEN_WIDTH - 64, 43, white_secondary, true);

    /* 3. Title List Cards */
    if (ctx->title_count == 0) {
        draw_aero_glass_card(renderer, CARD_MARGIN_X, 220, SCREEN_WIDTH - 2 * CARD_MARGIN_X, 160, 16, 30, false);
        render_text(
            renderer,
            ctx->font_body,
            "No Wii U titles loaded from server.",
            CARD_MARGIN_X + 36,
            262,
            white_primary,
            false
        );
        render_text(
            renderer,
            ctx->font_small,
            "Verify your LocaLeShop server is running and press A to retry.",
            CARD_MARGIN_X + 36,
            306,
            white_secondary,
            false
        );
    } else {
        int end_idx = ctx->scroll_offset + VISIBLE_CARDS;
        if (end_idx > ctx->title_count) {
            end_idx = ctx->title_count;
        }

        for (int i = ctx->scroll_offset; i < end_idx; i++) {
            int row = i - ctx->scroll_offset;
            int card_x = CARD_MARGIN_X;
            int card_y = CARD_START_Y + row * (CARD_HEIGHT + CARD_GAP_Y);
            int card_w = SCREEN_WIDTH - 2 * CARD_MARGIN_X;
            bool is_sel = (i == ctx->selected_index);

            /* Selected cards use higher frosted opacity (45), unselected use 25 */
            uint8_t alpha = is_sel ? 45 : 25;
            draw_aero_glass_card(renderer, card_x, card_y, card_w, CARD_HEIGHT, CARD_CORNER_RADIUS, alpha, is_sel);

            TitleMetadata *t = &ctx->titles[i];

            /* Left selection indicator pill */
            if (is_sel) {
                SDL_Color pill_color = {0, 215, 255, 230};
                draw_filled_rounded_rect(renderer, card_x + 12, card_y + 16, 6, CARD_HEIGHT - 32, 3, pill_color);
            }

            render_text(
                renderer,
                ctx->font_body,
                t->name,
                card_x + 30,
                card_y + 13,
                is_sel ? white_primary : white_secondary,
                false
            );

            char meta_line[256];
            snprintf(
                meta_line,
                sizeof(meta_line),
                "Title ID: %s   |   v%u   |   %d file(s)",
                t->title_id,
                t->version,
                t->file_count
            );
            render_text(
                renderer,
                ctx->font_small,
                meta_line,
                card_x + 30,
                card_y + 44,
                is_sel ? cyan_accent : white_secondary,
                false
            );

            char size_badge[64];
            format_bytes(t->total_size_bytes, size_badge, sizeof(size_badge));
            render_text(
                renderer,
                ctx->font_body,
                size_badge,
                card_x + card_w - 28,
                card_y + 24,
                is_sel ? cyan_accent : white_primary,
                true
            );
        }
    }

    /* 4. Bottom Translucent Glass Status Bar & Progress Meter */
    int status_x = 36;
    int status_y = SCREEN_HEIGHT - 102;
    int status_w = SCREEN_WIDTH - 72;
    int status_h = 82;
    draw_aero_glass_card(renderer, status_x, status_y, status_w, status_h, 14, 38, false);

    render_text(renderer, ctx->font_small, ctx->status_message, status_x + 24, status_y + 14, white_primary, false);

    const char *controls_hint = "D-Pad: Navigate   |   A: Download & Install   |   B: Cancel   |   HOME: Exit";
    render_text(
        renderer,
        ctx->font_small,
        controls_hint,
        status_x + status_w - 24,
        status_y + 14,
        white_secondary,
        true
    );

    /* Progress bar track */
    int bar_x = status_x + 24;
    int bar_y = status_y + 46;
    int bar_w = status_w - 48;
    int bar_h = 20;

    SDL_Color track_fill = {10, 18, 30, 140};
    SDL_Color track_rim  = {255, 255, 255, 70};
    draw_filled_rounded_rect(renderer, bar_x, bar_y, bar_w, bar_h, 8, track_fill);
    draw_rounded_rect_outline(renderer, bar_x, bar_y, bar_w, bar_h, 8, track_rim);

    double progress_ratio = 0.0;
    if (ctx->state == APP_STATE_DOWNLOADING) {
        uint64_t done = ctx->dl_completed_files_bytes + ctx->dl_current_file_bytes;
        if (ctx->dl_total_bytes > 0) {
            progress_ratio = (double)done / (double)ctx->dl_total_bytes;
        } else if (ctx->titles[ctx->dl_title_index].file_count > 0) {
            progress_ratio = (double)ctx->dl_file_index / (double)ctx->titles[ctx->dl_title_index].file_count;
        }
    } else if (ctx->state == APP_STATE_INSTALLING) {
        if (ctx->mcp_progress.sizeTotal > 0) {
            progress_ratio = (double)ctx->mcp_progress.sizeProgress / (double)ctx->mcp_progress.sizeTotal;
        }
    } else if (ctx->state == APP_STATE_COMPLETED) {
        progress_ratio = 1.0;
    }

    if (progress_ratio < 0.0) progress_ratio = 0.0;
    if (progress_ratio > 1.0) progress_ratio = 1.0;

    int fill_w = (int)((double)bar_w * progress_ratio);
    if (fill_w >= 16) {
        SDL_Color fill_color = (ctx->state == APP_STATE_INSTALLING)
            ? (SDL_Color){60, 240, 180, 210}
            : (SDL_Color){0, 205, 255, 215};
        draw_filled_rounded_rect(renderer, bar_x, bar_y, fill_w, bar_h, 8, fill_color);

        /* Specular top highlight on the progress bar */
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 130);
        SDL_RenderDrawLine(renderer, bar_x + 6, bar_y + 2, bar_x + fill_w - 6, bar_y + 2);
    }

    SDL_RenderPresent(renderer);
}

/* --------------------------------------------------------------------------
 * Input Handling (Wii U GamePad VPAD + SDL2 Events)
 * -------------------------------------------------------------------------- */

static void move_selection(AppContext *ctx, int delta) {
    if (ctx->title_count <= 0) {
        return;
    }
    ctx->selected_index += delta;
    if (ctx->selected_index < 0) {
        ctx->selected_index = 0;
    } else if (ctx->selected_index >= ctx->title_count) {
        ctx->selected_index = ctx->title_count - 1;
    }

    if (ctx->selected_index < ctx->scroll_offset) {
        ctx->scroll_offset = ctx->selected_index;
    } else if (ctx->selected_index >= ctx->scroll_offset + VISIBLE_CARDS) {
        ctx->scroll_offset = ctx->selected_index - VISIBLE_CARDS + 1;
    }
}

static bool handle_input(AppContext *ctx) {
    bool nav_up      = false;
    bool nav_down    = false;
    bool press_a     = false;
    bool press_b     = false;
    bool press_home  = false;

    /* 1. Poll Wii U GamePad via VPADRead */
    VPADStatus vpad_data;
    VPADReadError vpad_err = VPAD_READ_UNINITIALIZED;
    VPADRead(VPAD_CHAN_0, &vpad_data, 1, &vpad_err);
    if (vpad_err == VPAD_READ_SUCCESS) {
        uint32_t trig = vpad_data.trigger;
        if (trig & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP))     nav_up = true;
        if (trig & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN)) nav_down = true;
        if (trig & VPAD_BUTTON_A)                                    press_a = true;
        if (trig & VPAD_BUTTON_B)                                    press_b = true;
        if (trig & VPAD_BUTTON_HOME)                                 press_home = true;
    }

    /* 2. Drain SDL2 event queue */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            return false;
        }
        if (ev.type == SDL_KEYDOWN) {
            switch (ev.key.keysym.sym) {
                case SDLK_UP:     nav_up = true; break;
                case SDLK_DOWN:   nav_down = true; break;
                case SDLK_RETURN: press_a = true; break;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE: press_b = true; break;
                case SDLK_HOME:   press_home = true; break;
                default: break;
            }
        } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
            switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav_up = true; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav_down = true; break;
                case SDL_CONTROLLER_BUTTON_A:         press_a = true; break;
                case SDL_CONTROLLER_BUTTON_B:         press_b = true; break;
                case SDL_CONTROLLER_BUTTON_GUIDE:
                case SDL_CONTROLLER_BUTTON_START:     press_home = true; break;
                default: break;
            }
        }
    }

    if (press_home) {
        if (ctx->state == APP_STATE_DOWNLOADING) {
            cancel_active_operation(ctx);
        }
        SYSLaunchMenu();
        return false;
    }

    if (ctx->state == APP_STATE_BROWSING || ctx->state == APP_STATE_COMPLETED || ctx->state == APP_STATE_ERROR) {
        if (nav_up) {
            move_selection(ctx, -1);
        } else if (nav_down) {
            move_selection(ctx, 1);
        } else if (press_a) {
            if (ctx->title_count == 0) {
                fetch_titles_catalog(ctx);
            } else {
                begin_title_download(ctx, ctx->selected_index);
            }
        }
    } else if (ctx->state == APP_STATE_DOWNLOADING) {
        if (press_b) {
            cancel_active_operation(ctx);
        }
    }

    return true;
}

/* --------------------------------------------------------------------------
 * Initialization & Teardown
 * -------------------------------------------------------------------------- */

static TTF_Font *open_ui_font(int pt_size) {
    const char *candidates[] = {
        SYSTEM_FONT_PRIMARY,
        SYSTEM_FONT_FALLBACK_1,
        SYSTEM_FONT_FALLBACK_2
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        TTF_Font *f = TTF_OpenFont(candidates[i], pt_size);
        if (f) {
            return f;
        }
    }
    return NULL;
}

static bool init_application(AppContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->mcp_fd = -1;

    WHBProcInit();
    WHBLogCafeInit();
    WHBLogUdpInit();
    VPADInit();

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        WHBLogPrintf("Failed to initialize libcurl");
        return false;
    }

    ctx->curl_multi = curl_multi_init();
    if (!ctx->curl_multi) {
        WHBLogPrintf("Failed to initialize curl_multi");
        return false;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        WHBLogPrintf("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    if (TTF_Init() < 0) {
        WHBLogPrintf("TTF_Init failed: %s", TTF_GetError());
        return false;
    }

    ctx->window = SDL_CreateWindow(
        "LocaLeShop",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );
    if (!ctx->window) {
        WHBLogPrintf("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    ctx->renderer = SDL_CreateRenderer(
        ctx->window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!ctx->renderer) {
        WHBLogPrintf("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    /* Enable Aero Glass alpha blending */
    SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);

    ctx->font_title = open_ui_font(30);
    ctx->font_body  = open_ui_font(22);
    ctx->font_small = open_ui_font(17);

    load_server_config(ctx);
    return true;
}

static void shutdown_application(AppContext *ctx) {
    cleanup_active_download_handles(ctx);

    if (ctx->mcp_fd >= 0) {
        MCP_Close(ctx->mcp_fd);
        ctx->mcp_fd = -1;
    }

    if (ctx->curl_multi) {
        curl_multi_cleanup(ctx->curl_multi);
        ctx->curl_multi = NULL;
    }

    free_catalog(ctx);

    if (ctx->font_small) TTF_CloseFont(ctx->font_small);
    if (ctx->font_body)  TTF_CloseFont(ctx->font_body);
    if (ctx->font_title) TTF_CloseFont(ctx->font_title);

    if (ctx->renderer) SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window)   SDL_DestroyWindow(ctx->window);

    TTF_Quit();
    SDL_Quit();
    curl_global_cleanup();

    VPADShutdown();
    WHBLogUdpDeinit();
    WHBLogCafeDeinit();
    WHBProcShutdown();
}

/* --------------------------------------------------------------------------
 * Entry Point
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    AppContext *ctx = (AppContext *)calloc(1, sizeof(AppContext));
    if (!ctx) {
        return EXIT_FAILURE;
    }

    if (!init_application(ctx)) {
        shutdown_application(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Initial render while connecting to LAN server */
    snprintf(ctx->status_message, sizeof(ctx->status_message), "Fetching catalog from %s:%d...", ctx->server_ip, ctx->server_port);
    render_ui(ctx);
    fetch_titles_catalog(ctx);

    /* Main 60Hz non-blocking UI, download, and async MCP installation loop */
    while (WHBProcIsRunning()) {
        if (!handle_input(ctx)) {
            continue;
        }

        if (ctx->state == APP_STATE_DOWNLOADING) {
            step_download_pipeline(ctx);
        } else if (ctx->state == APP_STATE_INSTALLING) {
            step_mcp_installation(ctx);
        }

        render_ui(ctx);
    }

    shutdown_application(ctx);
    free(ctx);
    return EXIT_SUCCESS;
}