#include "tool_rss.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "tool_rss";

#define RSS_BUF_SIZE    (8 * 1024)
#define RSS_MAX_ITEMS   10
#define RSS_FIELD_MAX   256
#define RSS_SUMMARY_MAX 200

/* ── HTTP response buffer ──────────────────────────────────── */

typedef struct {
    char *data;
    int   len;
    int   cap;
} rss_buf_t;

static esp_err_t rss_http_event_cb(esp_http_client_event_t *evt)
{
    rss_buf_t *r = (rss_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (r->len + copy > r->cap - 1) copy = r->cap - 1 - r->len;
        if (copy > 0) {
            memcpy(r->data + r->len, evt->data, copy);
            r->len += copy;
            r->data[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── XML field extractor helpers ───────────────────────────── */

/*
 * Extract the text content of the first occurrence of <tag>...</tag>
 * or <tag attr...>...</tag> starting at *pos. Returns length written.
 * Sets *pos to just after the closing tag on success, or 0 if not found.
 */
static int xml_extract_tag(const char *xml, size_t xml_len,
                            const char *tag, size_t *pos,
                            char *out, size_t out_size)
{
    char open_tag[64];
    char close_tag[64];
    snprintf(open_tag,  sizeof(open_tag),  "<%s", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    /* Find opening tag from *pos */
    const char *start = (const char *)memmem(xml + *pos, xml_len - *pos,
                                              open_tag, strlen(open_tag));
    if (!start) return 0;

    /* Skip to end of opening tag (past '>') */
    const char *gt = memchr(start, '>', xml_len - (start - xml));
    if (!gt) return 0;
    const char *content = gt + 1;

    /* Find closing tag */
    const char *end = (const char *)memmem(content, xml_len - (content - xml),
                                            close_tag, strlen(close_tag));
    if (!end) return 0;

    size_t len = (size_t)(end - content);
    if (len >= out_size) len = out_size - 1;

    memcpy(out, content, len);
    out[len] = '\0';

    /* Advance pos past the closing tag */
    *pos = (size_t)(end - xml) + strlen(close_tag);
    return (int)len;
}

/* Strip CDATA wrapper: <![CDATA[...]]> → ... */
static void strip_cdata(char *s)
{
    const char *cdata_open  = "<![CDATA[";
    const char *cdata_close = "]]>";
    char *start = strstr(s, cdata_open);
    if (!start) return;
    char *end = strstr(start, cdata_close);
    if (!end) return;
    char *content = start + strlen(cdata_open);
    size_t len = (size_t)(end - content);
    memmove(s, content, len);
    s[len] = '\0';
}

/* Decode common HTML entities in place */
static void decode_html_entities(char *s)
{
    static const struct { const char *ent; char ch; } map[] = {
        {"&amp;",  '&'}, {"&lt;",  '<'}, {"&gt;",  '>'},
        {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''},
        {NULL, 0}
    };
    for (int i = 0; map[i].ent; i++) {
        size_t el = strlen(map[i].ent);
        char *p;
        while ((p = strstr(s, map[i].ent)) != NULL) {
            *p = map[i].ch;
            memmove(p + 1, p + el, strlen(p + el) + 1);
        }
    }
}

/* Truncate to max_len bytes, breaking at last space if possible */
static void truncate_str(char *s, size_t max_len)
{
    if (strlen(s) <= max_len) return;
    s[max_len] = '\0';
    char *sp = strrchr(s, ' ');
    if (sp && (size_t)(sp - s) > max_len / 2) *sp = '\0';
}

/* JSON-escape a string and append to output buffer */
static int json_escape_append(char *out, int pos, int cap, const char *s)
{
    if (pos >= cap - 1) return pos;
    out[pos++] = '"';
    for (; *s && pos < cap - 3; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') {
            out[pos++] = '\\'; out[pos++] = '"';
        } else if (c == '\\') {
            out[pos++] = '\\'; out[pos++] = '\\';
        } else if (c == '\n') {
            out[pos++] = '\\'; out[pos++] = 'n';
        } else if (c == '\r') {
            out[pos++] = '\\'; out[pos++] = 'r';
        } else if (c < 0x20) {
            /* skip other control chars */
        } else {
            out[pos++] = (char)c;
        }
    }
    if (pos < cap - 1) out[pos++] = '"';
    out[pos] = '\0';
    return pos;
}

/* ── RSS/Atom parser ──────────────────────────────────────── */

typedef struct {
    char title[RSS_FIELD_MAX];
    char link[RSS_FIELD_MAX];
    char date[64];
    char summary[RSS_SUMMARY_MAX];
} rss_item_t;

/*
 * Parse RSS 2.0 <item> or Atom <entry> blocks.
 * Returns number of items parsed.
 */
static int parse_feed(const char *xml, size_t xml_len,
                      rss_item_t *items, int max_items)
{
    int count = 0;
    size_t pos = 0;

    /* Detect feed type */
    bool is_atom = (memmem(xml, xml_len, "<entry>", 7) != NULL ||
                    memmem(xml, xml_len, "<entry ", 6) != NULL);
    const char *item_tag   = is_atom ? "entry" : "item";
    const char *title_tag  = "title";
    const char *link_tag   = is_atom ? "link" : "link";
    const char *date_tag   = is_atom ? "updated" : "pubDate";
    const char *summ_tag   = is_atom ? "summary" : "description";

    while (count < max_items) {
        /* Find next <item> or <entry> */
        char open[16];
        snprintf(open, sizeof(open), "<%s", item_tag);
        const char *item_start = (const char *)memmem(xml + pos, xml_len - pos,
                                                       open, strlen(open));
        if (!item_start) break;

        /* Find matching closing tag */
        char close[16];
        snprintf(close, sizeof(close), "</%s>", item_tag);
        const char *item_end = (const char *)memmem(item_start, xml_len - (item_start - xml),
                                                      close, strlen(close));
        if (!item_end) break;

        size_t item_len = (size_t)(item_end - item_start) + strlen(close);
        size_t ipos = (size_t)(item_start - xml);

        rss_item_t *it = &items[count];
        memset(it, 0, sizeof(*it));

        /* Extract title */
        size_t tpos = ipos;
        xml_extract_tag(xml, xml_len, title_tag, &tpos, it->title, sizeof(it->title));
        strip_cdata(it->title);
        decode_html_entities(it->title);

        /* Extract link — for Atom, it's <link href="..."/> not <link>...</link> */
        if (is_atom) {
            const char *lstart = (const char *)memmem(xml + ipos, item_len,
                                                        "<link", 5);
            if (lstart) {
                const char *href = strstr(lstart, "href=\"");
                if (href) {
                    href += 6;
                    const char *hend = strchr(href, '"');
                    if (hend) {
                        size_t hlen = (size_t)(hend - href);
                        if (hlen >= sizeof(it->link)) hlen = sizeof(it->link) - 1;
                        memcpy(it->link, href, hlen);
                        it->link[hlen] = '\0';
                    }
                }
            }
        } else {
            size_t lpos = ipos;
            xml_extract_tag(xml, xml_len, link_tag, &lpos, it->link, sizeof(it->link));
        }

        /* Extract date */
        size_t dpos = ipos;
        xml_extract_tag(xml, xml_len, date_tag, &dpos, it->date, sizeof(it->date));

        /* Extract summary/description */
        size_t spos = ipos;
        xml_extract_tag(xml, xml_len, summ_tag, &spos, it->summary, sizeof(it->summary));
        strip_cdata(it->summary);
        /* Remove HTML tags from summary */
        char *p = it->summary;
        while ((p = strchr(p, '<')) != NULL) {
            char *q = strchr(p, '>');
            if (!q) break;
            memmove(p, q + 1, strlen(q + 1) + 1);
        }
        decode_html_entities(it->summary);
        truncate_str(it->summary, RSS_SUMMARY_MAX - 1);

        /* Advance past this item */
        pos = (size_t)(item_start - xml) + item_len;
        count++;
    }

    return count;
}

/* ── Public execute ───────────────────────────────────────── */

esp_err_t tool_rss_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_url   = cJSON_GetObjectItem(input, "url");
    cJSON *j_max   = cJSON_GetObjectItem(input, "max_items");

    if (!j_url || !cJSON_IsString(j_url)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'url' is required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *url = j_url->valuestring;
    int max_items = (j_max && cJSON_IsNumber(j_max)) ? j_max->valueint : 5;
    if (max_items < 1)  max_items = 1;
    if (max_items > RSS_MAX_ITEMS) max_items = RSS_MAX_ITEMS;
    cJSON_Delete(input);

    /* Allocate response buffer */
    char *buf = malloc(RSS_BUF_SIZE);
    if (!buf) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    buf[0] = '\0';

    rss_buf_t rb = { .data = buf, .len = 0, .cap = RSS_BUF_SIZE };

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = rss_http_event_cb,
        .user_data         = &rb,
        .buffer_size       = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        snprintf(output, output_size, "Error: HTTP client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(buf);
        snprintf(output, output_size, "Error: HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status != 200) {
        free(buf);
        snprintf(output, output_size, "Error: HTTP %d from feed URL", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "RSS: fetched %d bytes from %s", rb.len, url);

    /* Parse feed */
    rss_item_t items[RSS_MAX_ITEMS];
    int count = parse_feed(buf, (size_t)rb.len, items, max_items);
    free(buf);

    if (count == 0) {
        snprintf(output, output_size, "Error: no items found in feed (not RSS/Atom?)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "RSS: parsed %d items", count);

    /* Build JSON output */
    int pos = 0;
    int cap = (int)output_size;
    pos += snprintf(output + pos, cap - pos, "[");

    for (int i = 0; i < count && pos < cap - 2; i++) {
        if (i > 0 && pos < cap - 1) output[pos++] = ',';
        pos += snprintf(output + pos, cap - pos, "{\"title\":");
        pos  = json_escape_append(output, pos, cap, items[i].title);
        pos += snprintf(output + pos, cap - pos, ",\"link\":");
        pos  = json_escape_append(output, pos, cap, items[i].link);
        pos += snprintf(output + pos, cap - pos, ",\"date\":");
        pos  = json_escape_append(output, pos, cap, items[i].date);
        pos += snprintf(output + pos, cap - pos, ",\"summary\":");
        pos  = json_escape_append(output, pos, cap, items[i].summary);
        if (pos < cap - 1) output[pos++] = '}';
    }

    if (pos < cap - 1) { output[pos++] = ']'; output[pos] = '\0'; }
    else { output[cap - 2] = ']'; output[cap - 1] = '\0'; }

    return ESP_OK;
}
