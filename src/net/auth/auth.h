#ifndef NET__AUTH__AUTH_H_
#define NET__AUTH__AUTH_H_

#include <stdlib.h>

int auth_init(void);
int auth_request_register(void);
int auth_get_token(char* dest, size_t* len);

#endif