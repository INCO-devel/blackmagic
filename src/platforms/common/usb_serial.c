/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011 Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2022 1BitSquared <info@1bitsquared.com>
 * Written by Rachel Mant <git@dragonmux.network>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

/* This file implements a the USB Communications Device Class - Abstract
 * Control Model (CDC-ACM) as defined in CDC PSTN subclass 1.2.
 * A Device Firmware Upgrade (DFU 1.1) class interface is provided for
 * field firmware upgrade.
 *
 * The device's unique id is used as the USB serial number string.
 *
 * Endpoint Usage
 *
 *     0 Control Endpoint
 * IN  1 GDB CDC DATA
 * OUT 1 GDB CDC DATA
 * IN  2 GDB CDC CTR
 * IN  3 UART CDC DATA
 * OUT 3 UART CDC DATA
 * OUT 4 UART CDC CTRL
 * In  5 Trace Capture
 *
 */

#include "general.h"
#include "gdb_if.h"
#include "usb_serial.h"
#ifdef PLATFORM_HAS_TRACESWO
#include "traceswo.h"
#endif
#include "usbuart.h"
#include "aux_serial.h"

#include <libopencm3/cm3/cortex.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/usb/cdc.h>

static bool gdb_uart_dtr = true;

static void usb_serial_set_state(usbd_device *dev, uint16_t iface, uint8_t ep);

#ifdef ENABLE_DEBUG
/*
 * This call initialises "SemiHosting", only we then do our own SVC interrupt things to
 * route all output through to the debug USB serial interface if debug_bmp is true.
 *
 * https://github.com/mirror/newlib-cygwin/blob/master/newlib/libc/sys/arm/syscalls.c#L115
 */
void initialise_monitor_handles(void);
#endif

static enum usbd_request_return_codes gdb_uart_control_request(usbd_device *dev, struct usb_setup_data *req,
	uint8_t **buf, uint16_t *const len, void (**complete)(usbd_device *dev, struct usb_setup_data *req))
{
	(void)buf;
	(void)complete;
	/* Is the request for the GDB UART interface? */
	if (req->wIndex != GDB_IF_NO)
		return USBD_REQ_NEXT_CALLBACK;

	switch (req->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE:
		usb_serial_set_state(dev, req->wIndex, CDCACM_GDB_ENDPOINT);
		gdb_uart_dtr = req->wValue & 1;
		return USBD_REQ_HANDLED;
	case USB_CDC_REQ_SET_LINE_CODING:
		if (*len < sizeof(struct usb_cdc_line_coding))
			return USBD_REQ_NOTSUPP;
		return USBD_REQ_HANDLED; /* Ignore on GDB Port */
	}
	return USBD_REQ_NOTSUPP;
}

static enum usbd_request_return_codes debug_uart_control_request(usbd_device *dev, struct usb_setup_data *req,
	uint8_t **buf, uint16_t *const len, void (**complete)(usbd_device *dev, struct usb_setup_data *req))
{
	(void)complete;
	/* Is the request for the physical/debug UART interface? */
	if (req->wIndex != UART_IF_NO)
		return USBD_REQ_NEXT_CALLBACK;

	switch (req->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE:
		usb_serial_set_state(dev, req->wIndex, CDCACM_UART_ENDPOINT);
#ifdef USBUSART_DTR_PIN
		gpio_set_val(USBUSART_PORT, USBUSART_DTR_PIN, !(req->wValue & 1U));
#endif
#ifdef USBUSART_RTS_PIN
		gpio_set_val(USBUSART_PORT, USBUSART_RTS_PIN, !((req->wValue >> 1U) & 1U));
#endif
		return USBD_REQ_HANDLED;
	case USB_CDC_REQ_SET_LINE_CODING:
		if (*len < sizeof(struct usb_cdc_line_coding))
			return USBD_REQ_NOTSUPP;
		aux_serial_set_encoding((struct usb_cdc_line_coding *)*buf);
		return USBD_REQ_HANDLED;
	}
	return USBD_REQ_NOTSUPP;
}

bool gdb_uart_get_dtr(void)
{
	return gdb_uart_dtr;
}

void usb_serial_set_state(usbd_device *const dev, const uint16_t iface, const uint8_t ep)
{
	uint8_t buf[10];
	struct usb_cdc_notification *notif = (void*)buf;
	/* We echo signals back to host as notification */
	notif->bmRequestType = 0xA1;
	notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
	notif->wValue = 0;
	notif->wIndex = iface;
	notif->wLength = 2;
	buf[8] = 3U;
	buf[9] = 0U;
	usbd_ep_write_packet(dev, ep, buf, sizeof(buf));
}

void usb_serial_set_config(usbd_device *dev, uint16_t value)
{
	usb_config = value;

	/* GDB interface */
#if defined(STM32F4) || defined(LM4F)
	usbd_ep_setup(dev, CDCACM_GDB_ENDPOINT, USB_ENDPOINT_ATTR_BULK, CDCACM_PACKET_SIZE, gdb_usb_out_cb);
#else
	usbd_ep_setup(dev, CDCACM_GDB_ENDPOINT, USB_ENDPOINT_ATTR_BULK, CDCACM_PACKET_SIZE, NULL);
#endif
	usbd_ep_setup(dev, CDCACM_GDB_ENDPOINT | USB_REQ_TYPE_IN, USB_ENDPOINT_ATTR_BULK, CDCACM_PACKET_SIZE, NULL);
	usbd_ep_setup(dev, (CDCACM_GDB_ENDPOINT + 1) | USB_REQ_TYPE_IN, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

	/* Serial interface */
	usbd_ep_setup(dev, CDCACM_UART_ENDPOINT, USB_ENDPOINT_ATTR_BULK, CDCACM_PACKET_SIZE / 2, usbuart_usb_out_cb);
	usbd_ep_setup(
		dev, CDCACM_UART_ENDPOINT | USB_REQ_TYPE_IN, USB_ENDPOINT_ATTR_BULK, CDCACM_PACKET_SIZE, usbuart_usb_in_cb);
	usbd_ep_setup(dev, (CDCACM_UART_ENDPOINT + 1) | USB_REQ_TYPE_IN, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

#ifdef PLATFORM_HAS_TRACESWO
	/* Trace interface */
	usbd_ep_setup(dev, TRACE_ENDPOINT | USB_REQ_TYPE_IN, USB_ENDPOINT_ATTR_BULK, 64, trace_buf_drain);
#endif

	usbd_register_control_callback(dev, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
		USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT, debug_uart_control_request);
	usbd_register_control_callback(dev, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
		USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT, gdb_uart_control_request);

	/* Notify the host that DCD is asserted.
	 * Allows the use of /dev/tty* devices on *BSD/MacOS
	 */
	usb_serial_set_state(dev, GDB_IF_NO, CDCACM_GDB_ENDPOINT);
	usb_serial_set_state(dev, UART_IF_NO, CDCACM_UART_ENDPOINT);

#ifdef ENABLE_DEBUG
	initialise_monitor_handles();
#endif
}

void debug_uart_send_stdout(const uint8_t *const data, const size_t len)
{
	for (size_t offset = 0; offset < len; offset += CDCACM_PACKET_SIZE) {
		const size_t count = MIN(len - offset, CDCACM_PACKET_SIZE);
		nvic_disable_irq(USB_IRQ);
		/* XXX: Do we actually care if this fails? Possibly not.. */
		usbd_ep_write_packet(usbdev, CDCACM_UART_ENDPOINT, data + offset, count);
		nvic_enable_irq(USB_IRQ);
	}
}

#ifdef ENABLE_DEBUG
size_t debug_uart_write(const char *buf, const size_t len)
{
	if (nvic_get_active_irq(USB_IRQ) || nvic_get_active_irq(USBUSART_IRQ) || nvic_get_active_irq(USBUSART_DMA_RX_IRQ))
		return 0;

	CM_ATOMIC_CONTEXT();

	for (size_t i = 0; i < len && (usb_dbg_in + 1) % RX_FIFO_SIZE != usb_dbg_out; ++i) {
		if (buf[i] == '\n') {
			usb_dbg_buf[usb_dbg_in++] = '\r';
			usb_dbg_in %= RX_FIFO_SIZE;

			if ((usb_dbg_in + 1) % RX_FIFO_SIZE == usb_dbg_out)
				break;
		}
		usb_dbg_buf[usb_dbg_in++] = buf[i];
		usb_dbg_in %= RX_FIFO_SIZE;
	}

	usbuart_run();
	return len;
}

/*
 * newlib defines _write as a weak link'd function for user code to override.
 *
 * This function forms the root of the implementation of a variety of functions
 * that can write to stdout/stderr, including printf().
 *
 * The result of this function is the number of bytes written.
 */
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
int _write(const int file, const void *const ptr, const size_t len)
{
	(void)file;
#ifdef PLATFORM_HAS_DEBUG
	if (debug_bmp)
		return debug_uart_write(ptr, len);
#endif
	return len;
}

/*
 * newlib defines isatty as a weak link'd function for user code to override.
 *
 * The result of this function is always 'true'.
 */
int isatty(const int file)
{
	(void)file;
	return true;
}

enum {
	RDI_SYS_OPEN = 0x01,
};

typedef struct ex_frame {
	uint32_t r0;
	const uint32_t *params;
	uint32_t r2;
	uint32_t r3;
	uint32_t r12;
	uintptr_t lr;
	uintptr_t return_address;
} ex_frame_s;

void debug_monitor_handler(void) __attribute__((used)) __attribute__((naked));

/*
 * This implements the other half of the newlib syscall puzzle.
 * When newlib is built for ARM, various calls that do file IO
 * such as printf end up calling [_swiwrite](https://github.com/mirror/newlib-cygwin/blob/master/newlib/libc/sys/arm/syscalls.c#L317)
 * and other similar low-level implementation functions. These
 * generate `swi` instructions for the "RDI Monitor" and that lands us.. here.
 *
 * The RDI calling convention sticks the file number in r0, the buffer pointer in r1, and length in r2.
 * ARMv7-M's SWI (SVC) instruction then takes all that and maps it into an exception frame on the stack.
 */
void debug_monitor_handler(void)
{
	ex_frame_s *frame;
	__asm__(
		"mov %[frame], sp" :
		[frame] "=r" (frame)
	);

	/* Make sure to return to the instruction after the SWI/BKPT */
	frame->return_address += 2U;

	switch (frame->r0) {
	case RDI_SYS_OPEN:
		frame->r0 = 1;
		break;
	default:
		frame->r0 = UINT32_MAX;
	}
	__asm__("bx lr");
}
#endif
