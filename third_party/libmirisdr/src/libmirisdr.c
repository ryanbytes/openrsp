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

/* potřebné funkce */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#if !defined (_WIN32) || defined(__MINGW32__)
#include <unistd.h>
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#include "libusb.h"

#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

/* hlavní hlavičkový soubor */
#include "mirisdr.h"

/* interní definice */
#include "constants.h"
#include "structs.h"

/* interní funkce - inline */
#include "reg.c"
#include "adc.c"
#include "convert/base.c"
#include "async.c"
#include "devices.c"
#include "gain.c"
#include "hard.c"
#include "streaming.c"
#include "soft.c"
#include "sync.c"

static int mirisdr_rspduo_load_firmware_and_reopen(mirisdr_dev_t *dev);

int mirisdr_setup (mirisdr_dev_t **out_dev, mirisdr_dev_t *dev) {
    int r;

    if (libusb_kernel_driver_active(dev->dh, 0) == 1) {
        dev->driver_active = 1;

#ifdef DETACH_KERNEL_DRIVER
        if (!libusb_detach_kernel_driver(dev->dh, 0)) {
            fprintf(stderr, "Detached kernel driver\n");
        } else {
            fprintf(stderr, "Detaching kernel driver failed!");
            dev->driver_active = 0;
            goto failed;
        }
#else
        fprintf(stderr, "\nKernel driver is active, or device is "
                "claimed by second instance of libmirisdr."
                "\nIn the first case, please either detach"
                " or blacklist the kernel module\n"
                "(msi001 and msi2500), or enable automatic"
                " detaching at compile time.\n\n");
#endif
    } else {
        dev->driver_active = 0;
    }

    if ((r = libusb_claim_interface(dev->dh, 0)) < 0) {
        fprintf(stderr, "failed to claim miri usb device %u with code %d: %s\n", dev->index, r, libusb_error_name(r));
        if (r == LIBUSB_ERROR_BUSY) {
            fprintf(stderr, "Verify that the SDRplay background service is not running by `sudo systemctl stop sdrplay` and try again.\n");
        }

        goto failed;
    }

    /* Resetting an RSPduo invalidates its macOS USB identity often enough to
     * require a physical replug.  The vendor sequence claims and initializes
     * it in place, so preserve that behavior for PID 0x3020. */
    if (dev->usb_pid != 0x3020u) mirisdr_reset(dev);

    /* Legacy devices need an eager stop. The reset/reopen RSPduo path starts
     * from a fresh firmware generation and follows its own ordered sequence. */
    if (dev->usb_pid != 0x3020u) {
        mirisdr_streaming_stop(dev);
        mirisdr_adc_stop(dev);
    }

    /* inicializace tuneru */
    dev->freq = DEFAULT_FREQ;
    dev->rate = DEFAULT_RATE;
    dev->gain = DEFAULT_GAIN;
    dev->bulk_buffer_size = dev->usb_pid == 0x3020u ? 65536u : DEFAULT_BULK_BUFFER;
    dev->band = MIRISDR_BAND_VHF; // matches always the default frequency of 90 MHz

    dev->gain_reduction_lna = 0;
    dev->gain_reduction_mixer = 0;
    dev->gain_reduction_baseband = 43;
    dev->if_freq = MIRISDR_IF_ZERO;
    dev->format_auto = MIRISDR_FORMAT_AUTO_ON;
    dev->bandwidth = MIRISDR_BW_8MHZ;
    dev->xtal = MIRISDR_XTAL_24M;
    dev->bias = 0;

    dev->hw_flavour = MIRISDR_HW_DEFAULT;

    /* ISOC is more stable but works only on Unix systems */
#if !defined (_WIN32) || defined(__MINGW32__)
    dev->transfer = MIRISDR_TRANSFER_ISOC;
#else
    dev->transfer = MIRISDR_TRANSFER_BULK;
#endif

    if (dev->usb_pid == 0x3020u) {
        dev->transfer = MIRISDR_TRANSFER_BULK;
        if (mirisdr_rspduo_frontend_init(dev) < 0) {
            if (dev->firmware_attempted ||
                mirisdr_rspduo_load_firmware_and_reopen(dev) < 0) goto failed;
            return mirisdr_setup(out_dev, dev);
        }
        if (libusb_set_interface_alt_setting(dev->dh, 0, 3) < 0) goto failed;
    }

    if (dev->usb_pid != 0x3020u) {
        mirisdr_adc_init(dev);
        mirisdr_set_hard(dev);
        mirisdr_set_soft(dev);
        mirisdr_set_gain(dev);
    }

    *out_dev = dev;

    return 0;

failed:
    if (dev) {
        if (dev->dh) {
            libusb_release_interface(dev->dh, 0);
            libusb_close(dev->dh);
        }
        if (dev->ctx) libusb_exit(dev->ctx);
        free(dev);
    }

    return -1;
}

int mirisdr_configure_rspduo(mirisdr_dev_t *p, uint32_t rate, uint32_t freq,
                             uint32_t if_freq, uint32_t bandwidth,
                             int gain_reduction, unsigned int lna_state)
{
    if (!p || p->usb_pid != 0x3020u) return -1;
    if (if_freq != 0u && if_freq != 450000u && if_freq != 1620000u &&
        if_freq != 2048000u) return -1;

    p->rate = rate;
    p->freq = freq;
    p->format_auto = MIRISDR_FORMAT_AUTO_OFF;
    p->format = rate <= 6048000u ? MIRISDR_FORMAT_252_S16 :
                rate <= 8064000u ? MIRISDR_FORMAT_336_S16 :
                rate <= 9216000u ? MIRISDR_FORMAT_384_S16 : MIRISDR_FORMAT_504_S16;
    p->transfer = MIRISDR_TRANSFER_BULK;
    p->if_freq = if_freq == 0u ? MIRISDR_IF_ZERO :
                 if_freq == 450000u ? MIRISDR_IF_450KHZ :
                 if_freq == 1620000u ? MIRISDR_IF_1620KHZ : MIRISDR_IF_2048KHZ;
    p->bandwidth = bandwidth <= 200000u ? MIRISDR_BW_200KHZ :
                   bandwidth <= 300000u ? MIRISDR_BW_300KHZ :
                   bandwidth <= 600000u ? MIRISDR_BW_600KHZ :
                   bandwidth <= 1536000u ? MIRISDR_BW_1536KHZ :
                   bandwidth <= 5000000u ? MIRISDR_BW_5MHZ :
                   bandwidth <= 6000000u ? MIRISDR_BW_6MHZ :
                   bandwidth <= 7000000u ? MIRISDR_BW_7MHZ : MIRISDR_BW_8MHZ;

    int adc_result = mirisdr_adc_init(p);
    int hard_result = mirisdr_set_hard(p);
    int soft_result = mirisdr_set_soft(p);
    (void)lna_state;
    int gain_result = mirisdr_set_tuner_gain(p, 102 - gain_reduction);
    int frontend_result = mirisdr_rspduo_finish_tuner_a(p);
    int result = adc_result | hard_result | soft_result | gain_result | frontend_result;
    if (result < 0) {
        fprintf(stderr, "RSPduo configure failed adc=%d hard=%d soft=%d gain=%d frontend=%d\n",
                adc_result, hard_result, soft_result, gain_result, frontend_result);
    }
    return result < 0 ? -1 : 0;
}

static int mirisdr_rspduo_reset_and_reopen(mirisdr_dev_t *dev)
{
    libusb_device *device = libusb_get_device(dev->dh);
    uint8_t bus = libusb_get_bus_number(device);
    uint8_t ports[8];
    int port_count = libusb_get_port_numbers(device, ports, (int)sizeof(ports));
    uint8_t old_address = libusb_get_device_address(device);
    if (port_count < 0) return port_count;
    int reset_result = libusb_reset_device(dev->dh);
    if (reset_result < 0 && reset_result != LIBUSB_ERROR_NOT_FOUND) return reset_result;
    libusb_close(dev->dh);
    dev->dh = NULL;
    libusb_exit(dev->ctx);
    dev->ctx = NULL;
    usleep(100000);
    if (libusb_init(&dev->ctx) < 0) return LIBUSB_ERROR_OTHER;

    for (unsigned int attempt = 0; attempt < 80u; ++attempt) {
        libusb_device **devices = NULL;
        ssize_t count = libusb_get_device_list(dev->ctx, &devices);
        for (ssize_t i = 0; i < count; ++i) {
            struct libusb_device_descriptor descriptor;
            uint8_t candidate_ports[8];
            int candidate_count = libusb_get_port_numbers(
                devices[i], candidate_ports, (int)sizeof(candidate_ports));
            if (libusb_get_device_descriptor(devices[i], &descriptor) == 0 &&
                descriptor.idVendor == dev->usb_vid && descriptor.idProduct == dev->usb_pid &&
                libusb_get_bus_number(devices[i]) == bus && candidate_count == port_count &&
                memcmp(candidate_ports, ports, (size_t)port_count) == 0 &&
                libusb_open(devices[i], &dev->dh) == 0) break;
        }
        libusb_free_device_list(devices, 1);
        if (dev->dh != NULL) {
            if (getenv("OPENRSP_TRACE_USB") != NULL) {
                fprintf(stderr, "OPENRSP_USB reset result=%d old_address=%u new_address=%u attempt=%u\n",
                        reset_result, old_address,
                        libusb_get_device_address(libusb_get_device(dev->dh)), attempt);
            }
            usleep(1000000);
            return 0;
        }
        usleep(25000);
    }
    return LIBUSB_ERROR_NO_DEVICE;
}

static int mirisdr_rspduo_load_firmware_and_reopen(mirisdr_dev_t *dev)
{
    const char *path = getenv("OPENRSP_RSPDUO_FIRMWARE");
    if (!path || path[0] == '\0')
        path = "/Library/OpenRSP/0.1/firmware/rspduo-3020.bin";
    unsigned char firmware[6115];
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "RSPduo firmware is required for cold boot: %s\n", path);
        return -1;
    }
    size_t bytes = fread(firmware, 1, sizeof(firmware), file);
    int extra = fgetc(file);
    fclose(file);
    if (bytes != sizeof(firmware) || extra != EOF) {
        fprintf(stderr, "RSPduo firmware has invalid length: %s\n", path);
        return -1;
    }

    libusb_device *device = libusb_get_device(dev->dh);
    uint8_t bus = libusb_get_bus_number(device);
    uint8_t ports[8];
    int port_count = libusb_get_port_numbers(device, ports, (int)sizeof(ports));
    if (port_count < 0) return port_count;
    dev->firmware_attempted = 1;
    int first = libusb_control_transfer(dev->dh, 0x40, 0x44, 0x0000, 0x0000,
                                        firmware, 4096, CTRL_TIMEOUT);
    int second = libusb_control_transfer(dev->dh, 0x40, 0x44, 0x1000, 0x0000,
                                         firmware + 4096, 2019, CTRL_TIMEOUT);
    if (first != 4096 || second != 2019) return -1;
    if (libusb_control_transfer(dev->dh, 0x40, 0x41, 0x8008, 0x0000,
                                NULL, 0, CTRL_TIMEOUT) < 0) return -1;
    int reset = libusb_control_transfer(dev->dh, 0x40, 0x40, 0x0001, 0x0000,
                                        NULL, 0, CTRL_TIMEOUT);
    if (reset < 0 && reset != LIBUSB_ERROR_NO_DEVICE &&
        reset != LIBUSB_ERROR_NOT_FOUND) return reset;

    libusb_close(dev->dh);
    dev->dh = NULL;
    libusb_exit(dev->ctx);
    dev->ctx = NULL;
    usleep(250000);
    if (libusb_init(&dev->ctx) < 0) return LIBUSB_ERROR_OTHER;
    for (unsigned int attempt = 0; attempt < 80u; ++attempt) {
        libusb_device **devices = NULL;
        ssize_t count = libusb_get_device_list(dev->ctx, &devices);
        for (ssize_t i = 0; i < count; ++i) {
            struct libusb_device_descriptor descriptor;
            uint8_t candidate_ports[8];
            int candidate_count = libusb_get_port_numbers(
                devices[i], candidate_ports, (int)sizeof(candidate_ports));
            if (libusb_get_device_descriptor(devices[i], &descriptor) == 0 &&
                descriptor.idVendor == dev->usb_vid && descriptor.idProduct == dev->usb_pid &&
                libusb_get_bus_number(devices[i]) == bus && candidate_count == port_count &&
                memcmp(candidate_ports, ports, (size_t)port_count) == 0 &&
                libusb_open(devices[i], &dev->dh) == 0) break;
        }
        libusb_free_device_list(devices, 1);
        if (dev->dh != NULL) {
            usleep(250000);
            return 0;
        }
        usleep(25000);
    }
    return LIBUSB_ERROR_NO_DEVICE;
}

int mirisdr_open (mirisdr_dev_t **p, uint32_t index) {
    mirisdr_dev_t *dev = NULL;
    libusb_device **list, *device = NULL;
    struct libusb_device_descriptor dd;
    ssize_t i, i_max;
    size_t count = 0;
    int r;

    *p = NULL;

    if (!(dev = malloc(sizeof(*dev)))) return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

    /* ostatní parametry */
    dev->index = index;

#ifdef __ANDROID__
    /* LibUSB does not support device discovery on android */
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif

    libusb_init(&dev->ctx);
    i_max = libusb_get_device_list(dev->ctx, &list);

    for (i = 0; i < i_max; i++) {
        libusb_get_device_descriptor(list[i], &dd);

        if ((mirisdr_device_get(dd.idVendor, dd.idProduct)) &&
            (count++ == index)) {
            device = list[i];
            break;
        }
    }

    /* nenašli jsme zařízení */
    if (!device) {
        libusb_free_device_list(list, 1);
        fprintf( stderr, "no miri device %u found\n", dev->index);
        goto failed;
    }

    /* otevření zařízení */
    if ((r = libusb_open(device, &dev->dh)) < 0) {
        libusb_free_device_list(list, 1);
        fprintf( stderr, "failed to open miri usb device %u with code %d\n", dev->index, r);
        goto failed;
    }

    dev->usb_vid = dd.idVendor;
    dev->usb_pid = dd.idProduct;

    libusb_free_device_list(list, 1);
    list = NULL;

    if (dev->usb_pid == 0x3020u &&
        getenv("OPENRSP_DISRUPTIVE_USB_RESET") != NULL &&
        mirisdr_rspduo_reset_and_reopen(dev) < 0) goto failed;

    return mirisdr_setup(p, dev);

failed:
    if (dev) {
        if (dev->dh) {
            libusb_close(dev->dh);
        }
        if (dev->ctx) libusb_exit(dev->ctx);
        free(dev);
    }

    return -1;
}

int mirisdr_open_fd (mirisdr_dev_t **p, int fd) {
    mirisdr_dev_t *dev = NULL;
    libusb_device **list, *device = NULL;
    struct libusb_device_descriptor dd;
    ssize_t i, i_max;
    size_t count = 0;
    int r;

    *p = NULL;

    if (!(dev = malloc(sizeof(*dev)))) return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

#ifdef __ANDROID__
    /* LibUSB does not support device discovery on android */
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif

    r = libusb_init(&dev->ctx);
    if(r < 0){
        free(dev);
        return -1;
    }
    
    r = libusb_wrap_sys_device(dev->ctx, (intptr_t)fd, &dev->dh);
    if (r || dev->dh == NULL){
        free(dev);
        return -1;
    }

    return mirisdr_setup(p, dev);
}

int mirisdr_close (mirisdr_dev_t *p) {
    if (!p) goto failed;

    /* ukončení async čtení okamžitě */
    mirisdr_cancel_async_now(p);

    // similar to rtl-sdr
#if defined(_WIN32) && !defined(__MINGW32__)
            Sleep(1);
#else
            usleep(1000);
#endif

    /* deinicializace tuneru */
    if (p->dh)
    {
        /* OpenRSP: leave the device in its idle interface state so another
         * process can claim it without requiring a physical/USB reset. */
        mirisdr_streaming_stop(p);
        if (p->usb_pid == 0x3020u) {
            usleep(110000);
            (void)mirisdr_rspduo_shutdown(p);
        } else {
            mirisdr_adc_stop(p);
        }
        (void)libusb_set_interface_alt_setting(p->dh, 0, 0);
        libusb_release_interface(p->dh, 0);

#ifdef DETACH_KERNEL_DRIVER
        if (p->driver_active) {
            if (!libusb_attach_kernel_driver(p->dh, 0))
                fprintf(stderr, "Reattached kernel driver\n");
            else
                fprintf(stderr, "Reattaching kernel driver failed!\n");
        }
#endif
        libusb_close(p->dh);
    }

    if (p->ctx) libusb_exit(p->ctx);

    if (p->samples) free(p->samples);

    free(p);

    return 0;

failed:
    return -1;
}

int mirisdr_reset (mirisdr_dev_t *p) {
    int r;

    if (!p) goto failed;
    if (!p->dh) goto failed;

    /* měli bychom uvolnit zařízení předem? */

    if ((r = libusb_reset_device(p->dh)) < 0) {
        fprintf( stderr, "failed to reset miri usb device %u with code %d\n", p->index, r);
        goto failed;
    }

    return 0;

failed:
    return -1;
}

int mirisdr_reset_buffer (mirisdr_dev_t *p) {
    if (!p) goto failed;
    if (!p->dh) goto failed;

    /* zatím není jasné k čemu by bylo, proto pouze provedeme reset async části */
    mirisdr_stop_async(p);
    mirisdr_start_async(p);

    return 0;

failed:
    return -1;
}

int mirisdr_get_usb_strings (mirisdr_dev_t *dev, char *manufact, char *product, char *serial) {
(void) dev;
    fprintf( stderr, "mirisdr_get_usb_strings not implemented yet\n");

    memset(manufact, 0, 256);
    memset(product, 0, 256);
    memset(serial, 0, 256);

    return 0;
}

int mirisdr_set_hw_flavour (mirisdr_dev_t *p, mirisdr_hw_flavour_t hw_flavour) {
    if (!p) goto failed;

    p->hw_flavour = hw_flavour;
    return 0;

failed:
    return -1;
}
