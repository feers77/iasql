/*
 * llm.h — build requests for, and parse responses from, an OpenAI-compatible
 * chat-completions endpoint, mapping the model output to wiki pages and edges.
 */
#ifndef IA_SQL_LLM_H
#define IA_SQL_LLM_H

#include "postgres.h"

typedef struct LlmPage
{
	char	   *entity;
	char	   *title;
	char	   *markdown;
	char	   *summary;
} LlmPage;

typedef struct LlmEdge
{
	char	   *source;
	char	   *target;
	char	   *relation;
	double		weight;
} LlmEdge;

typedef struct LlmFlag
{
	char	   *severity;
	char	   *description;
} LlmFlag;

typedef struct LlmResult
{
	LlmPage    *pages;
	int			n_pages;
	LlmEdge    *edges;
	int			n_edges;
	LlmFlag    *flags;			/* used by the lint path */
	int			n_flags;
	int			prompt_tokens;
	int			completion_tokens;
} LlmResult;

/*
 * INGEST: compile/update the wiki from a new document given the current context.
 * LINT:   audit a compiled page against its source documents.
 * Both fill *out (palloc'd in CurrentMemoryContext) and return true on success,
 * or return false and set *errbuf. Switch to a durable context before calling.
 */
extern bool ia_llm_ingest(const char *doc_id, const char *content,
						  const char *wiki_context, LlmResult *out, char **errbuf);

extern bool ia_llm_lint(const char *page_entity, const char *page_markdown,
						const char *sources, LlmResult *out, char **errbuf);

#endif							/* IA_SQL_LLM_H */
