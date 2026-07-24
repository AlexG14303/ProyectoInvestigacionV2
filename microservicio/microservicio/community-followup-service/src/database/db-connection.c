#include "db-connection.h"
const char *cf_db_host_env(void) { return "POSTGREST_HOST"; }
const char *cf_db_port_env(void) { return "POSTGREST_PORT"; }
const char *cf_db_name_env(void) { return "FOLLOWUP_TABLE"; }
