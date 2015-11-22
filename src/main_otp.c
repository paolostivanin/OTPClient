#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gcrypt.h>
#include <ctype.h>
#include <glib.h>
#include <cotp.h>
#include "otpclient.h"


struct _b64OutData {
    unsigned char *out;
    size_t out_len;
};


int
main (void)
{
    if (!gcry_check_version ("1.5.0"))
    {
        printf ("libgcrypt required version is 1.5.0 and above\n");
        return -2;
    }
    gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
    
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
    unsigned char *out_enc_text;
    unsigned char *concat_salt_iv_enc;
    char *b64_enc_key;
    
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
        multi_free (salt, iv, NULL);
		return NULL;
	}

	if (gcry_kdf_derive (pwd, strlen (pwd), GCRY_KDF_PBKDF2, GCRY_MD_SHA512, salt, SALT_LEN, 50000, keyLength, derived_crypto_key) != 0)
    {
		fprintf (stderr, "encrypt_token: key derivation error\n");
        multi_free (salt, iv, derived_crypto_key);
		return NULL;
	}
    
	gcry_cipher_setkey (hd, derived_crypto_key, keyLength);
    gcry_cipher_setctr (hd, iv, blkLength);
    
    out_enc_text = gcry_malloc (strlen (plain_token));
    if (out_enc_text == NULL)
    {
        multi_free (salt, iv, derived_crypto_key);
        return NULL;
    }
    gcry_cipher_encrypt (hd, out_enc_text, strlen (plain_token), plain_token, strlen (plain_token));
    
    concat_salt_iv_enc = gcry_malloc (SALT_LEN + blkLength + strlen (plain_token));
    if (concat_salt_iv_enc == NULL)
    {
        multi_free (salt, iv, derived_crypto_key);
        return NULL;
    }
    memcpy (concat_salt_iv_enc, salt, SALT_LEN);
    memcpy (concat_salt_iv_enc+SALT_LEN, iv, blkLength);
    memcpy (concat_salt_iv_enc+SALT_LEN+blkLength, out_enc_text, strlen (plain_token));
      
    multi_free (salt, iv, derived_crypto_key);
    gcry_free (out_enc_text);
    
    b64_enc_key = b64_encode (concat_salt_iv_enc, SALT_LEN + blkLength + strlen (plain_token));
    
    gcry_free (concat_salt_iv_enc);

    return b64_enc_key; // FREE AFTER USE
}


char
*decrypt_token (const char *pwd, const char *encoded_key)
{
    int algo = gcry_cipher_map_name ("aes256");
    size_t blkLength = gcry_cipher_get_algo_blklen (algo);
	size_t keyLength = gcry_cipher_get_algo_keylen (algo);
    
    unsigned char *salt, *iv, *derived_crypto_key;
    unsigned char *enc_text;
    char *decrypted_plain_token;
    
    struct _b64OutData *OutData = b64_decode (encoded_key);
    
    salt = gcry_malloc (SALT_LEN);
    if (salt == NULL)
    {
        free (OutData);
        return NULL;
    }
    
    iv = gcry_malloc (blkLength);
    if (iv == NULL)
    {
        gcry_free (salt);
        free (OutData);
        return NULL;
    }
    
    enc_text = gcry_malloc (OutData->enc_len);
    if (enc_text == NULL)
    {
        multi_free (salt, iv, NULL);
        free (OutData);
        return NULL;
    }
    
    memcpy (salt, OutData->out, SALT_LEN);
    memcpy (iv, OutData->out + SALT_LEN, blkLength);
    memcpy (enc_text, OutData->out + SALT_LEN + blkLength, OutData->enc_len);
    
    gcry_cipher_hd_t hd;
    gcry_cipher_open(&hd, algo, GCRY_CIPHER_MODE_CTR, 0);
	if (((derived_crypto_key = gcry_malloc_secure (keyLength)) == NULL))
    {
		fprintf (stderr, "encrypt_file: memory allocation error\n");
        multi_free (salt, iv, enc_text);
        free (OutData);
		return NULL;
	}

	if (gcry_kdf_derive (pwd, strlen (pwd), GCRY_KDF_PBKDF2, GCRY_MD_SHA512, salt, SALT_LEN, 50000, keyLength, derived_crypto_key) != 0)
    {
		fprintf (stderr, "encrypt_token: key derivation error\n");
        multi_free (salt, iv, derived_crypto_key);
        gcry_free (enc_text);
        free (OutData);
		return NULL;
	}
    
	gcry_cipher_setkey (hd, derived_crypto_key, keyLength);
    gcry_cipher_setctr (hd, iv, blkLength);
    
    decrypted_plain_token = gcry_malloc (OutData->enc_len);
    if (decrypted_token == NULL)
    {
        multi_free (salt, iv, derived_crypto_key);
        gcry_free (enc_text);
        free (OutData);
		return NULL;
    }
    gcry_cipher_decrypt (hd, decrypted_plain_token, OutData->enc_len, enc_text, OutData->enc_len);
    
    if (!g_str_is_ascii (decrypted_plain_token))
    {
        printf ("[E] Decryption failed (wrong password)\n");
        multi_free (salt, iv, derived_crypto_key);
        gcry_free (enc_text);
        free (OutData);
        return NULL;
    }
    
    multi_free (salt, iv, derived_key);
    gcry_free (enc_text);
    free (OutData);
    
    return decrypted_plain_token; // FREE AFTER USE
}


char
*b64_encode (unsigned char *in, size_t in_len)
{
    size_t encoded_text_len = (((in_len/3)+1)*4)+4;

    char *out = gcry_malloc (encoded_text_len);

    out = g_base64_encode (in, in_len);

    return out;
}


struct _b64OutData
*b64_decode (const char *in)
{
    struct _b64OutData *OutData = (struct _b64OutData *) malloc (sizeof (struct _b64OutData));

    OutData->out = g_base64_decode (in, &(OutData->out_len));

    return OutData;
}


void
multi_free (void *buf1, void *buf2, void *buf3)
{
    if (buf1 != NULL)
        gcry_free (buf1);
        
    if (buf2 != NULL)
        gcry_free (buf2);
        
    if (buf3 != NULL)
        gcry_free (buf3);
}
