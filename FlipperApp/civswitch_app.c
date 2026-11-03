#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <usb.h>
#include <usb_hid.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#include "payload.h"

#define EP_GAMEPAD_IN 0x81
#define EP_GAMEPAD_OUT 0x02
#define EP_KEYBOARD_IN 0x83
#define EP_KEYBOARD_OUT 0x03
#define HID_PACKET_SIZE 8

#define SWITCH_HAT_NONE 0x08
#define SWITCH_BUTTON_A (1U << 2)
#define KEY_MOD_LSHIFT (1U << 1)

typedef struct __attribute__((packed)) {
    uint16_t buttons;
    uint8_t hat;
    uint8_t lx, ly, rx, ry;
    uint8_t vendor;
} SwitchReport;

typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} KeyboardReport;

typedef enum {
    RunReady,
    RunSending,
    RunFinished,
    RunCanceled,
    RunError,
} RunState;

typedef enum {
    TxFailureNone,
    TxFailureDisconnected,
    TxFailureBusyTimeout,
    TxFailureEndpointConfig,
    TxFailureWriteRejected,
    TxFailureAckTimeout,
} TxFailure;

typedef struct {
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;
    FuriThread* worker;
    FuriHalUsbInterface* previous_usb;
    FuriSemaphore* gamepad_tx_sem;
    FuriSemaphore* keyboard_tx_sem;
    usbd_device* usb_dev;
    volatile bool connected;
    volatile bool configured;
    volatile bool stop_requested;
    volatile RunState state;
    volatile uint16_t completed_lines;
    volatile uint8_t failed_endpoint;
    volatile TxFailure tx_failure;
    volatile int32_t usb_result;
} CivSwitchApp;

static CivSwitchApp* active_app;
static bool keyboard_protocol_boot;

static void wake_tx_waiters(CivSwitchApp* app) {
    if(app->gamepad_tx_sem) furi_semaphore_release(app->gamepad_tx_sem);
    if(app->keyboard_tx_sem) furi_semaphore_release(app->keyboard_tx_sem);
}

static void record_tx_failure(CivSwitchApp* app, TxFailure failure, uint8_t ep, int32_t result) {
    if(app->tx_failure == TxFailureNone) {
        app->tx_failure = failure;
        app->failed_endpoint = ep;
        app->usb_result = result;
    }
}

static uint8_t switch_report_descriptor[] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x15,0x00,0x25,0x01,0x35,0x00,0x45,0x01,
    0x75,0x01,0x95,0x10,0x05,0x09,0x19,0x01,0x29,0x10,0x81,0x02,0x05,0x01,
    0x25,0x07,0x46,0x3B,0x01,0x75,0x04,0x95,0x01,0x65,0x14,0x09,0x39,0x81,
    0x42,0x65,0x00,0x95,0x01,0x81,0x01,0x26,0xFF,0x00,0x46,0xFF,0x00,0x09,
    0x30,0x09,0x31,0x09,0x32,0x09,0x35,0x75,0x08,0x95,0x04,0x81,0x02,0x06,
    0x00,0xFF,0x09,0x20,0x95,0x01,0x81,0x02,0x0A,0x21,0x26,0x95,0x08,0x91,
    0x02,0xC0,
};

static uint8_t keyboard_report_descriptor[] = {
    0x05,0x01,0x09,0x06,0xA1,0x01,0x75,0x01,0x95,0x08,0x05,0x07,0x19,0xE0,
    0x29,0xE7,0x15,0x00,0x25,0x01,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,
    0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,
    0x75,0x03,0x91,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,
    0x19,0x00,0x29,0x65,0x81,0x00,0xC0,
};

static uint8_t composite_config_descriptor[] = {
    0x09,0x02,0x49,0x00,0x02,0x01,0x00,0x80,0xFA,
    0x09,0x04,0x00,0x00,0x02,0x03,0x00,0x00,0x00,
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x56,0x00,
    0x07,0x05,0x02,0x03,0x40,0x00,0x01,
    0x07,0x05,0x81,0x03,0x40,0x00,0x01,
    0x09,0x04,0x01,0x00,0x02,0x03,0x01,0x01,0x00,
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x3F,0x00,
    0x07,0x05,0x83,0x03,0x08,0x00,0x01,
    0x07,0x05,0x03,0x03,0x08,0x00,0x01,
};

static struct usb_device_descriptor device_descriptor = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 8,
    .idVendor = 0x0F0D,
    .idProduct = 0x0092,
    .bcdDevice = VERSION_BCD(1, 0, 0),
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

static void endpoint_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    if(event == usbd_evt_eptx && active_app) {
        const uint8_t endpoint_number = ep & 0x7F;
        if(endpoint_number == (EP_GAMEPAD_IN & 0x7F) && active_app->gamepad_tx_sem) {
            furi_semaphore_release(active_app->gamepad_tx_sem);
        } else if(endpoint_number == (EP_KEYBOARD_IN & 0x7F) && active_app->keyboard_tx_sem) {
            furi_semaphore_release(active_app->keyboard_tx_sem);
        }
    } else if(event == usbd_evt_eprx) {
        uint8_t discard[HID_PACKET_SIZE];
        usbd_ep_read(dev, ep, discard, sizeof(discard));
    }
}

static usbd_respond configure_endpoints(usbd_device* dev, uint8_t cfg) {
    const uint8_t eps[] = {EP_GAMEPAD_IN, EP_GAMEPAD_OUT, EP_KEYBOARD_IN, EP_KEYBOARD_OUT};
    if(cfg == 0) {
        if(active_app) {
            active_app->configured = false;
            active_app->connected = false;
            wake_tx_waiters(active_app);
        }
        for(size_t i = 0; i < COUNT_OF(eps); i++) {
            usbd_ep_deconfig(dev, eps[i]);
            usbd_reg_endpoint(dev, eps[i], NULL);
        }
        return usbd_ack;
    }
    if(cfg != 1) return usbd_fail;
    const uint8_t endpoint_sizes[] = {64, 64, HID_PACKET_SIZE, HID_PACKET_SIZE};
    for(size_t i = 0; i < COUNT_OF(eps); i++) {
        if(!usbd_ep_config(dev, eps[i], USB_EPTYPE_INTERRUPT, endpoint_sizes[i])) {
            if(active_app) {
                record_tx_failure(active_app, TxFailureEndpointConfig, eps[i], 0);
                active_app->state = RunError;
            }
            for(size_t configured = 0; configured < i; configured++) {
                usbd_ep_deconfig(dev, eps[configured]);
                usbd_reg_endpoint(dev, eps[configured], NULL);
            }
            return usbd_fail;
        }
    }
    for(size_t i = 0; i < COUNT_OF(eps); i++) usbd_reg_endpoint(dev, eps[i], endpoint_callback);
    // Prime each IN endpoint with a zero-length packet before real reports.
    if(usbd_ep_write(dev, EP_GAMEPAD_IN, NULL, 0) < 0) {
        if(active_app) {
            record_tx_failure(active_app, TxFailureEndpointConfig, EP_GAMEPAD_IN, -1);
            active_app->state = RunError;
        }
        return usbd_fail;
    }
    if(usbd_ep_write(dev, EP_KEYBOARD_IN, NULL, 0) < 0) {
        if(active_app) {
            record_tx_failure(active_app, TxFailureEndpointConfig, EP_KEYBOARD_IN, -1);
            active_app->state = RunError;
        }
        return usbd_fail;
    }
    if(active_app) {
        active_app->configured = true;
        active_app->connected = true;
    }
    return usbd_ack;
}

static usbd_respond control_request(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);
    const uint8_t recipient_type = req->bmRequestType & (USB_REQ_RECIPIENT | USB_REQ_TYPE);
    if(recipient_type == (USB_REQ_INTERFACE | USB_REQ_CLASS) && req->wIndex == 0) {
        switch(req->bRequest) {
        case USB_HID_SETIDLE: return usbd_ack;
        case USB_HID_GETREPORT: {
            static const SwitchReport released = {.hat = SWITCH_HAT_NONE, .lx = 0x80, .ly = 0x80, .rx = 0x80, .ry = 0x80};
            dev->status.data_ptr = (uint8_t*)&released;
            dev->status.data_count = sizeof(released);
            return usbd_ack;
        }
        default: return usbd_fail;
        }
    }
    if(recipient_type == (USB_REQ_INTERFACE | USB_REQ_CLASS) && req->wIndex == 1) {
        switch(req->bRequest) {
        case USB_HID_SETIDLE: return usbd_ack;
        case USB_HID_SETPROTOCOL:
            if(req->wValue > 1) return usbd_fail;
            keyboard_protocol_boot = req->wValue == 0;
            return usbd_ack;
        case USB_HID_GETREPORT: {
            static const KeyboardReport released = {0};
            dev->status.data_ptr = (uint8_t*)&released;
            dev->status.data_count = sizeof(released);
            return usbd_ack;
        }
        default: return usbd_fail;
        }
    }
    if(recipient_type == (USB_REQ_INTERFACE | USB_REQ_STANDARD) && req->bRequest == USB_STD_GET_DESCRIPTOR) {
        const uint8_t descriptor_type = req->wValue >> 8;
        if(descriptor_type == USB_DTYPE_HID) {
            if(req->wIndex == 0) {
                dev->status.data_ptr = composite_config_descriptor + 18;
                dev->status.data_count = 9;
                return usbd_ack;
            }
            if(req->wIndex == 1) {
                dev->status.data_ptr = composite_config_descriptor + 50;
                dev->status.data_count = 9;
                return usbd_ack;
            }
        } else if(descriptor_type == USB_DTYPE_HID_REPORT) {
            if(req->wIndex == 0) {
                dev->status.data_ptr = switch_report_descriptor;
                dev->status.data_count = sizeof(switch_report_descriptor);
                return usbd_ack;
            }
            if(req->wIndex == 1) {
                dev->status.data_ptr = keyboard_report_descriptor;
                dev->status.data_count = sizeof(keyboard_report_descriptor);
                return usbd_ack;
            }
        }
    }
    return usbd_fail;
}

static void usb_deinit(usbd_device* dev) {
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
}

static void usb_suspend(usbd_device* dev) {
    UNUSED(dev);
    if(active_app) {
        active_app->connected = false;
        wake_tx_waiters(active_app);
    }
}

static void usb_wakeup(usbd_device* dev) {
    UNUSED(dev);
    if(active_app) active_app->connected = active_app->configured;
}

static void usb_state_callback(FuriHalUsbStateEvent event, void* ctx) {
    CivSwitchApp* app = ctx;
    if(event == FuriHalUsbStateEventReset) {
        app->connected = false;
        app->configured = false;
        wake_tx_waiters(app);
    } else if(event == FuriHalUsbStateEventWakeup) {
        app->connected = app->configured;
    } else if(event == FuriHalUsbStateEventSuspend) {
        app->connected = false;
        wake_tx_waiters(app);
    }
}

static void usb_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(intf);
    UNUSED(ctx);
    // Match Flipper's native HID driver: one initial token allows the first
    // report; endpoint completion releases the token for each following one.
    if(active_app && !active_app->gamepad_tx_sem) active_app->gamepad_tx_sem = furi_semaphore_alloc(1, 1);
    if(active_app && !active_app->keyboard_tx_sem) active_app->keyboard_tx_sem = furi_semaphore_alloc(1, 1);
    if(active_app) active_app->usb_dev = dev;
    usbd_reg_config(dev, configure_endpoints);
    usbd_reg_control(dev, control_request);
    usbd_connect(dev, true);
}

static FuriHalUsbInterface civswitch_usb = {
    .init = usb_init,
    .deinit = usb_deinit,
    .wakeup = usb_wakeup,
    .suspend = usb_suspend,
    .dev_descr = &device_descriptor,
    .str_manuf_descr = NULL,
    .str_prod_descr = NULL,
    .str_serial_descr = NULL,
    .cfg_descr = composite_config_descriptor,
};

static void app_draw(Canvas* canvas, void* ctx) {
    const CivSwitchApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 12, 12, "CivSwitch / Flipper");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 27, app->connected ? "USB: composite HID connected" : "USB: waiting for Switch...");
    char line[32];
    switch(app->state) {
    case RunReady:
        canvas_draw_str(canvas, 4, 42, "Ready. English (US) layout.");
        canvas_draw_str(canvas, 4, 56, "OK: start   Back: exit");
        break;
    case RunSending:
        snprintf(
            line,
            sizeof(line),
            "Sending line %u / %u",
            (unsigned)app->completed_lines + 1U,
            (unsigned)CIVSWITCH_PAYLOAD_LINE_COUNT);
        canvas_draw_str(canvas, 4, 42, line);
        canvas_draw_str(canvas, 4, 56, "Back: cancel");
        break;
    case RunFinished:
        canvas_draw_str(canvas, 4, 42, "Payload sent.");
        canvas_draw_str(canvas, 4, 56, "Back: exit");
        break;
    case RunCanceled:
        canvas_draw_str(canvas, 4, 42, "Canceled; HID released.");
        canvas_draw_str(canvas, 4, 56, "Back: exit");
        break;
    case RunError:
        switch(app->tx_failure) {
        case TxFailureDisconnected: canvas_draw_str(canvas, 4, 41, "USB disconnected during send"); break;
        case TxFailureBusyTimeout: canvas_draw_str(canvas, 4, 41, "USB endpoint stayed busy"); break;
        case TxFailureEndpointConfig: canvas_draw_str(canvas, 4, 41, "Could not configure USB IN"); break;
        case TxFailureWriteRejected: canvas_draw_str(canvas, 4, 41, "USB endpoint rejected report"); break;
        case TxFailureAckTimeout: canvas_draw_str(canvas, 4, 41, "USB report was not ACKed"); break;
        default: canvas_draw_str(canvas, 4, 41, "Payload contains unsupported key"); break;
        }
        if(app->tx_failure == TxFailureWriteRejected || app->tx_failure == TxFailureEndpointConfig) {
            snprintf(
                line,
                sizeof(line),
                "line %u ep %02X rc %ld",
                (unsigned)app->completed_lines + 1U,
                app->failed_endpoint,
                (long)app->usb_result);
        } else if(app->tx_failure == TxFailureBusyTimeout || app->tx_failure == TxFailureAckTimeout) {
            snprintf(
                line,
                sizeof(line),
                "line %u ep %02X wait",
                (unsigned)app->completed_lines + 1U,
                app->failed_endpoint);
        } else {
            snprintf(
                line,
                sizeof(line),
                "line %u ep %02X",
                (unsigned)app->completed_lines + 1U,
                app->failed_endpoint);
        }
        canvas_draw_str(canvas, 4, 52, line);
        canvas_draw_str(canvas, 4, 63, "Back: exit");
        break;
    }
}

static void app_input(InputEvent* event, void* ctx) {
    CivSwitchApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

static bool delay_or_cancel(CivSwitchApp* app, uint32_t delay_ms) {
    while(delay_ms && !app->stop_requested) {
        const uint32_t part = delay_ms > 10 ? 10 : delay_ms;
        furi_delay_ms(part);
        delay_ms -= part;
    }
    return !app->stop_requested;
}

static FuriSemaphore* endpoint_semaphore(CivSwitchApp* app, uint8_t ep) {
    return ep == EP_GAMEPAD_IN ? app->gamepad_tx_sem : app->keyboard_tx_sem;
}

static bool send_report(CivSwitchApp* app, uint8_t ep, const void* data, uint16_t len) {
    FuriSemaphore* tx_sem = endpoint_semaphore(app, ep);
    if(!app->connected || !app->usb_dev || !tx_sem) {
        record_tx_failure(app, TxFailureDisconnected, ep, 0);
        return false;
    }
    if(furi_semaphore_acquire(tx_sem, 250) != FuriStatusOk) {
        record_tx_failure(app, TxFailureBusyTimeout, ep, 0);
        return false;
    }
    if(!app->connected || !app->usb_dev) {
        furi_semaphore_release(tx_sem);
        record_tx_failure(app, TxFailureDisconnected, ep, 0);
        return false;
    }
    const int32_t write_result = usbd_ep_write(app->usb_dev, ep, data, len);
    if(write_result != len) {
        furi_semaphore_release(tx_sem);
        record_tx_failure(app, TxFailureWriteRejected, ep, write_result);
        return false;
    }
    return true;
}

static bool wait_report_complete(CivSwitchApp* app, uint8_t ep) {
    FuriSemaphore* tx_sem = endpoint_semaphore(app, ep);
    if(!app->connected || !app->usb_dev || !tx_sem) {
        record_tx_failure(app, TxFailureDisconnected, ep, 0);
        return false;
    }
    if(furi_semaphore_acquire(tx_sem, 250) != FuriStatusOk) {
        record_tx_failure(app, TxFailureAckTimeout, ep, 0);
        return false;
    }
    if(!app->connected || !app->usb_dev) {
        record_tx_failure(app, TxFailureDisconnected, ep, 0);
        return false;
    }
    return true;
}

static SwitchReport neutral_gamepad(void) {
    return (SwitchReport){.buttons = 0, .hat = SWITCH_HAT_NONE, .lx = 0x80, .ly = 0x80, .rx = 0x80, .ry = 0x80, .vendor = 0};
}

static bool gamepad_a(CivSwitchApp* app, uint32_t hold_ms) {
    SwitchReport report = neutral_gamepad();
    report.buttons = SWITCH_BUTTON_A;
    if(!send_report(app, EP_GAMEPAD_IN, &report, sizeof(report))) return false;
    if(!delay_or_cancel(app, hold_ms)) return false;
    report = neutral_gamepad();
    return send_report(app, EP_GAMEPAD_IN, &report, sizeof(report));
}

static KeyboardReport key_report(uint8_t usage, bool shift) {
    KeyboardReport report = {0};
    report.modifier = shift ? KEY_MOD_LSHIFT : 0;
    report.keycode[0] = usage;
    return report;
}

static uint8_t ascii_usage(char ch, bool* shift) {
    *shift = false;
    if(ch >= 'a' && ch <= 'z') return 0x04 + (uint8_t)(ch - 'a');
    if(ch >= 'A' && ch <= 'Z') { *shift = true; return 0x04 + (uint8_t)(ch - 'A'); }
    if(ch >= '1' && ch <= '9') return 0x1E + (uint8_t)(ch - '1');
    if(ch == '0') return 0x27;
    switch(ch) {
    case ' ': return 0x2C; case '\t': return 0x2B;
    case '!': *shift = true; return 0x1E; case '@': *shift = true; return 0x1F;
    case '#': *shift = true; return 0x20; case '$': *shift = true; return 0x21;
    case '%': *shift = true; return 0x22; case '^': *shift = true; return 0x23;
    case '&': *shift = true; return 0x24; case '*': *shift = true; return 0x25;
    case '(': *shift = true; return 0x26; case ')': *shift = true; return 0x27;
    case '-': return 0x2D; case '_': *shift = true; return 0x2D;
    case '=': return 0x2E; case '+': *shift = true; return 0x2E;
    case '[': return 0x2F; case '{': *shift = true; return 0x2F;
    case ']': return 0x30; case '}': *shift = true; return 0x30;
    case '\\': return 0x31; case '|': *shift = true; return 0x31;
    case ';': return 0x33; case ':': *shift = true; return 0x33;
    case '\'': return 0x34; case '"': *shift = true; return 0x34;
    case '`': return 0x35; case '~': *shift = true; return 0x35;
    case ',': return 0x36; case '<': *shift = true; return 0x36;
    case '.': return 0x37; case '>': *shift = true; return 0x37;
    case '/': return 0x38; case '?': *shift = true; return 0x38;
    default: return 0;
    }
}

static bool tap_keyboard(CivSwitchApp* app, uint8_t usage, bool shift, uint32_t hold_ms) {
    KeyboardReport report = key_report(usage, shift);
    if(!send_report(app, EP_KEYBOARD_IN, &report, sizeof(report))) return false;
    return delay_or_cancel(app, hold_ms);
}

static bool release_keyboard(CivSwitchApp* app, uint32_t gap_ms) {
    KeyboardReport report = {0};
    if(!send_report(app, EP_KEYBOARD_IN, &report, sizeof(report))) return false;
    return delay_or_cancel(app, gap_ms);
}

static bool type_line(CivSwitchApp* app, const char* line) {
    bool previous_valid = false;
    uint8_t previous_usage = 0;
    for(const char* p = line; *p; p++) {
        bool shift;
        const uint8_t usage = ascii_usage(*p, &shift);
        if(!usage) return false;
        if(previous_valid && usage == previous_usage && !release_keyboard(app, 32)) return false;
        if(!tap_keyboard(app, usage, shift, 32)) return false;
        previous_valid = true;
        previous_usage = usage;
    }
    if(!release_keyboard(app, 0)) return false;
    for(uint8_t n = 0; n < 2; n++) {
        if(!tap_keyboard(app, 0x28, false, 32)) return false;
        if(!release_keyboard(app, n == 0 ? 200 : 0)) return false;
    }
    return true;
}

static int32_t macro_worker(void* ctx) {
    CivSwitchApp* app = ctx;
    app->state = RunSending;
    app->completed_lines = 0;
    app->failed_endpoint = 0;
    app->tx_failure = TxFailureNone;
    app->usb_result = 0;
    if(!delay_or_cancel(app, 1000)) goto canceled;
    for(uint8_t i = 0; i < 5; i++) {
        if(!gamepad_a(app, 50) || !delay_or_cancel(app, 550)) goto canceled;
    }
    for(uint16_t i = 0; i < CIVSWITCH_PAYLOAD_LINE_COUNT; i++) {
        if(!tap_keyboard(app, 0x29, false, 32)) goto canceled; // Escape
        if(!release_keyboard(app, 0)) goto canceled;
        if(!delay_or_cancel(app, 750) || !gamepad_a(app, 50) || !delay_or_cancel(app, 550)) goto canceled;
        if(!type_line(app, civswitch_payload[i])) {
            app->state = app->stop_requested ? RunCanceled : RunError;
            goto done;
        }
        app->completed_lines = i + 1;
        if(i + 1 < CIVSWITCH_PAYLOAD_LINE_COUNT) {
            if(!delay_or_cancel(app, 750) || !gamepad_a(app, 50) || !delay_or_cancel(app, 550)) goto canceled;
        }
    }
    app->state = RunFinished;
    goto done;
canceled:
    app->state = app->stop_requested ? RunCanceled : RunError;
done:
    if(app->connected && app->usb_dev) {
        SwitchReport pad = neutral_gamepad();
        KeyboardReport keyboard = {0};
        const bool keyboard_sent = send_report(app, EP_KEYBOARD_IN, &keyboard, sizeof(keyboard));
        const bool gamepad_sent = send_report(app, EP_GAMEPAD_IN, &pad, sizeof(pad));
        if(keyboard_sent) wait_report_complete(app, EP_KEYBOARD_IN);
        if(gamepad_sent) wait_report_complete(app, EP_GAMEPAD_IN);
        if(app->tx_failure != TxFailureNone) app->state = RunError;
    }
    view_port_update(app->view_port);
    return 0;
}

int32_t civswitch_app(void* p) {
    UNUSED(p);
    CivSwitchApp app = {0};
    active_app = &app;
    app.state = RunReady;
    app.previous_usb = furi_hal_usb_get_config();
    app.input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app.view_port = view_port_alloc();
    app.gui = furi_record_open(RECORD_GUI);
    view_port_draw_callback_set(app.view_port, app_draw, &app);
    view_port_input_callback_set(app.view_port, app_input, &app);
    gui_add_view_port(app.gui, app.view_port, GuiLayerFullscreen);
    furi_hal_usb_set_state_callback(usb_state_callback, &app);
    furi_check(furi_hal_usb_set_config(&civswitch_usb, NULL));

    bool running = true;
    while(running) {
        InputEvent event;
        if(furi_message_queue_get(app.input_queue, &event, 100) == FuriStatusOk) {
            if(event.type != InputTypePress) continue;
            if(event.key == InputKeyOk && app.state == RunReady && app.connected) {
                app.stop_requested = false;
                app.state = RunSending;
                app.worker = furi_thread_alloc();
                furi_thread_set_name(app.worker, "CivSwitchMacro");
                furi_thread_set_stack_size(app.worker, 2048);
                furi_thread_set_callback(app.worker, macro_worker);
                furi_thread_set_context(app.worker, &app);
                furi_thread_start(app.worker);
                view_port_update(app.view_port);
            } else if(event.key == InputKeyBack) {
                if(app.state == RunSending) {
                    app.stop_requested = true;
                } else {
                    running = false;
                }
            }
        }
        view_port_update(app.view_port);
    }
    if(app.worker) {
        app.stop_requested = true;
        furi_thread_join(app.worker);
        furi_thread_free(app.worker);
    }
    gui_remove_view_port(app.gui, app.view_port);
    view_port_free(app.view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app.input_queue);
    furi_hal_usb_set_state_callback(NULL, NULL);
    furi_check(furi_hal_usb_set_config(app.previous_usb, NULL));
    if(app.gamepad_tx_sem) furi_semaphore_free(app.gamepad_tx_sem);
    if(app.keyboard_tx_sem) furi_semaphore_free(app.keyboard_tx_sem);
    active_app = NULL;
    return 0;
}
