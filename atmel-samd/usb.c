/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "usb.h"

#include <stdint.h>

// We must include this early because it sets values used in the ASF4 includes
// below.
#include "py/mpconfig.h"

#include "hal/include/hal_gpio.h"
#include "usb/class/cdc/device/cdcdf_acm.h"
// #include "hiddf_mouse.h"
// #include "hiddf_keyboard.h"
#include "usb/class/hid/device/hiddf_generic.h"
#include "usb/class/composite/device/composite_desc.h"
#include "peripheral_clk_config.h"
#include "hpl/pm/hpl_pm_base.h"
#include "hpl/gclk/hpl_gclk_base.h"

#include "lib/utils/interrupt_char.h"
#include "reset.h"

#include "supervisor/shared/autoreload.h"

// Store received characters on our own so that we can filter control characters
// and act immediately on CTRL-C for example.

// Receive buffer
static uint8_t usb_rx_buf[USB_RX_BUF_SIZE];

// Receive buffer head
static volatile uint8_t usb_rx_buf_head;

// Receive buffer tail
static volatile uint8_t usb_rx_buf_tail;

// Number of bytes in receive buffer
volatile uint8_t usb_rx_count;

volatile bool mp_cdc_enabled = false;

static uint8_t multi_desc_bytes[] = {
 /* Device descriptors and Configuration descriptors list. */
 COMPOSITE_DESCES_LS_FS,
};

static struct usbd_descriptors multi_desc = {multi_desc_bytes, multi_desc_bytes + sizeof(multi_desc_bytes)};

/** Ctrl endpoint buffer */
static uint8_t ctrl_buffer[64];

static void init_hardware(void) {
    #ifdef SAMD21
    _pm_enable_bus_clock(PM_BUS_APBB, USB);
    _pm_enable_bus_clock(PM_BUS_AHB, USB);
    _gclk_enable_channel(USB_GCLK_ID, GCLK_CLKCTRL_GEN_GCLK0_Val);
    #endif

    #ifdef SAMD51
    hri_gclk_write_PCHCTRL_reg(GCLK, USB_GCLK_ID, CONF_GCLK_USB_SRC | GCLK_PCHCTRL_CHEN);
    hri_mclk_set_AHBMASK_USB_bit(MCLK);
    hri_mclk_set_APBBMASK_USB_bit(MCLK);
    #endif

    usb_d_init();

    gpio_set_pin_direction(PIN_PA24, GPIO_DIRECTION_OUT);
    gpio_set_pin_level(PIN_PA24, false);
    gpio_set_pin_pull_mode(PIN_PA24, GPIO_PULL_OFF);
    gpio_set_pin_direction(PIN_PA25, GPIO_DIRECTION_OUT);
    gpio_set_pin_level(PIN_PA25, false);
    gpio_set_pin_pull_mode(PIN_PA25, GPIO_PULL_OFF);
    #ifdef SAMD21
    gpio_set_pin_function(PIN_PA24, PINMUX_PA24G_USB_DM);
    gpio_set_pin_function(PIN_PA25, PINMUX_PA25G_USB_DP);
    #endif
    #ifdef SAMD51
    gpio_set_pin_function(PIN_PA24, PINMUX_PA24H_USB_DM);
    gpio_set_pin_function(PIN_PA25, PINMUX_PA25H_USB_DP);
    #endif
}

extern uint32_t *_usb_ep1_cache;
static bool usb_device_cb_bulk_out(const uint8_t ep, const enum usb_xfer_code rc, const uint32_t count)
{
    volatile hal_atomic_t flags;
    atomic_enter_critical(&flags);
    // If our buffer can't fit the data received, then error out.
    if (count > (uint8_t) (USB_RX_BUF_SIZE - usb_rx_count)) {
        atomic_leave_critical(&flags);
        return true;
    }

    // We read the data but ignore it later. The data itself isn't correct but
    // it does mark it as read so further data is received ok.
    // TODO(tannewt): Get ASF4 fixed so the data is correct and then stop using
    // _usb_ep1_cache directly below.
    uint8_t buf[count];
    int32_t result = cdcdf_acm_read(buf, count);
    if (result != ERR_NONE) {
        atomic_leave_critical(&flags);
        return true;
    }

    for (uint16_t i = 0; i < count; i++) {
        uint8_t c = ((uint8_t*) &_usb_ep1_cache)[i];
        if (c == mp_interrupt_char) {
            atomic_leave_critical(&flags);
            mp_keyboard_interrupt();
            // Don't put the interrupt into the buffer, just continue.
            return false;
        } else {
            // The count of characters present in receive buffer is
            // incremented.
            usb_rx_count++;
            usb_rx_buf[usb_rx_buf_tail] = c;
            usb_rx_buf_tail++;
            if (usb_rx_buf_tail == USB_RX_BUF_SIZE) {
                // Reached the end of buffer, revert back to beginning of
                // buffer.
                usb_rx_buf_tail = 0x00;
            }
        }
    }
    atomic_leave_critical(&flags);

    /* No error. */
    return false;
}

static bool usb_device_cb_bulk_in(const uint8_t ep, const enum usb_xfer_code rc, const uint32_t count)
{
    /* No error. */
    return false;
}

volatile bool reset_on_disconnect = false;

static bool usb_device_cb_state_c(usb_cdc_control_signal_t state)
{
    if (state.rs232.DTR) {
    } else if (!state.rs232.DTR && reset_on_disconnect) {
        reset_to_bootloader();
    }

    /* No error. */
    return false;
}

static bool usb_device_cb_line_coding_c(const usb_cdc_line_coding_t* coding)
{
    reset_on_disconnect = coding->dwDTERate == 1200;
    /* Ok to change. */
    return true;
}

void init_usb(void) {
    init_hardware();

    usbdc_init(ctrl_buffer);

    /* usbdc_register_funcion inside */
    cdcdf_acm_init();
    // hiddf_mouse_init();
    // hiddf_keyboard_init();

    int32_t result = usbdc_start(&multi_desc);
    while (result != ERR_NONE) {}
    usbdc_attach();
}

static inline bool cdc_enabled(void) {
    if (mp_cdc_enabled) {
        return true;
    }
    if (!cdcdf_acm_is_enabled()) {
        return false;
    }
    cdcdf_acm_register_callback(CDCDF_ACM_CB_READ, (FUNC_PTR)usb_device_cb_bulk_out);
    cdcdf_acm_register_callback(CDCDF_ACM_CB_WRITE, (FUNC_PTR)usb_device_cb_bulk_in);
    cdcdf_acm_register_callback(CDCDF_ACM_CB_STATE_C, (FUNC_PTR)usb_device_cb_state_c);
    cdcdf_acm_register_callback(CDCDF_ACM_CB_LINE_CODING_C, (FUNC_PTR)usb_device_cb_line_coding_c);
    mp_cdc_enabled = true;

    // Ignored read.
    uint8_t buf[64];
    cdcdf_acm_read(buf, 64);
    return true;
}

bool usb_bytes_available(void) {
    if (usb_rx_count == 0) {
        cdc_enabled();
        return false;
    }
    return usb_rx_count > 0;
}

int usb_read(void) {
    if (!cdc_enabled() || usb_rx_count == 0) {
        return 0;
    }

    // Disable autoreload if someone is using the repl.
    // TODO(tannewt): Check that we're actually in the REPL. It could be an
    // input() call from a script.
    autoreload_disable();

    // Copy from head.
    int data;
    CRITICAL_SECTION_ENTER();
    data = usb_rx_buf[usb_rx_buf_head];
    usb_rx_buf_head++;
    usb_rx_count--;
    if ((USB_RX_BUF_SIZE) == usb_rx_buf_head) {
      usb_rx_buf_head = 0;
    }
    CRITICAL_SECTION_LEAVE();

    //usb_write((uint8_t *)&data, 1);

    return data;
}

void usb_write(const char* buffer, uint32_t len) {
    if (!cdc_enabled()) {
        return;
    }
    int32_t result = cdcdf_acm_write((uint8_t *)buffer, len);
    while (result == USB_BUSY) {
        #ifdef MICROPY_VM_HOOK_LOOP
            MICROPY_VM_HOOK_LOOP
        #endif
        result = cdcdf_acm_write((uint8_t *)buffer, len);
    }
    while (result != ERR_NONE) {}
}

bool usb_connected(void) {
    return cdc_enabled();
}
