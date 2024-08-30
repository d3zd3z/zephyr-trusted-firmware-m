/*
 * Copyright (c) 2021-2024, The TrustedFirmware-M Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "cc3xx_rng.h"

#include "cc3xx_error.h"
#include "cc3xx_dev.h"
#include "cc3xx_stdlib.h"

#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "fatal_error.h"

#ifdef CC3XX_CONFIG_RNG_EXTERNAL_TRNG
#include "cc3xx_rng_external_trng.h"
#endif /* CC3XX_CONFIG_RNG_EXTERNAL_TRNG */

#ifdef CC3XX_CONFIG_RNG_ENABLE
static inline uint32_t round_up(uint32_t num, uint32_t boundary)
{
    return (num + boundary - 1) - ((num + boundary - 1) % boundary);
}

static bool lfsr_seed_done = false;
static uint32_t lfsr_buf[sizeof(uint64_t) / sizeof(uint32_t)];
static size_t lfsr_buf_used_idx = sizeof(lfsr_buf);
static uint32_t entropy_buf[sizeof(P_CC3XX->rng.ehr_data) / sizeof(uint32_t)];
static size_t entropy_buf_used_idx = sizeof(entropy_buf);

#ifndef CC3XX_CONFIG_RNG_EXTERNAL_TRNG
static cc3xx_err_t trng_init(void)
{
    /* Enable clock */
    P_CC3XX->rng.rng_clk_enable = 0x1U;

    /* reset trng */
    P_CC3XX->rng.rng_sw_reset = 0x1U;

    /* Apparently there's no way to tell that the reset has finished, so just do
     * these things repeatedly until they succeed (and hence the reset has
     * finished). Works because the reset value of SAMPLE_CNT1 is 0xFFFF.
     */
    do {
        /* Enable clock */
        P_CC3XX->rng.rng_clk_enable = 0x1U;

        /* Set subsampling ratio */
        P_CC3XX->rng.sample_cnt1 = CC3XX_CONFIG_RNG_SUBSAMPLING_RATE;

    } while (P_CC3XX->rng.sample_cnt1 != CC3XX_CONFIG_RNG_SUBSAMPLING_RATE);

    /* Temporarily disable the random source */
    P_CC3XX->rng.rnd_source_enable = 0x0U;

    /* Clear the interrupts */
    P_CC3XX->rng.rng_icr = 0x3FU;

    /* Mask all interrupts except EHR_VALID */
    P_CC3XX->rng.rng_imr = 0x3EU;

    /* Select the oscillator ring (And set SOP_SEL to 0x1 as is mandatory) */
    P_CC3XX->rng.trng_config = CC3XX_CONFIG_RNG_RING_OSCILLATOR_ID | (0x1U << 2);

    /* Set debug control register to no bypasses */
    P_CC3XX->rng.trng_debug_control = 0x0U;

    /* Enable the random source */
    P_CC3XX->rng.rnd_source_enable = 0x1U;

    return CC3XX_ERR_SUCCESS;
}

static cc3xx_err_t trng_finish(void)
{
    /* Disable the random source */
    P_CC3XX->rng.rnd_source_enable = 0x0U;

    /* Disable clock */
    P_CC3XX->rng.rng_clk_enable = 0x0U;

    return CC3XX_ERR_SUCCESS;
}

static cc3xx_err_t trng_get_random(uint32_t *buf, size_t word_count) {
    uint32_t attempt_count = 0;
    uint32_t idx;

    assert(word_count == sizeof(P_CC3XX->rng.ehr_data) / sizeof(uint32_t));

    /* Wait until the RNG has finished. Any status other than 0x1 indicates
     * that either the RNG hasn't finished or a statistical test has been
     * failed.
     */
    do {
        if (P_CC3XX->rng.rng_isr & 0xEU) {
            /* At least one test has failed - the buffer contents aren't
             * random.
             */

            /* Reset EHR registers */
            P_CC3XX->rng.rst_bits_counter = 0x1U;

            /* Clear the interrupt bits to restart generator */
            P_CC3XX->rng.rng_icr = 0x3FU;

            attempt_count++;
        }
    } while ((! (P_CC3XX->rng.rng_isr & 0x1U))
             && attempt_count < CC3XX_CONFIG_RNG_MAX_ATTEMPTS);

    if (attempt_count == CC3XX_CONFIG_RNG_MAX_ATTEMPTS) {
        trng_finish();
        FATAL_ERR(CC3XX_ERR_RNG_TOO_MANY_ATTEMPTS);
        return CC3XX_ERR_RNG_TOO_MANY_ATTEMPTS;
    }

    /* Reset EHR register */
    P_CC3XX->rng.rst_bits_counter = 0x1U;

    /* Make sure the interrupt is cleared before the generator is
     * restarted, to avoid a race condition with the hardware
     */
    P_CC3XX->rng.rng_icr = 0xFFFFFFFF;

    /* Reading the EHR_DATA restarts the generator */
    for (idx = 0; idx < sizeof(P_CC3XX->rng.ehr_data) / sizeof(uint32_t); idx++) {
        buf[idx] = P_CC3XX->rng.ehr_data[idx];
    }

    return CC3XX_ERR_SUCCESS;
}
#endif /* !CC3XX_CONFIG_RNG_EXTERNAL_TRNG */

/* See https://en.wikipedia.org/wiki/Xorshift#xorshift+ */
static uint64_t xorshift_plus_128_lfsr(void)
{
    static uint64_t state[2] = {0};
    uint64_t temp0;
    uint64_t temp1;

    if (!lfsr_seed_done) {
        /* This function doesn't need to be perfectly random as it is only used
         * for the permutation function, so only seed once per boot.
         */
        cc3xx_lowlevel_rng_get_random((uint8_t *)&state, sizeof(state),
                                      CC3XX_RNG_CRYPTOGRAPHICALLY_SECURE);
        lfsr_seed_done = true;
    }

    temp0 = state[0];
    temp1 = state[1];
    state[0] = state[1];

    temp0 ^= temp0 << 23;
    temp0 ^= temp0 >> 18;
    temp0 ^= temp1 ^ (temp1 >> 5);

    state[1] = temp0;

    return temp0 + temp1;
}

static cc3xx_err_t lfsr_get_random(uint32_t* buf, size_t word_count)
{
    assert(word_count == sizeof(uint64_t) / sizeof(uint32_t));

    *(uint64_t *)buf = xorshift_plus_128_lfsr();

    return CC3XX_ERR_SUCCESS;
}

cc3xx_err_t cc3xx_lowlevel_rng_get_random(uint8_t* buf, size_t length,
                                          enum cc3xx_rng_quality_t quality)
{
    uint32_t *random_buf;
    size_t *used_idx;
    size_t max_buf_size;
    cc3xx_err_t (*random_fn)(uint32_t *, size_t);

    size_t copy_size;
    bool request_is_word_aligned = ((uintptr_t)buf & 0x3) == 0 && (length & 0x3) == 0;
    bool rng_required;
    cc3xx_err_t err;

    switch (quality) {
    case CC3XX_RNG_CRYPTOGRAPHICALLY_SECURE:
        random_buf = entropy_buf;
        used_idx = &entropy_buf_used_idx;
        max_buf_size = sizeof(entropy_buf);
        random_fn = trng_get_random;
        break;
    case CC3XX_RNG_FAST:
        random_buf = lfsr_buf;
        used_idx = &lfsr_buf_used_idx;
        max_buf_size = sizeof(lfsr_buf);
        random_fn = lfsr_get_random;
        break;
    default:
        return CC3XX_ERR_RNG_INVALID_RNG;
    }

    /* If the request is word-aligned, then throw away some of entropy buf so it
     * itself is aligned and we can use an aligned copy.
     */
    if (request_is_word_aligned) {
        *used_idx = round_up(*used_idx, sizeof(uint32_t));
    }

    /* Check if we need to initialize the RNG, or if we can just serve the
     * request from the buffered entropy.
     */
    rng_required = max_buf_size - *used_idx < length;

    if (rng_required && quality == CC3XX_RNG_CRYPTOGRAPHICALLY_SECURE) {
        err = trng_init();
        if (err != CC3XX_ERR_SUCCESS) {
            return err;
        }
    }

    while(length > 0) {
        if (*used_idx == max_buf_size) {
            err = random_fn(random_buf, max_buf_size / sizeof(uint32_t));
            if (err != CC3XX_ERR_SUCCESS) {
                goto out;
            }
            *used_idx = 0;
        }

        copy_size = max_buf_size - *used_idx < length ?
                    max_buf_size - *used_idx : length;

        if (request_is_word_aligned) {
            /* If we aligned things earlier, then all of these pointers and
             * sizes will stay word aligned
             */
            for (size_t idx = 0; idx < copy_size / sizeof(uint32_t); idx++) {
                ((uint32_t *)buf)[idx] = (random_buf + (*used_idx / sizeof(uint32_t)))[idx];
            }
        } else {
            memcpy(buf, ((uint8_t *)random_buf) + *used_idx, copy_size);
        }
        length -= copy_size;
        buf += copy_size;
        *used_idx += copy_size;
    }

    err = CC3XX_ERR_SUCCESS;
out:
    /* If we started the RNG, shut it down */
    if (rng_required && quality == CC3XX_RNG_CRYPTOGRAPHICALLY_SECURE) {
        err = trng_finish();

        if (err != CC3XX_ERR_SUCCESS) {
            return err;
        }
    }

    return err;
}

/* As per NIST SP800-90A A.5.1 */
cc3xx_err_t cc3xx_lowlevel_rng_get_random_uint(uint32_t bound, uint32_t *uint,
                                               enum cc3xx_rng_quality_t quality)
{
    uint32_t value;
    uint32_t attempts = 0;
    cc3xx_err_t err;
    uint32_t mask;

    /* Zero is not a sane bound */
    assert(bound != 0);

    /* There are two cases that we need to handle differently, the one where we
     * have a single bit set, and the one where we have multiple. First check
     * which we have.
     */
    if ((bound & (bound - 1)) == 0) {
        /* If a single bit is set, we can get the mask by subtracting one */
        mask = bound - 1;
    } else {
        /* Else, we shift the all-one word right until it matches the offset of
         * the leading one-bit in the bound.
         */
        mask = UINT32_MAX >> __builtin_clz(bound);
    }

    do {
        err = cc3xx_lowlevel_rng_get_random((uint8_t *)&value, sizeof(value), quality);
        if (err != CC3XX_ERR_SUCCESS) {
            return err;
        }

        value &= mask;

        attempts += 1;
        if (attempts >= CC3XX_CONFIG_RNG_MAX_ATTEMPTS) {
            FATAL_ERR(CC3XX_ERR_RNG_TOO_MANY_ATTEMPTS);
            return CC3XX_ERR_RNG_TOO_MANY_ATTEMPTS;
        }
    } while (value >= bound);

    *uint = value;

    return CC3XX_ERR_SUCCESS;
}

#ifdef CC3XX_CONFIG_DPA_MITIGATIONS_ENABLE
/* https://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle. This returns a
 * uniformly random permutation, verified by experiment.
 */
static void fisher_yates_shuffle(uint8_t *permutation_buf, size_t len,
                                 enum cc3xx_rng_quality_t quality)
{
    uint32_t idx;
    uint32_t swap_idx;
    uint8_t temp_elem;
    cc3xx_err_t err;

    if (len == 0) {
        return;
    }

    for (idx = 0; idx <= len - 1; idx++) {
        err = cc3xx_lowlevel_rng_get_random_uint(len - idx, &swap_idx, quality);
        if (err != CC3XX_ERR_SUCCESS) {
            continue;
        }

        swap_idx += idx;
        temp_elem = permutation_buf[idx];
        permutation_buf[idx] = permutation_buf[swap_idx];
        permutation_buf[swap_idx] = temp_elem;
    }
}
#endif /* CC3XX_CONFIG_DPA_MITIGATIONS_ENABLE */

void cc3xx_lowlevel_rng_get_random_permutation(uint8_t *permutation_buf, size_t len,
                                               enum cc3xx_rng_quality_t quality)
{
    uint32_t idx;

    /* Initializes the permutation buffer */
    for (idx = 0; idx < len; idx++) {
        permutation_buf[idx] = idx;
    }

#ifdef CC3XX_CONFIG_DPA_MITIGATIONS_ENABLE
    fisher_yates_shuffle(permutation_buf, len, quality);
#endif /* CC3XX_CONFIG_DPA_MITIGATIONS_ENABLE */
}
#endif /* CC3XX_CONFIG_RNG_ENABLE */
