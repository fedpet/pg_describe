# PGXS build. It asks pg_config for every include path, compiler flag and
# install location, so the extension always matches the server it is built
# against.
#
#   make && make install          # against the pg_config on PATH
#   make PG_CONFIG=/path/to/pg_config install
#   make installcheck             # run the regression suite
#
# You do not need this if you are using the Docker image, which runs it for you.

EXTENSION    = pg_describe
DATA         = sql/pg_describe--1.0.0.sql
MODULE_big   = pg_describe
OBJS         = src/pg_describe.o
REGRESS      = pg_describe
REGRESS_OPTS = --inputdir=test

PG_CONFIG ?= pg_config
PGXS      := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
