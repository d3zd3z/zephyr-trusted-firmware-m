/*
 * Copyright (c) 2024, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "rse_test_common.h"

#include "cc3xx_tests.h"
#include "rse_provisioning_tests.h"
#include "test_state_transitions.h"

#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof((arr)[0]))


static struct conditional_test_t provisioning_tests[] = {
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .any_sp_state = true,
        .test = {
            &rse_bl1_provisioning_test_0001,
            "RSE_BL1_1_PROVISIONING_TEST_0001",
            "Provisioning basic bundle validation test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .any_sp_state = true,
        .test = {
            &rse_bl1_provisioning_test_0002,
            "RSE_BL1_1_PROVISIONING_TEST_0002",
            "Provisioning negative bundle validation test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .any_sp_state = true,
        .test = {
            &rse_bl1_provisioning_test_0003,
            "RSE_BL1_1_PROVISIONING_TEST_0003",
            "Provisioning invalid key test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .any_sp_state = true,
        .test = {
            &rse_bl1_provisioning_test_0004,
            "RSE_BL1_1_PROVISIONING_TEST_0004",
            "Provisioning required test"
        },
    },
    {
        .any_tp_mode = true,
        .lcs = LCM_LCS_DM,
        .any_sp_state = true,
        .test = {
            &rse_bl1_provisioning_test_0101,
            "RSE_BL1_1_PROVISIONING_TEST_0101",
            "Provisioning key TP mode set test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .sp_enabled = LCM_TRUE,
        .test = {
            &rse_bl1_provisioning_test_0201,
            "RSE_BL1_1_PROVISIONING_TEST_0201",
            "Provisioning key setup test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .sp_enabled = LCM_FALSE,
        .test = {
            &rse_bl1_provisioning_test_0202,
            "RSE_BL1_1_PROVISIONING_TEST_0202",
            "Provisioning key setup negative test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .sp_enabled = LCM_FALSE,
        .test = {
            &rse_bl1_provisioning_test_0301,
            "RSE_BL1_1_PROVISIONING_TEST_0301",
            "Provisioning integration negative key setup test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .sp_enabled = LCM_TRUE,
        .test = {
            &rse_bl1_provisioning_test_0302,
            "RSE_BL1_1_PROVISIONING_TEST_0302",
            "Provisioning integration negative authentication test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .sp_enabled = LCM_TRUE,
        .test = {
            &rse_bl1_provisioning_test_0303,
            "RSE_BL1_1_PROVISIONING_TEST_0303",
            "Provisioning integration negative tag test"
        },
    },
    {
        .any_tp_mode = true,
        .any_lcs = true,
        .sp_enabled = LCM_TRUE,
        .test = {
            &rse_bl1_provisioning_test_0304,
            "RSE_BL1_1_PROVISIONING_TEST_0304",
            "Provisioning integration return value test"
        },
    },
};

static struct conditional_test_t state_transitions[] = {
    {
        .tp_mode = LCM_TP_MODE_VIRGIN,
        .any_lcs = true,
        .any_sp_state = true,
        .test = {
            &rse_test_virgin_to_tci_or_pci,
            "RSE_BL1_1_TRANSITIONING_TO_TCI/PCI",
            "Provisioning transition from Virgin TP mode to TCI or PCI mode"
        },
    },
    {
        .any_tp_mode = true,
        .lcs = LCM_LCS_CM,
        .sp_enabled = LCM_FALSE,
        .test = {
            &rse_test_cm_sp_enable,
            "RSE_BL1_1_TRANSITIONING_TO_CM_SP_ENABLE",
            "Provisioning transition to CM LCS secure provisioning enable mode"
        },
    },
    {
        .any_tp_mode = true,
        .lcs = LCM_LCS_CM,
        .sp_enabled = LCM_TRUE,
        .test = {
            &rse_test_cm_to_dm,
            "RSE_BL1_1_TRANSITIONING_TO_DM",
            "Provisioning transition to DM LCS"
        },
    },
    {
        .any_tp_mode = true,
        .lcs = LCM_LCS_DM,
        .sp_enabled = LCM_FALSE,
        .test = {
            &rse_test_dm_sp_enable,
            "RSE_BL1_1_TRANSITIONING_TO_DM_SP_ENABLE",
            "Provisioning transition to DM LCS secure provisioning enable mode"
        },
    },
    {
        .any_tp_mode = true,
        .lcs = LCM_LCS_DM,
        .sp_enabled = LCM_TRUE,
        .test = {
            &rse_test_dm_to_se,
            "RSE_BL1_1_TRANSITIONING_TO_SE",
            "Provisioning transition to SE LCS"
        },
    },
    {
        .any_tp_mode = true,
        .lcs = LCM_LCS_SE,
        .any_sp_state = true,
        .test = {
            &rse_test_se_to_rma,
            "RSE_BL1_1_TRANSITIONING_TO_RMA",
            "Provisioning transition to RMA LCS"
        },
    },
};

static struct test_t bl1_1_extra_tests[100];

void register_testsuite_extra_bl1_1(struct test_suite_t *p_test_suite)
{
    set_testsuite("RSE Tests", bl1_1_extra_tests, 0, p_test_suite);

    add_conditional_tests_to_testsuite(provisioning_tests, ARRAY_SIZE(provisioning_tests),
                                       p_test_suite, ARRAY_SIZE(bl1_1_extra_tests));

    add_cc3xx_tests_to_testsuite(p_test_suite, ARRAY_SIZE(bl1_1_extra_tests));

    /* This one must be added last in order for the state transitions to work */
    add_conditional_tests_to_testsuite(state_transitions, ARRAY_SIZE(state_transitions),
                                       p_test_suite, ARRAY_SIZE(bl1_1_extra_tests));
}
