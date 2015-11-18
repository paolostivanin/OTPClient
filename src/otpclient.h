#ifndef OTPCLIENT_H_INCLUDED
#define OTPCLIENT_H_INCLUDED

#define FILE_PATH ""
#define SALT_LEN 32

char *read_file (const char *);
char *encrypt_token (const char *, char *);
char *decrypt_skey (const char *, const char *);

#endif
