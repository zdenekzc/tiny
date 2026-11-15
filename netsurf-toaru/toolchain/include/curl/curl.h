/* ToaruOS wrapper around the real libcurl headers.
 *
 * <curl/curl.h> only includes <sys/select.h> on a hard-coded list of systems,
 * and ToaruOS is not on it.  Its <sys/types.h> defines the fd_set type but
 * not the FD_* macros, so the macros are pulled in here for curl's
 * curl_multi_fdset() and NetSurf's fetch code. */
#ifndef TOARU_SHIM_CURL_CURL_H
#define TOARU_SHIM_CURL_CURL_H
#include_next <curl/curl.h>
#include <sys/select.h>
#endif
