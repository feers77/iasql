# IA-SQL — PGXS build
MODULE_big = ia_sql
OBJS = src/ia_sql.o src/worker.o src/http_client.o src/llm.o src/vendor/cJSON.o
EXTENSION = ia_sql
DATA = sql/ia_sql--0.1.sql
PGFILEDESC = "IA-SQL - in-database LLM Wiki (Karpathy pattern)"

PG_CPPFLAGS += -Isrc
SHLIB_LINK += -lcurl

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
