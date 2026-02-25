#include "tool_smtp.h"
#include "gateway/ws_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "tool_smtp";

#define SMTP_BUF     512
#define SMTP_TIMEOUT 15000

/* ── Base64 encoder ───────────────────────────────────────────── */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t pos = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        unsigned char b0 =              (unsigned char)in[i];
        unsigned char b1 = (i+1 < in_len) ? (unsigned char)in[i+1] : 0;
        unsigned char b2 = (i+2 < in_len) ? (unsigned char)in[i+2] : 0;
        if (pos + 4 >= out_size) return -1;
        out[pos++] = B64[b0 >> 2];
        out[pos++] = B64[((b0 & 3) << 4) | (b1 >> 4)];
        out[pos++] = (i+1 < in_len) ? B64[((b1 & 0xf) << 2) | (b2 >> 6)] : '=';
        out[pos++] = (i+2 < in_len) ? B64[b2 & 0x3f]                      : '=';
    }
    out[pos] = '\0';
    return (int)pos;
}

/* ── Parse ## Email section from SERVICES.md ──────────────────── */

typedef struct {
    char smtp_host[128];
    char username[128];
    char password[128];   /* app password — spaces stripped */
    char from_addr[128];
    char to_addr[128];
    int  port;
} smtp_creds_t;

static bool parse_email_creds(smtp_creds_t *c)
{
    memset(c, 0, sizeof(*c));
    c->port = 465;  /* Gmail SMTPS default */

    FILE *f = fopen("/spiffs/config/SERVICES.md", "r");
    if (!f) return false;

    char line[256];
    bool in_section = false;

    while (fgets(line, sizeof(line), f)) {
        /* Trim trailing whitespace */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '))
            line[--len] = '\0';

        if (strcmp(line, "## Email") == 0) { in_section = true;  continue; }
        if (in_section && len > 1 && line[0] == '#') break;
        if (!in_section || len == 0) continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *key = line;
        const char *val = colon + 1;
        while (*val == ' ') val++;

        if      (strcmp(key, "smtp_host")    == 0) strncpy(c->smtp_host,  val, sizeof(c->smtp_host)  - 1);
        else if (strcmp(key, "smtp_port")    == 0) c->port = atoi(val);
        else if (strcmp(key, "username")     == 0) strncpy(c->username,   val, sizeof(c->username)   - 1);
        else if (strcmp(key, "from_address") == 0) strncpy(c->from_addr,  val, sizeof(c->from_addr)  - 1);
        else if (strcmp(key, "to_address")   == 0) strncpy(c->to_addr,    val, sizeof(c->to_addr)    - 1);
        else if (strcmp(key, "password")     == 0) {
            /* Strip spaces from app password (Gmail shows it as "xxxx xxxx xxxx xxxx") */
            int p = 0;
            for (int i = 0; val[i] && p < (int)sizeof(c->password) - 1; i++) {
                if (val[i] != ' ') c->password[p++] = val[i];
            }
            c->password[p] = '\0';
        }
    }

    fclose(f);
    return (c->username[0] && c->password[0] && c->to_addr[0]);
}

/* ── SMTP helpers ─────────────────────────────────────────────── */

static int smtp_write(esp_tls_t *tls, const char *data)
{
    int len = (int)strlen(data);
    int written = 0;
    while (written < len) {
        int n = esp_tls_conn_write(tls, data + written, len - written);
        if (n <= 0) return -1;
        written += n;
    }
    return written;
}

/* Read lines until we get one without a '-' continuation character.
   Returns the final status code, or -1 on error. */
static int smtp_read_status(esp_tls_t *tls)
{
    char buf[SMTP_BUF];
    int  code = -1;

    while (1) {
        int pos = 0;
        /* Read one line */
        while (pos < SMTP_BUF - 1) {
            int n = esp_tls_conn_read(tls, buf + pos, 1);
            if (n <= 0) return -1;
            pos++;
            if (pos >= 2 && buf[pos-1] == '\n') break;
        }
        buf[pos] = '\0';

        if (pos < 3) return -1;
        code = atoi(buf);

        /* "250-..." means more lines follow; "250 ..." is the last */
        if (pos < 4 || buf[3] != '-') break;
    }
    return code;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_smtp_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_subject = cJSON_GetObjectItem(input, "subject");
    cJSON *j_body    = cJSON_GetObjectItem(input, "body");
    cJSON *j_to      = cJSON_GetObjectItem(input, "to");

    if (!j_subject || !cJSON_IsString(j_subject) ||
        !j_body    || !cJSON_IsString(j_body)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'subject' and 'body' are required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *subject = j_subject->valuestring;
    const char *body    = j_body->valuestring;

    /* Load credentials */
    smtp_creds_t creds;
    if (!parse_email_creds(&creds)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: SERVICES.md missing ## Email section with username/password/to_address. "
                 "Add smtp_host, smtp_port, username, password, from_address, to_address.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allow caller to override to_address */
    if (j_to && cJSON_IsString(j_to) && j_to->valuestring[0]) {
        strncpy(creds.to_addr, j_to->valuestring, sizeof(creds.to_addr) - 1);
    }

    /* Default smtp_host to gmail if not set */
    if (!creds.smtp_host[0]) strncpy(creds.smtp_host, "smtp.gmail.com", sizeof(creds.smtp_host) - 1);
    /* Default from_address to username if not set */
    if (!creds.from_addr[0]) strncpy(creds.from_addr, creds.username, sizeof(creds.from_addr) - 1);

    cJSON_Delete(input);

    ESP_LOGI(TAG, "Sending email to %s via %s:%d", creds.to_addr, creds.smtp_host, creds.port);
    ws_server_broadcast_monitor_verbose("email", "Connecting to SMTP...");

    /* TLS connect */
    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = SMTP_TIMEOUT,
        .non_block         = false,
    };
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        snprintf(output, output_size, "Error: esp_tls_init failed");
        return ESP_FAIL;
    }

    int ret = esp_tls_conn_new_sync(creds.smtp_host, (int)strlen(creds.smtp_host),
                                    creds.port, &tls_cfg, tls);
    if (ret != 1) {
        esp_tls_conn_destroy(tls);
        snprintf(output, output_size, "Error: TLS connect to %s:%d failed",
                 creds.smtp_host, creds.port);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    char cmd[512];
    char b64u[256], b64p[256];

    /* Base64-encode username and password */
    b64_encode(creds.username, strlen(creds.username), b64u, sizeof(b64u));
    b64_encode(creds.password, strlen(creds.password), b64p, sizeof(b64p));

#define SMTP_CHECK(expected, label) \
    do { \
        int code = smtp_read_status(tls); \
        if (code != (expected)) { \
            snprintf(output, output_size, "Error: SMTP " label " failed (got %d, want %d)", code, expected); \
            err = ESP_FAIL; goto smtp_done; \
        } \
    } while(0)

#define SMTP_SEND(fmt, ...) \
    do { \
        snprintf(cmd, sizeof(cmd), fmt "\r\n", ##__VA_ARGS__); \
        if (smtp_write(tls, cmd) < 0) { \
            snprintf(output, output_size, "Error: SMTP write failed"); \
            err = ESP_FAIL; goto smtp_done; \
        } \
    } while(0)

    SMTP_CHECK(220, "greeting");
    SMTP_SEND("EHLO [127.0.0.1]");
    SMTP_CHECK(250, "EHLO");
    SMTP_SEND("AUTH LOGIN");
    SMTP_CHECK(334, "AUTH LOGIN");
    SMTP_SEND("%s", b64u);
    SMTP_CHECK(334, "username");
    SMTP_SEND("%s", b64p);
    SMTP_CHECK(235, "password");
    SMTP_SEND("MAIL FROM:<%s>", creds.from_addr);
    SMTP_CHECK(250, "MAIL FROM");
    SMTP_SEND("RCPT TO:<%s>", creds.to_addr);
    SMTP_CHECK(250, "RCPT TO");
    SMTP_SEND("DATA");
    SMTP_CHECK(354, "DATA");

    /* Message headers + body */
    snprintf(cmd, sizeof(cmd),
             "From: %s\r\n"
             "To: %s\r\n"
             "Subject: %s\r\n"
             "MIME-Version: 1.0\r\n"
             "Content-Type: text/plain; charset=utf-8\r\n"
             "\r\n"
             "%s\r\n"
             ".\r\n",
             creds.from_addr, creds.to_addr, subject, body);

    if (smtp_write(tls, cmd) < 0) {
        snprintf(output, output_size, "Error: failed to write message body");
        err = ESP_FAIL; goto smtp_done;
    }

    SMTP_CHECK(250, "message accepted");
    SMTP_SEND("QUIT");
    /* 221 expected but ignore — connection will close */

    snprintf(output, output_size, "OK: email sent to %s", creds.to_addr);
    ESP_LOGI(TAG, "Email sent to %s", creds.to_addr);
    ws_server_broadcast_monitor("email", "Email sent OK");

smtp_done:
    /* Zero credentials in stack buffers before returning */
    memset(b64u, 0, sizeof(b64u));
    memset(b64p, 0, sizeof(b64p));
    memset(&creds, 0, sizeof(creds));

    esp_tls_conn_destroy(tls);
    return err;
}
