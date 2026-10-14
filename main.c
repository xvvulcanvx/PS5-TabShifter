/*
 * PS5 TabShifter.elf v1.1 (Official sceAppInstUtil Native Installer Edition for FW 13.60+)
 * - Uses Sony's official libSceAppInstUtil (sceAppInstUtilAppInstallTitleDir) so the
 *   "TS" Home Screen tile can be natively deleted via PS5 Controller (Options -> Delete)!
 * - Also includes a 1-Click "Uninstall Tile & Stop Payload" button inside the UI
 * - Full Dual-Database 13.60 Engine: moves apps between Games & Media + hides PSN Bloatware
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ps5/kernel.h>
#include <sqlite3.h>

#define PORT 9095
#define TITLE_ID "TBSH00001"

#define DB_APP_PATH  "/system_data/priv/mms/app.db"
#define DB_INFO_PATH "/system_data/priv/mms/appinfo.db"
#define DB_APP_BAK   "/system_data/priv/mms/app.db.tabshifter.bak"
#define DB_INFO_BAK  "/system_data/priv/mms/appinfo.db.tabshifter.bak"

#define DISP_GAME   138
#define DISP_MEDIA  188
#define DISP_HIDDEN 0

#define CONCEPT_DISP_GAME   1162
#define CONCEPT_DISP_MEDIA  2236
#define CONCEPT_DISP_HIDDEN 0

#define CAT_GAME  0
#define CAT_MEDIA 65536

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void*);
int sceAppInstUtilAppUnInstall(const char*);

static volatile int g_running = 1;

static void ps5_notify(const char *fmt, ...) {
    notify_request_t req;
    va_list args;
    memset(&req, 0, sizeof(req));
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static int restart_sceshellui(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t buf_size = 0;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) < 0 || buf_size == 0) return -1;
    uint8_t *buf = malloc(buf_size);
    if (!buf) return -1;

    if (sysctl(mib, 4, buf, &buf_size, NULL, 0) < 0) {
        free(buf);
        return -1;
    }

    int killed_pid = -1;
    uint8_t *ptr = buf;
    uint8_t *end = buf + buf_size;

    while (ptr < end) {
        int struct_size = *(int *)ptr;
        if (struct_size <= 0) break;

        pid_t pid = *(pid_t *)(ptr + 0x48);
        for (int off = 0x180; off < struct_size - 16; off++) {
            if (strcmp((char *)(ptr + off), "SceShellUI") == 0 && pid > 1) {
                kill(pid, SIGKILL);
                killed_pid = pid;
                break;
            }
        }
        if (killed_pid > 0) break;
        ptr += struct_size;
    }

    free(buf);
    return killed_pid;
}

static void remove_wal_locks(void) {
    unlink("/system_data/priv/mms/app.db-wal");
    unlink("/system_data/priv/mms/app.db-shm");
    unlink("/system_data/priv/mms/appinfo.db-wal");
    unlink("/system_data/priv/mms/appinfo.db-shm");
}

static void copy_file_if_missing(const char *src_path, const char *dst_path) {
    if (access(dst_path, F_OK) == 0) return;
    FILE *src = fopen(src_path, "rb");
    if (!src) return;
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { fclose(src); return; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
}

static void force_copy_file(const char *src_path, const char *dst_path) {
    FILE *src = fopen(src_path, "rb");
    if (!src) return;
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { fclose(src); return; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
}

static void backup_databases_once(void) {
    copy_file_if_missing(DB_APP_PATH, DB_APP_BAK);
    copy_file_if_missing(DB_INFO_PATH, DB_INFO_BAK);
}

/* Pure C CRC32 & Adler32 for dependency-free PNG generation */
static uint32_t calc_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

static uint32_t calc_adler32(const uint8_t *buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void write_be32(FILE *f, uint32_t val) {
    uint8_t b[4] = { (val >> 24) & 0xff, (val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff };
    fwrite(b, 1, 4, f);
}

static void write_png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    write_be32(f, len);
    uint8_t *crc_buf = malloc(4 + len);
    if (!crc_buf) return;
    memcpy(crc_buf, type, 4);
    if (len > 0 && data) memcpy(crc_buf + 4, data, len);
    fwrite(crc_buf, 1, 4 + len, f);
    uint32_t crc = calc_crc32(crc_buf, 4 + len);
    write_be32(f, crc);
    free(crc_buf);
}

/* Generates a crisp 256x256 "TS" PNG icon natively in pure C */
static void generate_ts_icon_png(const char *filepath) {
    const int w = 256, h = 256;
    size_t raw_len = (size_t)h * (1 + w * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return;

    size_t idx = 0;
    for (int y = 0; y < h; y++) {
        raw[idx++] = 0;
        for (int x = 0; x < w; x++) {
            uint8_t r = 12, g = 16, b = 28;
            if (x < 10 || x >= w - 10 || y < 10 || y >= h - 10) {
                r = 37; g = 99; b = 235;
            }
            int t_top  = (x >= 48  && x <= 122 && y >= 75  && y <= 98);
            int t_stem = (x >= 74  && x <= 96  && y >= 75  && y <= 182);
            int s_top  = (x >= 138 && x <= 208 && y >= 75  && y <= 96);
            int s_mid  = (x >= 138 && x <= 208 && y >= 118 && y <= 139);
            int s_bot  = (x >= 138 && x <= 208 && y >= 161 && y <= 182);
            int s_ul   = (x >= 138 && x <= 159 && y >= 75  && y <= 139);
            int s_lr   = (x >= 186 && x <= 208 && y >= 118 && y <= 182);

            if (t_top || t_stem || s_top || s_mid || s_bot || s_ul || s_lr) {
                r = 255; g = 255; b = 255;
            }
            raw[idx++] = r;
            raw[idx++] = g;
            raw[idx++] = b;
        }
    }

    size_t num_blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + num_blocks * 5 + raw_len + 4;
    uint8_t *zbuf = malloc(z_len);
    if (!zbuf) { free(raw); return; }

    size_t zpos = 0;
    zbuf[zpos++] = 0x78;
    zbuf[zpos++] = 0x01;

    size_t rem = raw_len;
    size_t rpos = 0;
    while (rem > 0) {
        uint16_t blen = (rem > 65535) ? 65535 : (uint16_t)rem;
        uint8_t final_blk = (rem <= 65535) ? 1 : 0;
        uint16_t nlen = ~blen;
        zbuf[zpos++] = final_blk;
        zbuf[zpos++] = blen & 0xff;
        zbuf[zpos++] = (blen >> 8) & 0xff;
        zbuf[zpos++] = nlen & 0xff;
        zbuf[zpos++] = (nlen >> 8) & 0xff;
        memcpy(zbuf + zpos, raw + rpos, blen);
        zpos += blen;
        rpos += blen;
        rem -= blen;
    }

    uint32_t adler = calc_adler32(raw, raw_len);
    zbuf[zpos++] = (adler >> 24) & 0xff;
    zbuf[zpos++] = (adler >> 16) & 0xff;
    zbuf[zpos++] = (adler >> 8) & 0xff;
    zbuf[zpos++] = adler & 0xff;

    FILE *f = fopen(filepath, "wb");
    if (f) {
        const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
        fwrite(sig, 1, 8, f);
        uint8_t ihdr[13] = { 0, 0, 1, 0, 0, 0, 1, 0, 8, 2, 0, 0, 0 };
        write_png_chunk(f, "IHDR", ihdr, 13);
        write_png_chunk(f, "IDAT", zbuf, (uint32_t)zpos);
        write_png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }

    free(zbuf);
    free(raw);
}

/*
 * Official PS5 Native App Installer (from ps5-payload-dev/ftpsrv/install-ps5.c)
 * Registers /user/app/TBSH00001 via libSceAppInstUtil.sprx so the PS5 allows
 * native deletion from the dashboard via Controller Options -> Delete!
 */
static int native_install_tabshifter_tile(void) {
    sceAppInstUtilInitialize();

    /* Clean any stale registration first if the folder was deleted */
    if (access("/user/app/" TITLE_ID "/sce_sys/param.json", F_OK) != 0) {
        sceAppInstUtilAppUnInstall(TITLE_ID);
    }

    mkdir("/user/app/" TITLE_ID, 0755);
    mkdir("/user/app/" TITLE_ID "/sce_sys", 0755);

    generate_ts_icon_png("/user/app/" TITLE_ID "/sce_sys/icon0.png");
    /* Also copy icon0.png to appmeta if it already exists so cached icons update */
    mkdir("/user/appmeta/" TITLE_ID, 0755);
    generate_ts_icon_png("/user/appmeta/" TITLE_ID "/icon0.png");

    FILE *pf = fopen("/user/app/" TITLE_ID "/sce_sys/param.json", "wb");
    if (pf) {
        const char *pjson =
            "{\n"
            "  \"titleId\": \"" TITLE_ID "\",\n"
            "  \"applicationCategoryType\": 65536,\n"
            "  \"deeplinkUri\": \"http://127.0.0.1:9095/\",\n"
            "  \"localizedParameters\": {\n"
            "    \"defaultLanguage\": \"en-US\",\n"
            "    \"en-US\": {\n"
            "      \"titleName\": \"TabShifter\"\n"
            "    }\n"
            "  }\n"
            "}\n";
        fwrite(pjson, 1, strlen(pjson), pf);
        fclose(pf);
    }

    int (*sceAppInstUtilAppInstallTitleDir)(const char*, const char*, void*) = NULL;
    const char *nid = "Wudg3Xe3heE";
    uint32_t handle = 0;

    if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
        sceAppInstUtilAppInstallTitleDir = (void *)kernel_dynlib_resolve(-1, handle, nid);
    }

    if (sceAppInstUtilAppInstallTitleDir) {
        return sceAppInstUtilAppInstallTitleDir(TITLE_ID, "/user/app/", 0);
    }
    return sceAppInstUtilAppInstallAll(0);
}

/* Completely removes the TabShifter tile using Sony's native uninstaller + DB cleanup */
static void native_uninstall_tabshifter_tile(void) {
    sceAppInstUtilInitialize();
    sceAppInstUtilAppUnInstall(TITLE_ID);

    unlink("/user/app/" TITLE_ID "/sce_sys/icon0.png");
    unlink("/user/app/" TITLE_ID "/sce_sys/param.json");
    rmdir("/user/app/" TITLE_ID "/sce_sys");
    rmdir("/user/app/" TITLE_ID);

    /* Also scrub any manual rows if present in app.db / appinfo.db */
    sqlite3 *db_app = NULL;
    sqlite3 *db_info = NULL;
    if (sqlite3_open(DB_APP_PATH, &db_app) == SQLITE_OK) {
        sqlite3_exec(db_app, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
        sqlite3_exec(db_app, "DELETE FROM tbl_contentinfo WHERE titleId='TBSH00001';", NULL, NULL, NULL);
        sqlite3_exec(db_app, "DELETE FROM tbl_conceptmetadata WHERE localConceptId='cid:local:TBSH00001';", NULL, NULL, NULL);

        sqlite3_stmt *ust = NULL;
        char user_ids[16][32];
        int ucount = 0;
        if (sqlite3_prepare_v2(db_app, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%';", -1, &ust, NULL) == SQLITE_OK) {
            while (sqlite3_step(ust) == SQLITE_ROW && ucount < 16) {
                const char *tname = (const char *)sqlite3_column_text(ust, 0);
                if (tname && strlen(tname) > 13) strncpy(user_ids[ucount++], tname + 13, 31);
            }
            sqlite3_finalize(ust);
        }
        for (int i = 0; i < ucount; i++) {
            char *dsql = sqlite3_mprintf(
                "DELETE FROM tbl_iconinfo_%s WHERE titleId='TBSH00001';"
                "DELETE FROM tbl_concepticoninfo_%s WHERE primaryTitleId='TBSH00001' OR localConceptId='cid:local:TBSH00001';"
                "DELETE FROM tbl_group_%s WHERE itemId='TBSH00001';",
                user_ids[i], user_ids[i], user_ids[i]);
            sqlite3_exec(db_app, dsql, NULL, NULL, NULL);
            sqlite3_free(dsql);
        }
        sqlite3_close(db_app);
    }

    if (sqlite3_open(DB_INFO_PATH, &db_info) == SQLITE_OK) {
        sqlite3_exec(db_info, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
        sqlite3_exec(db_info, "DELETE FROM tbl_appinfo WHERE titleId='TBSH00001';", NULL, NULL, NULL);
        sqlite3_exec(db_info, "DELETE FROM tbl_conceptinfo WHERE local_concept_id='cid:local:TBSH00001';", NULL, NULL, NULL);
        sqlite3_close(db_info);
    }
    remove_wal_locks();
}

static void patch_param_json_on_disk(const char *tid, int cat_type) {
    const char *paths[3] = {
        "/user/app/%s/sce_sys/param.json",
        "/user/appmeta/%s/param.json",
        "/system_data/priv/appmeta/%s/param.json"
    };

    for (int i = 0; i < 3; i++) {
        char path[256];
        snprintf(path, sizeof(path), paths[i], tid);
        FILE *f = fopen(path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0 || len > 512 * 1024) { fclose(f); continue; }

        char *json = malloc(len + 64);
        if (!json) { fclose(f); continue; }
        fread(json, 1, len, f);
        json[len] = '\0';
        fclose(f);

        char *pos = strstr(json, "\"applicationCategoryType\"");
        if (pos) {
            char *colon = strchr(pos, ':');
            if (colon) {
                FILE *out = fopen(path, "wb");
                if (out) {
                    fwrite(json, 1, (colon - json) + 1, out);
                    fprintf(out, " %d", cat_type);
                    char *end = colon + 1;
                    while (*end == ' ' || (*end >= '0' && *end <= '9')) end++;
                    fwrite(end, 1, strlen(end), out);
                    fclose(out);
                }
            }
        }
        free(json);
    }
}

static void update_1360_tile(const char *tid, int change_tab, int to_media, int change_vis, int visible) {
    sqlite3 *db_app = NULL;
    sqlite3 *db_info = NULL;

    if (sqlite3_open(DB_APP_PATH, &db_app) != SQLITE_OK) return;
    if (sqlite3_open(DB_INFO_PATH, &db_info) != SQLITE_OK) {
        sqlite3_close(db_app);
        return;
    }

    sqlite3_exec(db_app, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
    sqlite3_exec(db_info, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);

    char local_cid[128] = {0};
    int curr_disp = DISP_GAME;
    int curr_cat = CAT_GAME;
    int curr_video = 0;

    sqlite3_stmt *st = NULL;
    char *q = sqlite3_mprintf("SELECT dispLocation, categoryType, is3rdPartyVideoApp, localConceptId FROM tbl_contentinfo WHERE titleId = %Q;", tid);
    if (sqlite3_prepare_v2(db_app, q, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            curr_disp = sqlite3_column_int(st, 0);
            curr_cat = sqlite3_column_int(st, 1);
            curr_video = sqlite3_column_int(st, 2);
            const char *cid_txt = (const char *)sqlite3_column_text(st, 3);
            if (cid_txt) strncpy(local_cid, cid_txt, sizeof(local_cid) - 1);
        }
        sqlite3_finalize(st);
    }
    sqlite3_free(q);

    int is_media = change_tab ? to_media : ((curr_disp == DISP_MEDIA) || (curr_cat == CAT_MEDIA) || (curr_video == 1));
    int is_vis   = change_vis ? visible : (curr_disp > 1);

    int disp_loc     = is_media ? DISP_MEDIA : DISP_GAME;
    int concept_disp = is_media ? CONCEPT_DISP_MEDIA : CONCEPT_DISP_GAME;
    int cat_type     = is_media ? CAT_MEDIA : CAT_GAME;
    int is_video     = is_media ? 1 : 0;
    int app_type_val = is_media ? 0 : 5;
    int vis_flag     = is_vis ? 1 : 0;

    char hub_uri[128];
    snprintf(hub_uri, sizeof(hub_uri), is_media ? "psmediahub:main?titleId=%s" : "pshome:gamehub?titleId=%s", tid);

    if (!is_vis) {
        disp_loc = DISP_HIDDEN;
        concept_disp = CONCEPT_DISP_HIDDEN;
    } else {
        if (strcmp(tid, "NPXS40037") == 0) { disp_loc = 147; concept_disp = 2195; }
        else if (strcmp(tid, "NPXS40047") == 0) { disp_loc = 146; concept_disp = 2194; }
        else if (strcmp(tid, "NPXS40074") == 0 || strcmp(tid, "NPXS40099") == 0) { disp_loc = 155; concept_disp = 2203; }
    }

    char *sql_info = sqlite3_mprintf(
        "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'DISPLAYLOCATION';"
        "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'CATEGORY_TYPE';"
        "UPDATE tbl_appinfo SET val = %Q WHERE titleId = %Q AND key = 'HUBAPP_URI';"
        "UPDATE tbl_appinfo SET val = %d WHERE titleId = %Q AND key = 'APP_TYPE';"
        "UPDATE tbl_conceptinfo SET val = %d WHERE local_concept_id = %Q AND key = 'display_location';"
        "UPDATE tbl_conceptinfo SET val = %Q WHERE local_concept_id = %Q AND key = 'hubapp_uri';"
        "UPDATE tbl_version SET status = status + 1 WHERE category LIKE '%%sync_index%%';",
        disp_loc, tid, cat_type, tid, hub_uri, tid, app_type_val, tid,
        disp_loc, local_cid, hub_uri, local_cid);
    sqlite3_exec(db_info, sql_info, NULL, NULL, NULL);
    sqlite3_free(sql_info);

    char *sql_app = sqlite3_mprintf(
        "UPDATE tbl_contentinfo SET dispLocation = %d, categoryType = %d, is3rdPartyVideoApp = %d, appType = %d, pprHubAppUri = %Q, "
        "AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', %d, '$.CATEGORY_TYPE', %d, '$.HUBAPP_URI', %Q, '$.APP_TYPE', %d) "
        "WHERE titleId = %Q;"
        "UPDATE tbl_conceptmetadata SET dispLocation = %d, pprHubAppUri = %Q WHERE localConceptId = %Q;"
        "UPDATE tbl_version SET status = status + 1 WHERE category LIKE '%%sync_index%%';",
        disp_loc, cat_type, is_video, app_type_val, hub_uri,
        disp_loc, cat_type, hub_uri, app_type_val, tid,
        disp_loc, hub_uri, local_cid);
    sqlite3_exec(db_app, sql_app, NULL, NULL, NULL);
    sqlite3_free(sql_app);

    sqlite3_stmt *ust = NULL;
    char user_ids[16][32];
    int ucount = 0;
    if (sqlite3_prepare_v2(db_app, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%';", -1, &ust, NULL) == SQLITE_OK) {
        while (sqlite3_step(ust) == SQLITE_ROW && ucount < 16) {
            const char *tname = (const char *)sqlite3_column_text(ust, 0);
            if (tname && strlen(tname) > 13) strncpy(user_ids[ucount++], tname + 13, 31);
        }
        sqlite3_finalize(ust);
    }

    for (int i = 0; i < ucount; i++) {
        char *usql = sqlite3_mprintf(
            "UPDATE tbl_iconinfo_%s SET dispLocation = %d, visible = %d, hubAppUri = %Q WHERE titleId = %Q;"
            "UPDATE tbl_concepticoninfo_%s SET dispLocation = %d, validFlag = %d, hubAppUri = %Q WHERE primaryTitleId = %Q OR localConceptId = %Q;",
            user_ids[i], disp_loc, vis_flag, hub_uri, tid,
            user_ids[i], concept_disp, vis_flag, hub_uri, tid, local_cid);
        sqlite3_exec(db_app, usql, NULL, NULL, NULL);
        sqlite3_free(usql);

        if (!is_vis) {
            char *dsql = sqlite3_mprintf("DELETE FROM tbl_group_%s WHERE itemId = %Q;", user_ids[i], tid);
            sqlite3_exec(db_app, dsql, NULL, NULL, NULL);
            sqlite3_free(dsql);
        }
    }

    sqlite3_close(db_info);
    sqlite3_close(db_app);
    remove_wal_locks();

    if (change_tab) {
        patch_param_json_on_disk(tid, cat_type);
    }
}

static void toggle_all_bloatware(int visible) {
    const char *bloat_ids[] = { "NPXS40037", "NPXS40047", "NPXS40074", "NPXS40099", "NPXS40063", "NPXS40305" };
    for (int i = 0; i < 6; i++) {
        update_1360_tile(bloat_ids[i], 0, 0, 1, visible);
    }
}

static int is_bloat_id(const char *tid) {
    const char *bloat_ids[] = { "NPXS40037", "NPXS40047", "NPXS40074", "NPXS40099", "NPXS40063", "NPXS40305" };
    for (int i = 0; i < 6; i++) {
        if (strcmp(tid, bloat_ids[i]) == 0) return 1;
    }
    return 0;
}

static void send_dashboard_html(int client_fd) {
    const char *header =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>PS5 TabShifter v1.1</title>"
        "<style>"
        "body{background:#080a10;color:#f0f4f8;font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:24px;max-width:860px;margin:auto;}"
        "h1{font-size:24px;margin:0 0 6px 0;color:#fff;} .sub{color:#8b949e;font-size:14px;margin-bottom:18px;}"
        ".toolbar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:20px;}"
        ".tbtn{flex:1;min-width:160px;text-align:center;padding:14px;border-radius:12px;text-decoration:none;font-weight:700;font-size:14px;color:#fff;border:2px solid transparent;}"
        ".tbtn:focus,.tbtn:hover,.btn:focus,.btn:hover{border-color:#fff;transform:scale(1.02);}"
        ".b-hide{background:#7c3aed;} .b-show{background:#334155;} .b-res{background:#b45309;} .b-uninst{background:#dc2626;}"
        ".card{background:#121622;border:1px solid #22293a;border-radius:14px;padding:14px 18px;margin-bottom:10px;display:flex;justify-content:space-between;align-items:center;gap:12px;}"
        ".title{font-weight:700;font-size:16px;color:#fff;} .meta{color:#8b949e;font-size:13px;margin-top:4px;}"
        ".badge-m{background:#1d4ed8;color:#fff;padding:2px 8px;border-radius:6px;font-size:11px;font-weight:700;}"
        ".badge-g{background:#059669;color:#fff;padding:2px 8px;border-radius:6px;font-size:11px;font-weight:700;}"
        ".actions{display:flex;gap:8px;}"
        ".btn{padding:10px 15px;border-radius:10px;text-decoration:none;font-weight:700;font-size:13px;border:2px solid transparent;}"
        ".to-media{background:#2563eb;color:#fff;} .to-game{background:#10b981;color:#fff;}"
        ".h-btn{background:#1e293b;color:#cbd5e1;border:1px solid #334155;}"
        "</style></head><body>"
        "<h1>PS5 TabShifter v1.1 (FW 13.60)</h1>"
        "<div class='sub'>Move apps between <b>Games</b> &amp; <b>Media</b> tabs or hide dead PSN bloatware with instant UI reload.</div>"
        "<div class='toolbar'>"
        "<a class='tbtn b-hide' href='/bloat?v=0'>🧹 Hide All PSN Bloatware</a>"
        "<a class='tbtn b-show' href='/bloat?v=1'>Unhide PSN Bloatware</a>"
        "<a class='tbtn b-res' href='/restore'>🔄 Restore DB Backup</a>"
        "<a class='tbtn b-uninst' href='/uninstall'>🗑️ Uninstall Tile &amp; Exit</a>"
        "</div>";

    write(client_fd, header, strlen(header));

    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(DB_APP_PATH, &db) == SQLITE_OK) {
        const char *q = "SELECT titleId, titleName, dispLocation, categoryType, is3rdPartyVideoApp FROM tbl_contentinfo ORDER BY dispLocation DESC, titleName ASC;";
        if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *tid = (const char *)sqlite3_column_text(stmt, 0);
                const char *tname = (const char *)sqlite3_column_text(stmt, 1);
                int disp = sqlite3_column_int(stmt, 2);
                int ctype = sqlite3_column_int(stmt, 3);
                int is_vid = sqlite3_column_int(stmt, 4);

                if (!tid) continue;
                if (strncmp(tid, "NPXS", 4) == 0 && !is_bloat_id(tid)) continue;

                int is_media = (disp == DISP_MEDIA) || (ctype == CAT_MEDIA) || (is_vid == 1);
                int is_vis = (disp > 1);

                char row[1400];
                snprintf(row, sizeof(row),
                    "<div class='card' style='opacity:%s'>"
                    "<div><div class='title'>%s %s</div>"
                    "<div class='meta'><span class='%s'>%s</span> &bull; %s</div></div>"
                    "<div class='actions'>"
                    "<a class='btn %s' href='/shift?tid=%s&media=%d'>Move to %s</a>"
                    "<a class='btn h-btn' href='/vis?tid=%s&v=%d'>%s</a>"
                    "</div></div>",
                    is_vis ? "1.0" : "0.45",
                    tname ? tname : tid,
                    is_vis ? "" : "(HIDDEN)",
                    is_media ? "badge-m" : "badge-g",
                    is_media ? "MEDIA TAB" : "GAMES TAB",
                    tid,
                    is_media ? "to-game" : "to-media", tid, is_media ? 0 : 1, is_media ? "Games" : "Media",
                    tid, is_vis ? 0 : 1, is_vis ? "Hide" : "Unhide");
                write(client_fd, row, strlen(row));
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    const char *footer = "</body></html>";
    write(client_fd, footer, strlen(footer));
}

int main(void) {
    backup_databases_once();
    native_install_tabshifter_tile();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ps5_notify("TabShifter Refreshed on Media Tab!");
        return 0;
    }

    listen(server_fd, 5);
    ps5_notify("PS5 TabShifter v1.1 Active!\nLaunch TabShifter from your Media Tab!");

    while (g_running) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

        char req[1024] = {0};
        read(client, req, sizeof(req) - 1);

        if (strncmp(req, "GET /shift?tid=", 15) == 0) {
            char tid[32] = {0};
            int to_media = 0;
            sscanf(req + 15, "%31[^&]&media=%d", tid, &to_media);
            update_1360_tile(tid, 1, to_media, 0, 1);
            ps5_notify("Moved %s to %s Tab!", tid, to_media ? "Media" : "Games");
            restart_sceshellui();
            write(client, "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n", 35);
        } else if (strncmp(req, "GET /vis?tid=", 13) == 0) {
            char tid[32] = {0};
            int v = 0;
            sscanf(req + 13, "%31[^&]&v=%d", tid, &v);
            update_1360_tile(tid, 0, 0, 1, v);
            ps5_notify("%s %s!", v ? "Unhid" : "Hid", tid);
            restart_sceshellui();
            write(client, "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n", 35);
        } else if (strncmp(req, "GET /bloat?v=", 13) == 0) {
            int v = atoi(req + 13);
            toggle_all_bloatware(v);
            ps5_notify("%s PSN Bloatware!", v ? "Restored" : "Hid");
            restart_sceshellui();
            write(client, "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n", 35);
        } else if (strncmp(req, "GET /restore", 12) == 0) {
            force_copy_file(DB_INFO_BAK, DB_INFO_PATH);
            force_copy_file(DB_APP_BAK, DB_APP_PATH);
            remove_wal_locks();
            ps5_notify("Restored Original Databases!");
            restart_sceshellui();
            write(client, "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n", 35);
        } else if (strncmp(req, "GET /uninstall", 14) == 0) {
            const char *bye = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
                              "<body style='background:#080a10;color:#fff;font-family:sans-serif;text-align:center;padding-top:100px;'>"
                              "<h1>TabShifter Tile Uninstalled &amp; Payload Stopped.</h1><p>You can now press the PS Button to return to the Home Screen.</p></body>";
            write(client, bye, strlen(bye));
            close(client);
            native_uninstall_tabshifter_tile();
            ps5_notify("TabShifter Tile Uninstalled & Stopped!");
            restart_sceshellui();
            g_running = 0;
            break;
        } else {
            send_dashboard_html(client);
        }
        close(client);
    }

    close(server_fd);
    return 0;
}
