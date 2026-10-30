# Camera service profiles

The service requests one capture buffer and enables the equivalent window for
the supported SC101IOT mode. Other sensors retain their native format. A native
VGA sensor retries with two capture buffers. Preview RGB565
buffers, display buffers and XIP are managed elsewhere and are unchanged here.

## Equivalent SC101IOT window

`CONFIG_MOSAICO_CAMERA_EQUIVALENT_ROI` defaults to enabled for SC101IOT.
It requires the public Session trim and ownership-result APIs supplied by the
pinned GSP 1.9.0 release. Configuring ROI with an SDK lacking those APIs fails
explicitly. The option can be disabled to compare the original native format.

The service uses public `VIDIOC_G_SENSOR_FMT` / `VIDIOC_S_SENSOR_FMT` operations
before allocating native capture buffers. It recognizes the resolved SC101IOT
1280x720 UYVY DVP modes at 20 MHz and 15/25 fps, checks their window registers,
and clones the complete selected mode. Only the horizontal sensor window changes
to 768x720. Unrecognized sensors or modes retain their original format; I/O and
cleanup failures follow the service's existing retry/cleanup path.

The original mode starts at sensor coordinate (2, 2). Mosaic consumes its central
768x720 window before rotation and scaling, so the equivalent sensor window is
x=[258, 1026), y=[2, 722). This preserves the source area and scale used by the
existing preview and 480x480 photograph conversion. It does not claim pixel
identity between separate exposures or guarantee image sharpness.

The sensor API borrows the format descriptor and its register table. Both live
in permanent static storage and are initialized once under the service activation
fence, including when cleanup fails. No task or heap allocation is added. On the
tested ESP32-S31 build the extra static storage is 572 bytes. With a native UYVY
buffer plus the driver's equal-sized backup, the pixel storage budget falls from
3,686,400 to 2,211,840 bytes: a saving of 1,474,560 bytes (1.40625 MiB). Actual heap
availability also depends on GSP session caches and the rest of the system.

On ACM5, ROI alone with the earlier GSP 1.8.0 profile failed after full-photo Album
returns. ROI with the matched public trim/session-result SDK candidate passed
the same return path. The 1.9.0 integration replaces that prototype with the
released API/library pair; its device regression must be recorded separately.
