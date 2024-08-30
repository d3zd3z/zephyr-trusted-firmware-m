/*
 * Copyright (c) 2024, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef __NI_TOWER_LIB_H__
#define __NI_TOWER_LIB_H__

#include "ni_tower_rse_drv.h"

#include "array.h"
#include <stdint.h>

/*
 * Platform specific apu region initialization macro wrapper. This macros
 * returns 'struct ni_tower_apu_reg_cfg_info' definition by providing
 * the base and end address of APU region and the associated access permission.
 */
#define INIT_APU_REGION(base, end, perm)        \
    {                                           \
        .base_addr = base,                      \
        .end_addr = end,                        \
        .background = NI_T_FOREGROUND,          \
        .permissions = { perm, 0, 0, 0 },       \
        .entity_ids = { 0, 0, 0, 0 },           \
        .id_valid = NI_T_ID_VALID_NONE,         \
        .region_enable = NI_T_REGION_ENABLE,    \
        .lock = NI_T_LOCK                       \
    }

/*
 * Platform specific apu region initialization macro wrapper with APU ID
 * Filtering. This macro returns 'struct ni_tower_apu_reg_cfg_info' definition
 * by providing the base and end address of APU region and the associated
 * access permission for all four enitities.
 */
#define INIT_APU_REGION_WITH_ALL_ID_FILTER(base, end, scp_perm,           \
                                           rse_perm, dap_perm)            \
    {                                                                     \
        .base_addr = base,                                                \
        .end_addr = end,                                                  \
        .background = NI_T_FOREGROUND,                                    \
        .permissions = { scp_perm, rse_perm, dap_perm },                  \
        .entity_ids = { SYSCTRL_SCP_APU_ID, SYSCTRL_RSE_APU_ID,           \
                        SYSCTRL_DAP_APU_ID },                             \
        .id_valid = NI_T_ID_VALID_ALL,                                    \
        .region_enable = NI_T_REGION_ENABLE,                              \
        .lock = NI_T_LOCK                                                 \
    }

/*
 * Platform specific apu region initialization macro wrapper without locking
 * the region. This macros returns 'struct ni_tower_apu_reg_cfg_info'
 * definition by providing the base and end address of APU region and the
 * associated access permission.
 */
#define INIT_APU_REGION_UNLOCKED(base, end, perm)   \
    {                                               \
        .base_addr = base,                          \
        .end_addr = end,                            \
        .background = NI_T_FOREGROUND,              \
        .permissions = { perm, 0, 0, 0 },           \
        .entity_ids = { 0, 0, 0, 0 },               \
        .id_valid = NI_T_ID_VALID_NONE,             \
        .region_enable = NI_T_REGION_ENABLE,        \
        .lock = NI_T_UNLOCK                         \
    }

/* Interface ID of xSNI components - completer interfaces */
enum sysctrl_xSNI_ids {
    /* Request from AP */
    SYSCTRL_APP_ASNI_ID = 0x0,
    /* Request from RSE ATU */
    SYSCTRL_RSE_MAIN_ASNI_ID = 0x3,
    /* Request from RSE and SCP targeting Cluster Utility address space */
    SYSCTRL_RSE_SCP_ASNI_ID = 0x4,
    /* Request from SCP ATU */
    SYSCTRL_SCP_ASNI_ID = 0x5,
};

/* Interface ID of xMNI components - requester interfaces */
enum sysctrl_xMNI_ids {
    /* Targets AP address space */
    SYSCTRL_APP_AMNI_ID = 0x0,
    /* Targets memory map shared between AP and SCP */
    SYSCTRL_APP_SCP_AMNI_ID = 0x2,
    /* Targets Cluster Utility address space */
    SYSCTRL_CLUS_UTIL_AMNI_ID = 0x3,
    /* Targets Shared SRAM between RSE and SCP */
    SYSCTRL_RSM_AMNI_ID = 0x5,
    /* Targets SI address space */
    SYSCTRL_RSE_SI_AMNI_ID = 0x6,
    /* Targets SCP address space */
    SYSCTRL_RSE_SCP_AMNI_ID = 0x7,
    /* Targets CMN GPV registers */
    SYSCTRL_CMN_PMNI_ID = 0x8,
    /* Targets Shared RAM between RSE and SCP */
    SYSCTRL_RSM_PMNI_ID = 0x9,
    /* Targets SYSCTRL SMMU registers */
    SYSCTRL_TCU_PMNI_ID = 0xA,
    /* Targets the System Control NI-Tower registers (default target) */
    SYSCTRL_CONFIG_SPACE_ID = 0xF
};

/* APU IDs of the initiator for filter access */
enum sysctrl_apu_filter_ids {
    SYSCTRL_SCP_APU_ID = 0x3D,
    SYSCTRL_RSE_APU_ID = 0x3E,
    SYSCTRL_DAP_APU_ID = 0x3F,
};

/* Interface ID of Peripheral xMNI components */
enum periph_xMNI_ids {
    /* Targets ARSM SRAM */
    PERIPH_RAM_AMNI_ID = 0x0,
    /* Targets Secure SRAM Error record block for the shared ARSM SRAM */
    PERIPH_ECCREG_PMNI_ID,
    /* Targets AP Generic Timer Control Frame */
    PERIPH_GTIMERCTRL_PMNI_ID,
    /* Targets AP Non-secure WatchDog */
    PERIPH_NSGENWDOG_PMNI_ID,
    /* Targets AP Non-secure Generic Timer Control Base Frame */
    PERIPH_NSGTIMER_PMNI_ID,
    /* Targets AP Non-secure UART */
    PERIPH_NSUART0_PMNI_ID,
    /* Targets AP Non-secure UART for RMM debug */
    PERIPH_NSUART1_PMNI_ID,
    /* Targets AP root WatchDog */
    PERIPH_ROOTGENWDOG_PMNI_ID,
    /* Targets AP Secure WatchDog */
    PERIPH_SECGENWDOG_PMNI_ID,
    /* Targets AP Secure Generic Timer Control Base Frame */
    PERIPH_SECGTIMER_PMNI_ID,
    /* Targets AP Secure UART */
    PERIPH_SECUART_PMNI_ID
};

/**
 * \brief Programs System Control block NI-Tower PSAM and APU for AON domain
 *
 * \return Returns -1 if there is an error, else 0.
 */
int32_t program_sysctrl_ni_tower_aon(void);

/**
 * \brief Programs System Control block NI-Tower PSAM and APU for SYSTOP domain
 *
 * \return Returns -1 if there is an error, else 0.
 */
int32_t program_sysctrl_ni_tower_systop(void);

/**
 * \brief Program NI-Tower peripheral block APU
 *
 * \return Returns -1 if there is an error, else 0.
 */
int32_t program_periph_ni_tower(void);

/**
 * \brief Program NI-Tower peripheral block APU to lock ram region after
 *        limiting the region as read only.
 *
 * \return Returns -1 if there is an error, else 0.
 */
int32_t program_periph_ni_tower_post_ap_bl2_load(void);

#endif /* __NI_TOWER_LIB_H__ */
