# A PostgreSQL 17 server with pg_describe already built, installed and created.
#
# The server image and the build image are deliberately the SAME image. A
# backend loads an extension by dlopen()ing a shared library out of its own
# $libdir; compiling anywhere else means shipping the .so to exactly the path
# pg_config reports on the target. Building where the server runs makes that
# whole class of problem disappear -- `make install` puts the .so where the
# backend already looks.
#
# The build toolchain is kept in the final image on purpose, so that
# `make installcheck` can be run inside the container against the real server.
# If you want a slimmer image for production, drop the build-essential and
# postgresql-server-dev-17 packages in a second stage and copy across just
# $(pg_config --pkglibdir)/pg_describe.so and $(pg_config --sharedir)/extension/.
FROM postgres:17-bookworm

# postgresql-server-dev-17 provides the server headers, the PGXS makefile
# fragment and the pg_regress test driver. The official postgres image already
# has the PGDG apt repository configured, so this resolves without adding one.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        postgresql-server-dev-17 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/pg_describe

COPY Makefile pg_describe.control ./
COPY src/ ./src/
COPY sql/ ./sql/
COPY test/ ./test/

RUN make && make install

# Runs once, on an empty data directory, when the container first starts.
COPY docker/init-pg-describe.sh /docker-entrypoint-initdb.d/10-pg-describe.sh
