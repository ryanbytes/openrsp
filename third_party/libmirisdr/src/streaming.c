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

int mirisdr_streaming_start (mirisdr_dev_t *p) {
    if (!p) goto failed;
    if (!p->dh) goto failed;

    uint8_t request_type = p->usb_pid == 0x3020u ? 0x40u : 0x42u;
    if (getenv("OPENRSP_TRACE_USB") != NULL)
        fprintf(stderr, "OPENRSP_USB control type=%02x request=43 value=0000 index=0000\n",
                request_type);
    int result = libusb_control_transfer(
        p->dh, request_type, 0x43, 0x0, 0x0, NULL, 0, CTRL_TIMEOUT);
    if (result < 0) {
        fprintf(stderr, "failed to start streaming on device %u: %d\n",
                p->index, result);
        return result;
    }
    p->streaming_active = 1;
    return 0;

failed:
    return -1;
}

int mirisdr_streaming_stop (mirisdr_dev_t *p) {
    if (!p) goto failed;
    if (!p->dh) goto failed;

    uint8_t request_type = p->usb_pid == 0x3020u ? 0x40u : 0x42u;
    if (getenv("OPENRSP_TRACE_USB") != NULL)
        fprintf(stderr, "OPENRSP_USB control type=%02x request=45 value=0000 index=0000\n",
                request_type);
    int result = libusb_control_transfer(
        p->dh, request_type, 0x45, 0x0, 0x0, NULL, 0, CTRL_TIMEOUT);
    if (result < 0) {
        fprintf(stderr, "failed to stop streaming on device %u: %d\n",
                p->index, result);
        return result;
    }
    p->streaming_active = 0;
    return 0;

failed:
    return -1;
}
