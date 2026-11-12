/*
 * Messages added by the Pico firmware on top of the FlipBase / Skylanders / LEGO frames
 * (vendor/inf/ble_proto.h). Same frame format; these type numbers are unused by the others.
 *
 * Phone -> Pico (answered whatever mode is active)
 *   0x30 MODE_GET                         -> MODE
 *   0x31 MODE_SET  [mode][game]           mode 1 Infinity, 2 Skylanders, 3 LEGO Dimensions;
 *                                         game (Skylanders only) 0..5 as the Skylanders page's list;
 *                                         the USB side disconnects and comes back as the new base.
 *                                         Sending the current mode again just re-plugs USB.
 *   0x32 LOG_GET   (empty)                -> LOG_TEXT chunks of everything buffered, then an empty one
 *   0x33 LOG_CTL   [stream][level]        stream 1/0 = push new lines live, level 0..2 (0xFF = keep)
 *   0x34 LOG_CLEAR (empty)
 *   0x35 INFO_GET  (empty)                -> INFO
 *   0x36 FACTORY_RESET [0xA5]             erase every saved figure/setting, then reboot
 * Pico -> phone
 *   0xB0 MODE      [mode][game][usb_state][log_level][streaming][fw_major][fw_minor][persist]
 *   0xB1 LOG_TEXT  utf-8 text (a frame with no bytes ends a LOG_GET)
 *   0xB2 INFO      utf-8 "key=value\n" lines
 *
 * usb_state: 0 not plugged in / no host, 1 host found the device, 2 host connected,
 * 3 a game is talking (same numbers each base's own STATUS uses, roughly).
 */
#pragma once

enum {
    TbMsgModeGet = 0x30,
    TbMsgModeSet = 0x31,
    TbMsgLogGet = 0x32,
    TbMsgLogCtl = 0x33,
    TbMsgLogClear = 0x34,
    TbMsgInfoGet = 0x35,
    TbMsgFactoryReset = 0x36,

    TbMsgMode = 0xB0,
    TbMsgLogText = 0xB1,
    TbMsgInfo = 0xB2,
};

enum { TbModeNone = 0, TbModeInfinity = 1, TbModeSkylanders = 2, TbModeLego = 3 };

#define TB_FW_MAJOR 0
#define TB_FW_MINOR 1
