# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Shared scene authoring helpers for mosaic hub and app packages."""

from __future__ import annotations

import json
from pathlib import Path

SCREEN_W = 480
SCREEN_H = 480
CONTENT_W = 480
CONTENT_H = 480
OX = (SCREEN_W - CONTENT_W) // 2
ASCII_PRINTABLE = "".join(chr(codepoint) for codepoint in range(32, 127))


def shared_charset(charset=ASCII_PRINTABLE):
    """A stable charset intended to exact-match through the font catalog."""
    return {"policy": "shared", "charset": charset}


def auto_charset():
    """Collect the characters actually used by static labels at this size."""
    return {"policy": "auto"}


def explicit_charset(charset):
    """Use a deliberately small charset for dynamic/specialized text."""
    return {"policy": "explicit", "charset": charset}


def label(parent, x, y, w, h, text, *, size=18, color="#FCFCFF",
          align="left", bind=None, name=None, font=None, events=None,
          callback=None, hidden=False, font_charset=None, font_link=None):
    obj = {
        "type": "label", "parent": parent,
        "x": x, "y": y, "w": w, "h": h,
        "text": text, "font_size": size, "fg_color": color,
        "text_align": align,
    }
    if bind:
        obj["bind"] = bind
    if name:
        obj["name"] = name
    if font:
        obj["font"] = font
    if font_charset is not None:
        obj["font_charset"] = font_charset
    if font_link is not None:
        obj["font_link"] = font_link
    if events:
        obj["events"] = events
    if callback:
        obj["callback"] = callback
    if hidden:
        obj["hidden"] = True
    return obj


def image(parent, src, x, y, w, h, *, name=None, callback=None, events=None,
          bind=None):
    obj = {
        "type": "image", "parent": parent,
        "x": x, "y": y, "w": w, "h": h, "image": src,
    }
    if name:
        obj["name"] = name
    if callback:
        obj["callback"] = callback
    if events:
        obj["events"] = events
    if bind:
        obj["bind"] = bind
    return obj


def button(parent, x, y, w, h, text, *, bg="#181819", fg="#FCFCFF",
           radius=0, size=16, name=None, events=None, callback=None,
           border=None, border_w=0, align=None, opacity=None):
    obj = {
        "type": "button", "parent": parent,
        "x": x, "y": y, "w": w, "h": h,
        "text": text, "font_size": size,
        "bg_color": bg, "fg_color": fg, "radius": radius,
    }
    if name:
        obj["name"] = name
    if events:
        obj["events"] = events
    if callback:
        obj["callback"] = callback
    if border:
        obj["border_color"] = border
        obj["border_width"] = border_w
    if align:
        obj["text_align"] = align
    if opacity is not None:
        obj["opacity"] = opacity
    return obj


def top_notice(objs, parent, *, name="top_notice", font=None,
               title="Notice", message="Something needs your attention",
               detail_size=18):
    """Author the standard transient top capsule.

    Visibility and both text fields are runtime-bound.  The matching native
    controller is mosaic_top_notice; keeping layout here makes every App use
    identical geometry while still compiling into its own scene bundle.
    """
    notice = len(objs)
    objs.append({
        "type": "layer", "parent": parent,
        "x": 0, "y": 0, "w": CONTENT_W, "h": 112,
        "hidden": True, "name": name,
        "bind": f"{name}_visible",
    })
    # Small offset shadow followed by the opaque capsule.
    objs.append(container(notice, 18, 15, 444, 78,
                          bg="#050505", radius=24, opacity=150))
    capsule = container(notice, 16, 12, 448, 76,
                        bg="#3B3C3D", radius=24)
    capsule["border_color"] = "#9A9A9F"
    capsule["border_width"] = 1
    objs.append(capsule)
    objs.append(container(notice, 34, 35, 26, 26,
                          bg="#3B3C3D", radius=13,
                          border="#FF4C01", border_w=2))
    objs.append(label(notice, 34, 35, 26, 26, "i", size=detail_size,
                      color="#FF4C01", align="center", font=font))
    objs.append(label(notice, 68, 22, 376, 27, title, size=20,
                      color="#FF4C01", bind=f"{name}_title",
                      name=f"{name}_title", font=font))
    objs.append(label(notice, 68, 49, 376, 25, message, size=detail_size,
                      color="#FCFCFF", bind=f"{name}_message",
                      name=f"{name}_message", font=font))
    return notice


def container(parent, x, y, w, h, *, bg="#000000", radius=0, name=None,
              border=None, border_w=0, bind=None, hidden=False,
              opacity=None, callback=None):
    obj = {
        "type": "container", "parent": parent,
        "x": x, "y": y, "w": w, "h": h,
        "bg_color": bg, "radius": radius,
    }
    if name:
        obj["name"] = name
    if border:
        obj["border_color"] = border
        obj["border_width"] = border_w
    if bind:
        obj["bind"] = bind
    if hidden:
        obj["hidden"] = True
    if opacity is not None:
        obj["opacity"] = opacity
    if callback:
        obj["callback"] = callback
    return obj


def layer(parent, x, y, w, h, *, hidden=False, name=None,
          block_scene_swipe=False, bind=None, bind_target=None):
    obj = {
        "type": "layer", "parent": parent,
        "x": x, "y": y, "w": w, "h": h,
    }
    if hidden:
        obj["hidden"] = True
    if block_scene_swipe:
        obj["block_scene_swipe"] = True
    if name:
        obj["name"] = name
    if bind:
        obj["bind"] = bind
    if bind_target:
        obj["bind_target"] = bind_target
    return obj


def shared_prefix(images, font_policies, bold_font):
    objs = [container(-1, 0, 0, SCREEN_W, SCREEN_H, name="root")]
    content = len(objs)
    objs.append(container(0, OX, 0, CONTENT_W, CONTENT_H, name="stage"))
    seed = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      hidden=True, name="resource_seed"))
    for size, spec in font_policies.items():
        policy = spec["policy"]
        if policy not in ("shared", "auto", "explicit"):
            raise ValueError(f"unsupported font charset policy: {policy}")
        if policy == "shared":
            objs.append(label(
                seed, 0, 0, 120, 80, "", size=size, color="#000000",
                font=bold_font if size >= 120 else None,
                font_charset=spec["charset"],
            ))
    for src, w, h in images:
        objs.append(image(seed, src, -512, 0, w, h))
    return objs, content


def image_seed_prefix(images):
    """Image-only resource seed for secondary hub scenes.

    Fonts are compiled once from mosaic_hub and shared across the bundle
    (starhub-style). Secondary scenes only need image refs into mosaic_assets.grb.
    """
    objs = [container(-1, 0, 0, SCREEN_W, SCREEN_H, name="root")]
    content = len(objs)
    objs.append(container(0, OX, 0, CONTENT_W, CONTENT_H, name="stage"))
    seed = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      hidden=True, name="resource_seed"))
    for src, w, h in images:
        objs.append(image(seed, src, -512, 0, w, h))
    return objs, content


# Interaction-spec list metrics (ESP-Mosaico HTML LISTS screen).
SETTINGS_ROW_H = 68
SETTINGS_ROW_GAP = 8
SETTINGS_ROW_STRIDE = SETTINGS_ROW_H + SETTINGS_ROW_GAP
SETTINGS_TITLE_SIZE = 30
SETTINGS_CHEVRON_SIZE = 46
SETTINGS_LABEL_SIZE = 24
SETTINGS_VALUE_SIZE = 22


def settings_list_row(objs, parent, y, title, value="", *, page=None,
                      drawer=None, stack_name="settings_stack", name=None,
                      title_color="#FCFCFF"):
    """Spec list row: 68px, Noto-sized label, value, › when navigable.

    Navigable targets:
      - page=N → stack_push (slide into child list)
      - drawer="name" → drawer_open (top shade, e.g. brightness/volume)
    """
    events = None
    navigable = False
    if page is not None:
        # Settings child pages share the StackView horizontal transition.
        events = [{"event": "click", "action": "stack_push",
                   "target_name": stack_name, "arg": page,
                   "animated": True}]
        navigable = True
    elif drawer is not None:
        events = [{"event": "click", "action": "drawer_open",
                   "target_name": drawer}]
        navigable = True

    row_name = name or (
        "row_" + "".join(ch.lower() if ch.isalnum() else "_" for ch in title)
    )
    if events:
        objs.append(button(
            parent, 12, y, 456, SETTINGS_ROW_H, "", bg="#181819", radius=20,
            name=row_name, events=events,
        ))
    else:
        objs.append(container(
            parent, 12, y, 456, SETTINGS_ROW_H, bg="#181819", radius=20,
            name=row_name,
        ))
    # Spec: label 24px left; value 22px + › on the right.
    objs.append(label(
        parent, 36, y + 22, 240, 28, title, size=SETTINGS_LABEL_SIZE,
        color=title_color, events=events,
    ))
    if value:
        objs.append(label(
            parent, 250, y + 23, 150, 28, value, size=SETTINGS_VALUE_SIZE,
            color="#91919B", align="right", events=events,
        ))
    if navigable:
        objs.append(label(
            parent, 412, y + 18, 40, 36, "›", size=26, color="#595959",
            align="center", events=events,
        ))


def settings_nav_header(objs, parent, title, *, name, root=False,
                        stack_name="settings_stack", back_events=None,
                        back_image=None):
    """Spec header with optional app-owned back-navigation events."""
    events = None
    callback = None
    if not root and back_events is not None:
        events = back_events
    elif not root:
        events = [{"event": "click", "action": "stack_pop",
                   "target_name": stack_name, "animated": True}]
    if root:
        objs.append(button(
            parent, 4, 8, 320, 52, "",
            radius=12, size=16, name=name,
            bg="#000000", fg="#000000", align="left",
            callback=callback, events=events,
        ))
        objs.append(container(parent, 16, 26, 22, 22,
                              bg="#FF4C01", radius=4))
        objs.append(label(
            parent, 52, 22, 260, 36, title, size=SETTINGS_TITLE_SIZE,
            color="#D6D6DE", events=events, callback=callback,
        ))
    else:
        # All Settings child pages share the web navigation skeleton: a
        # finger-sized back target on the left and a viewport-centered title.
        # Keeping the title independent of the chevron avoids long titles
        # appearing shifted to the right.
        objs.append(button(
            parent, 0, 0, 96, 64, "",
            radius=0, size=16, name=name,
            bg="#000000", fg="#000000", align="left",
            callback=callback, events=events,
        ))
        if back_image:
            objs.append(image(
                parent, back_image, 12, 8, 48, 48,
                events=events, callback=callback,
            ))
        else:
            objs.append(label(
                parent, 24, 8, 48, 48, "‹", size=36,
                color="#91919B", align="center",
                events=events, callback=callback,
            ))
        objs.append(label(
            parent, 0, 0, 480, 64, title, size=24,
            color="#91919B", align="center",
        ))


def app_root_shell(objs, parent, title, *, name="app_shell",
                   header_style="compact", bg="#000000", fg="#D6D6DE",
                   indicator_parent=None):
    """Declare shared chrome ownership without adding App scene objects."""
    del objs, parent, title, name, header_style, bg, fg, indicator_parent


def build_settings_list_page(objs, parent, *, title, rows, page_index,
                             root=False, subtitle=None):
    """One settings stack page: nav header + up to 5 rows + home indicator.

    rows: list of (title, value, dest) where dest is:
      None | int page | {"drawer": "quick_drawer"}
    """
    if root:
        app_root_shell(
            objs, parent, title, name=f"shell_{page_index}",
            header_style="settings",
        )
    else:
        settings_nav_header(
            objs, parent, title, name=f"nav_{page_index}", root=False,
        )
    y0 = 70
    if subtitle:
        objs.append(label(
            parent, 16, 58, 448, 22, subtitle, size=16, color="#91919B",
        ))
        y0 = 88
    for i, row in enumerate(rows):
        row_title, value, dest = row[0], row[1], row[2]
        page = dest if isinstance(dest, int) else None
        drawer = dest.get("drawer") if isinstance(dest, dict) else None
        settings_list_row(
            objs, parent, y0 + i * SETTINGS_ROW_STRIDE, row_title,
            value or "", page=page, drawer=drawer,
            name=f"row_{page_index}_{i}",
        )


def build_settings_quick_shade(objs, parent, *, drawer_name="quick_drawer"):
    """Top-shade Quick Settings (spec openShade for 亮度 / 音量)."""
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H,
                          bg="#000000", name="quick_background"))
    objs.append(container(parent, 0, 0, CONTENT_W, 456,
                          bg="#101011", name="quick_surface"))
    objs.append(container(parent, 201, 8, 78, 5, bg="#595959", radius=3))
    objs.append(label(parent, 16, 22, 300, 38, "QUICK SETTINGS", size=30,
                      color="#D6D6DE"))
    objs.append(button(
        parent, 378, 18, 86, 38, "Close", radius=19, size=16,
        events=[{"event": "click", "action": "drawer_close",
                 "target_name": drawer_name}],
    ))
    for x, title, value, bind in (
            (12, "VOLUME", 50, "quick_volume"),
            (246, "BRIGHTNESS", 70, "quick_brightness")):
        card = len(objs)
        objs.append(container(parent, x, 76, 222, 128, bg="#181819",
                              radius=28))
        objs.append(label(card, 18, 14, 150, 24, title, size=16,
                          color="#91919B"))
        objs.append(label(card, 164, 12, 40, 28, str(value), size=18,
                          align="right"))
        objs.append({
            "type": "slider", "parent": card,
            "x": 18, "y": 66, "w": 186, "h": 22,
            "value": value, "min": 0, "max": 100,
            "bind": bind, "name": bind,
            "bg_color": "#3B3C3D", "fg_color": "#FF4C01",
            "knob_color": "#FCFCFF", "knob": True,
        })
    objs.append(label(
        parent, 16, 230, 448, 40,
        "亮度 / 音量从显示与声音进入此下拉面板",
        size=16, color="#595959",
    ))


def build_camera_page(objs, parent, assets_prefix, camera_assets_prefix):
    app_root_shell(objs, parent, "Camera", name="shell_camera")
    del assets_prefix
    objs.append(image(
        parent, f"{camera_assets_prefix}/camera_canvas_fullscreen.png",
        0, 0, 480, 480, bind="camera_canvas", name="camera_preview",
    ))
    missing = len(objs)
    objs.append(layer(
        parent, 24, 200, 432, 80, hidden=True,
        name="camera_missing_layer", bind="camera_missing_visible",
    ))
    objs.append(label(
        missing, 0, 16, 432, 48, "Camera board not detected", size=24,
        color="#FCFCFF", align="center", name="camera_missing_label",
    ))
    # Keep one invisible-on-black glyph so the single-scene bundle still
    # materializes its required font resource after removing the title text.
    objs.append(label(
        parent, 0, 0, 20, 20, ".", size=16, color="#000000",
        font_charset=".",
    ))
    objs.append(container(
        parent, 0, 0, 480, 80,
        bg="#3B3C3D", opacity=160, name="camera_top_bar",
    ))
    objs.append(label(
        parent, 16, 27, 304, 28, "", size=18, color="#FCFCFF",
        bind="camera_recognition_status", name="camera_recognition_status",
    ))
    objs.append(image(
        parent, f"{camera_assets_prefix}/camera_recognition.png",
        344, 16, 48, 48, name="camera_recognition_button",
        callback="camera_recognition_menu",
    ))
    recognition_active = len(objs)
    objs.append(layer(
        parent, 379, 18, 8, 8, hidden=True,
        name="camera_recognition_active", bind="camera_recognition_active_visible",
    ))
    objs.append(container(recognition_active, 0, 0, 8, 8, bg="#FF4C01", radius=4))
    flash_off = len(objs)
    objs.append(layer(
        parent, 408, 16, 48, 48, name="camera_flash_off_layer",
        bind="camera_flash_off_visible",
    ))
    objs.append(image(
        flash_off, f"{camera_assets_prefix}/camera_flash_off.png",
        0, 0, 48, 48, name="camera_flash_off",
        callback="camera_flash_toggle",
    ))
    flash_auto = len(objs)
    objs.append(layer(
        parent, 408, 16, 48, 48, hidden=True,
        name="camera_flash_auto_layer", bind="camera_flash_auto_visible",
    ))
    objs.append(image(
        flash_auto, f"{camera_assets_prefix}/camera_flash_auto.png",
        0, 0, 48, 48, name="camera_flash_auto",
        callback="camera_flash_toggle",
    ))
    flash_on = len(objs)
    objs.append(layer(
        parent, 408, 16, 48, 48, hidden=True,
        name="camera_flash_on_layer", bind="camera_flash_on_visible",
    ))
    objs.append(image(
        flash_on, f"{camera_assets_prefix}/camera_flash_on.png",
        0, 0, 48, 48, name="camera_flash_on",
        callback="camera_flash_toggle",
    ))
    objs.append(container(
        parent, 0, 390, 480, 90,
        bg="#3B3C3D", opacity=160, name="camera_bottom_bar",
    ))
    objs.append(image(
        parent, f"{camera_assets_prefix}/camera_album.png",
        64, 409, 48, 48, name="camera_album", callback="camera_album",
    ))
    objs.append(image(
        parent, f"{camera_assets_prefix}/camera_shutter.png",
        210, 401, 60, 60, name="camera_shutter", callback="camera_shutter",
    ))
    objs.append(image(
        parent, f"{camera_assets_prefix}/camera_flip.png",
        368, 409, 48, 48, name="camera_flip", callback="camera_flip",
    ))
    result = len(objs)
    objs.append(layer(
        parent, 24, 326, 432, 48, hidden=True,
        name="camera_recognition_result", bind="camera_recognition_result_visible",
    ))
    objs.append(container(result, 0, 0, 432, 48, bg="#181819", radius=16, opacity=224))
    objs.append(label(
        result, 16, 11, 400, 26, "", size=18, color="#FCFCFF", align="center",
        bind="camera_recognition_result_text", name="camera_recognition_result_text",
    ))
    menu = len(objs)
    objs.append(layer(
        parent, 188, 80, 280, 128, hidden=True,
        name="camera_recognition_menu", bind="camera_recognition_menu_visible",
    ))
    objs.append(container(menu, 0, 0, 280, 128, bg="#181819", radius=20, opacity=240))
    objs.append(button(menu, 16, 14, 112, 96, "", bg="#282829", radius=16, callback="camera_qrcode_mode"))
    objs.append(button(menu, 152, 14, 112, 96, "", bg="#282829", radius=16, callback="camera_color_mode"))
    qrcode_selected = len(objs)
    objs.append(layer(menu, 16, 14, 112, 96, hidden=True,
                      name="camera_qrcode_selected", bind="camera_qrcode_selected_visible"))
    objs.append(container(qrcode_selected, 0, 0, 112, 96, bg="#5A2B1B", radius=16,
                          border="#FF4C01", border_w=2, callback="camera_qrcode_mode"))
    color_selected = len(objs)
    objs.append(layer(menu, 152, 14, 112, 96, hidden=True,
                      name="camera_color_selected", bind="camera_color_selected_visible"))
    objs.append(container(color_selected, 0, 0, 112, 96, bg="#5A2B1B", radius=16,
                          border="#FF4C01", border_w=2, callback="camera_color_mode"))
    objs.append(label(menu, 42, 22, 60, 48, "QR", size=26, color="#FCFCFF", align="center", callback="camera_qrcode_mode"))
    objs.append(label(menu, 178, 20, 60, 48, "●", size=32, color="#4FD66D", align="center", callback="camera_color_mode"))
    objs.append(label(menu, 24, 76, 96, 24, "QR Code", size=16, color="#D6D6DE", align="center", callback="camera_qrcode_mode"))
    objs.append(label(menu, 160, 76, 96, 24, "Color", size=16, color="#D6D6DE", align="center", callback="camera_color_mode"))


def build_imu_page(objs, parent):
    app_root_shell(objs, parent, "IMU", name="shell_imu")
    objs.append(container(parent, 12, 74, 456, 261, bg="#F9FAFB", radius=43,
                          name="imu_horizon"))
    objs.append(container(parent, 12, 204, 456, 2, bg="#000000"))
    objs.append(container(parent, 239, 74, 2, 261, bg="#000000"))
    bubble = len(objs)
    objs.append({
        "type": "container", "parent": parent,
        "x": {"default": 202, "min": 130, "max": 270},
        "y": {"default": 168, "min": 90, "max": 240},
        "w": 76, "h": 76,
        "bg_color": "#FFB020", "radius": 38, "name": "imu_bubble",
    })
    objs.append(label(bubble, 0, 14, 76, 48, "0°", size=36, color="#FCFCFF",
                      align="center", bind="imu_angle", name="imu_angle",
                      font_charset="-+.0123456789°"))
    cards = [
        (12, "Pitch", "76", "imu_pitch"),
        (170, "Roll", "0", "imu_roll"),
        (327, "Yaw", "120", "imu_yaw"),
    ]
    for x, name, val, bind in cards:
        card = len(objs)
        objs.append(container(parent, x, 351, 141, 106, bg="#181819",
                              radius=20, border="#3B3C3D", border_w=1))
        objs.append(label(card, 0, 16, 141, 22, name, size=16, color="#91919B",
                          align="center"))
        objs.append(label(card, 0, 42, 141, 50, val, size=48, color="#FCFCFF",
                          align="center", bind=bind, name=bind,
                          font_charset="-+.0123456789°"))


def build_tof_page(objs, parent, assets_prefix, bold_font):
    card = len(objs)
    objs.append(container(parent, 12, 12, 456, 456, bg="#181819", radius=40,
                          name="tof_card"))
    app_root_shell(
        objs, card, "TOF", name="shell_tof", bg="#181819",
        indicator_parent=parent,
    )
    objs.append(label(card, 20, 96, 360, 130, "32.5", size=120, color="#FCFCFF",
                      align="center", bind="tof_dist", name="tof_dist",
                      font=bold_font, font_charset="-.0123456789"))
    objs.append(label(card, 360, 200, 60, 30, "cm", size=20, color="#91919B",
                      align="left"))
    objs.append(image(card, f"{assets_prefix}/icons/tof_ruler.png",
                      16, 275, 414, 20))
    for x, t in ((16, "0"), (100, "25"), (190, "50"), (290, "75"), (390, "100")):
        objs.append(label(card, x, 300, 40, 28, t, size=20, color="#E1E4E9",
                          align="center"))
    objs.append(label(card, 16, 328, 120, 22, "Threshold", size=16,
                      color="#91919B", align="left"))
    objs.append({
        "type": "slider", "parent": card,
        "x": 16, "y": 352, "w": 414, "h": 20,
        "value": 50, "min": 0, "max": 100,
        "bind": "tof_thresh", "name": "tof_thresh",
        "bg_color": "#3B3C3D", "fg_color": "#FF4C01", "knob": True,
    })
    objs.append(container(card, 188, 374, 80, 32, bg="#FF4C01"))
    objs.append(label(card, 188, 376, 80, 28, "VALID", size=20, color="#FCFCFF",
                      align="center", bind="tof_status", name="tof_status",
                      font_charset="VALIDN"))


def build_env_page(objs, parent, assets_prefix):
    app_root_shell(objs, parent, "Env", name="shell_env")
    temp = len(objs)
    objs.append(container(parent, 12, 56, 219, 190, bg="#F9FAFB", radius=40))
    objs.append(container(temp, 24, 22, 14, 14, bg="#ED4600"))
    objs.append(label(temp, 46, 16, 160, 28, "TEMPERATURE", size=16,
                      color="#91919B"))
    objs.append(label(temp, 10, 60, 200, 70, "24.5", size=75, color="#1A1A1A",
                      align="center", bind="env_temp", name="env_temp",
                      font_charset="-.0123456789"))
    objs.append(label(temp, 160, 130, 40, 28, "°C", size=20, color="#91919B",
                      align="right"))
    objs.append(image(temp, f"{assets_prefix}/icons/env_flame.png",
                      24, 150, 24, 24))
    objs.append(image(temp, f"{assets_prefix}/icons/env_scale.png",
                      4, 172, 210, 14))

    hum = len(objs)
    objs.append(container(parent, 12, 258, 219, 190, bg="#181819", radius=40))
    objs.append(container(hum, 24, 20, 14, 14, bg="#FF5B24"))
    objs.append(label(hum, 46, 14, 150, 28, "HUMIDITY", size=16, color="#D6D6DE"))
    objs.append(label(hum, 10, 70, 200, 70, "76", size=75, color="#FCFCFF",
                      align="center", bind="env_humidity", name="env_humidity",
                      font_charset=".0123456789"))
    objs.append(label(hum, 140, 145, 60, 28, "%RH", size=20, color="#91919B",
                      align="right"))

    right = len(objs)
    objs.append(container(parent, 247, 56, 219, 392, bg="#181819", radius=40))
    objs.append(container(right, 24, 20, 14, 14, bg="#FF5B24"))
    objs.append(label(right, 46, 14, 120, 28, "HCHO", size=16, color="#D6D6DE"))
    objs.append(label(right, 10, 60, 200, 70, "30", size=75, color="#FCFCFF",
                      align="center", bind="env_hcho", name="env_hcho",
                      font_charset=".0123456789"))
    objs.append(label(right, 120, 130, 80, 28, "ug/m3", size=18, color="#91919B",
                      align="right"))
    objs.append(container(right, 24, 200, 14, 14, bg="#FF5B24"))
    objs.append(label(right, 46, 194, 120, 28, "TVOCS", size=16, color="#D6D6DE"))
    objs.append(label(right, 10, 250, 200, 70, "260", size=75, color="#FCFCFF",
                      align="center", bind="env_tvoc", name="env_tvoc",
                      font_charset=".0123456789"))
    objs.append(label(right, 120, 325, 80, 28, "ug/m3", size=18, color="#91919B",
                      align="right"))


def status_bar(objs, parent, assets_prefix, *, time_name="status_time",
               time_size=24, bind_prefix="status", y=12):
    """Fixed hub chrome."""
    objs.append(label(parent, 20, y, 120, 40, "12:00", size=time_size,
                      bind=f"{bind_prefix}_time", name=time_name,
                      font_charset=":0123456789"))
    # Match the web status bar's intrinsic-width flex row: actual icon ink is
    # packed at a 7 px gap and the battery remains 24 px from the right edge.
    # The PNGs stay on 40x40 canvases for GSP, so their slot origins account
    # for each asset's transparent inset.
    logo = len(objs)
    objs.append(layer(
        parent, 357, y, 40, 40,
        name=f"{bind_prefix}_agent", bind=f"{bind_prefix}_agent",
        hidden=True,
    ))
    objs.append(image(logo, f"{assets_prefix}/icons/status_logo.png",
                      0, 0, 40, 40))
    # Hub shows one of status_wifi_0..4 (0 = on/no-link, 1..4 = RSSI).
    wifi = len(objs)
    objs.append(layer(
        parent, 386, y, 40, 40,
        name=f"{bind_prefix}_wifi", bind=f"{bind_prefix}_wifi", hidden=True,
    ))
    for level in range(0, 5):
        objs.append(image(
            wifi,
            f"{assets_prefix}/icons/status_wifi_{level}.png",
            0, 0, 40, 40,
            name=f"{bind_prefix}_wifi_bar_{level}",
            bind=f"{bind_prefix}_wifi_bar_{level}",
        ))
        objs[-1]["bind_target"] = "visible"
        objs[-1]["hidden"] = level != 0
    # Web status glyph: continuous proportional fill inside one unchanged
    # silhouette. Charging switches the whole glyph to the success green.
    batt = len(objs)
    objs.append(layer(parent, 420, y, 40, 40,
                      name=f"{bind_prefix}_battery"))
    for state, outline, fill_color, track, hidden in (
            ("idle", "#91919B", "#FCFCFF", "#171719", False),
            ("low", "#91919B", "#FF0000", "#171719", True),
            ("charging", "#63C270", "#63C270", "#101F12", True)):
        state_layer = len(objs)
        objs.append(layer(
            batt, 0, 0, 40, 40,
            name=f"{bind_prefix}_battery_{state}",
            bind=f"{bind_prefix}_battery_{state}", hidden=hidden,
        ))
        objs.append(container(
            state_layer, 5, 13, 25, 14,
            bg=track, radius=4,
            border=outline, border_w=1,
            name=f"{bind_prefix}_battery_{state}_outline",
        ))
        objs.append({
            "type": "progress", "parent": state_layer,
            "x": 8, "y": 16, "w": 20, "h": 8,
            "value": 100, "min": 0, "max": 100,
            "bg_color": "#00000000", "fg_color": fill_color,
            "radius": 2,
            "name": f"{bind_prefix}_battery_{state}_level",
            "bind": f"{bind_prefix}_battery_{state}_level",
        })
        objs.append(container(
            state_layer, 33, 17, 2, 6,
            bg=outline, radius=1,
            name=f"{bind_prefix}_battery_{state}_terminal",
        ))


def page_dots(objs, parent, active):
    objs.append(container(
        parent, 200, 450, 12, 12,
        bg="#FCFCFF" if active == 1 else "#3B3C3D",
        radius=6, name="dot_apps1",
    ))
    objs.append(container(
        parent, 224, 450, 12, 12,
        bg="#FCFCFF" if active == 2 else "#3B3C3D",
        radius=6, name="dot_apps2",
    ))
    objs.append(button(
        parent, 194, 446, 16, 16, "", radius=8, size=16,
        bg="#000000", fg="#000000", name="tap_dot_apps1",
        callback="hub_goto_apps1",
    ))
    objs.append(button(
        parent, 218, 446, 16, 16, "", radius=8, size=16,
        bg="#000000", fg="#000000", name="tap_dot_apps2",
        callback="hub_goto_apps2",
    ))
    objs.append(button(
        parent, 16, 440, 72, 32, "Home", radius=16, size=16,
        name="back_home", callback="hub_goto_home",
    ))


def scene_out_path(here: Path, filename: str) -> Path:
    """Resolve scene JSON output path (build dir via MOSAIC_SCENE_OUT_DIR)."""
    import os

    out_dir = os.environ.get("MOSAIC_SCENE_OUT_DIR")
    if out_dir:
        return Path(out_dir) / filename
    return here / filename


def write_scene(path: Path, screen: str, objects, *, font: str,
                default_font_size: int = 18, actions=None):
    scene = {
        "screen": screen,
        "w": SCREEN_W,
        "h": SCREEN_H,
        "screen_bg": "#000000",
        "font": font,
        "default_font_size": default_font_size,
        "font_link": "auto",
        "objects": objects,
    }
    if actions:
        scene["actions"] = actions
    path.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")
    print("wrote", path.name)


def asset_scene(path: Path, screen: str, images, font_policies, bold_font,
                font):
    asset_objects = [container(-1, 0, 0, SCREEN_W, SCREEN_H)]
    for index, (src, w, h) in enumerate(images):
        asset_objects.append(image(
            0, src, (index % 4) * 120, (index // 4) * 120, w, h,
        ))
    write_scene(path, screen, asset_objects, font=font)
