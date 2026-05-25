/*
 * http_client.c — minimal libcurl wrapper used by the IA-SQL worker to talk to
 * an external OpenAI-compatible LLM endpoint.
 */
#include "postgres.h"
#include "lib/stringinfo.h"

#include <curl/curl.h>

#include "http_client.h"

void
ia_http_global_init(void)
{
	curl_global_init(CURL_GLOBAL_ALL);
}

void
ia_http_global_cleanup(void)
{
	curl_global_cleanup();
}

/* libcurl write callback: append the received bytes to a StringInfo. */
static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	StringInfo	buf = (StringInfo) userdata;
	size_t		total = size * nmemb;

	appendBinaryStringInfo(buf, ptr, (int) total);
	return total;
}

char *
ia_http_post_json(const char *url, const char *api_key, const char *json_body,
				  long timeout_ms, long *http_code, char **errbuf)
{
	CURL	   *curl;
	CURLcode	rc;
	struct curl_slist *headers = NULL;
	StringInfoData resp;

	*errbuf = NULL;
	*http_code = 0;

	curl = curl_easy_init();
	if (curl == NULL)
	{
		*errbuf = pstrdup("curl_easy_init failed");
		return NULL;
	}

	initStringInfo(&resp);

	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (api_key != NULL && api_key[0] != '\0')
	{
		StringInfoData auth;

		initStringInfo(&auth);
		appendStringInfo(&auth, "Authorization: Bearer %s", api_key);
		headers = curl_slist_append(headers, auth.data);
		pfree(auth.data);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "ia_sql/0.1");

	rc = curl_easy_perform(curl);

	if (rc != CURLE_OK)
	{
		StringInfoData e;

		initStringInfo(&e);
		appendStringInfo(&e, "HTTP request failed: %s", curl_easy_strerror(rc));
		*errbuf = e.data;
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		if (resp.data)
			pfree(resp.data);
		return NULL;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return resp.data;			/* palloc'd, NUL-terminated by StringInfo */
}
