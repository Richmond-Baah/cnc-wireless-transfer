#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stddef.h>

#define CNC_AUTH_MAX_TOKENS      5
#define CNC_AUTH_TOKEN_LEN       32
#define CNC_AUTH_TOKEN_STR_LEN   65

#ifndef CNC_AUTH_TOKEN_TTL_SEC
#define CNC_AUTH_TOKEN_TTL_SEC   3600
#endif

void auth_init(void);
bool auth_check_credentials(const char *username, const char *password);

const char *auth_create_session(void);

bool auth_validate_token(const char *token);

void auth_invalidate_token(const char *token);
void auth_invalidate_all(void);

size_t auth_get_active_token_count(void);

#endif // AUTH_H
