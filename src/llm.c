/*
 * llm.c — talk to an OpenAI-compatible /chat/completions endpoint and turn the
 * model's reply into structured wiki pages/edges (ingest) or flags (lint).
 *
 * cJSON (which uses malloc/free) is confined to this file: every value we keep
 * is copied into palloc'd memory before the cJSON tree is freed, and no
 * ereport() happens while a cJSON tree is alive, so there are no leaks across
 * error unwinding.
 */
#include "postgres.h"
#include "lib/stringinfo.h"

#include "ia_sql.h"
#include "http_client.h"
#include "llm.h"
#include "vendor/cJSON.h"

/* pstrdup a string field from a JSON object, or NULL if missing/not a string. */
static char *
jstr(const cJSON *obj, const char *key)
{
	cJSON	   *it = cJSON_GetObjectItemCaseSensitive(obj, key);

	if (cJSON_IsString(it) && it->valuestring != NULL)
		return pstrdup(it->valuestring);
	return NULL;
}

/*
 * Send a system+user chat and return the assistant's message content (palloc'd),
 * or NULL with *errbuf set. Fills token counts when the endpoint reports them.
 */
static char *
chat(const char *system, const char *user,
	 int *prompt_tokens, int *completion_tokens, char **errbuf)
{
	cJSON	   *root;
	cJSON	   *messages;
	cJSON	   *m;
	char	   *req;
	char	   *resp;
	long		code = 0;
	StringInfoData url;
	cJSON	   *rj;
	cJSON	   *choices;
	cJSON	   *content_item;
	cJSON	   *usage;
	char	   *content = NULL;

	*errbuf = NULL;

	/* ---- build the request body ---- */
	root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", ia_sql_llm_model);
	cJSON_AddNumberToObject(root, "temperature", ia_sql_llm_temperature);
	cJSON_AddNumberToObject(root, "max_tokens", ia_sql_llm_max_tokens);
	cJSON_AddBoolToObject(root, "stream", 0);
	messages = cJSON_AddArrayToObject(root, "messages");

	m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", "system");
	cJSON_AddStringToObject(m, "content", system);
	cJSON_AddItemToArray(messages, m);

	m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", "user");
	cJSON_AddStringToObject(m, "content", user);
	cJSON_AddItemToArray(messages, m);

	/*
	 * Merge provider-specific extra fields (top-level keys override). Lets a
	 * deployment add e.g. {"chat_template_kwargs":{"enable_thinking":false}}
	 * for Qwen3, or {"response_format":{"type":"json_object"}}, without code
	 * changes — keeping the engine provider-agnostic.
	 */
	if (ia_sql_llm_extra_json != NULL && ia_sql_llm_extra_json[0] != '\0')
	{
		cJSON	   *extra = cJSON_Parse(ia_sql_llm_extra_json);

		if (extra != NULL)
		{
			cJSON	   *child = extra->child;

			while (child != NULL)
			{
				cJSON	   *next = child->next;

				cJSON_DeleteItemFromObjectCaseSensitive(root, child->string);
				cJSON_AddItemToObject(root, child->string,
									  cJSON_Duplicate(child, 1));
				child = next;
			}
			cJSON_Delete(extra);
		}
	}

	req = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (req == NULL)
	{
		*errbuf = pstrdup("failed to serialise LLM request");
		return NULL;
	}

	/* ---- POST it ---- */
	initStringInfo(&url);
	appendStringInfo(&url, "%s/chat/completions", ia_sql_llm_base_url);
	resp = ia_http_post_json(url.data, ia_sql_llm_api_key, req,
							 ia_sql_llm_timeout_ms, &code, errbuf);
	cJSON_free(req);
	pfree(url.data);

	if (resp == NULL)
		return NULL;			/* *errbuf already set */

	if (code != 200)
	{
		StringInfoData e;

		initStringInfo(&e);
		appendStringInfo(&e, "LLM endpoint returned HTTP %ld: %.400s", code, resp);
		*errbuf = e.data;
		pfree(resp);
		return NULL;
	}

	/* ---- parse the envelope ---- */
	rj = cJSON_Parse(resp);
	pfree(resp);
	if (rj == NULL)
	{
		*errbuf = pstrdup("LLM response was not valid JSON");
		return NULL;
	}

	choices = cJSON_GetObjectItemCaseSensitive(rj, "choices");
	if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0)
	{
		cJSON	   *c0 = cJSON_GetArrayItem(choices, 0);
		cJSON	   *msg = cJSON_GetObjectItemCaseSensitive(c0, "message");

		content_item = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
		if (cJSON_IsString(content_item) && content_item->valuestring)
			content = pstrdup(content_item->valuestring);
	}

	usage = cJSON_GetObjectItemCaseSensitive(rj, "usage");
	if (usage)
	{
		cJSON	   *pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
		cJSON	   *ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");

		if (cJSON_IsNumber(pt))
			*prompt_tokens = pt->valueint;
		if (cJSON_IsNumber(ct))
			*completion_tokens = ct->valueint;
	}

	cJSON_Delete(rj);

	if (content == NULL)
		*errbuf = pstrdup("LLM response had no message content");

	return content;
}

/*
 * Models sometimes wrap JSON in prose or ```json fences. Return a palloc'd copy
 * of the substring from the first '{' to the last '}', or NULL.
 */
static char *
extract_json_object(const char *s)
{
	const char *start = strchr(s, '{');
	const char *end = strrchr(s, '}');

	if (start == NULL || end == NULL || end < start)
		return NULL;
	return pnstrdup(start, (Size) (end - start + 1));
}

bool
ia_llm_ingest(const char *doc_id, const char *content,
			  const char *wiki_context, LlmResult *out, char **errbuf)
{
	StringInfoData user;
	char	   *reply;
	char	   *json;
	cJSON	   *root;
	cJSON	   *pages;
	cJSON	   *edges;
	int			i;

	memset(out, 0, sizeof(*out));
	*errbuf = NULL;

	initStringInfo(&user);
	appendStringInfo(&user,
		"=== CURRENT WIKI CONTEXT (entity — summary) ===\n%s\n\n"
		"=== NEW SOURCE DOCUMENT (id=%s) ===\n%s\n\n"
		"Compile/update the wiki now. Reply ONLY with the JSON object.",
		(wiki_context && wiki_context[0]) ? wiki_context : "(empty)",
		doc_id ? doc_id : "", content ? content : "");

	reply = chat(ia_sql_wiki_system_prompt, user.data,
				 &out->prompt_tokens, &out->completion_tokens, errbuf);
	pfree(user.data);
	if (reply == NULL)
		return false;

	json = extract_json_object(reply);
	pfree(reply);
	if (json == NULL)
	{
		*errbuf = pstrdup("LLM did not return a JSON object");
		return false;
	}

	root = cJSON_Parse(json);
	pfree(json);
	if (root == NULL)
	{
		*errbuf = pstrdup("LLM JSON output could not be parsed");
		return false;
	}

	pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
	if (cJSON_IsArray(pages))
	{
		int			n = cJSON_GetArraySize(pages);

		out->pages = (LlmPage *) palloc0(sizeof(LlmPage) * Max(n, 1));
		for (i = 0; i < n; i++)
		{
			cJSON	   *p = cJSON_GetArrayItem(pages, i);
			char	   *entity = jstr(p, "entity");

			if (entity == NULL)
				continue;		/* a page without an entity key is unusable */
			out->pages[out->n_pages].entity = entity;
			out->pages[out->n_pages].title = jstr(p, "title");
			out->pages[out->n_pages].markdown = jstr(p, "markdown");
			out->pages[out->n_pages].summary = jstr(p, "summary");
			out->n_pages++;
		}
	}

	edges = cJSON_GetObjectItemCaseSensitive(root, "edges");
	if (cJSON_IsArray(edges))
	{
		int			n = cJSON_GetArraySize(edges);

		out->edges = (LlmEdge *) palloc0(sizeof(LlmEdge) * Max(n, 1));
		for (i = 0; i < n; i++)
		{
			cJSON	   *e = cJSON_GetArrayItem(edges, i);
			char	   *src = jstr(e, "source");
			char	   *tgt = jstr(e, "target");
			cJSON	   *w;

			if (src == NULL || tgt == NULL)
				continue;
			out->edges[out->n_edges].source = src;
			out->edges[out->n_edges].target = tgt;
			out->edges[out->n_edges].relation = jstr(e, "relation");
			w = cJSON_GetObjectItemCaseSensitive(e, "weight");
			out->edges[out->n_edges].weight = cJSON_IsNumber(w) ? w->valuedouble : 1.0;
			out->n_edges++;
		}
	}

	cJSON_Delete(root);

	if (out->n_pages == 0)
	{
		*errbuf = pstrdup("LLM returned no usable pages");
		return false;
	}
	return true;
}

bool
ia_llm_lint(const char *page_entity, const char *page_markdown,
			const char *sources, LlmResult *out, char **errbuf)
{
	StringInfoData user;
	char	   *reply;
	char	   *json;
	cJSON	   *root;
	cJSON	   *flags;
	int			i;

	memset(out, 0, sizeof(*out));
	*errbuf = NULL;

	initStringInfo(&user);
	appendStringInfo(&user,
		"=== WIKI PAGE (entity=%s) ===\n%s\n\n"
		"=== SOURCE DOCUMENTS (ground truth) ===\n%s\n\n"
		"Audit the page against the sources. Reply ONLY with the JSON object.",
		page_entity ? page_entity : "", page_markdown ? page_markdown : "",
		(sources && sources[0]) ? sources : "(none)");

	reply = chat(ia_sql_lint_system_prompt, user.data,
				 &out->prompt_tokens, &out->completion_tokens, errbuf);
	pfree(user.data);
	if (reply == NULL)
		return false;

	json = extract_json_object(reply);
	pfree(reply);
	if (json == NULL)
	{
		*errbuf = pstrdup("LLM did not return a JSON object");
		return false;
	}

	root = cJSON_Parse(json);
	pfree(json);
	if (root == NULL)
	{
		*errbuf = pstrdup("LLM JSON output could not be parsed");
		return false;
	}

	flags = cJSON_GetObjectItemCaseSensitive(root, "flags");
	if (cJSON_IsArray(flags))
	{
		int			n = cJSON_GetArraySize(flags);

		out->flags = (LlmFlag *) palloc0(sizeof(LlmFlag) * Max(n, 1));
		for (i = 0; i < n; i++)
		{
			cJSON	   *f = cJSON_GetArrayItem(flags, i);
			char	   *desc = jstr(f, "description");

			if (desc == NULL)
				continue;
			out->flags[out->n_flags].severity = jstr(f, "severity");
			out->flags[out->n_flags].description = desc;
			out->n_flags++;
		}
	}

	cJSON_Delete(root);
	return true;
}
