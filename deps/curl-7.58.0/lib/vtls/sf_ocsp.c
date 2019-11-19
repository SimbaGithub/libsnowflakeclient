/*
* Copyright (c) 2017-2018 Snowflake Computing
*/

#include "curl_setup.h"

#include "urldata.h"
#include "sendf.h"
#include "formdata.h" /* for the boundary function */
#include "url.h" /* for the ssl config check function */
#include "inet_pton.h"
#include "openssl.h"
#include "connect.h"
#include "slist.h"
#include "select.h"
#include "vtls.h"
#include "strcase.h"
#include "hostcheck.h"
#include "curl_printf.h"
#include "sf_ocsp.h"
#include "sf_cJSON.h"
#include <openssl/ssl.h>
#include <openssl/ocsp.h>
#include <openssl/asn1.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#ifdef __linux__
#include <linux/limits.h>
#endif

#ifdef _WIN32
// Windows
#include <windows.h>
#include <direct.h>
#include <time.h>

typedef HANDLE SF_THREAD_HANDLE;
typedef CONDITION_VARIABLE SF_CONDITION_HANDLE;
typedef CRITICAL_SECTION SF_CRITICAL_SECTION_HANDLE;
typedef SRWLOCK SF_RWLOCK_HANDLE;
typedef HANDLE SF_MUTEX_HANDLE;

#define PATH_MAX MAX_PATH  /* Maximum PATH legnth */
#define F_OK    0          /* Test for existence.  */
#define PATH_SEP "\\"
#else
// Linux and MacOSX
#include <stdlib.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

typedef pthread_t SF_THREAD_HANDLE;
typedef pthread_cond_t SF_CONDITION_HANDLE;
typedef pthread_mutex_t SF_CRITICAL_SECTION_HANDLE;
typedef pthread_rwlock_t SF_RWLOCK_HANDLE;
typedef pthread_mutex_t SF_MUTEX_HANDLE;
#define PATH_SEP "/"
#endif

#include "curl_base64.h"
#include "curl_memory.h"
#include "memdebug.h"

#define DEFAULT_OCSP_RESPONSE_CACHE_HOST "http://ocsp.snowflakecomputing.com"
#define OCSP_RESPONSE_CACHE_JSON "ocsp_response_cache.json"
#define OCSP_RESPONSE_CACHE_URL "%s/%s"

/* private function declarations */
static char *ossl_strerror(unsigned long error, char *buf, size_t size);
static CURLcode checkResponse(OCSP_RESPONSE *resp,
                       STACK_OF(X509) *ch, X509_STORE *st, struct Curl_easy *data);
static int prepareRequest(OCSP_REQUEST **req,
                   OCSP_CERTID *id,
                   struct Curl_easy *data);
static void updateOCSPResponseInMem(OCSP_CERTID *id, OCSP_RESPONSE *resp,
                             struct Curl_easy *data);
static OCSP_RESPONSE * findOCSPRespInMem(OCSP_CERTID *certid, struct Curl_easy *data);
static OCSP_RESPONSE * getOCSPResponse(X509 *cert, X509 *issuer,
                                struct connectdata *conn);
static CURLcode checkOneCert(X509 *cert, X509 *issuer,
                      STACK_OF(X509) *ch, X509_STORE *st,
                      struct connectdata *conn);
static char* ensureCacheDir(char* cache_dir, struct Curl_easy* data);
static char* mkdirIfNotExists(char* dir, struct Curl_easy* data);
static void writeOCSPCacheFile(struct Curl_easy* data);
static void readOCSPCacheFile(struct Curl_easy* data);
static OCSP_RESPONSE * queryResponderUsingCurl(
                                        char *url,
                                        OCSP_REQUEST *req,
                                        struct Curl_easy *data);
static void initOCSPCacheServer(struct Curl_easy *data);
static void downloadOCSPCache(struct Curl_easy *data);
static char* encodeOCSPCertIDToBase64(OCSP_CERTID *certid, struct Curl_easy *data);
static char* encodeOCSPRequestToBase64(OCSP_REQUEST *reqp, struct Curl_easy *data);
static char* encodeOCSPResponseToBase64(OCSP_RESPONSE* resp, struct Curl_easy *data);
static OCSP_CERTID* decodeOCSPCertIDFromBase64(char* src, struct Curl_easy *data);
static OCSP_RESPONSE* decodeOCSPResponseFromBase64(char* src, struct Curl_easy *data);
static OCSP_RESPONSE* extractOCSPRespFromValue(snowflake_libcurl_cJSON *cache_value, struct Curl_easy *data);
static snowflake_libcurl_cJSON *getCacheEntry(OCSP_CERTID* certid, struct Curl_easy *data);
static void deleteCacheEntry(OCSP_CERTID* certid, struct Curl_easy *data);
static void updateCacheWithBulkEntries(snowflake_libcurl_cJSON* tmp_cache, struct Curl_easy *data);

static void termCertOCSP();

static int _mutex_init(SF_MUTEX_HANDLE *lock);
static int _mutex_lock(SF_MUTEX_HANDLE *lock);
static int _mutex_unlock(SF_MUTEX_HANDLE *lock);
static int _mutex_term(SF_MUTEX_HANDLE *lock);

/* in memory response cache */
static snowflake_libcurl_cJSON * ocsp_cache_root = NULL;

/* mutex for ocsp_cache_root */
static SF_MUTEX_HANDLE ocsp_response_cache_mutex;


/** OCSP Cache Server is used if enabled */
static int ocsp_cache_server_enabled = 0;

static char* ocsp_cache_server_url_env = NULL;

static char ocsp_cache_server_url[4096] = "";

static char ocsp_cache_server_retry_url_pattern[4096];

/* Mutex */
int _mutex_init(SF_MUTEX_HANDLE *lock) {
#ifdef _WIN32
  *lock = CreateMutex(
	NULL,  // default security attribute
	FALSE, // initially not owned
	NULL   // unnamed mutext
  );
  return 0;
#else
  return pthread_mutex_init(lock, NULL);
#endif
}

int _mutex_lock(SF_MUTEX_HANDLE *lock) {
#ifdef _WIN32
  DWORD ret = WaitForSingleObject(*lock, INFINITE);
  return ret == WAIT_OBJECT_0 ? 0 : 1;
#else
  return pthread_mutex_lock(lock);
#endif
}

int _mutex_unlock(SF_MUTEX_HANDLE *lock) {
#ifdef _WIN32
  return ReleaseMutex(*lock) == 0;
#else
  return pthread_mutex_unlock(lock);
#endif
}

int _mutex_term(SF_MUTEX_HANDLE *lock) {
#ifdef _WIN32
  return CloseHandle(*lock) == 0;
#else
  return pthread_mutex_destroy(lock);
#endif
}

#ifdef _WIN32
/** start to sleep for 1s before retrying (milliseconds) */
static const long START_SLEEP_TIME = 1000;

    /** max sleep time before retrying (milliseconds) */
static const long MAX_SLEEP_TIME = 16000;
#else
/** start to sleep for 1s before retrying (microseconds) */
static const unsigned int START_SLEEP_TIME = 1000000;

/** max sleep time before retrying (microseconds) */
static const unsigned int MAX_SLEEP_TIME = 16000000;
#endif
static const int MAX_RETRY = 10;


/* Return error string for last OpenSSL error
 */
static char *ossl_strerror(unsigned long error, char *buf, size_t size)
{
  ERR_error_string_n(error, buf, size);
  return buf;
}


CURLcode checkResponse(OCSP_RESPONSE *resp,
                       STACK_OF(X509) *ch, X509_STORE *st, struct Curl_easy *data)
{
  int i;
  CURLcode result;
  OCSP_BASICRESP *br = NULL;
  int ocsp_res = 0;
  char error_buffer[1024];

  br = OCSP_response_get1_basic(resp);
  if (br == NULL)
  {
    failf(data, "Failed to get OCSP response basic\n");
    result = CURLE_SSL_INVALIDCERTSTATUS;
    goto end;
  }

  ocsp_res = OCSP_basic_verify(br, ch, st, 0);
  /* ocsp_res: 1... success, 0... error, -1... fatal error */
  if (ocsp_res <= 0)
  {
    failf(data,
          "OCSP response signature verification failed. ret: %s\n",
          ossl_strerror(ERR_get_error(), error_buffer,
                        sizeof(error_buffer)));
    result = CURLE_SSL_INVALIDCERTSTATUS;
    goto end;
  }
  infof(data,
        "OCSP response signature verification success. ret: %s\n",
        ossl_strerror(ERR_get_error(), error_buffer,
                      sizeof(error_buffer)));

  for (i = 0; i < OCSP_resp_count(br); i++)
  {
    int cert_status, crl_reason;
    int psec, pday;
    long skewInSec = 900L;

    OCSP_SINGLERESP *single = NULL;

    ASN1_GENERALIZEDTIME *rev, *thisupd, *nextupd;

    single = OCSP_resp_get0(br, i);
    if(!single)
      continue;

    cert_status = OCSP_single_get0_status(single, &crl_reason, &rev,
                                          &thisupd, &nextupd);


    if (ASN1_TIME_diff(&pday, &psec, thisupd, nextupd))
    {
      long validity = (pday*24*60*60 + psec)/100;
      skewInSec = validity > skewInSec ? validity : skewInSec;
      infof(data, "Diff between thisupd and nextupd "
              "day: %d, sec: %d, Tolerant skew: %d\n", pday, psec,
              skewInSec);
    }
    else
    {
      failf(data, "Invalid structure of ASN1_GENERALIZEDTIME\n");
      result = CURLE_SSL_INVALIDCERTSTATUS;
      goto end;
    }

    if(!OCSP_check_validity(thisupd, nextupd, skewInSec, -1L)) {
      failf(data, "OCSP response has expired\n");
      result = CURLE_SSL_INVALIDCERTSTATUS;
      goto end;
    }

    switch(cert_status)
    {
      case V_OCSP_CERTSTATUS_GOOD:
        result = CURLE_OK;
        infof(data, "SSL certificate status: %s (%d)\n",
              OCSP_cert_status_str(cert_status), cert_status);
        break;

      case V_OCSP_CERTSTATUS_REVOKED:
      case V_OCSP_CERTSTATUS_UNKNOWN:
        result = CURLE_SSL_INVALIDCERTSTATUS;
        failf(data, "SSL certificate revocation reason: %s (%d)\n",
              OCSP_crl_reason_str(crl_reason), crl_reason);
        goto end;
    }
  }

end:
  OCSP_BASICRESP_free(br);
  return result;
}

struct curl_read_data
{
  char * read_data_ptr;
  int size_left;
};

struct curl_memory_write
{
  char *memory_ptr;
  size_t size;
};

static size_t
write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct curl_memory_write *mem = (struct curl_memory_write *)userp;

  mem->memory_ptr = realloc(mem->memory_ptr, mem->size + realsize + 1);

  memcpy(&(mem->memory_ptr[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory_ptr[mem->size] = 0; /* null terminated */

  return realsize;
}

OCSP_RESPONSE * queryResponderUsingCurl(char *url, OCSP_REQUEST *req,
                                        struct Curl_easy *data)
{
  unsigned char *ocsp_req_der = NULL;
  int len_ocsp_req_der = 0;
  OCSP_RESPONSE * ocsp_response = NULL;
  struct curl_memory_write ocsp_response_raw;
  unsigned char *ocsp_response_der = NULL;
  CURL *ocsp_curl = NULL;
  char *ocsp_req_base64 = NULL;
  size_t ocsp_req_base64_len = 0;
  CURLcode result;
  int use_ssl;
  char *host = NULL, *port = NULL, *path = NULL;
  char urlbuf[4096];
  int ocsp_retry_cnt = 0;
#ifdef _WIN32
  long sleep_time = START_SLEEP_TIME;
#else
  unsigned int sleep_time = START_SLEEP_TIME;
#endif

  ocsp_response_raw.memory_ptr = malloc(1);  /* will be grown as needed by the realloc above */
  ocsp_response_raw.size = 0;

  len_ocsp_req_der = i2d_OCSP_REQUEST(req, &ocsp_req_der);
  if (len_ocsp_req_der<= 0 || ocsp_req_der == NULL)
  {
    failf(data, "Failed to encode ocsp request into DER format\n");
    goto end;
  }

  result = Curl_base64_encode(
      data, (char *)ocsp_req_der, (size_t)len_ocsp_req_der,
      &ocsp_req_base64, &ocsp_req_base64_len);
  if (result != CURLE_OK)
  {
    failf(data, "Failed to encode ocsp requst into base64 format\n");
    goto end;
  }

  OCSP_parse_url(url, &host, &port, &path, &use_ssl);
  /* build a direct OCSP URL or OCSP Dynamic Cache Server URL */
  snprintf(urlbuf, sizeof(urlbuf),
           ocsp_cache_server_retry_url_pattern,
           host, ocsp_req_base64);

  OPENSSL_free(host);
  OPENSSL_free(port);
  OPENSSL_free(path);

  infof(data, "OCSP fetch URL: %s", urlbuf);

  /* allocate another curl handle for ocsp checking */
  ocsp_curl = curl_easy_init();

  if (ocsp_curl)
  {
    curl_easy_setopt(ocsp_curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(ocsp_curl, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(ocsp_curl, CURLOPT_URL, urlbuf);
    curl_easy_setopt(ocsp_curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(ocsp_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(ocsp_curl, CURLOPT_WRITEDATA, &ocsp_response_raw);

    ocsp_retry_cnt = 0;
    do
    {
      CURLcode res = curl_easy_perform(ocsp_curl);

      if (res != CURLE_OK && ocsp_retry_cnt >= MAX_RETRY)
      {
        failf(data, "OCSP checking curl_easy_perform() failed: %s\n",
              curl_easy_strerror(res));
        goto end;
      }
      else if (res == CURLE_OK)
      {
        long response_code;
        curl_easy_getinfo(ocsp_curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code >= 200 && response_code < 300)
        {
          infof(data, "OCSP request returned with http status code 2XX");
          break;
        }
        else if ((response_code < 200 || response_code >= 300 )
                  && ocsp_retry_cnt >= MAX_RETRY)
        {
          failf(data, "OCSP request failed with non-200 level code: %d\n",
                response_code);
          goto end;
        }
        infof(data, "OCSP request failed with non-200 level code: %d\n",
             response_code);
      }
      infof(data, "Retrying after sleeping %ld (s)\n", (long)(sleep_time/START_SLEEP_TIME));
#ifdef _WIN32
      _sleep(sleep_time);  // sleep time is in milliseconds for windows
#else
      usleep(sleep_time);  // sleep time is in microseconds for linux
#endif
      if (sleep_time < MAX_SLEEP_TIME)
      {
        sleep_time *= 2;
      }
      ocsp_retry_cnt++;
    }
    while(ocsp_retry_cnt < MAX_RETRY);
  }

  ocsp_response_der = (unsigned char*)ocsp_response_raw.memory_ptr;
  ocsp_response = d2i_OCSP_RESPONSE(NULL,
                                    (const unsigned char**)&ocsp_response_der,
                                    (long)ocsp_response_raw.size);
  if (ocsp_response == NULL)
  {
    failf(data, "Failed to decode ocsp response\n");
  }

end:
  if (ocsp_req_base64) curl_free(ocsp_req_base64);
  if (ocsp_req_der) OPENSSL_free(ocsp_req_der);
  if (ocsp_curl) curl_easy_cleanup(ocsp_curl);

  free(ocsp_response_raw.memory_ptr);

  return ocsp_response;
}

/**
 * Prepares OCSP request
 * @param req OCSP_REQUEST
 * @param certid OCSP_CERTID
 * @param data curl handle
 * @return 1 if success otherwise 0
 */
int prepareRequest(OCSP_REQUEST **req,
                   OCSP_CERTID *certid,
                   struct Curl_easy *data)
{
  if(!*req) *req = OCSP_REQUEST_new();
  if(!*req)
  {
    failf(data, "Failed to allocate OCSP request\n");
    goto err;
  }
  if(!OCSP_request_add0_id(*req, certid))
  {
    failf(data, "Failed to attach CertID to OCSP request\n");
    goto err;
  }
  return 1;

err:
  return 0;
}

/**
 * Update the cache entry with the CertID and Response
 * @param certid OCSP_CERTID
 * @param resp OCSP_RESPONSE
 * @param data curl handle
 */
void updateOCSPResponseInMem(OCSP_CERTID *certid, OCSP_RESPONSE *resp,
                             struct Curl_easy *data)
{
  char * cert_id_encode= NULL;
  char * ocsp_response_encode = NULL;

  unsigned long unix_time;

  snowflake_libcurl_cJSON * cache_val_array = NULL;
  snowflake_libcurl_cJSON *found = NULL;

  /* encode OCSP CertID and OCSP Response */
  cert_id_encode = encodeOCSPCertIDToBase64(certid, data);
  if (cert_id_encode == NULL)
  {
    goto end;
  }
  ocsp_response_encode = encodeOCSPResponseToBase64(resp, data);
  if (ocsp_response_encode == NULL)
  {
    goto end;
  }

  /* timestamp */
  unix_time = (unsigned long)time(NULL);

  /* write to mem cache */
  cache_val_array = snowflake_libcurl_cJSON_CreateArray();
  snowflake_libcurl_cJSON_AddItemToArray(cache_val_array, snowflake_libcurl_cJSON_CreateNumber((double) unix_time));
  snowflake_libcurl_cJSON_AddItemToArray(cache_val_array, snowflake_libcurl_cJSON_CreateString(ocsp_response_encode));

  _mutex_lock(&ocsp_response_cache_mutex);
  if (ocsp_cache_root == NULL)
  {
    ocsp_cache_root = snowflake_libcurl_cJSON_CreateObject();
  }
  found = getCacheEntry(certid, data);
  if (found != NULL)
  {
    /* delete existing entry first if exists. */
    snowflake_libcurl_cJSON_DeleteItemFromObject(ocsp_cache_root, found->string);
  }
  snowflake_libcurl_cJSON_AddItemToObject(ocsp_cache_root, cert_id_encode, cache_val_array);
  _mutex_unlock(&ocsp_response_cache_mutex);
end:
  if (cert_id_encode) curl_free(cert_id_encode);
  if (ocsp_response_encode) curl_free(ocsp_response_encode);
}

/**
 * Encode OCSP CertID to Base 64 string
 *
 * @param certid OCSP_CERTID
 * @param data curl handle
 * @return base64 string
 */
char* encodeOCSPCertIDToBase64(OCSP_CERTID *certid, struct Curl_easy *data)
{
  int len;
  char* ret = NULL;
  unsigned char *der_buf = NULL;
  size_t encode_len = 0;
  CURLcode result;

  len = i2d_OCSP_CERTID(certid, &der_buf);
  if (len <= 0 || der_buf == NULL)
  {
    infof(data, "Failed to encode OCSP CertId\n");
    goto end;
  }
  result = Curl_base64_encode(data,(char *)der_buf, (size_t)len,
                              &ret, &encode_len);
  if (result != CURLE_OK)
  {
    infof(data, "Failed to encode OCSP CertId to base64: %s",
          curl_easy_strerror(result));
  }
end:
  if (der_buf) OPENSSL_free(der_buf);
  return ret;
}

/**
 * Encode OCSP Request to Base 64 string
 * @param reqp OCSP_REQUEST
 * @param data curl handle
 * @return base64 string
 */
char *encodeOCSPRequestToBase64(OCSP_REQUEST *reqp, struct Curl_easy *data)
{
  int len;
  char* ret = NULL;
  unsigned char *der_buf = NULL;
  size_t encode_len = 0;
  CURLcode result;

  len = i2d_OCSP_REQUEST(reqp, &der_buf);
  if (len <= 0 || der_buf == NULL)
  {
    infof(data, "Failed to encode OCSP response");
    goto end;
  }

  result = Curl_base64_encode(data, (char *)der_buf, (size_t)len,
                              &ret, &encode_len);
  if (result != CURLE_OK)
  {
    infof(data, "Failed to encode OCSP response to base64: %s",
          curl_easy_strerror(result));
    goto end;
  }
end:
  if (der_buf) OPENSSL_free(der_buf);
  return ret;
}

/**
 * Encode OCSP Response to Base 64 string
 *
 * @param resp OCSP_RESPONSE
 * @param data curl handle
 * @return base64 string
 */
char *encodeOCSPResponseToBase64(OCSP_RESPONSE* resp, struct Curl_easy *data)
{
  int len;
  char* ret = NULL;
  unsigned char *der_buf = NULL;
  size_t encode_len = 0;
  CURLcode result;

  len = i2d_OCSP_RESPONSE(resp, &der_buf);
  if (len <= 0 || der_buf == NULL)
  {
    infof(data, "Failed to encode OCSP response");
    goto end;
  }

  result = Curl_base64_encode(data, (char *)der_buf, (size_t)len,
                              &ret, &encode_len);
  if (result != CURLE_OK)
  {
    infof(data, "Failed to encode OCSP response to base64: %s",
          curl_easy_strerror(result));
    goto end;
  }
end:
  if (der_buf) OPENSSL_free(der_buf);
  return ret;
}

/**
 * Decoede OCSP CertID from Base 64 string
 * @param src a base64 string
 * @param data curl handle
 * @return OCSP_CERTID
 */
OCSP_CERTID* decodeOCSPCertIDFromBase64(char* src, struct Curl_easy *data)
{
  unsigned char *ocsp_certid_der = NULL;
  unsigned char *ocsp_certid_der_org = NULL;
  size_t ocs_certid_der_len;
  OCSP_CERTID *target_certid = NULL;
  CURLcode result;
  if (src == NULL)
  {
    infof(data, "Base64 input is NULL for decoding OCSP CertID\n");
    return NULL;
  }

  result = Curl_base64_decode(src,
                              &ocsp_certid_der, &ocs_certid_der_len);
  if (result != CURLE_OK)
  {
    infof(data, "Failed to decode OCSP CertID in the cache. Ignored: %s",
          curl_easy_strerror(result));
    return NULL;
  }
  /* keep the original pointer as it will be updated in d2i function */
  ocsp_certid_der_org = ocsp_certid_der;

  target_certid = d2i_OCSP_CERTID(
    NULL, (const unsigned char**)&ocsp_certid_der, (long)ocs_certid_der_len);

  curl_free(ocsp_certid_der_org);

  if (target_certid == NULL)
  {
    infof(data, "Failed to decode OCSP CertID.\n");
    return NULL;
  }
  return target_certid;
}

/**
 * Decode OCSP Response from Base 64 string
 * @param src a base64 string
 * @param data curl handle
 * @return OCSP_RESPONSE or NULL
 */
OCSP_RESPONSE * decodeOCSPResponseFromBase64(char* src, struct Curl_easy *data)
{
  OCSP_RESPONSE *resp = NULL;
  unsigned char *ocsp_response_der;
  unsigned char *ocsp_response_der_org;
  size_t ocsp_response_der_len;
  CURLcode result;

  if (src == NULL)
  {
    infof(data, "Base64 input is NULL for decoding OCSP Response\n");
    return NULL;
  }
  result = Curl_base64_decode(src,
                              &ocsp_response_der, &ocsp_response_der_len);
  if (result)
  {
    infof(data, "Failed to decode OCSP response from base64 string: %s\n",
          curl_easy_strerror(result));
    return NULL;
  }
  /* keep the original pointer as it will be updated in d2i function */
  ocsp_response_der_org = ocsp_response_der;

  resp = d2i_OCSP_RESPONSE(NULL, (const unsigned char **) &ocsp_response_der,
                           (long) ocsp_response_der_len);
  curl_free(ocsp_response_der_org);
  if (resp == NULL)
  {
    infof(data, "Failed to decode OCSP response cache from der format\n");
    return NULL;
  }
  return resp;
}

/**
 * Extract OCSP Response from the cache value.
 *
 * Must mutex protected as it may delete the existing element if the last
 * query time is older than 24 hours.
 *
 * @param cache_value a cache value to update
 * @param data curl handle
 * @return OCSP_RESPONSE
 */
OCSP_RESPONSE* extractOCSPRespFromValue(snowflake_libcurl_cJSON *cache_value, struct Curl_easy *data)
{
  long last_query_time_l = 0L;
  snowflake_libcurl_cJSON * resp_bas64_j = NULL;
  snowflake_libcurl_cJSON * last_query_time = NULL;

  if (cache_value == NULL || !snowflake_libcurl_cJSON_IsArray(cache_value))
  {
    infof(data, "OCSP Cache value is invalid\n");
    return NULL;
  }

  /* First item is the timestamp when the cache entry was created. */
  last_query_time = snowflake_libcurl_cJSON_GetArrayItem(cache_value, 0);
  if (!snowflake_libcurl_cJSON_IsNumber(last_query_time))
  {
    infof(data, "OCSP Cache Last query time is invalid\n");
    snowflake_libcurl_cJSON_DeleteItemFromObjectCaseSensitive(ocsp_cache_root,
                                            cache_value->string);
    return NULL;
  }

  last_query_time_l = (long)last_query_time->valuedouble;

  /* valid for 24 hours */
  if ((unsigned long)time(NULL) - last_query_time_l >= 24*60*60)
  {
    infof(data, "OCSP Response Cache Expired\n");
    return NULL;
  }

  /* Second item is the actual OCSP response data */
  resp_bas64_j = snowflake_libcurl_cJSON_GetArrayItem(cache_value, 1);
  if (!snowflake_libcurl_cJSON_IsString(resp_bas64_j) || resp_bas64_j->valuestring == NULL)
  {
    infof(data, "OCSP Response cache is invalid. Deleting it from the cache.\n");
    snowflake_libcurl_cJSON_DeleteItemFromObjectCaseSensitive(ocsp_cache_root,
                                            cache_value->string);
    return NULL;
  }

  /* decode OCSP Response from base64 string */
  return decodeOCSPResponseFromBase64(resp_bas64_j->valuestring, data);
}

/**
 * Find OCSP Response in memory cache.
 * @param certid OCSP CertID
 * @param data curl handle
 * @return OCSP Response if success otherwise NULL
 */
OCSP_RESPONSE * findOCSPRespInMem(OCSP_CERTID *certid, struct Curl_easy *data)
{
  /* calculate certid */
  OCSP_RESPONSE *resp = NULL;
  snowflake_libcurl_cJSON *found = NULL;

  _mutex_lock(&ocsp_response_cache_mutex);
  found = getCacheEntry(certid, data);
  if (!found)
  {
    goto end;
  }
  resp = extractOCSPRespFromValue(found, data);
  if (resp)
  {
    /* NOTE encode to base64 only for logging */
    char* cert_id_encode = encodeOCSPCertIDToBase64(certid, data);
    infof(data, "OCSP Response Cache found!!!: %s\n", cert_id_encode);
    if (cert_id_encode) curl_free(cert_id_encode);
  }
end:
  _mutex_unlock(&ocsp_response_cache_mutex);
  return resp;
}

/**
 * Get the cache entry for the CertID.
 *
 * @param certid OCSP CertID
 * @param data curl handle
 * @return snowflake_libcurl_cJSON cache entry if found otherwise NULL
 */
snowflake_libcurl_cJSON *getCacheEntry(OCSP_CERTID* certid, struct Curl_easy *data)
{
  snowflake_libcurl_cJSON *ret = NULL;
  snowflake_libcurl_cJSON *element_pointer = NULL;

  if (ocsp_cache_root == NULL)
  {
    ocsp_cache_root = snowflake_libcurl_cJSON_CreateObject();
  }

  snowflake_libcurl_cJSON_ArrayForEach(element_pointer, ocsp_cache_root)
  {
    int cmp = 0;
    OCSP_CERTID *target_certid = decodeOCSPCertIDFromBase64(
      element_pointer->string, data);

    if (target_certid == NULL)
    {
      infof(data, "Failed to decode OCSP CertID in the cache.\n");
      continue;
    }
    cmp = OCSP_id_cmp(target_certid, certid);
    OCSP_CERTID_free(target_certid);
    if (cmp == 0)
    {
      ret = element_pointer;
      break;
    }
  }
  return ret;
}

/**
 * Delete a Cache entry for Cert ID
 * @param certid OCSP_CERTID
 * @param data curl handle
 */
void deleteCacheEntry(OCSP_CERTID* certid, struct Curl_easy *data)
{
  snowflake_libcurl_cJSON *found = NULL;

  _mutex_lock(&ocsp_response_cache_mutex);
  found = getCacheEntry(certid, data);
  if (found)
  {
    snowflake_libcurl_cJSON_DeleteItemFromObject(ocsp_cache_root, found->string);
  }
  _mutex_unlock(&ocsp_response_cache_mutex);
}

/**
 * Update OCSP cache with the snowflake_libcurl_cJSON data
 * @param tmp_cache a snowflake_libcurl_cJSON data
 * @param data curl handle
 */
void updateCacheWithBulkEntries(snowflake_libcurl_cJSON* tmp_cache, struct Curl_easy *data)
{
  snowflake_libcurl_cJSON *element_pointer = NULL;
  snowflake_libcurl_cJSON *found = NULL;
  snowflake_libcurl_cJSON *new_value = NULL;

  /* Detect the existing elements */
  snowflake_libcurl_cJSON_ArrayForEach(element_pointer, tmp_cache)
  {
    OCSP_CERTID *cert_id = decodeOCSPCertIDFromBase64(
      element_pointer->string, data);
    if (cert_id == NULL)
    {
      infof(data, "CertID is NULL\n");
      continue;
    }
    found = getCacheEntry(cert_id, data);
    OCSP_CERTID_free(cert_id);
    new_value = snowflake_libcurl_cJSON_Duplicate(element_pointer, 1);
    if (found != NULL)
    {
      snowflake_libcurl_cJSON_DeleteItemFromObject(ocsp_cache_root, found->string);
    }
    snowflake_libcurl_cJSON_AddItemToObject(ocsp_cache_root, element_pointer->string, new_value);
  }
}

/**
 * Download a OCSP cache content from OCSP cache server.
 * @param data curl handle
 */
void downloadOCSPCache(struct Curl_easy *data)
{
  int retry_cnt;
  struct curl_memory_write ocsp_response_cache_json_mem;
  CURL *curlh = NULL;
  struct curl_slist *headers = NULL;
  snowflake_libcurl_cJSON *tmp_cache = NULL;
  CURLcode res = CURLE_OK;
#ifdef _WIN32
  long sleep_time = START_SLEEP_TIME;
#else
  unsigned int sleep_time = START_SLEEP_TIME;
#endif

  ocsp_response_cache_json_mem.memory_ptr = malloc(1);  /* will be grown as needed by the realloc above */
  ocsp_response_cache_json_mem.size = 0;


  /* allocate another curl handle for ocsp checking */
  curlh = curl_easy_init();
  if (!curlh)
  {
    failf(data, "Failed to initialize curl to download OCSP cache.\n");
    goto end;
  }

  headers = curl_slist_append(headers, "Content-Type:application/json");

  curl_easy_setopt(curlh, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(curlh, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt(curlh, CURLOPT_URL, ocsp_cache_server_url);
  /* empty means use all of builtin supported encodings */
  curl_easy_setopt(curlh, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curlh, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curlh, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curlh, CURLOPT_WRITEDATA, &ocsp_response_cache_json_mem);

  retry_cnt = 0;
  res = CURLE_OK;
  do
  {
    res = curl_easy_perform(curlh);
    if (res == CURLE_OK)
    {
      long response_code;
      curl_easy_getinfo(curlh, CURLINFO_RESPONSE_CODE, &response_code);
      if (response_code >= 200 && response_code < 300)
      {
        /* Success */
        infof(data, "OCSP request returned with http status code 2XX");
        break;
      }
      else if (response_code < 200 || response_code >= 300 )
      {
        failf(data, "OCSP request failed with non-200 level code: %d, retrying\n",
              response_code);
      }
    }
    if (retry_cnt >= MAX_RETRY)
    {
      failf(data, "OCSP cache could not be downloaded. %s\n",
        curl_easy_strerror(res));
      goto end;
    }
    infof(data, "Retrying after sleeping %ld (s)\n", (long)(sleep_time/START_SLEEP_TIME));
#ifdef _WIN32
    _sleep(sleep_time);  // sleep time is in milliseconds for windows
#else
    usleep(sleep_time);  // sleep time is in microseconds for linux
#endif
    if (sleep_time < MAX_SLEEP_TIME)
    {
        sleep_time *= 2;
    }
    ++retry_cnt;
  } while (retry_cnt < MAX_RETRY);

  if ( retry_cnt < MAX_RETRY)
  {
      infof(data, "Successfully downloaded OCSP Cache.\n");
  }
  else
  {
      failf(data, "OCSP cache could not be downloaded. %s\n",
            curl_easy_strerror(res));
      goto end;
  }

  _mutex_lock(&ocsp_response_cache_mutex);
  if (ocsp_cache_root == NULL)
  {
    ocsp_cache_root = snowflake_libcurl_cJSON_CreateObject();
  }
  tmp_cache = snowflake_libcurl_cJSON_Parse(ocsp_response_cache_json_mem.memory_ptr);

  /* update OCSP cache with the downloaded cache */
  updateCacheWithBulkEntries(tmp_cache, data);
  infof(data, "Number of cache entries: %d\n",
        snowflake_libcurl_cJSON_GetArraySize(ocsp_cache_root));

  _mutex_unlock(&ocsp_response_cache_mutex);
end:
  if (tmp_cache) snowflake_libcurl_cJSON_Delete(tmp_cache);
  if (curlh) curl_easy_cleanup(curlh);
  if (headers) curl_slist_free_all(headers);

  free(ocsp_response_cache_json_mem.memory_ptr);
}

/**
 * Find OCSP Response in local cache first. If not found, go to OCSP server to
 * get the response.

 * @param cert subject certificate
 * @param issuer issuer certificate
 * @param data curl connection data
 * @return OCSP response
 */
OCSP_RESPONSE * getOCSPResponse(X509 *cert, X509 *issuer,
                                struct connectdata *conn)
{
  int i;
  OCSP_RESPONSE *resp = NULL;
  OCSP_REQUEST *req = NULL;
  OCSP_CERTID *certid = NULL;
  STACK_OF(OPENSSL_STRING) *ocsp_list = NULL;
  const EVP_MD *cert_id_md = EVP_sha1();

  /* get certid first */
  certid = OCSP_cert_to_id(cert_id_md, cert, issuer);
  resp = findOCSPRespInMem(certid, conn->data);
  if (resp != NULL)
  {
    goto end; /* found OCSP response in cache */
  }

  if (ocsp_cache_server_enabled)
  {
    /* download OCSP Cache from the server*/
    downloadOCSPCache(conn->data);

    /* try hitting cache again */
    resp = findOCSPRespInMem(certid, conn->data);
    if (resp != NULL)
    {
      goto end; /* found OCSP response in cache */
    }
  }

  if (!prepareRequest(&req, certid, conn->data))
  {
    goto end; /* failed to prepare request */
  }

  /* loop through OCSP urls */
  ocsp_list = X509_get1_ocsp(cert);
  for (i = 0; i < sk_OPENSSL_STRING_num(ocsp_list); i++)
  {
    int use_ssl;
    int url_parse_result;
    char *host = NULL, *port = NULL, *path = NULL;

    char *ocsp_url = sk_OPENSSL_STRING_value(ocsp_list, i);
    infof(conn->data, "OCSP Validation URL: %s\n", ocsp_url);

    url_parse_result = OCSP_parse_url(
        ocsp_url, &host, &port, &path, &use_ssl);
    if (!url_parse_result)
    {
      failf(conn->data, "Invalid OCSP Validation URL: %s\n", ocsp_url);
      /* try the next OCSP server if available. Usually only one OCSP server
       * is attached, so it is unlikely the second OCSP server is used, but
       * in theory it could, so it let try. */
      continue;
    }

    resp = queryResponderUsingCurl(ocsp_url, req, conn->data);
    /* update local cache */
    OPENSSL_free(host);
    OPENSSL_free(path);
    OPENSSL_free(port);

    updateOCSPResponseInMem(certid, resp, conn->data);
    break; /* good if any OCSP server works */
  }

end:
  if (ocsp_list) X509_email_free(ocsp_list);
  /* when ocsp certid is found in mem, which means that no OCSP_REQUEST object
     will be created, so needs to explicitly freed certid. Otherwise, certid
     will be freed along with the deallocation of OCSP_REQUEST object */
  if (!req && certid) OCSP_CERTID_free(certid);
  /* https://www.openssl.org/docs/man1.1.0/crypto/OCSP_request_add0_id.html
   * certid must NOT be freed here */
  if (req) OCSP_REQUEST_free(req);
  return resp;
}

/**
 * Check one certificate
 * @param cert subject certificate
 * @param issuer issuer certificate
 * @param ch certificate chain
 * @param st CA certificates
 * @param conn curl connection data
 * @return curl return code
 */
CURLcode checkOneCert(X509 *cert, X509 *issuer,
                      STACK_OF(X509) *ch, X509_STORE *st,
                      struct connectdata *conn)
{
  struct Curl_easy *data = conn->data;
  CURLcode result;
#define MAX_CERT_NAME_LEN 100
  char X509_cert_name[MAX_CERT_NAME_LEN + 1];
  int retry_count = 0;
  OCSP_RESPONSE *resp = NULL;

  while(retry_count < MAX_RETRY)
  {
    OCSP_CERTID *certid = NULL;
    int ocsp_status = 0;
    resp = getOCSPResponse(cert, issuer, conn);
    infof(data, "Starting checking cert for %s...\n",
          X509_NAME_oneline(X509_get_subject_name(cert), X509_cert_name,
                            MAX_CERT_NAME_LEN));
    if (!resp)
    {
      failf(data, "Unable to get OCSP response\n");
      result = CURLE_SSL_INVALIDCERTSTATUS;
      ++retry_count;
      continue;
    }

    ocsp_status = OCSP_response_status(resp);
    if (ocsp_status != OCSP_RESPONSE_STATUS_SUCCESSFUL)
    {
      failf(data, "Invalid OCSP response status: %s (%d)\n",
            OCSP_response_status_str(ocsp_status), ocsp_status);
      result = CURLE_SSL_INVALIDCERTSTATUS;
      if (resp) OCSP_RESPONSE_free(resp);
      resp = NULL;
      ++retry_count;
      continue;
    }

    result = checkResponse(resp, ch, st, data);
    if (result == CURLE_OK)
    {
      break;
    }

    /* delete the cache if the validation failed. */
    certid = OCSP_cert_to_id(certid, cert, issuer);
    deleteCacheEntry(certid, data);
    OCSP_CERTID_free(certid);

    /* retry */
    ++retry_count;
  }

  if (result != CURLE_OK)
  {
    /* delete the cache if OCSP revocation check fails */
    OCSP_CERTID *certid = NULL;
    const EVP_MD *cert_id_md = EVP_sha1();

    /* remove the entry from cache */
    certid = OCSP_cert_to_id(cert_id_md, cert, issuer);
    deleteCacheEntry(certid, data);
    OCSP_CERTID_free(certid);
  }
  if (resp) OCSP_RESPONSE_free(resp);
  return result;
}

/**
 * Make a directory if not exists
 * @param dir a directory to create
 * @param data curl handle
 * @return directory name if success otherwise NULL
 */
char* mkdirIfNotExists(char* dir, struct Curl_easy *data)
{
#ifdef _WIN32
  int result = _mkdir(dir);
#else
  int result = mkdir(dir, 0755);
#endif
  if (result != 0)
  {
    if (errno != EEXIST)
    {
      failf(data, "Failed to create %s directory. Ignored. Error: %d\n",
            dir, errno);
      return NULL;

    }
    infof(data, "Already exists %s directory.\n", dir);
  }
  else
  {
    infof(data, "Created %s directory.\n", dir);
  }
  return dir;
}

/**
 * Ensure th cache directory is accessible
 * @param cache_dir cache directory
 * @param data curl handle
 * @return cache directory name or NULL
 */
char* ensureCacheDir(char* cache_dir, struct Curl_easy *data)
{
#ifdef __linux__
  char *home_env = getenv("HOME");
  strcpy(cache_dir, (home_env == NULL ? (char*)"/tmp" : home_env));

  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "/.cache");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "/snowflake");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
#elif defined(__APPLE__)
  char *home_env = getenv("HOME");
  strcpy(cache_dir, (home_env == NULL ? (char*)"/tmp" : home_env));
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "/Library");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "/Caches");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "/Snowflake");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
#elif  defined(_WIN32)
  char *home_env = getenv("USERPROFILE");
  if (home_env == NULL)
  {
    home_env = getenv("TMP");
	if (home_env == NULL)
    {
      home_env = getenv("TEMP");
    }
  }
  strcpy(cache_dir, (home_env == NULL ? (char*)"c:\\temp" : home_env));
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "\\AppData");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "\\Local");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "\\Snowflake");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
  strcat(cache_dir, "\\Caches");
  if (mkdirIfNotExists(cache_dir, data) == NULL)
  {
    goto err;
  }
#endif
  infof(data, "OCSP cache file directory: %s\n", cache_dir);
  return cache_dir;
err:
  return NULL;
}

/**
 * Write OCSP cache onto a file in the cache directory
 * @param data curl handle
 */
void writeOCSPCacheFile(struct Curl_easy* data)
{
  char cache_dir[PATH_MAX] = "";
  char cache_file[PATH_MAX] = "";
  char cache_lock_file[PATH_MAX] = "";
  FILE *fh;
  FILE *fp;
  char * jsonText;

  _mutex_lock(&ocsp_response_cache_mutex);
  if (ocsp_cache_root == NULL)
  {
      infof(data, "Skipping writing OCSP cache file as no OCSP cache root exists.\n");
      goto end;
  }

  if (ensureCacheDir(cache_dir, data) == NULL)
  {
    failf(data, "The cache file is not accessible.\n");
    goto end;
  }

  /* cache file */
  strcpy(cache_file, cache_dir);
  strcat(cache_file, PATH_SEP);
  strcat(cache_file, OCSP_RESPONSE_CACHE_JSON);
  infof(data, "OCSP Cache file: %s\n", cache_file);

  /* cache lock directory/file */
  strcpy(cache_lock_file, cache_file);
  strcat(cache_lock_file, ".lck");

  if (access(cache_lock_file, F_OK) != -1)
  {
    /* lck file exists */
    struct stat statbuf;
    if (stat(cache_lock_file, &statbuf) != -1)
    {
      if ((long)time(NULL) - (long) statbuf.st_mtime < 60*60)
      {
        infof(data, "Other process lock the file, ignored\n");
        goto end;
      }
      else
      {
        infof(data, "Remove the old lock file\n");
        if (remove(cache_lock_file) != 0)
        {
          infof(data, "Failed to delete the lock file: %s, ignored\n", cache_lock_file);
          goto end;
        }
      }
    }
  }

  /* create a new lck file */
  fh = fopen(cache_lock_file, "w");
  if (fh == NULL)
  {
    infof(data, "Failed to create a lock file: %s. Skipping writing OCSP cache file.\n",
        cache_lock_file);
    goto end;
  }
  if (fclose(fh) != 0)
  {
    infof(data, "Failed to close a lock file: %s. Ignored.\n", cache_lock_file);
    goto end;
  }

  fp = fopen(cache_file, "w");
  if (fp == NULL)
  {
    infof(data, "Failed to open OCSP response cache file. Skipping writing OCSP cache file.\n");
    goto end;
  }
  jsonText = snowflake_libcurl_cJSON_PrintUnformatted(ocsp_cache_root);
  if (fprintf(fp, "%s", jsonText) < 0)
  {
    infof(data, "Failed to write OCSP response cache file. Skipping\n");
  }

  if (fclose(fp) != 0)
  {
    infof(data, "Failed to close OCSP response cache file: %s. Ignored\n", cache_file);
  }
  infof(data, "Write OCSP Response to cache file\n");

  /* deallocate json string */
  snowflake_libcurl_cJSON_free(jsonText);

  if (remove(cache_lock_file) != 0)
  {
    infof(data, "Failed to delete the lock file: %s, ignored\n", cache_lock_file);
  }
end:
  if (ocsp_cache_root != NULL)
  {
    snowflake_libcurl_cJSON_Delete(ocsp_cache_root);
    ocsp_cache_root = NULL;
  }
  _mutex_unlock(&ocsp_response_cache_mutex);
}

/**
 * Read OCSP cache from from the local cache directory
 * @param data curl handle
 */
void readOCSPCacheFile(struct Curl_easy* data)
{
  char cache_dir[PATH_MAX] = "";
  char cache_file[PATH_MAX] = "";
  char ocsp_resp_cache_str[20 * 1200]; /* TODO: malloc? */
  FILE* pfile = NULL;

  ensureCacheDir(cache_dir, data);

  /* cache file */
  strcpy(cache_file, cache_dir);
  strcat(cache_file, PATH_SEP);
  strcat(cache_file, OCSP_RESPONSE_CACHE_JSON);
  infof(data, "OCSP cache file: %s\n", cache_file);

  _mutex_lock(&ocsp_response_cache_mutex);
  if (ocsp_cache_root != NULL)
  {
    infof(data, "OCSP cache was already read onto memory\n");
    goto end;
  }
  if (access(cache_file, F_OK) == -1)
  {
    infof(data, "No OCSP cache file found on disk. file: %s\n", cache_file);
    goto end;
  }

  pfile = fopen(cache_file, "r");
  if (pfile == NULL)
  {
    infof(data, "Failed to open OCSP response cache file. Ignored.\n");
    goto end;
  }

  if (fgets(ocsp_resp_cache_str, sizeof(ocsp_resp_cache_str), pfile) == NULL) {
    infof(data, "Failed to read OCSP response cache file. Ignored\n");
    goto file_close;
  }
  /* just attached the whole JSON object */
  ocsp_cache_root = snowflake_libcurl_cJSON_Parse(ocsp_resp_cache_str);
  if (ocsp_cache_root == NULL)
  {
    infof(data, "Failed to parse cache file content in json format\n");
  }
  else
  {
    infof(data, "OCSP cache file was successfully loaded\n");
  }
file_close:
  if (fclose(pfile) != 0)
  {
    infof(data, "Failed to close cache file. Ignored.\n");
    goto end;
  }
end:
  if (ocsp_cache_root == NULL) ocsp_cache_root = snowflake_libcurl_cJSON_CreateObject();
  _mutex_unlock(&ocsp_response_cache_mutex);
  return;
}

/**
 * Initialize OCSP Cache Server
 * @param data curl handle
 */
void initOCSPCacheServer(struct Curl_easy *data)
{
  char* ocsp_cache_server_enabled_env = NULL;
  /* OCSP cache server URL */
  _mutex_lock(&ocsp_response_cache_mutex);
    ocsp_cache_server_enabled_env = getenv(
    "SF_OCSP_RESPONSE_CACHE_SERVER_ENABLED");
  if (ocsp_cache_server_enabled_env == NULL ||
      strcmp(ocsp_cache_server_enabled_env, "false") != 0)
  {
    /* OCSP Cache server enabled by default */
    ocsp_cache_server_enabled = 1;
  }
  else
  {
    ocsp_cache_server_enabled = 0;
  }

  ocsp_cache_server_url_env = getenv(
    "SF_OCSP_RESPONSE_CACHE_SERVER_URL");

  if (ocsp_cache_server_url_env == NULL)
  {
    /* default URL */
    snprintf(ocsp_cache_server_url, sizeof(ocsp_cache_server_url),
             OCSP_RESPONSE_CACHE_URL,
             DEFAULT_OCSP_RESPONSE_CACHE_HOST,
             OCSP_RESPONSE_CACHE_JSON);
    strncpy(ocsp_cache_server_retry_url_pattern,
            "http://%s/%s", sizeof(ocsp_cache_server_retry_url_pattern));
  }
  else
  {
    // if specified, always use the special retry url
    int use_ssl;
    char *host = NULL, *port = NULL, *path = NULL;
    snprintf(ocsp_cache_server_url, sizeof(ocsp_cache_server_url),
            ocsp_cache_server_url_env);
    OCSP_parse_url(
        ocsp_cache_server_url_env, &host, &port, &path, &use_ssl);
    snprintf(ocsp_cache_server_retry_url_pattern,
           sizeof(ocsp_cache_server_retry_url_pattern),
           "%s://%s:%s/retry/%%s/%%s", use_ssl ? "https" : "http", host, port);
    OPENSSL_free(host);
    OPENSSL_free(path);
    OPENSSL_free(port);
  }

  infof(data, "SF_OCSP_RESPONSE_CACHE_SERVER_ENABLED: %s\n",
    ocsp_cache_server_enabled ? "true" : "false");
  infof(data, "SF_OCSP_RESPONSE_CACHE_SERVER_URL: %s\n",
    ocsp_cache_server_url);
  _mutex_unlock(&ocsp_response_cache_mutex);
}

/**
 * Initialize Check OCSP revocation status method
 */
SF_PUBLIC(CURLcode) initCertOCSP()
{
  /* must call only once. not thread safe */
  _mutex_init(&ocsp_response_cache_mutex);
  atexit(termCertOCSP);
  return CURLE_OK;
}

/**
 * Terminate Check OCSP revocation status method
 */
static void termCertOCSP()
{
  /* terminate the mutex */
  _mutex_term(&ocsp_response_cache_mutex);
}

/**
 * Main entry point. Check OCSP revocation status.
 * @param conn Curl connection
 * @param ch a chain of certificates
 * @param st CA trust
 * @return Curl return code
 */
SF_PUBLIC(CURLcode) checkCertOCSP(struct connectdata *conn, STACK_OF(X509) *ch, X509_STORE *st)
{
  struct Curl_easy *data = conn->data;
  int numcerts;
  int i;
  CURLcode rs = CURLE_OK;

  infof(data, "Cert Data Store: %s, Ceritifcate Chain: %s\n", st, ch);
  initOCSPCacheServer(data);

  infof(data, "Start SF OCSP Validation...\n");
  readOCSPCacheFile(data);

  numcerts = sk_X509_num(ch);
  infof(data, "Number of certificates in the chain: %d\n", numcerts);
  for (i =0; i < numcerts - 1; i++)
  {
    X509* cert = sk_X509_value(ch, i);
    X509* issuer = sk_X509_value(ch, i+1);

    rs = checkOneCert(cert, issuer, ch, st, conn);
    if (rs != CURLE_OK)
    {
      goto end;
    }
  }
  writeOCSPCacheFile(data);
end:
  infof(data, "End SF OCSP Validation... Result: %d\n", rs);
  return rs;
}
