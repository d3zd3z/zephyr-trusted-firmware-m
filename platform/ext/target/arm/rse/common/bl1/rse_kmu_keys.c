/*
 * Copyright (c) 2023-2024, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "rse_kmu_keys.h"

#include <stdint.h>
#include <string.h>

#include "device_definition.h"
#include "dpa_hardened_word_copy.h"
#include "cc3xx_drv.h"
#include "trng.h"

static const struct kmu_key_export_config_t aes_key0_export_config = {
    CC3XX_BASE_S + 0x400, /* CC3XX AES_KEY_0 register */
    0, /* No delay */
    0x01, /* Increment by 4 bytes with each write */
    KMU_DESTINATION_PORT_WIDTH_32_BITS, /* Write 32 bits with each write */
    KMU_DESTINATION_PORT_WIDTH_8_WRITES, /* Perform 8 writes (total 256 bits) */
    true,  /* refresh the masking */
    false, /* Don't disable the masking */
};

static const struct kmu_key_export_config_t aes_key1_export_config = {
    CC3XX_BASE_S + 0x420, /* CC3XX AES_KEY_1 register */
    0, /* No delay */
    0x01, /* Increment by 4 bytes with each write */
    KMU_DESTINATION_PORT_WIDTH_32_BITS, /* Write 32 bits with each write */
    KMU_DESTINATION_PORT_WIDTH_8_WRITES, /* Perform 8 writes (total 256 bits) */
    true,  /* refresh the masking */
    false, /* Don't disable the masking */
};

static const struct kmu_key_export_config_t sic_dr0_export_config = {
    SIC_BASE_S + 0x120, /* CC3XX DR0_KEY_WORD0 register */
    0, /* No delay */
    0x01, /* Increment by 4 bytes with each write */
    KMU_DESTINATION_PORT_WIDTH_32_BITS, /* Write 32 bits with each write */
    KMU_DESTINATION_PORT_WIDTH_8_WRITES, /* Perform 8 writes (total 256 bits) */
    true,  /* refresh the masking */
    false, /* Don't disable the masking */
};

static const struct kmu_key_export_config_t sic_dr1_export_config = {
    SIC_BASE_S + 0x220, /* SIC DR1_KEY_WORD0 register */
    0, /* No delay */
    0x01, /* Increment by 4 bytes with each write */
    KMU_DESTINATION_PORT_WIDTH_32_BITS, /* Write 32 bits with each write */
    KMU_DESTINATION_PORT_WIDTH_8_WRITES, /* Perform 8 writes (total 256 bits) */
    true,  /* refresh the masking */
    false, /* Don't disable the masking */
};

static const struct kmu_key_export_config_t cc3xx_pka_sram_key_config = {
    CC3XX_BASE_S + 0xFD0, /* CC3XX keystore mux. This address is reused from the
                           * CC3XX register map and redirected to the PKA SRAM
                           * keystore.
                           */
    0, /* No delay */
    0x01, /* Increment by 4 bytes with each write */
    KMU_DESTINATION_PORT_WIDTH_32_BITS, /* Write 32 bits with each write */
    KMU_DESTINATION_PORT_WIDTH_4_WRITES, /* Perform 4 writes (total 128 bits) */
    true,  /* refresh the masking */
    false, /* Don't disable the masking */
};

static enum tfm_plat_err_t set_export_config_and_lock_key(enum rse_kmu_slot_id_t slot,
                                                          const struct kmu_key_export_config_t *config)
{
    enum kmu_error_t kmu_err;
    /* TODO lock keyslots once the runtime CC3XX driver supports locked KMU
     * keyslots
     */
    /* kmu_err = kmu_set_key_locked(&KMU_DEV_S, slot); */
    /* if (kmu_err != KMU_ERROR_NONE) { */
    /*     return (enum tfm_plat_err_t) kmu_err; */
    /* } */


    kmu_err = kmu_set_key_export_config(&KMU_DEV_S, slot, config);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    kmu_err = kmu_set_key_export_config_locked(&KMU_DEV_S, slot);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    return TFM_PLAT_ERR_SUCCESS;
}

static enum tfm_plat_err_t duplicate_into_next_slot(enum rse_kmu_slot_id_t slot,
                                                    const struct kmu_key_export_config_t *config)
{
    enum kmu_error_t kmu_err;
    volatile uint32_t *p_kmu_slot_buf;
    volatile uint32_t *p_kmu_secondary_slot_buf;
    size_t kmu_slot_size;
    enum tfm_plat_err_t plat_err;

    kmu_err = kmu_get_key_buffer_ptr(&KMU_DEV_S, slot,
                                     &p_kmu_slot_buf, &kmu_slot_size);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    kmu_err = kmu_get_key_buffer_ptr(&KMU_DEV_S, slot + 1,
                                     &p_kmu_secondary_slot_buf,
                                     &kmu_slot_size);

    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    dpa_hardened_word_copy(p_kmu_secondary_slot_buf, p_kmu_slot_buf,
                        kmu_slot_size / sizeof(uint32_t));

    plat_err = set_export_config_and_lock_key(slot + 1, config);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    return TFM_PLAT_ERR_SUCCESS;
}

enum tfm_plat_err_t setup_key_from_derivation(enum kmu_hardware_keyslot_t input_key_id,
                                              uint32_t *key_buf, const uint8_t *label,
                                              size_t label_len, enum rse_kmu_slot_id_t slot,
                                              const struct kmu_key_export_config_t *export_config,
                                              const struct kmu_key_export_config_t *aead_export_config,
                                              bool setup_aes_aead_key, boot_state_include_mask mask)
{
    enum tfm_plat_err_t plat_err;
    uint8_t context[32] = {0};
    size_t context_len;
    enum kmu_error_t kmu_err;
    volatile uint32_t *p_kmu_slot_buf;
    size_t kmu_slot_size;

    plat_err = rse_get_boot_state(context, sizeof(context), &context_len, mask);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    kmu_err = kmu_get_key_buffer_ptr(&KMU_DEV_S, slot, &p_kmu_slot_buf, &kmu_slot_size);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    plat_err = cc3xx_lowlevel_kdf_cmac((cc3xx_aes_key_id_t)input_key_id,
                                 key_buf, CC3XX_AES_KEYSIZE_256, label,
                                 label_len, context, context_len,
                                 (uint32_t *)p_kmu_slot_buf, 32);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    /* Due to limitations in CryptoCell, any key that needs to be used for
     * AES-CCM needs to be duplicated into a second slot.
     */
    if (setup_aes_aead_key) {
        plat_err = duplicate_into_next_slot(slot, aead_export_config);
        if (plat_err != TFM_PLAT_ERR_SUCCESS) {
            return plat_err;
        }
    }

    plat_err = set_export_config_and_lock_key(slot, export_config);

    return plat_err;
}

enum tfm_plat_err_t setup_key_from_otp(enum rse_kmu_slot_id_t slot,
                                       enum tfm_otp_element_id_t otp_id,
                                       const struct kmu_key_export_config_t *export_config,
                                       const struct kmu_key_export_config_t *aead_export_config,
                                       bool setup_aes_aead_key)
{
    enum tfm_plat_err_t plat_err;
    enum kmu_error_t kmu_err;
    volatile uint32_t *p_kmu_slot_buf;
    size_t kmu_slot_size;

    kmu_err = kmu_get_key_buffer_ptr(&KMU_DEV_S, slot, &p_kmu_slot_buf, &kmu_slot_size);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t)kmu_err;
    }

    plat_err = tfm_plat_otp_read(otp_id, kmu_slot_size, (uint8_t *)p_kmu_slot_buf);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    /* Due to limitations in CryptoCell, any key that needs to be used for
     * AES-CCM needs to be duplicated into a second slot.
     */
    if (setup_aes_aead_key) {
        plat_err = duplicate_into_next_slot(slot, aead_export_config);
        if (plat_err != TFM_PLAT_ERR_SUCCESS) {
            return plat_err;
        }
    }

    plat_err = set_export_config_and_lock_key(slot, export_config);

    return plat_err;
}

enum tfm_plat_err_t setup_key_from_rng(enum rse_kmu_slot_id_t slot,
                                       const struct kmu_key_export_config_t *export_config,
                                       const struct kmu_key_export_config_t *aead_export_config,
                                       bool setup_aes_aead_key)
{
    enum tfm_plat_err_t plat_err;
    enum kmu_error_t kmu_err;
    volatile uint32_t *p_kmu_slot_buf;
    size_t kmu_slot_size;

    kmu_err = kmu_get_key_buffer_ptr(&KMU_DEV_S, slot, &p_kmu_slot_buf, &kmu_slot_size);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t)kmu_err;
    }

    bl1_trng_generate_random((uint8_t *)p_kmu_slot_buf, kmu_slot_size);

    /* Due to limitations in CryptoCell, any key that needs to be used for
     * AES-CCM needs to be duplicated into a second slot.
     */
    if (setup_aes_aead_key) {
        plat_err = duplicate_into_next_slot(slot, aead_export_config);
        if (plat_err != TFM_PLAT_ERR_SUCCESS) {
            return plat_err;
        }
    }

    plat_err = set_export_config_and_lock_key(slot, export_config);

    return plat_err;
}

enum tfm_plat_err_t rse_setup_cpak_seed(void)
{
    const uint8_t cpak_seed_label[] = "BL1_CPAK_SEED_DERIVATION";
    const boot_state_include_mask boot_state_config =
        RSE_BOOT_STATE_INCLUDE_LCS | RSE_BOOT_STATE_INCLUDE_TP_MODE |
        RSE_BOOT_STATE_INCLUDE_BL1_2_HASH | RSE_BOOT_STATE_INCLUDE_REPROVISIONING_BITS;

    return setup_key_from_derivation(KMU_HW_SLOT_GUK, NULL, cpak_seed_label,
                                     sizeof(cpak_seed_label),
                                     RSE_KMU_SLOT_CPAK_SEED,
                                     &aes_key0_export_config, NULL, false,
                                     boot_state_config);
}

#ifdef RSE_BOOT_KEYS_CCA
enum tfm_plat_err_t rse_setup_dak_seed(void)
{
    const uint8_t dak_seed_label[]  = "BL1_DAK_SEED_DERIVATION";
    const boot_state_include_mask boot_state_config =
        RSE_BOOT_STATE_INCLUDE_LCS | RSE_BOOT_STATE_INCLUDE_TP_MODE |
        RSE_BOOT_STATE_INCLUDE_BL1_2_HASH | RSE_BOOT_STATE_INCLUDE_REPROVISIONING_BITS;

    return setup_key_from_derivation(KMU_HW_SLOT_GUK, NULL, dak_seed_label,
                                     sizeof(dak_seed_label),
                                     RSE_KMU_SLOT_DAK_SEED,
                                     &aes_key0_export_config, NULL, false,
                                     boot_state_config);
}
#endif /* RSE_BOOT_KEYS_CCA */

#ifdef RSE_BOOT_KEYS_DPE
enum tfm_plat_err_t rse_setup_rot_cdi(void)
{
    const uint8_t rot_cdi_label[] = "BL1_ROT_CDI_DERIVATION";
    const boot_state_include_mask boot_state_config =
        RSE_BOOT_STATE_INCLUDE_LCS | RSE_BOOT_STATE_INCLUDE_TP_MODE |
        RSE_BOOT_STATE_INCLUDE_BL1_2_HASH | RSE_BOOT_STATE_INCLUDE_REPROVISIONING_BITS;

    return setup_key_from_derivation(KMU_HW_SLOT_HUK, NULL, rot_cdi_label,
                                     sizeof(rot_cdi_label), RSE_KMU_SLOT_ROT_CDI,
                                     &aes_key0_export_config, NULL, false,
                                     boot_state_config);
}
#endif /* RSE_BOOT_KEYS_DPE */

enum tfm_plat_err_t rse_derive_vhuk_seed(uint32_t *vhuk_seed, size_t vhuk_seed_buf_len,
                         size_t *vhuk_seed_size)
{
    const uint8_t vhuk_seed_label[]  = "VHUK_SEED_DERIVATION";
    enum tfm_plat_err_t plat_err;

    if (vhuk_seed_buf_len < 32) {
        return 1;
    }

    plat_err = cc3xx_lowlevel_kdf_cmac((cc3xx_aes_key_id_t)KMU_HW_SLOT_HUK,
                                 NULL, CC3XX_AES_KEYSIZE_256, vhuk_seed_label,
                                 sizeof(vhuk_seed_label), NULL, 0, vhuk_seed, 32);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    *vhuk_seed_size = 32;

    return TFM_PLAT_ERR_SUCCESS;
}

enum tfm_plat_err_t rse_setup_vhuk(const uint8_t *vhuk_seeds, size_t vhuk_seeds_len)
{
    const boot_state_include_mask boot_state_config =
        RSE_BOOT_STATE_INCLUDE_LCS | RSE_BOOT_STATE_INCLUDE_TP_MODE |
        RSE_BOOT_STATE_INCLUDE_BL1_2_HASH | RSE_BOOT_STATE_INCLUDE_REPROVISIONING_BITS;

    return setup_key_from_derivation(KMU_HW_SLOT_GUK, NULL, vhuk_seeds,
                                     vhuk_seeds_len, RSE_KMU_SLOT_VHUK,
                                     &aes_key0_export_config, NULL, false,
                                     boot_state_config);
}

enum tfm_plat_err_t rse_setup_session_key(const uint8_t *ivs, size_t ivs_len)
{
    enum tfm_plat_err_t plat_err;
    enum kmu_error_t kmu_err;
    const boot_state_include_mask boot_state_config =
        RSE_BOOT_STATE_INCLUDE_LCS | RSE_BOOT_STATE_INCLUDE_TP_MODE |
        RSE_BOOT_STATE_INCLUDE_BL1_2_HASH | RSE_BOOT_STATE_INCLUDE_REPROVISIONING_BITS;


    plat_err = setup_key_from_derivation(KMU_HW_SLOT_GUK, NULL, ivs, ivs_len,
                                   RSE_KMU_SLOT_SESSION_KEY_0,
                                     &aes_key0_export_config,
                                     &aes_key1_export_config, true,
                                   boot_state_config);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    /* TODO: Should be removed once setup_key properly locks KMU slots */
    kmu_err = kmu_set_key_locked(&KMU_DEV_S, RSE_KMU_SLOT_SESSION_KEY_0);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    kmu_err = kmu_set_key_locked(&KMU_DEV_S,  RSE_KMU_SLOT_SESSION_KEY_0 + 1);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    return TFM_PLAT_ERR_SUCCESS;
}

static enum tfm_plat_err_t derive_using_krtl_or_zero_key(const uint8_t *label,
                                                         size_t label_len,
                                                         enum rse_kmu_slot_id_t output_slot,
                                                         boot_state_include_mask mask)
{
    enum tfm_plat_err_t plat_err;
    uint32_t zero_key[8] = {0};
    enum kmu_error_t kmu_err;
    enum lcm_tp_mode_t tp_mode;
    enum lcm_bool_t sp_enabled;
    enum rse_kmu_slot_id_t input_slot;
    uint32_t *key_buf;

    lcm_get_tp_mode(&LCM_DEV_S, &tp_mode);

    lcm_get_sp_enabled(&LCM_DEV_S, &sp_enabled);

    if (sp_enabled != LCM_TRUE) {
        return TFM_PLAT_ERR_SYSTEM_ERR;
    }

    switch(tp_mode) {
    case LCM_TP_MODE_PCI:
        input_slot = (enum rse_kmu_slot_id_t)KMU_HW_SLOT_KRTL;
        key_buf = NULL;
        break;
    case LCM_TP_MODE_TCI:
        input_slot = (enum rse_kmu_slot_id_t)0;
        key_buf = zero_key;
        break;
    default:
        return (enum tfm_plat_err_t) TFM_PLAT_ERR_KEY_DERIVATION_INVALID_TP_MODE;
    }

    plat_err = setup_key_from_derivation((enum kmu_hardware_keyslot_t)input_slot,
                                   key_buf, label, label_len, output_slot,
                                   &aes_key0_export_config,
                                   &aes_key1_export_config, true, mask);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    /* Until we can lock all the keys, just lock the ones only used in
     * BL1/provisioning.
     */
    kmu_err = kmu_set_key_locked(&KMU_DEV_S, output_slot);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    kmu_err = kmu_set_key_locked(&KMU_DEV_S, output_slot + 1);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }

    return TFM_PLAT_ERR_SUCCESS;
}

enum tfm_plat_err_t rse_setup_cm_provisioning_key(void)
{
    const uint8_t cm_provisioning_label[] = "CM_PROVISIONING";
    const boot_state_include_mask boot_state_config =
        RSE_BOOT_STATE_INCLUDE_LCS | RSE_BOOT_STATE_INCLUDE_TP_MODE |
        RSE_BOOT_STATE_INCLUDE_BL1_2_HASH | RSE_BOOT_STATE_INCLUDE_REPROVISIONING_BITS;

    return derive_using_krtl_or_zero_key(cm_provisioning_label,
                                         sizeof(cm_provisioning_label),
                                         RSE_KMU_SLOT_CM_PROVISIONING_KEY,
                                         boot_state_config);
}

enum tfm_plat_err_t rse_setup_dm_provisioning_key(void)
{
    const uint8_t dm_provisioning_label[] = "DM_PROVISIONING";
    const boot_state_include_mask boot_state_config =
        RSE_BOOT_STATE_INCLUDE_LCS | RSE_BOOT_STATE_INCLUDE_TP_MODE |
        RSE_BOOT_STATE_INCLUDE_BL1_2_HASH | RSE_BOOT_STATE_INCLUDE_REPROVISIONING_BITS;

    return derive_using_krtl_or_zero_key(dm_provisioning_label,
                                         sizeof(dm_provisioning_label),
                                         RSE_KMU_SLOT_DM_PROVISIONING_KEY,
                                         boot_state_config);
}


enum tfm_plat_err_t rse_setup_runtime_secure_image_encryption_key(void)
{
#ifdef RSE_XIP
    enum tfm_plat_err_t plat_err;
    enum kmu_error_t kmu_err;

    plat_err = setup_key_from_otp(RSE_KMU_SLOT_SECURE_ENCRYPTION_KEY,
                                  PLAT_OTP_ID_KEY_SECURE_ENCRYPTION,
                                  &sic_dr0_export_config, NULL, false);

    kmu_err = kmu_set_key_locked(&KMU_DEV_S, RSE_KMU_SLOT_SECURE_ENCRYPTION_KEY);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }
#endif /* RSE_XIP */

    /* TODO FIXME Allow Mcuboot to decrypt using on-device keys */
    return TFM_PLAT_ERR_SUCCESS;
}

enum tfm_plat_err_t rse_setup_runtime_non_secure_image_encryption_key(void)
{
#ifdef RSE_XIP
    enum tfm_plat_err_t plat_err;
    enum kmu_error_t kmu_err;

    plat_err = setup_key_from_otp(RSE_KMU_SLOT_NON_SECURE_ENCRYPTION_KEY,
                                  PLAT_OTP_ID_KEY_NON_SECURE_ENCRYPTION,
                                  &sic_dr1_export_config, NULL, false);

    kmu_err = kmu_set_key_locked(&KMU_DEV_S, RSE_KMU_SLOT_NON_SECURE_ENCRYPTION_KEY);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t) kmu_err;
    }
#endif /* RSE_XIP */

    /* TODO FIXME Allow Mcuboot to decrypt using on-device keys */
    return TFM_PLAT_ERR_SUCCESS;
}

enum tfm_plat_err_t rse_setup_cc3xx_pka_sram_encryption_key(void)
{
    enum tfm_plat_err_t plat_err;
    enum kmu_error_t kmu_err;

    plat_err = setup_key_from_rng(RSE_KMU_SLOT_CC3XX_PKA_SRAM_ENCRYPTION_KEY,
                            &cc3xx_pka_sram_key_config, NULL, false);
    if (plat_err != TFM_PLAT_ERR_SUCCESS) {
        return plat_err;
    }

    /* TODO FIXME remove this when all keys can be locked. */
    kmu_err = kmu_set_key_locked(&KMU_DEV_S, RSE_KMU_SLOT_CC3XX_PKA_SRAM_ENCRYPTION_KEY);
    if (kmu_err != KMU_ERROR_NONE) {
        return (enum tfm_plat_err_t)kmu_err;
    }

    return TFM_PLAT_ERR_SUCCESS;
}
