/*
 * Crypto wrapper for internal crypto implementation
 * Copyright (c) 2006-2011, Jouni Malinen <j@w1.fi>
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

#include "utils/includes.h"
#include "utils/common.h"
#include "crypto.h"
#include "sha1_i.h"
#include "md5_i.h"

struct crypto_hash {
	enum crypto_hash_alg alg;
	union {
		struct MD5Context md5;
		struct SHA1Context sha1;
	} u;
	u8 key[64];
	size_t key_len;
};


struct crypto_hash *  crypto_hash_init(enum crypto_hash_alg alg, const u8 *key,
				      size_t key_len)
{
    ESP_LOGV("FUNC", "crypto_hash_init");

	struct crypto_hash *ctx;
	u8 k_pad[64];
	u8 tk[32];
	size_t i;

	ctx = (struct crypto_hash *)os_zalloc(sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->alg = alg;

	switch (alg) {
	case CRYPTO_HASH_ALG_MD5:
		MD5Init(&ctx->u.md5);
		break;
	case CRYPTO_HASH_ALG_SHA1:
		SHA1Init(&ctx->u.sha1);
		break;
	case CRYPTO_HASH_ALG_HMAC_MD5:
		if (key_len > sizeof(k_pad)) {
			MD5Init(&ctx->u.md5);
			MD5Update(&ctx->u.md5, key, key_len);
			MD5Final(tk, &ctx->u.md5);
			key = tk;
			key_len = 16;
		}
		os_memcpy(ctx->key, key, key_len);
		ctx->key_len = key_len;

		os_memcpy(k_pad, key, key_len);
		if (key_len < sizeof(k_pad))
			os_memset(k_pad + key_len, 0, sizeof(k_pad) - key_len);
		for (i = 0; i < sizeof(k_pad); i++)
			k_pad[i] ^= 0x36;
		MD5Init(&ctx->u.md5);
		MD5Update(&ctx->u.md5, k_pad, sizeof(k_pad));
		break;
	case CRYPTO_HASH_ALG_HMAC_SHA1:
		if (key_len > sizeof(k_pad)) {
			SHA1Init(&ctx->u.sha1);
			SHA1Update(&ctx->u.sha1, key, key_len);
			SHA1Final(tk, &ctx->u.sha1);
			key = tk;
			key_len = 20;
		}
		os_memcpy(ctx->key, key, key_len);
		ctx->key_len = key_len;

		os_memcpy(k_pad, key, key_len);
		if (key_len < sizeof(k_pad))
			os_memset(k_pad + key_len, 0, sizeof(k_pad) - key_len);
		for (i = 0; i < sizeof(k_pad); i++)
			k_pad[i] ^= 0x36;
		SHA1Init(&ctx->u.sha1);
		SHA1Update(&ctx->u.sha1, k_pad, sizeof(k_pad));
		break;
	default:
		os_free(ctx);
		return NULL;
	}

	return ctx;
}


void  crypto_hash_update(struct crypto_hash *ctx, const u8 *data, size_t len)
{
    ESP_LOGV("FUNC", "crypto_hash_update");

	if (ctx == NULL)
		return;

	switch (ctx->alg) {
	case CRYPTO_HASH_ALG_MD5:
	case CRYPTO_HASH_ALG_HMAC_MD5:
		MD5Update(&ctx->u.md5, data, len);
		break;
	case CRYPTO_HASH_ALG_SHA1:
	case CRYPTO_HASH_ALG_HMAC_SHA1:
		SHA1Update(&ctx->u.sha1, data, len);
		break;
	default:
		break;
	}
}


int  crypto_hash_finish(struct crypto_hash *ctx, u8 *mac, size_t *len)
{
    ESP_LOGV("FUNC", "crypto_hash_finish");

	u8 k_pad[64];
	size_t i;

	if (ctx == NULL)
		return -2;

	if (mac == NULL || len == NULL) {
		os_free(ctx);
		return 0;
	}

	switch (ctx->alg) {
	case CRYPTO_HASH_ALG_MD5:
		if (*len < 16) {
			*len = 16;
			os_free(ctx);
			return -1;
		}
		*len = 16;
		MD5Final(mac, &ctx->u.md5);
		break;
	case CRYPTO_HASH_ALG_SHA1:
		if (*len < 20) {
			*len = 20;
			os_free(ctx);
			return -1;
		}
		*len = 20;
		SHA1Final(mac, &ctx->u.sha1);
		break;
	case CRYPTO_HASH_ALG_HMAC_MD5:
		if (*len < 16) {
			*len = 16;
			os_free(ctx);
			return -1;
		}
		*len = 16;

		MD5Final(mac, &ctx->u.md5);

		os_memcpy(k_pad, ctx->key, ctx->key_len);
		os_memset(k_pad + ctx->key_len, 0,
			  sizeof(k_pad) - ctx->key_len);
		for (i = 0; i < sizeof(k_pad); i++)
			k_pad[i] ^= 0x5c;
		MD5Init(&ctx->u.md5);
		MD5Update(&ctx->u.md5, k_pad, sizeof(k_pad));
		MD5Update(&ctx->u.md5, mac, 16);
		MD5Final(mac, &ctx->u.md5);
		break;
	case CRYPTO_HASH_ALG_HMAC_SHA1:
		if (*len < 20) {
			*len = 20;
			os_free(ctx);
			return -1;
		}
		*len = 20;

		SHA1Final(mac, &ctx->u.sha1);

		os_memcpy(k_pad, ctx->key, ctx->key_len);
		os_memset(k_pad + ctx->key_len, 0,
			  sizeof(k_pad) - ctx->key_len);
		for (i = 0; i < sizeof(k_pad); i++)
			k_pad[i] ^= 0x5c;
		SHA1Init(&ctx->u.sha1);
		SHA1Update(&ctx->u.sha1, k_pad, sizeof(k_pad));
		SHA1Update(&ctx->u.sha1, mac, 20);
		SHA1Final(mac, &ctx->u.sha1);
		break;
	default:
		os_free(ctx);
		return -1;
	}

	os_free(ctx);

	return 0;
}


int  crypto_global_init(void)
{
    ESP_LOGV("FUNC", "crypto_global_init");

	return 0;
}


void  crypto_global_deinit(void)
{
    ESP_LOGV("FUNC", "crypto_global_deinit");

}
