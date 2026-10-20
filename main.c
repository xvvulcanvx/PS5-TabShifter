#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sqlite3.h>

#define TABSHIFTER_VERSION      "1.3.0"
#define DEFAULT_ELF_FALLBACK    "/data/payloads/PS5-TabShifter.elf"
#define APP_DB_PATH             "/system_data/priv/mms/app.db"
#define APPINFO_DB_PATH         "/system_data/priv/mms/appinfo.db"

#define DISP_LOCATION_HIDDEN    1
#define DISP_LOCATION_GAMES     138
#define DISP_LOCATION_MEDIA     188

#define CATEGORY_PS5_GAME       0
#define CATEGORY_PS4_GAME       61440
#define CATEGORY_MEDIA_APP      65536

#define SQLITE_BUSY_TIMEOUT_MS  10000
#define MAX_USER_PROFILES       16
#define HTTP_PORT               8088

/* ========================================================================= */
/*                    BINARY SELF-PATH RESOLVER (FREEBSD)                    */
/* ========================================================================= */

static void get_running_elf_path(char *out_path, size_t max_len) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t len = max_len;
    if (sysctl(mib, 4, out_path, &len, NULL, 0) == 0 && len > 1 && out_path[0] == '/') {
        return;
    }
    strncpy(out_path, DEFAULT_ELF_FALLBACK, max_len - 1);
    out_path[max_len - 1] = '\0';
}

static int apply_self_update(const uint8_t *binary_data, size_t binary_len) {
    if (!binary_data || binary_len < 1024) return -1;

    /* Verify ELF Magic: 0x7F 'E' 'L' 'F' */
    if (binary_data[0] != 0x7F || binary_data[1] != 'E' ||
        binary_data[2] != 'L'  || binary_data[3] != 'F') {
        return -2;
    }

    char target_path[512];
    char temp_path[540];
    get_running_elf_path(target_path, sizeof(target_path));
    snprintf(temp_path, sizeof(temp_path), "%s.new", target_path);

    /* Ensure /data/payloads directory exists if using fallback */
    mkdir("/data/payloads", 0777);

    FILE *fp = fopen(temp_path, "wb");
    if (!fp) return -3;

    size_t written = fwrite(binary_data, 1, binary_len, fp);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (written != binary_len) {
        unlink(temp_path);
        return -4;
    }

    chmod(temp_path, 0777);

    /* Atomic inode swap (avoids ETXTBSY on active FreeBSD binaries) */
    if (rename(temp_path, target_path) != 0) {
        unlink(temp_path);
        return -5;
    }

    return 0;
}

/* ========================================================================= */
/*                   MULTI-USER PROFILE & TABLE DISCOVERY                    */
/* ========================================================================= */

static int get_all_user_ids(sqlite3 *db, char uids[MAX_USER_PROFILES][32]) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT name FROM sqlite3_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%';";
    int count = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        const char *prefix = "tbl_iconinfo_";
        size_t prefix_len = strlen(prefix);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < MAX_USER_PROFILES) {
            const char *tname = (const char *)sqlite3_column_text(stmt, 0);
            if (tname && strncmp(tname, prefix, prefix_len) == 0) {
                strncpy(uids[count], tname + prefix_len, 31);
                uids[count][31] = '\0';
                count++;
            }
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

static bool table_exists(sqlite3 *db, const char *table_name) {
    sqlite3_stmt *stmt;
    bool exists = false;
    const char *sql = "SELECT 1 FROM sqlite3_master WHERE type='table' AND name = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = true;
        sqlite3_finalize(stmt);
    }
    return exists;
}

static bool is_protected_payload_shortcut(const char *title_id) {
    if (!title_id) return true;
    /* Guardrail against Issue #12 (Game Compressor & self-tile panics) */
    if (strcmp(title_id, "TBSH00001") == 0 ||
        strncmp(title_id, "GMCM", 4) == 0 ||
        strncmp(title_id, "COMP", 4) == 0) {
        return true;
    }
    return false;
}

/* ========================================================================= */
/*              TOKEN-SAFE UNIVERSAL DUAL-SCHEMA JSON PATCHER                */
/* ========================================================================= */

static char *patch_json_universal(const char *json_in, const char *title_id, int target_loc, const char *new_title) {
    if (!json_in) return NULL;

    size_t in_len = strlen(json_in);
    size_t max_out = in_len + 4096;
    char *json_out = calloc(1, max_out);
    if (!json_out) return NULL;

    char hub_uri[128];
    if (target_loc == DISP_LOCATION_MEDIA) {
        snprintf(hub_uri, sizeof(hub_uri), "psmediahub:main?titleId=%s", title_id);
    } else {
        snprintf(hub_uri, sizeof(hub_uri), "pshome:gamehub?titleId=%s", title_id);
    }

    int cat_type = (target_loc == DISP_LOCATION_MEDIA) ? CATEGORY_MEDIA_APP :
                   ((strncmp(title_id, "CUSA", 4) == 0) ? CATEGORY_PS4_GAME : CATEGORY_PS5_GAME);

    /* --- SCHEMA A: `field_list` Array Format (FW 5.00-10.xx & all ConceptInfoJson) --- */
    if (strstr(json_in, "\"field_list\"") != NULL) {
        const char *p = json_in;
        char *dst = json_out;

        const char *fl_start = strstr(json_in, "[");
        if (!fl_start) {
            strcpy(json_out, json_in);
            return json_out;
        }

        /* Copy prefix up to '[' */
        size_t prefix_len = (size_t)(fl_start - json_in) + 1;
        memcpy(dst, json_in, prefix_len);
        dst += prefix_len;
        p = fl_start + 1;

        while (*p) {
            if (*p == '{') {
                /* Find matching '}' while respecting string quotes */
                const char *obj_start = p;
                const char *scan = p + 1;
                bool in_quotes = false;
                while (*scan) {
                    if (*scan == '\"' && *(scan - 1) != '\\') in_quotes = !in_quotes;
                    else if (*scan == '}' && !in_quotes) break;
                    scan++;
                }

                if (*scan == '}') {
                    size_t obj_len = (size_t)(scan - obj_start) + 1;
                    char obj_buf[1024];
                    if (obj_len < sizeof(obj_buf)) {
                        memcpy(obj_buf, obj_start, obj_len);
                        obj_buf[obj_len] = '\0';

                        if (target_loc > 0 && strstr(obj_buf, "\"key\":\"display_location\"") != NULL) {
                            dst += sprintf(dst, "{\"data\":%d,\"key\":\"display_location\",\"size\":8,\"type\":0}", target_loc);
                            p = scan + 1;
                            continue;
                        }
                        if (target_loc > 0 && strstr(obj_buf, "\"key\":\"hubapp_uri\"") != NULL && strncmp(title_id, "NPXS", 4) != 0) {
                            dst += sprintf(dst, "{\"data\":\"%s\",\"key\":\"hubapp_uri\",\"size\":%zu,\"type\":2}", hub_uri, strlen(hub_uri));
                            p = scan + 1;
                            continue;
                        }
                        if (new_title && new_title[0] != '\0' && strstr(obj_buf, "\"key\":\"concept_name\"") != NULL) {
                            dst += sprintf(dst, "{\"data\":\"%s\",\"key\":\"concept_name\",\"size\":%zu,\"type\":2}", new_title, strlen(new_title));
                            p = scan + 1;
                            continue;
                        }
                        if (new_title && new_title[0] != '\0' && strstr(obj_buf, "\"key\":\"concept_name_01\"") != NULL) {
                            dst += sprintf(dst, "{\"data\":\"%s\",\"key\":\"concept_name_01\",\"size\":%zu,\"type\":2}", new_title, strlen(new_title));
                            p = scan + 1;
                            continue;
                        }
                    }
                    memcpy(dst, obj_start, obj_len);
                    dst += obj_len;
                    p = scan + 1;
                    continue;
                }
            }
            *dst++ = *p++;
        }
        *dst = '\0';
        return json_out;
    }

    /* --- SCHEMA B: Modern Flat Key-Value JSON (FW 11.40-13.60 AppInfoJson) --- */
    const char *src = json_in;
    char *dst = json_out;

    while (*src) {
        if (target_loc > 0 && strncmp(src, "\"DISPLAYLOCATION\":", 18) == 0) {
            dst += sprintf(dst, "\"DISPLAYLOCATION\":%d", target_loc);
            src += 18;
            while (*src == '-' || (*src >= '0' && *src <= '9')) src++;
            continue;
        }
        if (target_loc > 0 && strncmp(title_id, "NPXS", 4) != 0 && strncmp(src, "\"CATEGORY_TYPE\":", 16) == 0) {
            dst += sprintf(dst, "\"CATEGORY_TYPE\":%d", cat_type);
            src += 16;
            while (*src == '-' || (*src >= '0' && *src <= '9')) src++;
            continue;
        }
        if (target_loc > 0 && strncmp(title_id, "NPXS", 4) != 0 && strncmp(src, "\"HUBAPP_URI\":\"", 14) == 0) {
            dst += sprintf(dst, "\"HUBAPP_URI\":\"%s\"", hub_uri);
            src += 14;
            while (*src && !(*src == '\"' && *(src - 1) != '\\')) src++;
            if (*src == '\"') src++;
            continue;
        }
        if (new_title && new_title[0] != '\0' && strncmp(src, "\"TITLE\":\"", 9) == 0) {
            dst += sprintf(dst, "\"TITLE\":\"%s\"", new_title);
            src += 9;
            while (*src && !(*src == '\"' && *(src - 1) != '\\')) src++;
            if (*src == '\"') src++;
            continue;
        }
        if (new_title && new_title[0] != '\0' && strncmp(src, "\"TITLE_01\":\"", 12) == 0) {
            dst += sprintf(dst, "\"TITLE_01\":\"%s\"", new_title);
            src += 12;
            while (*src && !(*src == '\"' && *(src - 1) != '\\')) src++;
            if (*src == '\"') src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return json_out;
}

/* ========================================================================= */
/*               CORE DATABASE OPERATIONS (SHIFT, HIDE, PIN, RENAME)         */
/* ========================================================================= */

static int internal_shift_single(sqlite3 *app_db, sqlite3 *appinfo_db,
                                 char uids[MAX_USER_PROFILES][32], int uid_count,
                                 const char *title_id, int target_loc) {
    if (is_protected_payload_shortcut(title_id)) return SQLITE_OK;

    sqlite3_stmt *stmt;
    char sql[512];
    char local_cid[128] = {0};
    char *app_json = NULL;
    char *concept_json = NULL;

    /* 1. Fetch localConceptId & AppInfoJson from tbl_contentinfo */
    if (sqlite3_prepare_v2(app_db, "SELECT localConceptId, AppInfoJson FROM tbl_contentinfo WHERE titleId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cid = (const char *)sqlite3_column_text(stmt, 0);
            const char *aj  = (const char *)sqlite3_column_text(stmt, 1);
            if (cid) strncpy(local_cid, cid, sizeof(local_cid) - 1);
            if (aj)  app_json = strdup(aj);
        }
        sqlite3_finalize(stmt);
    }

    if (local_cid[0] == '\0') {
        snprintf(local_cid, sizeof(local_cid), "cid:local:%s", title_id);
    }

    /* 2. Fetch ConceptInfoJson from tbl_conceptmetadata */
    if (sqlite3_prepare_v2(app_db, "SELECT ConceptInfoJson FROM tbl_conceptmetadata WHERE localConceptId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, local_cid, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cj = (const char *)sqlite3_column_text(stmt, 0);
            if (cj) concept_json = strdup(cj);
        }
        sqlite3_finalize(stmt);
    }

    char *patched_app_json     = patch_json_universal(app_json, title_id, target_loc, NULL);
    char *patched_concept_json = patch_json_universal(concept_json, title_id, target_loc, NULL);
    free(app_json);
    free(concept_json);

    int cat_type = (target_loc == DISP_LOCATION_MEDIA) ? CATEGORY_MEDIA_APP :
                   ((strncmp(title_id, "CUSA", 4) == 0) ? CATEGORY_PS4_GAME : CATEGORY_PS5_GAME);
    int is_video = (target_loc == DISP_LOCATION_MEDIA) ? 1 : 0;

    char hub_uri[128];
    if (target_loc == DISP_LOCATION_MEDIA) {
        snprintf(hub_uri, sizeof(hub_uri), "psmediahub:main?titleId=%s", title_id);
    } else {
        snprintf(hub_uri, sizeof(hub_uri), "pshome:gamehub?titleId=%s", title_id);
    }

    /* 3. Update tbl_contentinfo */
    const char *up_content =
        "UPDATE tbl_contentinfo SET dispLocation = ?, categoryType = ?, is3rdPartyVideoApp = ?, "
        "pprHubAppUri = CASE WHEN titleId LIKE 'NPXS%' THEN pprHubAppUri ELSE ? END, "
        "AppInfoJson = coalesce(?, AppInfoJson) WHERE titleId = ?;";
    if (sqlite3_prepare_v2(app_db, up_content, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_loc);
        sqlite3_bind_int(stmt, 2, cat_type);
        sqlite3_bind_int(stmt, 3, is_video);
        sqlite3_bind_text(stmt, 4, hub_uri, -1, SQLITE_STATIC);
        if (patched_app_json) sqlite3_bind_text(stmt, 5, patched_app_json, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, 5);
        sqlite3_bind_text(stmt, 6, title_id, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* 4. Update tbl_conceptmetadata */
    const char *up_concept =
        "UPDATE tbl_conceptmetadata SET dispLocation = ?, "
        "pprHubAppUri = CASE WHEN localConceptId LIKE 'cid:local:NPXS%' THEN pprHubAppUri ELSE ? END, "
        "ConceptInfoJson = coalesce(?, ConceptInfoJson) WHERE localConceptId = ?;";
    if (sqlite3_prepare_v2(app_db, up_concept, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_loc);
        sqlite3_bind_text(stmt, 2, hub_uri, -1, SQLITE_STATIC);
        if (patched_concept_json) sqlite3_bind_text(stmt, 3, patched_concept_json, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, 3);
        sqlite3_bind_text(stmt, 4, local_cid, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    free(patched_app_json);
    free(patched_concept_json);

    /* 5. Update all User Profile Presentation Tables */
    for (int i = 0; i < uid_count; i++) {
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_iconinfo_%s SET dispLocation = ?, visible = 1, "
                 "hubAppUri = CASE WHEN titleId LIKE 'NPXS%%' THEN hubAppUri ELSE ? END WHERE titleId = ?;",
                 uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_loc);
            sqlite3_bind_text(stmt, 2, hub_uri, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_concepticoninfo_%s SET dispLocation = ?, "
                 "hubAppUri = CASE WHEN primaryTitleId LIKE 'NPXS%%' THEN hubAppUri ELSE ? END "
                 "WHERE localConceptId = ? OR primaryTitleId = ?;",
                 uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_loc);
            sqlite3_bind_text(stmt, 2, hub_uri, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, local_cid, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    /* 6. Dual-Sync appinfo.db (String Keys Verified in FW 11.40 Dump) */
    if (appinfo_db) {
        if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_appinfo SET val = ? WHERE titleId = ? AND key = 'DISPLAYLOCATION';", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_loc);
            sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        if (strncmp(title_id, "NPXS", 4) != 0) {
            if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_appinfo SET val = ? WHERE titleId = ? AND key = 'CATEGORY_TYPE';", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, cat_type);
                sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_appinfo SET val = ? WHERE titleId = ? AND key = 'HUBAPP_URI';", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, hub_uri, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
        if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_conceptinfo SET val = ? WHERE local_concept_id = ? AND key = 'display_location';", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_loc);
            sqlite3_bind_text(stmt, 2, local_cid, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return SQLITE_OK;
}

/* ========================================================================= */
/*            PERSISTENT BLOATWARE HIDE / UNHIDE (ISSUES #16 & #17)          */
/* ========================================================================= */

static int internal_set_visibility(sqlite3 *app_db, sqlite3 *appinfo_db,
                                   char uids[MAX_USER_PROFILES][32], int uid_count,
                                   const char *title_id, bool hide) {
    sqlite3_stmt *stmt;
    char sql[512];

    int restore_loc = DISP_LOCATION_GAMES;
    if (strcmp(title_id, "NPXS40053") == 0 || strcmp(title_id, "NPXS40054") == 0 ||
        strcmp(title_id, "NPXS40056") == 0 || strcmp(title_id, "NPXS40139") == 0) {
        restore_loc = DISP_LOCATION_MEDIA;
    }
    int target_loc = hide ? DISP_LOCATION_HIDDEN : restore_loc;

    internal_shift_single(app_db, appinfo_db, uids, uid_count, title_id, target_loc);

    for (int i = 0; i < uid_count; i++) {
        /* 1. Toggle visible flag in tbl_iconinfo_<UID> */
        snprintf(sql, sizeof(sql), "UPDATE tbl_iconinfo_%s SET visible = ?, dispLocation = ? WHERE titleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, hide ? 0 : 1);
            sqlite3_bind_int(stmt, 2, target_loc);
            sqlite3_bind_text(stmt, 3, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        /* 2. Set pushed:false in tbl_info_<UID> so sleep/wake daemon does not re-advertise */
        snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO tbl_info_%s (key, value) VALUES (?, ?);", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            char flag_key[128];
            snprintf(flag_key, sizeof(flag_key), "nsx_content_flag_%s", title_id);
            const char *flag_val = hide ? "{\"deleted\":true,\"pushed\":false}" : "{\"deleted\":false,\"pushed\":true}";
            sqlite3_bind_text(stmt, 1, flag_key, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, flag_val, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        /* 3. Purge from tbl_group_<UID> MyHome on FW 11.40 - 13.60 */
        char grp_table[64];
        snprintf(grp_table, sizeof(grp_table), "tbl_group_%s", uids[i]);
        if (hide && table_exists(app_db, grp_table)) {
            snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE itemId = ? AND groupName = 'MyHome';", grp_table);
            if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }
    return SQLITE_OK;
}

/* ========================================================================= */
/*                    HARDWARE PINNING SYNC (ISSUE #11)                      */
/* ========================================================================= */

static int internal_set_pin(sqlite3 *app_db, char uids[MAX_USER_PROFILES][32], int uid_count,
                            const char *title_id, bool pin) {
    sqlite3_stmt *stmt;
    char sql[512];

    for (int i = 0; i < uid_count; i++) {
        snprintf(sql, sizeof(sql), "UPDATE tbl_iconinfo_%s SET pinnedStatus = ? WHERE titleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, pin ? 1 : 2);
            sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        char grp_table[64];
        snprintf(grp_table, sizeof(grp_table), "tbl_group_%s", uids[i]);
        if (table_exists(app_db, grp_table)) {
            if (pin) {
                snprintf(sql, sizeof(sql),
                         "INSERT OR REPLACE INTO %s (itemId, groupName, type, idx, lastModifiedDate) "
                         "VALUES (?, 'MyHome', 'title', 0, datetime('now'));", grp_table);
                if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            } else {
                snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE itemId = ? AND groupName = 'MyHome';", grp_table);
                if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
        }
    }
    return SQLITE_OK;
}

/* ========================================================================= */
/*               CUSTOM TITLE RENAMER & ICON CACHE-BUSTER                    */
/* ========================================================================= */

static int internal_rename_title(sqlite3 *app_db, sqlite3 *appinfo_db,
                                 char uids[MAX_USER_PROFILES][32], int uid_count,
                                 const char *title_id, const char *new_name) {
    if (!new_name || new_name[0] == '\0') return SQLITE_ERROR;

    sqlite3_stmt *stmt;
    char sql[512];
    char local_cid[128] = {0};
    char *app_json = NULL;
    char *concept_json = NULL;

    if (sqlite3_prepare_v2(app_db, "SELECT localConceptId, AppInfoJson FROM tbl_contentinfo WHERE titleId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cid = (const char *)sqlite3_column_text(stmt, 0);
            const char *aj  = (const char *)sqlite3_column_text(stmt, 1);
            if (cid) strncpy(local_cid, cid, sizeof(local_cid) - 1);
            if (aj)  app_json = strdup(aj);
        }
        sqlite3_finalize(stmt);
    }

    if (local_cid[0] != '\0') {
        if (sqlite3_prepare_v2(app_db, "SELECT ConceptInfoJson FROM tbl_conceptmetadata WHERE localConceptId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, local_cid, -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *cj = (const char *)sqlite3_column_text(stmt, 0);
                if (cj) concept_json = strdup(cj);
            }
            sqlite3_finalize(stmt);
        }
    }

    char *patched_app_json     = patch_json_universal(app_json, title_id, 0, new_name);
    char *patched_concept_json = patch_json_universal(concept_json, title_id, 0, new_name);
    free(app_json);
    free(concept_json);

    if (sqlite3_prepare_v2(app_db, "UPDATE tbl_contentinfo SET titleName = ?, AppInfoJson = coalesce(?, AppInfoJson) WHERE titleId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
        if (patched_app_json) sqlite3_bind_text(stmt, 2, patched_app_json, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, 2);
        sqlite3_bind_text(stmt, 3, title_id, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (local_cid[0] != '\0') {
        if (sqlite3_prepare_v2(app_db, "UPDATE tbl_conceptmetadata SET conceptName = ?, ConceptInfoJson = coalesce(?, ConceptInfoJson) WHERE localConceptId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
            if (patched_concept_json) sqlite3_bind_text(stmt, 2, patched_concept_json, -1, SQLITE_STATIC);
            else sqlite3_bind_null(stmt, 2);
            sqlite3_bind_text(stmt, 3, local_cid, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    free(patched_app_json);
    free(patched_concept_json);

    for (int i = 0; i < uid_count; i++) {
        snprintf(sql, sizeof(sql), "UPDATE tbl_iconinfo_%s SET titleName = ?, utmTitleName = ? WHERE titleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, new_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        snprintf(sql, sizeof(sql), "UPDATE tbl_concepticoninfo_%s SET conceptName = ?, primaryTitleName = ?, ucmConceptName = ? WHERE primaryTitleId = ? OR localConceptId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, new_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, new_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, title_id, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 5, local_cid, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    if (appinfo_db) {
        if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_appinfo SET val = ? WHERE titleId = ? AND (key = 'TITLE' OR key LIKE 'TITLE_%');", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        if (local_cid[0] != '\0') {
            if (sqlite3_prepare_v2(appinfo_db, "UPDATE tbl_conceptinfo SET val = ? WHERE local_concept_id = ? AND (key = 'concept_name' OR key LIKE 'concept_name_%');", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, local_cid, -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }

    return SQLITE_OK;
}

static int internal_refresh_icon(sqlite3 *app_db, char uids[MAX_USER_PROFILES][32], int uid_count, const char *title_id) {
    sqlite3_stmt *stmt;
    char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "/user/app/%s/sce_sys/icon0.png?ts=%ld", title_id, (long)time(NULL));

    struct stat st;
    char check_meta[256];
    snprintf(check_meta, sizeof(check_meta), "/user/appmeta/%s/icon0.png", title_id);
    if (stat(check_meta, &st) == 0) {
        snprintf(icon_path, sizeof(icon_path), "/user/appmeta/%s/icon0.png?ts=%ld", title_id, (long)time(NULL));
    }

    if (sqlite3_prepare_v2(app_db, "UPDATE tbl_contentinfo SET icon0Info = ? WHERE titleId = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, icon_path, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    for (int i = 0; i < uid_count; i++) {
        char sql[512];
        snprintf(sql, sizeof(sql), "UPDATE tbl_concepticoninfo_%s SET icon0Info = ? WHERE primaryTitleId = ?;", uids[i]);
        if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, icon_path, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return SQLITE_OK;
}

/* ========================================================================= */
/*          SMART TIDY & NON-DESTRUCTIVE LAYOUT RESET (ISSUE #13)            */
/* ========================================================================= */

static int execute_smart_tidy(sqlite3 *app_db, sqlite3 *appinfo_db, char uids[MAX_USER_PROFILES][32], int uid_count) {
    sqlite3_stmt *stmt;
    const char *homebrew_prefixes[] = { "PLDM", "PMNT", "FMGR", "WKAL", "FAKE", "ITEM", "LAPY", "PPSA99", NULL };
    const char *bloat_ids[] = { "NPXS40047", "NPXS40037", "NPXS40053", "NPXS40054", "NPXS40032", NULL };

    sqlite3_exec(app_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
    if (appinfo_db) sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);

    /* 1. Move Homebrew to Media Tab */
    char tid_list[256][16];
    int tid_count = 0;
    if (sqlite3_prepare_v2(app_db, "SELECT titleId FROM tbl_contentinfo;", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && tid_count < 256) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) {
                strncpy(tid_list[tid_count], t, 15);
                tid_list[tid_count][15] = '\0';
                tid_count++;
            }
        }
        sqlite3_finalize(stmt);
    }

    for (int i = 0; i < tid_count; i++) {
        for (int p = 0; homebrew_prefixes[p] != NULL; p++) {
            if (strncmp(tid_list[i], homebrew_prefixes[p], strlen(homebrew_prefixes[p])) == 0) {
                internal_shift_single(app_db, appinfo_db, uids, uid_count, tid_list[i], DISP_LOCATION_MEDIA);
                break;
            }
        }
    }

    /* 2. Persistently Hide Stock PSN Bloatware */
    for (int b = 0; bloat_ids[b] != NULL; b++) {
        internal_set_visibility(app_db, appinfo_db, uids, uid_count, bloat_ids[b], true);
    }

    if (appinfo_db) sqlite3_exec(appinfo_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);
    return SQLITE_OK;
}

static int execute_safe_layout_reset(sqlite3 *app_db, sqlite3 *appinfo_db, char uids[MAX_USER_PROFILES][32], int uid_count) {
    sqlite3_stmt *stmt;
    char tid_list[512][16];
    int tid_count = 0;

    sqlite3_exec(app_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
    if (appinfo_db) sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);

    const char *sql = "SELECT titleId FROM tbl_contentinfo WHERE titleId NOT LIKE 'NPXS%' AND titleId != 'TBSH00001';";
    if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && tid_count < 512) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) {
                strncpy(tid_list[tid_count], t, 15);
                tid_list[tid_count][15] = '\0';
                tid_count++;
            }
        }
        sqlite3_finalize(stmt);
    }

    for (int i = 0; i < tid_count; i++) {
        internal_shift_single(app_db, appinfo_db, uids, uid_count, tid_list[i], DISP_LOCATION_GAMES);
    }

    const char *stock_apps[] = { "NPXS40047", "NPXS40037", "NPXS40053", "NPXS40054", "NPXS40056", "NPXS40071", "NPXS40075", "NPXS40139", NULL };
    for (int s = 0; stock_apps[s] != NULL; s++) {
        internal_set_visibility(app_db, appinfo_db, uids, uid_count, stock_apps[s], false);
    }

    if (appinfo_db) sqlite3_exec(appinfo_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);
    return SQLITE_OK;
}

/* ========================================================================= */
/*          EMBEDDED WEB UI (NO SCROLL-JUMP, SEARCH, BATCH, UPDATER)         */
/* ========================================================================= */

static const char *WEB_UI_HTML =
"<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>PS5-TabShifter v1.3.0</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0b0e14;color:#e6edf3;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;padding-bottom:90px}"
"#update-banner{display:none;background:linear-gradient(90deg,#11294d,#1a3d6e);border-bottom:1px solid #2e62a6;padding:12px 20px}"
".ub-row{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;max-width:1100px;margin:0 auto}"
".ub-actions{display:flex;gap:8px}"
"#changelog-drawer{display:none;max-width:1100px;margin:10px auto 0;background:#081221;border:1px solid #234b82;border-radius:6px;padding:12px;font-size:13px;line-height:1.5;max-height:220px;overflow-y:auto;white-space:pre-wrap;color:#b8d0eb}"
".container{max-width:1100px;margin:0 auto;padding:20px}"
"header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;margin-bottom:20px;padding-bottom:15px;border-bottom:1px solid #21262d}"
"h1{font-size:22px;font-weight:700;color:#fff;display:flex;align-items:center;gap:10px}"
".badge{font-size:12px;background:#1f6feb;color:#fff;padding:3px 8px;border-radius:12px}"
".top-controls{display:flex;gap:8px;flex-wrap:wrap}"
".btn{background:#21262d;color:#c9d1d9;border:1px solid #30363d;padding:7px 13px;border-radius:6px;cursor:pointer;font-size:13px;font-weight:600;transition:0.15s}"
".btn:hover{background:#30363d;color:#fff}"
".btn-primary{background:#0070d1;color:#fff;border-color:#0070d1}"
".btn-primary:hover{background:#0084f5}"
".btn-danger{background:#8b1e24;color:#fff;border-color:#a8242b}"
".toolbar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px;align-items:center}"
"#search-input{flex:1;min-width:220px;background:#161b22;border:1px solid #30363d;color:#fff;padding:9px 14px;border-radius:6px;font-size:14px}"
".pills{display:flex;gap:6px;flex-wrap:wrap}"
".pill{background:#161b22;border:1px solid #30363d;color:#8b949e;padding:6px 12px;border-radius:16px;cursor:pointer;font-size:12px;font-weight:600}"
".pill.active{background:#1f6feb;color:#fff;border-color:#1f6feb}"
".app-list{display:flex;flex-direction:column;gap:8px}"
".app-card{display:flex;align-items:center;justify-content:space-between;background:#161b22;border:1px solid #21262d;border-radius:8px;padding:12px 16px;gap:12px;transition:0.15s}"
".app-card:hover{border-color:#388bfd}"
".app-left{display:flex;align-items:center;gap:12px;min-width:0;flex:1}"
".app-chk{width:18px;height:18px;cursor:pointer}"
".app-info{min-width:0;flex:1}"
".app-title-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}"
".app-title{font-weight:600;font-size:15px;color:#f0f6fc;word-break:break-word}"
".app-tid{font-family:monospace;font-size:12px;color:#8b949e}"
".loc-tag{font-size:11px;padding:2px 7px;border-radius:4px;font-weight:700}"
".loc-138{background:#133929;color:#3fb950}"
".loc-188{background:#3b234a;color:#d2a8ff}"
".loc-1{background:#3d1c20;color:#f85149}"
".pin-tag{font-size:11px;background:#3d2e0f;color:#e3b341;padding:2px 7px;border-radius:4px;font-weight:700}"
".app-actions{display:flex;gap:6px;flex-wrap:wrap}"
"#batch-bar{position:fixed;bottom:0;left:0;right:0;background:#161b22;border-top:1px solid #30363d;padding:12px 20px;display:none;z-index:100;box-shadow:0 -4px 16px rgba(0,0,0,0.5)}"
".batch-inner{max-width:1100px;margin:0 auto;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px}"
"#toast{position:fixed;bottom:75px;right:20px;background:#1f6feb;color:#fff;padding:10px 16px;border-radius:6px;font-size:13px;font-weight:600;display:none;z-index:200}"
"</style></head><body>"
"<div id='update-banner'>"
"  <div class='ub-row'>"
"    <div><strong>Update Available:</strong> <span id='ub-ver'></span></div>"
"    <div class='ub-actions'>"
"      <button type='button' class='btn' onclick='toggleChangelog()'>View Changelog &#9662;</button>"
"      <button type='button' class='btn btn-primary' id='ub-install-btn' onclick='runSelfUpdate()'>Update Now</button>"
"    </div>"
"  </div>"
"  <div id='changelog-drawer'></div>"
"</div>"
"<div class='container'>"
"  <header>"
"    <h1>PS5-TabShifter <span class='badge' id='cur-ver'>v1.3.0</span></h1>"
"    <div class='top-controls'>"
"      <button type='button' class='btn btn-primary' onclick='smartTidy()'>Smart Tidy (Auto-Sort)</button>"
"      <button type='button' class='btn btn-danger' onclick='resetLayout()'>Reset Layout to Stock</button>"
"    </div>"
"  </header>"
"  <div class='toolbar'>"
"    <input type='text' id='search-input' placeholder='Search by title or Title ID (e.g., PPSA, CUSA, NPXS)...' oninput='renderList()'>"
"    <div class='pills'>"
"      <button type='button' class='pill active' onclick='setFilter(\"all\",this)'>All</button>"
"      <button type='button' class='pill' onclick='setFilter(\"games\",this)'>Games Tab</button>"
"      <button type='button' class='pill' onclick='setFilter(\"media\",this)'>Media Tab</button>"
"      <button type='button' class='pill' onclick='setFilter(\"hidden\",this)'>Hidden / System</button>"
"      <button type='button' class='pill' onclick='setFilter(\"ps5\",this)'>PS5</button>"
"      <button type='button' class='pill' onclick='setFilter(\"ps4\",this)'>PS4</button>"
"      <button type='button' class='pill' onclick='setFilter(\"hb\",this)'>Homebrew</button>"
"    </div>"
"  </div>"
"  <div class='app-list' id='app-list'></div>"
"</div>"
"<div id='batch-bar'>"
"  <div class='batch-inner'>"
"    <div><strong id='sel-count'>0</strong> items selected</div>"
"    <div class='app-actions'>"
"      <button type='button' class='btn btn-primary' onclick='batchExec(\"media\")'>Move to Media</button>"
"      <button type='button' class='btn btn-primary' onclick='batchExec(\"games\")'>Move to Games</button>"
"      <button type='button' class='btn' onclick='batchExec(\"pin\")'>Pin</button>"
"      <button type='button' class='btn' onclick='batchExec(\"unpin\")'>Unpin</button>"
"      <button type='button' class='btn btn-danger' onclick='batchExec(\"hide\")'>Hide</button>"
"      <button type='button' class='btn' onclick='batchExec(\"unhide\")'>Unhide</button>"
"      <button type='button' class='btn' onclick='clearSelection()'>Clear</button>"
"    </div>"
"  </div>"
"</div>"
"<div id='toast'></div>"
"<script>"
"let APPS=[];let FILTER='all';let SELECTED=new Set();let LATEST_ELF_URL='';"
"function showToast(m){const t=document.getElementById('toast');t.textContent=m;t.style.display='block';setTimeout(()=>t.style.display='none',2600);}"
"async function loadApps(){"
"  const res=await fetch('/api/apps');"
"  APPS=await res.json();"
"  renderList();"
"}"
"function isHomebrew(tid){return /^(PLDM|PMNT|FMGR|WKAL|FAKE|ITEM|LAPY|PPSA99)/.test(tid);}"
"function setFilter(f,el){FILTER=f;document.querySelectorAll('.pill').forEach(p=>p.classList.remove('active'));el.classList.add('active');renderList();}"
"function locLabel(loc){if(loc===138)return '<span class=\"loc-tag loc-138\">GAMES</span>';if(loc===188)return '<span class=\"loc-tag loc-188\">MEDIA</span>';return '<span class=\"loc-tag loc-1\">HIDDEN ('+loc+')</span>';}"
"function renderList(){"
"  const scrollY=window.scrollY;"
"  const q=document.getElementById('search-input').value.toLowerCase();"
"  const container=document.getElementById('app-list');"
"  container.innerHTML=APPS.filter(a=>{"
"    if(q && !a.title.toLowerCase().includes(q) && !a.tid.toLowerCase().includes(q))return false;"
"    if(FILTER==='games')return a.loc===138;"
"    if(FILTER==='media')return a.loc===188;"
"    if(FILTER==='hidden')return a.loc!==138 && a.loc!==188;"
"    if(FILTER==='ps5')return a.tid.startsWith('PPSA') && !isHomebrew(a.tid);"
"    if(FILTER==='ps4')return a.tid.startsWith('CUSA');"
"    if(FILTER==='hb')return isHomebrew(a.tid);"
"    return true;"
"  }).map(a=>{"
"    const chk=SELECTED.has(a.tid)?'checked':'';"
"    const pinBadge=a.pinned===1?'<span class=\"pin-tag\">PINNED</span>':'';"
"    const nextTab=a.loc===188?'games':'media';"
"    const nextTabTxt=a.loc===188?'To Games':'To Media';"
"    const hideAct=(a.loc===1||a.visible===0)?'unhide':'hide';"
"    const hideTxt=(a.loc===1||a.visible===0)?'Unhide':'Hide';"
"    const pinAct=a.pinned===1?'unpin':'pin';"
"    const pinTxt=a.pinned===1?'Unpin':'Pin';"
"    return `<div class='app-card' id='card-${a.tid}'>"
"      <div class='app-left'>"
"        <input type='checkbox' class='app-chk' ${chk} onchange='toggleSel(\"${a.tid}\",this.checked)'>"
"        <div class='app-info'>"
"          <div class='app-title-row'>"
"            <span class='app-title' id='title-${a.tid}'>${a.title}</span>"
"            ${locLabel(a.loc)} ${pinBadge}"
"          </div>"
"          <div class='app-tid'>${a.tid}</div>"
"        </div>"
"      </div>"
"      <div class='app-actions'>"
"        <button type='button' class='btn btn-primary' onclick='singleAct(\"${a.tid}\",\"${nextTab}\")'>${nextTabTxt}</button>"
"        <button type='button' class='btn' onclick='singleAct(\"${a.tid}\",\"${pinAct}\")'>${pinTxt}</button>"
"        <button type='button' class='btn' onclick='singleAct(\"${a.tid}\",\"${hideAct}\")'>${hideTxt}</button>"
"        <button type='button' class='btn' onclick='promptRename(\"${a.tid}\")'>Rename</button>"
"        <button type='button' class='btn' onclick='singleAct(\"${a.tid}\",\"refresh-icon\")'>Fix Icon</button>"
"      </div>"
"    </div>`;"
"  }).join('');"
"  window.scrollTo(0,scrollY);"
"}"
"function toggleSel(tid,checked){if(checked)SELECTED.add(tid);else SELECTED.delete(tid);updateBatchBar();}"
"function clearSelection(){SELECTED.clear();updateBatchBar();renderList();}"
"function updateBatchBar(){const b=document.getElementById('batch-bar');document.getElementById('sel-count').textContent=SELECTED.size;b.style.display=SELECTED.size>0?'block':'none';}"
"async function singleAct(tid,action){"
"  const res=await fetch('/api/batch',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,tids:[tid]})});"
"  if(res.ok){"
"    const item=APPS.find(x=>x.tid===tid);"
"    if(item){"
"      if(action==='media'){item.loc=188;item.visible=1;}"
"      else if(action==='games'){item.loc=138;item.visible=1;}"
"      else if(action==='hide'){item.loc=1;item.visible=0;item.pinned=2;}"
"      else if(action==='unhide'){item.loc=138;item.visible=1;}"
"      else if(action==='pin'){item.pinned=1;}"
"      else if(action==='unpin'){item.pinned=2;}"
"    }"
"    renderList();"
"    showToast(`${tid}: ${action.toUpperCase()} applied`);"
"  }"
"}"
"async function batchExec(action){"
"  const tids=Array.from(SELECTED);"
"  const res=await fetch('/api/batch',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,tids})});"
"  if(res.ok){clearSelection();await loadApps();showToast(`Batch ${action} applied to ${tids.length} items`);}"
"}"
"async function promptRename(tid){"
"  const item=APPS.find(x=>x.tid===tid);"
"  const nw=prompt('Enter new display name:',item?item.title:'');"
"  if(nw && nw.trim().length>0){"
"    const res=await fetch('/api/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tid,name:nw.trim()})});"
"    if(res.ok){if(item)item.title=nw.trim();renderList();showToast(`Renamed ${tid}`);}"
"  }"
"}"
"async function smartTidy(){if(confirm('Auto-sort all Homebrew to Media Tab and hide PSN bloatware?')){await fetch('/api/smart-tidy',{method:'POST'});await loadApps();showToast('Smart Tidy complete!');}}"
"async function resetLayout(){if(confirm('Reset all shifted apps back to the Games tab and restore stock layout? (Installed games will NOT be deleted)')){await fetch('/api/reset-layout',{method:'POST'});await loadApps();showToast('Stock layout restored!');}}"
"function toggleChangelog(){const d=document.getElementById('changelog-drawer');d.style.display=d.style.display==='block'?'none':'block';}"
"async function checkGitHubUpdate(){"
"  try{"
"    const vRes=await fetch('/api/version').then(r=>r.json());"
"    const gh=await fetch('https://api.github.com/repos/xvvulcanvx/PS5-TabShifter/releases/latest').then(r=>r.json());"
"    const latest=gh.tag_name.replace(/^v/,'');"
"    if(latest && latest!==vRes.version){"
"      const asset=(gh.assets||[]).find(a=>a.name.endsWith('.elf'));"
"      if(asset){"
"        LATEST_ELF_URL=asset.browser_download_url;"
"        document.getElementById('ub-ver').textContent=gh.tag_name;"
"        document.getElementById('changelog-drawer').textContent=gh.body||'No release notes provided.';"
"        document.getElementById('update-banner').style.display='block';"
"      }"
"    }"
"  }catch(e){}"
"}"
"async function runSelfUpdate(){"
"  if(!LATEST_ELF_URL)return;"
"  const btn=document.getElementById('ub-install-btn');btn.textContent='Downloading...';btn.disabled=true;"
"  try{"
"    const blob=await fetch(LATEST_ELF_URL).then(r=>r.blob());"
"    btn.textContent='Installing...';"
"    const up=await fetch('/api/update-payload',{method:'POST',body:blob});"
"    if(up.ok){showToast('Update installed! Reload payload on next launch.');btn.textContent='Updated!';}"
"    else{btn.textContent='Update Failed';btn.disabled=false;}"
"  }catch(e){btn.textContent='Error';btn.disabled=false;}"
"}"
"loadApps();checkGitHubUpdate();"
"</script></body></html>";

/* ========================================================================= */
/*                    MINIMAL JSON & HTTP SERVER HANDLERS                    */
/* ========================================================================= */

static void json_escape_str(const char *in, char *out, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] != '\0' && j + 2 < max_len; i++) {
        if (in[i] == '\"' || in[i] == '\\') out[j++] = '\\';
        if ((unsigned char)in[i] >= 0x20) out[j++] = in[i];
    }
    out[j] = '\0';
}

static char *build_apps_json(sqlite3 *app_db, const char *primary_uid) {
    size_t cap = 131072;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    size_t len = (size_t)sprintf(buf, "[");
    sqlite3_stmt *stmt;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT c.titleId, coalesce(c.titleName, i.titleName, c.titleId), "
             "coalesce(i.dispLocation, c.dispLocation, 138), coalesce(i.pinnedStatus, 2), coalesce(i.visible, 1) "
             "FROM tbl_contentinfo c LEFT JOIN tbl_iconinfo_%s i ON c.titleId = i.titleId "
             "ORDER BY c.titleName COLLATE NOCASE ASC;", primary_uid);

    if (sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *tid   = (const char *)sqlite3_column_text(stmt, 0);
            const char *tname = (const char *)sqlite3_column_text(stmt, 1);
            int loc           = sqlite3_column_int(stmt, 2);
            int pinned        = sqlite3_column_int(stmt, 3);
            int vis           = sqlite3_column_int(stmt, 4);

            if (!tid) continue;
            char esc_name[256];
            json_escape_str(tname ? tname : tid, esc_name, sizeof(esc_name));

            if (len + 512 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            len += (size_t)snprintf(buf + len, cap - len,
                                    "%s{\"tid\":\"%s\",\"title\":\"%s\",\"loc\":%d,\"pinned\":%d,\"visible\":%d}",
                                    first ? "" : ",", tid, esc_name, loc, pinned, vis);
            first = false;
        }
        sqlite3_finalize(stmt);
    }
    snprintf(buf + len, cap - len, "]");
    return buf;
}

static void handle_batch_json(sqlite3 *app_db, sqlite3 *appinfo_db,
                              char uids[MAX_USER_PROFILES][32], int uid_count,
                              const char *body) {
    if (!body) return;
    char action[32] = {0};
    const char *act_ptr = strstr(body, "\"action\":\"");
    if (act_ptr) {
        act_ptr += 10;
        size_t i = 0;
        while (act_ptr[i] && act_ptr[i] != '\"' && i < sizeof(action) - 1) {
            action[i] = act_ptr[i];
            i++;
        }
    }

    const char *tids_arr = strstr(body, "\"tids\":[");
    if (!tids_arr || action[0] == '\0') return;

    sqlite3_exec(app_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
    if (appinfo_db) sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);

    const char *p = tids_arr + 8;
    while (*p && *p != ']') {
        if (*p == '\"') {
            p++;
            char tid[32] = {0};
            size_t i = 0;
            while (*p && *p != '\"' && i < sizeof(tid) - 1) tid[i++] = *p++;
            if (*p == '\"') p++;

            if (tid[0] != '\0') {
                if (strcmp(action, "media") == 0)       internal_shift_single(app_db, appinfo_db, uids, uid_count, tid, DISP_LOCATION_MEDIA);
                else if (strcmp(action, "games") == 0)  internal_shift_single(app_db, appinfo_db, uids, uid_count, tid, DISP_LOCATION_GAMES);
                else if (strcmp(action, "hide") == 0)   internal_set_visibility(app_db, appinfo_db, uids, uid_count, tid, true);
                else if (strcmp(action, "unhide") == 0) internal_set_visibility(app_db, appinfo_db, uids, uid_count, tid, false);
                else if (strcmp(action, "pin") == 0)    internal_set_pin(app_db, uids, uid_count, tid, true);
                else if (strcmp(action, "unpin") == 0)  internal_set_pin(app_db, uids, uid_count, tid, false);
                else if (strcmp(action, "refresh-icon") == 0) internal_refresh_icon(app_db, uids, uid_count, tid);
            }
        } else {
            p++;
        }
    }

    if (appinfo_db) sqlite3_exec(appinfo_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);
}

static void handle_rename_json(sqlite3 *app_db, sqlite3 *appinfo_db,
                               char uids[MAX_USER_PROFILES][32], int uid_count,
                               const char *body) {
    if (!body) return;
    char tid[32] = {0};
    char name[256] = {0};

    const char *tp = strstr(body, "\"tid\":\"");
    if (tp) {
        tp += 7;
        for (size_t i = 0; tp[i] && tp[i] != '\"' && i < sizeof(tid) - 1; i++) tid[i] = tp[i];
    }
    const char *np = strstr(body, "\"name\":\"");
    if (np) {
        np += 8;
        for (size_t i = 0; np[i] && np[i] != '\"' && i < sizeof(name) - 1; i++) name[i] = np[i];
    }

    if (tid[0] != '\0' && name[0] != '\0') {
        sqlite3_exec(app_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
        if (appinfo_db) sqlite3_exec(appinfo_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
        internal_rename_title(app_db, appinfo_db, uids, uid_count, tid, name);
        if (appinfo_db) sqlite3_exec(appinfo_db, "COMMIT;", NULL, NULL, NULL);
        sqlite3_exec(app_db, "COMMIT;", NULL, NULL, NULL);
    }
}

static void send_http_response(int client_fd, int status, const char *content_type, const void *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d OK\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n",
                        status, content_type, body_len);
    send(client_fd, header, (size_t)hlen, 0);
    if (body && body_len > 0) send(client_fd, body, body_len, 0);
}

static void handle_http_client(int client_fd, sqlite3 *app_db, sqlite3 *appinfo_db,
                               char uids[MAX_USER_PROFILES][32], int uid_count) {
    size_t cap = 65536;
    uint8_t *req = malloc(cap);
    if (!req) { close(client_fd); return; }

    ssize_t received = recv(client_fd, req, cap - 1, 0);
    if (received <= 0) { free(req); close(client_fd); return; }
    req[received] = '\0';

    /* Parse Content-Length for binary POSTs (e.g., /api/update-payload) */
    size_t content_len = 0;
    const char *cl_hdr = strstr((char *)req, "Content-Length:");
    if (!cl_hdr) cl_hdr = strstr((char *)req, "content-length:");
    if (cl_hdr) content_len = (size_t)strtoul(cl_hdr + 15, NULL, 10);

    uint8_t *body_ptr = (uint8_t *)strstr((char *)req, "\r\n\r\n");
    if (body_ptr) {
        body_ptr += 4;
        size_t header_bytes = (size_t)(body_ptr - req);
        size_t total_needed = header_bytes + content_len;
        if (total_needed > cap && total_needed < 32 * 1024 * 1024) {
            req = realloc(req, total_needed + 1);
            body_ptr = req + header_bytes;
        }
        while ((size_t)received < total_needed) {
            ssize_t r = recv(client_fd, req + received, total_needed - (size_t)received, 0);
            if (r <= 0) break;
            received += r;
        }
        req[received] = '\0';
    }

    if (strncmp((char *)req, "GET / ", 6) == 0) {
        send_http_response(client_fd, 200, "text/html; charset=utf-8", WEB_UI_HTML, strlen(WEB_UI_HTML));
    } else if (strncmp((char *)req, "GET /api/version", 16) == 0) {
        char elf_path[512];
        char vjson[640];
        get_running_elf_path(elf_path, sizeof(elf_path));
        int vlen = snprintf(vjson, sizeof(vjson), "{\"version\":\"%s\",\"elf_path\":\"%s\"}", TABSHIFTER_VERSION, elf_path);
        send_http_response(client_fd, 200, "application/json", vjson, (size_t)vlen);
    } else if (strncmp((char *)req, "GET /api/apps", 13) == 0) {
        char *ajson = build_apps_json(app_db, uids[0]);
        if (ajson) {
            send_http_response(client_fd, 200, "application/json", ajson, strlen(ajson));
            free(ajson);
        }
    } else if (strncmp((char *)req, "POST /api/batch", 15) == 0 && body_ptr) {
        handle_batch_json(app_db, appinfo_db, uids, uid_count, (const char *)body_ptr);
        send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strncmp((char *)req, "POST /api/rename", 16) == 0 && body_ptr) {
        handle_rename_json(app_db, appinfo_db, uids, uid_count, (const char *)body_ptr);
        send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strncmp((char *)req, "POST /api/smart-tidy", 20) == 0) {
        execute_smart_tidy(app_db, appinfo_db, uids, uid_count);
        send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strncmp((char *)req, "POST /api/reset-layout", 22) == 0) {
        execute_safe_layout_reset(app_db, appinfo_db, uids, uid_count);
        send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
    } else if (strncmp((char *)req, "POST /api/update-payload", 24) == 0 && body_ptr && content_len > 0) {
        int rc = apply_self_update(body_ptr, content_len);
        if (rc == 0) send_http_response(client_fd, 200, "application/json", "{\"ok\":true}", 11);
        else send_http_response(client_fd, 500, "application/json", "{\"ok\":false}", 12);
    } else {
        send_http_response(client_fd, 404, "text/plain", "Not Found", 9);
    }

    free(req);
    close(client_fd);
}

/* ========================================================================= */
/*                                MAIN ENTRY                                 */
/* ========================================================================= */

int main(void) {
    sqlite3 *app_db = NULL;
    sqlite3 *appinfo_db = NULL;
    char uids[MAX_USER_PROFILES][32];

    if (sqlite3_open(APP_DB_PATH, &app_db) != SQLITE_OK) return 1;
    sqlite3_busy_timeout(app_db, SQLITE_BUSY_TIMEOUT_MS);

    if (sqlite3_open(APPINFO_DB_PATH, &appinfo_db) == SQLITE_OK) {
        sqlite3_busy_timeout(appinfo_db, SQLITE_BUSY_TIMEOUT_MS);
    } else {
        appinfo_db = NULL;
    }

    int uid_count = get_all_user_ids(app_db, uids);
    if (uid_count <= 0) {
        if (appinfo_db) sqlite3_close(appinfo_db);
        sqlite3_close(app_db);
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server_fd, 8) < 0) {
        close(server_fd);
        return 1;
    }

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            handle_http_client(client_fd, app_db, appinfo_db, uids, uid_count);
        }
    }

    close(server_fd);
    if (appinfo_db) sqlite3_close(appinfo_db);
    sqlite3_close(app_db);
    return 0;
}
