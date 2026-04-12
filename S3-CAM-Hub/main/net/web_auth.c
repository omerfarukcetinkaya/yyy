/**
 * @file web_auth.c
 * @brief HTTP Basic Auth for the admin panel.
 *
 * DEV CREDENTIALS — see web_auth.h. Replace before production.
 */
#include "web_auth.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "web_auth";

/* Pre-computed "Basic <base64(user:pass)>" string */
static char s_expected_auth[128] = {0};
/* Per-boot session token — 32 hex chars (128-bit random) */
static char s_session_token[33]  = {0};
static bool s_initialized = false;

void web_auth_init(void)
{
    if (s_initialized) return;

    /* Construct "username:password" */
    char credentials[64];
    snprintf(credentials, sizeof(credentials), "%s:%s",
             CONFIG_VH_WEB_USERNAME, CONFIG_VH_WEB_PASSWORD);

    /* Base64-encode */
    unsigned char b64[96] = {0};
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                          (const unsigned char *)credentials,
                          strlen(credentials));

    snprintf(s_expected_auth, sizeof(s_expected_auth), "Basic %.*s",
             (int)b64_len, b64);

    /* Generate per-boot session token from hardware RNG */
    uint32_t rnd[4];
    for (int i = 0; i < 4; i++) rnd[i] = esp_random();
    snprintf(s_session_token, sizeof(s_session_token),
             "%08lx%08lx%08lx%08lx",
             (unsigned long)rnd[0], (unsigned long)rnd[1],
             (unsigned long)rnd[2], (unsigned long)rnd[3]);

    s_initialized = true;
    ESP_LOGI(TAG, "Auth initialized for user '%s'.", CONFIG_VH_WEB_USERNAME);
}

bool web_auth_check(httpd_req_t *req)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Auth not initialized — denying request.");
        return false;
    }

    /* 1. Check session cookie (fast path — no password prompt after first login) */
    char cookie_hdr[192] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie",
                                    cookie_hdr, sizeof(cookie_hdr)) == ESP_OK) {
        if (strstr(cookie_hdr, s_session_token) != NULL) {
            return true;
        }
    }

    /* 2. Fall back to Basic Auth (first visit / incognito) */
    char auth_hdr[128] = {0};
    esp_err_t ret = httpd_req_get_hdr_value_str(req, "Authorization",
                                                auth_hdr, sizeof(auth_hdr));
    if (ret != ESP_OK) return false;

    return (strncmp(auth_hdr, s_expected_auth, strlen(s_expected_auth)) == 0);
}

void web_auth_set_session_cookie(httpd_req_t *req)
{
    char hdr[96];
    /* SameSite=Lax (not Strict): iOS Safari drops Strict cookies on the
     * WebSocket upgrade request, which would 401 /ws/stream. Lax still
     * protects against cross-site CSRF for mutating requests while
     * allowing same-origin WS handshakes to carry the session. */
    snprintf(hdr, sizeof(hdr),
             "sid=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400",
             s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
}

void web_auth_clear_session_cookie(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "sid=; Path=/; HttpOnly; Max-Age=0");
}

const char *web_auth_get_session_token(void)
{
    return s_session_token;
}

bool web_auth_token_matches(const char *token)
{
    if (!s_initialized || !token) return false;
    /* Constant-time compare to avoid trivial timing leaks. */
    size_t n = strlen(s_session_token);
    if (strlen(token) != n) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= token[i] ^ s_session_token[i];
    return diff == 0;
}

void web_auth_send_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"yyy Vision Hub\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Unauthorized. Provide valid credentials.");
}
