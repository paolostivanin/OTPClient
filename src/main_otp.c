#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gcrypt.h>
#include <ctype.h>
#include <cotp.h>
#include "otpclient.h"


int
main (void)
{
    size_t i;
    char *account_name_from_user;
    char buffer[128];
    char *user_pwd;
    
    printf ("Account name: ");
    fgets (buffer, sizeof (buffer), stdin);
    account_name_from_user = malloc (strlen (buffer)+1);
    if (account_name_from_user == NULL)
    {
        fprintf (stderr, "[!] ERROR during memory allocation\n");
        return -1;
    }
    strncpy (account_name_from_user, buffer, strlen(buffer)-1); // -1 because we delete the \n char
    for (i=0; i<strlen(account_name_from_user); i++)
        account_name_from_user[i] = tolower(account_name_from_user[i]);
    
    printf ("%s\n", account_name_from_user);
    char *enc_key = read_file (account_name_from_user);
    if (enc_key == NULL)
    {
        fprintf (stderr, "[!] ERROR something went wrong\n");
        free (account_name_from_user);
        return -1;
    }
    
    // TODO: read password from user
    // TODO: gcry_malloc_secure pwd user
    char *sec_key = decrypt_skey (user_pwd, enc_key);
    // TODO: zeroing out pwd
    // TODO: zeroing out sec_key
    
    free (enc_key);
    gcry_free (user_pwd);
    gcry_free (sec_key);
    free (account_name_from_user);
    
    return 0;
}


char
*read_file (const char *account_name)
{
    size_t i;
    char *ac_nm;
    char *enc_key_tk;
    char *e_key;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    FILE *fp = fopen (FILE_PATH, "r");
    if (fp == NULL)
    {
        fprintf (stderr, "[!] ERROR opening file %s\n", FILE_PATH);
        return NULL;
    }
    while ((read = getline (&line, &len, fp)) != -1)
    {
        ac_nm = strtok (line, " ");
        if (strcmp (ac_nm, account_name) == 0)
        {
            enc_key_tk = strtok (NULL, " ");
            e_key = gcry_malloc_secure (strlen (enc_key_tk)+1);
            if (e_key == NULL)
            {
                fprintf (stderr, "[!] ERROR during memory allocation\n");
                free (line);
                return NULL;
            }
            for (i = 0; i < strlen (enc_key_tk); i++)
            {
                if (enc_key_tk[i] == '\n')
                    enc_key_tk[i] = '\0';
            }
            strncpy (e_key, enc_key_tk, strlen (enc_key_tk));
            memset (enc_key_tk, 0, sizeof (enc_key_tk));
            free (line);
            return e_key;
        }
    }      
    free (line);
    return NULL;
} 


char
*encrypt_token (const char *pwd, char *plain_token)
{
    int algo = gcry_cipher_map_name ("aes256");
    size_t blkLength = gcry_cipher_get_algo_blklen (algo);
	size_t keyLength = gcry_cipher_get_algo_keylen (algo);
    
    unsigned char *salt;
    unsigned char *iv;
    unsigned char *derived_crypto_key;
    
    salt = gcry_malloc (SALT_LEN);
    if (salt == NULL)
    {
        return NULL;
    }
    
    iv = gcry_malloc (blkLength);
    if (iv == NULL)
    {
        gcry_free (salt);
        return NULL;
    }
    
    gcry_create_nonce (salt, SALT_LEN);
    gcry_create_nonce (iv, blkLength);

    gcry_cipher_hd_t hd;
    gcry_cipher_open(&hd, algo, GCRY_CIPHER_MODE_CTR, 0);
	if (((derived_crypto_key = gcry_malloc_secure (keyLength)) == NULL))
    {
		fprintf (stderr, "encrypt_file: memory allocation error\n");
        gcry_free (salt);
        gcry_free (iv);
		return NULL;
	}

	if (gcry_kdf_derive (pwd, strlen (pwd), GCRY_KDF_PBKDF2, GCRY_MD_SHA512, salt, SALT_LEN, 50000, keyLength, derived_crypto_key) != 0)
    {
		fprintf (stderr, "encrypt_token: key derivation error\n");
        gcry_free (salt);
        gcry_free (iv);
        gcry_free (derived_crypto_key);
		return NULL;
	}
    
	gcry_cipher_setkey (hd, derived_crypto_key, keyLength);
    gcry_cipher_setctr (hd, iv, blkLength);
    
    gcry_cipher_encrypt (hd, ...);
       
    /* 1) encrypt e_key
     * 2) concat (salt,iv,enc)
     * 3) b64(4)
     */
     
     gcry_free (salt);
     gcry_free (iv);
     gcry_free (derived_crypto_key);
     
     return b64_enc_key
}


char
*decrypt_skey (const char *pwd, const char *e_key)
{
    /* 1) b64 decode e_key
     * 2) split key (salt+iv+key)
     * 3) use pwd to decrypt e_key
     */ 
}
