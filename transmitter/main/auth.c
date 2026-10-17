#include "auth.h"
#include "sdkconfig.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_CNC_DEFAULT_WEB_USER
#define CONFIG_CNC_DEFAULT_WEB_USER "admin"
#endif

#ifndef CONFIG_CNC_DEFAULT_WEB_PASS
#define CONFIG_CNC_DEFAULT_WEB_PASS "admin123"
#endif

#define DEFAULT_USER CONFIG_CNC_DEFAULT_WEB_USER
#define DEFAULT_PASS CONFIG_CNC_DEFAULT_WEB_PASS

static const char *TAG = "AUTH";

typedef struct {
    char     token[CNC_AUTH_TOKEN_STR_LEN];
    uint32_t created_at;
} auth_token_entry_t;

static auth_token_entry_t s_tokens[CNC_AUTH_MAX_TOKENS];
static size_t            s_active_tokens = 0;

static char sys_user[32] = {0};
static char sys_pass[64] = {0};

static uint32_t monotonic_seconds(void) {
    return (uint32_t)(esp_log_timestamp() / 1000);
}

static bool constant_time_strcmp(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) {
        diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    }
    return diff == 0;
}

static void evict_expired_tokens(void) {
    uint32_t now = monotonic_seconds();
    size_t   w   = 0;
    for (size_t i = 0; i < s_active_tokens; i++) {
        uint32_t age = now - s_tokens[i].created_at;
        if (age < CNC_AUTH_TOKEN_TTL_SEC) {
            if (w != i) s_tokens[w] = s_tokens[i];
            w++;
        }
    }
    for (size_t i = w; i < s_active_tokens; i++) {
        memset(&s_tokens[i], 0, sizeof(s_tokens[i]));
    }
    s_active_tokens = w;
}

static void generate_token_string(char *out) {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    uint32_t r4 = esp_random();
    snprintf(out, CNC_AUTH_TOKEN_STR_LEN, "%08lx%08lx%08lx%08lx",
             (unsigned long)r1, (unsigned long)r2,
             (unsigned long)r3, (unsigned long)r4);
}

void auth_init(void) {
    nvs_handle_t nvs_handle;
    // Start with compiled-in defaults
    snprintf(sys_user, sizeof(sys_user), "%s", DEFAULT_USER);
    snprintf(sys_pass, sizeof(sys_pass), "%s", DEFAULT_PASS);

    esp_err_t err = nvs_open("sys_cfg", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        char tmp_user[32] = "";   // same size as sys_user
        char tmp_pass[64] = "";   // same size as sys_pass
        size_t len;
        // Only use NVS value if it is non-empty
        len = sizeof(tmp_user);
        if (nvs_get_str(nvs_handle, "auth_user", tmp_user, &len) == ESP_OK && strlen(tmp_user) > 0) {
            strncpy(sys_user, tmp_user, sizeof(sys_user) - 1);
            sys_user[sizeof(sys_user) - 1] = '\0';
        } else {
            ESP_LOGW(TAG, "auth_user missing/empty in NVS, using default");
        }
        len = sizeof(tmp_pass);
        if (nvs_get_str(nvs_handle, "auth_pass", tmp_pass, &len) == ESP_OK && strlen(tmp_pass) > 0) {
            strncpy(sys_pass, tmp_pass, sizeof(sys_pass) - 1);
            sys_pass[sizeof(sys_pass) - 1] = '\0';
        } else {
            ESP_LOGW(TAG, "auth_pass missing/empty in NVS, using default");
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "sys_cfg NVS open failed, using default credentials");
    }
    s_active_tokens = 0;
    ESP_LOGI(TAG, "Auth initialized (user=[%s], TTL=%ds, max_tokens=%d)",
             sys_user, CNC_AUTH_TOKEN_TTL_SEC, CNC_AUTH_MAX_TOKENS);
}

bool auth_check_credentials(const char *username, const char *password) {
    if (!username || !password) return false;
    if (constant_time_strcmp(username, sys_user) &&
        constant_time_strcmp(password, sys_pass)) {
        return true;
    }
    return false;
}

const char *auth_create_session(void) {
    evict_expired_tokens();

    if (s_active_tokens >= CNC_AUTH_MAX_TOKENS) {
        memmove(&s_tokens[0], &s_tokens[1],
                (CNC_AUTH_MAX_TOKENS - 1) * sizeof(auth_token_entry_t));
        s_active_tokens = CNC_AUTH_MAX_TOKENS - 1;
    }

    char *slot = s_tokens[s_active_tokens].token;
    generate_token_string(slot);
    s_tokens[s_active_tokens].created_at = monotonic_seconds();
    s_active_tokens++;

    ESP_LOGI(TAG, "Session created (active=%u/%d)",
             (unsigned)s_active_tokens, CNC_AUTH_MAX_TOKENS);
    return slot;
}

bool auth_validate_token(const char *token) {
    if (!token) return false;

    evict_expired_tokens();

    for (size_t i = 0; i < s_active_tokens; i++) {
        if (constant_time_strcmp(token, s_tokens[i].token)) {
            return true;
        }
    }
    return false;
}

void auth_invalidate_token(const char *token) {
    if (!token) return;
    for (size_t i = 0; i < s_active_tokens; i++) {
        if (constant_time_strcmp(token, s_tokens[i].token)) {
            memset(&s_tokens[i], 0, sizeof(s_tokens[i]));
            for (size_t j = i; j + 1 < s_active_tokens; j++) {
                s_tokens[j] = s_tokens[j + 1];
            }
            s_active_tokens--;
            ESP_LOGI(TAG, "Token invalidated (active=%u)",
                     (unsigned)s_active_tokens);
            return;
        }
    }
}

void auth_invalidate_all(void) {
    memset(s_tokens, 0, sizeof(s_tokens));
    s_active_tokens = 0;
    ESP_LOGI(TAG, "All tokens invalidated");
}

size_t auth_get_active_token_count(void) {
    return s_active_tokens;
}
