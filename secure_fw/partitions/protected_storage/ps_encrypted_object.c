/*
 * Copyright (c) 2018-2021, Arm Limited. All rights reserved.
 * Copyright (c) 2024 Cypress Semiconductor Corporation (an Infineon company)
 * or an affiliate of Cypress Semiconductor Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "ps_encrypted_object.h"

#include <stddef.h>
#include <string.h>

#include "crypto/ps_crypto_interface.h"
#include "psa/internal_trusted_storage.h"
#include "ps_object_defs.h"
#include "ps_utils.h"

/* Size (in bytes) of the additional data that gets stored with the object data, including any padding */
#define STORED_HEADER_DATA_SIZE (offsetof(struct ps_object_t, header.info) \
                                 - offsetof(struct ps_object_t, header.crypto.ref.iv))

/* Gets the size of data to encrypt */
#define PS_ENCRYPT_SIZE(plaintext_size) \
    ((plaintext_size) + offsetof(struct ps_object_t, data) - offsetof(struct ps_object_t, header.info))

#define PS_OBJECT_START_POSITION  0

/* Buffer to store the maximum encrypted object */
/* FIXME: Do partial encrypt/decrypt to reduce the size of internal buffer */
#define PS_MAX_ENCRYPTED_OBJ_SIZE PS_ENCRYPT_SIZE(PS_MAX_OBJECT_DATA_SIZE)

/* Add the tag length to the crypto buffer size to account for the tag
 * being appended to the ciphertext by the crypto layer.
 */
#define PS_CRYPTO_BUF_LEN (PS_MAX_ENCRYPTED_OBJ_SIZE + PS_TAG_LEN_BYTES)

/**
 * \brief Performs authenticated decryption on object data, with the header as
 *        the associated data.
 *
 * \param[in]  fid       File ID
 * \param[in]  cur_size  Size of the object data to decrypt
 * \param[in,out] obj    Pointer to the object structure to authenticate and
 *                       fill in with the decrypted data. The tag of the object
 *                       is the one stored in the object table for the given
 *                       File ID.
 *
 * \return Returns error code as specified in \ref psa_status_t
 */
static psa_status_t ps_object_auth_decrypt(uint32_t fid,
                                           uint32_t cur_size,
                                           struct ps_object_t *obj)
{
    psa_status_t err;
    uint8_t *p_obj_data = (uint8_t *)&obj->header.info;
    size_t out_len;

    /* Use File ID as a part of the associated data to authenticate
     * the object in the FS. The tag will be stored in the object table and
     * not as a part of the object's data stored in the FS.
     */

    err = ps_crypto_auth_and_decrypt(&obj->header.crypto,
                                     (const uint8_t *)&fid,
                                     sizeof(fid),
                                     p_obj_data,
                                     cur_size,
                                     p_obj_data,
                                     sizeof(*obj) - offsetof(struct ps_object_t, header.info),
                                     &out_len);
    if (err != PSA_SUCCESS || out_len != cur_size) {
        return PSA_ERROR_GENERIC_ERROR;
    }

    return PSA_SUCCESS;
}

/**
 * \brief Performs authenticated encryption on object data, with the header as
 *        the associated data.
 *
 * \param[in]  fid       File ID
 * \param[in]  cur_size  Size of the object data to encrypt
 * \param[out] obj       Pointer to the object structure to authenticate and
 *                       fill in with the encrypted data.
 *
 * \return Returns error code as specified in \ref psa_status_t
 */
static psa_status_t ps_object_auth_encrypt(uint32_t fid,
                                           uint32_t cur_size,
                                           struct ps_object_t *obj)
{
    psa_status_t err;
    uint8_t *p_obj_data = (uint8_t *)&obj->header.info;
    size_t out_len;

    /* Get a new IV for each encryption */
    err = ps_crypto_get_iv(&obj->header.crypto);
    if (err != PSA_SUCCESS) {
        return err;
    }

    /* Use File ID as a part of the associated data to authenticate
     * the object in the FS. The tag will be stored in the object table and
     * not as a part of the object's data stored in the FS.
     */

    err = ps_crypto_encrypt_and_tag(&obj->header.crypto,
                                    (const uint8_t *)&fid,
                                    sizeof(fid),
                                    p_obj_data,
                                    cur_size,
                                    (uint8_t *)&obj->header.info,
                                    PS_CRYPTO_BUF_LEN,
                                    &out_len);
    if (err != PSA_SUCCESS || out_len != cur_size) {
        return PSA_ERROR_GENERIC_ERROR;
    }

    return PSA_SUCCESS;
}

psa_status_t ps_encrypted_object_read(uint32_t fid, struct ps_object_t *obj)
{
    psa_status_t err;
    uint32_t decrypt_size;
    size_t data_length;

    /* Read the encrypted object from the persistent area. The data stored via
     * ITS interface of this `fid` is the IV together with the encrypted object.
     * In the psa_its_get, the buffer size is not checked. Check the buffer size
     * here.
     */
    err = psa_its_get(fid, PS_OBJECT_START_POSITION,
                      PS_MAX_ENCRYPTED_OBJ_SIZE + STORED_HEADER_DATA_SIZE,
                      (void *)obj->header.crypto.ref.iv,
                      &data_length);
    if (err != PSA_SUCCESS) {
        return err;
    }

    /* Get the decrypt size. IV is also stored by ITS service. It is at the start
     * of the read out data. Toolchains may add padding byte after iv array in
     * crypto.ref structure.
     */
    decrypt_size = data_length - STORED_HEADER_DATA_SIZE;

    /* Decrypt the object data */
    err = ps_object_auth_decrypt(fid, decrypt_size, obj);
    if (err != PSA_SUCCESS) {
        return err;
    }

    return PSA_SUCCESS;
}

psa_status_t ps_encrypted_object_write(uint32_t fid, struct ps_object_t *obj)
{
    psa_status_t err;
    uint32_t wrt_size;

    wrt_size = PS_ENCRYPT_SIZE(obj->header.info.current_size);

    /* Authenticate and encrypt the object */
    err = ps_object_auth_encrypt(fid, wrt_size, obj);
    if (err != PSA_SUCCESS) {
        return err;
    }

    /* The IV will also be stored. The encrypted data is stored in ps_crypto_buf
     * now.
     * Toolchains may add padding byte after iv array in crypto.ref structure.
     * The padding byte will be written into the storage area.
     */
    wrt_size += STORED_HEADER_DATA_SIZE;

    /* Write the encrypted object to the persistent area. The tag is not
     * copied as it is stored in the object table.
     */
    return psa_its_set(fid, wrt_size, (const void *)obj->header.crypto.ref.iv,
                       PSA_STORAGE_FLAG_NONE);
}
