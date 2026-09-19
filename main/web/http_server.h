/*
 * NetDash HTTP server: the gzipped single-page UI plus the JSON REST API
 * documented in docs/API.md.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * esp_http_server sizes its handler table at start time and there is no
 * Kconfig symbol for it in IDF 5.5, so http_server_init() must set
 * httpd_config_t::max_uri_handlers to this value.
 *
 * Overflowing this does not fail the build or stop the server: the routes past
 * the limit simply fail to register and answer 404 or 405 at runtime, which
 * looks exactly like a routing bug. http_server.c carries a _Static_assert
 * against the size of the route table so that cannot happen quietly again.
 */
#define NETDASH_HTTPD_MAX_URI_HANDLERS 48

/* Listening port. */
#define NETDASH_HTTPD_PORT 80

/* Starts esp_http_server on port 80 and registers every handler. */
esp_err_t http_server_init(void);

/* Stops the server. Safe to call when it is not running. */
esp_err_t http_server_stop(void);

#ifdef __cplusplus
}
#endif
