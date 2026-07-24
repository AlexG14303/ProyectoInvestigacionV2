#ifndef DB_CONNECTION_H
#define DB_CONNECTION_H
#ifdef __cplusplus
extern "C" {
#endif
const char *cf_db_host_env(void);
const char *cf_db_port_env(void);
const char *cf_db_name_env(void);
#ifdef __cplusplus
}
#endif
#endif
