
/*
 * 8 bitový formát
 * 1024 bajtů odpovídá 1008 hodnotám
 * struktura:
 *   16b hlavička
 *  1008b bloků obsahujících 1008 8b hodnot
 */
static int mirisdr_samples_convert_504_s16 (mirisdr_dev_t *p, unsigned char* buf, uint8_t *dst8, int cnt) {
    int i, i_max, j, ret = 0;
    uint32_t addr = 0;
    uint8_t *src = buf;
    int16_t *dst = (int16_t*) dst8;

    /* dostáváme 1-3 1024 bytů dlouhé bloky */
    for (i_max = cnt >> 10, i = 0; i < i_max; i++, src+= 1008) {
        /* pozice hlavičky */
        addr = src[3] << 24 | src[2] << 16 | src[1] << 8 | src[0] << 0;

        if (p->rspduo_dual && getenv("OPENRSP_TRACE_DUAL_HEADERS") != NULL) {
            static unsigned int traced_headers;
            if (traced_headers < 32u) {
                fprintf(stderr,
                        "OPENRSP_DUAL_HEADER index=%u addr=%08x bytes=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
                        traced_headers, addr, src[16], src[17], src[18], src[19],
                        src[20], src[21], src[22], src[23]);
                ++traced_headers;
            }
        }

        /* potenciálně ztracená data */
        if ((i == 0) && p->usb_pid != 0x3020u && p->addr_valid && (addr != p->addr)) {
            fprintf(stderr, "%u samples lost, %d, %08x:%08x\n", addr - p->addr, cnt, p->addr, addr);
            p->sync_loss_cnt++;
        }

        /* přeskočíme hlavičku 16 bitů, 504 I+Q párů */
        for (src+= 16, j = 0; j < 1008; j+= 2, ret+= 2) {
            /* bitovým posunem zajistíme plný rozsah a zároveň správné znaménko */
            dst[ret + 0] = src[j + 0] << 8;
            dst[ret + 1] = src[j + 1] << 8;
        }
    }

    p->addr = addr + 504;
    p->addr_valid = 1;

    /* total used bytes */
    return ret * 2;
}
