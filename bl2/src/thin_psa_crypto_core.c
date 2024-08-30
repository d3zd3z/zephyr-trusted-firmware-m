/*
 * Copyright (c) 2024, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
/**
 * \note This source file is derivative work of psa_crypto.c from the Mbed TLS project
 */
#include <assert.h>
#include <string.h>
#include <stdbool.h>

/* This module includes the driver_wrappers which assumes that private access to the
 * fields of implementation structures is enabled through the following defined macro
 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "psa/crypto.h"
#include "psa_crypto_driver_wrappers.h"
#include "psa_crypto_driver_wrappers_no_static.h"
#if defined(BLX_BUILTIN_KEY_LOADER)
#include "tfm_plat_crypto_keys.h"
#include "tfm_plat_otp.h"
#endif /* BLX_BUILTIN_KEY_LOADER */

/* For mbedtls_psa_ecdsa_verify_hash() */
#include "psa_crypto_ecp.h"

/* A few Mbed TLS definitions */
#include "mbedtls/entropy.h"
#include "mbedtls/ecp.h"

/**
 * \brief Aligns a value x up to an alignment a.
 */
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/**
 * @note The assumption is that key import will happen just
 *       before the key is used during bootloading stages,
 *       hence the key management system is simplified to
 *       just hold a pointer to the key
 */

#if defined(BLX_BUILTIN_KEY_LOADER)
/**
 * @brief Static local buffer that holds enough data for the key material
 *        provisioned bundle to be retrieved from the platform.
 *
 * @note  The buffer is aligned on 4-byte words to match the requirements on the
 *        alignment for the underlying OTP memory
 */
#if PSA_WANT_ECC_SECP_R1_384 == 1
static uint32_t g_pubkey_data[ALIGN(PSA_EXPORT_PUBLIC_KEY_OUTPUT_SIZE(PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1), 384), 4)];
#elif PSA_WANT_ECC_SECP_R1_256 == 1
static uint32_t g_pubkey_data[ALIGN(PSA_EXPORT_PUBLIC_KEY_OUTPUT_SIZE(PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1), 256), 4)];
#endif
#endif /* BLX_BUILTIN_KEY_LOADER */

/**
 * @brief A structure describing the contents of a thin key slot, which
 *        holds key material and metadata following a psa_import_key() call
 */
struct thin_key_slot_s {
    uint8_t *buf;              /*!< Pointer to the buffer holding the key material */
    size_t len;                /*!< Size in bytes of the \a buf buffer */
    psa_key_attributes_t attr; /*!< Attributes of the key */
#if !defined(BLX_BUILTIN_KEY_LOADER)
    psa_key_id_t key_id;       /*!< Key ID assigned to the key */
    bool is_valid;             /*!< Boolean value, true if the key material is valid */
#endif
};

/**
 * @brief This static global variable holds the key slot. The thin PSA Crypto core
 *        supports only a single key to be imported at any given time. Importing a
 *        new key will just cause the existing key to be forgotten
 */
static struct thin_key_slot_s g_key_slot =  {
#if !defined(BLX_BUILTIN_KEY_LOADER)
    .buf = NULL,
    .len = 0,
    .attr = PSA_KEY_ATTRIBUTES_INIT,
    .key_id = PSA_KEY_ID_NULL,
    .is_valid = false,
#else
    .buf  = (uint8_t *)g_pubkey_data,
    .len  = sizeof(g_pubkey_data),
    .attr = PSA_KEY_ATTRIBUTES_INIT,
#endif /* !BLX_BUILTIN_KEY_LOADER */
};

/**
 * @brief Context required by the RNG function, mocked
 *
 */
static mbedtls_psa_external_random_context_t *g_ctx = NULL;

#if defined(BLX_BUILTIN_KEY_LOADER)
static psa_status_t get_builtin_key(psa_key_id_t key_id)
{

    const tfm_plat_builtin_key_descriptor_t *desc_table = NULL;
    size_t number_of_keys = tfm_plat_builtin_key_get_desc_table_ptr(&desc_table);
    size_t idx;

    for (idx = 0; idx < number_of_keys; idx++) {
        if (desc_table[idx].key_id == key_id) {
            psa_key_bits_t key_bits;
            psa_algorithm_t alg;
            psa_key_type_t key_type;
            enum tfm_plat_err_t plat_err;

            /* Load key */
            plat_err = desc_table[idx].loader_key_func(
                g_key_slot.buf, sizeof(g_pubkey_data), &g_key_slot.len,
                &key_bits, &alg, &key_type);
            if (plat_err != TFM_PLAT_ERR_SUCCESS) {
                return PSA_ERROR_GENERIC_ERROR;
            }

            /* Set metadata */
            psa_set_key_algorithm(&g_key_slot.attr, alg);
            psa_set_key_type(&g_key_slot.attr, key_type);
            psa_set_key_bits(&g_key_slot.attr, key_bits);
            break;
        }
    }

    if (idx == number_of_keys) {
        return PSA_ERROR_DOES_NOT_EXIST;
    }

    /* Set policy */
    const tfm_plat_builtin_key_policy_t *policy_table = NULL;
    (void)tfm_plat_builtin_key_get_policy_table_ptr(&policy_table);
    psa_set_key_usage_flags(&g_key_slot.attr, policy_table[idx].usage);

    return PSA_SUCCESS;
}
#endif /* BLX_BUILTIN_KEY_LOADER */

/*!
 * \defgroup thin_psa_crypto Set of functions implementing a thin PSA Crypto core
 *                           with the bare minimum set of APIs required for
 *                           bootloading use cases.
 */
/*!@{*/
psa_status_t psa_crypto_init(void)
{
    psa_status_t status = psa_driver_wrapper_init();

    /* This will have to perform RNG/DRBG init in case that will be ever required by
     * any API
     */
    return status;
}

psa_status_t psa_hash_abort(psa_hash_operation_t *operation)
{
    /* Aborting a non-active operation is allowed */
    if (operation->id == 0) {
        return PSA_SUCCESS;
    }

    psa_status_t status = psa_driver_wrapper_hash_abort(operation);

    operation->id = 0;

    return status;
}

psa_status_t psa_hash_setup(psa_hash_operation_t *operation,
                            psa_algorithm_t alg)
{
    psa_status_t status;

    /* A context must be freshly initialized before it can be set up. */
    assert(operation->id == 0);
    assert(PSA_ALG_IS_HASH(alg));

    /* Ensure all of the context is zeroized, since PSA_HASH_OPERATION_INIT only
     * directly zeroes the int-sized dummy member of the context union.
     */
    memset(&operation->ctx, 0, sizeof(operation->ctx));

    status = psa_driver_wrapper_hash_setup(operation, alg);

    if (status != PSA_SUCCESS) {
        psa_hash_abort(operation);
    }

    return status;
}

psa_status_t psa_hash_update(psa_hash_operation_t *operation,
                             const uint8_t *input,
                             size_t input_length)
{
    psa_status_t status;

    assert(operation->id != 0);

    /* Don't require hash implementations to behave correctly on a
     * zero-length input, which may have an invalid pointer.
     */
    if (input_length == 0) {
        return PSA_SUCCESS;
    }

    status = psa_driver_wrapper_hash_update(operation, input, input_length);

    if (status != PSA_SUCCESS) {
        psa_hash_abort(operation);
    }

    return status;
}

psa_status_t psa_hash_finish(psa_hash_operation_t *operation,
                             uint8_t *hash,
                             size_t hash_size,
                             size_t *hash_length)
{
    *hash_length = 0;
    assert(operation->id != 0);

    psa_status_t status = psa_driver_wrapper_hash_finish(
        operation, hash, hash_size, hash_length);
    psa_hash_abort(operation);
    return status;
}

#if !defined(BLX_BUILTIN_KEY_LOADER)
/**
 * The key management subsystem is simplified to support only the key encodings
 * as expected by MCUboot. MCUboot key bundles can be encoded in the
 * SubjectPublicKeyInfo format (RFC 5480):
 *
 * SubjectPublicKeyInfo  ::= SEQUENCE  {
 *     algorithm            AlgorithmIdentifier,
 *     subjectPublicKey     BIT STRING
 * }
 *
 * where, for RSA, the subjectPublicKey is either specified in RFC 3447 (PKCS#1v2.1)
 * or the newest RFC 8017 (PKCS#1v2.2) using RSAPublicKey:
 *
 * RSAPublicKey ::= SEQUENCE {
 *     modulus           INTEGER,  -- n
 *     publicExponent    INTEGER   -- e
 * }
 *
 * or for ECDSA is specified in RFC 5480 itself as ECPoint ::= OCTET STRING
 *
 * For ECDSA, MCUboot passes the uncompressed format (i.e. 0x04 X Y), i.e. the
 * key encoding is parsed by MCUboot itself before being imported. For RSA, the
 * AlgorithmIdentifier is instead specified in RFC 3279 as the value of the OID
 * rsaEncryption: 1.2.840.113549.1.1.1, but MCUboot chooses in this case to
 * pass already the RSAPublicKey structure to the APIs, hence the code below just
 * understands the length of n by inspecting the fields of the ASN.1 encoding
 *
 */
psa_status_t psa_import_key(const psa_key_attributes_t *attributes,
                            const uint8_t *data,
                            size_t data_length,
                            psa_key_id_t *key)
{
#if PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY == 1
    /* This is either a 2048, 3072 or 4096 bit RSA key, hence the TLV must place
     * the length at index (6,7) with a leading 0x00. The leading 0x00 is due to
     * the fact that the MSB will always be set for RSA keys where the length is
     * a multiple of 8 bits.
     */
    const size_t bits =
        PSA_BYTES_TO_BITS((((uint16_t)data[6]) << 8) | (uint16_t)data[7]) - 8;

#elif PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY == 1
    /* The public key is expected in uncompressed format, i.e. 0x04 X Y
     * for 256 or 384 bit lengths, and the driver wrappers expect to receive
     * it in that format
     */
    assert(data[0] == 0x04);
    const size_t bits = PSA_BYTES_TO_BITS((data_length - 1)/2);
#endif

    g_key_slot.buf = (uint8_t *)data;
    g_key_slot.len = data_length;

    memcpy(&g_key_slot.attr, attributes, sizeof(psa_key_attributes_t));
    g_key_slot.attr.bits = (psa_key_bits_t)bits;

    /* This signals that a new key has been imported */
    *key = (++g_key_slot.key_id);

    g_key_slot.is_valid = true;

    return PSA_SUCCESS;
}

psa_status_t psa_get_key_attributes(psa_key_id_t key,
                                    psa_key_attributes_t *attributes)
{
    assert(g_key_slot.is_valid && (g_key_slot.key_id == key));
    memcpy(attributes, &g_key_slot.attr, sizeof(psa_key_attributes_t));
    return PSA_SUCCESS;
}
#endif /* !BLX_BUILTIN_KEY_LOADER */

psa_status_t psa_destroy_key(psa_key_id_t key)
{
#if !defined(BLX_BUILTIN_KEY_LOADER)
    assert(g_key_slot.is_valid && (g_key_slot.key_id == key));

    g_key_slot.buf = NULL;
    g_key_slot.len = 0;
    g_key_slot.attr = psa_key_attributes_init();
    g_key_slot.is_valid = false;

    /* This will keep the value of the key_id so that a new import will
     * just use the next key_id. This allows to keep track of potential
     * clients trying to reuse a deleted key ID
     */
#else
    memset(g_pubkey_data, 0, sizeof(g_pubkey_data));
    g_key_slot.len = 0;
    g_key_slot.attr = psa_key_attributes_init();
#endif /* !BLX_BUILTIN_KEY_LOADER */

    return PSA_SUCCESS;
}

/* Signature verification supports only RSA or ECDSA with P256 or P384 */
psa_status_t psa_verify_hash(psa_key_id_t key,
                             psa_algorithm_t alg,
                             const uint8_t *hash,
                             size_t hash_length,
                             const uint8_t *signature,
                             size_t signature_length)
{
    psa_status_t status;

#if !defined(BLX_BUILTIN_KEY_LOADER)
    assert(g_key_slot.is_valid && (g_key_slot.key_id == key));
#else
    status = get_builtin_key(key);
    if (status != PSA_SUCCESS) {
        return status;
    }
#endif /* !BLX_BUILTIN_KEY_LOADER */

    status = psa_driver_wrapper_verify_hash(
                &g_key_slot.attr,
                g_key_slot.buf, g_key_slot.len,
                alg, hash, hash_length,
                signature, signature_length);

    return status;
}

psa_status_t mbedtls_to_psa_error(int ret)
{
    /* We don't require precise error translation */
    if (!ret) {
        return PSA_SUCCESS;
    } else {
        return PSA_ERROR_GENERIC_ERROR;
    }
}

#if defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG)
int mbedtls_psa_get_random(void *p_rng,
                           unsigned char *output,
                           size_t output_size)
{
    /* This function takes a pointer to the RNG state because that's what
     * classic mbedtls functions using an RNG expect. The PSA RNG manages
     * its own state internally and doesn't let the caller access that state.
     * So we just ignore the state parameter, and in practice we'll pass
     * NULL.
     */
    (void) p_rng;
    psa_status_t status = psa_generate_random(output, output_size);

    if (status == PSA_SUCCESS) {
        return 0;
    } else {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
}
#endif /* MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG */

psa_status_t psa_generate_random(uint8_t *output,
                                 size_t output_size)
{
#if defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG)

    size_t output_length = 0;
    psa_status_t status = mbedtls_psa_external_get_random(g_ctx,
                                                          output, output_size,
                                                          &output_length);
    if (status != PSA_SUCCESS) {
        return status;
    }
    /* Breaking up a request into smaller chunks is currently not supported
     * for the external RNG interface.
     */
    if (output_length != output_size) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    return PSA_SUCCESS;

#endif
    return PSA_ERROR_NOT_SUPPORTED;
}

/* This gets linked by the driver wrapper if no driver is present */
psa_status_t psa_verify_hash_builtin(
    const psa_key_attributes_t *attributes,
    const uint8_t *key_buffer, size_t key_buffer_size,
    psa_algorithm_t alg, const uint8_t *hash, size_t hash_length,
    const uint8_t *signature, size_t signature_length)
{
#if PSA_WANT_ALG_RSA_PSS == 1
    if (PSA_KEY_TYPE_IS_RSA(psa_get_key_type(attributes))) {
        if (PSA_ALG_IS_RSA_PKCS1V15_SIGN(alg) ||
            PSA_ALG_IS_RSA_PSS(alg)) {
#if defined(MBEDTLS_PSA_BUILTIN_ALG_RSA_PSS)
            return mbedtls_psa_rsa_verify_hash(
                attributes,
                key_buffer, key_buffer_size,
                alg, hash, hash_length,
                signature, signature_length);
#endif /* defined(MBEDTLS_PSA_BUILTIN_ALG_RSA_PSS) */
        } else {
            return PSA_ERROR_INVALID_ARGUMENT;
        }
    }
#elif PSA_WANT_ALG_ECDSA == 1
    if (PSA_KEY_TYPE_IS_ECC(psa_get_key_type(attributes))) {
        if (PSA_ALG_IS_ECDSA(alg)) {
#if defined(MBEDTLS_PSA_BUILTIN_ALG_ECDSA) || \
            defined(MBEDTLS_PSA_BUILTIN_ALG_DETERMINISTIC_ECDSA)
            return mbedtls_psa_ecdsa_verify_hash(
                attributes,
                key_buffer, key_buffer_size,
                alg, hash, hash_length,
                signature, signature_length);
#endif /* defined(MBEDTLS_PSA_BUILTIN_ALG_ECDSA) ||
        * defined(MBEDTLS_PSA_BUILTIN_ALG_DETERMINISTIC_ECDSA)
        */
        } else {
            return PSA_ERROR_INVALID_ARGUMENT;
        }
    }
#endif

    (void) key_buffer;
    (void) key_buffer_size;
    (void) hash;
    (void) hash_length;
    (void) signature;
    (void) signature_length;

    return PSA_ERROR_NOT_SUPPORTED;
}

/* Required when Mbed TLS backend converts from PSA to Mbed TLS native */
mbedtls_ecp_group_id mbedtls_ecc_group_from_psa(psa_ecc_family_t family,
                                                size_t bits)
{
    switch (family) {
    case PSA_ECC_FAMILY_SECP_R1:
        switch (bits) {
#if defined(PSA_WANT_ECC_SECP_R1_192)
        case 192:
            return MBEDTLS_ECP_DP_SECP192R1;
#endif
#if defined(PSA_WANT_ECC_SECP_R1_224)
        case 224:
            return MBEDTLS_ECP_DP_SECP224R1;
#endif
#if defined(PSA_WANT_ECC_SECP_R1_256)
        case 256:
            return MBEDTLS_ECP_DP_SECP256R1;
#endif
#if defined(PSA_WANT_ECC_SECP_R1_384)
        case 384:
            return MBEDTLS_ECP_DP_SECP384R1;
#endif
#if defined(PSA_WANT_ECC_SECP_R1_521)
        case 521:
            return MBEDTLS_ECP_DP_SECP521R1;
#endif
        }
        break;

    case PSA_ECC_FAMILY_BRAINPOOL_P_R1:
        switch (bits) {
#if defined(PSA_WANT_ECC_BRAINPOOL_P_R1_256)
        case 256:
            return MBEDTLS_ECP_DP_BP256R1;
#endif
#if defined(PSA_WANT_ECC_BRAINPOOL_P_R1_384)
        case 384:
            return MBEDTLS_ECP_DP_BP384R1;
#endif
#if defined(PSA_WANT_ECC_BRAINPOOL_P_R1_512)
        case 512:
            return MBEDTLS_ECP_DP_BP512R1;
#endif
        }
        break;

    case PSA_ECC_FAMILY_MONTGOMERY:
        switch (bits) {
#if defined(PSA_WANT_ECC_MONTGOMERY_255)
        case 255:
            return MBEDTLS_ECP_DP_CURVE25519;
#endif
#if defined(PSA_WANT_ECC_MONTGOMERY_448)
        case 448:
            return MBEDTLS_ECP_DP_CURVE448;
#endif
        }
        break;

    case PSA_ECC_FAMILY_SECP_K1:
        switch (bits) {
#if defined(PSA_WANT_ECC_SECP_K1_192)
        case 192:
            return MBEDTLS_ECP_DP_SECP192K1;
#endif
#if defined(PSA_WANT_ECC_SECP_K1_224)
            /* secp224k1 is not and will not be supported in PSA (#3541). */
#endif
#if defined(PSA_WANT_ECC_SECP_K1_256)
        case 256:
            return MBEDTLS_ECP_DP_SECP256K1;
#endif
        }
        break;
    }

    return MBEDTLS_ECP_DP_NONE;
}

/* We don't need the full driver wrapper, we know the key is already a public key */
psa_status_t psa_driver_wrapper_export_public_key(
    const psa_key_attributes_t *attributes,
    const uint8_t *key_buffer, size_t key_buffer_size,
    uint8_t *data, size_t data_size, size_t *data_length)
{
    /* The verification handles only public keys, and this is called from P256M verify */
    assert(PSA_KEY_TYPE_IS_PUBLIC_KEY(psa_get_key_type(attributes)));

#if PSA_WANT_ALG_ECDSA == 1
    assert(PSA_KEY_TYPE_IS_ECC(psa_get_key_type(attributes)));
    assert(key_buffer_size <= data_size);
    memcpy(data, key_buffer, key_buffer_size);
    *data_length = key_buffer_size;
#else
    assert(0); /* Should never be reached */
#endif /* PSA_WANT_ALG_ECDSA */

    return PSA_SUCCESS;
}
/*!@}*/
