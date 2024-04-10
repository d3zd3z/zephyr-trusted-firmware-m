/*
 * Copyright (c) 2024, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "boot_hal.h"
#include "bootutil/bootutil_log.h"
#include "crypto_hw.h"
#include "device_definition.h"
#include "fih.h"
#include "flash_map/flash_map.h"
#include "host_base_address.h"
#include "interrupts_bl2.h"
#include "mhu_v3_x.h"
#include "platform_base_address.h"
#include "platform_regs.h"
#include "rse_expansion_regs.h"

#include <string.h>

/*
 * ============================ INIT FUNCTIONS =================================
 */

int32_t boot_platform_post_init(void)
{
    int32_t result;

    result = interrupts_bl2_init();
    if (result != 0) {
        return result;
    }

    result = crypto_hw_accelerator_init();
    if (result) {
        return 1;
    }

    (void)fih_delay_init();

    return 0;
}

/*
 * Last function called before jumping to runtime. Used for final setup and
 * cleanup.
 */
static int boot_platform_finish(void)
{
    /*
     * Disable SCP to RSE MHUv3 Interrupt to ensure interrupt doesn't trigger
     * while switching to runtime.
     */
    NVIC_DisableIRQ(CMU_MHU4_Receiver_IRQn);
    return 0;
}

/*
 * ========================= SECURE LOAD FUNCTIONS =============================
 */

static int boot_platform_pre_load_secure(void)
{
    return 0;
}

static int boot_platform_post_load_secure(void)
{
    return boot_platform_finish();
}

/*
 * ============================ SCP LOAD FUNCTIONS =============================
 */

static int initialize_rse_scp_mhu(void)
{
    enum mhu_v3_x_error_t mhuv3_err;
    uint8_t ch;
    uint8_t num_ch;

    /* Setup RSE to SCP MHU Sender */

    /* Initialize the RSE to SCP Sender MHU */
    mhuv3_err = mhu_v3_x_driver_init(&MHU_V3_RSE_TO_SCP_DEV);
    if (mhuv3_err != MHU_V_3_X_ERR_NONE) {
        BOOT_LOG_ERR("BL2: RSE to SCP MHU driver init failed: %d", mhuv3_err);
        return 1;
    }

    /* Get number of channels for sender */
    mhuv3_err = mhu_v3_x_get_num_channel_implemented(
                &MHU_V3_RSE_TO_SCP_DEV, MHU_V3_X_CHANNEL_TYPE_DBCH, &num_ch);
    if (mhuv3_err != MHU_V_3_X_ERR_NONE) {
        BOOT_LOG_ERR("BL2: RSE to SCP MHU get channels failed: %d", mhuv3_err);
        return 1;
    }

    /* Disable interrupts for sender */
    for (ch = 0; ch < num_ch; ++ch) {
        mhuv3_err = mhu_v3_x_channel_interrupt_disable(
                    &MHU_V3_RSE_TO_SCP_DEV, ch, MHU_V3_X_CHANNEL_TYPE_DBCH);
        if (mhuv3_err != MHU_V_3_X_ERR_NONE) {
            BOOT_LOG_ERR("BL2: RSE to SCP MHU interrupt disable failed: %d",
                         mhuv3_err);
            return 1;
        }
    }

    /* Setup SCP to RSE MHU Receiver */

    /* Initialize the SCP to RSE Receiver MHU */
    mhuv3_err = mhu_v3_x_driver_init(&MHU_V3_SCP_TO_RSE_DEV);
    if (mhuv3_err != MHU_V_3_X_ERR_NONE) {
        BOOT_LOG_ERR("BL2: SCP to RSE MHU driver init failed: %d", mhuv3_err);
        return 1;
    }

    /* Get number of channels of receiver */
    mhuv3_err = mhu_v3_x_get_num_channel_implemented(&MHU_V3_SCP_TO_RSE_DEV,
            MHU_V3_X_CHANNEL_TYPE_DBCH, &num_ch);
    if (mhuv3_err != MHU_V_3_X_ERR_NONE) {
        BOOT_LOG_ERR("BL2: SCP to RSE MHU get channels failed: %d", mhuv3_err);
        return 1;
    }

    /*
     * Clear receiver interrupt mask on all channels and enable interrupts for
     * all channels so interrupts are triggered when data is received on any
     * channel.
     */
    for (ch = 0; ch < num_ch; ch++) {
        mhuv3_err = mhu_v3_x_channel_interrupt_enable(&MHU_V3_SCP_TO_RSE_DEV, ch,
                                                MHU_V3_X_CHANNEL_TYPE_DBCH);
        if (mhuv3_err != MHU_V_3_X_ERR_NONE) {
            BOOT_LOG_ERR("BL2: RSE to SCP MHU interrupt enable failed: %d",
                         mhuv3_err);
            return 1;
        }
        mhuv3_err = mhu_v3_x_doorbell_mask_clear(&MHU_V3_SCP_TO_RSE_DEV, ch,
                                                 UINT32_MAX);
        if (mhuv3_err != MHU_V_3_X_ERR_NONE) {
            BOOT_LOG_ERR("BL2: RSE to SCP MHU mask clear failed: %d",
                         mhuv3_err);
            return 1;
        }
    }

    /* Enable SCP to RSE MHUv3 Interrupt */
    NVIC_ClearPendingIRQ(CMU_MHU4_Receiver_IRQn);
    NVIC_EnableIRQ(CMU_MHU4_Receiver_IRQn);

    BOOT_LOG_INF("BL2: RSE to SCP and SCP to RSE MHUs initialized");
    return 0;
}

/* Function called before SCP firmware is loaded. */
static int boot_platform_pre_load_scp(void)
{
    enum atu_error_t atu_err;
    int mhu_err;

    BOOT_LOG_INF("BL2: SCP pre load start");

    /* Configure ATUs for loading to areas not directly addressable by RSE. */

    /*
     * Configure RSE ATU to access header region for SCP. The header part of
     * the image is loaded at the end of the ITCM to allow the code part of the
     * image to be placed at the start of the ITCM. For this, setup a separate
     * ATU region for the image header.
     */
    atu_err = atu_initialize_region(&ATU_DEV_S,
                                    RSE_ATU_IMG_HDR_LOAD_ID,
                                    HOST_SCP_HDR_ATU_WINDOW_BASE_S,
                                    HOST_SCP_HDR_PHYS_BASE,
                                    RSE_IMG_HDR_ATU_WINDOW_SIZE);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    /* Configure RSE ATU to access SCP ITCM region */
    atu_err = atu_initialize_region(&ATU_DEV_S,
                                    RSE_ATU_IMG_CODE_LOAD_ID,
                                    HOST_SCP_IMG_CODE_BASE_S,
                                    HOST_SCP_PHYS_BASE,
                                    HOST_SCP_ATU_SIZE);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    /* Initialize RSE to SCP and SCP to RSE MHUs */
    mhu_err = initialize_rse_scp_mhu();
    if (mhu_err) {
        return mhu_err;
    }

    BOOT_LOG_INF("BL2: SCP pre load complete");

    return 0;
}

/* Function called after SCP firmware is loaded. */
static int boot_platform_post_load_scp(void)
{
    enum atu_error_t atu_err;
    struct rse_integ_t *integ_layer =
            (struct rse_integ_t *)RSE_INTEG_LAYER_BASE_S;
    enum mscp_error_t mscp_err;

    BOOT_LOG_INF("BL2: SCP post load start");

    /*
     * Since the measurement are taken at this point, clear the image header
     * part in the ITCM before releasing SCP out of reset.
     */
    memset(HOST_SCP_IMG_HDR_BASE_S, 0, BL2_HEADER_SIZE);

    /* Enable SCP's ATU Access Permission (ATU AP) */
    integ_layer->atu_ap |= RSE_INTEG_ATU_AP_SCP_ATU;

    /* Configure RSE ATU to access SCP INIT_CTRL region */
    atu_err = atu_initialize_region(&ATU_DEV_S,
                                    HOST_SCP_INIT_CTRL_ATU_ID,
                                    HOST_SCP_INIT_CTRL_BASE_S,
                                    HOST_SCP_INIT_CTRL_PHYS_BASE,
                                    HOST_SCP_INIT_CTRL_SIZE);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    mscp_err = mscp_driver_release_cpu(&HOST_SCP_DEV);
    if (mscp_err != MSCP_ERR_NONE) {
        BOOT_LOG_ERR("BL2: SCP release failed");
        return 1;
    }
    BOOT_LOG_INF("BL2: SCP is released out of reset");

    /* Close RSE ATU region configured to access SCP INIT_CTRL region */
    atu_err = atu_uninitialize_region(&ATU_DEV_S, HOST_SCP_INIT_CTRL_ATU_ID);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    /* Close RSE ATU region configured to access RSE header region for SCP */
    atu_err = atu_uninitialize_region(&ATU_DEV_S, RSE_ATU_IMG_HDR_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    /* Close RSE ATU region configured to access SCP ITCM region */
    atu_err = atu_uninitialize_region(&ATU_DEV_S, RSE_ATU_IMG_CODE_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    BOOT_LOG_INF("BL2: SCP post load complete");

    return 0;
}

/*
 * =========================== AP BL2 LOAD FUNCTIONS ===========================
 */

/* Function called before AP BL2 firmware is loaded. */
static int boot_platform_pre_load_ap_bl2(void)
{
    enum atu_error_t atu_err;
    enum atu_roba_t roba_value;

    BOOT_LOG_INF("BL2: AP BL2 pre load start");

    BOOT_LOG_INF("BL2: Wait for doorbell from SCP before loading AP BL2...");

    /*
     * Ensure SCP has notified it is ready and setup anything needed for access
     * to the application processor subsystem.
     */
    if (host_system_prepare_ap_access() != 0) {
        BOOT_LOG_ERR("BL2: Could not setup access to AP systems.");
        return 1;
    }

    BOOT_LOG_INF("BL2: Doorbell received from SCP!");

    /* Configure RSE ATU to access RSE header region for AP BL2 */
    atu_err = atu_initialize_region(&ATU_DEV_S,
                                    RSE_ATU_IMG_HDR_LOAD_ID,
                                    HOST_AP_BL2_HDR_ATU_WINDOW_BASE_S,
                                    HOST_AP_BL2_HDR_PHYS_BASE,
                                    RSE_IMG_HDR_ATU_WINDOW_SIZE);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    roba_value = ATU_ROBA_SET_1;
    atu_err = set_axnsc(&ATU_DEV_S, roba_value, RSE_ATU_IMG_HDR_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        BOOT_LOG_INF("BL2: Unable to modify AxNSE");
        return 1;
    }

    roba_value = ATU_ROBA_SET_0;
    atu_err = set_axprot1(&ATU_DEV_S, roba_value, RSE_ATU_IMG_HDR_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        BOOT_LOG_INF("BL2: Unable to modify AxPROT1");
        return 1;
    }

    /* Configure RSE ATU to access AP BL2 Shared SRAM region */
    atu_err = atu_initialize_region(&ATU_DEV_S,
                                    RSE_ATU_IMG_CODE_LOAD_ID,
                                    HOST_AP_BL2_IMG_CODE_BASE_S,
                                    HOST_AP_BL2_PHYS_BASE,
                                    HOST_AP_BL2_ATU_SIZE);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    roba_value = ATU_ROBA_SET_1;
    atu_err = set_axnsc(&ATU_DEV_S, roba_value, RSE_ATU_IMG_CODE_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        BOOT_LOG_INF("BL2: Unable to modify AxNSE");
        return 1;
    }

    roba_value = ATU_ROBA_SET_0;
    atu_err = set_axprot1(&ATU_DEV_S, roba_value, RSE_ATU_IMG_CODE_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        BOOT_LOG_INF("BL2: Unable to modify AxPROT1");
        return 1;
    }

    BOOT_LOG_INF("BL2: AP BL2 pre load complete");

    return 0;
}

/* Function called after AP BL2 firmware is loaded. */
static int boot_platform_post_load_ap_bl2(void)
{
    enum atu_error_t atu_err;

    BOOT_LOG_INF("BL2: AP BL2 post load start");

    /*
     * Since the measurement are taken at this point, clear the image
     * header part in the Shared SRAM before releasing AP BL2 out of reset.
     */
    memset(HOST_AP_BL2_IMG_HDR_BASE_S, 0, BL2_HEADER_SIZE);

    /* Close RSE ATU region configured to access RSE header region for AP BL2 */
    atu_err = atu_uninitialize_region(&ATU_DEV_S, RSE_ATU_IMG_HDR_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    /* Close RSE ATU region configured to access AP BL2 Shared SRAM region */
    atu_err = atu_uninitialize_region(&ATU_DEV_S, RSE_ATU_IMG_CODE_LOAD_ID);
    if (atu_err != ATU_ERR_NONE) {
        return 1;
    }

    BOOT_LOG_INF("BL2: AP BL2 post load complete");

    return 0;
}

/*
 * ================================= VECTORS ==================================
 */

/*
 * Array of function pointers to call before each image is loaded indexed by
 * image id
 */
static int (*boot_platform_pre_load_vector[RSE_FIRMWARE_COUNT]) (void) = {
    [RSE_FIRMWARE_SECURE_ID]        = boot_platform_pre_load_secure,
    [RSE_FIRMWARE_SCP_ID]           = boot_platform_pre_load_scp,
    [RSE_FIRMWARE_AP_BL2_ID]        = boot_platform_pre_load_ap_bl2,
};

/*
 * Array of function pointers to call after each image is loaded indexed by
 * image id
 */
static int (*boot_platform_post_load_vector[RSE_FIRMWARE_COUNT]) (void) = {
    [RSE_FIRMWARE_SECURE_ID]        = boot_platform_post_load_secure,
    [RSE_FIRMWARE_SCP_ID]           = boot_platform_post_load_scp,
    [RSE_FIRMWARE_AP_BL2_ID]        = boot_platform_post_load_ap_bl2,
};

/*
 * ============================== LOAD FUNCTIONS ==============================
 */

int boot_platform_pre_load(uint32_t image_id)
{
    if (image_id >= RSE_FIRMWARE_COUNT) {
        BOOT_LOG_WRN("BL2: no pre load for image %d", image_id);
        return 0;
    }

    return boot_platform_pre_load_vector[image_id]();
}

int boot_platform_post_load(uint32_t image_id)
{
    if (image_id >= RSE_FIRMWARE_COUNT) {
        BOOT_LOG_WRN("BL2: no post load for image %d", image_id);
        return 0;
    }

    return boot_platform_post_load_vector[image_id]();
}

bool boot_platform_should_load_image(uint32_t image_id)
{
    if (image_id == RSE_FIRMWARE_NON_SECURE_ID) {
        return false;
    }

    if (image_id >= RSE_FIRMWARE_COUNT) {
        BOOT_LOG_WRN("BL2: Image %d beyond expected Firmware count: %d",
                image_id, RSE_FIRMWARE_COUNT);
        return false;
    }

    return true;
}
