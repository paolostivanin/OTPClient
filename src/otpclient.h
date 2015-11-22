#ifndef OTPCLIENT_H_INCLUDED
#define OTPCLIENT_H_INCLUDED

#define FILE_PATH ""
#define SALT_LEN 32

char *read_file (const char *, const char *);
char *encrypt_token (const char *, char *);
char *decrypt_token (const char *, const char *);
void create_enc_file();
char *b64_encode (unsigned char *, size_t);
struct _b64OutData *b64_decode (const char *);
void multi_free (void *, void *, void *);

#endif
