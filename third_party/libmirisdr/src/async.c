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

#include "async.h"

/* uložení dat */
static int mirisdr_feed_async (mirisdr_dev_t *p, unsigned char *samples, uint32_t bytes) {
    uint32_t i;

    if (!p) goto failed;
    if (!p->cb) goto failed;

    /* automatická velikost */
    if (!p->xfer_out_len) {
        /* přímé zaslání */
        p->cb(samples, bytes, p->cb_ctx);
    /* fixní velikost bufferu bez předchozích dat */
    } else if (p->xfer_out_pos == 0) {
        /* buffer přesně odpovídá - málo časté, přímé zaslání */
        if (bytes == p->xfer_out_len) {
            p->cb(samples, bytes, p->cb_ctx);
        /* buffer je kratší */
        } else if (bytes < p->xfer_out_len) {
            memcpy(p->xfer_out, samples, bytes);
            p->xfer_out_pos = bytes;
        /* buffer je delší */
        } else {
            /* muže být i x násobkem délky */
            for (i = 0;; i+= p->xfer_out_len) {
                if (i + p->xfer_out_len > bytes) {
                    if (bytes > i) {
                        memcpy(p->xfer_out, samples + i, bytes - i);
                        p->xfer_out_pos = bytes - i;
                    }
                    break;
                }
                p->cb(samples + i, p->xfer_out_len, p->cb_ctx);
            }
        }
    /* data jsou přesně, využije se interní buffer */
    } else if (p->xfer_out_pos + bytes == p->xfer_out_len) {
        memcpy(p->xfer_out + p->xfer_out_pos, samples, bytes);
        p->cb(p->xfer_out, p->xfer_out_len, p->cb_ctx);
        p->xfer_out_pos = 0;
    /* není dostatek dat */
    } else if (p->xfer_out_pos + bytes < p->xfer_out_len) {
        memcpy(p->xfer_out + p->xfer_out_pos, samples, bytes);
        p->xfer_out_pos+= bytes;
    /* dat je více než potřebujeme, nejsložitější případ */
    } else {
        memcpy(p->xfer_out + p->xfer_out_pos, samples, p->xfer_out_len - p->xfer_out_pos);
        p->cb(p->xfer_out, p->xfer_out_len, p->cb_ctx);
        for (i = p->xfer_out_len - p->xfer_out_pos;; i+= p->xfer_out_len) {
            if (i + p->xfer_out_len > bytes) {
                if (bytes > i) {
                    memcpy(p->xfer_out, samples + i, bytes - i);
                    p->xfer_out_pos = bytes - i;
                } else {
                    p->xfer_out_pos = 0;
                }
                break;
            }
            p->cb(samples + i, p->xfer_out_len, p->cb_ctx);
        }
    }
    return 0;

failed:
    return -1;
}

static uint8_t *samples_realloc(mirisdr_dev_t *p, int size)
{
    if(p->samples_size < size)
    {
        if(p->samples)
            free(p->samples);
        p->samples=malloc(size);
        p->samples_size=size;
    }
    return p->samples;
}

/* volání pro zasílání dat */
static void LIBUSB_CALL _libusb_callback (struct libusb_transfer *xfer) {
    size_t i;
    int len, bytes = 0;
    int resubmitted = 0;
    int transfer_length = xfer->actual_length;
    unsigned char *transfer_buffer = xfer->buffer;
    unsigned char *owned_buffer = NULL;
    static unsigned char *iso_packet_buf;
    mirisdr_dev_t *p = (mirisdr_dev_t*) xfer->user_data;
    uint8_t *samples = p->samples;

    if (!p) goto failed;

    /* zpracujeme pouze kompletní přenos */
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (p->usb_pid == 0x3020u && xfer->type == LIBUSB_TRANSFER_TYPE_BULK)
            p->bulk_recovery_attempts = 0u;
        if (p->usb_pid == 0x3020u && xfer->type == LIBUSB_TRANSFER_TYPE_BULK &&
            transfer_length > 0) {
            owned_buffer = malloc((size_t)transfer_length);
            if (owned_buffer == NULL) goto failed;
            memcpy(owned_buffer, xfer->buffer, (size_t)transfer_length);
            transfer_buffer = owned_buffer;
            if (libusb_submit_transfer(xfer) < 0) goto failed;
            resubmitted = 1;
        }
        /*
         * Určení správné velikosti bufferu, tato část musí být provedena
         * v jednom kroku, jinak může dojít ke změně formátu uprostřed procesu,
         * druhá možnost je používat lock.
         */
        switch (xfer->type) {
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
            switch (p->format) {
            case MIRISDR_FORMAT_252_S16:
                samples = samples_realloc(p, 504 * DEFAULT_ISO_BUFFERS * DEFAULT_ISO_PACKETS * 2);
                for (i = 0; i < DEFAULT_ISO_PACKETS; i++) {
                    struct libusb_iso_packet_descriptor *packet = &xfer->iso_packet_desc[i];

                    /* buffer_simple je pouze pro stejně velké pakety */
                    if ((packet->actual_length > 0) &&
                        (iso_packet_buf = libusb_get_iso_packet_buffer_simple(xfer, i))) {
                        /* menší velikost než 3072 nevadí, je běžný násobek 1024, cokoliv jiného je chyba */
                        len = mirisdr_samples_convert_252_s16(p, iso_packet_buf, samples + bytes, packet->actual_length);
                        bytes+= len;
                    }
                }
                break;
            case MIRISDR_FORMAT_336_S16:
                samples = samples_realloc(p, 672 * DEFAULT_ISO_BUFFERS * DEFAULT_ISO_PACKETS * 2);
                for (i = 0; i < DEFAULT_ISO_PACKETS; i++) {
                    struct libusb_iso_packet_descriptor *packet = &xfer->iso_packet_desc[i];
                    if ((packet->actual_length > 0) &&
                        (iso_packet_buf = libusb_get_iso_packet_buffer_simple(xfer, i))) {
                        len = mirisdr_samples_convert_336_s16(p, iso_packet_buf, samples + bytes, packet->actual_length);
                        bytes+= len;
                    }
                }
                break;
            case MIRISDR_FORMAT_384_S16:
                samples = samples_realloc(p, 768 * DEFAULT_ISO_BUFFERS * DEFAULT_ISO_PACKETS * 2);
                for (i = 0; i < DEFAULT_ISO_PACKETS; i++) {
                    struct libusb_iso_packet_descriptor *packet = &xfer->iso_packet_desc[i];
                    if ((packet->actual_length > 0) &&
                        (iso_packet_buf = libusb_get_iso_packet_buffer_simple(xfer, i))) {
                        len = mirisdr_samples_convert_384_s16(p, iso_packet_buf, samples + bytes, packet->actual_length);
                        bytes+= len;
                    }
                }
                break;
            case MIRISDR_FORMAT_504_S16:
                samples = samples_realloc(p, 1008 * DEFAULT_ISO_BUFFERS * DEFAULT_ISO_PACKETS * 2);
                for (i = 0; i < DEFAULT_ISO_PACKETS; i++) {
                    struct libusb_iso_packet_descriptor *packet = &xfer->iso_packet_desc[i];
                    if ((packet->actual_length > 0) &&
                        (iso_packet_buf = libusb_get_iso_packet_buffer_simple(xfer, i))) {
                        len = mirisdr_samples_convert_504_s16(p, iso_packet_buf, samples + bytes, packet->actual_length);
                        bytes+= len;
                    }
                }
                break;
            case MIRISDR_FORMAT_504_S8:
                samples = samples_realloc(p, 1008 * DEFAULT_ISO_BUFFERS * DEFAULT_ISO_PACKETS);
                for (i = 0; i < DEFAULT_ISO_PACKETS; i++) {
                    struct libusb_iso_packet_descriptor *packet = &xfer->iso_packet_desc[i];
                    if ((packet->actual_length > 0) &&
                        (iso_packet_buf = libusb_get_iso_packet_buffer_simple(xfer, i))) {
                        len = mirisdr_samples_convert_504_s8(p, iso_packet_buf, samples + bytes, packet->actual_length);
                        bytes+= len;
                    }
                }
                break;
            }
            break;
        case LIBUSB_TRANSFER_TYPE_BULK:
            switch (p->format) {
            case MIRISDR_FORMAT_252_S16:
                samples = samples_realloc(p, (p->bulk_buffer_size / 1024u) * 1008u);
                bytes = mirisdr_samples_convert_252_s16(p, transfer_buffer, samples, transfer_length);
                break;
            case MIRISDR_FORMAT_336_S16:
                samples = samples_realloc(p, (p->bulk_buffer_size / 1024u) * 1344u);
                bytes = mirisdr_samples_convert_336_s16(p, transfer_buffer, samples, transfer_length);
                break;
            case MIRISDR_FORMAT_384_S16:
                samples = samples_realloc(p, (p->bulk_buffer_size / 1024u) * 1536u);
                bytes = mirisdr_samples_convert_384_s16(p, transfer_buffer, samples, transfer_length);
                break;
            case MIRISDR_FORMAT_504_S16:
                samples = samples_realloc(p, (p->bulk_buffer_size / 1024u) * 2016u);
                bytes = mirisdr_samples_convert_504_s16(p, transfer_buffer, samples, transfer_length);
                break;
            case MIRISDR_FORMAT_504_S8:
                samples = samples_realloc(p, (p->bulk_buffer_size / 1024u) * 1008u);
                bytes = mirisdr_samples_convert_504_s8(p, transfer_buffer, samples, transfer_length);
                break;
            }
            break;
        default:
            fprintf( stderr, "not isoc or bulk transfer type on usb device: %u\n", p->index);
            goto failed;
        }

        if (bytes > 0 && p->usb_pid == 0x3020u) {
            /* RSPduo single-tuner USB words are two real ADC lanes, not an
             * interleaved complex pair. Both A and B single-tuner routing
             * place the active ADC on lane 1. Produce analytic IQ before
             * exposing the ordinary libmirisdr callback. */
            mirisdr_rspduo_analytic_process(&p->analytic_state,
                                            (int16_t *)samples,
                                            (size_t)bytes / (2u * sizeof(int16_t)), 1u);
        }
        if (bytes > 0) mirisdr_feed_async(p, samples, bytes);

        if (!resubmitted && xfer->type == LIBUSB_TRANSFER_TYPE_BULK)
        {
            if(p->usb_pid != 0x3020u && p->sync_loss_cnt > (int)p->xfer_buf_num)
            {
                p->sync_loss_cnt = -p->xfer_buf_num +1;
                xfer->length = (int)p->bulk_buffer_size - 512;
                fprintf(stderr,"libmirisdr: Sync lost. Trying to synchronize.\n");
            } else {
                xfer->length = (int)p->bulk_buffer_size;
                if (p->usb_pid == 0x3020u) p->sync_loss_cnt = 0;
            }
        }
        /* pokračujeme dalším přenosem */
        if (!resubmitted && libusb_submit_transfer(xfer) < 0) {
            fprintf( stderr, "error re-submitting URB on device %u\n", p->index);
            goto failed;
        }
    } else if (p->usb_pid == 0x3020u && xfer->type == LIBUSB_TRANSFER_TYPE_BULK &&
               (xfer->status == LIBUSB_TRANSFER_OVERFLOW ||
                xfer->status == LIBUSB_TRANSFER_STALL) &&
               p->bulk_recovery_attempts < 3u) {
        ++p->bulk_recovery_attempts;
        int recovery = 0;
        if (xfer->status == LIBUSB_TRANSFER_STALL)
            recovery = libusb_clear_halt(p->dh, 0x81u);
        if (recovery == 0) recovery = libusb_submit_transfer(xfer);
        if (recovery == 0) {
            fprintf(stderr,
                    "recovering RSPduo bulk transfer status %d attempt %u on device %u\n",
                    xfer->status, p->bulk_recovery_attempts, p->index);
            return;
        }
        fprintf(stderr,
                "RSPduo bulk recovery failed status %d attempt %u result %d on device %u\n",
                xfer->status, p->bulk_recovery_attempts, recovery, p->index);
        goto failed;
    } else if (xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf( stderr, "error async transfer status %d on device %u\n", xfer->status, p->index);
        goto failed;
    }

    free(owned_buffer);
    return;

failed:
    free(owned_buffer);
    mirisdr_cancel_async(p);
    /* stav failed má absolutní přednost */
    p->async_status = MIRISDR_ASYNC_FAILED;
}

/* ukončení async části */
int mirisdr_cancel_async (mirisdr_dev_t *p) {
    if (!p) goto failed;

    switch (p->async_status) {
    case MIRISDR_ASYNC_INACTIVE:
    case MIRISDR_ASYNC_CANCELING:
        goto canceled;
    case MIRISDR_ASYNC_RUNNING:
    case MIRISDR_ASYNC_PAUSED:
        p->async_status = MIRISDR_ASYNC_CANCELING;
        break;
    case MIRISDR_ASYNC_FAILED:
        goto failed;
    }

    return 0;

failed:
    return -1;

canceled:
    return -2;
}

/* ukončení async části včetně čekání */
int mirisdr_cancel_async_now (mirisdr_dev_t *p) {
    if (!p) goto failed;

    switch (p->async_status) {
    case MIRISDR_ASYNC_INACTIVE:
        goto done;
    case MIRISDR_ASYNC_CANCELING:
        break;
    case MIRISDR_ASYNC_RUNNING:
    case MIRISDR_ASYNC_PAUSED:
        p->async_status = MIRISDR_ASYNC_CANCELING;
        break;
    case MIRISDR_ASYNC_FAILED:
        goto failed;
    }

    /* cyklujeme dokud není vše ukončeno */
    while ((p->async_status != MIRISDR_ASYNC_INACTIVE) &&
           (p->async_status != MIRISDR_ASYNC_FAILED))
#if defined (_WIN32) && !defined(__MINGW32__)
    Sleep(20);
#else
    usleep(20000);
#endif

done:
    return 0;

failed:
    return -1;
}

/* alokace asynchronních bufferů */
static int mirisdr_async_alloc (mirisdr_dev_t *p) {
    size_t i;

    if (!p->xfer) {
        p->xfer = malloc(p->xfer_buf_num * sizeof(*p->xfer));

        for (i = 0; i < p->xfer_buf_num; i++) {
            switch (p->transfer) {
            case MIRISDR_TRANSFER_BULK:
                p->xfer[i] = libusb_alloc_transfer(0);
                break;
            case MIRISDR_TRANSFER_ISOC:
                p->xfer[i] = libusb_alloc_transfer(DEFAULT_ISO_PACKETS);
                break;
            }
        }
    }

    if (!p->xfer_buf) {
        p->xfer_buf = malloc(p->xfer_buf_num * sizeof(*p->xfer_buf));

        for (i = 0; i < p->xfer_buf_num; i++) {
            switch (p->transfer) {
            case MIRISDR_TRANSFER_BULK:
                p->xfer_buf[i] = malloc(p->bulk_buffer_size);
                break;
            case MIRISDR_TRANSFER_ISOC:
                p->xfer_buf[i] = malloc(DEFAULT_ISO_BUFFER * DEFAULT_ISO_BUFFERS * DEFAULT_ISO_PACKETS);
                break;
            }
        }
    }

    if ((!p->xfer_out) &&
        (p->xfer_out_len)) {
        p->xfer_out = malloc(p->xfer_out_len * sizeof(*p->xfer_out));
    }

    return 0;
}

/* uvolnění asynchronních bufferů */
static int mirisdr_async_free (mirisdr_dev_t *p) {
    size_t i;

    if (p->xfer) {
        for (i = 0; i < p->xfer_buf_num; i++) {
            if (p->xfer[i]) libusb_free_transfer(p->xfer[i]);
        }

        free(p->xfer);
        p->xfer = NULL;
    }

    if (p->xfer_buf) {
        for (i = 0; i < p->xfer_buf_num; i++) {
            if (p->xfer_buf[i]) free(p->xfer_buf[i]);
        }

        free(p->xfer_buf);
        p->xfer_buf = NULL;
    }

    if (p->xfer_out) {
        free(p->xfer_out);
        p->xfer_out = NULL;
    }

    return 0;
}

/* Cancel every transfer that libusb still owns before freeing its storage.
 * A transfer that already completed with an error returns NOT_FOUND from
 * libusb_cancel_transfer and is safe to leave alone. */
static int mirisdr_async_cancel_pending(mirisdr_dev_t *p)
{
    struct timeval timeout = {1, 0};
    for (unsigned int pass = 0; pass < 8u; ++pass) {
        int cancellation_requested = 0;
        for (size_t i = 0; i < p->xfer_buf_num; ++i) {
            if (!p->xfer || !p->xfer[i] ||
                p->xfer[i]->status == LIBUSB_TRANSFER_CANCELLED) continue;
            int result = libusb_cancel_transfer(p->xfer[i]);
            if (result == 0) cancellation_requested = 1;
            else if (result != LIBUSB_ERROR_NOT_FOUND) return result;
        }
        if (!cancellation_requested) return 0;
        int result = libusb_handle_events_timeout(p->ctx, &timeout);
        if (result < 0 && result != LIBUSB_ERROR_INTERRUPTED) return result;
    }
    return LIBUSB_ERROR_TIMEOUT;
}

/* spuštění async části */
int mirisdr_read_async (mirisdr_dev_t *p, mirisdr_read_async_cb_t cb, void *ctx, uint32_t num, uint32_t len) {
    size_t i;
    int r;
    int transfer_failed = 0;
    struct timeval tv = {1, 0};

    if (!p) goto failed;
    if (!p->dh) goto failed;

    /* nedovolíme spustit jiný stav než neaktivní */
    if (p->async_status != MIRISDR_ASYNC_INACTIVE) goto failed;

    p->cb = cb;
    p->cb_ctx = ctx;

    p->xfer_buf_num = (num == 0) ? DEFAULT_BUF_NUMBER : num;
    /* jde o fixní velikost výstupního bufferu */
    p->xfer_out_len = (len == 0) ? 0 : len;
    p->xfer_out_pos = 0;
#if MIRISDR_DEBUG >= 1
    fprintf( stderr, "async read on device %u, buffers: %lu, output size: ",
                                p->index, (long)p->xfer_buf_num);
    if (p->xfer_out_len) {
        fprintf( stderr, "%lu", (long)p->xfer_out_len);
    } else {
        fprintf( stderr, "auto");
    }
#endif
    p->sync_loss_cnt = 0;
    p->addr_valid = 0;
    if (p->usb_pid == 0x3020u)
        mirisdr_rspduo_analytic_reset(&p->analytic_state);
    /* použití správného rozhraní které zasílá data - není kritické */
    switch (p->transfer) {
    case MIRISDR_TRANSFER_BULK:
#if MIRISDR_DEBUG >= 1
        fprintf( stderr, ", transfer: bulk\n");
#endif
        if (p->usb_pid != 0x3020u &&
            (r = libusb_set_interface_alt_setting(p->dh, 0, 3)) < 0) {
            fprintf( stderr, "failed to use alternate setting for Bulk mode on miri usb device %u with code %d\n", p->index, r);
        }
        break;
    case MIRISDR_TRANSFER_ISOC:
#if MIRISDR_DEBUG >= 1
        fprintf( stderr, ", transfer: isochronous\n");
#endif
        if ((r = libusb_set_interface_alt_setting(p->dh, 0, 1)) < 0) {
            fprintf( stderr, "failed to use alternate setting for Isochronous mode on miri usb device %u with code %d\n", p->index, r);
        }
        break;
    default:
        fprintf( stderr, "\nunsupported transfer type on miri usb device %u\n", p->index);
        goto failed;
    }

    mirisdr_async_alloc(p);

    /* The RSPduo firmware needs its bulk engine explicitly stopped and given
     * time to drain after tuner configuration. */
    if (p->usb_pid == 0x3020u) {
        if (mirisdr_streaming_stop(p) < 0) goto failed_free;
        usleep(110000);
        /* A process killed during an active bulk stream can leave endpoint
         * 0x81 halted even after the next process claims and reconfigures the
         * interface. Submitting into that state fails with LIBUSB_ERROR_PIPE
         * and previously required a physical reconnect. Clear only this
         * receiver's claimed IQ endpoint before allocating new transfers. */
        r = libusb_clear_halt(p->dh, 0x81u);
        if (r < 0) {
            fprintf(stderr, "failed to clear RSPduo bulk endpoint halt: %d\n", r);
            goto failed_free;
        }
    }

    /* spustíme přenosy */
    for (i = 0; i < p->xfer_buf_num; i++) {
        switch (p->transfer) {
        case MIRISDR_TRANSFER_BULK:
            libusb_fill_bulk_transfer(p->xfer[i],
                                      p->dh,
                                      0x81,
                                      p->xfer_buf[i],
                                      (int)p->bulk_buffer_size,
                                      _libusb_callback,
                                      (void*) p,
                                      p->usb_pid == 0x3020u ? 4000u : DEFAULT_BULK_TIMEOUT);
            break;
        case MIRISDR_TRANSFER_ISOC:
            libusb_fill_iso_transfer(p->xfer[i],
                                     p->dh,
                                     0x81,
                                     p->xfer_buf[i],
                                     DEFAULT_ISO_BUFFER * DEFAULT_ISO_BUFFERS * DEFAULT_ISO_PACKETS,
                                     DEFAULT_ISO_PACKETS,
                                     _libusb_callback,
                                     (void*) p,
                                     DEFAULT_ISO_TIMEOUT);
            libusb_set_iso_packet_lengths(p->xfer[i], DEFAULT_ISO_BUFFER * DEFAULT_ISO_BUFFERS);
            break;
        default:
            fprintf( stderr, "unsupported transfer type\n");
            goto failed_free;
        }

        r = libusb_submit_transfer(p->xfer[i]);

		if (r < 0) {
			fprintf(stderr, "Failed to submit transfer %lu reason: %d\n", i, r);
			(void)mirisdr_async_cancel_pending(p);
			goto failed_free;
		}
    }

    /* spustíme streamování dat */
    mirisdr_streaming_start(p);

    p->async_status = MIRISDR_ASYNC_RUNNING;

    while (p->async_status != MIRISDR_ASYNC_INACTIVE) {
        /* počkáme na další událost */
        if ((r = libusb_handle_events_timeout(p->ctx, &tv)) < 0) {
            fprintf( stderr, "libusb_handle_events returned: %d\n", r);
            if (r == LIBUSB_ERROR_INTERRUPTED) continue; /* stray */
            goto failed_free;
        }

        /* dochází k ukončení */
        if (p->async_status == MIRISDR_ASYNC_FAILED) {
            transfer_failed = 1;
            p->async_status = MIRISDR_ASYNC_CANCELING;
        }
        if (p->async_status == MIRISDR_ASYNC_CANCELING) {
            if (p->xfer && mirisdr_async_cancel_pending(p) < 0) transfer_failed = 1;
            p->async_status = MIRISDR_ASYNC_INACTIVE;
            break;
        }
    }

    /* dealokujeme buffer */
    mirisdr_async_free(p);

    /* ukončíme streamování dat */
#if defined (_WIN32) && !defined(__MINGW32__)
    Sleep(20);
#else
    usleep(20000);
#endif
    /* RSPduo is stopped by the API owner before transfer cancellation.  A
     * second synchronous control transfer here races Darwin libusb teardown. */
    if (p->usb_pid != 0x3020u) mirisdr_streaming_stop(p);
    /* je vhodné ukončit i adc, jenže pak by při dalším otevření bylo nutné provést inicializaci */

    return transfer_failed ? -1 : 0;

failed_free:
    mirisdr_async_free(p);

failed:
    return -1;
}

/* spuštění streamování */
int mirisdr_start_async (mirisdr_dev_t *p) {
    size_t i;

    /* nedovolíme jiný stav než pozastavený */
    if (p->async_status != MIRISDR_ASYNC_PAUSED) goto failed;

    /* reset interního bufferu */
    p->xfer_out_pos = 0;

    for (i = 0; i < p->xfer_buf_num; i++) {
        if (!p->xfer[i]) continue;

        if (libusb_submit_transfer(p->xfer[i])< 0) {
            goto failed;
        }
    }

    if (p->async_status != MIRISDR_ASYNC_PAUSED) goto failed;

    mirisdr_streaming_start(p);

    p->async_status = MIRISDR_ASYNC_RUNNING;

    return 0;

failed:
    return -1;
}

/* zastavení streamování */
int mirisdr_stop_async (mirisdr_dev_t *p) {
    /* nedovolíme jiný stav než spuštěný */
    if (p->async_status != MIRISDR_ASYNC_RUNNING) goto failed;

    if (mirisdr_async_cancel_pending(p) < 0) goto failed;

    if (p->async_status != MIRISDR_ASYNC_RUNNING) goto failed;

#if defined (_WIN32) && !defined(__MINGW32__)
    Sleep(20);
#else
    usleep(20000);
#endif
    mirisdr_streaming_stop(p);

    p->async_status = MIRISDR_ASYNC_PAUSED;

    return 0;

failed:
    return -1;
}
