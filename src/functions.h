#ifndef PG_DESCRIBE_FUNCTIONS_H
#define PG_DESCRIBE_FUNCTIONS_H

#include "postgres.h"
#include "nodes/pg_list.h"

/* Argument facts are booleans; false means no non-null proof. */
extern bool pd_function_result_not_null(Oid function, List *argument_facts);

#endif
