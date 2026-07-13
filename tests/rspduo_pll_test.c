/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mirisdr.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint32_t reg3 = 0u;
    uint32_t reg4 = 0u;
    assert(mirisdr_rspduo_pll_words(2048000u, &reg3, &reg4) == 0);
    assert(reg3 == 0x01181fu);
    assert(reg4 == 0x0624ddu);
    assert(mirisdr_rspduo_252_format_word(2048000u) == 0x000005u);
    assert(mirisdr_rspduo_252_format_word(6000000u) == 0x000094u);
    assert(mirisdr_rspduo_pll_words(0u, &reg3, &reg4) < 0);
    assert(mirisdr_rspduo_pll_words(2048000u, NULL, &reg4) < 0);
    puts("RSPDUO_PLL_OK");
    return 0;
}
