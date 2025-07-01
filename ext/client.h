#ifndef DM_CLIENT_H
#define DM_CLIENT_H

typedef struct {
  VALUE encoding;
  VALUE active_fiber; /* rb_fiber_current() or Qnil */
  long server_version;
  int reconnect_enabled;
  unsigned int connect_timeout;
  int active;
  int initialized;
  int refcount;
  int closed;
  int is_select;
  int encode_code;
  sdbyte lastrowid[20];
  sdint8 affected_rows;
  dhenv env;
  dhcon client;
  dhstmt stmt;
} dm_client_wrapper;

void rb_dm_set_server_query_flags(dhcon client, VALUE result);

extern const rb_data_type_t rb_dm_client_type;

#ifdef NEW_TYPEDDATA_WRAPPER
#define GET_CLIENT(self) \
  dm_client_wrapper *wrapper; \
  TypedData_Get_Struct(self, dm_client_wrapper, &rb_dm_client_type, wrapper);
#else
#define GET_CLIENT(self) \
  dm_client_wrapper *wrapper; \
  Data_Get_Struct(self, dm_client_wrapper, wrapper);
#endif

void init_dm_client(void);
void decr_dm_client(dm_client_wrapper *wrapper);

#endif