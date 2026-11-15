/* Minimal compile-time stub: this build has NETSURF_USE_CURL=NO.
 * content/fetch.c includes curl.h unconditionally for the CURL types even
 * when the cURL fetcher is not built. */
#ifndef TOARU_SHIM_CURL_CURL_H
#define TOARU_SHIM_CURL_CURL_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CURL CURL;
typedef struct CURLM CURLM;
typedef struct curl_slist curl_slist;
typedef struct curl_httppost curl_httppost;
#ifdef __cplusplus
}
#endif
#endif
