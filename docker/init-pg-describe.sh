#!/bin/bash
# Runs once, on first start of an empty data directory.
#
# Creating the extension in template1 as well means every database created
# afterwards has pg_describe available without a second CREATE EXTENSION --
# convenient when you are trying it out and make yourself a scratch database.
set -euo pipefail

for db in "$POSTGRES_DB" template1; do
    psql --username "$POSTGRES_USER" --dbname "$db" <<-'SQL'
        CREATE EXTENSION IF NOT EXISTS pg_describe;
SQL
done

echo "pg_describe: installed in $POSTGRES_DB and template1"
