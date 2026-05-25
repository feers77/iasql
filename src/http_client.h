/*
 * http_client.h — minimal libcurl wrapper for IA-SQL.
 */
#ifndef IA_SQL_HTTP_CLIENT_H
#define IA_SQL_HTTP_CLIENT_H

#include "postgres.h"

/* Call once per process (worker startup) / cleanup at shutdown. */
extern void ia_http_global_init(void);
extern void ia_http_global_cleanup(void);

/*
 * POST json_body to url with an optional "Authorization: Bearer <api_key>".
 * On success returns a palloc'd, NUL-terminated response body and sets
 * *http_code. On transport failure returns NULL and sets *errbuf (palloc'd).
 * Allocations land in CurrentMemoryContext, so switch to a durable context
 * before calling.
 */
extern char *ia_http_post_json(const char *url, const char *api_key,
							   const char *json_body, long timeout_ms,
							   long *http_code, char **errbuf);

#endif							/* IA_SQL_HTTP_CLIENT_H */
