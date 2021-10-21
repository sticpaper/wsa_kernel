// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2021 Google LLC
 *
 * Authors: Elena Petrova <lenaptr@google.com>,
 *          Eric Biggers <ebiggers@google.com>
 *
 * Self-tests of fips140.ko cryptographic functionality.  These are run at
 * module load time to fulfill FIPS 140 and NIAP FPT_TST_EXT.1 requirements.
 *
 * The actual requirements for these self-tests are somewhat vague, but
 * section 9 ("Self-Tests") of the FIPS 140-2 Implementation Guidance document
 * (https://csrc.nist.gov/csrc/media/projects/cryptographic-module-validation-program/documents/fips140-2/fips1402ig.pdf)
 * is somewhat helpful.  Basically, all implementations of all FIPS approved
 * algorithms (including modes of operation) must be tested.  However:
 *
 *   - If an implementation won't be used, it doesn't have to be tested.  So
 *     when multiple implementations of the same algorithm are registered with
 *     the crypto API, we only have to test the default (highest-priority) one.
 *
 *   - There are provisions for skipping tests that are already sufficiently
 *     covered by other tests.  E.g., HMAC-SHA256 may cover SHA-256.
 *
 *   - Only one test vector is required per algorithm, and it can be generated
 *     by any known-good implementation or taken from any official document.
 *
 *   - For ciphers, both encryption and decryption must be tested.
 *
 *   - Only one key size per algorithm needs to be tested.
 *
 * See fips140_selftests[] for the list of tests we've selected.  Currently, all
 * our test vectors except the DRBG ones were generated by the script
 * tools/crypto/gen_fips140_testvecs.py, using the known-good implementations in
 * the Python packages hashlib, pycryptodome, and cryptography.  The DRBG test
 * vectors were manually extracted from
 * https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents/drbg/drbgtestvectors.zip.
 *
 * Note that we don't reuse the upstream crypto API's self-tests
 * (crypto/testmgr.{c,h}), for several reasons:
 *
 *   - To meet FIPS requirements, the self-tests must be located within the FIPS
 *     module boundary (fips140.ko).  But testmgr is integrated into the crypto
 *     API framework and can't be extracted into the module.
 *
 *   - testmgr is much more heavyweight than required for FIPS and NIAP; it
 *     tests more algorithms and does more tests per algorithm, as it's meant to
 *     do proper testing and not just meet certification requirements.  We need
 *     tests that can run with minimal overhead on every boot-up.
 *
 *   - Despite being more heavyweight in general, testmgr doesn't test the
 *     SHA-256 and AES library APIs, despite that being needed here.
 */
#include <crypto/aead.h>
#include <crypto/aes.h>
#include <crypto/drbg.h>
#include <crypto/hash.h>
#include <crypto/internal/cipher.h>
#include <crypto/rng.h>
#include <crypto/sha.h>
#include <crypto/skcipher.h>

#include "fips140-module.h"

/* Test vector for a block cipher algorithm */
struct blockcipher_testvec {
	const u8 *key;
	size_t key_size;
	const u8 *plaintext;
	const u8 *ciphertext;
	size_t block_size;
};

/* Test vector for an AEAD algorithm */
struct aead_testvec {
	const u8 *key;
	size_t key_size;
	const u8 *iv;
	size_t iv_size;
	const u8 *assoc;
	size_t assoc_size;
	const u8 *plaintext;
	size_t plaintext_size;
	const u8 *ciphertext;
	size_t ciphertext_size;
};

/* Test vector for a length-preserving encryption algorithm */
struct skcipher_testvec {
	const u8 *key;
	size_t key_size;
	const u8 *iv;
	size_t iv_size;
	const u8 *plaintext;
	const u8 *ciphertext;
	size_t message_size;
};

/* Test vector for a hash algorithm */
struct hash_testvec {
	const u8 *key;
	size_t key_size;
	const u8 *message;
	size_t message_size;
	const u8 *digest;
	size_t digest_size;
};

/* Test vector for a DRBG algorithm */
struct drbg_testvec {
	const u8 *entropy;
	size_t entropy_size;
	const u8 *pers;
	size_t pers_size;
	const u8 *entpr_a;
	const u8 *entpr_b;
	size_t entpr_size;
	const u8 *add_a;
	const u8 *add_b;
	size_t add_size;
	const u8 *output;
	size_t out_size;
};

/*
 * A struct which specifies an algorithm name (using crypto API syntax), a test
 * function for that algorithm, and a test vector used by that test function.
 */
struct fips_test {
	const char *alg;
	int __must_check (*func)(const struct fips_test *test);
	union {
		struct blockcipher_testvec blockcipher;
		struct aead_testvec aead;
		struct skcipher_testvec skcipher;
		struct hash_testvec hash;
		struct drbg_testvec drbg;
	};
};

/* Maximum IV size (in bytes) among any algorithm tested here */
#define MAX_IV_SIZE	16

static int __init __must_check
fips_check_result(const struct fips_test *test, u8 *result,
		  const u8 *expected_result, size_t result_size,
		  const char *operation)
{
#ifdef CONFIG_CRYPTO_FIPS140_MOD_ERROR_INJECTION
	/* Inject a failure (via corrupting the result) if requested. */
	if (fips140_broken_alg && strcmp(test->alg, fips140_broken_alg) == 0)
		result[0] ^= 0xff;
#endif
	if (memcmp(result, expected_result, result_size) != 0) {
		pr_err("wrong result from %s %s\n", test->alg, operation);
		return -EBADMSG;
	}
	return 0;
}

/*
 * None of the algorithms should be ASYNC, as the FIPS module doesn't register
 * any ASYNC algorithms.  (The ASYNC flag is only declared by hardware
 * algorithms, which would need their own FIPS certification.)
 *
 * Ideally we would verify alg->cra_module == THIS_MODULE here as well, but that
 * doesn't work because the files are compiled as built-in code.
 */
static int __init __must_check
fips_validate_alg(const struct crypto_alg *alg)
{
	if (alg->cra_flags & CRYPTO_ALG_ASYNC) {
		pr_err("unexpectedly got async implementation of %s (%s)\n",
		       alg->cra_name, alg->cra_driver_name);
		return -EINVAL;
	}
	return 0;
}

/* Test a block cipher using the crypto_cipher API. */
static int __init __must_check
fips_test_blockcipher(const struct fips_test *test)
{
	const struct blockcipher_testvec *vec = &test->blockcipher;
	struct crypto_cipher *tfm;
	u8 block[MAX_CIPHER_BLOCKSIZE];
	int err;

	if (WARN_ON(vec->block_size > MAX_CIPHER_BLOCKSIZE))
		return -EINVAL;

	tfm = crypto_alloc_cipher(test->alg, 0, 0);
	if (IS_ERR(tfm)) {
		err = PTR_ERR(tfm);
		pr_err("failed to allocate %s tfm: %d\n", test->alg, err);
		return err;
	}
	err = fips_validate_alg(tfm->base.__crt_alg);
	if (err)
		goto out;
	if (crypto_cipher_blocksize(tfm) != vec->block_size) {
		pr_err("%s has wrong block size\n", test->alg);
		err = -EINVAL;
		goto out;
	}

	err = crypto_cipher_setkey(tfm, vec->key, vec->key_size);
	if (err) {
		pr_err("failed to set %s key: %d\n", test->alg, err);
		goto out;
	}

	/* Encrypt the plaintext, then verify the resulting ciphertext. */
	memcpy(block, vec->plaintext, vec->block_size);
	crypto_cipher_encrypt_one(tfm, block, block);
	err = fips_check_result(test, block, vec->ciphertext, vec->block_size,
				"encryption");
	if (err)
		goto out;

	/* Decrypt the ciphertext, then verify the resulting plaintext. */
	crypto_cipher_decrypt_one(tfm, block, block);
	err = fips_check_result(test, block, vec->plaintext, vec->block_size,
				"decryption");
out:
	crypto_free_cipher(tfm);
	return err;
}

/*
 * Test for plain AES (no mode of operation).  We test this separately from the
 * AES modes because the implementation of AES which is used by the "aes"
 * crypto_cipher isn't necessarily the same as that used by the AES modes such
 * as "ecb(aes)".  Similarly, the aes_{encrypt,decrypt}() library functions may
 * use a different implementation as well, so we test them separately too.
 */
static int __init __must_check
fips_test_aes(const struct fips_test *test)
{
	const struct blockcipher_testvec *vec = &test->blockcipher;
	struct crypto_aes_ctx ctx;
	u8 block[AES_BLOCK_SIZE];
	int err;

	if (WARN_ON(vec->block_size != AES_BLOCK_SIZE))
		return -EINVAL;

	err = fips_test_blockcipher(test);
	if (err)
		return err;

	err = aes_expandkey(&ctx, vec->key, vec->key_size);
	if (err) {
		pr_err("aes_expandkey() failed: %d\n", err);
		return err;
	}
	aes_encrypt(&ctx, block, vec->plaintext);
	err = fips_check_result(test, block, vec->ciphertext, AES_BLOCK_SIZE,
				"encryption (library API)");
	if (err)
		return err;
	aes_decrypt(&ctx, block, block);
	return fips_check_result(test, block, vec->plaintext, AES_BLOCK_SIZE,
				 "decryption (library API)");
}

/* Test a length-preserving symmetric cipher using the crypto_skcipher API. */
static int __init __must_check
fips_test_skcipher(const struct fips_test *test)
{
	const struct skcipher_testvec *vec = &test->skcipher;
	struct crypto_skcipher *tfm;
	struct skcipher_request *req = NULL;
	u8 *message = NULL;
	struct scatterlist sg;
	u8 iv[MAX_IV_SIZE];
	int err;

	if (WARN_ON(vec->iv_size > MAX_IV_SIZE))
		return -EINVAL;

	tfm = crypto_alloc_skcipher(test->alg, 0, 0);
	if (IS_ERR(tfm)) {
		err = PTR_ERR(tfm);
		pr_err("failed to allocate %s tfm: %d\n", test->alg, err);
		return err;
	}
	err = fips_validate_alg(&crypto_skcipher_alg(tfm)->base);
	if (err)
		goto out;
	if (crypto_skcipher_ivsize(tfm) != vec->iv_size) {
		pr_err("%s has wrong IV size\n", test->alg);
		err = -EINVAL;
		goto out;
	}

	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	message = kmemdup(vec->plaintext, vec->message_size, GFP_KERNEL);
	if (!req || !message) {
		err = -ENOMEM;
		goto out;
	}
	sg_init_one(&sg, message, vec->message_size);

	skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP,
				      NULL, NULL);
	skcipher_request_set_crypt(req, &sg, &sg, vec->message_size, iv);

	err = crypto_skcipher_setkey(tfm, vec->key, vec->key_size);
	if (err) {
		pr_err("failed to set %s key: %d\n", test->alg, err);
		goto out;
	}

	/* Encrypt the plaintext, then verify the resulting ciphertext. */
	memcpy(iv, vec->iv, vec->iv_size);
	err = crypto_skcipher_encrypt(req);
	if (err) {
		pr_err("%s encryption failed: %d\n", test->alg, err);
		goto out;
	}
	err = fips_check_result(test, message, vec->ciphertext,
				vec->message_size, "encryption");
	if (err)
		goto out;

	/* Decrypt the ciphertext, then verify the resulting plaintext. */
	memcpy(iv, vec->iv, vec->iv_size);
	err = crypto_skcipher_decrypt(req);
	if (err) {
		pr_err("%s decryption failed: %d\n", test->alg, err);
		goto out;
	}
	err = fips_check_result(test, message, vec->plaintext,
				vec->message_size, "decryption");
out:
	kfree(message);
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	return err;
}

/* Test an AEAD using the crypto_aead API. */
static int __init __must_check
fips_test_aead(const struct fips_test *test)
{
	const struct aead_testvec *vec = &test->aead;
	const int tag_size = vec->ciphertext_size - vec->plaintext_size;
	struct crypto_aead *tfm;
	struct aead_request *req = NULL;
	u8 *assoc = NULL;
	u8 *message = NULL;
	struct scatterlist sg[2];
	int sg_idx = 0;
	u8 iv[MAX_IV_SIZE];
	int err;

	if (WARN_ON(vec->iv_size > MAX_IV_SIZE))
		return -EINVAL;
	if (WARN_ON(vec->ciphertext_size <= vec->plaintext_size))
		return -EINVAL;

	tfm = crypto_alloc_aead(test->alg, 0, 0);
	if (IS_ERR(tfm)) {
		err = PTR_ERR(tfm);
		pr_err("failed to allocate %s tfm: %d\n", test->alg, err);
		return err;
	}
	err = fips_validate_alg(&crypto_aead_alg(tfm)->base);
	if (err)
		goto out;
	if (crypto_aead_ivsize(tfm) != vec->iv_size) {
		pr_err("%s has wrong IV size\n", test->alg);
		err = -EINVAL;
		goto out;
	}

	req = aead_request_alloc(tfm, GFP_KERNEL);
	assoc = kmemdup(vec->assoc, vec->assoc_size, GFP_KERNEL);
	message = kzalloc(vec->ciphertext_size, GFP_KERNEL);
	if (!req || !assoc || !message) {
		err = -ENOMEM;
		goto out;
	}
	memcpy(message, vec->plaintext, vec->plaintext_size);

	sg_init_table(sg, ARRAY_SIZE(sg));
	if (vec->assoc_size)
		sg_set_buf(&sg[sg_idx++], assoc, vec->assoc_size);
	sg_set_buf(&sg[sg_idx++], message, vec->ciphertext_size);

	aead_request_set_ad(req, vec->assoc_size);
	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP, NULL, NULL);

	err = crypto_aead_setkey(tfm, vec->key, vec->key_size);
	if (err) {
		pr_err("failed to set %s key: %d\n", test->alg, err);
		goto out;
	}

	err = crypto_aead_setauthsize(tfm, tag_size);
	if (err) {
		pr_err("failed to set %s authentication tag size: %d\n",
		       test->alg, err);
		goto out;
	}

	/*
	 * Encrypt the plaintext, then verify the resulting ciphertext (which
	 * includes the authentication tag).
	 */
	memcpy(iv, vec->iv, vec->iv_size);
	aead_request_set_crypt(req, sg, sg, vec->plaintext_size, iv);
	err = crypto_aead_encrypt(req);
	if (err) {
		pr_err("%s encryption failed: %d\n", test->alg, err);
		goto out;
	}
	err = fips_check_result(test, message, vec->ciphertext,
				vec->ciphertext_size, "encryption");
	if (err)
		goto out;

	/*
	 * Decrypt the ciphertext (which includes the authentication tag), then
	 * verify the resulting plaintext.
	 */
	memcpy(iv, vec->iv, vec->iv_size);
	aead_request_set_crypt(req, sg, sg, vec->ciphertext_size, iv);
	err = crypto_aead_decrypt(req);
	if (err) {
		pr_err("%s decryption failed: %d\n", test->alg, err);
		goto out;
	}
	err = fips_check_result(test, message, vec->plaintext,
				vec->plaintext_size, "decryption");
out:
	kfree(message);
	kfree(assoc);
	aead_request_free(req);
	crypto_free_aead(tfm);
	return err;
}

/*
 * Test a hash algorithm using the crypto_shash API.
 *
 * Note that we don't need to test the crypto_ahash API too, since none of the
 * hash algorithms in the FIPS module have the ASYNC flag, and thus there will
 * be no hash algorithms that can be accessed only through crypto_ahash.
 */
static int __init __must_check
fips_test_hash(const struct fips_test *test)
{
	const struct hash_testvec *vec = &test->hash;
	struct crypto_shash *tfm;
	u8 digest[HASH_MAX_DIGESTSIZE];
	int err;

	if (WARN_ON(vec->digest_size > HASH_MAX_DIGESTSIZE))
		return -EINVAL;

	tfm = crypto_alloc_shash(test->alg, 0, 0);
	if (IS_ERR(tfm)) {
		err = PTR_ERR(tfm);
		pr_err("failed to allocate %s tfm: %d\n", test->alg, err);
		return err;
	}
	err = fips_validate_alg(&crypto_shash_alg(tfm)->base);
	if (err)
		goto out;
	if (crypto_shash_digestsize(tfm) != vec->digest_size) {
		pr_err("%s has wrong digest size\n", test->alg);
		err = -EINVAL;
		goto out;
	}

	if (vec->key) {
		err = crypto_shash_setkey(tfm, vec->key, vec->key_size);
		if (err) {
			pr_err("failed to set %s key: %d\n", test->alg, err);
			goto out;
		}
	}

	err = crypto_shash_tfm_digest(tfm, vec->message, vec->message_size,
				      digest);
	if (err) {
		pr_err("%s digest computation failed: %d\n", test->alg, err);
		goto out;
	}
	err = fips_check_result(test, digest, vec->digest, vec->digest_size,
				"digest");
out:
	crypto_free_shash(tfm);
	return err;
}

/*
 * Test the sha256() library function, as it may not be covered by the "sha256"
 * crypto_shash, and thus may not be covered by the "hmac(sha256)" test we do.
 */
static int __init __must_check
fips_test_sha256_library(const struct fips_test *test)
{
	const struct hash_testvec *vec = &test->hash;
	u8 digest[SHA256_DIGEST_SIZE];

	if (WARN_ON(vec->digest_size != SHA256_DIGEST_SIZE))
		return -EINVAL;

	sha256(vec->message, vec->message_size, digest);
	return fips_check_result(test, digest, vec->digest, vec->digest_size,
				 "digest (library API)");
}

/* Test a DRBG using the crypto_rng API. */
static int __init __must_check
fips_test_drbg(const struct fips_test *test)
{
	const struct drbg_testvec *vec = &test->drbg;
	struct crypto_rng *rng;
	u8 *output = NULL;
	struct drbg_test_data test_data;
	struct drbg_string addtl, pers, testentropy;
	int err;

	rng = crypto_alloc_rng(test->alg, 0, 0);
	if (IS_ERR(rng)) {
		err = PTR_ERR(rng);
		pr_err("failed to allocate %s tfm: %d\n", test->alg, err);
		return PTR_ERR(rng);
	}
	err = fips_validate_alg(&crypto_rng_alg(rng)->base);
	if (err)
		goto out;

	output = kzalloc(vec->out_size, GFP_KERNEL);
	if (!output) {
		err = -ENOMEM;
		goto out;
	}

	/*
	 * Initialize the DRBG with the entropy and personalization string given
	 * in the test vector.
	 */
	test_data.testentropy = &testentropy;
	drbg_string_fill(&testentropy, vec->entropy, vec->entropy_size);
	drbg_string_fill(&pers, vec->pers, vec->pers_size);
	err = crypto_drbg_reset_test(rng, &pers, &test_data);
	if (err) {
		pr_err("failed to reset %s\n", test->alg);
		goto out;
	}

	/*
	 * Generate some random bytes using the additional data string provided
	 * in the test vector.  Also use the additional entropy if provided
	 * (relevant for the prediction-resistant DRBG variants only).
	 */
	drbg_string_fill(&addtl, vec->add_a, vec->add_size);
	if (vec->entpr_size) {
		drbg_string_fill(&testentropy, vec->entpr_a, vec->entpr_size);
		err = crypto_drbg_get_bytes_addtl_test(rng, output,
						       vec->out_size, &addtl,
						       &test_data);
	} else {
		err = crypto_drbg_get_bytes_addtl(rng, output, vec->out_size,
						  &addtl);
	}
	if (err) {
		pr_err("failed to get bytes from %s (try 1): %d\n",
		       test->alg, err);
		goto out;
	}

	/*
	 * Do the same again, using a second additional data string, and (when
	 * applicable) a second additional entropy string.
	 */
	drbg_string_fill(&addtl, vec->add_b, vec->add_size);
	if (test->drbg.entpr_size) {
		drbg_string_fill(&testentropy, vec->entpr_b, vec->entpr_size);
		err = crypto_drbg_get_bytes_addtl_test(rng, output,
						       vec->out_size, &addtl,
						       &test_data);
	} else {
		err = crypto_drbg_get_bytes_addtl(rng, output, vec->out_size,
						  &addtl);
	}
	if (err) {
		pr_err("failed to get bytes from %s (try 2): %d\n",
		       test->alg, err);
		goto out;
	}

	/* Check that the DRBG generated the expected output. */
	err = fips_check_result(test, output, vec->output, vec->out_size,
				"get_bytes");
out:
	kfree(output);
	crypto_free_rng(rng);
	return err;
}

/* Include the test vectors generated by the Python script. */
#include "fips140-generated-testvecs.h"

/* List of all self-tests.  Keep this in sync with fips140_algorithms[]. */
static const struct fips_test fips140_selftests[] __initconst = {
	/*
	 * Tests for AES and AES modes.
	 *
	 * The full list of AES algorithms we potentially need to test are AES
	 * by itself, AES-CBC, AES-CTR, AES-ECB, AES-GCM, and AES-XTS.  We can
	 * follow the FIPS 140-2 Implementation Guidance (IG) document to try to
	 * reduce this list, but we run into the issue that the architecture-
	 * specific implementations of these algorithms in Linux often don't
	 * share the "same" underlying AES implementation.  E.g., the ARMv8 CE
	 * optimized implementations issue ARMv8 CE instructions directly rather
	 * than going through a separate AES implementation.  In this case,
	 * separate tests are needed according to section 9.2 of the IG.
	 */
	{
		.alg		= "aes",
		.func		= fips_test_aes,
		.blockcipher	= {
			.key		= fips_aes_key,
			.key_size	= sizeof(fips_aes_key),
			.plaintext	= fips_message,
			.ciphertext	= fips_aes_ecb_ciphertext,
			.block_size	= 16,
		}
	}, {
		.alg		= "cbc(aes)",
		.func		= fips_test_skcipher,
		.skcipher	= {
			.key		= fips_aes_key,
			.key_size	= sizeof(fips_aes_key),
			.iv		= fips_aes_iv,
			.iv_size	= sizeof(fips_aes_iv),
			.plaintext	= fips_message,
			.ciphertext	= fips_aes_cbc_ciphertext,
			.message_size	= sizeof(fips_message),
		}
	}, {
		.alg		= "ctr(aes)",
		.func		= fips_test_skcipher,
		.skcipher	= {
			.key		= fips_aes_key,
			.key_size	= sizeof(fips_aes_key),
			.iv		= fips_aes_iv,
			.iv_size	= sizeof(fips_aes_iv),
			.plaintext	= fips_message,
			.ciphertext	= fips_aes_ctr_ciphertext,
			.message_size	= sizeof(fips_message),
		}
	}, {
		.alg		= "ecb(aes)",
		.func		= fips_test_skcipher,
		.skcipher	= {
			.key		= fips_aes_key,
			.key_size	= sizeof(fips_aes_key),
			.plaintext	= fips_message,
			.ciphertext	= fips_aes_ecb_ciphertext,
			.message_size	= sizeof(fips_message)
		}
	}, {
		.alg		= "gcm(aes)",
		.func		= fips_test_aead,
		.aead		= {
			.key		= fips_aes_key,
			.key_size	= sizeof(fips_aes_key),
			.iv		= fips_aes_iv,
			/* The GCM implementation assumes an IV size of 12. */
			.iv_size	= 12,
			.assoc		= fips_aes_gcm_assoc,
			.assoc_size	= sizeof(fips_aes_gcm_assoc),
			.plaintext	= fips_message,
			.plaintext_size	= sizeof(fips_message),
			.ciphertext	= fips_aes_gcm_ciphertext,
			.ciphertext_size = sizeof(fips_aes_gcm_ciphertext),
		}
	}, {
		.alg		= "xts(aes)",
		.func		= fips_test_skcipher,
		.skcipher	= {
			.key		= fips_aes_xts_key,
			.key_size	= sizeof(fips_aes_xts_key),
			.iv		= fips_aes_iv,
			.iv_size	= sizeof(fips_aes_iv),
			.plaintext	= fips_message,
			.ciphertext	= fips_aes_xts_ciphertext,
			.message_size	= sizeof(fips_message),
		}
	/*
	 * Tests for SHA-1, SHA-256, HMAC-SHA256, and SHA-512.
	 *
	 * The selection of these specific tests follows the guidance from
	 * section 9 of the FIPS 140-2 Implementation Guidance (IG) document to
	 * achieve a minimal list of tests, rather than testing all of
	 * SHA-{1,224,256,384,512} and HMAC-SHA{1,224,256,384,512}.  As per the
	 * IG, testing SHA-224 is only required if SHA-256 isn't implemented,
	 * and testing SHA-384 is only required if SHA-512 isn't implemented.
	 * Also, HMAC only has to be tested with one underlying SHA, and the
	 * HMAC test also fulfills the test for its underlying SHA.  That would
	 * result in a test list of e.g. SHA-1, HMAC-SHA256, and SHA-512.
	 *
	 * However we also need to take into account cases where implementations
	 * aren't shared in the "natural" way assumed by the IG.  Currently the
	 * only known exception w.r.t. SHA-* and HMAC-* is the sha256() library
	 * function which may not be covered by the test of the "hmac(sha256)"
	 * crypto_shash.  So, we test sha256() separately.
	 */
	}, {
		.alg		= "sha1",
		.func		= fips_test_hash,
		.hash		= {
			.message	= fips_message,
			.message_size	= sizeof(fips_message),
			.digest		= fips_sha1_digest,
			.digest_size	= sizeof(fips_sha1_digest)
		}
	}, {
		.alg		= "sha256",
		.func		= fips_test_sha256_library,
		.hash		= {
			.message	= fips_message,
			.message_size	= sizeof(fips_message),
			.digest		= fips_sha256_digest,
			.digest_size	= sizeof(fips_sha256_digest)
		}
	}, {
		.alg		= "hmac(sha256)",
		.func		= fips_test_hash,
		.hash		= {
			.key		= fips_hmac_key,
			.key_size	= sizeof(fips_hmac_key),
			.message	= fips_message,
			.message_size	= sizeof(fips_message),
			.digest		= fips_hmac_sha256_digest,
			.digest_size	= sizeof(fips_hmac_sha256_digest)
		}
	}, {
		.alg		= "sha512",
		.func		= fips_test_hash,
		.hash		= {
			.message	= fips_message,
			.message_size	= sizeof(fips_message),
			.digest		= fips_sha512_digest,
			.digest_size	= sizeof(fips_sha512_digest)
		}
	/*
	 * Tests for DRBG algorithms.
	 *
	 * Only the default variant (the one that users get when they request
	 * "stdrng") is required to be tested, as we don't consider the other
	 * variants to be used / usable in the FIPS security policy.  This is
	 * similar to how e.g. we don't test both "xts(aes-generic)" and
	 * "xts-aes-ce" but rather just "xts(aes)".
	 *
	 * Currently the default one is "drbg_nopr_hmac_sha256"; however, just
	 * in case we also test the prediction-resistant enabled variant too.
	 */
	}, {
		.alg	= "drbg_nopr_hmac_sha256",
		.func	= fips_test_drbg,
		.drbg	= {
			.entropy =
				"\xf9\x7a\x3c\xfd\x91\xfa\xa0\x46\xb9\xe6\x1b\x94"
				"\x93\xd4\x36\xc4\x93\x1f\x60\x4b\x22\xf1\x08\x15"
				"\x21\xb3\x41\x91\x51\xe8\xff\x06\x11\xf3\xa7\xd4"
				"\x35\x95\x35\x7d\x58\x12\x0b\xd1\xe2\xdd\x8a\xed",
			.entropy_size = 48,
			.output =
				"\xc6\x87\x1c\xff\x08\x24\xfe\x55\xea\x76\x89\xa5"
				"\x22\x29\x88\x67\x30\x45\x0e\x5d\x36\x2d\xa5\xbf"
				"\x59\x0d\xcf\x9a\xcd\x67\xfe\xd4\xcb\x32\x10\x7d"
				"\xf5\xd0\x39\x69\xa6\x6b\x1f\x64\x94\xfd\xf5\xd6"
				"\x3d\x5b\x4d\x0d\x34\xea\x73\x99\xa0\x7d\x01\x16"
				"\x12\x6d\x0d\x51\x8c\x7c\x55\xba\x46\xe1\x2f\x62"
				"\xef\xc8\xfe\x28\xa5\x1c\x9d\x42\x8e\x6d\x37\x1d"
				"\x73\x97\xab\x31\x9f\xc7\x3d\xed\x47\x22\xe5\xb4"
				"\xf3\x00\x04\x03\x2a\x61\x28\xdf\x5e\x74\x97\xec"
				"\xf8\x2c\xa7\xb0\xa5\x0e\x86\x7e\xf6\x72\x8a\x4f"
				"\x50\x9a\x8c\x85\x90\x87\x03\x9c",
			.out_size = 128,
			.add_a =
				"\x51\x72\x89\xaf\xe4\x44\xa0\xfe\x5e\xd1\xa4\x1d"
				"\xbb\xb5\xeb\x17\x15\x00\x79\xbd\xd3\x1e\x29\xcf"
				"\x2f\xf3\x00\x34\xd8\x26\x8e\x3b",
			.add_b =
				"\x88\x02\x8d\x29\xef\x80\xb4\xe6\xf0\xfe\x12\xf9"
				"\x1d\x74\x49\xfe\x75\x06\x26\x82\xe8\x9c\x57\x14"
				"\x40\xc0\xc9\xb5\x2c\x42\xa6\xe0",
			.add_size = 32,
		}
	}, {
		.alg	= "drbg_pr_hmac_sha256",
		.func	= fips_test_drbg,
		.drbg	= {
			.entropy =
				"\xc7\xcc\xbc\x67\x7e\x21\x66\x1e\x27\x2b\x63\xdd"
				"\x3a\x78\xdc\xdf\x66\x6d\x3f\x24\xae\xcf\x37\x01"
				"\xa9\x0d\x89\x8a\xa7\xdc\x81\x58\xae\xb2\x10\x15"
				"\x7e\x18\x44\x6d\x13\xea\xdf\x37\x85\xfe\x81\xfb",
			.entropy_size = 48,
			.entpr_a =
				"\x7b\xa1\x91\x5b\x3c\x04\xc4\x1b\x1d\x19\x2f\x1a"
				"\x18\x81\x60\x3c\x6c\x62\x91\xb7\xe9\xf5\xcb\x96"
				"\xbb\x81\x6a\xcc\xb5\xae\x55\xb6",
			.entpr_b =
				"\x99\x2c\xc7\x78\x7e\x3b\x88\x12\xef\xbe\xd3\xd2"
				"\x7d\x2a\xa5\x86\xda\x8d\x58\x73\x4a\x0a\xb2\x2e"
				"\xbb\x4c\x7e\xe3\x9a\xb6\x81\xc1",
			.entpr_size = 32,
			.output =
				"\x95\x6f\x95\xfc\x3b\xb7\xfe\x3e\xd0\x4e\x1a\x14"
				"\x6c\x34\x7f\x7b\x1d\x0d\x63\x5e\x48\x9c\x69\xe6"
				"\x46\x07\xd2\x87\xf3\x86\x52\x3d\x98\x27\x5e\xd7"
				"\x54\xe7\x75\x50\x4f\xfb\x4d\xfd\xac\x2f\x4b\x77"
				"\xcf\x9e\x8e\xcc\x16\xa2\x24\xcd\x53\xde\x3e\xc5"
				"\x55\x5d\xd5\x26\x3f\x89\xdf\xca\x8b\x4e\x1e\xb6"
				"\x88\x78\x63\x5c\xa2\x63\x98\x4e\x6f\x25\x59\xb1"
				"\x5f\x2b\x23\xb0\x4b\xa5\x18\x5d\xc2\x15\x74\x40"
				"\x59\x4c\xb4\x1e\xcf\x9a\x36\xfd\x43\xe2\x03\xb8"
				"\x59\x91\x30\x89\x2a\xc8\x5a\x43\x23\x7c\x73\x72"
				"\xda\x3f\xad\x2b\xba\x00\x6b\xd1",
			.out_size = 128,
			.add_a =
				"\x18\xe8\x17\xff\xef\x39\xc7\x41\x5c\x73\x03\x03"
				"\xf6\x3d\xe8\x5f\xc8\xab\xe4\xab\x0f\xad\xe8\xd6"
				"\x86\x88\x55\x28\xc1\x69\xdd\x76",
			.add_b =
				"\xac\x07\xfc\xbe\x87\x0e\xd3\xea\x1f\x7e\xb8\xe7"
				"\x9d\xec\xe8\xe7\xbc\xf3\x18\x25\x77\x35\x4a\xaa"
				"\x00\x99\x2a\xdd\x0a\x00\x50\x82",
			.add_size = 32,
			.pers =
				"\xbc\x55\xab\x3c\xf6\x52\xb0\x11\x3d\x7b\x90\xb8"
				"\x24\xc9\x26\x4e\x5a\x1e\x77\x0d\x3d\x58\x4a\xda"
				"\xd1\x81\xe9\xf8\xeb\x30\x8f\x6f",
			.pers_size = 32,
		}
	}
};

bool __init fips140_run_selftests(void)
{
	int i;

	pr_info("running self-tests\n");
	for (i = 0; i < ARRAY_SIZE(fips140_selftests); i++) {
		const struct fips_test *test = &fips140_selftests[i];
		int err;

		err = test->func(test);
		if (err) {
			pr_emerg("self-tests failed for algorithm %s: %d\n",
				 test->alg, err);
			/* The caller is responsible for calling panic(). */
			return false;
		}
	}
	pr_info("all self-tests passed\n");
	return true;
}
