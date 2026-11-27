// navi-style-paint: the change from one map style to another that differs from it only in its layers' paint, as
// Navi's day and night styles do (one generator, two palettes, style/navi-style.py). Made over the map's own
// layers, it leaves their tiles as they are: no reload, no frame without them, the colours change where they are.
// Plain rapidjson, no MapLibre, so the tests can run it on the styles themselves.
#pragma once

#include <rapidjson/document.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace navi {

// MapLibre's own JSON types (mln::JSDocument, mln::JSValue): its conversions take these
using StyleDocument = rapidjson::GenericDocument<rapidjson::UTF8<>, rapidjson::CrtAllocator>;
using StyleValue = rapidjson::GenericValue<rapidjson::UTF8<>, rapidjson::CrtAllocator>;

struct PaintChange {
    std::string layer, property;
    const StyleValue* value; // in PaintSwitch::to, or a JSON null: the property back to its default
};

struct PaintSwitch {
    std::unique_ptr<StyleDocument> to; // the new style, which `changes` point into
    std::vector<PaintChange> changes;
};

// What makes style `from` look like style `to` (both JSON text), when the two are the same but for their name and
// their layers' "paint": the same sources, glyphs, sprite and everything else at the top, the same layers in the
// same order, each with the same type, source, filter, layout and zoom range. std::nullopt when they differ in
// anything else, or either does not parse; then `to` has to be loaded as a style of its own.
std::optional<PaintSwitch> style_paint_switch(const char* from, std::size_t from_length, const char* to,
                                              std::size_t to_length);

} // namespace navi
