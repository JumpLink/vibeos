// navi-style-paint: see navi-style-paint.hpp
#include "navi-style-paint.hpp"

#include <cstring>

namespace navi {

namespace {

const StyleValue null_value; // a JSON null

// whether objects a and b have the same members but for `except` (top-level "name" and "layers", a layer's "paint")
bool same_but(const StyleValue& a, const StyleValue& b, const char* const* except) {
    auto skipped = [&](const StyleValue& name) {
        for (const char* const* e = except; *e; e++)
            if (name.GetStringLength() == std::strlen(*e) &&
                std::memcmp(name.GetString(), *e, name.GetStringLength()) == 0)
                return true;
        return false;
    };
    rapidjson::SizeType counted = 0;
    for (auto m = a.MemberBegin(); m != a.MemberEnd(); ++m) {
        if (skipped(m->name)) continue;
        auto other = b.FindMember(m->name);
        if (other == b.MemberEnd() || other->value != m->value) return false;
        counted++;
    }
    rapidjson::SizeType others = 0;
    for (auto m = b.MemberBegin(); m != b.MemberEnd(); ++m)
        if (!skipped(m->name)) others++;
    return counted == others;
}

std::unique_ptr<StyleDocument> parse(const char* text, std::size_t length) {
    auto doc = std::make_unique<StyleDocument>();
    doc->Parse<0>(text, length);
    if (doc->HasParseError() || !doc->IsObject()) return nullptr;
    return doc;
}

} // namespace

std::optional<PaintSwitch> style_paint_switch(const char* from_text, std::size_t from_length, const char* to_text,
                                              std::size_t to_length) {
    if (!from_text || !to_text) return std::nullopt;
    std::unique_ptr<StyleDocument> from = parse(from_text, from_length), to = parse(to_text, to_length);
    if (!from || !to) return std::nullopt;
    static const char* const top[] = {"name", "layers", nullptr};
    if (!same_but(*from, *to, top)) return std::nullopt;
    auto from_layers = from->FindMember("layers"), to_layers = to->FindMember("layers");
    if (from_layers == from->MemberEnd() || to_layers == to->MemberEnd() || !from_layers->value.IsArray() ||
        !to_layers->value.IsArray() || from_layers->value.Size() != to_layers->value.Size())
        return std::nullopt;

    PaintSwitch result;
    static const char* const layer_keys[] = {"paint", nullptr};
    for (rapidjson::SizeType i = 0; i < to_layers->value.Size(); i++) {
        const StyleValue& a = from_layers->value[i];
        const StyleValue& b = to_layers->value[i];
        if (!a.IsObject() || !b.IsObject() || !same_but(a, b, layer_keys)) return std::nullopt;
        auto id = b.FindMember("id");
        if (id == b.MemberEnd() || !id->value.IsString()) return std::nullopt;
        std::string layer(id->value.GetString(), id->value.GetStringLength());
        auto pa = a.FindMember("paint"), pb = b.FindMember("paint");
        const StyleValue* paint_a = pa != a.MemberEnd() && pa->value.IsObject() ? &pa->value : nullptr;
        const StyleValue* paint_b = pb != b.MemberEnd() && pb->value.IsObject() ? &pb->value : nullptr;
        if (paint_b)
            for (auto m = paint_b->MemberBegin(); m != paint_b->MemberEnd(); ++m) {
                auto old = paint_a ? paint_a->FindMember(m->name) : paint_b->MemberEnd();
                if (paint_a && old != paint_a->MemberEnd() && old->value == m->value) continue;
                result.changes.push_back({layer, std::string(m->name.GetString(), m->name.GetStringLength()),
                                          &m->value});
            }
        if (paint_a)
            for (auto m = paint_a->MemberBegin(); m != paint_a->MemberEnd(); ++m)
                if (!paint_b || !paint_b->HasMember(m->name))
                    result.changes.push_back({layer, std::string(m->name.GetString(), m->name.GetStringLength()),
                                              &null_value});
    }
    result.to = std::move(to);
    return result;
}

} // namespace navi
