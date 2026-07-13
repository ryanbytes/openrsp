/*
 * Copyright (C) 2013 by Miroslav Slugen <thunder.m@email.cz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "hard.h"

uint32_t mirisdr_rspduo_252_format_word(uint32_t rate)
{
	return rate == 6000000u ? 0x000094u : 0x000005u;
}

int mirisdr_rspduo_pll_words(uint32_t rate, uint32_t *reg3_out, uint32_t *reg4_out)
{
	if (!reg3_out || !reg4_out || rate < MIRISDR_SAMPLE_RATE_MIN ||
	    rate > MIRISDR_SAMPLE_RATE_MAX) return -1;
	uint64_t i, vco = 0, n, fract;
	for (i = 4; i < 16; i += 2)
	{
		vco = (uint64_t)rate * i * 12;
		if (vco >= 384000000UL) break;
	}
	if (i >= 16)
	{
		i = 16;
		vco = (uint64_t)rate * i * 12;
	}
	n = vco / 48000000UL;
	if (n > 15u) return -1;
	fract = 0x200000UL * (vco % 48000000UL) / 48000000UL;
	uint32_t reg3 = 3u;
	reg3 |= (uint32_t)(i / 2u - 1u) << 2;
	reg3 |= (uint32_t)((fract >> 20) & 1u) << 7;
	reg3 |= (uint32_t)n << 8;
	reg3 |= (rate <= 6048000u ? 0x01u : rate <= 8064000u ? 0x05u :
	         rate <= 9216000u ? 0x09u : 0x0du) << 12;
	reg3 |= 1u << 16;
	*reg3_out = reg3;
	*reg4_out = (uint32_t)fract & 0xfffffu;
	return 0;
}

/* nastavení parametrů které vyžadují restart */
/* parameters that require restart */
int mirisdr_set_hard(mirisdr_dev_t *p)
{
	int streaming = 0;
	uint32_t reg3 = 0, reg4 = 0;
	uint64_t i, vco, n, fract;

	/* při změně registrů musíme zastavit streamování */
	/* at a registry change we must stop streaming */
	if (p->async_status == MIRISDR_ASYNC_RUNNING)
	{
		streaming = 1;

		if ((mirisdr_stop_async(p) < 0) || (mirisdr_adc_stop(p) < 0)) {
			goto failed;
		}
	}

	/* omezení rozsahu */
	/* limit the scope of */
	if (p->rate > MIRISDR_SAMPLE_RATE_MAX)
	{
		fprintf(stderr, "can't set rate %u, setting maximum rate: %d\n", p->rate, MIRISDR_SAMPLE_RATE_MAX);
		p->rate = MIRISDR_SAMPLE_RATE_MAX;
	}
	else if (p->rate < MIRISDR_SAMPLE_RATE_MIN)
	{
		fprintf(stderr, "can't set rate %u, setting minimum rate: %d\n", p->rate, MIRISDR_SAMPLE_RATE_MIN);
		p->rate = MIRISDR_SAMPLE_RATE_MIN;
	}

	/* automatická volba formátu */
	/* automatic choice format */
	if (p->format_auto == MIRISDR_FORMAT_AUTO_ON)
	{
		if (p->rate <= 6048000) {
			p->format = MIRISDR_FORMAT_252_S16;
		} else if (p->rate <= 8064000) {
			p->format = MIRISDR_FORMAT_336_S16;
		} else if (p->rate <= 9216000) {
			p->format = MIRISDR_FORMAT_384_S16;
		} else {
			p->format = MIRISDR_FORMAT_504_S16;
		}
	}

	/* typ forámtu a šířka pásma */
	/* format type and bandwidth */
	switch (p->format)
	{
	case MIRISDR_FORMAT_252_S16:
		/* maximum rate 6.048 Msps | 24.576 MB/s | 196.608 Mbit/s  */
#if MIRISDR_DEBUG >= 1
		fprintf( stderr, "format: 252\n");
#endif
		/* The live RSPduo delivers the full six-megasample stream with the
		 * standard 252-word format value.  Retain the captured 0x05 value for
		 * other rates until each tuple has its own hardware evidence. */
		mirisdr_write_reg(p, 0x07, p->usb_pid == 0x3020u ?
		                  mirisdr_rspduo_252_format_word(p->rate) : 0x000094);
		p->addr = 252 + 2;
		break;
	case MIRISDR_FORMAT_336_S16:
		/* maximum rate 8.064 Msps | 24.576 MB/s | 196.608 Mbit/s */
#if MIRISDR_DEBUG >= 1
		fprintf( stderr, "format: 336\n");
#endif
		mirisdr_write_reg(p, 0x07, 0x000085);
		p->addr = 336 + 2;
		break;
	case MIRISDR_FORMAT_384_S16:
		/* maximum rate 9.216 Msps | 24.576 MB/s | 196.608 Mbit/s */
#if MIRISDR_DEBUG >= 1
		fprintf( stderr, "format: 384\n");
#endif
		mirisdr_write_reg(p, 0x07, 0x0000a5);
		p->addr = 384 + 2;
		break;
	case MIRISDR_FORMAT_504_S16:
	case MIRISDR_FORMAT_504_S8:
		/* maximum rate 12.096 Msps | 24.576 MB/s | 196.608 Mbit/s */
#if MIRISDR_DEBUG >= 1
		fprintf( stderr, "format: 504\n");
#endif
		mirisdr_write_reg(p, 0x07, 0x000c94);
		p->addr = 504 + 2;
		break;
	}

	/*
	 * Výpočet dělení vzorkovací frekvence
	 * Min: >= 1.3 Msps
	 * Max: <= 15 Msps, od 12,096 Msps prokládaně
	 * Poznámka: Nastavení vyšší frekvence než 15 Msps uvede tuner do speciálního
	 *           režimu kdy není možné přepnout rate zpět, stejně tak nastavení nižší
	 *           frekvence než je 571429 sps, protože pak bude N menší jak 2, což není
	 *           přípustný stav.
	 */
	/*
	 * Calculating division sampling frequency
	 * Min: >= 1.3 Msps
	 * Max: <= 15 Msps, from 12,096 Msps interpolated
	 * Note: Setting a higher frequency than 15 Msps indicate tuner into a special mode
	 * 		 where you can not switch back rate, as well as setting a lower frequency than 571,429 SPS
	 * 		 because it will be less than N 2, which is not an acceptable condition.
	 */
	if (p->usb_pid == 0x3020u)
	{
		if (mirisdr_rspduo_pll_words(p->rate, &reg3, &reg4) < 0) goto failed;
		mirisdr_write_reg(p, 0x04, reg4);
		mirisdr_write_reg(p, 0x03, reg3);
		if ((streaming) && (mirisdr_start_async(p) < 0)) goto failed;
		return 0;
	}
	else
	{
		for (i = 4; i < 16; i += 2)
		{
			vco = (uint64_t) p->rate * i * 12;
			if (vco >= 202000000UL) break;
		}
	}

	/* z předchozího výpočtu je N minimálně 4 */
	/* from the previous calculation N is at least 4 */
	n = vco / 48000000UL;
	fract = 0x200000UL * (vco % 48000000UL) / 48000000UL;
#if MIRISDR_DEBUG >= 1
	fprintf( stderr, "rate: %u, vco: %lu (%lu), n: %lu, fraction: %lu\n",
			p->rate, (long unsigned int)vco, (long unsigned int)(i / 2) - 1,
			(long unsigned int)n, (long unsigned int)fract);
#endif
	/* nastavení vzorkovací frekvence */
	/* Setting the sampling rate */
	reg3 |= (0x03 & 3) << 0; /* ?? */
	reg3 |= (0x07 & (i / 2 - 1)) << 2; /* rozlišení / distinction */
	reg3 |= (0x03 & 0) << 5; /* ?? */
	reg3 |= (0x01 & (fract >> 20)) << 7; /* +0.5 */
	reg3 |= (0x0f & n) << 8; /* hlavní rozsah / main range */

	switch (p->format)
	{ /* AGC */
	case MIRISDR_FORMAT_252_S16:
		reg3 |= (0x0f & 0x01) << 12;
		break;
	case MIRISDR_FORMAT_336_S16:
		reg3 |= (0x0f & 0x05) << 12;
		break;
	case MIRISDR_FORMAT_384_S16:
		reg3 |= (0x0f & 0x09) << 12;
		break;
	case MIRISDR_FORMAT_504_S16:
	case MIRISDR_FORMAT_504_S8:
		reg3 |= (0x0f & 0x0d) << 12;
		break;
	}

	reg3 |= (0x01 & 1) << 16; /* ?? */

	/* registr pro detailní nastavení vzorkovací frekvence */
	/* Registry settings for detailed sampling frequency */
	reg4 |= (0xfffff & fract) << 0;

	mirisdr_write_reg(p, 0x04, reg4);
	mirisdr_write_reg(p, 0x03, reg3);

	/* opětovné spuštění streamu */
	/* restart stream */
	if ((streaming) && (mirisdr_start_async(p) < 0)) {
		goto failed;
	}

	return 0;

	failed: return -1;
}

int mirisdr_set_sample_rate(mirisdr_dev_t *p, uint32_t rate)
{
    p->rate = rate;
    if (p->usb_pid == 0x3020u) {
        p->format_auto = MIRISDR_FORMAT_AUTO_OFF;
        p->format = rate <= 6048000u ? MIRISDR_FORMAT_252_S16 :
                    rate <= 8064000u ? MIRISDR_FORMAT_336_S16 :
                    rate <= 9216000u ? MIRISDR_FORMAT_384_S16 : MIRISDR_FORMAT_504_S16;
    }

    return mirisdr_set_hard(p);
}

uint32_t mirisdr_get_sample_rate(mirisdr_dev_t *p)
{
	return p->rate;
}

int mirisdr_set_sample_format(mirisdr_dev_t *p, const char *v)
{
	if (!strcmp(v, "AUTO"))
	{
		p->format_auto = MIRISDR_FORMAT_AUTO_ON;
	}
	else
	{
		p->format_auto = MIRISDR_FORMAT_AUTO_OFF;
		if (!strcmp(v, "252_S16")) {
			p->format = MIRISDR_FORMAT_252_S16;
		} else if (!strcmp(v, "336_S16")) {
			p->format = MIRISDR_FORMAT_336_S16;
		} else if (!strcmp(v, "384_S16")) {
			p->format = MIRISDR_FORMAT_384_S16;
		} else if (!strcmp(v, "504_S16")) {
			p->format = MIRISDR_FORMAT_504_S16;
		} else if (!strcmp(v, "504_S8")) {
			p->format = MIRISDR_FORMAT_504_S8;
		} else {
			fprintf(stderr, "unsupported format: %s\n", v);
			goto failed;
		}
	}

	return mirisdr_set_hard(p);

	failed: return -1;
}

const char *mirisdr_get_sample_format(mirisdr_dev_t *p)
{
	if (p->format_auto == MIRISDR_FORMAT_AUTO_ON) {
		return "AUTO";
	}

	switch (p->format)
	{
	case MIRISDR_FORMAT_252_S16:
		return "252_S16";
	case MIRISDR_FORMAT_336_S16:
		return "336_S16";
	case MIRISDR_FORMAT_384_S16:
		return "384_S16";
	case MIRISDR_FORMAT_504_S16:
		return "504_S16";
	case MIRISDR_FORMAT_504_S8:
		return "504_S8";
	}

	return "";
}
