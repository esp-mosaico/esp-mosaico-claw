#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Single-scene Mosaic hub: StackView + PageFlow + top Drawer.

Launcher PageFlow tabs:
  0 Home | 1 Apps

StackView pages:
  0 launcher | 1 notification center | 2 insert notification

Lock Screen is a top-level modal sibling of Hub navigation. AOD and charging
are mutually exclusive lock-screen modes, not navigation pages.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / 'common'))

from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    auto_charset,
    CONTENT_H,
    CONTENT_W,
    SCREEN_H,
    SCREEN_W,
    button,
    container,
    image,
    label,
    layer,
    shared_prefix,
    scene_out_path,
    status_bar,
    asset_scene,
    explicit_charset,
    shared_charset,
)


def _clock_hand(parent, name, size, color, sweep, angle):
    offset = (169 - size) // 2
    return {
        'type': 'arc', 'parent': parent,
        'x': offset, 'y': offset, 'w': size, 'h': size,
        'fg_color': color, 'thickness': size // 2,
        'sweep': sweep, 'value': 100,
        'start_angle': {'default': angle, 'min': 0, 'max': 359},
        'name': name,
    }


def append_home_clock(objs, parent, x, y):
    """Append the Hub's fixed 169px Atelier Sector analog clock."""
    name = 'home_clock'
    face = len(objs)
    objs.extend((
        {'type': 'container', 'parent': parent, 'x': x, 'y': y,
         'w': 169, 'h': 169, 'bg_color': '#E8E8E4', 'radius': 20,
         'name': name},
        {'type': 'container', 'parent': face, 'x': 8, 'y': 8,
         'w': 153, 'h': 153, 'bg_color': '#5B5D60', 'radius': 13,
         'name': f'{name}_chapter_outer'},
        {'type': 'container', 'parent': face, 'x': 9, 'y': 9,
         'w': 151, 'h': 151, 'bg_color': '#E8E8E4', 'radius': 12,
         'name': f'{name}_chapter_fill'},
        {'type': 'container', 'parent': face, 'x': 12, 'y': 12,
         'w': 145, 'h': 145, 'bg_color': '#9A9B9C', 'radius': 10,
         'name': f'{name}_chapter_inner'},
        {'type': 'container', 'parent': face, 'x': 13, 'y': 13,
         'w': 143, 'h': 143, 'bg_color': '#E8E8E4', 'radius': 9,
         'name': f'{name}_chapter_inner_fill'},
    ))
    for hour in range(12):
        major = hour in (0, 3, 6, 9)
        objs.append({
            'type': 'arc', 'parent': face,
            'x': 0, 'y': 0, 'w': 169, 'h': 169,
            'fg_color': '#FF4C01' if hour == 0 else '#55585B',
            'thickness': 14 if major else 12,
            'start_angle': hour * 30,
            'sweep': 3 if major else 2, 'value': 100,
            'name': f'{name}_tick{hour}',
        })
    objs.extend((
        _clock_hand(face, f'{name}_hour', 68, '#202124', 7, 12),
        _clock_hand(face, f'{name}_minute', 104, '#202124', 4, 144),
        _clock_hand(face, f'{name}_second', 130, '#FF4C01', 2, 0),
        {'type': 'container', 'parent': face, 'x': 80, 'y': 80,
         'w': 9, 'h': 9, 'bg_color': '#202124', 'radius': 4,
         'name': f'{name}_hub'},
    ))
    return face

# Keep the bundled DejaVu family for stable layout metrics across hosts.
FONT = DEJAVU_SANS
BOLD = DEJAVU_SANS_BOLD
MOSAIC = FONT
ASSETS = '../../common/assets'

# Figma 首页1-熄屏 (AOD only) — not the same as 时钟.
AOD_DATE_SIZE = 26
AOD_TIME_SIZE = 144
AOD_DATE_Y = 68
AOD_TIME_Y = 92

# H1 weather card temperature.  Its explicit charset keeps the degree glyph
# available even when no other static label uses it at this size.
HOME_TEMP_SIZE = 95

# Stack page indices (must match stack_push arg values).
STACK_NOTIF = 1
STACK_INSERT = 2
STACK_PAGE_COUNT = 3

# Launcher PageFlow tab indices.
TAB_HOME = 0
FLOW_PAGE_COUNT = 2

# Shared app-icon grid (Figma 时钟 bottom / 应用界面). Coords are relative to
# the PageFlow layer (stage y=56); absolute screen y = 56 + value.
# Clock bottom Frame 48095606: abs y=300 → flow y=244; cols 12 / 180 / 348.
ICON_SIZE = 118
ICON_COLS = (12, 180, 348)
ICON_ROW_TOP_Y = 35          # 应用界面 top row ≈ abs y=91
ICON_ROW_BOT_Y = 237         # prototype H1 app row: abs y=293
ICON_LABEL_GAP = 12
ICON_LABEL_H = 22

HUB_IMAGES = [
    (f'{ASSETS}/home_bg.png', 480, 480),
    (f'{ASSETS}/icons/up_chevron.png', 32, 32),
    (f'{ASSETS}/control_center/capsule_mask_72.png', 72, 152),
    (f'{ASSETS}/icons/status_logo.png', 40, 40),
    (f'{ASSETS}/icons/status_wifi.png', 40, 40),
    *[(f'{ASSETS}/icons/status_wifi_{level}.png', 40, 40)
      for level in range(0, 5)],
    (f'{ASSETS}/icons/camera.png', 118, 118),
    (f'{ASSETS}/icons/imu.png', 118, 118),
    (f'{ASSETS}/icons/controller.png', 118, 118),
    (f'{ASSETS}/icons/claw.png', 118, 118),
    (f'{ASSETS}/icons/settings.png', 118, 118),
    (f'{ASSETS}/icons/skills.png', 118, 118),
    (f'{ASSETS}/icons/album.png', 118, 118),
    (f'{ASSETS}/icons/music.png', 118, 118),
    (f'{ASSETS}/icons/bricks.png', 118, 118),
    (f'{ASSETS}/icons/weather.png', 118, 118),
    *[(f'{ASSETS}/control_center/{name}.png', width, height)
      for name, width, height in (
          ('wifi', 44, 44), ('wifi_active', 44, 44),
          ('airdrop', 44, 44), ('airdrop_active', 44, 44),
          ('bt', 44, 44), ('bt_active', 44, 44),
          ('batt', 44, 44), ('batt_active', 44, 44),
          *[(f'batt_{level}', 44, 44) for level in range(0, 5)],
          *[(f'batt_active_{level}', 44, 44) for level in range(0, 5)],
          ('mpRing', 139, 139), ('prev', 48, 48),
          ('pause', 48, 48), ('play', 48, 48), ('next', 48, 48),
          ('bellOn', 44, 44), ('bellOff', 44, 44),
          ('vibOn', 44, 44), ('vibOn_active', 44, 44),
          ('slotL', 94, 94), ('slotR', 94, 94),
          ('slot_camera', 72, 72),
          ('sunSemi_active', 48, 48), ('soundLow_active', 48, 48),
      )],
]

CHARGE_ROWS = (
    '0:180,200,220;20:160,180,200,220;40:140,160,180,200;'
    '60:140,160,180,200;63.41:s123.41;80:120,140,160,180,200;'
    '100:100,120,140,160,180,200;120:80,100,120,140,160,180,200;'
    '140:80,100,120,140,160,180;160:60,80,100,120,140,160,180,200,220,240,260,280;'
    '180:40,60,80,100,120,140,160,180,200,220,240,260,280;'
    '200:40,60,80,100,120,140,160,180,200,220,240,260;'
    '220:20,40,60,80,100,120,140,160,180,200,220,240;'
    '240:0,20,40,60,80,100,120,140,160,180,200,220;242.04:m242.04;'
    '260:100,120,140,160,180,200,220;280:100,120,140,160,180,200;'
    '300:100,120,140,160,180;320:100,120,140,160,180;'
    '340:80,100,120,140,160;360:80,100,120,140;380:80,100,120,140;'
    '400:80,100,120;420:80,100;440:80,100;460:60,80;480:60;500:60'
)


def charge_blocks():
    blocks = []
    for row in CHARGE_ROWS.split(';'):
        y_text, cells = row.split(':')
        y = float(y_text)
        for cell in cells.split(','):
            kind = cell[0] if cell[0] in 'sm' else ''
            x = float(cell[1:] if kind else cell)
            size = 3.76 if kind == 's' else 6.5 if kind == 'm' else 10.58
            blocks.append((x, y, size))
    return sorted(blocks, key=lambda block: (-block[1], block[0]))


def charge_band(block, blocks):
    """Return a bottom-to-top 0..9 band without splitting horizontal rows."""
    center_y = block[1] + block[2] / 2
    centers = [item[1] + item[2] / 2 for item in blocks]
    top = min(centers)
    bottom = max(centers)
    span = max(1.0, bottom - top)
    return min(9, int((bottom - center_y) * 10 / span))

# Keep the home temperature inside the runtime's eight-font-pack ceiling.
FONT_POLICIES = {
    16: shared_charset(),
    18: shared_charset(),
    24: auto_charset(),
    AOD_DATE_SIZE: auto_charset(),
    AOD_TIME_SIZE: explicit_charset(":0123456789"),
    HOME_TEMP_SIZE: explicit_charset("0123456789°"),
    144: explicit_charset("0123456789"),
}


def append_unlock_hint(objs, parent):
    """Shared bottom-center unlock affordance for both lock modes."""
    hint_color = {'theme': 'aod_hint'}
    # Use one anti-aliased bitmap on both lock modes instead of two coarse
    # renderer lines; the caption below remains theme-animated.
    objs.append(image(
        parent, f'{ASSETS}/icons/up_chevron.png', 228, 408, 24, 24,
    ))
    objs.append(label(
        parent, 0, 438, 480, 28, 'Swipe up to open',
        size=24, color=hint_color, align='center',
        font_charset='Swipe up to open',
    ))


def build_aod_face(objs, parent):
    """Figma 首页1-熄屏: centered date + bold HH:MM over mosaic."""
    objs.append(image(
        parent, f'{ASSETS}/home_bg.png', 0, 0, 480, 480,
        name='aod_mosaic',
    ))
    objs.append(label(
        parent, 80, AOD_DATE_Y, 173, 34, '7/20 · ',
        size=AOD_DATE_SIZE, color='#FCFCFF', align='right',
        bind='aod_date_prefix', name='aod_date_prefix',
        font=FONT,
        font_charset='0123456789/ ·',
    ))
    objs.append(label(
        parent, 253, AOD_DATE_Y, 147, 34, 'Mon',
        size=AOD_DATE_SIZE, color='#FF4C01', align='left',
        bind='aod_date_day', name='aod_date_day',
        font=FONT,
        font_charset='SunMonTueWedThuFriSat',
    ))
    objs.append(label(
        parent, 0, AOD_TIME_Y, 480, 144, '12:54',
        size=AOD_TIME_SIZE, color='#FCFCFF', align='center',
        bind='aod_clock', name='aod_clock', font=BOLD,
        font_charset=':0123456789',
    ))
    append_unlock_hint(objs, parent)


def build_clock_face(objs, parent):
    """mosaico-prototype H1: weather copy at left, live clock at right."""
    card_x, card_y = 13, 10
    card_w = 454
    objs.append(container(
        parent, card_x, card_y, card_w, 209, bg='#242425', radius=40,
        name='clock_card', callback='app_weather',
    ))

    # Match the runtime's unavailable state on the very first frame. Live
    # weather replaces these placeholders without a sample-data flash.
    weather_x = card_x + 21
    objs.append(label(
        parent, weather_x, card_y + 33, 205, 98, '--',
        size=HOME_TEMP_SIZE, color='#FCFCFF', name='home_weather_temp',
        bind='home_weather_temp', font_charset='-0123456789°',
    ))
    objs.append(label(
        parent, weather_x, card_y + 140, 205, 34, '--',
        size=24, color='#FCFCFF', name='home_weather_desc',
        bind='home_weather_desc',
        font_charset='-ClearFairCloudyRainSnowFogSleetThunderWeather',
    ))
    objs.append(label(
        parent, weather_x, card_y + 174, 205, 26, '--',
        size=18, color='#B7B7BE', name='home_weather_city',
        bind='home_weather_city',
        font_charset=shared_charset()['charset'],
    ))

    # Right face — mosaico-prototype H1 home / Atelier Sector.  The 169px
    # rounded-square geometry and warm-grey chapter ring match the prototype
    # clock tile while retaining the firmware's live hand bindings.
    # Emit contiguous native Arc nodes so StackView visibility groups stay
    # valid and firmware can update each hand through its stable name.
    orb_x, orb_y = card_x + 262, card_y + 20
    append_home_clock(objs, parent, orb_x, orb_y)  # demo 12:24

    # Web H1 order: Camera / CLAW / Works.
    shortcuts = (
        (ICON_COLS[0], 'camera', 'Camera', 'app_camera'),
        (ICON_COLS[1], 'claw', 'CLAW', 'app_ai_create'),
        (ICON_COLS[2], 'skills', 'Works', 'app_works'),
    )
    for x, icon, text, cb in shortcuts:
        objs.append(image(
            parent, f'{ASSETS}/icons/{icon}.png',
            x, ICON_ROW_BOT_Y, ICON_SIZE, ICON_SIZE,
            name=f'clock_shortcut_{icon}', callback=cb,
        ))
        objs.append(label(
            parent, x, ICON_ROW_BOT_Y + ICON_SIZE + ICON_LABEL_GAP,
            ICON_SIZE, ICON_LABEL_H, text, size=16,
            color='#D6D6DE', align='center', callback=cb,
        ))


def stack_pop_btn(objs, parent, *, x=12, y=14, w=72, h=44, text='Back'):
    objs.append(button(
        parent, x, y, w, h, text, radius=22, size=16,
        events=[{'event': 'click', 'action': 'stack_pop',
                 'target_name': 'hub_stack'}],
    ))


def place_app_icon(objs, parent, *, name, text, x, y, icon=None,
                   callback=None, events=None, button_text=None,
                   bg='#181819', visible_bind=None, hidden=False):
    """One launcher cell: 118² icon (image or solid button) + label under it."""
    cell = len(objs)
    objs.append(layer(
        parent, x, y, ICON_SIZE, ICON_SIZE + ICON_LABEL_GAP + ICON_LABEL_H,
        name=f'{name}_slot', bind=visible_bind, hidden=hidden,
    ))
    cb = callback
    if button_text is not None:
        objs.append(button(
            cell, 0, 0, ICON_SIZE, ICON_SIZE, button_text,
            bg=bg, radius=32, size=24, name=name, callback=cb, events=events,
        ))
    else:
        asset = icon if icon is not None else name
        objs.append(image(
            cell, f'{ASSETS}/icons/{asset}.png', 0, 0, ICON_SIZE, ICON_SIZE,
            name=name, callback=cb, events=events,
        ))
    objs.append(label(
        cell, 0, ICON_SIZE + ICON_LABEL_GAP, ICON_SIZE, ICON_LABEL_H,
        text, size=18, align='center', callback=cb, events=events,
    ))


def place_app_grid(objs, parent, slots):
    """Pack one launcher page into the fixed 3 x 2 App grid."""
    for i, slot in enumerate(slots):
        col, row = i % 3, i // 3
        y = ICON_ROW_TOP_Y if row == 0 else ICON_ROW_BOT_Y
        place_app_icon(
            objs, parent,
            name=slot['name'],
            text=slot['text'],
            x=ICON_COLS[col],
            y=y,
            icon=slot.get('icon'),
            callback=slot.get('callback'),
            events=slot.get('events'),
            button_text=slot.get('button_text'),
            bg=slot.get('bg', '#181819'),
            visible_bind=slot.get('visible_bind'),
            hidden=slot.get('visible_bind') is not None,
        )


def build_apps1_content(objs, parent):
    """Apps page 1 — first six launchers in the fixed 3 x 2 grid."""
    place_app_grid(objs, parent, (
        {'name': 'app_settings', 'icon': 'settings', 'text': 'Settings',
         'callback': 'app_settings'},
        {'name': 'app_imu', 'icon': 'imu', 'text': 'IMU',
         'callback': 'app_imu', 'visible_bind': 'app_slot_imu_visible'},
        {'name': 'app_album', 'icon': 'album', 'text': 'Album',
         'callback': 'app_album'},
        {'name': 'app_music', 'icon': 'music', 'text': 'Music',
         'callback': 'app_music'},
        {'name': 'app_breakout', 'icon': 'bricks', 'text': 'Bricks',
         'callback': 'app_breakout'},
        {'name': 'app_weather', 'icon': 'weather', 'text': 'Weather',
         'callback': 'app_weather'},
    ))


def build_quick_content(objs, parent):
    """C1 control center from the Mosaico interaction prototype."""
    # Keep a sliver of the covered page visible and round only the lower edge.
    # The square cap hides the top corners of the rounded panel while it remains
    # attached to the display edge.
    objs.append(container(parent, 0, 0, CONTENT_W, 472,
                          bg='#272727', radius=60,
                          name='quick_background'))
    objs.append(container(parent, 0, 0, CONTENT_W, 54, bg='#272727'))

    # Clip the mirrored status row to its real 40 px chrome band. It must sit
    # behind the module cards, as it does through the web drawer backdrop.
    status_clip = len(objs)
    objs.append(layer(parent, 0, 0, CONTENT_W, 54,
                      name='quick_status_clip'))
    status_bar(
        objs, status_clip, ASSETS, time_name='quick_status_time',
        bind_prefix='quick_status', y=7,
    )

    # Four plates match the web control center: connectivity/actions, slots,
    # media, and the two level controls.
    left_group = len(objs)
    objs.append(container(parent, 12, 54, 220, 286,
                          bg='#181819', radius=60,
                          name='quick_connectivity_group'))
    right_group = len(objs)
    objs.append(container(parent, 248, 54, 220, 206,
                          bg='#181819', radius=60,
                          name='quick_media_group'))
    slot_group = len(objs)
    objs.append(container(parent, 12, 356, 220, 116,
                          bg='#181819', radius=44,
                          name='quick_slot_group'))
    level_group = len(objs)
    objs.append(container(parent, 248, 276, 220, 196,
                          bg='#181819', radius=60,
                          name='quick_level_group'))

    # Web C1 uses a 72 px grid. Selected controls use a colored tile and a
    # white glyph; inactive controls remain charcoal with the same glyph.
    # The battery glyph is the web prototype's Low Power toggle; live SoC stays
    # exclusively in the top status bar.
    for x, y, icon, checked, name, callback in (
            (22, 8, 'wifi', True, 'quick_wlan', 'quick_wlan_toggle'),
            (126, 8, 'airdrop', True, 'quick_join', 'quick_join_toggle'),
            (22, 100, 'bt', True, 'quick_bluetooth', 'quick_bluetooth_toggle')):
        objs.append(container(
            left_group, x, y, 72, 72,
            bg='#FF4C01' if checked else '#3B3C3D', radius=36,
            name=name, callback=callback, bind=name + '_color'))
        icon_size = {'wifi': 45, 'airdrop': 48, 'bt': 44}[icon]
        icon_x = x + (72 - icon_size) // 2
        icon_y = y + (72 - icon_size) // 2
        off = image(left_group, f'{ASSETS}/control_center/{icon}.png',
                    icon_x, icon_y, icon_size, icon_size, name=name + '_off',
                    bind=name + '_off_visible', callback=callback)
        off['bind_target'] = 'visible'
        off['hidden'] = False
        objs.append(off)
        on = image(left_group,
                   f'{ASSETS}/control_center/{icon}_active.png',
                   icon_x, icon_y, icon_size, icon_size, name=name + '_on',
                   bind=name + '_on_visible', callback=callback)
        on['bind_target'] = 'visible'
        on['hidden'] = True
        objs.append(on)

    batt_x, batt_y = 126, 100
    objs.append(container(
        left_group, batt_x, batt_y, 72, 72,
        bg='#3B3C3D', radius=36, callback='quick_low_power_toggle',
        name='quick_low_power', bind='quick_low_power_color'))
    objs.append(image(
        left_group, f'{ASSETS}/control_center/batt.png',
        batt_x + 17, batt_y + 17, 38, 38,
        name='quick_low_power_icon', callback='quick_low_power_toggle',
    ))

    # Music dial and transport controls. The nested circles reproduce the
    # prototype's film-ring silhouette without adding another bitmap asset.
    objs.append(image(right_group, f'{ASSETS}/control_center/mpRing.png',
                      40, 13, 139, 139))
    objs.append(image(right_group, f'{ASSETS}/control_center/prev.png',
                      34, 152, 48, 48))
    objs.append(image(right_group, f'{ASSETS}/control_center/pause.png',
                      86, 152, 48, 48, name='quick_play'))
    objs.append(image(right_group, f'{ASSETS}/control_center/next.png',
                      138, 152, 48, 48))
    # Playback is intentionally not implemented in the Hub. Treat the whole
    # media plate as a launcher and hand control to the Music App.
    objs.append(container(
        right_group, 0, 0, 220, 206,
        bg='#00000000', radius=60,
        name='quick_music_launcher', callback='app_music',
    ))

    # Sound and vibration belong to the same upper-left plate as the four
    # connectivity controls, rather than floating between groups.
    for x, icon, checked, name, callback in (
            (22, 'bellOn', False, 'quick_ringtone', 'quick_ringtone_toggle'),
            (126, 'vibOn', True, 'quick_vibration', 'quick_vibration_toggle')):
        objs.append(container(
            left_group, x, 192, 72, 72,
            bg=('#FF3B30' if name == 'quick_ringtone' else '#FF4C01')
            if checked else '#3B3C3D', radius=36,
            name=name, callback=callback, bind=name + '_color'))
        icon_size = 39 if icon == 'sound' else 37
        icon_x = x + (72 - icon_size) // 2
        icon_y = 192 + (72 - icon_size) // 2
        off = image(left_group, f'{ASSETS}/control_center/{icon}.png',
                    icon_x, icon_y, icon_size, icon_size, name=name + '_off',
                    bind=name + '_off_visible', callback=callback)
        off['bind_target'] = 'visible'
        off['hidden'] = False
        objs.append(off)
        on_asset = 'bellOff' if name == 'quick_ringtone' else icon
        on = image(left_group, f'{ASSETS}/control_center/{on_asset}.png',
                   icon_x, icon_y, icon_size, icon_size, name=name + '_on',
                   bind=name + '_on_visible', callback=callback)
        on['bind_target'] = 'visible'
        on['hidden'] = True
        objs.append(on)
    for x, side in ((22, 'L'), (126, 'R')):
        empty = image(
            slot_group, f'{ASSETS}/control_center/slot{side}.png',
            x, 22, 72, 72, name='quick_slot_' + side.lower())
        if side == 'L':
            empty['bind'] = 'quick_slot_l_empty_visible'
            empty['bind_target'] = 'visible'
        objs.append(empty)
        if side == 'L':
            camera = image(
                slot_group, f'{ASSETS}/control_center/slot_camera.png',
                x, 22, 72, 72, name='quick_slot_l_camera',
                bind='quick_slot_l_camera_visible')
            camera['bind_target'] = 'visible'
            camera['hidden'] = True
            objs.append(camera)

    # Volume is on the left and brightness on the right, as on the web. The
    # shorter capsules fit completely inside the non-fullscreen drawer.
    for x, value, bind, icon in (
            (22, 41, 'quick_volume', 'soundLow_active'),
            (126, 58, 'quick_brightness', 'sunSemi_active')):
        objs.append(container(level_group, x, 22, 72, 152,
                              bg='#3B3C3D', radius=36))
        objs.append({
            'type': 'progress', 'parent': level_group,
            'x': x, 'y': 22, 'w': 72, 'h': 152,
            'value': value, 'min': 0, 'max': 100, 'vertical': True,
            'bind': bind + '_fill', 'name': bind + '_fill',
            'fg_color': '#EDEFF2', 'radius': 0,
        })
        # The proven progressbar path stays rectangular internally. This
        # antialiased mask supplies only the capsule's outer clipping, without
        # adding another runtime value or mover.
        objs.append(image(
            level_group,
            f'{ASSETS}/control_center/capsule_mask_72.png',
            x, 22, 72, 152,
        ))
        icon_size = 40 if bind == 'quick_volume' else 32
        objs.append(image(
            level_group, f'{ASSETS}/control_center/{icon}.png',
            x + (72 - icon_size) // 2,
            22 + 152 - 13 - icon_size,
            icon_size, icon_size,
        ))
        # Transparent native slider remains the topmost hit target. Hub code
        # mirrors its pointer value into the flat progress fill above.
        objs.append({
            'type': 'slider', 'parent': level_group,
            'x': x, 'y': 22, 'w': 72, 'h': 152,
            'value': value, 'min': 0, 'max': 100, 'vertical': True,
            'bind': bind, 'name': bind + '_input',
            'bg_color': '#00000000', 'fg_color': '#00000000',
            'knob_color': '#00000000', 'knob': False, 'radius': 36,
            'track_size': 72, 'opacity': 0,
        })

    # Collapse affordance sits between the two lower control plates instead
    # of competing with the status icons in the top chrome.
    objs.append(image(
        parent, f'{ASSETS}/icons/up_chevron.png', 228, 450, 24, 24,
        name='quick_up_chevron',
    ))

    feedback = len(objs)
    objs.append(layer(
        parent, 0, 0, CONTENT_W, 54,
        hidden=True, name='quick_feedback', bind='quick_feedback_visible',
        bind_target='visible',
    ))
    objs.append(label(
        feedback, 120, 14, 240, 30, '', size=18, color='#FCFCFF',
        align='center', name='quick_feedback_text',
        bind='quick_feedback_text',
        font_charset='Wi-Fi OnOffVibrationNot Supported',
    ))


def build_notification_content(objs, parent):
    """Notification center: newest first (selected=0). Scroll list of items."""
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H,
                          bg='#000000', name='notif_bg'))
    objs.append(label(parent, 16, 16, 320, 40, 'Notification Center', size=24))
    stack_pop_btn(objs, parent, x=360, y=12, w=100, h=40, text='Close')

    # Newest-first list. selected=0 → open anchored on the latest item.
    objs.append({
        'type': 'list',
        'parent': parent,
        'x': 12,
        'y': 64,
        'w': 456,
        'h': 400,
        'item_height': 96,
        'selected': 0,
        'name': 'notif_list',
        'bg_color': '#000000',
        'fg_color': '#FCFCFF',
        'items': [
            'Unexpected removal · just now',
            'Environment Sensor · 2m',
            'Charging · 16min',
            'System · Setup completed · 1h',
            'CLAW · Benchmark finished',
        ],
    })
    objs.append(label(
        parent, 12, 456, 456, 20,
        'Latest on top · scroll for older', size=16, color='#595959',
        align='center',
    ))


def build_aod_content(objs, parent):
    """Always-on / 息屏 (Figma 首页1-熄屏). Hard-cut in; tap/swipe-up wakes."""
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H,
                          bg='#050505', name='aod_bg'))
    build_aod_face(objs, parent)
    mode = button(
        parent, 382, 12, 86, 44, 'CHRG', bg='#181819', radius=22,
        size=16, name='aod_show_charge', callback='lock_screen_charge',
    )
    mode['bind'] = 'aod_mode_switch_visible'
    mode['bind_target'] = 'visible'
    objs.append(mode)


def build_charging_content(objs, parent):
    """Charging sibling of AOD, using the web spec's halftone silhouette."""
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H,
                          bg='#000000', name='charge_bg'))
    # CHARGE_ROWS is expressed in the original web coordinate space
    # (about 291x511). Scale it to the authored 192x328 silhouette so every
    # block remains inside the 480x480 viewport on both renderers.
    shape_x, shape_y = 286, 90
    shape_scale = 0.64
    blocks = charge_blocks()
    for x, y, size in blocks:
        objs.append(container(
            parent, round(shape_x + x * shape_scale),
            round(shape_y + y * shape_scale),
            max(1, round(size * shape_scale)),
            max(1, round(size * shape_scale)),
            bg='#1A1A1A',
        ))
    # Ten incremental groups reproduce the web order exactly: bottom-to-top,
    # then left-to-right within each row. Runtime keeps groups <= level on.
    for level in range(1, 11):
        band_blocks = [
            block for block in blocks
            if charge_band(block, blocks) == level - 1
        ]
        scaled_blocks = [
            (
                round(shape_x + x * shape_scale),
                round(shape_y + y * shape_scale),
                max(1, round(size * shape_scale)),
            )
            for x, y, size in band_blocks
        ]
        band_x = min(x for x, _, _ in scaled_blocks)
        band_y = min(y for _, y, _ in scaled_blocks)
        band_right = max(x + size for x, _, size in scaled_blocks)
        band_bottom = max(y + size for _, y, size in scaled_blocks)
        fill = len(objs)
        objs.append(layer(
            parent, band_x, band_y, band_right - band_x,
            band_bottom - band_y,
            name=f'charge_fill_{level}', bind=f'charge_fill_{level}',
            hidden=True,
        ))
        for x, y, size in scaled_blocks:
            objs.append(container(
                fill, x - band_x, y - band_y, size, size,
                bg='#FF4C01',
            ))
    # Number + "%" like HTML flex (gap 4). "%" sits at digit-width slots;
    # runtime shows the slot that matches 1/2/3 digits (0 / 10 / 100).
    standard = len(objs)
    objs.append(layer(
        parent, 0, 0, 480, 240,
        name='charge_percent_standard', bind='charge_percent_standard',
    ))
    objs.append(label(
        standard, 32, 98, 320, 116, '0', size=144, color='#FCFCFF',
        align='left', bind='charge_percent', name='charge_percent', font=BOLD,
        font_charset='0123456789',
    ))
    full = len(objs)
    objs.append(layer(
        parent, 0, 0, 480, 240,
        name='charge_percent_100', bind='charge_percent_100', hidden=True,
    ))
    objs.append(label(
        full, 32, 126, 220, 88, '100', size=104, color='#FCFCFF',
        align='left', font=BOLD,
    ))
    # DejaVu Bold @144 digit advance ≈ 100px; gap 4px → x = 32 + n*100 + 4.
    for digits, x, y in ((1, 136, 158), (2, 236, 158), (3, 258, 170)):
        slot = len(objs)
        objs.append(layer(
            parent, x, y, 51, 44,
            name=f'charge_percent_unit_{digits}',
            bind=f'charge_percent_unit_{digits}',
            hidden=(digits != 1),
        ))
        objs.append(label(
            slot, 0, 2, 51, 40, '%', size=24, color='#91919B',
            align='left',
        ))
    append_unlock_hint(objs, parent)
    mode = button(
        parent, 382, 12, 86, 44, 'AOD', bg='#181819', radius=22,
        size=16, name='charge_show_aod', callback='lock_screen_aod',
    )
    mode['bind'] = 'charge_mode_switch_visible'
    mode['bind_target'] = 'visible'
    objs.append(mode)


def build_insert_content(objs, parent):
    """A2/A3 insert notice, matching the reviewed Web interaction."""
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H,
                          bg='#000000', name='insert_bg'))
    objs.append(label(
        parent, 49, 10, 382, 34, 'Right port', size=24,
        color='#FF4C01', bind='insert_port', name='insert_port',
        font_charset='LeftRight port'))
    objs.append(label(
        parent, 49, 52, 382, 48, 'Environment Sensor', size=36,
        color='#FCFCFF', bind='insert_board_name', name='insert_board_name',
        font_charset='Environment SensorIMUTOFThermal CameraRelayLED Matrix'))
    objs.append(label(
        parent, 49, 102, 382, 36, 'Temp / Humidity / TVOC', size=24,
        color='#91919B', bind='insert_board_cap', name='insert_board_cap',
        font_charset='Temp / HumidityTVOCMotion orientationDistance rangingThermal imagingSwitch outputsLight controlPhoto Video'))
    objs.append(button(
        parent, 64, 280, 352, 56, 'Open', radius=28, size=24,
        bg='#FF4C01', name='insert_open', callback='insert_open',
    ))
    objs.append(button(
        parent, 64, 360, 352, 56, 'Done', radius=28, size=24,
        bg='#181819', name='insert_dismiss',
        events=[{'event': 'click', 'action': 'stack_pop',
                 'target_name': 'hub_stack'}],
    ))


def build_hub_objects():
    objs, content = shared_prefix(HUB_IMAGES, FONT_POLICIES, BOLD)

    stack = len(objs)
    objs.append({
        'type': 'stackview', 'parent': content,
        'x': 0, 'y': 0, 'w': CONTENT_W, 'h': CONTENT_H,
        'name': 'hub_stack', 'page_count': STACK_PAGE_COUNT,
        'initial_page': 0,
        'capacity': STACK_PAGE_COUNT, 'axis': 'horizontal',
    })

    launcher_page = len(objs)
    objs.append(layer(stack, 0, 0, CONTENT_W, CONTENT_H,
                      name='hub_stack_page0'))
    flow = len(objs)
    objs.append({
        'type': 'page_flow', 'parent': launcher_page,
        # The status bar is fixed chrome.  Its pixels deliberately sit
        # outside the snapshot-composition viewport used while dragging.
        'x': 0, 'y': 56, 'w': CONTENT_W, 'h': CONTENT_H - 56,
        'name': 'launcher_flow', 'page_count': FLOW_PAGE_COUNT,
        'selected': TAB_HOME,
        'bar_height': 0, 'axis': 'horizontal', 'cyclic': False,
    })
    # tab0: Home = Figma 时钟 (not 息屏)
    home_main = len(objs)
    objs.append(layer(flow, 0, 0, CONTENT_W, CONTENT_H,
                      name='launcher_flow_tab0'))
    build_clock_face(objs, home_main)

    # tab1: Apps1 — icon grid only (no bottom dots / Home chrome).
    apps1_main = len(objs)
    objs.append(layer(flow, CONTENT_W, 0, CONTENT_W, CONTENT_H,
                      name='launcher_flow_tab1'))
    build_apps1_content(objs, apps1_main)

    # Fixed chrome is authored after PageFlow content so it remains topmost.
    status_bar(objs, launcher_page, ASSETS)

    # stack page 1: Notification Center
    notif_main = len(objs)
    objs.append(layer(stack, CONTENT_W, 0, CONTENT_W, CONTENT_H,
                      name='hub_stack_page1'))
    build_notification_content(objs, notif_main)

    # stack page 2: Insert notification
    insert_main = len(objs)
    objs.append(layer(stack, CONTENT_W * 2, 0, CONTENT_W, CONTENT_H,
                      name='hub_stack_page2'))
    build_insert_content(objs, insert_main)

    drawer = len(objs)
    objs.append({
        'type': 'drawer', 'parent': content,
        # The visual panel remains 472 px tall, but the Drawer owns the full
        # input viewport so an upward close gesture can begin at y=479.
        'x': 0, 'y': 0, 'w': CONTENT_W, 'h': CONTENT_H,
        'name': 'quick_drawer', 'edge': 'top', 'open': False,
    })
    quick_main = len(objs)
    objs.append(layer(drawer, 0, 0, CONTENT_W, CONTENT_H,
                      name='quick_main'))
    build_quick_content(objs, quick_main)

    # Physical insertion prelude. Both strips are authored so the hardware
    # event can select its actual port.
    # Bounded x/y make the strips eligible for synchronized position tweens.
    for side, x_default, x_min, x_max, radius in (
            ('left', -16, -16, 0, 12),
            ('right', 480, 464, 480, 12)):
        strip = container(
            content,
            {'default': x_default, 'min': x_min, 'max': x_max},
            {'default': 80, 'min': 79, 'max': 81},
            16, 315, bg='#FF4C01', radius=radius,
            name=f'insert_fx_{side}', bind=f'insert_fx_{side}_visible',
            hidden=True, opacity=204,
        )
        strip['bind_target'] = 'visible'
        objs.append(strip)

    # Lock Screen is authored last and is therefore above Drawer and all
    # navigation. Its input interceptor consumes every sample while visible.
    lock_screen = len(objs)
    objs.append(layer(
        content, 0, 0, CONTENT_W, CONTENT_H,
        hidden=True, name='lock_screen', bind='lock_screen_visible',
        block_scene_swipe=True,
    ))
    lock_aod = len(objs)
    objs.append(layer(
        lock_screen, 0, 0, CONTENT_W, CONTENT_H,
        name='lock_screen_aod', bind='lock_screen_aod_visible',
    ))
    build_aod_content(objs, lock_aod)
    lock_charge = len(objs)
    objs.append(layer(
        lock_screen, 0, 0, CONTENT_W, CONTENT_H,
        hidden=True, name='lock_screen_charge',
        bind='lock_screen_charge_visible',
    ))
    build_charging_content(objs, lock_charge)
    return objs


def write_scene(path: Path, screen: str, objects):
    scene = {
        'screen': screen,
        'w': SCREEN_W,
        'h': SCREEN_H,
        'screen_bg': '#000000',
        'themes': {
            'aod_hint': {
                'type': 'color', 'default': '#D6D6DE', 'dynamic': True,
            },
        },
        'font': FONT,
        'default_font_size': 18,
        'font_link': 'auto',
        'objects': objects,
    }
    path.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + '\n',
                    encoding='utf-8')
    print('wrote', path.name)


def main():
    objects = build_hub_objects()
    write_scene(scene_out_path(HERE, 'mosaic_hub_480.json'), 'mosaic_hub', objects)

    asset_scene(scene_out_path(HERE, 'mosaic_assets_480.json'), 'mosaic_assets', HUB_IMAGES,
                FONT_POLICIES, BOLD, FONT)
    print(f'generated {len(objects)} objects in one hub scene')


if __name__ == '__main__':
    main()
