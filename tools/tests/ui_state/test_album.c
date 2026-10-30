/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../../../components/mosaic_ui/apps/album/album_app.c"

static bool normal, selection, deletion;
esp_gsp_err_t esp_gsp_set_visible(esp_gsp_handle_t ui, uint16_t bind, bool visible)
{
    (void)ui;
    if (bind == GSP_BIND_ALBUM_TOOLBAR_NORMAL_VISIBLE) normal = visible;
    else if (bind == GSP_BIND_ALBUM_TOOLBAR_SELECT_VISIBLE) selection = visible;
    else if (bind == GSP_BIND_ALBUM_SELECT_DELETE_ENABLED_VISIBLE) deletion = visible;
    else assert(false);
    return ESP_GSP_OK;
}
esp_gsp_err_t esp_gsp_set_text(esp_gsp_handle_t ui, uint16_t bind, const char *text)
{ (void)ui; assert(bind == GSP_BIND_ALBUM_SELECT_COUNT); assert(text != NULL); return ESP_GSP_OK; }
int main(void)
{
    esp_gsp_handle_t ui = (esp_gsp_handle_t)(uintptr_t)1;
    memset(&s_album, 0, sizeof(s_album));
    album_refresh_toolbar(ui);
    assert(normal && !selection && !deletion);
    s_album.photo_count = 3; s_album.multi_select = true;
    album_refresh_toolbar(ui);
    assert(!normal && selection && !deletion);
    s_album.selected[1] = true;
    album_refresh_toolbar(ui);
    assert(selection && deletion);
    s_album.selected[1] = false;
    album_refresh_toolbar(ui);
    assert(selection && !deletion);
    puts("PASS Delete follows a nonempty selection and disappears after deselecting the last photo");

    s_album.fullscreen = true; s_album.multi_select = false;
    album_refresh_toolbar(ui);
    assert(!normal && !selection && !deletion);
    puts("PASS fullscreen with no selection exposes neither selection toolbar nor Delete");
    return 0;
}
