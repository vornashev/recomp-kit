// Shared canvas state. Access only under the guest scheduler baton. Guest DIB
// addresses stay in the arena; window surfaces own top-down ARGB host pixels.
#pragma once
#include <algorithm>
#include <cstdint>
#include <climits>
#include "gdi_image.h"
#include "imports.h"
#include <array>
#include <map>
namespace gdi {
struct Surface {
    int w = 0, h = 0;
    bool dirty = false;
    std::vector<uint32_t> argb;
};
struct Rect {
    int32_t l = 0, t = 0, r = 0, b = 0;
};
struct DcState {
    uint32_t surface = 0;
    int32_t org_x = 0, org_y = 0, viewport_x = 0, viewport_y = 0;
    int32_t brush_x = 0, brush_y = 0;
    uint32_t pen = 0x4f107, brush = 0x4f100, font = 0x4f10d;
    uint32_t bitmap = 0, palette = 0, region = 0;
    uint32_t text_color = 0, bk_color = 0xffffff;
    int bk_mode = 2, rop2 = 13, stretch_mode = 1;
    int32_t pos_x = 0, pos_y = 0;
    bool clipped = false;
    std::vector<Rect> clip;
};
struct DeviceContext : DcState {
    bool memory = true;
    uint32_t window = 0; // The acquiring window, possibly a child of surface.
    // WS_CLIPCHILDREN: the visible children's areas, in surface coordinates,
    // taken when the DC is made. Writes there are dropped.
    std::vector<Rect> excluded;
    std::vector<DcState> saved;
    // The device-space box everything drawn through this DC has covered, so
    // a DirectDraw ReleaseDC compares only those rows with its baseline.
    int64_t written[4] = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
    void note_written(int64_t l, int64_t t, int64_t r, int64_t b) {
        written[0] = std::min(written[0], l);
        written[1] = std::min(written[1], t);
        written[2] = std::max(written[2], r);
        written[3] = std::max(written[3], b);
    }
};
struct Object {
    enum Kind { Brush, Pen, Font, Region } kind = Brush;
    uint32_t color = 0, style = 0;
    int32_t width = 1;
    std::array<uint8_t, 92> logfont{};
    Rect rect{};
};
std::map<uint32_t, DeviceContext> &dcs();
DeviceContext *dc_of(uint32_t dc);
std::map<uint32_t, Object> &objects();
uint32_t make_object(const Object &object);
uint32_t colorref(uint32_t argb); // Swapping R and B is its own inverse.
uint32_t argb(uint32_t color);
bool brush_color(uint32_t brush, uint32_t *pixel);
bool dc_size(uint32_t dc, int *w, int *h);
// Preserve alpha only for 32-bit BI_RGB DIB storage; normal GDI reads are opaque.
bool dc_has_alpha(uint32_t dc);
// A 1-bit DIB is selected into this DC, so blits to or from it convert between
// colour and monochrome rather than matching the nearest palette entry.
bool dc_is_monochrome(uint32_t dc);
bool read_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t *pixel, bool preserve_alpha = false);
// SRCCOPY of a w x h rectangle between two same-format DIB-backed DCs with no
// clip regions; false (nothing written) when the general per-pixel path is needed.
bool copy_dib_rect(uint32_t dst, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t src,
                   int32_t sx, int32_t sy);
// MaskBlt's mask: true with *set written when `bitmap` is a 1-bit bitmap and
// (x, y) is inside it. A set bit selects the foreground raster operation.
bool mask_bit(uint32_t bitmap, int64_t x, int64_t y, bool *set);
bool write_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t pixel, bool blend = false);
Rect clip_box(uint32_t dc);
bool drawable(uint32_t dc, int64_t x, int64_t y);
void fill(uint32_t dc, Rect r, uint32_t pixel);
void line(uint32_t dc, int32_t x, int32_t y, int32_t x1, int32_t y1);
void register_draw();
void register_text();
uint32_t draw_text(uint32_t dc, uint32_t text, uint32_t count, uint32_t rect, uint32_t flags);
} // namespace gdi
uint32_t gdi_window_dc(uint32_t window);
bool gdi_release_window_dc(uint32_t window, uint32_t dc);
void gdi_destroy_window(uint32_t window);
// Borrowed DirectDraw memory is never freed by GDI. The owning surface pairs
// bind/unbind around its DC lifetime and publishes changes on ReleaseDC.
void gdi_bind_surface_dc(uint32_t dc, int w, int h, int bpp, uint32_t pitch, uint32_t bits,
                         const uint32_t *palette);
void gdi_unbind_surface_dc(uint32_t dc);
// The rows [first, last] GDI wrote through `dc` since it was bound; false when
// it wrote nothing.
bool gdi_written_rows(uint32_t dc, int32_t *first, int32_t *last);
namespace gdi {
Rect to_device(uint32_t dc, Rect rect);
}

uint32_t gdi_new_dc();

// Called only with the guest baton. A present owns the composed pixel copy.
void gdi_present_windows(bool refresh = false);
void gdi_composite_windows(uint32_t *argb, int w, int h);
