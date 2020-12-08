/*
 * Crypto wrapper for internal crypto implementation - Cipher wrappers
 * Copyright (c) 2006-2009, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */
/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Hardware crypto support Copyright 2017-2019 Espressif Systems (Shanghai) PTE LTD
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utils/common.h"
#include "utils/includes.h"
#include "crypto.h"
#include "aes.h"

struct crypto_cipher {
	enum crypto_cipher_alg alg;
	union {
		struct {
			size_t used_bytes;
			u8 key[16];
			size_t keylen;
		} rc4;
		struct {
			u8 cbc[32];
			void *ctx_enc;
			void *ctx_dec;
		} aes;
	} u;
};


struct crypto_cipher *  crypto_cipher_init(enum crypto_cipher_alg alg,
					  const u8 *iv, const u8 *key,
					  size_t key_len)
{
    ESP_LOGV("FUNC", "crypto_cipher_init");

	struct crypto_cipher *ctx;

	ctx = (struct crypto_cipher *)os_zalloc(sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->alg = alg;

	switch (alg) {
	case CRYPTO_CIPHER_ALG_RC4:
		if (key_len > sizeof(ctx->u.rc4.key)) {
			os_free(ctx);
			return NULL;
		}
		ctx->u.rc4.keylen = key_len;
		os_memcpy(ctx->u.rc4.key, key, key_len);
		break;
	case CRYPTO_CIPHER_ALG_AES:
		ctx->u.aes.ctx_enc = aes_encrypt_init(key, key_len);
		if (ctx->u.aes.ctx_enc == NULL) {
			os_free(ctx);
			return NULL;
		}
		ctx->u.aes.ctx_dec = aes_decrypt_init(key, key_len);
		if (ctx->u.aes.ctx_dec == NULL) {
			aes_encrypt_deinit(ctx->u.aes.ctx_enc);
			os_free(ctx);
			return NULL;
		}
		os_memcpy(ctx->u.aes.cbc, iv, AES_BLOCK_SIZE);
		break;
	default:
		os_free(ctx);
		return NULL;
	}

	return ctx;
}


int  crypto_cipher_encrypt(struct crypto_cipher *ctx, const u8 *plain,
			  u8 *crypt, size_t len)
{
    ESP_LOGV("FUNC", "crypto_cipher_encrypt");

	size_t i, j, blocks;

	switch (ctx->alg) {
	case CRYPTO_CIPHER_ALG_RC4:
		if (plain != crypt)
			os_memcpy(crypt, plain, len);
		rc4_skip(ctx->u.rc4.key, ctx->u.rc4.keylen,
			 ctx->u.rc4.used_bytes, crypt, len);
		ctx->u.rc4.used_bytes += len;
		break;
	case CRYPTO_CIPHER_ALG_AES:
		if (len % AES_BLOCK_SIZE)
			return -1;
		blocks = len / AES_BLOCK_SIZE;
		for (i = 0; i < blocks; i++) {
			for (j = 0; j < AES_BLOCK_SIZE; j++)
				ctx->u.aes.cbc[j] ^= plain[j];
			aes_encrypt(ctx->u.aes.ctx_enc, ctx->u.aes.cbc,
				    ctx->u.aes.cbc);
			os_memcpy(crypt, ctx->u.aes.cbc, AES_BLOCK_SIZE);
			plain += AES_BLOCK_SIZE;
			crypt += AES_BLOCK_SIZE;
		}
		break;
	default:
		return -1;
	}

	return 0;
}


int  crypto_cipher_decrypt(struct crypto_cipher *ctx, const u8 *crypt,
			  u8 *plain, size_t len)
{
    ESP_LOGV("FUNC", "crypto_cipher_decrypt");

	size_t i, j, blocks;
	u8 tmp[32];

	switch (ctx->alg) {
	case CRYPTO_CIPHER_ALG_RC4:
		if (plain != crypt)
			os_memcpy(plain, crypt, len);
		rc4_skip(ctx->u.rc4.key, ctx->u.rc4.keylen,
			 ctx->u.rc4.used_bytes, plain, len);
		ctx->u.rc4.used_bytes += len;
		break;
	case CRYPTO_CIPHER_ALG_AES:
		if (len % AES_BLOCK_SIZE)
			return -1;
		blocks = len / AES_BLOCK_SIZE;
		for (i = 0; i < blocks; i++) {
			os_memcpy(tmp, crypt, AES_BLOCK_SIZE);
			aes_decrypt(ctx->u.aes.ctx_dec, crypt, plain);
			for (j = 0; j < AES_BLOCK_SIZE; j++)
				plain[j] ^= ctx->u.aes.cbc[j];
			os_memcpy(ctx->u.aes.cbc, tmp, AES_BLOCK_SIZE);
			plain += AES_BLOCK_SIZE;
			crypt += AES_BLOCK_SIZE;
		}
		break;
	default:
		return -1;
	}

	return 0;
}


void  crypto_cipher_deinit(struct crypto_cipher *ctx)
{
    ESP_LOGV("FUNC", "crypto_cipher_deinit");

	switch (ctx->alg) {
	case CRYPTO_CIPHER_ALG_AES:
		aes_encrypt_deinit(ctx->u.aes.ctx_enc);
		aes_decrypt_deinit(ctx->u.aes.ctx_dec);
		break;
	default:
		break;
	}
	os_free(ctx);
}
