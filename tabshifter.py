/*
 * PS5 TabShifter.elf v1.0 (Native PS5 On-Console Edition for FW 13.60+)
 * - Runs directly on the PS5 (Zero PC required!)
 * - Full Dual-Database 13.60 Engine: patches appinfo.db + app.db (all 4 tables) + param.json
 * - Built-in PS5 TV / Controller Dark-Mode UI on http://127.0.0.1:9095
 * - Native SceShellUI SIGKILL auto-refresh in 1.5 seconds
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>

#define PORT 9095

#define DB_APP_PATH  "/system_data/priv/mms/app.db"
#define DB_INFO_PATH "/system_data/priv/mms/appinfo.db"
#define DB_APP_BAK   "/system_data/priv/mms/app.db.tabshifter.bak"
#define DB_INFO_BAK  "/system_data/priv/mms/appinfo.db.tabshifter.bak"

/* Exact FW 13.60 Constants */
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

/* Native PS5 Kernel Process Scanner: Finds SceShellUI and sends SIGKILL for instant 1.5s UI reload */
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

/* Patches applicationCategoryType in param.json on disk so changes survive database rebuilds */
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

/* Core FW 13.60 Dual-DB Updater: Updates appinfo.db + app.db (all 4 tables) + param.json */
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

    /* Fetch current values & localConceptId from tbl_contentinfo */
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

    /* 1. Update appinfo.db (tbl_appinfo & tbl_conceptinfo) */
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

    /* 2. Update app.db (tbl_contentinfo & tbl_conceptmetadata) */
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

    /* 3. Dynamically iterate all per-user tables (tbl_iconinfo_<UID> & tbl_concepticoninfo_<UID>) */
    sqlite3_stmt *ust = NULL;
    char user_ids[16][32];
    int ucount = 0;
    if (sqlite3_prepare_v2(db_app, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%';", -1, &ust, NULL) == SQLITE_OK) {
        while (sqlite3_step(ust) == SQLITE_ROW && ucount < 16) {
            const char *tname = (const char *)sqlite3_column_text(ust, 0);
            if (tname && strlen(tname) > 13) {
                strncpy(user_ids[ucount++], tname + 13, 31);
            }
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

/* Renders the PS5 Controller-Friendly Dark-Mode UI on Port 9095 */
static void send_dashboard_html(int client_fd) {
    const char *header =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>PS5 TabShifter v1.0</title>"
        "<style>"
        "body{background:#080a10;color:#f0f4f8;font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:24px;max-width:860px;margin:auto;}"
        "h1{font-size:24px;margin:0 0 6px 0;color:#fff;} .sub{color:#8b949e;font-size:14px;margin-bottom:18px;}"
        ".toolbar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:20px;}"
        ".tbtn{flex:1;min-width:160px;text-align:center;padding:14px;border-radius:12px;text-decoration:none;font-weight:700;font-size:14px;color:#fff;border:2px solid transparent;}"
        ".tbtn:focus,.tbtn:hover,.btn:focus,.btn:hover{border-color:#fff;transform:scale(1.02);}"
        ".b-hide{background:#7c3aed;} .b-show{background:#334155;} .b-res{background:#b45309;}"
        ".card{background:#121622;border:1px solid #22293a;border-radius:14px;padding:14px 18px;margin-bottom:10px;display:flex;justify-content:space-between;align-items:center;gap:12px;}"
        ".title{font-weight:700;font-size:16px;color:#fff;} .meta{color:#8b949e;font-size:13px;margin-top:4px;}"
        ".badge-m{background:#1d4ed8;color:#fff;padding:2px 8px;border-radius:6px;font-size:11px;font-weight:700;}"
        ".badge-g{background:#059669;color:#fff;padding:2px 8px;border-radius:6px;font-size:11px;font-weight:700;}"
        ".actions{display:flex;gap:8px;}"
        ".btn{padding:10px 15px;border-radius:10px;text-decoration:none;font-weight:700;font-size:13px;border:2px solid transparent;}"
        ".to-media{background:#2563eb;color:#fff;} .to-game{background:#10b981;color:#fff;}"
        ".h-btn{background:#1e293b;color:#cbd5e1;border:1px solid #334155;}"
        "</style></head><body>"
        "<h1>PS5 TabShifter v1.0 (FW 13.60)</h1>"
        "<div class='sub'>Move apps between <b>Games</b> &amp; <b>Media</b> tabs or hide dead PSN bloatware with instant UI reload.</div>"
        "<div class='toolbar'>"
        "<a class='tbtn b-hide' href='/bloat?v=0'>🧹 Hide All PSN Bloatware</a>"
        "<a class='tbtn b-show' href='/bloat?v=1'>Unhide PSN Bloatware</a>"
        "<a class='tbtn b-res' href='/restore'>🔄 Restore Original DB Backup</a>"
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

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ps5_notify("TabShifter Error: Port %d already in use!", PORT);
        return 1;
    }

    listen(server_fd, 5);
    ps5_notify("PS5 TabShifter v1.0 Ready!\nOpen http://<PS5_IP>:%d", PORT);

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
        } else {
            send_dashboard_html(client);
        }
        close(client);
    }

    close(server_fd);
    return 0;
}