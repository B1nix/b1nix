/* M55 acceptance: validate the b1nix C++ runtime with a real modern engine.
 *
 * litehtml is a C++ HTML/CSS layout engine (STL-heavy: std::string/vector/map/
 * shared_ptr, exceptions, RTTI). Running it end-to-end on b1nix — parse HTML,
 * cascade CSS, lay out a box tree, and emit draw calls — exercises the whole
 * hosted C++ stack (libstdc++ + libsupc++ + libgcc unwinder) far harder than a
 * unit smoke can. We feed it a styled page and assert the layout it produces:
 * the <h1> renders at its CSS font-size, above the <p>, with the right text.
 *
 * litehtml separates layout from drawing: the embedder supplies a
 * document_container. Ours records draw_text calls (text + font size + y) with
 * a fixed font-metrics model — enough to prove cascade + layout + draw without
 * a rasterizer. */
#include <litehtml.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

using litehtml::pixel_t;

static void mark(const char *s) { ::write(1, s, std::strlen(s)); }

struct DrawnText {
  std::string text;
  pixel_t font_size;
  pixel_t y;
};

/* Minimal recording container. Font handle == rounded pixel size, so draw_text
 * can recover the size the cascade chose for each run. */
class RecordingContainer : public litehtml::document_container {
public:
  std::vector<DrawnText> drawn;
  pixel_t viewport_w = 800, viewport_h = 600;

  litehtml::uint_ptr create_font(const litehtml::font_description &descr,
                                 const litehtml::document *,
                                 litehtml::font_metrics *fm) override {
    pixel_t sz = descr.size > 0 ? descr.size : 16;
    if (fm) {
      fm->font_size = sz;
      fm->height = sz * 1.2f;
      fm->ascent = sz * 0.8f;
      fm->descent = sz * 0.2f;
      fm->x_height = sz * 0.5f;
      fm->ch_width = sz * 0.5f;
      fm->draw_spaces = false;
    }
    return (litehtml::uint_ptr)(long)(sz + 0.5f);
  }
  void delete_font(litehtml::uint_ptr) override {}
  pixel_t text_width(const char *text, litehtml::uint_ptr hFont) override {
    return (pixel_t)std::strlen(text) * ((pixel_t)hFont * 0.5f);
  }
  void draw_text(litehtml::uint_ptr, const char *text, litehtml::uint_ptr hFont,
                 litehtml::web_color, const litehtml::position &pos) override {
    drawn.push_back({std::string(text), (pixel_t)hFont, pos.y});
  }
  pixel_t pt_to_px(float pt) const override { return pt * 4.0f / 3.0f; }
  pixel_t get_default_font_size() const override { return 16; }
  const char *get_default_font_name() const override { return "monospace"; }

  void get_viewport(litehtml::position &viewport) const override {
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = viewport_w;
    viewport.height = viewport_h;
  }
  void get_media_features(litehtml::media_features &media) const override {
    media.type = litehtml::media_type_screen;
    media.width = (pixel_t)viewport_w;
    media.height = (pixel_t)viewport_h;
    media.device_width = (pixel_t)viewport_w;
    media.device_height = (pixel_t)viewport_h;
    media.color = 8;
    media.resolution = 96;
  }

  /* Everything below is irrelevant to a headless layout check: no images, no
   * external CSS, no painting backend. */
  void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker &) override {}
  void load_image(const char *, const char *, bool) override {}
  void get_image_size(const char *, const char *, litehtml::size &sz) override {
    sz.width = 0;
    sz.height = 0;
  }
  void draw_image(litehtml::uint_ptr, const litehtml::background_layer &,
                  const std::string &, const std::string &) override {}
  void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer &,
                       const litehtml::web_color &) override {}
  void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer &,
                            const litehtml::background_layer::linear_gradient &) override {}
  void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer &,
                            const litehtml::background_layer::radial_gradient &) override {}
  void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer &,
                           const litehtml::background_layer::conic_gradient &) override {}
  void draw_borders(litehtml::uint_ptr, const litehtml::borders &,
                    const litehtml::position &, bool) override {}
  void set_caption(const char *) override {}
  void set_base_url(const char *) override {}
  void link(const std::shared_ptr<litehtml::document> &,
            const litehtml::element::ptr &) override {}
  void on_anchor_click(const char *, const litehtml::element::ptr &) override {}
  void on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) override {}
  void set_cursor(const char *) override {}
  void transform_text(litehtml::string &, litehtml::text_transform) override {}
  void import_css(litehtml::string &, const litehtml::string &, litehtml::string &) override {}
  void set_clip(const litehtml::position &, const litehtml::border_radiuses &) override {}
  void del_clip() override {}
  litehtml::element::ptr create_element(const char *, const litehtml::string_map &,
                                        const std::shared_ptr<litehtml::document> &) override {
    return nullptr;
  }
  void get_language(litehtml::string &language, litehtml::string &culture) const override {
    language = "en";
    culture = "";
  }
};

int main() {
  const char *html =
      "<html><head><style>"
      "h1 { font-size: 32px; }"
      "p  { font-size: 16px; }"
      "</style></head><body>"
      "<h1>Title</h1>"
      "<p>Hello b1nix</p>"
      "</body></html>";

  RecordingContainer container;

  litehtml::document::ptr doc =
      litehtml::document::createFromString(html, &container);
  if (!doc) {
    mark("M55-LITEHTML: fail parse\n");
    return 1;
  }
  mark("M55-LITEHTML: ok parse\n");

  pixel_t height = doc->render(container.viewport_w);
  if (!(height > 0)) {
    mark("M55-LITEHTML: fail layout\n");
    return 1;
  }
  mark("M55-LITEHTML: ok layout\n");

  litehtml::position clip(0, 0, (pixel_t)container.viewport_w, height);
  doc->draw((litehtml::uint_ptr)0, 0, 0, &clip);

  /* Find the title and paragraph text runs and verify the cascade + layout. */
  const DrawnText *title = nullptr;
  const DrawnText *para = nullptr;
  for (const auto &d : container.drawn) {
    if (d.text.find("Title") != std::string::npos)
      title = &d;
    if (d.text.find("Hello") != std::string::npos)
      para = &d;
  }
  if (!title || !para) {
    mark("M55-LITEHTML: fail draw (missing text runs)\n");
    return 1;
  }
  /* CSS cascade applied: h1 is 32px, p is 16px. */
  if (!(title->font_size > para->font_size) ||
      std::fabs(title->font_size - 32) > 1.0f ||
      std::fabs(para->font_size - 16) > 1.0f) {
    mark("M55-LITEHTML: fail draw (cascade)\n");
    return 1;
  }
  /* Block layout: the h1 is laid out above the paragraph. */
  if (!(title->y < para->y)) {
    mark("M55-LITEHTML: fail draw (layout order)\n");
    return 1;
  }
  mark("M55-LITEHTML: ok draw\n");
  return 0;
}
