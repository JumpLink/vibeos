// test-style-paint: Navi's day and night styles differ only in their layers' paint, so the map switches between them
// over the tiles it has (navi-style-paint.hpp, navi_map_switch_style_url): no reload, no blank frame, which is what
// lets the map go dark for every drive. The styles are the build's own (style-day.json, style-night.json in
// G_TEST_BUILDDIR); anything more than paint between two styles means a reload.
#include "navi-style-paint.hpp"

#include <glib.h>

#include <set>
#include <string>

static std::string read_style(const char* name) {
    g_autofree char* path = g_test_build_filename(G_TEST_BUILT, name, nullptr);
    g_autofree char* text = nullptr;
    gsize length = 0;
    g_autoptr(GError) error = nullptr;
    g_assert_true(g_file_get_contents(path, &text, &length, &error));
    g_assert_no_error(error);
    return std::string(text, length);
}

static std::optional<navi::PaintSwitch> change(const std::string& from, const std::string& to) {
    return navi::style_paint_switch(from.data(), from.size(), to.data(), to.size());
}

static void test_day_night() {
    std::string day = read_style("style-day.json"), night = read_style("style-night.json");
    auto to_night = change(day, night), to_day = change(night, day);
    g_assert_true(to_night.has_value());
    g_assert_true(to_day.has_value());
    // every change is a colour or another paint value, none to a property's default
    std::set<std::string> layers, properties;
    for (const auto& c : to_night->changes) {
        g_assert_nonnull(c.value);
        g_assert_false(c.value->IsNull());
        layers.insert(c.layer);
        properties.insert(c.property);
    }
    g_assert_cmpuint(to_night->changes.size(), ==, to_day->changes.size());
    g_assert_cmpuint(to_night->changes.size(), >, 1000); // 55 countries' layers, each with its colours
    g_assert_true(layers.count("background"));
    g_assert_true(properties.count("background-color"));
    g_assert_true(properties.count("line-color"));
    g_assert_true(properties.count("text-color"));
    g_assert_true(properties.count("text-halo-color"));
    for (const auto& p : properties) // paint only: never a layout property
        g_assert_true(g_str_has_prefix(p.c_str(), "background-") || g_str_has_prefix(p.c_str(), "fill-") ||
                      g_str_has_prefix(p.c_str(), "line-") || g_str_has_prefix(p.c_str(), "text-") ||
                      g_str_has_prefix(p.c_str(), "icon-") || g_str_has_prefix(p.c_str(), "circle-"));
    // the night's ground
    bool ground = false;
    for (const auto& c : to_night->changes)
        if (c.layer == "background" && c.property == "background-color")
            ground = c.value->IsString() && g_ascii_strcasecmp(c.value->GetString(), "#272E3A") == 0;
    g_assert_true(ground);
    // the same style: nothing to change
    auto same = change(day, day);
    g_assert_true(same.has_value());
    g_assert_cmpuint(same->changes.size(), ==, 0);
}

static void test_more_than_paint() {
    const std::string base =
        R"({"version":8,"name":"a","sources":{"s":{"type":"vector","tiles":["https://x/{z}/{x}/{y}.mvt"]}},)"
        R"("layers":[{"id":"bg","type":"background","paint":{"background-color":"#fff"}},)"
        R"({"id":"road","type":"line","source":"s","source-layer":"t","filter":["==","class","minor"],)"
        R"("layout":{"line-cap":"round"},"paint":{"line-color":"#ccc","line-width":2}}]})";
    auto replace = [&](const std::string& from, const std::string& to) {
        std::string s = base;
        size_t at = s.find(from);
        g_assert_cmpuint(at, !=, std::string::npos);
        s.replace(at, from.size(), to);
        return s;
    };
    // paint and name: in place
    auto c = change(base, replace(R"("line-color":"#ccc")", R"("line-color":"#345")"));
    g_assert_true(c.has_value());
    g_assert_cmpuint(c->changes.size(), ==, 1);
    g_assert_cmpstr(c->changes[0].layer.c_str(), ==, "road");
    g_assert_cmpstr(c->changes[0].property.c_str(), ==, "line-color");
    g_assert_cmpstr(c->changes[0].value->GetString(), ==, "#345");
    g_assert_true(change(base, replace(R"("name":"a")", R"("name":"b")")).has_value());
    // a paint property gone: back to its default (a null)
    c = change(base, replace(R"(,"line-width":2)", ""));
    g_assert_true(c.has_value());
    g_assert_cmpuint(c->changes.size(), ==, 1);
    g_assert_cmpstr(c->changes[0].property.c_str(), ==, "line-width");
    g_assert_true(c->changes[0].value->IsNull());
    // anything else: a style of its own
    g_assert_false(change(base, replace(R"("line-cap":"round")", R"("line-cap":"butt")")).has_value());
    g_assert_false(change(base, replace(R"("minor")", R"("major")")).has_value());
    g_assert_false(change(base, replace(R"("source-layer":"t")", R"("source-layer":"u")")).has_value());
    g_assert_false(change(base, replace(R"(https://x/)", R"(https://y/)")).has_value());
    g_assert_false(change(base, replace(R"("id":"road")", R"("id":"street")")).has_value());
    g_assert_false(change(base, replace(R"("type":"line",)", R"("type":"line","minzoom":12,)")).has_value());
    g_assert_false(change(base, replace(R"({"id":"bg","type":"background","paint":{"background-color":"#fff"}},)",
                                        "")).has_value());
    g_assert_false(change(base, "{not json").has_value());
    g_assert_false(change(base, "[]").has_value());
}

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/style-paint/day-night", test_day_night);
    g_test_add_func("/style-paint/more-than-paint", test_more_than_paint);
    return g_test_run();
}
