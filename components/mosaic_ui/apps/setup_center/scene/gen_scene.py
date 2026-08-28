#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Setup Center scene, matched to the initialization screens in the prototype."""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from scene_common import (  # noqa: E402
    CONTENT_H,
    CONTENT_W,
    auto_charset,
    button,
    container,
    image,
    label,
    layer,
    scene_out_path,
    shared_prefix,
    top_notice,
    write_scene,
)

FONT = "../../settings/scene/settings_font.ttf"
BG = "#000000"
PANEL = "#3B3C3D"
CARD = "#181819"
FG = "#FCFCFF"
MUTED = "#91919B"
DIM = "#242426"
ACCENT = "#FF4C01"
PROGRESS = "#EB4126"

WLAN_LOCK = "../../settings/scene/settings_wlan_lock.png"
WLAN_WIFI = "../../../common/assets/icons/status_wifi.png"
WLAN_CLOSE = "../../settings/scene/settings_wlan_close.png"
WLAN_CHECK = "../../settings/scene/settings_wlan_join.png"
WLAN_PHONE = "../../settings/scene/settings_wlan_phone.png"
KEY_SHIFT = "../../settings/scene/settings_keyboard_shift.png"
KEY_DELETE = "../../settings/scene/settings_keyboard_delete.png"
KEY_GO = "../../settings/scene/settings_keyboard_go.png"
HTML_BACK = "setup_html_back.png"
HTML_CHEVRON = "setup_html_chevron.png"
HTML_REFRESH = "setup_html_refresh.png"
HTML_LOADING = "setup_html_loading.png"
HTML_YES = "setup_html_yes.png"
HTML_NO = "setup_html_no.png"
QR_PLACEHOLDER = "setup_qr_placeholder.png"
LLM_QR_PLACEHOLDER = "setup_llm_qr_placeholder.png"

PAGE_OVERVIEW = 0
PAGE_NETWORK = 1
PAGE_WECHAT = 2
PAGE_LLM = 3
PAGE_DONE = 4
PAGE_INTEGRATIONS = 5
PAGE_COUNT = 6

FONT_POLICIES = {
    20: auto_charset(), 24: auto_charset(), 30: auto_charset(),
    32: auto_charset(), 34: auto_charset(), 44: auto_charset(),
    48: auto_charset(), 56: auto_charset(),
}


def _validate_static_assets():
    """Validate the checked-in PNG assets used by Setup Center."""
    from PIL import Image

    assets = (
        (HTML_BACK, (48, 48)), (HTML_CHEVRON, (48, 48)),
        (HTML_REFRESH, (48, 48)), (HTML_LOADING, (48, 48)),
        (HTML_YES, (100, 100)), (HTML_NO, (100, 100)),
        (QR_PLACEHOLDER, (256, 256)),
        (LLM_QR_PLACEHOLDER, (104, 104)),
    )
    for name, expected_size in assets:
        path = HERE / name
        if not path.is_file():
            raise FileNotFoundError(f"missing Setup Center asset: {path}")
        with Image.open(path) as image_file:
            if image_file.size != expected_size:
                raise RuntimeError(
                    f"invalid Setup Center asset size: {path}: "
                    f"{image_file.size} != {expected_size}")


def call(name):
    return [{"event": "click", "action": "call", "target_name": name}]


def page(objs, stack, index):
    item = len(objs)
    objs.append(layer(stack, CONTENT_W * index, 0, CONTENT_W, CONTENT_H,
                      name=f"setup_stack_page{index}", block_scene_swipe=True))
    objs.append(container(item, 0, 0, CONTENT_W, CONTENT_H, bg=BG,
                          name=f"setup_bg_{index}"))
    return item


def topbar(objs, parent, *, back=None, back_bind=None, title="", skip=None,
           skip_bind=None, close=None, confirm=None):
    if back:
        events = call(back)
        back_parent = parent
        if back_bind:
            back_parent = len(objs)
            objs.append(layer(parent, 0, 0, 72, 64, hidden=True,
                              name=back_bind, bind=back_bind))
        objs.append(button(back_parent, 0, 0, 72, 64, "", bg=BG, fg=BG,
                           size=20, events=events))
        objs.append(image(back_parent, HTML_BACK, 12, 8, 48, 48,
                          events=events))
    if close:
        events = call(close)
        objs.append(button(parent, 0, 0, 72, 64, "", bg=BG, fg=BG,
                           size=20, events=events))
        objs.append(image(parent, WLAN_CLOSE, 12, 8, 48, 48, events=events))
    if title:
        objs.append(label(parent, 80, 16, 320, 34, title, size=24,
                          align="center"))
    if skip:
        events = call(skip)
        skip_parent = parent
        if skip_bind:
            skip_parent = len(objs)
            objs.append(layer(parent, 0, 0, 480, 64, hidden=True,
                              name=skip_bind, bind=skip_bind))
        objs.append(button(skip_parent, 364, 0, 116, 64, "", bg=BG, fg=BG,
                           size=20, events=events))
        # Match the HTML topbar: both children occupy the same 48px flex row.
        # Use the real chevron artwork rather than a font glyph whose baseline
        # makes it appear lower than the Skip text.
        objs.append(label(skip_parent, 356, 8, 72, 48, "Skip", size=24,
                          color=ACCENT, align="right", events=events))
        objs.append(image(skip_parent, HTML_CHEVRON, 420, 8, 48, 48,
                          events=events))
    if confirm:
        events = call(confirm)
        objs.append(button(parent, 408, 0, 72, 64, "", bg=BG, fg=BG,
                           size=20, events=events))
        objs.append(image(parent, WLAN_CHECK, 420, 8, 48, 48, events=events))


def primary_button(objs, parent, y, text, action, *, secondary=False,
                   name=None):
    objs.append(button(parent, 64, y, 352, 86, text,
                       bg=PANEL if secondary else ACCENT,
                       fg=FG, radius=43, size=30, callback=action, name=name))


def prototype_progress(objs, parent, y, prefix):
    """Author the HTML's 32 x 5 cell mosaic over 12 runtime segments."""
    x = 16
    width = 381
    for index in range(12):
        left = x + (width * index) // 12
        right = x + (width * (index + 1)) // 12
        objs.append(container(parent, left, y, right - left, 57, bg=DIM,
                              bind=f"{prefix}_{index + 1:02d}",
                              name=f"{prefix}_{index + 1:02d}"))
    # The overlaid gutters produce exactly 32 columns x 5 rows (9px cells,
    # 3px gaps) while the controller only updates twelve color segments.
    for col in range(1, 32):
        objs.append(container(parent, x + col * 12 - 3, y, 3, 57, bg=BG))
    for row in range(1, 5):
        objs.append(container(parent, x, y + row * 12 - 3, width, 3, bg=BG))


def success_screen(objs, parent, *, title, detail, action, action_text):
    objs.append(image(parent, HTML_YES, 190, 120, 100, 100))
    objs.append(label(parent, 24, 242, 432, 44, title, size=34,
                      align="center"))
    objs.append(label(parent, 24, 292, 432, 34, detail, size=24,
                      color=MUTED, align="center"))
    primary_button(objs, parent, 362, action_text, action, secondary=True)


def compact_info_row(objs, parent, y, title, value, bind):
    objs.append(container(parent, 12, y, 456, 44, bg=CARD, radius=14))
    objs.append(label(parent, 28, y + 10, 154, 24, title, size=20))
    item = label(parent, 176, y + 10, 274, 24, value, size=20,
                 color=MUTED, align="right", bind=bind, name=bind)
    item["overflow"] = "ellipsis"
    objs.append(item)


def llm_config_footer(objs, parent, y):
    url_card = len(objs)
    objs.append(container(parent, 12, y, 324, 112, bg=CARD, radius=16))
    objs.append(label(url_card, 16, 14, 292, 22, "Configuration URL",
                      size=20, color=MUTED))
    url = label(url_card, 16, 44, 292, 48, "Unavailable", size=20,
                bind="setup_llm_config_url", name="setup_llm_config_url")
    url["overflow"] = "ellipsis"
    objs.append(url)
    objs.append(image(parent, LLM_QR_PLACEHOLDER, 352, y + 4, 104, 104,
                      name="setup_llm_config_qr_canvas",
                      bind="setup_llm_config_qr_canvas"))


def status_card(objs, parent, y, title, subtitle, name, action):
    events = call(action)
    objs.append(button(parent, 14, y, 452, 92, "", bg="#181819", radius=24,
                       size=20, events=events))
    objs.append(label(parent, 32, y + 15, 280, 34, title, size=24,
                      events=events))
    objs.append(label(parent, 32, y + 52, 350, 26, subtitle, size=20,
                      color=MUTED, bind=f"{name}_summary",
                      name=f"{name}_summary", events=events))
    objs.append(label(parent, 436, y + 24, 24, 36, "›", size=30,
                      color=MUTED, events=events))


def build_overview(objs, parent):
    topbar(objs, parent, back="setup_center_close", title="AI SETUP")
    objs.append(label(parent, 24, 68, 432, 58, "AI Setup", size=44))
    objs.append(label(parent, 24, 126, 432, 30,
                      "Network, WeChat and LLM", size=20, color=MUTED))
    status_card(objs, parent, 172, "Network", "Not configured",
                "setup_network", "setup_overview_network")
    status_card(objs, parent, 272, "WeChat", "Not linked",
                "setup_wechat", "setup_overview_wechat")
    status_card(objs, parent, 372, "LLM", "Not configured",
                "setup_llm", "setup_overview_llm")


def build_integrations(objs, parent):
    topbar(objs, parent, back="setup_center_close", title="AI & INTEGRATIONS")
    objs.append(label(parent, 24, 68, 432, 58, "AI & Integrations", size=44))
    objs.append(label(parent, 24, 126, 432, 30,
                      "Model and message channel setup", size=20,
                      color=MUTED))
    status_card(objs, parent, 172, "AI Model", "Not configured",
                "setup_integrations_llm", "setup_overview_llm")
    status_card(objs, parent, 272, "IM Channels", "Not linked",
                "setup_integrations_wechat", "setup_overview_wechat")


def build_network(objs, parent):
    scan = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, name="setup_network_scan",
                      bind="setup_network_scan_visible"))
    topbar(objs, scan, back="setup_network_back",
           back_bind="setup_network_back_visible",
           skip="setup_network_skip",
           skip_bind="setup_network_skip_visible")
    # gsp labels are single-line; author the HTML's explicit <br> as two
    # labels so font fallback cannot collapse the line break.
    objs.append(label(scan, 24, 64, 432, 56, "Connect to", size=48))
    objs.append(label(scan, 24, 112, 228, 56, "Network", size=48))
    spin = len(objs)
    objs.append(layer(scan, 263, 112, 48, 48, name="setup_network_spinner",
                      bind="setup_network_spinner_visible"))
    objs.append(image(spin, HTML_LOADING, 0, 0, 48, 48))
    refresh = len(objs)
    objs.append(layer(scan, 263, 112, 48, 48, hidden=True,
                      name="setup_network_refresh",
                      bind="setup_network_refresh_visible"))
    objs.append(image(refresh, HTML_REFRESH, 0, 0, 48, 48,
                      events=call("setup_network_refresh")))

    # Match the Settings WLAN scanner exactly: the list is part of the black
    # page (not a separate rounded sheet), has the same section caption,
    # 100 px rows, icon positions and one-pixel separators.  Keeping the
    # visual contract identical also makes a later real Wi-Fi data source
    # interchangeable between Settings and Setup Center.
    panel = len(objs)
    objs.append(layer(scan, 0, 176, 480, 304, hidden=True,
                      name="setup_network_panel",
                      bind="setup_network_panel_visible"))
    objs.append(label(panel, 12, 6, 180, 34, "Network",
                      size=24, color=MUTED))
    objs.append({
        "type": "list", "parent": panel,
        "x": 0, "y": 42, "w": 480, "h": 262,
        "item_height": 100, "name": "setup_network_list",
        "row_template": "setup_network_row", "item_count": 10,
        "bg_color": BG, "fg_color": FG,
    })
    row = len(objs)
    objs.append({
        "type": "container", "parent": -1, "x": 0, "y": 0,
        "w": 480, "h": 100, "template": "setup_network_row",
        "callback": "setup_network_select", "bg_color": BG,
        "radius": 0,
    })
    objs.append(label(row, 12, 29, 320, 42, "SSID1", size=30,
                      name="setup_network_row_ssid"))
    objs.append(image(row, WLAN_LOCK, 380, 30, 40, 40))
    objs.append(image(row, WLAN_WIFI, 428, 30, 40, 40))
    objs.append(container(row, 0, 99, 480, 1, bg=PANEL))

    password = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_network_password",
                      bind="setup_network_password_visible"))
    topbar(objs, password, close="setup_network_cancel",
           confirm="setup_network_join")
    objs.append(label(password, 80, 16, 320, 34, "SSID1", size=24,
                      align="center", bind="setup_selected_ssid",
                      name="setup_selected_ssid"))
    objs.append(label(password, 24, 96, 432, 48, "Password", size=30,
                      color=MUTED, bind="setup_password_display",
                      name="setup_password_display"))
    # Keyboard owns this offscreen storage bind. The controller mirrors the
    # current value into the visible field without retaining an old password.
    objs.append(label(password, 0, 220, 1, 1, "", size=20, color=BG,
                      bind="setup_password_value", name="setup_password_value"))
    objs.append(container(password, 24, 154, 432, 1, bg=PANEL))
    phone_events = call("setup_network_phone_open")
    objs.append(image(password, WLAN_PHONE, 24, 172, 26, 26,
                      events=phone_events))
    objs.append(label(password, 62, 168, 300, 34, "Enter on phone", size=24,
                      color=ACCENT, events=phone_events))
    objs.append({
        "type": "keyboard", "parent": password,
        "x": 0, "y": 231, "w": 480, "h": 233,
        "name": "setup_password_keyboard", "bg_color": BG,
        "fg_color": "#ECECF0", "font_size": 24,
        "key_color": "#55555A", "function_color": PANEL,
        "function_text_color": "#ECECF0", "delete_color": PANEL,
        "delete_text_color": "#ECECF0", "space_color": "#55555A",
        "space_text_color": "#ECECF0", "ok_color": ACCENT,
        "ok_text_color": FG, "key_radius": 8, "shift_label": "",
        "delete_label": "", "ok_label": "", "symbols_label": "123",
        "letters_label": "ABC", "space_label": "",
        "function_font_size": 18,
        "shift_icon": KEY_SHIFT, "delete_icon": KEY_DELETE,
        "ok_icon": KEY_GO,
    })

    phone = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_network_phone",
                      bind="setup_network_phone_visible"))
    objs.append(label(phone, 384, 8, 72, 48, "Skip", size=24,
                      color=ACCENT, align="right",
                      callback="setup_network_phone_cancel"))
    objs.append(label(phone, 24, 56, 432, 54, "Wi-Fi Setup", size=34))
    objs.append(label(phone, 24, 108, 432, 32,
                      "Scan with your phone to join", size=24,
                      color=MUTED))
    objs.append(image(phone, QR_PLACEHOLDER, 112, 146, 256, 256,
                      name="setup_network_phone_qr_canvas",
                      bind="setup_network_phone_qr_canvas"))
    objs.append(label(phone, 0, 406, 480, 26, "Device", size=20,
                      color=MUTED, align="center",
                      name="setup_network_phone_ap_ssid",
                      bind="setup_network_phone_ap_ssid"))
    objs.append(label(phone, 0, 442, 480, 30, "Submitted on phone", size=20,
                      color=ACCENT, align="center",
                      callback="setup_network_phone_submitted"))

    joining = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_network_joining",
                      bind="setup_network_joining_visible"))
    objs.append(label(joining, 16, 174, 448, 124, "Joining…", size=56))
    objs.append(label(joining, 16, 306, 448, 34, "SSID1", size=24,
                      color=ACCENT, bind="setup_joining_ssid",
                      name="setup_joining_ssid"))
    prototype_progress(objs, joining, 358, "setup_network_progress")

    connected = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_network_connected",
                      bind="setup_network_connected_visible"))
    topbar(objs, connected, back="setup_network_back",
           back_bind="setup_network_connected_back_visible")
    objs.append(image(connected, HTML_YES, 190, 120, 100, 100))
    objs.append(label(connected, 24, 242, 432, 44, "Network connected",
                      size=34, align="center"))
    objs.append(label(connected, 24, 292, 432, 34, "SSID1", size=24,
                      color=MUTED, align="center", bind="setup_connected_ssid",
                      name="setup_connected_ssid"))
    continue_button = len(objs)
    objs.append(layer(connected, 64, 362, 352, 86,
                      name="setup_network_connected_continue_visible",
                      bind="setup_network_connected_continue_visible"))
    objs.append(button(continue_button, 0, 0, 352, 86, "Continue",
                       bg=PANEL, fg=FG, radius=43, size=30,
                       callback="setup_network_continue"))
    change_button = len(objs)
    objs.append(layer(connected, 64, 362, 352, 86, hidden=True,
                      name="setup_network_connected_change_visible",
                      bind="setup_network_connected_change_visible"))
    objs.append(button(change_button, 0, 0, 352, 86, "Change Network",
                       bg=PANEL, fg=FG, radius=43, size=30,
                       callback="setup_network_change"))


def qr_page(objs, parent, *, title, subtitle, back_action,
            prompt=None, prompt_action=None, skip_action=None,
            back_bind=None, skip_bind=None, qr_bind=None,
            subtitle_bind=None, prompt_bind=None, large=False,
            title_size=44, code_label="MOSAICO-SETUP", qr_frame_pad=8,
            subtitle_below_qr=False, qr_y_override=None):
    topbar(objs, parent, back=back_action, back_bind=back_bind,
           skip=skip_action, skip_bind=skip_bind)
    qr_size = 256 if large else 220
    qr_x = (480 - qr_size) // 2
    qr_y = (164 if large else 170) if qr_y_override is None else qr_y_override
    if title:
        objs.append(label(parent, 24, 66, 432, 50, title, size=title_size))
    subtitle_y = qr_y + qr_size + 12 if subtitle_below_qr else 122
    objs.append(label(parent, 24, subtitle_y, 432, 32, subtitle, size=24,
                      color=MUTED,
                      align="center" if subtitle_below_qr else "left",
                      bind=subtitle_bind, name=subtitle_bind))
    objs.append(container(parent, qr_x - qr_frame_pad, qr_y - qr_frame_pad,
                          qr_size + qr_frame_pad * 2,
                          qr_size + qr_frame_pad * 2, bg=FG,
                          radius=0 if qr_frame_pad == 0 else 4))
    objs.append(image(parent, QR_PLACEHOLDER, qr_x, qr_y, qr_size, qr_size,
                      bind=qr_bind, name=qr_bind))
    if code_label:
        code_y = 438 if large else 404
        objs.append(label(parent, 24, code_y, 432, 28, code_label, size=20,
                          color=MUTED, align="center"))
    if prompt:
        objs.append(label(parent, 24, 442, 432, 32, prompt, size=20,
                          color=ACCENT, align="center",
                          callback=prompt_action, bind=prompt_bind,
                          name=prompt_bind))


def build_wechat(objs, parent):
    binding = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480,
                      name="setup_wechat_binding",
                      bind="setup_wechat_binding_visible"))
    qr_page(objs, binding, title=None,
            subtitle="Preparing QR code",
            back_action="setup_wechat_cancel",
            skip_action="setup_center_close",
            back_bind="setup_wechat_binding_back_visible",
            skip_bind="setup_wechat_binding_skip_visible",
            qr_bind="setup_wechat_qr_canvas",
            subtitle_bind="setup_wechat_bind_status", large=True,
            code_label=None, qr_frame_pad=0, subtitle_below_qr=True,
            qr_y_override=132)

    progress = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_wechat_progress",
                      bind="setup_wechat_progress_visible"))
    objs.append(label(progress, 16, 184, 448, 60,
                      "Binding", size=56))
    objs.append(label(progress, 16, 244, 448, 60, "WeChat…", size=56))
    objs.append(label(progress, 16, 316, 448, 34, "Verifying device…",
                      size=24, color=ACCENT, bind="setup_wechat_stage_text",
                      name="setup_wechat_stage_text"))
    prototype_progress(objs, progress, 364, "setup_wechat_progress_bar")

    success = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_wechat_success",
                      bind="setup_wechat_success_visible"))
    topbar(objs, success, back="setup_wechat_success_back",
           back_bind="setup_wechat_success_back_visible")
    objs.append(image(success, HTML_YES, 190, 120, 100, 100))
    objs.append(label(success, 24, 242, 432, 44, "Mosaico-Lab", size=34,
                      align="center"))
    objs.append(label(success, 24, 292, 432, 34, "Account linked", size=24,
                      color=MUTED, align="center"))
    get_started = len(objs)
    objs.append(layer(success, 64, 362, 352, 86,
                      name="setup_wechat_success_get_started_visible",
                      bind="setup_wechat_success_get_started_visible"))
    objs.append(button(get_started, 0, 0, 352, 86, "Continue",
                       bg=PANEL, fg=FG, radius=43, size=30,
                       callback="setup_center_close"))
    rebind = len(objs)
    objs.append(layer(success, 64, 362, 352, 86, hidden=True,
                      name="setup_wechat_success_rebind_visible",
                      bind="setup_wechat_success_rebind_visible"))
    objs.append(button(rebind, 0, 0, 352, 86, "Rebind",
                       bg=PANEL, fg=FG, radius=43, size=30,
                       callback="setup_wechat_rebind"))


def build_llm(objs, parent):
    status = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, name="setup_llm_status",
                      bind="setup_llm_status_visible"))
    topbar(objs, status, back="setup_llm_back",
           back_bind="setup_llm_status_back_visible",
           skip="setup_llm_skip",
           skip_bind="setup_llm_status_skip_visible")
    compact_info_row(objs, status, 66, "Backend", "Not configured",
                     "setup_llm_backend")
    compact_info_row(objs, status, 116, "Model", "Not configured",
                     "setup_llm_model")
    compact_info_row(objs, status, 166, "Base URL", "Not configured",
                     "setup_llm_base_url")
    compact_info_row(objs, status, 216, "Capabilities", "Tools -- · Vision --",
                     "setup_llm_capabilities")
    llm_config_footer(objs, status, 272)

    configuring = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_llm_configuring",
                      bind="setup_llm_configuring_visible"))
    qr_page(objs, configuring, title="AI Setup",
            subtitle="Configure on your phone", prompt="Submitted on phone",
            prompt_action="setup_llm_submitted", back_action="setup_llm_cancel",
            skip_action="setup_llm_skip",
            back_bind="setup_llm_configuring_back_visible",
            skip_bind="setup_llm_configuring_skip_visible")

    progress = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_llm_progress",
                      bind="setup_llm_progress_visible"))
    objs.append(label(progress, 16, 198, 448, 64,
                      "Configuring", size=56))
    objs.append(label(progress, 16, 254, 448, 64, "AI…", size=56))
    objs.append(label(progress, 16, 330, 448, 34,
                      "Validating configuration…", size=24, color=ACCENT))
    prototype_progress(objs, progress, 358, "setup_llm_progress_bar")

    success = len(objs)
    objs.append(layer(parent, 0, 0, 480, 480, hidden=True,
                      name="setup_llm_success",
                      bind="setup_llm_success_visible"))
    success_screen(objs, success, title="LLM Ready",
                   detail="OpenAI Compatible", action="setup_llm_continue",
                   action_text="Continue")
    objs.append(label(success, 64, 454, 352, 26, "Reconfigure", size=20,
                      color=MUTED, align="center",
                      callback="setup_llm_reconfigure"))


def build_done(objs, parent):
    success_screen(objs, parent, title="Setup Complete",
                   detail="Your Mosaico is ready",
                   action="setup_center_close", action_text="Continue")
    objs.append(label(parent, 64, 448, 352, 28, "View setup status", size=20,
                      color=MUTED, align="center", callback="setup_view_status"))


def main():
    _validate_static_assets()
    images = [
        (WLAN_LOCK, 40, 40), (WLAN_WIFI, 40, 40),
        (WLAN_CLOSE, 48, 48), (WLAN_CHECK, 48, 48),
        (WLAN_PHONE, 26, 26), (KEY_SHIFT, 26, 26),
        (KEY_DELETE, 30, 26), (KEY_GO, 48, 48),
        (HTML_BACK, 48, 48), (HTML_CHEVRON, 48, 48),
        (HTML_REFRESH, 48, 48),
        (HTML_LOADING, 48, 48), (HTML_YES, 100, 100),
        (HTML_NO, 100, 100), (QR_PLACEHOLDER, 256, 256),
    ]
    objs, content = shared_prefix(images, FONT_POLICIES, FONT)
    stack = len(objs)
    objs.append({
        "type": "stackview", "parent": content,
        "x": 0, "y": 0, "w": CONTENT_W, "h": CONTENT_H,
        "name": "setup_stack", "page_count": PAGE_COUNT,
        "initial_page": PAGE_OVERVIEW, "capacity": 8,
        "axis": "horizontal", "transition_ms": 300,
        "transition_easing": "ease_out",
    })
    build_overview(objs, page(objs, stack, PAGE_OVERVIEW))
    build_network(objs, page(objs, stack, PAGE_NETWORK))
    build_wechat(objs, page(objs, stack, PAGE_WECHAT))
    build_llm(objs, page(objs, stack, PAGE_LLM))
    build_done(objs, page(objs, stack, PAGE_DONE))
    build_integrations(objs, page(objs, stack, PAGE_INTEGRATIONS))
    top_notice(objs, content, name="setup_top_notice",
               title="Connection failed", message="Incorrect password")
    write_scene(scene_out_path(HERE, "setup_center_480.json"),
                "setup_center", objs, font=FONT)


if __name__ == "__main__":
    main()
