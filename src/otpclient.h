#ifndef OTPCLIENT_H_INCLUDED
#define OTPCLIENT_H_INCLUDED

#define FILE_PATH ""
#define SALT_LEN 32

char *read_file (char *);
char *decrypt_skey (const char *, const char *);
char *encrypt_skey (const char *, const char *);

#endif
