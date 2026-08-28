#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Settings app scene.

The scene owns only presentation and local UI behaviour. Runtime device state
is exposed through named binds so an Edge Agent settings provider can populate
the values without coupling this reusable Mosaic component to app_config.
"""

from __future__ import annotations

import base64
import importlib.util
import sys
from pathlib import Path

from PIL import Image as PILImage
from PIL import ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from scene_common import (  # noqa: E402
    CONTENT_H,
    CONTENT_W,
    asset_scene,
    auto_charset,
    button,
    container,
    image,
    label,
    layer,
    settings_nav_header,
    shared_prefix,
    shared_charset,
    top_notice,
    scene_out_path,
    write_scene,
)

FONT = "settings_font.ttf"
BOLD = FONT

# Stack page indices. StackView navigation depth is capped at eight, but the
# number of addressable pages may be larger.
PAGE_SET = 0
PAGE_DISPLAY = 1
PAGE_ABOUT = 2
PAGE_WLAN = 3
PAGE_WLAN_DETAIL = 4
PAGE_WLAN_PASSWORD = 5
PAGE_WLAN_PHONE_SETUP = 6
PAGE_DETAIL = 7
PAGE_FACTORY = 8
PAGE_INTEGRATIONS_MODEL = 9
PAGE_INTEGRATIONS_CHANNELS = 10
PAGE_SOUND = 11
PAGE_SECURITY = 12
PAGE_UPDATE = 13
PAGE_COUNT = 14
STACK_CAPACITY = 8

# Mock scan results for the settings WLAN list (prototype SYS_NETWORKS + extras).
MOCK_WLAN_NETWORKS = (
    ("ssid1", True),
    ("ssid2", True),
    ("ssid3", True),
    ("ssid4", True),
    ("ssid5", True),
    ("ssid6", True),
    ("ssid7", True),
    ("ssid8", True),
    ("ssid9", True),
    ("ssid10", True),
)
WLAN_LIST_ITEM_H = 100

QR_PLACEHOLDER = "settings_qr_placeholder.png"
CHANNEL_ICON_ASSETS = {
    "wechat": ("settings_channel_wechat.png", "settings_channel_wechat_off.png"),
    "qq": ("settings_channel_qq.png", "settings_channel_qq_off.png"),
    "feishu": ("settings_channel_feishu.png", "settings_channel_feishu_off.png"),
    "telegram": ("settings_channel_telegram.png", "settings_channel_telegram_off.png"),
}
ROOT_DYNAMIC_PLACEHOLDER = "settings_root_dynamic_placeholder.png"
QR_SIZE = 104
WLAN_WIFI_ICON = "../../../common/assets/icons/status_wifi.png"
WLAN_LOCK_ICON = "settings_wlan_lock.png"
WLAN_CHECK_ICON = "settings_wlan_check.png"
WLAN_LOADING_ICONS = [
    "settings_wlan_loading.png",
    *[f"settings_wlan_loading_{frame}.png" for frame in range(1, 8)],
]
WLAN_JOIN_ICON = "settings_wlan_join.png"
WLAN_CLOSE_ICON = "settings_wlan_close.png"
WLAN_PHONE_ICON = "settings_wlan_phone.png"
ROOT_DISPLAY_ICON = "settings_root_display.png"
ROOT_SOUND_ICON = "settings_root_sound.png"
ROOT_AI_ICON = "settings_root_ai.png"
ROOT_CHANNELS_ICON = "settings_root_channels.png"
ROOT_NETWORK_ICON = "settings_root_network.png"
ROOT_SECURITY_ICON = "settings_root_security.png"
ROOT_ABOUT_ICON = "settings_root_about.png"
ROOT_CHEVRON_ICON = "settings_root_chevron.png"
BACK_ICON = "settings_html_back.png"
SLIDER_STRIPE_MASK_IMAGE = "settings_slider_stripe_mask.png"
KEYBOARD_SHIFT_ICON = "settings_keyboard_shift.png"
KEYBOARD_DELETE_ICON = "settings_keyboard_delete.png"
KEYBOARD_GO_ICON = "settings_keyboard_go.png"
IMAGES: list[tuple[str, int, int]] = [
    (QR_PLACEHOLDER, QR_SIZE, QR_SIZE),
    (QR_PLACEHOLDER, 128, 128),
    (QR_PLACEHOLDER, 184, 184),
    *((asset, 88, 88) for pair in CHANNEL_ICON_ASSETS.values()
      for asset in pair),
    (ROOT_DYNAMIC_PLACEHOLDER, 48, 48),
    (WLAN_WIFI_ICON, 40, 40),
    (WLAN_LOCK_ICON, 40, 40),
    (WLAN_CHECK_ICON, 48, 48),
    *((icon, 48, 48) for icon in WLAN_LOADING_ICONS),
    (WLAN_JOIN_ICON, 48, 48),
    (WLAN_CLOSE_ICON, 48, 48),
    (WLAN_PHONE_ICON, 26, 26),
    (ROOT_DISPLAY_ICON, 48, 48),
    (ROOT_SOUND_ICON, 48, 48),
    (ROOT_AI_ICON, 48, 48),
    (ROOT_CHANNELS_ICON, 48, 48),
    (ROOT_NETWORK_ICON, 48, 48),
    (ROOT_SECURITY_ICON, 48, 48),
    (ROOT_ABOUT_ICON, 48, 48),
    (ROOT_CHEVRON_ICON, 48, 48),
    (BACK_ICON, 48, 48),
    ("../../setup_center/scene/setup_html_chevron.png", 48, 48),
    ("../../setup_center/scene/setup_html_yes.png", 100, 100),
    ("../../setup_center/scene/setup_qr_placeholder.png", 256, 256),
    ("../../setup_center/scene/setup_qr_placeholder.png", 220, 220),
    (SLIDER_STRIPE_MASK_IMAGE, 456, 62),
    (KEYBOARD_SHIFT_ICON, 26, 26),
    (KEYBOARD_DELETE_ICON, 30, 26),
]
# Spec sizes: 16 subtitle, 22 value, 24 row, 26 ›, 30 title, 46 ‹
FONT_POLICIES = {
    16: shared_charset(),
    18: auto_charset(),
    20: shared_charset(
        "".join(chr(codepoint) for codepoint in range(32, 127)) + "°"),
    24: auto_charset(),
    30: auto_charset(),
    32: auto_charset(),
    36: auto_charset(),
    48: auto_charset(),
}

# Canvas placeholders must be opaque and use the compiled scene's pixel
# format.  A transparent app icon is not Canvas-compatible and causes GSP to
# reject runtime QR frames.  The compiler converts this opaque RGB PNG to the
# selected profile format.  Black keeps the unavailable state visually empty.
QR_PLACEHOLDER_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAGgAAABoCAIAAACSfiL2AAAANklEQVR42u3B"
    "gQAAAADDoPlTX+EAVQEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADA"
    "b38oAAGXY5yXAAAAAElFTkSuQmCC"
)

BG = "#000000"
CARD = "#181819"
FG = "#FCFCFF"
MUTED = "#91919B"
DIM = "#595959"
ACCENT = "#FF4C01"
SETTINGS_RUNTIME_CHARSET = (
    "".join(chr(codepoint) for codepoint in range(32, 127))
    + "°×·…‹›"
)

def _bound_label(objs, parent, x, y, w, h, text, bind, *, size=20,
                 color=FG, align="left"):
    item = label(parent, x, y, w, h, text, size=size, color=color,
                 align=align, bind=bind, name=bind)
    item["overflow"] = "ellipsis"
    item["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(item)


def _nav_header(objs, parent, title, *, name):
    settings_nav_header(
        objs, parent, title, name=name, back_image=BACK_ICON)


def _page(objs, stack, index):
    page = len(objs)
    objs.append(layer(stack, CONTENT_W * index, 0, CONTENT_W, CONTENT_H,
                      name=f"settings_stack_page{index}"))
    objs.append(container(page, 0, 0, CONTENT_W, CONTENT_H, bg=BG,
                          name=f"settings_bg_{index}"))
    return page


def _toggle(objs, parent, x, y, name, checked, events=None):
    objs.append({
        "type": "toggle",
        "parent": parent,
        "x": x,
        "y": y,
        "w": 110,
        "h": 48,
        "checked": checked,
        "bg_color": "#3B3C3D",
        "fg_color": ACCENT,
        "knob_color": FG,
        "radius": 24,
        "bind": name,
        "name": name,
        **({"events": events} if events else {}),
    })


def _rotation_dropdown(objs, parent, y):
    objs.append({
        "type": "dropdown", "parent": parent,
        "x": 316, "y": y, "w": 128, "h": 46,
        "name": "settings_rotation_dropdown",
        "callback": "settings_rotation_select",
        "options": ["0°", "90°", "180°", "270°"],
        "selected": 0, "item_height": 36,
        "font_size": 20, "fg_color": FG,
        "bg_color": "#29292B", "panel_color": "#29292B",
        "border_color": "#464649", "border_width": 1,
        "radius": 14,
    })


def _screen_timeout_dropdown(objs, parent, y):
    objs.append({
        "type": "dropdown", "parent": parent,
        "x": 316, "y": y, "w": 128, "h": 46,
        "name": "settings_screen_timeout_dropdown",
        "callback": "settings_screen_timeout_select",
        "options": ["10 sec", "30 sec", "1 min", "2 min", "5 min", "Never"],
        "selected": 1, "item_height": 36, "open_direction": "up",
        "font_size": 20, "fg_color": FG,
        "bg_color": "#29292B", "panel_color": "#29292B",
        "border_color": "#464649", "border_width": 1,
        "radius": 14,
    })


def _build_root(objs, page):
    objs.append({
        "type": "list", "parent": page,
        "x": 0, "y": 64, "w": 480, "h": 400,
        "name": "settings_root_list",
        "item_height": 100,
        "item_count": 11,
        "row_template": "settings_root_row",
        "scroll_snapshot": True,
        "snap_to_item": False,
        "bg_color": BG,
        "fg_color": FG,
    })
    row = len(objs)
    objs.append({
        "type": "container",
        "parent": -1,
        "x": 0,
        "y": 0,
        "w": 480,
        "h": 100,
        "template": "settings_root_row",
        "callback": "settings_root_row",
        "bg_color": BG,
        "radius": 0,
    })
    objs.append({
        "type": "image", "parent": row,
        "x": 12, "y": 26, "w": 48, "h": 48,
        # Keep the dynamic slot's authored fallback opaque and local to this
        # scene. The row binder replaces it with the corresponding PNG before
        # presentation; using a shared transparent icon here lets font-link
        # externalize the fallback and leaves recycled rows with a missing GRB
        # resource during startup.
        "image": ROOT_DYNAMIC_PLACEHOLDER, "fit": "cover",
        "dynamic_image": True, "name": "settings_root_row_icon",
    })
    title = label(row, 68, 27, 190, 46, "AI Model", size=30,
                  name="settings_root_row_title")
    title["overflow"] = "ellipsis"
    title["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(title)
    value = label(row, 260, 33, 152, 34, "Trial", size=20,
                  color=MUTED, align="right",
                  name="settings_root_row_value")
    value["overflow"] = "ellipsis"
    value["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(value)
    objs.append(image(row, ROOT_CHEVRON_ICON, 420, 26, 48, 48))
    objs.append(container(row, 0, 99, 480, 1, bg="#3B3C3D"))
def _display_back_events():
    return [
        {
            "event": "click",
            "action": "stack_pop",
            "target_name": "settings_stack",
            "animated": True,
        },
        {
            "event": "click",
            "action": "call",
            "target_name": "settings_display_page",
            "arg": PAGE_SET,
        },
    ]


def _striped_slider(objs, parent, y, title, bind_prefix, value):
    objs.append(label(parent, 12, y, 300, 45, title, size=32))
    _bound_label(objs, parent, 340, y + 6, 128, 34, f"{value}%",
                 f"{bind_prefix}_value", size=24, color=MUTED,
                 align="right")
    scale_y = y + 58
    objs.append(label(parent, 12, scale_y, 60, 26, "0", size=18,
                      color="#CCCCCC"))
    objs.append(label(parent, 210, scale_y, 60, 26, "50", size=18,
                      color="#CCCCCC", align="center"))
    objs.append(label(parent, 408, scale_y, 60, 26, "100", size=18,
                      color="#CCCCCC", align="right"))
    track_y = y + 96
    objs.append(container(parent, 12, track_y, 456, 62,
                          bg=CARD, radius=8))
    fill_width = max(1, round(value * 456 / 100))
    objs.append(container(
        parent, 12, track_y,
        {"default": fill_width, "min": 1, "max": 456}, 62,
        bg=ACCENT, radius=8, name=f"{bind_prefix}_fill"))
    objs.append(image(parent, SLIDER_STRIPE_MASK_IMAGE,
                      12, track_y, 456, 62))
    objs.append({
        "type": "slider", "parent": parent,
        "x": 12, "y": track_y, "w": 456, "h": 62,
        "value": value, "min": 0, "max": 100,
        "bind": f"{bind_prefix}_input_value",
        "name": f"{bind_prefix}_input",
        "bg_color": "#00000000", "fg_color": "#00000000",
        "knob_color": "#00000000", "knob": False,
        "track_size": 62, "radius": 8, "opacity": 0,
    })


def _build_display(objs, page):
    _nav_header(objs, page, "Display", name="nav_display")
    display_back_events = _display_back_events()
    for item in objs[-3:]:
        item["events"] = display_back_events
    _striped_slider(objs, page, 80, "Brightness",
                    "settings_brightness", 58)
    objs.append(label(page, 12, 246, 280, 46, "Rotation", size=24))
    objs.append(label(page, 12, 320, 280, 46, "Screen-off", size=24))
    _rotation_dropdown(objs, page, 246)
    _screen_timeout_dropdown(objs, page, 316)


def _build_sound(objs, page):
    _nav_header(objs, page, "Sound", name="nav_sound")
    display_back_events = [
        {
            "event": "click",
            "action": "stack_pop",
            "target_name": "settings_stack",
            "animated": True,
        },
        {
            "event": "click",
            "action": "call",
            "target_name": "settings_display_page",
            "arg": PAGE_SET,
        },
    ]
    for item in objs[-3:]:
        item["events"] = display_back_events
    _striped_slider(objs, page, 80, "Volume", "settings_volume", 41)
    objs.append(label(page, 12, 280, 320, 45,
                      "Notification Sound", size=32))
    _toggle(objs, page, 358, 279, "settings_notification_sound",
            True)
    objs.append(container(page, 0, 357, 480, 1, bg="#3B3C3D"))
def _wlan_nav_row(objs, parent, y, title, page):
    events = [{
        "event": "click",
        "action": "stack_push",
        "target_name": "settings_stack",
        "arg": page,
        "animated": True,
    }]
    objs.append(button(parent, 12, y, 456, 60, "", bg=CARD, radius=18,
                       events=events))
    objs.append(label(parent, 30, y + 17, 280, 28, title, size=20,
                      events=events))
    objs.append(label(parent, 424, y + 14, 28, 30, "›", size=24,
                      color=DIM, align="center", events=events))


def _build_wlan(objs, page):
    _nav_header(objs, page, "Network", name="nav_wlan")
    wlan_back_events = [
        {
            "event": "click",
            "action": "call",
            "target_name": "settings_wlan_leave",
        },
    ]
    # The App owns this navigation transaction so duplicate device releases
    # cannot pop more than one StackView depth.
    objs[-3]["events"] = wlan_back_events
    objs[-2].pop("events", None)
    objs[-1].pop("events", None)
    objs.append(label(page, 12, 72, 240, 48, "WLAN", size=36))
    _toggle(objs, page, 358, 73, "settings_wlan_enabled",
            checked=True, events=[{
                "event": "click",
                "action": "call",
                "target_name": "wlan_toggle",
            }])

    off_layer = len(objs)
    objs.append(layer(page, 0, 134, CONTENT_W, 346,
                      hidden=True, name="settings_wlan_off_layer",
                      bind="settings_wlan_off_visible"))
    objs.append(label(off_layer, 12, 6, 456, 80,
                      "claw, Feature1, Feature require WLAN",
                      size=20, color=MUTED))

    list_layer = len(objs)
    objs.append(layer(page, 0, 134, CONTENT_W, 346,
                      name="settings_wlan_list_layer",
                      bind="settings_wlan_list_visible"))
    current_events = [{
        "event": "click",
        "action": "call",
        "target_name": "wlan_current_network",
    }]
    current_layer = len(objs)
    objs.append(layer(list_layer, 0, 0, CONTENT_W, 100,
                      hidden=True, name="settings_wlan_current_layer",
                      bind="settings_wlan_current_visible"))
    objs.append(button(current_layer, 0, 0, 480, 100, "", bg=BG, radius=0,
                       events=current_events))
    check_layer = len(objs)
    objs.append(layer(current_layer, 12, 24, 48, 48,
                      name="settings_wlan_current_check",
                      bind="settings_wlan_current_check_visible"))
    objs.append(image(check_layer, WLAN_CHECK_ICON, 0, 0, 48, 48,
                      events=current_events))
    for frame, icon in enumerate(WLAN_LOADING_ICONS):
        loading_layer = len(objs)
        objs.append(layer(
            current_layer, 12, 24, 48, 48, hidden=True,
            name=f"settings_wlan_current_loading_{frame}",
            bind=f"settings_wlan_current_loading_{frame}_visible"))
        objs.append(image(loading_layer, icon, 0, 0, 48, 48,
                          events=current_events))
    error_layer = len(objs)
    objs.append(layer(current_layer, 12, 24, 48, 48, hidden=True,
                      name="settings_wlan_current_error",
                      bind="settings_wlan_current_error_visible"))
    objs.append(image(error_layer, WLAN_CLOSE_ICON, 0, 0, 48, 48,
                      events=current_events))
    objs.append(label(current_layer, 68, 17, 300, 42, "Network", size=30,
                      bind="settings_wlan_current_ssid",
                      name="settings_wlan_current_ssid",
                      events=current_events))
    objs.append(label(current_layer, 68, 59, 330, 34, "Current Network",
                      size=24, color=MUTED,
                      bind="settings_wlan_current_status",
                      name="settings_wlan_current_status",
                      events=current_events))
    objs.append(image(current_layer, WLAN_WIFI_ICON, 428, 30, 40, 40,
                      events=current_events))
    objs.append(container(current_layer, 0, 99, 480, 1, bg="#3B3C3D"))

    # Keep one List and move its owning layer. Dynamic row instances belong to
    # the List driver, so duplicating the List for two layouts would make the
    # two instance pools compete for the same row template state.
    scan_layer = len(objs)
    objs.append(layer(
        list_layer,
        {"default": 0, "min": 0, "max": 1},
        {"default": 100, "min": 0, "max": 100},
        CONTENT_W, 346, name="settings_wlan_scan_layer"))
    objs.append(label(scan_layer, 12, 6, 180, 34, "Network",
                      size=24, color=MUTED))
    objs.append({
        "type": "list",
        "parent": scan_layer,
        "x": 0,
        "y": 42,
        "w": 480,
        "h": 304,
        "item_height": WLAN_LIST_ITEM_H,
        "name": "settings_wlan_scan_list",
        "row_template": "settings_wlan_row",
        "item_count": 0,
        "scroll_snapshot": True,
        "bg_color": BG,
        "fg_color": FG,
    })
    row_tpl = len(objs)
    objs.append({
        "type": "container",
        "parent": -1,
        "x": 0,
        "y": 0,
        "w": 480,
        "h": WLAN_LIST_ITEM_H,
        "template": "settings_wlan_row",
        "callback": "wlan_network_select",
        "bg_color": BG,
        "radius": 0,
    })
    objs.append(label(
        row_tpl, 12, 29, 320, 42, "ssid1", size=30,
        name="settings_wlan_row_ssid",
    ))
    objs.append(image(row_tpl, WLAN_LOCK_ICON, 380, 30, 40, 40))
    objs.append(image(row_tpl, WLAN_WIFI_ICON, 428, 30, 40, 40))
    objs.append(container(row_tpl, 0, 99, 480, 1, bg="#3B3C3D"))


def _build_wlan_detail(objs, page):
    back_events = [{
        "event": "click",
        "action": "call",
        "target_name": "wlan_list_resume",
    }]
    objs.append(button(page, 0, 0, 96, 64, "", radius=0, size=16,
                       name="nav_wlan_detail", bg=BG, fg=BG,
                       events=back_events))
    objs.append(image(page, BACK_ICON, 12, 8, 48, 48,
                      events=back_events))
    objs.append(label(page, 0, 0, 480, 64, "Network", size=24,
                      color=MUTED, align="center",
                      bind="settings_wlan_detail_ssid",
                      name="settings_wlan_detail_ssid",
                      events=back_events,
                      font_charset=SETTINGS_RUNTIME_CHARSET))
    objs.append(container(page, 12, 70, 456, 60, bg=CARD, radius=18))
    objs.append(label(page, 30, 87, 220, 28, "Auto-Join", size=20))
    _toggle(objs, page, 358, 76, "settings_wlan_auto_join",
            checked=True, events=[{
                "event": "click",
                "action": "call",
                "target_name": "wlan_auto_join_toggle",
            }])
    objs.append(button(page, 12, 142, 456, 60, "Forget This Network",
                       bg=CARD, fg=ACCENT, radius=18, size=20,
                       name="wlan_forget_network",
                       callback="wlan_forget_network"))


def _build_wlan_password(objs, page):
    back_events = [{
        "event": "click",
        "action": "call",
        "target_name": "wlan_password_detach",
    }]
    join_events = [{
        "event": "click",
        "action": "call",
        "target_name": "wlan_connect",
    }]
    objs.append(image(page, WLAN_CLOSE_ICON, 24, 8, 48, 48,
                      events=back_events))
    objs.append(label(page, 72, 14, 336, 36, "Network", size=24,
                      color=MUTED, align="center",
                      bind="settings_wlan_password_ssid",
                      name="settings_wlan_password_ssid"))
    objs.append(image(page, WLAN_JOIN_ICON, 408, 8, 48, 48,
                      events=join_events))
    objs.append(label(page, 24, 96, 432, 48,
                      "Enter the password to join", size=30,
                      color=MUTED, bind="settings_wlan_password_value",
                      name="settings_wlan_password_value"))
    objs.append(container(page, 24, 154, 432, 1, bg="#3B3C3D"))
    phone_events = [{
        "event": "click",
        "action": "call",
        "target_name": "wlan_phone_setup_open",
    }, {
        "event": "click",
        "action": "stack_push",
        "target_name": "settings_stack",
        "arg": PAGE_WLAN_PHONE_SETUP,
        "animated": True,
    }]
    objs.append(button(page, 16, 164, 220, 50, "", radius=0,
                       bg=BG, fg=BG, events=phone_events))
    objs.append(image(page, WLAN_PHONE_ICON, 24, 176, 26, 26,
                      events=phone_events))
    objs.append(label(page, 60, 172, 176, 34,
                      "Enter on phone", size=24, color=ACCENT,
                      events=phone_events))
    objs.append({
        "type": "keyboard",
        "parent": page,
        "x": 0,
        "y": 231,
        "w": 480,
        "h": 233,
        "name": "settings_wlan_password_keyboard",
        "bg_color": BG,
        "fg_color": "#ECECF0",
        "font_size": 24,
        "key_color": "#55555A",
        "function_color": "#3B3C3D",
        "function_text_color": "#ECECF0",
        "delete_color": "#3B3C3D",
        "delete_text_color": "#ECECF0",
        "space_color": "#55555A",
        "space_text_color": "#ECECF0",
        "ok_color": ACCENT,
        "ok_text_color": FG,
        "key_radius": 8,
        "shift_label": "SHIFT",
        "delete_label": "DELETE",
        "ok_label": "GO",
        "symbols_label": "ABC",
        "letters_label": "ABC",
        "space_label": "",
        "function_font_size": 20,
        "shift_icon": KEYBOARD_SHIFT_ICON,
        "delete_icon": KEYBOARD_DELETE_ICON,
    })
def _build_wlan_phone_setup(objs, page):
    close_events = [{
        "event": "click",
        "action": "stack_pop",
        "target_name": "settings_stack",
        "animated": True,
    }, {
        "event": "click",
        "action": "call",
        "target_name": "wlan_phone_setup_close",
    }]
    objs.append(label(page, 384, 8, 72, 48, "Skip", size=24,
                      color=ACCENT, align="right", events=close_events))
    objs.append(label(page, 24, 56, 432, 54, "Wi-Fi Setup", size=36))
    objs.append(label(page, 24, 108, 432, 32,
                      "Scan with your phone to join", size=24,
                      color=MUTED))
    objs.append(container(page, 140, 158, 200, 200, bg=FG, radius=6))
    objs.append(image(page, QR_PLACEHOLDER, 148, 166, 184, 184,
                      name="settings_wlan_phone_qr_canvas",
                      bind="settings_wlan_phone_qr_canvas"))
    objs.append(label(page, 0, 376, 480, 26, "Device", size=20,
                      color=MUTED, align="center",
                      name="settings_wlan_phone_ap_ssid",
                      bind="settings_wlan_phone_ap_ssid"))
    objs.append(label(page, 0, 414, 480, 30, "Submitted on phone", size=20,
                      color=ACCENT, align="center", events=[{
                          "event": "click",
                          "action": "call",
                          "target_name": "wlan_phone_submitted",
                      }]))


def _build_list_page(objs, page, title, rows, *, nav_name, child_pages=None,
                     callbacks=None):
    _nav_header(objs, page, title, name=nav_name)
    child_pages = child_pages or {}
    callbacks = callbacks or {}
    compact = len(rows) > 5
    row_step = 58 if compact else 68
    row_height = 52 if compact else 60
    for index, (row_title, value) in enumerate(rows):
        y = 64 + index * row_step if compact else 70 + index * row_step
        dest = child_pages.get(index)
        events = None
        if dest is not None:
            events = [{
                "event": "click",
                "action": "stack_push",
                "target_name": "settings_stack",
                "arg": dest,
                "animated": True,
            }]
        elif index in callbacks:
            events = [{
                "event": "click",
                "action": "call",
                "target_name": callbacks[index],
            }]
        if events is not None:
            objs.append(button(page, 0, y, 480, row_height, "", bg=BG,
                               radius=0, events=events))
        else:
            objs.append(container(page, 0, y, 480, row_height, bg=BG, radius=0))
        text_y = y + (13 if compact else 14)
        objs.append(label(page, 12, text_y, 270, 36, row_title,
                          size=24 if compact else 30,
                          events=events))
        if value:
            objs.append(label(page, 280, text_y + 2, 132, 30, value, size=20,
                              color=MUTED, align="right", events=events))
        if dest is not None:
            objs.append(label(page, 424, y + (10 if compact else 14), 40, 36, "›", size=24,
                              color=DIM, align="center", events=events))
        objs.append(container(page, 0, y + row_height - 1, 480, 1,
                              bg="#3B3C3D"))


def _build_security(objs, page):
    _web_nav(objs, page, "Security", "nav_security")
    events = [{
        "event": "click", "action": "stack_push",
        "target_name": "settings_stack", "arg": PAGE_FACTORY,
        "animated": True,
    }]
    objs.append(button(page, 0, 64, 480, 100, "", bg=BG, radius=0,
                       events=events))
    objs.append(label(page, 12, 91, 256, 46, "Erase All Data", size=32,
                      color=ACCENT, events=events))
    objs.append(label(page, 444, 97, 24, 34, "›", size=24,
                      color="#B4B6BA", align="center", events=events))
    objs.append(container(page, 0, 163, 480, 1, bg="#3B3C3D"))


def _web_nav(objs, page, title, name):
    events = [{"event": "click", "action": "stack_pop",
               "target_name": "settings_stack", "animated": True}]
    objs.append(button(page, 0, 0, 96, 64, "", bg=BG, radius=0,
                       name=name, events=events))
    objs.append(image(page, BACK_ICON, 12, 8, 48, 48,
                      events=events))
    title_obj = label(page, 0, 0, 480, 64, title, size=24,
                      color=MUTED, align="center", name=f"{name}_title")
    if name == "nav_settings_detail":
        title_obj["bind"] = "settings_detail_title"
        title_obj["font_charset"] = "AboutDiagnosticSafe ModeBattery"
    objs.append(title_obj)


def _web_static_row(objs, page, y, title, value="", *, callback=None,
                    danger=False, chevron=False, value_bind=None,
                    value_x=284, value_w=152):
    events = ([{"event": "click", "action": "call",
               "target_name": callback}] if callback else None)
    if events:
        objs.append(button(page, 0, y, 480, 100, "", bg=BG, radius=0,
                           events=events))
    else:
        objs.append(container(page, 0, y, 480, 100, bg=BG, radius=0))
    objs.append(label(page, 12, y + 27, 256, 46, title, size=32,
                      color=ACCENT if danger else FG, events=events))
    if value:
        value_obj = label(page, value_x, y + 33, value_w, 34, value, size=24,
                          color=MUTED, align="right", events=events)
        if value_bind:
            value_obj["bind"] = value_bind
            value_obj["name"] = value_bind
            value_obj["font_charset"] = SETTINGS_RUNTIME_CHARSET
        objs.append(value_obj)
    if chevron:
        objs.append(label(page, 444, y + 33, 24, 34, "›", size=24,
                          color="#B4B6BA", align="center", events=events))
    objs.append(container(page, 0, y + 99, 480, 1, bg="#3B3C3D"))


def _build_about(objs, page):
    _web_nav(objs, page, "About", "nav_about")
    _web_static_row(objs, page, 64, "Model", "ESP-MOSAICO",
                    value_x=180, value_w=256)
    _web_static_row(objs, page, 164, "GSP", "--",
                    value_bind="settings_about_gsp_version",
                    value_x=180, value_w=256)
    _web_static_row(objs, page, 264, "Display", "2.16 in · 480×480",
                    value_x=180, value_w=256)
    _web_static_row(objs, page, 364, "Serial Number", "MSC-0427-8831",
                    value_x=180, value_w=256)


def _copy_setup_integration_page(objs, page, builder, prefix):
    """Copy Setup's proven page composition into the Settings scene.

    Only symbol names and asset paths change. Settings owns the resulting
    binds/actions and never switches to the Setup Center App.
    """
    module_path = HERE.parent.parent / "setup_center" / "scene" / "gen_scene.py"
    spec = importlib.util.spec_from_file_location("mosaic_setup_scene", module_path)
    setup_scene = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(setup_scene)
    setup_scene._materialize_prototype_assets()
    start = len(objs)
    getattr(setup_scene, builder)(objs, page)

    def rewrite(value):
        if isinstance(value, str):
            if value == "setup_html_back.png":
                return BACK_ICON
            if value == "setup_llm_qr_placeholder.png":
                return QR_PLACEHOLDER
            if value.startswith("setup_") and value.endswith(".png"):
                return f"../../setup_center/scene/{value}"
            if value.startswith("setup_llm"):
                return value.replace("setup_llm", "settings_integrations_llm", 1)
            if value.startswith("setup_wechat"):
                return value.replace("setup_wechat", "settings_integrations_wechat", 1)
            if value == "setup_center_close":
                return "settings_integrations_flow_back"
            return value
        if isinstance(value, list):
            return [rewrite(item) for item in value]
        if isinstance(value, dict):
            return {key: rewrite(item) for key, item in value.items()}
        return value

    phase_layers = []
    status_dividers = []
    for index in range(start, len(objs)):
        objs[index] = rewrite(objs[index])
        item = objs[index]
        if item.get("type") == "layer" and item.get("parent") == page:
            phase_layers.append(index)

        # Align the AI configuration footer with IM Channels. Children inside
        # the URL card move with their parent; only direct status children need
        # the vertical offset.
        if (prefix == "llm" and item.get("parent") == start and
                isinstance(item.get("y"), int) and item.get("y") >= 272):
            item["y"] += 48
        if (prefix == "llm" and item.get("parent") == start and
                item.get("type") == "container" and
                item.get("x") == 12 and item.get("w") == 324 and
                item.get("h") == 112):
            item["w"] = 304
        if (prefix == "llm" and item.get("type") == "label" and
                item.get("w") == 292):
            item["w"] = 272
        if (prefix == "llm" and item.get("type") == "label" and
                item.get("text") == "Configuration URL"):
            item["name"] = "settings_integrations_llm_connection_hint"
            item["bind"] = "settings_integrations_llm_connection_hint"
            item["font_charset"] = SETTINGS_RUNTIME_CHARSET
            item["overflow"] = "ellipsis"
        if (prefix == "llm" and
                item.get("name") == "settings_integrations_llm_config_qr_canvas"):
            item["x"] = 336

        # Backend / Model / Base URL / Capabilities are Settings rows, not
        # onboarding cards. Keep their compact geometry so the existing QR
        # footer still fits, but use the standard black surface and dividers.
        if (prefix == "llm" and item.get("type") == "container" and
                item.get("x") == 12 and item.get("w") == 456 and
                item.get("h") == 44):
            item["x"] = 0
            item["w"] = 480
            item["h"] = 50
            item["bg_color"] = BG
            item["radius"] = 0
            status_dividers.append((item.get("parent"), item.get("y") + 49))

        # Keep Setup's controls and state transitions, but express them using
        # the Settings type scale. Setup uses oversized onboarding headlines;
        # Settings uses 36 px content headings below a 24 px navbar.
        if objs[index].get("font_size") in (44, 48, 56):
            objs[index]["font_size"] = 36
        elif objs[index].get("font_size") == 34:
            objs[index]["font_size"] = 32

        # The AI status page intentionally keeps its four compact rows so all
        # existing information and the QR footer still fit. Raise their old
        # Setup-only 20 px type to the Settings 24 px value scale and give the
        # glyphs enough vertical room without changing row hit geometry.
        if (item.get("type") == "label" and
                item.get("font_size") == 20 and
                item.get("x") in (28, 176) and
                item.get("w") in (154, 274)):
            item["font_size"] = 24
            item["y"] -= 5
            item["h"] = 34
            if item.get("x") == 28:
                item["x"] = 12

    for parent, divider_y in status_dividers:
        objs.append(container(parent, 0, divider_y, 480, 1,
                              bg="#3B3C3D"))

    # Each copied phase remains an independent retained layer. Give every
    # phase the same centered Settings navbar title while leaving the original
    # back/skip controls and their callbacks untouched.
    nav_title = "AI Model" if prefix == "llm" else "IM Channels"
    for phase in phase_layers:
        objs.append(label(phase, 0, 0, 480, 64, nav_title, size=24,
                          color=MUTED, align="center"))


def _build_integrations_model(objs, page):
    _copy_setup_integration_page(objs, page, "build_llm", "llm")


def _build_integrations_channels(objs, page):
    back_events = [{"event": "click", "action": "call",
                    "target_name": "settings_integrations_flow_back"}]
    objs.append(button(page, 0, 0, 96, 64, "", bg=BG, radius=0,
                       name="nav_integrations_channels",
                       events=back_events))
    objs.append(image(page, BACK_ICON, 12, 8, 48, 48,
                      events=back_events))
    objs.append(label(page, 0, 0, 480, 64, "IM Channels", size=24,
                      color=MUTED, align="center",
                      name="nav_integrations_channels_title"))

    objs.append(label(page, 23, 76, 434, 45, "Link account", size=32))
    objs.append(label(page, 23, 123, 434, 30,
                      "Link a messaging app to chat with",
                      size=24, color=MUTED))
    objs.append(label(page, 23, 153, 434, 30,
                      "Claw from your phone", size=24, color=MUTED))

    providers = (
        ("wechat", "WeChat", 23),
        ("qq", "QQ", 135),
        ("feishu", "Feishu", 247),
        ("telegram", "Telegram", 359),
    )
    for key, title, x in providers:
        on_asset, off_asset = CHANNEL_ICON_ASSETS[key]
        on_layer = len(objs)
        objs.append(layer(
            page, x, 188, 88, 116, hidden=True,
            name=f"settings_channels_{key}_on",
            bind=f"settings_channels_{key}_on_visible"))
        objs.append(image(on_layer, on_asset, 0, 0, 88, 88))
        objs.append(label(on_layer, 0, 91, 88, 25, title, size=18,
                          align="center"))

        off_layer = len(objs)
        objs.append(layer(
            page, x, 188, 88, 116,
            name=f"settings_channels_{key}_off",
            bind=f"settings_channels_{key}_off_visible"))
        objs.append(image(off_layer, off_asset, 0, 0, 88, 88))
        objs.append(label(off_layer, 0, 91, 88, 25, title, size=18,
                          color=MUTED, align="center"))

    address_card = len(objs)
    objs.append(container(page, 12, 320, 304, 112, bg=CARD, radius=16))
    hint = label(address_card, 16, 14, 272, 22,
                 "Connect to device network first", size=20, color=MUTED,
                 name="settings_channels_connection_hint",
                 bind="settings_channels_connection_hint")
    hint["font_charset"] = SETTINGS_RUNTIME_CHARSET
    hint["overflow"] = "ellipsis"
    objs.append(hint)
    address = label(
        address_card, 16, 44, 272, 48, "Unavailable", size=20,
        name="settings_channels_config_url",
        bind="settings_channels_config_url")
    address["overflow"] = "ellipsis"
    address["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(address)
    objs.append(image(
        page, QR_PLACEHOLDER, 336, 324, 104, 104,
        name="settings_channels_config_qr_canvas",
        bind="settings_channels_config_qr_canvas"))


def _build_detail(objs, page):
    _web_nav(objs, page, "About", "nav_settings_detail")
    objs.append({
        "type": "list", "parent": page,
        "x": 0, "y": 64, "w": 480, "h": 416,
        "item_height": 100, "item_count": 6,
        "name": "settings_detail_list",
        "row_template": "settings_detail_row",
        "scroll_snapshot": True,
        "bg_color": BG, "fg_color": FG,
    })
    row = len(objs)
    objs.append({
        "type": "container", "parent": -1,
        "x": 0, "y": 0, "w": 480, "h": 100,
        "template": "settings_detail_row",
        "callback": "settings_detail_row",
        "bg_color": BG, "radius": 0,
    })
    # About uses compact left-side labels (Software / Update), leaving the
    # wider right column free for versions and check-result status text.
    title = label(row, 12, 27, 180, 46, "Model", size=32,
                  name="settings_detail_row_title")
    title["dynamic_color"] = True
    title["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(title)
    value = label(row, 208, 33, 228, 34, "ESP-MOSAICO", size=24,
                  color=MUTED, align="right",
                  name="settings_detail_row_value")
    value["dynamic_color"] = True
    value["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(value)
    chevron = label(row, 444, 33, 24, 34, "", size=24,
                    color="#B4B6BA", align="center",
                    name="settings_detail_row_chevron")
    chevron["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(chevron)
    objs.append(container(row, 0, 99, 480, 1, bg="#3B3C3D"))


def _build_update(objs, page):
    back_events = [{"event": "click", "action": "call",
                    "target_name": "settings_update_leave"}]
    objs.append(button(page, 0, 0, 96, 64, "", bg=BG, radius=0,
                       name="nav_software_update", events=back_events))
    back = label(page, 12, 0, 48, 64, "‹", size=48, color=MUTED,
                 align="center", name="settings_update_back",
                 events=back_events)
    back["bind"] = "settings_update_back"
    back["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(back)
    nav_title = label(page, 0, 0, 480, 64, "Software Update", size=24,
                      color=MUTED, align="center",
                      name="settings_update_nav_title")
    nav_title["bind"] = "settings_update_nav_title"
    nav_title["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(nav_title)

    status_card = len(objs)
    card = container(page, 16, 76, 448, 96, bg=CARD, radius=20,
                     border="#29292B", border_w=1)
    card["bind"] = "settings_update_status_card_color"
    card["bind_target"] = "color"
    objs.append(card)
    objs.append(container(status_card, 0, 0, 4, 96,
                          bg=ACCENT, radius=2))
    objs.append(container(status_card, 16, 24, 48, 48,
                          bg="#2A1810", radius=24,
                          border=ACCENT, border_w=1))
    status_icon = label(status_card, 16, 24, 48, 48, "i", size=24,
                        color=ACCENT, align="center",
                        name="settings_update_status_icon")
    status_icon["bind"] = "settings_update_status_icon"
    status_icon["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(status_icon)
    status = label(status_card, 78, 10, 350, 32, "Ready to Check", size=24,
                   color=FG, name="settings_update_status")
    status["bind"] = "settings_update_status"
    status["overflow"] = "ellipsis"
    objs.append(status)
    status_detail = label(status_card, 78, 43, 350, 21,
                          "Compare with the published release", size=16,
                          color=MUTED, name="settings_update_status_detail")
    status_detail["bind"] = "settings_update_status_detail"
    status_detail["overflow"] = "ellipsis"
    objs.append(status_detail)
    status_detail_2 = label(status_card, 78, 65, 350, 21, "", size=16,
                            color=MUTED,
                            name="settings_update_status_detail_2")
    status_detail_2["bind"] = "settings_update_status_detail_2"
    status_detail_2["overflow"] = "ellipsis"
    objs.append(status_detail_2)

    heading = label(page, 20, 184, 240, 22, "ABOUT UPDATE CHECKS", size=16,
                    color=MUTED, name="settings_update_heading")
    heading["bind"] = "settings_update_heading"
    heading["font_charset"] = SETTINGS_RUNTIME_CHARSET
    heading["overflow"] = "ellipsis"
    objs.append(heading)
    published_layer = len(objs)
    objs.append(layer(page, 250, 184, 210, 22, hidden=True,
                      name="settings_update_published_layer",
                      bind="settings_update_published_visible"))
    published = label(published_layer, 0, 0, 210, 22, "2026-08-21", size=16,
                      color=MUTED, align="right",
                      name="settings_update_published_at")
    published["bind"] = "settings_update_published_at"
    published["overflow"] = "ellipsis"
    objs.append(published)

    # Keep version comparison compact and visually part of the release notes
    # rather than presenting it as a second large status card.
    version_row = len(objs)
    objs.append(container(page, 16, 210, 448, 38, bg=BG, radius=0))
    current_label = label(version_row, 4, 8, 72, 22, "CURRENT", size=16,
                          color=MUTED, name="settings_update_current_label")
    current_label["bind"] = "settings_update_current_label"
    current_label["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(current_label)
    current = label(version_row, 84, 5, 86, 28, "0.0.0", size=20,
                    name="settings_update_current_version")
    current["bind"] = "settings_update_current_version"
    current["overflow"] = "ellipsis"
    objs.append(current)
    arrow = label(version_row, 174, 5, 34, 28, ">", size=20,
                  color=ACCENT, align="center", name="settings_update_arrow")
    arrow["bind"] = "settings_update_arrow"
    arrow["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(arrow)
    latest_label = label(version_row, 214, 8, 96, 22, "PUBLISHED", size=16,
                         color=MUTED,
                         name="settings_update_latest_label")
    latest_label["bind"] = "settings_update_latest_label"
    latest_label["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(latest_label)
    latest = label(version_row, 316, 5, 128, 28, "Unavailable", size=20,
                   name="settings_update_latest_version")
    latest["bind"] = "settings_update_latest_version"
    latest["overflow"] = "ellipsis"
    objs.append(latest)

    objs.append({
        "type": "list", "parent": page,
        "x": 16, "y": 252, "w": 448, "h": 140,
        "name": "settings_update_notes_list",
        "item_height": 28,
        "item_count": 1,
        "row_template": "settings_update_note_row",
        "scroll_snapshot": True,
        "snap_to_item": False,
        "bg_color": BG,
        "fg_color": FG,
    })
    note_row = len(objs)
    objs.append({
        "type": "container", "parent": -1,
        "x": 0, "y": 0, "w": 448, "h": 28,
        "template": "settings_update_note_row",
        "bg_color": BG, "radius": 0,
    })
    note_text = label(note_row, 4, 1, 440, 26,
                      "Release information only", size=18,
                      name="settings_update_note_row_text")
    note_text["dynamic_color"] = True
    note_text["font_charset"] = SETTINGS_RUNTIME_CHARSET
    objs.append(note_text)

    disclaimer = label(page, 20, 398, 440, 18,
                       "Information only · No automatic installation",
                       size=16, color=MUTED, align="center",
                       name="settings_update_disclaimer")
    disclaimer["bind"] = "settings_update_disclaimer"
    disclaimer["font_charset"] = SETTINGS_RUNTIME_CHARSET
    disclaimer["overflow"] = "ellipsis"
    objs.append(disclaimer)

    check_events = [{"event": "click", "action": "call",
                     "target_name": "settings_update_check"}]
    check_button = button(page, 20, 416, 440, 46, "", bg=CARD, radius=16,
                          border=ACCENT, border_w=1, events=check_events)
    check_button["bind"] = "settings_update_action_card_color"
    check_button["bind_target"] = "color"
    objs.append(check_button)
    action = label(page, 20, 416, 440, 46, "Check Again", size=20,
                   color=ACCENT, align="center", name="settings_update_action",
                   events=check_events)
    action["bind"] = "settings_update_action"
    action["overflow"] = "ellipsis"
    objs.append(action)


def _build_factory(objs, page):
    _web_nav(objs, page, "Factory Reset", "nav_factory_reset")
    objs.append(label(page, 24, 142, 432, 34,
                      "Hold the button for 3 seconds",
                      size=24, color=MUTED, align="center"))
    objs.append(label(page, 24, 176, 432, 34,
                      "to erase all data.",
                      size=24, color=MUTED, align="center"))
    hold = len(objs)
    objs.append(container(page, 64, 300, 352, 86,
                          bg="#3B3C3D", radius=24,
                          name="settings_factory_hold_track"))
    objs.append({
        "type": "progress", "parent": hold,
        "x": 0, "y": 0, "w": 352, "h": 86,
        "value": 0, "min": 0, "max": 100,
        "fg_color": ACCENT, "radius": 24,
        "bind": "settings_factory_hold_progress",
        "name": "settings_factory_hold_progress",
    })
    objs.append(label(hold, 0, 0, 352, 86, "Hold 3s to Erase",
                      size=32, align="center"))


def build_settings_ui(objs, content):
    stack = len(objs)
    objs.append({
        "type": "stackview",
        "parent": content,
        "x": 0,
        "y": 0,
        "w": CONTENT_W,
        "h": CONTENT_H,
        "name": "settings_stack",
        "page_count": PAGE_COUNT,
        "initial_page": PAGE_SET,
        "capacity": STACK_CAPACITY,
        "axis": "horizontal",
        "transition_ms": 280,
        "transition_easing": "ease_out",
    })

    page = _page(objs, stack, PAGE_SET)
    _build_root(objs, page)

    page = _page(objs, stack, PAGE_DISPLAY)
    _build_display(objs, page)

    page = _page(objs, stack, PAGE_ABOUT)
    _build_about(objs, page)
    page = _page(objs, stack, PAGE_WLAN)
    _build_wlan(objs, page)

    page = _page(objs, stack, PAGE_WLAN_DETAIL)
    _build_wlan_detail(objs, page)

    page = _page(objs, stack, PAGE_WLAN_PASSWORD)
    _build_wlan_password(objs, page)

    page = _page(objs, stack, PAGE_WLAN_PHONE_SETUP)
    _build_wlan_phone_setup(objs, page)

    page = _page(objs, stack, PAGE_DETAIL)
    _build_detail(objs, page)
    page = _page(objs, stack, PAGE_FACTORY)
    _build_factory(objs, page)

    page = _page(objs, stack, PAGE_INTEGRATIONS_MODEL)
    _build_integrations_model(objs, page)
    page = _page(objs, stack, PAGE_INTEGRATIONS_CHANNELS)
    _build_integrations_channels(objs, page)
    page = _page(objs, stack, PAGE_SOUND)
    _build_sound(objs, page)
    page = _page(objs, stack, PAGE_SECURITY)
    _build_security(objs, page)
    page = _page(objs, stack, PAGE_UPDATE)
    _build_update(objs, page)

    # Notifications belong to the App stage rather than an individual
    # StackView page. Connection failures arrive after the password page has
    # popped, and the capsule must remain visible on the WLAN page.
    top_notice(
        objs, content, name="settings_top_notice", font=FONT,
        title="Connection failed",
        message="Check the network and try again", detail_size=20)


def main():
    (HERE / QR_PLACEHOLDER).write_bytes(
        base64.b64decode(QR_PLACEHOLDER_PNG)
    )
    PILImage.new("RGB", (48, 48), (0, 0, 0)).save(
        HERE / ROOT_DYNAMIC_PLACEHOLDER)
    stripe_mask = PILImage.new("RGBA", (456, 62), (0, 0, 0, 0))
    stripe_draw = ImageDraw.Draw(stripe_mask)
    for index in range(23):
        x = 10 + index * 19
        stripe_draw.rectangle((x, 0, x + 8, 61),
                              fill=(24, 24, 25, 255))
    stripe_mask.save(HERE / SLIDER_STRIPE_MASK_IMAGE)
    objs, content = shared_prefix(IMAGES, FONT_POLICIES, BOLD)
    build_settings_ui(objs, content)
    # Every runtime-bound Settings label may receive values that are not
    # present in its design-time placeholder. Pin the full product charset so
    # gspc does not silently compile only the placeholder glyphs and render
    # later symbols as '?'.
    for obj in objs:
        if obj.get("type") == "label" and obj.get("bind") and \
                not obj.get("font_charset"):
            obj["font_charset"] = SETTINGS_RUNTIME_CHARSET
    write_scene(scene_out_path(HERE, "settings_480.json"), "settings", objs, font=FONT)
    asset_scene(scene_out_path(HERE, "settings_assets_480.json"), "settings_assets", IMAGES,
                FONT_POLICIES, BOLD, FONT)


if __name__ == "__main__":
    main()
