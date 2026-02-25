#include "tool_search_files.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_search";

#define MAX_MATCHES  20
#define MAX_LINE_LEN 120

/* Case-insensitive substring search */
static bool icontains(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return false;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) break;
        }
        if (j == nlen) return true;
    }
    return false;
}

esp_err_t tool_search_files_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_pattern = cJSON_GetObjectItem(input, "pattern");
    cJSON *j_prefix  = cJSON_GetObjectItem(input, "prefix");

    if (!j_pattern || !cJSON_IsString(j_pattern) || !j_pattern->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'pattern' is required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *pattern = j_pattern->valuestring;

    /* Prefix filter: strip leading /spiffs/ to match SPIFFS readdir names */
    const char *prefix_filter = NULL;
    char prefix_buf[64] = {0};
    if (j_prefix && cJSON_IsString(j_prefix) && j_prefix->valuestring[0]) {
        const char *p = j_prefix->valuestring;
        /* readdir names start after /spiffs/ mount point */
        if (strncmp(p, MIMI_SPIFFS_BASE "/", strlen(MIMI_SPIFFS_BASE) + 1) == 0) {
            p += strlen(MIMI_SPIFFS_BASE) + 1;
        } else if (p[0] == '/') {
            p++;  /* strip leading slash if no mount prefix */
        }
        strncpy(prefix_buf, p, sizeof(prefix_buf) - 1);
        prefix_filter = prefix_buf;
    }

    cJSON_Delete(input);

    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        snprintf(output, output_size, "Error: cannot open SPIFFS");
        return ESP_FAIL;
    }

    size_t out_pos  = 0;
    int    matches  = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && matches < MAX_MATCHES) {
        /* Skip console and sessions — too large / not useful to search */
        if (strncmp(ent->d_name, "console/", 8) == 0) continue;
        if (strncmp(ent->d_name, "sessions/", 9) == 0) continue;

        /* Apply prefix filter */
        if (prefix_filter && strncmp(ent->d_name, prefix_filter, strlen(prefix_filter)) != 0) {
            continue;
        }

        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, ent->d_name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char line[256];
        int  linenum = 0;

        while (fgets(line, sizeof(line), f) && matches < MAX_MATCHES) {
            linenum++;
            /* Trim trailing newline for display */
            int len = (int)strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
            line[len] = '\0';

            if (!icontains(line, pattern)) continue;

            /* Build display path as /spiffs/<name> — reuse full_path */
            const char *display_path = full_path;

            /* Truncate long lines */
            char trunc[MAX_LINE_LEN + 4];
            if (len > MAX_LINE_LEN) {
                snprintf(trunc, sizeof(trunc), "%.*s...", MAX_LINE_LEN, line);
            } else {
                strncpy(trunc, line, sizeof(trunc) - 1);
                trunc[sizeof(trunc)-1] = '\0';
            }

            int written = snprintf(output + out_pos, output_size - out_pos,
                                   "%s:%d: %s\n", display_path, linenum, trunc);
            if (written > 0 && out_pos + (size_t)written < output_size - 1) {
                out_pos += written;
                matches++;
            } else {
                break;
            }
        }
        fclose(f);
    }

    closedir(dir);

    if (matches == 0) {
        snprintf(output, output_size, "No matches found.");
    } else {
        output[out_pos] = '\0';
        if (matches == MAX_MATCHES) {
            /* Append truncation notice if there's room */
            size_t room = output_size - out_pos;
            if (room > 40) {
                snprintf(output + out_pos, room, "(results capped at %d matches)", MAX_MATCHES);
            }
        }
    }

    ESP_LOGI(TAG, "search '%s' → %d matches", pattern, matches);
    return ESP_OK;
}
