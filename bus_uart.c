/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2021, Silicon Labs
 * Main authors:
 *     - Jérôme Pouiller <jerome.pouiller@silabs.com>
 */
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "log.h"
#include "utils.h"
#include "wsbr.h"
#include "bus_uart.h"

static uint16_t crc16(const uint8_t *data, int len)
{
    uint8_t x;
    uint16_t crc = 0;

    while (len--) {
        x = crc >> 8 ^ *data++;
        x ^= x >> 4;
        crc <<= 8;
        crc ^= ((uint16_t)x) << 12;
        crc ^= ((uint16_t)x) << 5;
        crc ^= x;
    }
    return crc;
}

int wsbr_uart_open(const char *device, int bitrate, bool hardflow)
{
    static const struct {
        int val;
        int symbolic;
    } conversion[] = {
        { 9600, B9600 },
        { 19200, B19200 },
        { 38400, B38400 },
        { 57600, B57600 },
        { 115200, B115200 },
        { 230400, B230400 },
        { 460800, B460800 },
        { 921600, B921600 },
    };
    struct termios tty;
    int sym_bitrate = -1;
    int fd, i;

    fd = open(device, O_RDWR);
    if (fd < 0)
        FATAL(1, "%s: %m", device);

    if(tcgetattr(fd, &tty) == -1)
        FATAL(1, "tcgetattr: %m");
    for (i = 0; i < ARRAY_SIZE(conversion); i++)
        if (conversion[i].val == bitrate)
            sym_bitrate = conversion[i].symbolic;
    if (sym_bitrate < 0)
        FATAL(1, "invalid bitrate: %d", bitrate);
    cfsetispeed(&tty, sym_bitrate);
    cfsetospeed(&tty, sym_bitrate);
    cfmakeraw(&tty);
    tty.c_cc[VTIME] = 0;
    tty.c_cc[VMIN] = 1;
    tty.c_iflag &= ~IXON;
    tty.c_iflag &= ~IXOFF;
    tty.c_iflag &= ~IXANY;
    tty.c_cflag &= ~HUPCL;
    tty.c_cflag |= CLOCAL;
    if (hardflow)
        tty.c_cflag |= CRTSCTS;
    else
        tty.c_cflag &= ~CRTSCTS;
    if (tcsetattr(fd, TCSAFLUSH, &tty) < 0)
        FATAL(1, "tcsetattr: %m");
    return fd;
}

int wsbr_uart_tx(struct wsbr_ctxt *ctxt, const void *buf, unsigned int buf_len)
{
    uint16_t crc = crc16(buf, buf_len);
    uint8_t *frame = malloc(buf_len * 2 + 3);
    const uint8_t *buf8 = buf;
    int i, frame_len;
    int ret;

    frame_len = 0;
    frame[frame_len++] = 0x7E;
    for (i = 0; i < buf_len; i++) {
        if (buf8[i] == 0x7D || buf8[i] == 0x7E) {
            frame[frame_len++] = 0x7D;
            frame[frame_len++] = buf8[i] ^ 0x20;
        } else {
            frame[frame_len++] = buf8[i];
        }
    }
    memcpy(frame + frame_len, &crc, sizeof(crc));
    frame_len += sizeof(crc);
    ret = write(ctxt->rcp_fd, frame, frame_len);
    BUG_ON(ret != frame_len);
    free(frame);

    return frame_len;
}

int wsbr_uart_rx(struct wsbr_ctxt *ctxt, void *buf, unsigned int buf_len)
{
    uint8_t *buf8 = buf;
    int i = 0;
    int frame_len = 0;
    uint16_t crc;

    BUG_ON(ctxt->rcp_uart_rx_buf_len && ctxt->rcp_uart_rx_buf[0] == 0x7E);
    while (i == ctxt->rcp_uart_rx_buf_len) {
        i = 0;
        ctxt->rcp_uart_rx_buf_len = read(ctxt->rcp_fd, ctxt->rcp_uart_rx_buf, sizeof(ctxt->rcp_uart_rx_buf));
        BUG_ON(ctxt->rcp_uart_rx_buf_len < 0);
        while (buf8[i] == 0x7E && i < ctxt->rcp_uart_rx_buf_len)
            i++;
    };
    while (ctxt->rcp_uart_rx_buf[i] != 0x7E) {
        while (ctxt->rcp_uart_rx_buf[i] != 0x7E && i < ctxt->rcp_uart_rx_buf_len) {
            BUG_ON(frame_len > buf_len);
            if (buf8[i] == 0x7D) {
                i++;
                buf8[frame_len++] = ctxt->rcp_uart_rx_buf[i++] ^ 0x20;
            } else {
                BUG_ON(ctxt->rcp_uart_rx_buf[i] == 0x7E);
                buf8[frame_len++] = buf8[i++];
            }
        }
        if (i == ctxt->rcp_uart_rx_buf_len)
            ctxt->rcp_uart_rx_buf_len = read(ctxt->rcp_fd, ctxt->rcp_uart_rx_buf, sizeof(ctxt->rcp_uart_rx_buf));
    }
    while (ctxt->rcp_uart_rx_buf[i] == 0x7E && i < ctxt->rcp_uart_rx_buf_len)
        i++;
    memmove(ctxt->rcp_uart_rx_buf, ctxt->rcp_uart_rx_buf + i, ctxt->rcp_uart_rx_buf_len - i);
    ctxt->rcp_uart_rx_buf_len -= i;
    frame_len -= sizeof(uint16_t);
    crc = crc16(buf8, frame_len);
    if (memcmp(buf8 + frame_len, &crc, sizeof(uint16_t))) {
        WARN("bad crc, frame dropped");
        return 0;
    }
    return frame_len;
}
