// Stremio PS5 - Native catalog, detail, settings and player presentation.
// Copyright (C) 2026 LoZazaMastro and contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "home_ui.h"
#include "app.h"

#include "core/input.hpp"
#include "core/tween.hpp"
#include "ui/components/button.hpp"
#include "ui/components/breadcrumb.hpp"
#include "ui/components/carousel.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/form.hpp"
#include "ui/components/grid.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/components/list.hpp"
#include "ui/components/loading_screen.hpp"
#include "ui/components/media_controls.hpp"
#include "ui/components/range_slider.hpp"
#include "ui/components/sheet.hpp"
#include "ui/components/stat.hpp"
#include "ui/components/surface.hpp"
#include "ui/components/tabs.hpp"
#include "ui/components/text_field.hpp"
#include "ui/components/text_view.hpp"
#include "ui/components/toast.hpp"
#include "ui/components/transition.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {
namespace ui = hui::ui;
namespace gfx = hui::gfx;
using hui::Action;
using hui::Direction;
using hui::InputFrame;
using ui::Canvas;
using ui::Event;
using ui::Feedback;
using gfx::Color;
using gfx::Rect;

constexpr float kLeft = 96.0f;
constexpr float kRight = 1824.0f;
constexpr float kWidth = kRight - kLeft;
constexpr float kFooter = 1020.0f;
constexpr float kRowPitch = 450.0f;
constexpr float kPi = 3.14159265358979323846f;

float fraction(const std::string& text) {
    char* end = nullptr;
    float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) return 0;
    return std::clamp(value / 100.0f, 0.0f, 1.0f);
}
float seconds(const std::string& text) {
    float result = 0;
    const char* cursor = text.c_str();
    for (int part = 0; part != 4 && *cursor; ++part) {
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(value)) break;
        result = result * 60.0f + std::max(value, 0.0f);
        if (*end != ':') break;
        cursor = end + 1;
    }
    return std::min(result, 604800.0f);
}
float partial_seek_limit(const App& app) {
    if (app.w_seekable_until < 0) return -1;
    const double limit = std::isfinite(app.w_seekable_until)
        ? std::clamp(app.w_seekable_until, 0.0, static_cast<double>(seconds(app.w_duration))) : 0.0;
    float value = static_cast<float>(limit);
    // A rounded-up float must not put the preview beyond the backend's exact
    // bound, even by a fraction of a frame.
    if (static_cast<double>(value) > limit) value = std::nextafter(value, 0.0f);
    return value;
}
float number(const std::string& text) {
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    return end != text.c_str() && std::isfinite(value) ? value : 0.0f;
}
int bounded(int value, std::size_t count) {
    return std::clamp(value, 0, std::max(0, static_cast<int>(count) - 1));
}
template <class T> void place(T& component, const Rect& box) {
    const auto current = component.bounds();
    if (current.x != box.x || current.y != box.y || current.w != box.w || current.h != box.h)
        component.set_bounds(box);
}
std::string first_letter(const std::string& value) {
    if (value.empty()) return "S";
    std::size_t pos = 0;
    while (pos < value.size() && std::isspace(static_cast<unsigned char>(value[pos]))) ++pos;
    if (pos == value.size()) return "S";
    const auto first = pos;
    gfx::next_codepoint(value, &pos);
    std::string letter = value.substr(first, pos - first);
    if (letter.size() == 1) letter[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(letter[0])));
    return letter;
}
void append_codepoint(std::string& result, unsigned int value) {
    if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return;
    if (value < 0x80) result += static_cast<char>(value);
    else if (value < 0x800) {
        result += static_cast<char>(0xc0 | (value >> 6));
        result += static_cast<char>(0x80 | (value & 63));
    } else if (value < 0x10000) {
        result += static_cast<char>(0xe0 | (value >> 12));
        result += static_cast<char>(0x80 | ((value >> 6) & 63));
        result += static_cast<char>(0x80 | (value & 63));
    } else {
        result += static_cast<char>(0xf0 | (value >> 18));
        result += static_cast<char>(0x80 | ((value >> 12) & 63));
        result += static_cast<char>(0x80 | ((value >> 6) & 63));
        result += static_cast<char>(0x80 | (value & 63));
    }
}

// The model's subtitle string used to be inserted into an Rml document.
// Preserve breaks and entities without rendering tags as visible captions.
std::string plain_text(std::string_view input) {
    std::string result;
    result.reserve(std::min<std::size_t>(input.size(), 16384));
    for (std::size_t pos = 0; pos < input.size() && result.size() < 16384;) {
        if (input[pos] == '<') {
            const auto end = input.find('>', pos + 1);
            if (end != std::string_view::npos && end - pos < 128) {
                std::string tag(input.substr(pos + 1, end - pos - 1));
                for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const auto space = tag.find_first_of(" \t");
                if (space != std::string::npos) tag.resize(space);
                if (!tag.empty() && tag.back() == '/') tag.pop_back();
                if (tag == "br" || tag == "/p") {
                    result += '\n'; pos = end + 1; continue;
                }
                if (!tag.empty() && tag.front() == '/') tag.erase(tag.begin());
                if (tag == "b" || tag == "i" || tag == "u" || tag == "s" || tag == "em" ||
                    tag == "strong" || tag == "font" || tag == "span" || tag == "p") {
                    pos = end + 1; continue;
                }
                // Literal angle-bracket metadata (for example release tags)
                // is data; only the supported caption markup is discarded.
            }
        }
        if (input[pos] == '&') {
            const auto end = input.find(';', pos + 1);
            if (end != std::string_view::npos && end - pos < 16) {
                const auto entity = input.substr(pos + 1, end - pos - 1);
                bool decoded = true;
                if (entity == "amp") result += '&';
                else if (entity == "lt") result += '<';
                else if (entity == "gt") result += '>';
                else if (entity == "quot") result += '"';
                else if (entity == "apos" || entity == "#39") result += '\'';
                else if (entity == "nbsp") result += ' ';
                else if (!entity.empty() && entity.front() == '#') {
                    const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
                    const std::string digits(entity.substr(hex ? 2 : 1));
                    char* final = nullptr;
                    const unsigned long cp = std::strtoul(digits.c_str(), &final, hex ? 16 : 10);
                    if (!digits.empty() && final && *final == 0 && cp <= 0x10ffff)
                        append_codepoint(result, static_cast<unsigned int>(cp));
                    else decoded = false;
                } else decoded = false;
                if (decoded) {
                    pos = end + 1;
                    continue;
                }
            }
        }
        result += input[pos++];
    }
    return result;
}

ui::Theme stremio_theme(bool contrast) {
    ui::Theme theme = ui::default_theme();
    theme.id = "stremio";
    theme.name = "Stremio Plus";
    theme.family = "Stremio Plus";
    theme.summary = "Cinematic dark surfaces with a clear controller focus";
    theme.style = ui::SurfaceStyle::flat;
    theme.page = Color::rgb(contrast ? 0x000000 : 0x0b0912);
    theme.surface = Color::rgb(contrast ? 0x151515 : 0x201b30);
    theme.surface_high = Color::rgb(contrast ? 0x292929 : 0x2d2542);
    theme.text = theme.page_text = Color::rgb(0xf6f3fc);
    theme.text_muted = theme.page_text_muted = Color::rgb(contrast ? 0xe6e6e6 : 0xb8afcc);
    theme.primary = Color::rgb(contrast ? 0xcbb4ff : 0x8360ec);
    theme.on_primary = Color::rgb(contrast ? 0x090909 : 0xffffff);
    theme.secondary = Color::rgb(contrast ? 0x303030 : 0x292136);
    theme.on_secondary = theme.text;
    theme.accent = Color::rgb(contrast ? 0xf0df97 : 0xb39af9);
    theme.outline = Color::rgb(contrast ? 0x999999 : 0x514464, contrast ? 1.0f : 0.66f);
    theme.focus = Color::rgb(0xffffff);
    theme.shadow = Color::rgb(0x000000, 0.60f);
    theme.light = Color::rgb(0xffffff, 0.15f);
    theme.radius = 14;
    theme.radius_card = 20;
    theme.border = contrast ? 2 : 1;
    theme.focus_width = contrast ? 5 : 3;
    theme.focus_gap = 5;
    theme.heading = ui::FontRole::semibold;
    theme.label = ui::FontRole::semibold;
    theme.omega = 21;
    theme.damping = 0.98f;
    theme.dark = true;
    return theme;
}

void text(Canvas& canvas, const ui::FontRef& font, const std::string& value,
          float x, float y, float size, Color ink, float width,
          gfx::Align align = gfx::Align::left) {
    if (value.empty()) return;
    std::string line = value;
    std::replace(line.begin(), line.end(), '\n', ' ');
    std::replace(line.begin(), line.end(), '\r', ' ');
    ui::text(canvas.list, font, font.font->fit(line, size, width), x, y, size, ink, align);
}

void icon(Canvas& canvas, const Rect& box, int kind, Color ink) {
    const float x = box.cx(), y = box.cy(), s = std::min(box.w, box.h) * 0.5f;
    auto& list = canvas.list;
    const float line = 2.5f;
    if (kind == 0) {
        list.line(x - s * .85f, y - s * .05f, x, y - s * .8f, line, ink);
        list.line(x, y - s * .8f, x + s * .85f, y - s * .05f, line, ink);
        list.bordered_rect({x - s * .62f, y - s * .02f, s * 1.24f, s * .85f}, 3,
                           ink.with_alpha(.06f), line, ink);
        list.rounded_rect({x - s * .15f, y + s * .35f, s * .3f, s * .46f}, 1, ink);
    } else if (kind == 1) {
        list.ring(x, y, s * .8f, line, ink);
        list.triangle({x - s * .27f, y - s * .55f, s * .54f, s * 1.1f}, ink, 0, .55f);
    } else if (kind == 2) {
        list.bordered_rect({x - s * .88f, y - s * .72f, s * .7f, s * 1.44f}, 3,
                           ink.with_alpha(.04f), line, ink);
        list.bordered_rect({x + s * .06f, y - s * .72f, s * .7f, s * 1.44f}, 3,
                           ink.with_alpha(.04f), line, ink);
    } else if (kind == 3) {
        for (int r = 0; r < 2; ++r) for (int col = 0; col < 2; ++col)
            list.bordered_rect({x - s * .8f + static_cast<float>(col) * s * .94f,
                                y - s * .8f + static_cast<float>(r) * s * .94f,
                                s * .66f, s * .66f}, 3, ink.with_alpha(.04f), line, ink);
    } else if (kind == 4) {
        list.ring(x, y, s * .58f, line, ink);
        list.circle(x, y, s * .19f, ink);
        for (int n = 0; n < 8; ++n) {
            const float a = static_cast<float>(n) * kPi * .25f;
            list.line(x + std::cos(a) * s * .61f, y + std::sin(a) * s * .61f,
                      x + std::cos(a) * s * .88f, y + std::sin(a) * s * .88f, line, ink);
        }
    } else if (kind == 6) {
        const float points[] = {x-s*.85f,y-s*.28f,x-s*.4f,y-s*.28f,x+s*.05f,y-s*.7f,
                                x+s*.05f,y+s*.7f,x-s*.4f,y+s*.28f,x-s*.85f,y+s*.28f};
        list.polygon(points, 6, ink);
        list.arc(x, y, s*.9f, line, kPi*.23f, kPi*.54f, ink);
        list.arc(x, y, s*.58f, line, kPi*.27f, kPi*.46f, ink);
    } else if (kind == 7) {
        list.bordered_rect({x-s*.94f,y-s*.67f,s*1.88f,s*1.34f}, 3, ink.with_alpha(.03f), line, ink);
        list.line(x-s*.61f,y+s*.02f,x-s*.16f,y+s*.02f,line,ink);
        list.line(x+s*.12f,y+s*.02f,x+s*.61f,y+s*.02f,line,ink);
        list.line(x-s*.61f,y+s*.32f,x+s*.1f,y+s*.32f,line,ink);
        list.line(x+s*.31f,y+s*.32f,x+s*.61f,y+s*.32f,line,ink);
    } else if (kind == 8) {
        list.circle(x, y-s*.42f, s*.23f, ink);
        list.arc(x, y+s*.57f, s*.48f, line, -kPi*.49f, kPi*.98f, ink);
        list.circle(x-s*.65f,y-s*.12f,s*.16f,ink.with_alpha(.7f));
        list.circle(x+s*.65f,y-s*.12f,s*.16f,ink.with_alpha(.7f));
        list.line(x-s*.72f,y+s*.3f,x-s*.83f,y+s*.65f,line,ink.with_alpha(.7f));
        list.line(x+s*.72f,y+s*.3f,x+s*.83f,y+s*.65f,line,ink.with_alpha(.7f));
    } else if (kind == 9) {
        list.line(x,y-s*.75f,x,y+s*.2f,line,ink);
        list.line(x-s*.3f,y-s*.08f,x,y+s*.24f,line,ink);
        list.line(x+s*.3f,y-s*.08f,x,y+s*.24f,line,ink);
        list.line(x-s*.76f,y+s*.2f,x-s*.76f,y+s*.73f,line,ink);
        list.line(x-s*.76f,y+s*.73f,x+s*.76f,y+s*.73f,line,ink);
        list.line(x+s*.76f,y+s*.73f,x+s*.76f,y+s*.2f,line,ink);
    } else if (kind == 10) {
        list.circle(x-s*.38f,y+s*.13f,s*.39f,ink);
        list.circle(x+s*.04f,y-s*.11f,s*.53f,ink);
        list.circle(x+s*.51f,y+s*.16f,s*.32f,ink);
        list.rounded_rect({x-s*.64f,y+s*.06f,s*1.33f,s*.47f}, 2, ink);
    } else if (kind == 11) {
        list.arc(x-s*.29f,y+s*.22f,s*.5f,line,kPi*.7f,kPi*1.35f,ink);
        list.arc(x+s*.29f,y-s*.22f,s*.5f,line,-kPi*.3f,kPi*1.35f,ink);
        list.line(x-s*.25f,y+s*.21f,x+s*.25f,y-s*.21f,line,ink);
    } else if (kind == 12) {
        list.star(x, y, s*.9f, ink);
    } else if (kind == 13) {
        list.line(x-s*.67f,y-s*.02f,x-s*.15f,y+s*.47f,line*1.2f,ink);
        list.line(x-s*.15f,y+s*.47f,x+s*.72f,y-s*.54f,line*1.2f,ink);
    } else if (kind == 14) {
        const float points[] = {x-s*.62f,y-s*.85f,x+s*.62f,y-s*.85f,x+s*.62f,y+s*.85f,
                                x,y+s*.39f,x-s*.62f,y+s*.85f};
        list.polygon(points, 5, ink);
    } else if (kind == 15) {
        list.ring(x,y,s*.82f,line,ink);
        list.arc(x,y,s*.6f,line,-kPi*.2f,kPi*.4f,ink);
        list.arc(x,y,s*.6f,line,kPi*.8f,kPi*.4f,ink);
        list.line(x-s*.72f,y,x+s*.72f,y,line,ink);
        list.line(x,y-s*.77f,x,y+s*.77f,line,ink);
    } else if (kind == 16) {
        list.line(x-s*.71f,y+s*.67f,x,y-s*.72f,line,ink);
        list.line(x,y-s*.72f,x+s*.71f,y+s*.67f,line,ink);
        list.line(x-s*.42f,y+s*.15f,x+s*.42f,y+s*.15f,line,ink);
    } else if (kind == 17) {
        list.ring(x,y,s*.85f,line,ink);
        list.line(x,y,x,y-s*.45f,line,ink);
        list.line(x,y,x+s*.4f,y+s*.2f,line,ink);
    } else if (kind == 18) {
        list.bordered_rect({x-s*.86f,y-s*.7f,s*1.72f,s*1.4f}, 3, ink.with_alpha(.04f), line, ink);
        list.triangle({x-s*.22f,y-s*.39f,s*.53f,s*.78f},ink,0,kPi*.5f);
    } else if (kind == 19) {
        list.bordered_rect({x-s*.9f,y-s*.65f,s*1.8f,s*1.15f}, 3, ink.with_alpha(.04f), line, ink);
        list.line(x-s*.45f,y+s*.81f,x+s*.45f,y+s*.81f,line,ink);
        list.line(x,y+s*.5f,x,y+s*.81f,line,ink);
    } else if (kind == 20) {
        list.rounded_rect({x-s*.68f,y-s*.42f,s*1.36f,s*1.17f},3,ink.with_alpha(.13f));
        list.bordered_rect({x-s*.68f,y-s*.42f,s*1.36f,s*1.17f},3,ink.with_alpha(.04f),line,ink);
        list.arc(x,y-s*.39f,s*.47f,line,-kPi*.5f,kPi,ink);
        list.circle(x,y+s*.12f,s*.11f,ink);
    } else if (kind == 21) {
        list.bordered_rect({x-s*.82f,y-s*.82f,s*.85f,s*1.64f},3,ink.with_alpha(.04f),line,ink);
        list.line(x-s*.17f,y,x+s*.8f,y,line,ink);
        list.line(x+s*.42f,y-s*.34f,x+s*.8f,y,line,ink);
        list.line(x+s*.42f,y+s*.34f,x+s*.8f,y,line,ink);
    } else if (kind == 22) {
        list.bordered_rect({x-s*.76f,y-s*.45f,s*1.52f,s*1.24f},3,ink.with_alpha(.05f),line,ink);
        list.bordered_rect({x-s*.88f,y-s*.81f,s*1.76f,s*.36f},2,ink.with_alpha(.05f),line,ink);
        list.line(x-s*.2f,y-s*.05f,x+s*.2f,y-s*.05f,line,ink);
    } else if (kind == 23) {
        list.line(x,y-s*.46f,x-s*.58f,y+s*.46f,line,ink);
        list.line(x,y-s*.46f,x+s*.58f,y+s*.46f,line,ink);
        list.line(x-s*.58f,y+s*.46f,x+s*.58f,y+s*.46f,line,ink);
        list.circle(x,y-s*.57f,s*.25f,ink); list.circle(x-s*.63f,y+s*.52f,s*.25f,ink); list.circle(x+s*.63f,y+s*.52f,s*.25f,ink);
    } else if (kind == 24) {
        list.triangle({x-s*.94f,y-s*.87f,s*1.88f,s*1.7f},ink,line);
        list.line(x,y-s*.27f,x,y+s*.12f,line,ink); list.circle(x,y+s*.47f,s*.07f,ink);
    } else {
        list.ring(x - s * .18f, y - s * .18f, s * .54f, line, ink);
        list.line(x + s * .2f, y + s * .2f, x + s * .78f, y + s * .78f, line, ink);
    }
}

std::string flag_country(std::string language) {
    for (char& c : language) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, const char*> values[] = {
        {"ita","IT"},{"it","IT"},{"eng","GB"},{"en","GB"},{"en-us","US"},{"usa","US"},
        {"fre","FR"},{"fra","FR"},{"fr","FR"},{"ger","DE"},{"deu","DE"},{"de","DE"},
        {"spa","ES"},{"es","ES"},{"por","PT"},{"pt","PT"},{"pob","BR"},{"pt-br","BR"},
        {"dut","NL"},{"nld","NL"},{"nl","NL"},{"rus","RU"},{"ru","RU"},{"ukr","UA"},{"uk","UA"},
        {"jpn","JP"},{"ja","JP"},{"chi","CN"},{"zho","CN"},{"zh","CN"},{"kor","KR"},{"ko","KR"},
        {"ara","SA"},{"ar","SA"},{"hin","IN"},{"hi","IN"},{"heb","IL"},{"he","IL"},{"tur","TR"},{"tr","TR"},
        {"pol","PL"},{"pl","PL"},{"swe","SE"},{"sv","SE"},{"nor","NO"},{"no","NO"},{"dan","DK"},{"da","DK"},
        {"fin","FI"},{"fi","FI"},{"gre","GR"},{"ell","GR"},{"el","GR"},{"rum","RO"},{"ron","RO"},{"ro","RO"},
        {"hun","HU"},{"hu","HU"},{"cze","CZ"},{"ces","CZ"},{"cs","CZ"},{"tha","TH"},{"th","TH"},
        {"ind","ID"},{"id","ID"},{"vie","VN"},{"vi","VN"},{"bul","BG"},{"bg","BG"},
        {"hrv","HR"},{"hr","HR"},{"srp","RS"},{"sr","RS"},{"per","IR"},{"fas","IR"},{"fa","IR"},
        {"alb","AL"},{"sqi","AL"},{"sq","AL"},{"bos","BA"},{"bs","BA"},{"cat","CT"},{"ca","CT"},
        {"est","EE"},{"et","EE"},{"lav","LV"},{"lv","LV"},{"lit","LT"},{"lt","LT"},
        {"mac","MK"},{"mkd","MK"},{"mk","MK"},{"may","MY"},{"msa","MY"},{"ms","MY"},
        {"slo","SK"},{"slk","SK"},{"sk","SK"},{"slv","SI"},{"sl","SI"},
    };
    for (const auto& item : values) if (language == item.first) return item.second;
    if (language.size() == 2) {
        for (char& c : language) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return language;
    }
    return {};
}

void language_flag(Canvas& c, const Rect& box, const std::string& language) {
    const std::string country = flag_country(language);
    auto& list = c.list;
    const Color white = Color::rgb(0xffffff), red = Color::rgb(0xd82035), blue = Color::rgb(0x20458b);
    const Color green = Color::rgb(0x12965b), yellow = Color::rgb(0xf6ce38), black = Color::rgb(0x171820);
    const auto rect = [&](float x, float y, float w, float h, Color color) {
        list.rounded_rect({box.x + x*box.w, box.y + y*box.h, w*box.w, h*box.h}, 0, color);
    };
    const auto horizontal = [&](Color a, Color b, Color d) {
        rect(0,0,1,1.f/3,a); rect(0,1.f/3,1,1.f/3,b); rect(0,2.f/3,1,1.f/3,d);
    };
    const auto vertical = [&](Color a, Color b, Color d) {
        rect(0,0,1.f/3,1,a); rect(1.f/3,0,1.f/3,1,b); rect(2.f/3,0,1.f/3,1,d);
    };
    list.push_clip(box);
    if (country == "IT") vertical(green,white,red);
    else if (country == "FR") vertical(blue,white,red);
    else if (country == "DE") horizontal(black,red,yellow);
    else if (country == "RO") vertical(blue,yellow,red);
    else if (country == "BE") vertical(black,yellow,red);
    else if (country == "NL") horizontal(red,white,blue);
    else if (country == "RU") horizontal(white,blue,red);
    else if (country == "HU") horizontal(red,white,green);
    else if (country == "EE") horizontal(blue,black,white);
    else if (country == "LT") horizontal(yellow,green,red);
    else if (country == "LV") { horizontal(Color::rgb(0x8f2539),white,Color::rgb(0x8f2539)); rect(0,.4f,1,.2f,white); }
    else if (country == "CT") { rect(0,0,1,1,yellow); for (int r=0;r<4;++r) rect(0,(r*2.f+1)/9,1,1.f/9,red); }
    else if (country == "SK" || country == "SI") {
        horizontal(white,blue,red);
        list.bordered_rect({box.x+box.w*.22f,box.y+box.h*.27f,box.w*.21f,box.h*.44f},3,red,box.h*.035f,white);
    }
    else if (country == "BA") {
        rect(0,0,1,1,blue); const float p[] = {box.x+box.w*.29f,box.y,box.x+box.w*.85f,box.y,box.x+box.w*.85f,box.y+box.h};
        list.polygon(p,3,yellow);
        for(int i=0;i<4;++i) list.star(box.x+box.w*(.27f+i*.13f),box.y+box.h*(.17f+i*.24f),box.h*.08f,white);
    }
    else if (country == "AL") {
        rect(0,0,1,1,red);
        const float p[] = {box.cx(),box.y+box.h*.24f,box.x+box.w*.22f,box.y+box.h*.18f,box.x+box.w*.30f,box.y+box.h*.62f,
                           box.cx(),box.y+box.h*.83f,box.x+box.w*.70f,box.y+box.h*.62f,box.x+box.w*.78f,box.y+box.h*.18f};
        list.polygon(p,6,black); list.circle(box.cx()-box.w*.06f,box.y+box.h*.23f,box.h*.085f,black); list.circle(box.cx()+box.w*.06f,box.y+box.h*.23f,box.h*.085f,black);
    }
    else if (country == "MK") {
        rect(0,0,1,1,red);
        for(int i=0;i<8;++i) { const float a=i*kPi*.25f; list.line(box.cx(),box.cy(),box.cx()+std::cos(a)*box.w,box.cy()+std::sin(a)*box.h,box.h*.11f,yellow); }
        list.circle(box.cx(),box.cy(),box.h*.22f,yellow);
    }
    else if (country == "BG") horizontal(white,green,red);
    else if (country == "IR") horizontal(green,white,red);
    else if (country == "HR" || country == "RS") { horizontal(red,white,blue); rect(.38f,.32f,.18f,.34f,red); }
    else if (country == "ES") { horizontal(red,yellow,red); rect(0,.25f,1,.5f,yellow); rect(.26f,.40f,.1f,.22f,red); }
    else if (country == "PT") { rect(0,0,.42f,1,green); rect(.42f,0,.58f,1,red); list.circle(box.x+box.w*.42f,box.cy(),box.h*.2f,yellow); }
    else if (country == "BR") {
        rect(0,0,1,1,green);
        const float p[] = {box.cx(),box.y+box.h*.10f,box.x+box.w*.90f,box.cy(),box.cx(),box.y+box.h*.90f,box.x+box.w*.10f,box.cy()};
        list.polygon(p,4,yellow); list.circle(box.cx(),box.cy(),box.h*.25f,blue);
    } else if (country == "GB") {
        rect(0,0,1,1,blue);
        list.line(box.x,box.y,box.x+box.w,box.y+box.h,box.h*.24f,white);
        list.line(box.x+box.w,box.y,box.x,box.y+box.h,box.h*.24f,white);
        list.line(box.x,box.y,box.x+box.w,box.y+box.h,box.h*.08f,red);
        list.line(box.x+box.w,box.y,box.x,box.y+box.h,box.h*.08f,red);
        rect(0,.34f,1,.32f,white); rect(.38f,0,.24f,1,white);
        rect(0,.42f,1,.16f,red); rect(.44f,0,.12f,1,red);
    } else if (country == "MY") {
        rect(0,0,1,1,white); for (int r=0;r<7;++r) rect(0,static_cast<float>(r)*2.f/14,1,1.f/14,red);
        rect(0,0,.5f,.62f,blue); list.circle(box.x+box.w*.23f,box.y+box.h*.30f,box.h*.21f,yellow);
        list.circle(box.x+box.w*.27f,box.y+box.h*.26f,box.h*.18f,blue); list.star(box.x+box.w*.36f,box.y+box.h*.31f,box.h*.12f,yellow);
    } else if (country == "US") {
        rect(0,0,1,1,white);
        for (int r=0;r<7;++r) rect(0,static_cast<float>(r)*2.f/13,1,1.f/13,red);
        rect(0,0,.45f,.54f,blue);
        for (int r=0;r<3;++r) for (int x=0;x<4;++x)
            list.circle(box.x+box.w*(.06f+x*.1f),box.y+box.h*(.09f+r*.17f),box.h*.025f,white);
    } else if (country == "JP") { rect(0,0,1,1,white); list.circle(box.cx(),box.cy(),box.h*.29f,red); }
    else if (country == "CN" || country == "VN") {
        rect(0,0,1,1,red);
        list.star(country == "CN" ? box.x+box.w*.25f : box.cx(), country == "CN" ? box.y+box.h*.31f : box.cy(), box.h*.23f,yellow);
    } else if (country == "KR") {
        rect(0,0,1,1,white); list.circle(box.cx(),box.cy(),box.h*.25f,blue);
        list.arc(box.cx(),box.cy(),box.h*.25f,box.h*.25f,-kPi*.5f,kPi,red,false);
        rect(.13f,.18f,.12f,.09f,black); rect(.76f,.7f,.12f,.09f,black);
    } else if (country == "UA") { rect(0,0,1,.5f,blue); rect(0,.5f,1,.5f,yellow); }
    else if (country == "PL") { rect(0,0,1,.5f,white); rect(0,.5f,1,.5f,red); }
    else if (country == "ID") { rect(0,0,1,.5f,red); rect(0,.5f,1,.5f,white); }
    else if (country == "CZ") {
        rect(0,0,1,.5f,white); rect(0,.5f,1,.5f,red);
        const float p[] = {box.x,box.y,box.x+box.w*.48f,box.cy(),box.x,box.y+box.h}; list.polygon(p,3,blue);
    } else if (country == "TH") { horizontal(red,white,red); rect(0,.2f,1,.6f,white); rect(0,.35f,1,.3f,blue); }
    else if (country == "IN") {
        horizontal(Color::rgb(0xef993d),white,green); list.ring(box.cx(),box.cy(),box.h*.13f,box.h*.035f,blue);
    } else if (country == "TR") {
        rect(0,0,1,1,red); list.circle(box.x+box.w*.41f,box.cy(),box.h*.28f,white);
        list.circle(box.x+box.w*.46f,box.cy()-box.h*.03f,box.h*.23f,red);
        list.star(box.x+box.w*.66f,box.cy(),box.h*.13f,white);
    } else if (country == "SA") {
        rect(0,0,1,1,green); rect(.25f,.32f,.5f,.15f,white); rect(.25f,.63f,.5f,.05f,white);
    } else if (country == "IL") {
        rect(0,0,1,1,white); rect(0,.15f,1,.12f,blue); rect(0,.73f,1,.12f,blue);
        const Rect star{box.cx()-box.h*.19f,box.cy()-box.h*.22f,box.h*.38f,box.h*.44f};
        list.triangle(star,blue,box.h*.03f); list.triangle(star,blue,box.h*.03f,kPi);
    } else if (country == "SE" || country == "FI" || country == "DK" || country == "NO") {
        const Color base = country == "SE" ? blue : country == "FI" ? white : red;
        const Color cross = country == "SE" ? yellow : country == "FI" || country == "NO" ? blue : white;
        rect(0,0,1,1,base);
        if (country == "NO") { rect(.27f,0,.22f,1,white); rect(0,.37f,1,.28f,white); }
        rect(.32f,0,.12f,1,cross); rect(0,.43f,1,.16f,cross);
    } else if (country == "GR") {
        rect(0,0,1,1,white); for (int r=0;r<5;++r) rect(0,static_cast<float>(r)*2.f/9,1,1.f/9,blue);
        rect(0,0,.45f,.56f,blue); rect(.16f,0,.1f,.56f,white); rect(0,.22f,.45f,.11f,white);
    } else {
        list.rounded_rect(box, 4, Color::rgb(0x706783,.3f));
        icon(c, {box.cx()-box.h*.43f,box.y+box.h*.07f,box.h*.86f,box.h*.86f},15,white.with_alpha(.8f));
    }
    list.pop_clip();
    list.bordered_rect(box,3,white.with_alpha(0),1,white.with_alpha(.22f));
}

void hint_row(Canvas& canvas, const ui::Theme& theme, std::initializer_list<ui::Hint> hints,
              float y = kFooter, float left = 96.0f, float max_width = 1728.0f,
              bool frosted = false) {
    auto glyphs = ui::GlyphStyle::dark();
    glyphs.ink = theme.text;
    glyphs.label = theme.text_muted;
    ui::HintLayout layout;
    layout.cy = y;
    layout.size = 30;
    layout.text_size = 22;
    layout.item_gap = 32;
    std::vector<ui::Hint> visible(hints);
    // Translations and long provider names must never push hints off the TV.
    while (!visible.empty() &&
           ui::measure_hints(canvas.fonts, visible.data(), static_cast<int>(visible.size()), layout) > max_width)
        visible.pop_back();
    if (frosted && !visible.empty()) {
        const float width = ui::measure_hints(canvas.fonts, visible.data(), static_cast<int>(visible.size()), layout);
        ui::draw_overlay_panel(canvas, theme, {left - 14, y - 25, width + 28, 50}, true, .58f, 16);
    }
    ui::draw_hints(canvas.list, canvas.fonts, glyphs, visible.data(),
                   static_cast<int>(visible.size()), left, false, layout);
}

Btn direction_button(Direction direction) {
    if (direction == Direction::down) return Btn::Down;
    if (direction == Direction::left) return Btn::Left;
    if (direction == Direction::right) return Btn::Right;
    return Btn::Up;
}
void forward_input(App& app, const InputFrame& input) {
    if (input.nav != Direction::none) app.on_button(direction_button(input.nav));
    constexpr std::array<std::pair<Action, Btn>, 12> buttons{{
        {Action::confirm, Btn::Cross}, {Action::back, Btn::Circle},
        {Action::west, Btn::Square}, {Action::north, Btn::Triangle},
        {Action::menu, Btn::Options}, {Action::touch, Btn::Touchpad},
        {Action::page_prev, Btn::L1}, {Action::page_next, Btn::R1},
        {Action::jump_prev, Btn::L2}, {Action::jump_next, Btn::R2},
        {Action::l3, Btn::L3}, {Action::r3, Btn::R3}
    }};
    for (const auto& entry : buttons)
        if (input.is_pressed(entry.first)) app.on_button(entry.second);
}

bool confirmation(const App& app) {
    if (app.dd_options.empty() || app.dd_options.size() > 3) return false;
    for (const auto& item : app.dd_options) {
        std::string label = item.label;
        for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (label.find("remove") != std::string::npos ||
            label.find("delete") != std::string::npos ||
            label.find("rimuov") != std::string::npos ||
            label.find("elimina") != std::string::npos) return true;
    }
    return false;
}

std::string stamp(const std::vector<ui::ListItem>& items) {
    std::string value;
    for (const auto& item : items) {
        value += item.title; value += '\x1e';
        value += item.subtitle; value += '\x1e';
        value += item.badge; value += '\x1e';
        value += item.value; value += '\x1e';
        value += item.disabled ? '1' : '0';
        value += std::to_string(item.tag);
    }
    return value;
}
void sync_list(ui::ListView& list, std::string& previous, std::vector<ui::ListItem> items) {
    std::string next = stamp(items);
    if (next != previous || items.size() != list.items().size()) {
        previous = std::move(next);
        list.set_items(std::move(items));
    }
}

struct AudioScope {
    Feedback& feedback;
    std::size_t begin;
    float gain;
    ~AudioScope() {
        for (std::size_t i = begin; i < feedback.cues.size(); ++i) feedback.cues[i].gain *= gain;
    }
};
} // namespace

struct HomeUi::Impl {
    TextureLookup textures;
    TextureSizeLookup texture_sizes;
    const App* model = nullptr;
    ui::Theme theme = stremio_theme(false);
    ui::FontRef subtitle_font, subtitle_serif, subtitle_mono;
    ui::TabBar nav;
    ui::PushButton search;
    std::vector<ui::Carousel> shelves;
    std::vector<std::string> shelf_keys;
    std::string focused_shelf_key;
    int focused_shelf_index = 0;
    bool startup_was_visible = false;
    bool search_shelves = false;
    ui::GridView grid;
    ui::GridView addon_grid;
    ui::TabBar filters;
    ui::Form settings;
    ui::TabBar season;
    ui::Carousel episodes;
    ui::TabBar sources;
    ui::ListView streams, choices, audio, subtitles, downloads, directories;
    ui::Carousel download_wheel;
    ui::TabBar download_seasons;
    ui::Panel download_panel;
    ui::Transition download_panel_transition;
    ui::Breadcrumb directory_path;
    std::unordered_map<std::string, ui::ProgressBar> download_progress;
    ui::TabBar track_tabs;
    ui::Slider subtitle_delay;
    ui::MediaControls media;
    ui::ProgressBar work, launch_work, directory_move;
    ui::Spinner spinner, launch_spinner, playback_spinner, directory_spinner;
    ui::EmptyState empty;
    ui::Sheet dropdown, keyboard_sheet, tracks, source_info;
    ui::TextView source_text;
    ui::Dialog dialog, directory_notice;
    ui::PushButton login_retry, next_play, next_ignore;
    ui::Keyboard keyboard;
    ui::TextField input;
    ui::LoadingScreen loader;
    ui::ToastStack toasts;
    std::array<ui::StatTile, 3> torrent_stats;
    ui::ProgressBar next_countdown;
    Feedback pending;

    std::string last_view, grid_view, settings_schema, last_toast, last_banner;
    std::uint64_t last_toast_revision = 0;
    std::string episodes_stamp, streams_stamp, choices_stamp, audio_stamp, subs_stamp, dropdown_stamp, downloads_stamp;
    std::string directories_stamp, directory_path_stamp;
    std::string download_groups_stamp, download_seasons_stamp, download_open_id, download_open_image, download_open_title;
    int download_open_index = 0;
    std::vector<std::string> download_episode_ids;
    std::vector<UiDownload> download_closing_rows;
    std::string last_language, sub_source, sub_text;
    std::string hero_name, hero_image, hero_logo, hero_desc, hero_meta;
    std::string old_hero_name, old_hero_image, old_hero_logo, old_hero_desc, old_hero_meta;
    hui::tween::Spring row_scroll, page_enter, watch_alpha;
    hui::tween::Spring download_position, download_lift;
    float hero_mix = 1, time = 0, delay_focus = 0, next_enter = 0;
    bool last_input = false, last_dropdown = false, last_launch = false;
    bool use_dialog = false;
    float shelf_top = 506;
    bool last_contrast = false;

    explicit Impl(TextureLookup lookup) : textures(std::move(lookup)) {
        page_enter.snap(1);
        watch_alpha.snap(1);
        nav.style.kind = ui::TabKind::underline;
        nav.style.width = ui::TabWidth::fit;
        nav.style.height = 60;
        nav.style.gap = 22;
        nav.style.padding = 22;
        nav.style.text_size = 25;
        nav.style.track = false;
        nav.style.on_page = true;
        nav.style.focus_ring = false;
        nav.style.glyph_width = 0;
        nav.style.glyph_gap = 0;
        nav.set_focused(false);
        nav.style.thickness = 3;
        nav.set_bounds({180, 60, 1110, 70});

        search.style.role = ui::ButtonRole::secondary;
        search.style.justify = ui::ButtonJustify::between;
        search.style.icon_size = 26;
        search.style.gap = 16;
        search.style.text_size = 22;
        search.style.padding = 20;
        search.style.rumble = 0;
        search.glyph = ui::Button::triangle;
        search.set_bounds({1180, 183, 644, 64});
        search.icon = [](Canvas& canvas, const Rect& box, Color ink, float) {
            icon(canvas, box, 5, ink);
        };

        grid.style.card.art_aspect = 2.0f / 3.0f;
        grid.style.card.title_size = 23;
        grid.style.card.text = ui::CardText::none;
        grid.style.card.subtitle_size = 0;
        grid.style.card.radius = 14;
        grid.style.card.glow = true;
        grid.style.card.focus_scale = 1.045f;
        grid.style.card.lift = 5;
        grid.style.padding = 20;
        grid.style.gap_x = 24;
        grid.style.gap_y = 30;
        grid.style.exits.left = grid.style.exits.up = true;
        grid.content = [this](Canvas& c, const Rect& box, const ui::CardItem&, int index, float focus) {
            if (!model) return;
            const auto& cards = grid_view == "library" ? model->lib_cards : model->disc_cards;
            if (index < 0 || index >= static_cast<int>(cards.size())) return;
            paint_card(c, box, cards[static_cast<std::size_t>(index)], focus, grid.style);
        };

        addon_grid.style.columns = kAddonCols;
        addon_grid.style.cell_height = 244;
        addon_grid.style.gap_x = 24;
        addon_grid.style.gap_y = 24;
        addon_grid.style.padding = 16;
        addon_grid.style.card.text = ui::CardText::none;
        addon_grid.style.card.art_aspect = 0;
        addon_grid.style.card.plate = true;
        addon_grid.style.card.focus_scale = 1.025f;
        addon_grid.style.card.radius = 20;
        addon_grid.style.exits.left = addon_grid.style.exits.up = true;
        addon_grid.set_bounds({kLeft - 16, 318, kWidth + 16, 654});
        addon_grid.content = [this](Canvas& c, const Rect& box, const ui::CardItem&, int index, float focus) {
            paint_addon(c, box, index, focus);
        };

        style_list(downloads, 142, 27);
        downloads.style.gap = 10;
        downloads.style.panel = false;
        downloads.style.cards = false;
        downloads.style.focus_shift = 0;
        downloads.style.padding = 0;
        downloads.style.highlight.radius = 18;
        downloads.set_bounds({650, 358, 1136, 562});
        downloads.content = [this](Canvas& c, const Rect& box, const ui::ListItem&, int index, float focus) {
            paint_download(c, box, index, focus);
        };
        download_wheel.style.mode = ui::CarouselMode::centered;
        download_wheel.style.item_width = 320;
        download_wheel.style.item_height = 480;
        download_wheel.style.card.art_aspect = 2.0f / 3.0f;
        download_wheel.style.card.radius = 16;
        download_wheel.style.card.text = ui::CardText::none;
        download_wheel.style.counter = download_wheel.style.dots = false;
        download_wheel.set_bounds({kLeft, 254, kWidth, 500});
        download_seasons.style = season.style;
        download_seasons.style.kind = ui::TabKind::underline;
        download_seasons.style.width = ui::TabWidth::fit;
        download_seasons.style.text_size = 26;
        download_seasons.style.padding = 20;
        download_seasons.style.track = false;
        download_seasons.set_bounds({658, 275, 1114, 64});
        download_panel.style.radius = 28;
        download_panel.set_bounds({620, 250, 1204, 700});
        download_panel_transition.style.duration = .32f;
        download_panel_transition.style.distance = 44;
        download_panel_transition.hide();
        download_position.snap(0); download_lift.snap(0);

        style_list(directories, 76, 27);
        directories.style.panel = false;
        directories.style.cards = true;
        directories.style.padding = 20;
        directories.style.leading_width = 60;
        directories.style.focus_shift = 0;
        directories.set_bounds({118, 386, 800, 544});
        directories.leading = [this](Canvas& c, const Rect& box, const ui::ListItem& item, int, float) {
            const float x = box.x + 4, y = box.cy();
            const auto ink = item.tag == 1 ? theme.accent : theme.text_muted;
            c.list.rounded_rect({x, y - 14, 18, 10}, 3, ink);
            c.list.rounded_rect({x, y - 9, 40, 26}, 4, ink);
            if (item.tag == 1) {
                c.list.line(x + 20, y + 10, x + 20, y - 3, 2.5f, theme.surface);
                c.list.line(x + 14, y + 2, x + 20, y - 4, 2.5f, theme.surface);
                c.list.line(x + 26, y + 2, x + 20, y - 4, 2.5f, theme.surface);
            }
        };
        directory_path.set_bounds({1026, 349, 756, 42});
        directory_path.style.text_size = 26;
        directory_path.style.separator = ui::CrumbSeparator::slash;
        directory_path.style.max_segments = 6;
        directory_spinner.style.kind = ui::SpinnerKind::arc;
        directory_spinner.set_bounds({1748, 204, 30, 30});
        directory_move.set_bounds({1026, 751, 756, 9});
        directory_move.style.height = 9;
        directory_move.style.placement = ui::LabelPlacement::none;
        directory_move.style.radius_source = ui::RadiusSource::pill;
        directory_move.style.finish_flash = false;

        filters.style.kind = ui::TabKind::boxed;
        filters.style.height = 54;
        filters.style.gap = 16;
        filters.style.text_size = 23;
        filters.style.on_page = true;
        filters.style.padding = 22;
        filters.set_bounds({kLeft, 252, kWidth, 58});

        settings.style.row_height = 76;
        settings.style.label_size = 25;
        settings.style.value_size = 23;
        settings.style.description_size = 21;
        settings.style.description_inline = false;
        settings.style.highlight.kind = ui::HighlightKind::ring;
        settings.style.highlight.color = Color::rgb(0xa58bf4);
        settings.style.panel = true;
        settings.style.on_page = false;
        settings.style.label_ratio = .58f;
        settings.style.control_width = 250;
        settings.style.number_width = 116;
        settings.style.toggle_width = 70;
        settings.style.padding = 24;
        settings.set_bounds({572, 262, 1252, 692});

        season.style.kind = ui::TabKind::underline;
        season.style.width = ui::TabWidth::fit;
        season.style.height = 56;
        season.style.text_size = 26;
        season.style.padding = 18;
        season.style.gap = 26;
        season.style.track = false;
        season.style.focus_ring = false;
        season.style.on_page = true;
        season.style.wrap = false;
        season.set_focused(false);
        season.set_bounds({kLeft - 18, 504, kWidth + 18, 64});

        episodes.style.mode = ui::CarouselMode::leading;
        episodes.style.item_width = 372;
        episodes.style.item_height = 302;
        episodes.style.gap = 30;
        episodes.style.peek = 94;
        episodes.style.stop_at_end = true;
        episodes.style.edge_fade = .68f;
        episodes.style.card.art_aspect = 16.0f / 9.0f;
        episodes.style.card.text = ui::CardText::below;
        episodes.style.card.title_size = 25;
        episodes.style.card.subtitle_size = 23;
        episodes.style.card.text_gap = 18;
        episodes.style.card.radius = 14;
        episodes.style.card.focus_scale = 1.045f;
        episodes.style.card.lift = 6;
        episodes.style.card.glow = true;
        episodes.style.counter = false;
        episodes.style.dots = false;
        episodes.style.exits.left = episodes.style.exits.right = true;
        episodes.style.entrance_step = .025f;
        episodes.set_bounds({kLeft + 12, 610, kWidth - 24, 330});
        episodes.art = [this](Canvas& c, const Rect& art, float radius, const ui::CardItem& item, float focus) {
            const int index = item.tag;
            if (!model || index < 0 || index >= static_cast<int>(model->d_episodes.size())) return;
            const auto& episode = model->d_episodes[static_cast<std::size_t>(index)];
            if (texture(episode.thumb)) picture(c, episode.thumb, art, radius);
            else {
                c.list.gradient_rect(art, radius, theme.surface_high, theme.surface);
                icon(c, {art.cx() - 24, art.cy() - 24, 48, 48}, 18, theme.accent);
            }
            if (episode.watched) {
                const Rect check{art.x + art.w - 48, art.y + 14, 34, 34};
                c.list.rounded_rect(check, 10, theme.page.with_alpha(.85f));
                icon(c, check.inset(7), 13, theme.success);
            }
            const std::string number_label = episode.number.empty() ? std::string() : episode.number + ". ";
            // The kit owns the card and its gliding artwork ring. Keep the
            // two-line episode caption below the art, fading words on peeking
            // edge cards while their neighbouring thumbnails remain visible.
            const Rect shelf = episodes.bounds();
            const float share = std::clamp((std::min(art.x + art.w, shelf.x + shelf.w) - std::max(art.x, shelf.x)) / art.w, 0.0f, 1.0f);
            c.list.push_opacity(hui::tween::smoothstep((share - .6f) / .35f));
            ui::paragraph(c.list, c.fonts.semibold, number_label + episode.title,
                          art.x, art.y + art.h + 37, 25, art.w, 32,
                          focus > .4f ? theme.text : theme.text_muted, 2);
            c.list.pop_opacity();
        };

        sources.style.kind = ui::TabKind::pill;
        sources.style.width = ui::TabWidth::fit;
        sources.style.height = 52;
        sources.style.text_size = 23;
        sources.style.padding = 22;
        sources.style.track = false;
        sources.style.focus_ring = false;
        sources.style.on_page = true;
        sources.set_focused(false);

        style_list(streams, 154, 24);
        streams.style.panel = true;
        streams.style.padding = 22;
        streams.style.subtitle_size = 22;
        streams.content = [this](Canvas& c, const Rect& box, const ui::ListItem& item, int index, float focus) {
            paint_stream(c, box, item, index, focus);
        };
        style_list(choices, 76, 27);
        choices.style.panel = false;
        choices.content = [this](Canvas& c, const Rect& box, const ui::ListItem& item, int index, float focus) {
            paint_choice(c, box, item, index, focus);
        };
        style_list(audio, 78, 24);
        style_list(subtitles, 78, 24);
        audio.style.leading_width = subtitles.style.leading_width = 62;
        audio.leading = [this](Canvas& c, const Rect& box, const ui::ListItem&, int index, float) {
            if (model && index >= 0 && index < static_cast<int>(model->m_audio.size()))
                language_flag(c, {box.x, box.cy() - 13, 38, 26}, model->m_audio[index].lang);
        };
        subtitles.leading = [this](Canvas& c, const Rect& box, const ui::ListItem&, int index, float) {
            if (model && index >= 0 && index < static_cast<int>(model->m_subs.size()))
                language_flag(c, {box.x, box.cy() - 13, 38, 26}, model->m_subs[index].lang);
        };

        track_tabs.style.kind = ui::TabKind::segmented;
        track_tabs.style.width = ui::TabWidth::fill;
        track_tabs.style.height = 56;
        track_tabs.style.text_size = 25;
        track_tabs.style.track = true;
        track_tabs.style.glyph_width = 26;
        track_tabs.glyph = [](Canvas& c, const Rect& box, const ui::TabItem&, int index, float, Color ink) {
            icon(c, box, index == 0 ? 6 : 7, ink);
        };
        track_tabs.set_focused(false);

        subtitle_delay.style.bubble = ui::BubbleMode::never;
        subtitle_delay.style.value_in_label = true;
        subtitle_delay.style.on_page = false;
        subtitle_delay.style.label_size = 23;
        subtitle_delay.set_range(-20, 20, .25f);
        subtitle_delay.format = [](float value) {
            char formatted[48];
            std::snprintf(formatted, sizeof(formatted), "%+.2f s", static_cast<double>(value));
            return std::string(formatted);
        };

        media.style.layout = ui::MediaLayout::compact;
        media.style.show_art = false;
        media.style.show_shuffle = false;
        media.style.show_repeat = false;
        media.style.show_skip = false;
        media.style.show_tracks = false;
        media.style.show_volume = false;
        media.style.panel = false;
        media.style.padding = 30;
        media.style.title_size = 30;
        media.style.artist_size = 23;
        media.style.play_size = 68;
        media.style.track_height = 8;
        media.style.seek_step = 10;
        media.style.seek_growth = 1;
        media.style.seek_max = 10;
        media.set_bounds({108, 812, 1704, 152});

        work.style.mode = ui::ProgressMode::indeterminate;
        work.style.placement = ui::LabelPlacement::none;
        work.style.height = 4;
        work.style.radius_source = ui::RadiusSource::pill;
        work.style.finish_flash = false;
        work.set_bounds({1160, 130, 450, 4});
        launch_work.style.mode = ui::ProgressMode::indeterminate;
        launch_work.style.placement = ui::LabelPlacement::none;
        launch_work.style.height = 5;
        launch_work.style.radius_source = ui::RadiusSource::pill;
        launch_work.style.finish_flash = false;
        launch_work.style.sheen = true;
        spinner.style.kind = ui::SpinnerKind::arc;
        spinner.set_bounds({1638, 79, 32, 32});
        launch_spinner.style.kind = ui::SpinnerKind::arc;
        launch_spinner.set_bounds({1052, 698, 30, 30});
        playback_spinner.style.kind = ui::SpinnerKind::arc;
        playback_spinner.style.thickness = 5;
        playback_spinner.set_bounds({926, 452, 68, 68});

        empty.style.title_size = 34;
        empty.style.body_size = 26;
        empty.style.hint_size = 24;
        empty.style.max_text_width = 760;
        empty.style.max_lines = 4;
        empty.style.panel = false;
        empty.set_bounds({kLeft + 240, 398, 1120, 440});
        empty.icon = [this](Canvas& c, const Rect& box) {
            icon(c, box.inset(16), model && model->view == "downloads" ? 9 : 2, theme.accent);
        };

        dropdown.style.edge = ui::SheetEdge::right;
        dropdown.style.size = 800;
        dropdown.style.margin = 48;
        dropdown.style.footer = 64;
        dropdown.style.padding = 38;
        dropdown.style.title_size = 34;
        dropdown.style.handle = false;
        dropdown.content = [this](Canvas& c, const Rect&, float) { choices.draw(c); };

        login_retry.style.role = ui::ButtonRole::primary;
        login_retry.style.rumble = 0;
        login_retry.style.text_size = 25;
        login_retry.glyph = ui::Button::cross;
        login_retry.set_active(true);
        login_retry.set_bounds({710, 910, 500, 66});

        keyboard_sheet.style.edge = ui::SheetEdge::bottom;
        keyboard_sheet.style.size = 860;
        keyboard_sheet.style.margin = 64;
        keyboard_sheet.style.footer = 32;
        keyboard_sheet.style.padding = 36;
        keyboard_sheet.style.handle = false;
        keyboard_sheet.content = [this](Canvas& c, const Rect&, float) {
            input.draw(c);
            keyboard.draw(c);
        };
        keyboard.style.bindings = ui::KeyboardBindings::standard();
        keyboard.style.bindings.done = Action::menu;
        keyboard.style.max_length = 8192;
        keyboard.style.panel = false;
        keyboard.style.highlight.kind = ui::HighlightKind::ring;
        keyboard.style.gap = 12;
        keyboard.style.label_size = 28;
        keyboard.style.wide_label_size = 23;
        keyboard.on_text = [this](std::string_view value) { input.insert(value); };
        keyboard.on_backspace = [this]() { input.backspace(); };
        input.style.max_length = 8192;
        input.style.counter = false;
        input.style.field_height = 66;
        input.style.helper_size = 23;
        input.style.helper_gap = 14;
        input.set_active(false);

        tracks.style.edge = ui::SheetEdge::right;
        tracks.style.size = 984;
        tracks.style.margin = 48;
        tracks.style.footer = 64;
        tracks.style.padding = 36;
        tracks.style.title_size = 34;
        tracks.style.handle = false;
        tracks.content = [this](Canvas& c, const Rect& area, float) { paint_tracks(c, area); };

        source_info.style.edge = ui::SheetEdge::right;
        source_info.style.size = 1100;
        source_info.style.margin = 48;
        source_info.style.footer = 64;
        source_info.style.padding = 40;
        source_info.style.title_size = 34;
        source_info.style.handle = false;
        source_info.content = [this](Canvas& c, const Rect&, float) { source_text.draw(c); };
        source_text.style.panel = false;
        source_text.style.padding = 24;
        source_text.style.body_size = 26;
        source_text.style.subheading_size = 30;
        source_text.style.footer = false;
        source_text.style.toc = false;

        dialog.style.width = 820;
        dialog.style.title_size = 38;
        dialog.style.body_size = 27;
        directory_notice.style.width = 820;
        directory_notice.style.title_size = 38;
        directory_notice.style.body_size = 27;

        next_play.set_bounds({994, 838, 384, 66});
        next_ignore.set_bounds({1396, 838, 396, 66});
        next_play.style.role = ui::ButtonRole::primary;
        next_ignore.style.role = ui::ButtonRole::secondary;
        next_play.style.text_size = next_ignore.style.text_size = 26;
        next_play.style.on_page = next_ignore.style.on_page = false;
        next_play.style.rumble = next_ignore.style.rumble = .18f;
        next_countdown.set_bounds({994, 925, 798, 6});
        next_countdown.style.height = 6;
        next_countdown.style.mode = ui::ProgressMode::determinate;
        next_countdown.style.placement = ui::LabelPlacement::none;
        next_countdown.style.radius_source = ui::RadiusSource::pill;
        next_countdown.style.finish_flash = false;

        loader.style.layout = ui::LoadingLayout::corner;
        loader.style.indicator = ui::LoadingIndicator::none;
        loader.style.title_size = 66;
        loader.style.subtitle_size = 27;
        loader.style.tip_dots = false;
        loader.style.tip_label.clear();
        loader.style.percent = false;
        loader.style.veil = .35f;
        loader.style.tips_by_hand = false;
        loader.style.close_on_continue = false;
        loader.art = [this](Canvas& c, const Rect& box, float) {
            if (model) picture(c, model->launch_image, box, 0);
        };

        toasts.style.anchor = ui::ToastAnchor::bottom_right;
        toasts.style.width = 530;
        toasts.style.margin = 80;
        toasts.style.title_size = 24;
        toasts.style.body_size = 22;
        toasts.style.body_lines = 2;
        toasts.style.max_visible = 2;
        toasts.style.frosted = false;
        toasts.set_bounds({0, 0, 1920, 960});

        for (std::size_t i = 0; i < torrent_stats.size(); ++i) {
            auto& stat = torrent_stats[i];
            stat.style.panel = false;
            stat.style.sparkline = false;
            stat.style.show_delta = false;
            stat.style.value_size = 34;
            stat.style.label_size = 21;
            stat.set_bounds({1020 + static_cast<float>(i) * 274, 818, 256, 146});
        }
        apply_theme(false, false);
    }

    bool italian() const { return !model || model->ui_language != "en"; }
    const char* tr(const char* it, const char* en) const { return italian() ? it : en; }
    std::string translated(const std::string& value) const {
        if (!italian()) return value;
        static const std::pair<const char*, const char*> labels[] = {
            {"Continue watching", "Continua a guardare"}, {"Continue Watching", "Continua a guardare"},
            {"Popular movies", "Film popolari"}, {"Popular series", "Serie popolari"},
            {"Featured", "In evidenza"}, {"All types", "Tutti i tipi"}, {"All genres", "Tutti i generi"},
            {"Last watched", "Visti di recente"}, {"Most watched", "Più guardati"},
            {"Not signed in", "Accesso non effettuato"}, {"Stremio account", "Account Stremio"},
            {"Play torrents with", "Riproduci torrent con"}, {"This app", "Questa app"},
            {"Streaming server", "Server di streaming"}, {"Not set", "Non impostato"},
            {"Subtitle languages", "Lingue dei sottotitoli"}, {"Audio languages", "Lingue audio"},
            {"Subtitles on automatically", "Sottotitoli automatici"}, {"Subtitle size", "Dimensione sottotitoli"},
            {"Play next episode automatically", "Riproduci il prossimo episodio"},
            {"Add addon by URL", "Aggiungi add-on da URL"}, {"Reload addons and catalogs", "Ricarica add-on e cataloghi"},
            {"Exit Stremio", "Esci da Stremio"}, {"Small", "Piccoli"}, {"Medium", "Medi"}, {"Large", "Grandi"},
            {"Default", "Predefinita"}, {"None", "Nessuno"}, {"On", "Attivo"}, {"Off", "Disattivato"},
            {"Movie", "Film"}, {"Series", "Serie"}, {"Movies", "Film"}, {"Keep", "Mantieni"},
            {"Remove addon", "Rimuovi add-on"}, {"Search", "Cerca"}, {"Loading...", "Caricamento..."},
            {"Searching...", "Ricerca in corso..."}, {"No streams found", "Nessuna fonte trovata"},
            {"Loading streams...", "Ricerca delle fonti..."}, {"No results", "Nessun risultato"},
            {"Cancel", "Annulla"}, {"Open", "Apri"}, {"Audio", "Audio"}, {"Subtitles", "Sottotitoli"},
            {"The sign-in code expired. Request a new link.", "Il codice di accesso è scaduto. Richiedi un nuovo link."}
        };
        for (const auto& label : labels) if (value == label.first) return label.second;
        if (value.starts_with("Could not get a code: ")) return "Impossibile ottenere un codice: " + value.substr(std::string_view("Could not get a code: ").size());
        if (value.starts_with("Season ")) return "Stagione " + value.substr(7);
        return value;
    }
    std::uint32_t texture(const std::string& path) const {
        return textures && !path.empty() ? textures(path) : 0;
    }
    std::string provider_text(const std::string& value) const {
        std::string source = plain_text(value);
        // Torrentio's streamInfo.js emits these markers with these meanings.
        // Keep the App/SDK data untouched and preserve every accompanying value.
        const std::pair<const char*, const char*> markers[] = {
            {"👤", tr("Peer:", "Seeders:")}, {"💾", tr("Dimensione:", "Size:")},
            {"⚙️", "Provider:"}, {"⚙", "Provider:"}
        };
        for (const auto& marker : markers) {
            std::size_t at = 0;
            const std::string from(marker.first), to(marker.second);
            while ((at = source.find(from, at)) != std::string::npos) {
                source.replace(at, from.size(), to);
                at += to.size();
            }
        }
        // A regional-indicator pair is a flag. Its ISO country code carries
        // the same information in the atlas without guessing an audio language.
        std::string result;
        result.reserve(source.size());
        for (std::size_t at = 0; at < source.size();) {
            const auto begin = at;
            const auto cp = gfx::next_codepoint(source, &at);
            if (cp == 0xfe0e || cp == 0xfe0f) continue; // invisible style selectors
            if (cp >= 0x1f1e6 && cp <= 0x1f1ff && at < source.size()) {
                auto next = at;
                const auto second = gfx::next_codepoint(source, &next);
                if (second >= 0x1f1e6 && second <= 0x1f1ff) {
                    result += '[';
                    result += static_cast<char>('A' + cp - 0x1f1e6);
                    result += static_cast<char>('A' + second - 0x1f1e6);
                    result += ']';
                    at = next;
                    continue;
                }
            }
            result.append(source, begin, at - begin);
        }
        return result;
    }
    void picture(Canvas& c, const std::string& path, const Rect& box, float radius,
                 float opacity = 1.0f) const {
        const auto image = texture(path);
        if (image) c.list.image(image, box, gfx::kFullUv, Color::rgb(0xffffff, opacity), radius);
    }
    bool paint_launch_logo(Canvas& c, const App& app) const {
        const auto image = texture(app.launch_logo);
        if (!image || !texture_sizes) return false;
        const auto [width, height] = texture_sizes(app.launch_logo);
        if (width <= 0 || height <= 0) return false;
        // Keep every launch logo within the same left-aligned, aspect-correct box.
        constexpr float max_width = kWidth * .25f, desired_height = 243, center_y = 540;
        const float scale = std::min(max_width / width, desired_height / height);
        const float shown_height = height * scale;
        c.list.image(image, {kLeft, center_y - shown_height * .5f, width * scale, shown_height},
                     gfx::kFullUv, Color::rgb(0xffffff), 0);
        return true;
    }
    void hero_picture(Canvas& c, const std::string& path, float opacity) const {
        const auto image = texture(path);
        if (!image || opacity <= 0) return;
        constexpr Rect source{640, 112, 1280, 720};
        constexpr float visible_height = 440;
        constexpr int bands = 32;
        constexpr float band_height = visible_height / bands;
        // Fade the image's alpha to zero at its actual top and bottom, instead
        // of clipping a still-visible image through a longer cover gradient.
        // The original UV mapping keeps the selected backdrop's composition.
        const auto alpha = [opacity](float y) {
            const float top = hui::tween::smoothstep(std::clamp(y / 128.0f, 0.0f, 1.0f));
            const float bottom = hui::tween::smoothstep(std::clamp((visible_height - y) / 232.0f, 0.0f, 1.0f));
            return opacity * top * bottom;
        };
        for (int band = 0; band < bands; ++band) {
            const float y = band * band_height;
            c.list.image_gradient(image, {source.x, source.y + y, source.w, band_height},
                                  {0, y / source.h, 1, band_height / source.h},
                                  Color::rgb(0xffffff, alpha(y)),
                                  Color::rgb(0xffffff, alpha(y + band_height)));
        }
    }
    void style_list(ui::ListView& list, float height, float size) {
        list.style.row_height = height;
        list.style.title_size = size;
        list.style.subtitle_size = 21;
        list.style.gap = 8;
        list.style.highlight.kind = ui::HighlightKind::ring;
        list.style.highlight.color = Color::rgb(0xa58bf4);
        list.style.highlight.radius = 14;
        list.style.highlight.thickness = 4;
        list.style.focus_shift = 3;
        list.style.entrance_step = .018f;
    }
    template <class T> void component_theme(T& component, bool reduced) {
        component.style.theme = theme;
        component.style.reduced_motion = reduced;
    }
    void apply_theme(bool contrast, bool reduced) {
        theme = stremio_theme(contrast);
        if (reduced) theme.omega = 60;
        settings.style.highlight.color = theme.accent;
        streams.style.highlight.color = theme.accent;
        downloads.style.highlight.color = theme.accent;
        directories.style.highlight.color = theme.accent;
        choices.style.highlight.color = audio.style.highlight.color = subtitles.style.highlight.color = theme.accent;
        component_theme(nav, reduced);
        component_theme(search, reduced);
        component_theme(grid, reduced);
        component_theme(addon_grid, reduced);
        component_theme(filters, reduced);
        component_theme(settings, reduced);
        component_theme(season, reduced);
        component_theme(sources, reduced);
        component_theme(episodes, reduced);
        component_theme(streams, reduced);
        component_theme(downloads, reduced);
        component_theme(download_wheel, reduced);
        component_theme(download_seasons, reduced);
        component_theme(download_panel, reduced);
        component_theme(download_panel_transition, reduced);
        component_theme(directories, reduced);
        component_theme(directory_path, reduced);
        component_theme(directory_spinner, reduced);
        component_theme(directory_move, reduced);
        component_theme(choices, reduced);
        component_theme(audio, reduced);
        component_theme(subtitles, reduced);
        component_theme(track_tabs, reduced);
        component_theme(subtitle_delay, reduced);
        component_theme(media, reduced);
        component_theme(work, reduced);
        component_theme(launch_work, reduced);
        component_theme(spinner, reduced);
        component_theme(launch_spinner, reduced);
        component_theme(playback_spinner, reduced);
        component_theme(empty, reduced);
        component_theme(dropdown, reduced);
        component_theme(login_retry, reduced);
        component_theme(keyboard_sheet, reduced);
        component_theme(keyboard, reduced);
        component_theme(input, reduced);
        component_theme(tracks, reduced);
        component_theme(source_info, reduced);
        component_theme(source_text, reduced);
        component_theme(dialog, reduced);
        component_theme(directory_notice, reduced);
        component_theme(next_play, reduced);
        component_theme(next_ignore, reduced);
        component_theme(next_countdown, reduced);
        component_theme(loader, reduced);
        component_theme(toasts, reduced);
        for (auto& item : shelves) component_theme(item, reduced);
        for (auto& item : torrent_stats) component_theme(item, reduced);
        for (auto& [id, item] : download_progress) component_theme(item, reduced);
        settings.style.on_text = tr("Sì", "On");
        settings.style.off_text = tr("No", "Off");
        keyboard.style.space_label = tr("Spazio", "Space");
        keyboard.style.done_label = tr("Fine", "Done");
    }

    void paint_art(Canvas& c, const Rect& box, float radius, const UiCard& card) const {
        const auto image = texture(card.image);
        if (image) {
            c.list.image(image, box, gfx::kFullUv, Color::rgb(0xffffff), radius);
        } else {
            c.list.gradient_rect(box, radius, theme.surface_high, theme.surface);
            const std::string initials = first_letter(card.initials.empty() ? card.title : card.initials);
            text(c, c.fonts.display, initials, box.cx(), box.cy() + 18, 52,
                 theme.accent.with_alpha(.65f), box.w - 24, gfx::Align::center);
        }
        if (card.in_library) {
            const Rect mark{box.x + box.w - 44, box.y + 10, 34, 38};
            c.list.rounded_rect(mark, 10, theme.page.with_alpha(.82f));
            icon(c, mark.inset(7), 14, theme.accent);
        }
    }
    void paint_card(Canvas& c, const Rect& box, const UiCard& card, float focus,
                    const ui::GridStyle& style) const {
        ui::CardItem item;
        item.title = card.title;
        item.badge = card.badge;
        item.image_aspect = 2.0f / 3.0f;
        const float progress = fraction(card.progress);
        item.progress = progress > .001f ? progress : -1;
        ui::CardState state;
        state.focus = focus;
        state.emphasis = 0;
        state.marks = false;
        ui::draw_card(c, style, style.card, box, item, state,
            [this, &card](Canvas& canvas, const Rect& art, float radius,
                          const ui::CardItem&, float) { paint_art(canvas, art, radius, card); });
    }
    void paint_addon(Canvas& c, const Rect& box, int index, float focus) const {
        if (!model || index < 0 || index >= static_cast<int>(model->addon_rows.size())) return;
        const auto& addon = model->addon_rows[static_cast<std::size_t>(index)];
        ui::Painter paint(c.list, c.fonts, theme, c.glass);
        paint.panel(box);
        const Rect art{box.x + 24, box.y + 22, 56, 56};
        const auto image = texture(addon.logo);
        if (image) c.list.image(image, art, gfx::kFullUv, Color::rgb(0xffffff), 10);
        else {
            c.list.rounded_rect(art, 12, theme.primary.with_alpha(.22f));
            text(c, c.fonts.semibold, first_letter(addon.initials.empty() ? addon.name : addon.initials), art.cx(), art.cy() + 10, 30,
                 theme.accent, 48, gfx::Align::center);
        }
        text(c, c.fonts.semibold, addon.name, box.x + 96, box.y + 49, 25, theme.text,
             box.w - 118);
        text(c, c.fonts.regular, addon.version, box.x + 96, box.y + 76, 21,
             theme.text_muted, box.w - 118);
        ui::paragraph(c.list, c.fonts.regular, plain_text(addon.desc),
                      box.x + 24, box.y + 113, 22, box.w - 48, 29, theme.text_muted, 3);
        const std::string source = addon.local ? tr("AGGIUNTO QUI", "ADDED HERE")
                                               : tr("ACCOUNT / PREDEFINITO", "ACCOUNT / DEFAULT");
        text(c, c.fonts.semibold, source, box.x + 24, box.y + box.h - 20, 17,
             focus > .2f ? theme.accent : theme.text_muted, box.w - 48);
    }
    float badge(Canvas& c, float x, float y, const std::string& label, Color ink,
                int glyph = -1, float max_width = 300) const {
        const float icon_width = glyph < 0 ? 0 : 26;
        const float width = std::min(max_width, c.fonts.semibold.measure(label, 20) + 24 + icon_width);
        c.list.rounded_rect({x, y, width, 32}, 8, ink.with_alpha(.13f));
        if (glyph >= 0) icon(c, {x + 8, y + 6, 20, 20}, glyph, ink);
        text(c, c.fonts.semibold, label, x + 12 + icon_width, y + 23, 20, ink, width - 24 - icon_width);
        return width;
    }
    float rating_badge(Canvas& c, const std::string& rating, float x, float y) const {
        if (rating.empty()) return 0;
        const Color imdb = Color::rgb(0xf5c518);
        c.list.rounded_rect({x, y - 22, 58, 26}, 5, imdb);
        text(c, c.fonts.semibold, "IMDb", x + 29, y - 3, 18, Color::rgb(0x171717), 51, gfx::Align::center);
        icon(c, {x + 68, y - 20, 22, 22}, 12, imdb);
        text(c, c.fonts.semibold, rating, x + 99, y, 23, theme.text, 78);
        return 106 + c.fonts.semibold.measure(rating, 23);
    }
    void paint_metadata(Canvas& c, const std::string& year, const std::string& runtime,
                        const std::string& rating, const std::string& genres,
                        float x, float y, float width, float size = 24) const {
        const float origin = x;
        if (!rating.empty()) x += rating_badge(c, rating, x, y) + 26;
        if (!year.empty()) {
            text(c, c.fonts.semibold, year, x, y, size, theme.text_muted, width - (x-origin));
            x += c.fonts.semibold.measure(year, size) + 26;
        }
        if (!runtime.empty() && x < origin + width - 130) {
            icon(c, {x, y - 21, 23, 23}, 17, theme.text_muted);
            x += 34;
            text(c, c.fonts.semibold, runtime, x, y, size, theme.text_muted, width - (x-origin));
            x += c.fonts.semibold.measure(runtime, size) + 26;
        }
        if (!genres.empty() && x < origin + width - 160)
            text(c, c.fonts.regular, genres, x, y, size, theme.text_muted, width - (x-origin));
    }
    void paint_choice(Canvas& c, const Rect& box, const ui::ListItem& item, int index, float focus) const {
        float left = box.x + 24 + focus * 3;
        if (model && index >= 0 && index < static_cast<int>(model->dd_options.size())) {
            const auto& option = model->dd_options[static_cast<std::size_t>(index)];
            if (!option.lang.empty()) {
                language_flag(c, {left, box.cy() - 14, 42, 28}, option.lang);
                left += 62;
            }
            if (model->dd_multiselect) {
                const Rect check{box.x + box.w - 58, box.cy() - 17, 34, 34};
                c.list.bordered_rect(check, 8, option.active ? theme.primary : theme.surface.with_alpha(.2f),
                                     2, option.active ? theme.accent : theme.outline);
                if (option.active) icon(c, check.inset(6), 13, theme.on_primary);
            } else if (option.active) {
                icon(c, {box.x + box.w - 56, box.cy() - 13, 26, 26}, 13, theme.accent);
            }
        }
        text(c, c.fonts.semibold, item.title, left, box.cy() + 9, 26, theme.text,
             box.x + box.w - 82 - left);
    }
    void paint_stream(Canvas& c, const Rect& box, const ui::ListItem& item, int index, float focus) const {
        if (!model || index < 0 || index >= static_cast<int>(model->d_streams.size())) return;
        const auto& stream = model->d_streams[static_cast<std::size_t>(index)];
        const float left = box.x + 20 + focus * 3;
        const float right = box.x + box.w - 30;
        if (model->d_pick_res) {
            icon(c, {left, box.cy() - 18, 36, 36}, 19, theme.accent);
            text(c, c.fonts.semibold, provider_text(stream.name), left + 58, box.y + 43, 27, theme.text, right - left - 90);
            text(c, c.fonts.regular, provider_text(stream.desc.empty() ? stream.addon : stream.desc),
                 left + 58, box.y + 77, 23, theme.text_muted, right - left - 90);
            c.list.triangle({right - 8, box.cy() - 8, 12, 16}, theme.accent, 0, kPi*.5f);
            return;
        }
        const bool torrent = stream.type == "torrent";
        const int type_icon = torrent ? 9 : stream.type == "usenet" ? 23 : stream.type == "archive" ? 22
            : stream.type == "debrid" ? 10 : stream.type == "external" ? 11 : stream.type == "unavailable" ? 24 : 18;
        const Color type_color = stream.type == "unavailable" ? theme.warning
            : stream.cached ? theme.success : torrent ? theme.accent : theme.text_muted;
        c.list.rounded_rect({left, box.y + 14, 42, 42}, 12, type_color.with_alpha(.13f));
        icon(c, {left + 9, box.y + 23, 24, 24}, type_icon, type_color);
        const std::string title = provider_text(item.title.empty() ? stream.addon : item.title);
        text(c, c.fonts.semibold, title, left + 60, box.y + 42, 26, theme.text, right - left - 92);
        const std::string type_label = torrent ? "Torrent" : stream.type == "debrid" ? "Debrid"
            : stream.type == "external" ? tr("Link esterno", "External link")
            : stream.type == "youtube" ? "YouTube" : stream.type == "usenet" ? "Usenet"
            : stream.type == "archive" ? tr("Archivio", "Archive")
            : stream.type == "unavailable" ? tr("Non disponibile", "Unavailable")
            : tr("Streaming diretto", "Direct stream");
        std::string source = type_label;
        if (stream.cached) source += tr(" · Disponibile in cache", " · Cached");
        if (!stream.addon.empty()) source += " · " + stream.addon;
        if (!stream.desc.empty()) source += " · " + provider_text(stream.desc);
        text(c, c.fonts.regular, source, left, box.y + 78, 22, theme.text_muted, right - left - 18);
        float x = left;
        const float y = box.y + 101;
        // Flags and seed counts never share a clipped release-name line.
        // Unknown counts remain explicit; a missing value is not zero.
        const std::string seed_count = stream.seeders >= 0 ? std::to_string(stream.seeders) : "—";
        const std::string seed_label = seed_count + " seeders";
        float metadata_room = 0;
        if (stream.seeders >= 0 || torrent)
            metadata_room += std::min(220.0f, c.fonts.semibold.measure(seed_label, 20) + 50) + 12;
        if (!stream.quality.empty()) metadata_room += c.fonts.semibold.measure(stream.quality, 20) + 36;
        if (!stream.size.empty()) metadata_room += c.fonts.semibold.measure(stream.size, 20) + 36;
        const float flag_room = right - left - metadata_room;
        int max_flags = std::clamp(static_cast<int>(flag_room / 90), 1, 12);
        if (max_flags < static_cast<int>(stream.languages.size()))
            max_flags = std::max(1, std::min(max_flags, static_cast<int>((flag_room - 48) / 90)));
        const int flags = std::min(max_flags, static_cast<int>(stream.languages.size()));
        for (int i = 0; i < flags; ++i) {
            const auto& language = stream.languages[static_cast<std::size_t>(i)];
            language_flag(c, {x, y + 4, 34, 24}, language);
            std::string code = language;
            for (char& ch : code) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            text(c, c.fonts.semibold, code, x + 42, y + 24, 20, theme.text, 44);
            x += 90;
        }
        if (flags < static_cast<int>(stream.languages.size())) {
            const std::string more = "+" + std::to_string(stream.languages.size() - flags);
            text(c, c.fonts.semibold, more, x, y + 23, 20, theme.text_muted, 54); x += 48;
        }
        if (flags == 0 && !model->d_pick_res) {
            icon(c, {x, y + 4, 22, 22}, 15, theme.text_muted);
            text(c, c.fonts.regular, tr("Lingua n/d", "Language n/a"), x + 31, y + 23, 19, theme.text_muted, 126);
            x += 159;
        }
        if (stream.seeders >= 0 || torrent) {
            x += badge(c, x, y, seed_label, stream.seeders > 0 ? theme.success : theme.text_muted, 8, 220) + 12;
        }
        if (!stream.quality.empty() && x < right - 64)
            x += badge(c, x, y, stream.quality, theme.accent, -1, right-x) + 12;
        if (!stream.size.empty() && x < right - 78)
            badge(c, x, y, stream.size, theme.text_muted, -1, right-x);
        c.list.triangle({right - 8, box.y + 28, 12, 16}, theme.accent.with_alpha(.5f + .5f*focus), 0, kPi*.5f);
    }

    void paint_download(Canvas& c, const Rect& box, int index, float focus) {
        if (!model || index < 0) return;
        const UiDownload* selected = nullptr;
        if (model->download_series_id.empty() && download_panel_transition.leaving()) {
            if (index < int(download_closing_rows.size())) selected = &download_closing_rows[static_cast<size_t>(index)];
        } else if (index < int(model->download_episode_rows.size())) {
            const int row = model->download_episode_rows[static_cast<size_t>(index)];
            if (row >= 0 && row < int(model->download_rows.size())) selected = &model->download_rows[static_cast<size_t>(row)];
        }
        if (!selected) return;
        const auto& item = *selected;
        const Rect poster{box.x + 18, box.y + 17, 192, 108};
        if (texture(item.thumbnail)) picture(c, item.thumbnail, poster, 10);
        else {
            c.list.gradient_rect(poster, 10, theme.primary.with_alpha(.3f), theme.surface);
            icon(c, {poster.cx() - 21, poster.cy() - 21, 42, 42}, 18, theme.accent.with_alpha(.65f));
        }
        const float left = box.x + 232, right = box.x + box.w - 24;
        const float room = right - left;
        const std::string number = item.episode >= 0 ? std::to_string(item.episode) + ". " : "";
        const std::string title = item.episode_title.empty() ? tr("Episodio", "Episode") : plain_text(item.episode_title);
        text(c, c.fonts.semibold, number + title, left, item.complete ? poster.cy() + 9 : box.y + 39,
             26, theme.text, item.complete ? room : room - 48);
        if (item.complete) return;
        const Color state_color = item.failed ? theme.danger : item.paused ? theme.warning : theme.accent;
        std::string state = item.status;
        if (state.empty()) state = item.failed ? tr("Download interrotto", "Download interrupted")
            : item.paused ? tr("In pausa", "Paused")
            : item.active ? tr("Download in corso", "Downloading") : tr("In coda", "Queued");
        if (item.playable_while_downloading)
            state += tr(" · Pronto da guardare", " · Ready to watch");
        text(c, c.fonts.regular, translated(plain_text(state)), left, box.y + 72,
             21, state_color, room - 95);
        if (item.progress >= 0) {
            const int percent = static_cast<int>(std::clamp(item.progress, 0.0f, 1.0f) * 100);
            text(c, c.fonts.semibold, std::to_string(percent) + "%", right, box.y + 72,
                 23, state_color, 88, gfx::Align::right);
        }
        const auto progress = download_progress.find(item.id);
        if (progress != download_progress.end()) {
            progress->second.set_bounds({left, box.y + 88, room, 6});
            progress->second.draw(c);
        }
        std::string transfer = item.size;
        if (!item.speed.empty()) transfer += (transfer.empty() ? "" : " · ") + item.speed;
        if (!item.remaining.empty()) transfer += (transfer.empty() ? "" : " · ") + item.remaining;
        text(c, c.fonts.regular, transfer, left, box.y + 123,
             20, theme.text_muted, room);
        const Rect state_icon{right - 30, box.y + 15, 28, 28};
        if (item.active)
            c.list.arc(state_icon.cx(), state_icon.cy(), 11, 3,
                       model->ui_reduced_motion ? 0 : time * 2, kPi * 1.4f, state_color);
        else icon(c, state_icon, item.failed ? 24 : 9,
                  state_color.with_alpha(.65f + .35f * focus));
    }
    void sync_downloads(const App& app, float dt) {
        std::string groups_signature;
        for (const auto& group : app.download_groups) groups_signature += std::to_string(group.id.size()) + ':' + group.id;
        const bool order_changed = groups_signature != download_groups_stamp;
        if (order_changed) {
            std::vector<ui::CardItem> cards;
            cards.reserve(app.download_groups.size());
            for (const auto& group : app.download_groups) {
                ui::CardItem item;
                item.title = group.title;
                cards.push_back(std::move(item));
            }
            download_wheel.set_items(std::move(cards));
            download_groups_stamp = std::move(groups_signature);
            // A sort changes indices, not the selected media. Keep that cover
            // centered instead of animating through unrelated new positions.
            download_position.snap(static_cast<float>(app.download_group_sel));
            if (!download_open_id.empty() || download_panel_transition.visible())
                download_open_index = app.download_group_sel;
        }
        download_wheel.set_focus(app.download_group_sel, order_changed);
        download_wheel.set_active(app.download_series_id.empty() && !modal(app));
        download_position.target = static_cast<float>(app.download_group_sel);
        download_position.update(dt, app.ui_reduced_motion ? 60.0f : 13.0f);
        if (download_open_id != app.download_series_id) {
            const bool opening = !app.download_series_id.empty();
            if (opening && app.download_group_sel >= 0 && app.download_group_sel < int(app.download_groups.size())) {
                const auto& group = app.download_groups[static_cast<size_t>(app.download_group_sel)];
                download_open_index = app.download_group_sel;
                download_open_image = group.image; download_open_title = group.title;
                download_closing_rows.clear();
                download_panel_transition.start(ui::TransitionKind::slide, Direction::left);
                downloads.enter();
            } else {
                download_closing_rows.clear();
                for (const auto& id : download_episode_ids) {
                    const auto row = std::find_if(app.download_rows.begin(), app.download_rows.end(),
                        [&](const UiDownload& item) { return item.id == id; });
                    if (row != app.download_rows.end()) download_closing_rows.push_back(*row);
                }
                download_panel_transition.leave(ui::TransitionKind::fade);
            }
            download_open_id = app.download_series_id;
        }
        download_lift.target = app.download_series_id.empty() ? 0.0f : 1.0f;
        download_lift.update(dt, app.ui_reduced_motion ? 60.0f : 12.0f);

        const bool closing = app.download_series_id.empty() && download_panel_transition.visible();
        if (!closing) {
            download_closing_rows.clear();
            std::string signature = app.download_series_id + ':' + std::to_string(app.download_season);
            for (const int index : app.download_episode_rows) {
                const auto& item = app.download_rows[static_cast<size_t>(index)];
                signature += std::to_string(item.id.size()) + ':' + item.id;
            }
            if (signature != downloads_stamp || downloads.items().size() != app.download_episode_rows.size()) {
                std::vector<ui::ListItem> rows;
                rows.reserve(app.download_episode_rows.size());
                download_episode_ids.clear();
                for (const int index : app.download_episode_rows) {
                    const auto& item = app.download_rows[static_cast<size_t>(index)];
                    ui::ListItem row;
                    row.title = item.episode_title;
                    rows.push_back(std::move(row));
                    download_episode_ids.push_back(item.id);
                }
                downloads.set_items(std::move(rows));
                downloads_stamp = std::move(signature);
            }
            const auto selected = std::find(app.download_episode_rows.begin(), app.download_episode_rows.end(), app.download_sel);
            downloads.set_focus(selected == app.download_episode_rows.end() ? 0 : int(selected - app.download_episode_rows.begin()), false);
        }
        downloads.set_active(!app.download_series_id.empty() && app.zone == "content" && !modal(app));
        std::string seasons_signature = app.download_series_id + app.ui_language;
        const auto group = std::find_if(app.download_groups.begin(), app.download_groups.end(),
            [&](const UiDownloadGroup& item) { return item.id == app.download_series_id; });
        if (group != app.download_groups.end()) {
            for (const int value : group->seasons) seasons_signature += ':' + std::to_string(value);
            if (seasons_signature != download_seasons_stamp) {
                std::vector<ui::TabItem> tabs;
                for (const int value : group->seasons) {
                    ui::TabItem tab;
                    tab.label = value == 0 ? tr("Speciali", "Specials") : value < 0 ? tr("Altri episodi", "Other episodes")
                        : std::string(tr("Stagione ", "Season ")) + std::to_string(value);
                    tabs.push_back(std::move(tab));
                }
                download_seasons.set_tabs(std::move(tabs));
                download_seasons_stamp = std::move(seasons_signature);
            }
            const auto season = std::find(group->seasons.begin(), group->seasons.end(), app.download_season);
            download_seasons.set_active(season == group->seasons.end() ? 0 : int(season - group->seasons.begin()), false);
            download_seasons.set_focused(false);
        }
        for (auto it = download_progress.begin(); it != download_progress.end();) {
            if (std::none_of(app.download_rows.begin(), app.download_rows.end(),
                             [&](const UiDownload& row) { return row.id == it->first && !row.complete; }))
                it = download_progress.erase(it);
            else ++it;
        }
        for (const auto& item : app.download_rows) {
            if (item.complete) continue;
            auto [it, inserted] = download_progress.try_emplace(item.id);
            auto& bar = it->second;
            component_theme(bar, app.ui_reduced_motion);
            bar.style.height = 6;
            bar.style.radius_source = ui::RadiusSource::pill;
            bar.style.track = ui::TrackStyle::flat;
            bar.style.placement = ui::LabelPlacement::none;
            bar.style.finish_flash = false;
            bar.style.sheen = item.active;
            bar.style.mode = item.progress < 0 && !item.paused && !item.failed
                ? ui::ProgressMode::indeterminate : ui::ProgressMode::determinate;
            bar.style.status = item.failed ? ui::Status::danger
                : item.paused ? ui::Status::warning : ui::Status::accent;
            bar.set_value(std::max(item.progress, 0.0f), inserted);
            bar.update(dt);
        }
    }

    void sync_shelves(const App& app, bool is_search) {
        const auto& rows = is_search ? app.search_rows : app.home_rows;
        const float top = is_search ? 330.0f : 506.0f;
        const bool changed_page = search_shelves != is_search;
        std::vector<std::string> keys;
        keys.reserve(rows.size());
        for (const auto& source : rows) keys.push_back(source.dom_id);
        const int row = bounded(is_search ? app.search_row : app.home_row, rows.size());
        const std::string focus_key = row >= 0 && row < static_cast<int>(keys.size()) ? keys[row] : std::string();
        if (changed_page) {
            shelves.clear(); shelf_keys.clear(); focused_shelf_key.clear();
            row_scroll.snap(static_cast<float>(row) * kRowPitch);
        } else if (!focus_key.empty() && focused_shelf_key == focus_key && row != focused_shelf_index) {
            // Preserve the actual viewport, including a navigation animation
            // in progress, when a late catalog or account row inserts above it.
            const float shift = static_cast<float>(row - focused_shelf_index) * kRowPitch;
            row_scroll.value += shift;
            row_scroll.target += shift;
        }
        if (changed_page || keys != shelf_keys) {
            std::vector<ui::Carousel> retained;
            retained.reserve(rows.size());
            for (std::size_t r = 0; r < rows.size(); ++r) {
                const auto found = std::find(shelf_keys.begin(), shelf_keys.end(), keys[r]);
                if (found != shelf_keys.end()) {
                    retained.push_back(std::move(shelves[static_cast<std::size_t>(found - shelf_keys.begin())]));
                    continue;
                }
                retained.emplace_back();
                auto& shelf = retained.back();
                component_theme(shelf, app.ui_reduced_motion);
                shelf.style.mode = ui::CarouselMode::leading;
                shelf.style.item_width = 212;
                shelf.style.gap = 24;
                shelf.style.peek = 88;
                shelf.style.stop_at_end = true;
                shelf.style.edge_fade = .75f;
                shelf.style.title_size = 30;
                shelf.style.title_gap = 44;
                shelf.style.card.art_aspect = 2.0f / 3.0f;
                shelf.style.card.text = ui::CardText::none;
                shelf.style.card.radius = 14;
                shelf.style.card.focus_scale = 1.045f;
                shelf.style.card.lift = 5;
                shelf.style.card.glow = true;
                shelf.style.counter = false;
                shelf.style.dots = false;
                shelf.style.exits.left = shelf.style.exits.right = true;
                shelf.style.entrance_step = .018f;
                // Home rows are revealed together after staging. A late row
                // must not replay entrances in already visible catalogs.
                if (is_search) shelf.enter();
            }
            shelves = std::move(retained);
            shelf_keys = std::move(keys);
        }
        search_shelves = is_search;
        focused_shelf_key = focus_key;
        focused_shelf_index = row;
        shelf_top = top;
        const int col = is_search ? app.search_col : app.home_col;
        row_scroll.target = static_cast<float>(row) * kRowPitch;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            auto& shelf = shelves[r];
            const auto& source = rows[r];
            const Rect bounds{kLeft + 12, top + static_cast<float>(r) * kRowPitch, kWidth - 24, 424};
            const auto& old = shelf.bounds();
            if (old.x != bounds.x || old.y != bounds.y || old.w != bounds.w || old.h != bounds.h)
                shelf.set_bounds(bounds, false);
            // A retained carousel can move to a different vector index.
            // Rebind its artwork to the new row instead of capturing old r.
            shelf.art = [this, r](Canvas& c, const Rect& box, float radius,
                                 const ui::CardItem& item, float) {
                if (!model) return;
                const auto& source = search_shelves ? model->search_rows : model->home_rows;
                if (r >= source.size() || item.tag < 0 || item.tag >= static_cast<int>(source[r].cards.size())) return;
                paint_art(c, box, radius, source[r].cards[static_cast<std::size_t>(item.tag)]);
            };
            shelf.title = translated(source.title);
            const int selected = bounded(col, source.cards.size());
            const bool changed = shelf.items().size() != source.cards.size();
            // Keep the complete catalog in Carousel. Its own scroll leaves a
            // glimpse of adjacent artwork and stops at the true final card.
            if (changed) {
                std::vector<ui::CardItem> items(source.cards.size());
                for (std::size_t i = 0; i < items.size(); ++i) items[i].tag = static_cast<int>(i);
                shelf.set_items(std::move(items));
                if (is_search) shelf.enter();
            }
            for (std::size_t i = 0; i < source.cards.size(); ++i) {
                auto& item = shelf.item(static_cast<int>(i));
                const auto& card = source.cards[i];
                item.title.clear();
                item.badge = card.badge;
                const float progress = fraction(card.progress);
                item.progress = progress > .001f ? progress : -1;
            }
            if (static_cast<int>(r) == row) shelf.set_focus(selected, changed);
            shelf.set_active(static_cast<int>(r) == row && app.zone == "content" && !modal(app));
        }
    }

    bool modal(const App& app) const {
        return app.home_startup_visible() || app.download_directory_notice || app.directory_picker_visible || app.dd_visible || app.login_visible || app.input_visible_ || app.launch_visible || app.watching() ||
               source_info.is_open();
    }
    void sync_settings(const App& app) {
        std::string schema = app.ui_language;
        for (const auto& source : app.s_rows) schema += source.id + "/" + source.kind + ";";
        if (schema != settings_schema) {
            settings_schema = std::move(schema);
            settings.clear();
            const auto section = [](const std::string& id) {
                if (id == "display_resolution" || id == "display_refresh") return 1;
                if (id == "audio_languages" || id == "audio_passthrough") return 2;
                if (id.rfind("subtitle_", 0) == 0 || id == "auto_subtitles") return 3;
                if (id == "autoplay_next" || id == "next_episode_delay_seconds" || id == "seek_seconds" || id == "shoulder_seek_seconds" || id == "show_stats") return 4;
                if (id == "download_directory" || id == "torrent_speed_profile" || id == "addons" || id == "reload" || id == "server_url") return 5;
                if (id == "reduced_motion" || id == "high_contrast" || id == "controller_ambient_light" || id == "ui_sounds" || id == "sound_volume") return 6;
                if (id == "exit") return 7;
                return 0;
            };
            const std::array<const char*, 8> headers{{tr("Generali", "General"), tr("Video e schermo", "Video & Display"),
                "Audio", tr("Sottotitoli", "Subtitles"), tr("Riproduzione", "Playback"),
                tr("Download e add-on", "Downloads & Add-ons"), tr("Interfaccia", "Interface"),
                tr("Applicazione", "Application")}};
            for (int group = 0; group < int(headers.size()); ++group) {
                bool header = false;
                for (std::size_t i = 0; i < app.s_rows.size(); ++i) {
                const auto& source = app.s_rows[i];
                if (section(source.id) != group) continue;
                if (!header) { settings.add_header(headers[static_cast<size_t>(group)]); header = true; }
                const int index = static_cast<int>(i);
                if (source.kind == "toggle")
                    settings.add_toggle(index, translated(source.label), source.on);
                else if (source.kind == "slider") {
                    auto& row = settings.add_slider(index, translated(source.label),
                        static_cast<float>(source.number), static_cast<float>(source.minimum),
                        static_cast<float>(source.maximum), static_cast<float>(source.step));
                    if (source.id.find("volume") != std::string::npos || source.id == "subtitle_background_opacity") row.unit = " %";
                    else if (source.id == "subtitle_offset_ms") row.unit = " ms";
                    else if (source.id == "seek_seconds" || source.id == "shoulder_seek_seconds") row.unit = " s";
                } else settings.add_action(index, translated(source.label));
                }
            }
            settings.style.header_size = 22;
            settings.enter();
        }
        for (std::size_t i = 0; i < app.s_rows.size(); ++i) {
            const auto& source = app.s_rows[i];
            const int index = static_cast<int>(i);
            auto* row = settings.row(index);
            if (!row) continue;
            row->label = translated(source.label);
            row->description.clear();
            row->disabled = !source.enabled;
            row->text = translated(source.value);
            row->chevron = source.kind == "choice" || source.kind == "text" || source.kind == "action" || source.kind == "checklist";
            row->danger = source.id == "exit" || source.id == "clear_cache";
            if (source.kind == "toggle") settings.set_toggle(index, source.on);
            if (source.kind == "slider") settings.set_slider(index, static_cast<float>(source.number));
        }
        settings.focus_row(app.s_sel, false);
        settings.set_active(app.zone == "content" && !modal(app));
        settings.style.highlight.kind = app.zone == "content" && !modal(app)
                                      ? ui::HighlightKind::ring : ui::HighlightKind::none;
    }

    void sync_directories(const App& app) {
        std::vector<ui::ListItem> rows;
        for (const auto& entry : app.directory_picker_entries) {
            ui::ListItem item;
            item.title = entry == ".." ? tr("Cartella superiore", "Parent folder") : entry;
            item.tag = entry == ".." ? 1 : 0;
            item.chevron = true;
            rows.push_back(std::move(item));
        }
        sync_list(directories, directories_stamp, std::move(rows));
        const bool changed = directory_path_stamp != app.directory_picker_path;
        directories.set_focus(app.directory_picker_sel, changed);
        directories.set_active(!app.directory_picker_loading && !app.directory_picker_committing);
        if (changed) {
            directory_path_stamp = app.directory_picker_path;
            std::vector<std::string> parts{tr("PS5", "PS5")};
            for (const auto& part : split(app.directory_picker_path, '/')) if (!part.empty()) parts.push_back(part);
            directory_path.set_path(parts, !app.ui_reduced_motion);
            directories.enter();
        }
        directory_spinner.set_spinning(app.directory_picker_loading || app.directory_picker_committing, false);
        directory_move.style.mode = app.directory_move_total > 0
            ? ui::ProgressMode::determinate : ui::ProgressMode::indeterminate;
        directory_move.set_value(app.directory_move_total > 0 ? std::clamp(
            float(double(app.directory_move_done) / app.directory_move_total), 0.0f, 1.0f) : 0.0f);
    }

    void sync_detail(const App& app) {
        std::string episode_signature = app.d_name + "/" + app.d_season_label;
        for (const auto& episode : app.d_episodes) episode_signature += "\x1e" + episode.number + "\x1e" + episode.title;
        const bool episode_changed = episode_signature != episodes_stamp || episodes.items().size() != app.d_episodes.size();
        if (episode_changed) {
            episodes_stamp = std::move(episode_signature);
            std::vector<ui::CardItem> ep(app.d_episodes.size());
            for (std::size_t i = 0; i < ep.size(); ++i) {
                ep[i].image_aspect = 16.0f / 9.0f;
                ep[i].tag = static_cast<int>(i);
            }
            episodes.set_items(std::move(ep));
            episodes.enter();
        }
        episodes.set_focus(app.d_episode_sel, episode_changed);
        episodes.set_active(app.d_zone != "streams" && !modal(app));
        bool season_changed = season.tabs().size() != app.d_seasons.size();
        if (!season_changed) for (std::size_t i = 0; i < app.d_seasons.size(); ++i)
            if (season.tabs()[i].label != translated(app.d_seasons[i].value)) season_changed = true;
        if (season_changed) {
            std::vector<ui::TabItem> tabs(app.d_seasons.size());
            for (std::size_t i = 0; i < app.d_seasons.size(); ++i) tabs[i].label = translated(app.d_seasons[i].value);
            season.set_tabs(std::move(tabs));
        }
        season.set_active(app.d_season_sel, false);
        season.set_focused(false);

        streams.style.row_height = app.d_pick_res ? 108.0f : 154.0f;
        std::vector<ui::ListItem> list;
        list.reserve(app.d_streams.size());
        for (const auto& source : app.d_streams) {
            ui::ListItem item;
            item.title = source.name;
            item.subtitle = source.addon;
            if (!source.desc.empty()) {
                if (!item.subtitle.empty()) item.subtitle += "  ·  ";
                item.subtitle += source.desc;
            }
            item.chevron = true;
            list.push_back(std::move(item));
        }
        sync_list(streams, streams_stamp, std::move(list));
        streams.set_focus(app.d_stream_sel, false);
        streams.set_active(app.d_zone == "streams" && !modal(app));
        streams.style.highlight.kind = app.d_zone == "streams" && !modal(app)
                                     ? ui::HighlightKind::ring : ui::HighlightKind::none;
        const Rect stream_rect = app.d_series ? Rect{kLeft, 548, kWidth, 410}
                                              : Rect{1016, 410, 808, 546};
        place(streams, stream_rect);
        place(sources, app.d_series ? Rect{kLeft, 480, kWidth, 54} : Rect{1016, 343, 808, 54});
        bool source_changed = sources.tabs().size() != app.d_sources.size();
        if (!source_changed) for (std::size_t i = 0; i < app.d_sources.size(); ++i)
            if (sources.tabs()[i].label != translated(app.d_sources[i].value)) source_changed = true;
        if (source_changed) {
            std::vector<ui::TabItem> tabs(app.d_sources.size());
            for (std::size_t i = 0; i < app.d_sources.size(); ++i) tabs[i].label = translated(app.d_sources[i].value);
            sources.set_tabs(std::move(tabs));
        }
        sources.set_active(app.d_source_sel, false);
        sources.set_focused(false);
    }

    void sync_overlays(const App& app) {
        next_play.label = tr("Guarda ora", "Watch now");
        next_ignore.label = tr("Ignora", "Ignore");
        next_play.set_active(app.next_episode_visible && app.next_episode_sel == 0);
        next_ignore.set_active(app.next_episode_visible && app.next_episode_sel == 1);
        if (app.next_episode_visible)
            next_countdown.set_value(std::clamp(float(app.next_episode_seconds) /
                std::max(5, app.settings().next_episode_delay_seconds), 0.0f, 1.0f), next_enter == 0);
        if (app.download_directory_notice && !directory_notice.is_open()) {
            ui::DialogContent content;
            content.icon = ui::StatusKind::info;
            content.title = tr("Cartella dei download", "Download folder");
            content.body = tr("Scegli una cartella dei download nelle Impostazioni.",
                              "Choose a download folder in Settings.");
            content.buttons = {{"OK", ui::ButtonKind::primary}};
            directory_notice.open(std::move(content), pending);
        } else if (!app.download_directory_notice && directory_notice.is_open()) {
            directory_notice.close(pending);
        }
        if (source_info.is_open() && (app.view != "detail" || app.watching() || app.launch_visible)) source_info.dismiss();
        if (source_info.is_open()) place(source_text, source_info.content_rect());
        if (app.dd_visible) {
            use_dialog = !app.dd_multiselect && confirmation(app);
            std::vector<ui::ListItem> options;
            for (const auto& source : app.dd_options) {
                ui::ListItem item;
                item.title = translated(source.label);
                if (source.active && !app.dd_multiselect) item.badge = tr("ATTUALE", "CURRENT");
                item.tag = source.active ? 1 : 0;
                options.push_back(std::move(item));
            }
            const std::string signature = app.dd_title + stamp(options);
            if (use_dialog && (!last_dropdown || signature != dropdown_stamp)) {
                ui::DialogContent content;
                content.icon = ui::StatusKind::warning;
                content.title = app.dd_title;
                content.body = tr("Conferma l'operazione selezionata.", "Confirm the selected action.");
                for (const auto& option : options) {
                    ui::DialogButton button;
                    button.label = option.title;
                    button.kind = ui::ButtonKind::secondary;
                    std::string folded = option.title;
                    for (char& c : folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    button.destructive = folded.find("remove") != std::string::npos ||
                                         folded.find("delete") != std::string::npos ||
                                         folded.find("rimuov") != std::string::npos ||
                                         folded.find("elimina") != std::string::npos;
                    content.buttons.push_back(std::move(button));
                }
                content.default_button = app.dd_sel;
                dialog.open(std::move(content), pending);
                dropdown.dismiss();
            } else if (!use_dialog) {
                dropdown.set_title(translated(app.dd_title));
                if (!dropdown.is_open()) dropdown.open(pending);
                dialog.dismiss();
                sync_list(choices, choices_stamp, std::move(options));
                place(choices, dropdown.content_rect());
                choices.set_focus(app.dd_sel, false);
                choices.set_active(true);
            }
            dropdown_stamp = signature;
            if (use_dialog) dialog.set_focus(app.dd_sel);
        } else {
            if (dropdown.is_open()) dropdown.close(pending);
            if (dialog.is_open()) dialog.close(pending);
        }
        last_dropdown = app.dd_visible;

        login_retry.label = tr("Richiedi nuovo link", "Request a new link");
        login_retry.set_loading(app.login_visible && app.login_state == App::LoginState::loading);

        if (app.input_visible_) {
            keyboard_sheet.set_title(translated(app.input_title));
            if (!last_input) {
                keyboard_sheet.open(pending);
                keyboard.set_layout(0);
                keyboard.set_shift(ui::KeyboardShift::off);
                keyboard.set_focus(1, 0);
                keyboard.enter();
            }
            if (input.text() != app.input_value) input.set_text(app.input_value);
            input.set_label(tr("TESTO", "TEXT"));
            input.set_helper(translated(app.input_hint));
            const auto area = keyboard_sheet.content_rect();
            input.set_bounds({area.x + 112, area.y + 10, area.w - 224, 144});
            keyboard.set_bounds({area.x + 180, area.y + 186, area.w - 360, area.h - 204});
            keyboard.set_length(input.length());
        } else if (keyboard_sheet.is_open()) keyboard_sheet.close(pending);
        last_input = app.input_visible_;

        if (app.launch_visible) {
            loader.title.clear();
            loader.subtitle.clear();
            if (!last_launch) loader.show(pending);
            loader.set_progress(-1);
            loader.set_ready(false);
            loader.set_tips({});
        } else if (loader.is_open()) loader.hide();
        last_launch = app.launch_visible;

        if (app.watching() && app.menu_visible) {
            tracks.set_title(tr("Audio e sottotitoli", "Audio and subtitles"));
            if (!tracks.is_open()) tracks.open(pending);
            std::vector<ui::ListItem> audio_items, sub_items;
            for (const auto& source : app.m_audio) {
                ui::ListItem item;
                item.title = source.label;
                if (source.active) item.badge = tr("ATTIVO", "ACTIVE");
                audio_items.push_back(std::move(item));
            }
            for (const auto& source : app.m_subs) {
                ui::ListItem item;
                item.title = translated(source.label);
                if (source.active) item.badge = tr("ATTIVO", "ACTIVE");
                sub_items.push_back(std::move(item));
            }
            sync_list(audio, audio_stamp, std::move(audio_items));
            sync_list(subtitles, subs_stamp, std::move(sub_items));
            const auto area = tracks.content_rect();
            track_tabs.set_bounds({area.x, area.y, area.w, 60});
            place(audio, {area.x, area.y + 86, area.w, area.h - 224});
            place(subtitles, audio.bounds());
            audio.set_focus(app.m_audio_sel, false);
            subtitles.set_focus(app.m_sub_sel, false);
            audio.set_active(app.m_col == 0 && delay_focus <= 0);
            subtitles.set_active(app.m_col == 1 && delay_focus <= 0);
            audio.style.highlight.kind = app.m_col == 0 && delay_focus <= 0
                                       ? ui::HighlightKind::ring : ui::HighlightKind::none;
            subtitles.style.highlight.kind = app.m_col == 1 && delay_focus <= 0
                                           ? ui::HighlightKind::ring : ui::HighlightKind::none;
            track_tabs.set_active(app.m_col);
            subtitle_delay.set_bounds({area.x, area.y + area.h - 111, area.w, 102});
            subtitle_delay.set_label(tr("Ritardo dei sottotitoli", "Subtitle delay"));
            const auto sign = app.m_delay.find_first_of("+-");
            const float delay = sign == std::string::npos ? 0 : number(app.m_delay.substr(sign));
            subtitle_delay.set_value(delay);
            subtitle_delay.set_active(delay_focus > 0);
        } else if (tracks.is_open()) tracks.close(pending);
    }

    void update(const App& app, float dt) {
        model = &app;
        const bool startup_visible = app.home_startup_visible();
        const bool startup_revealed = startup_was_visible && !startup_visible && app.view == "home";
        startup_was_visible = startup_visible;
        dt = std::clamp(std::isfinite(dt) ? dt : 0.0f, 0.0f, .05f);
        time += dt;
        delay_focus = std::max(0.0f, delay_focus - dt);
        const bool language_changed = last_language != app.ui_language;
        if (language_changed || nav.tabs().empty()) {
            std::vector<ui::TabItem> entries(5);
            const std::array<const char*, 5> it{{"Home", "Scopri", "Libreria", "Download", "Impostazioni"}};
            const std::array<const char*, 5> en{{"Home", "Discover", "Library", "Downloads", "Settings"}};
            for (std::size_t i = 0; i < entries.size(); ++i) entries[i].label = italian() ? it[i] : en[i];
            nav.set_tabs(std::move(entries));
            std::vector<ui::TabItem> tabs(2);
            tabs[0].label = "Audio";
            tabs[1].label = tr("Sottotitoli", "Subtitles");
            track_tabs.set_tabs(std::move(tabs));
            last_language = app.ui_language;
        }
        apply_theme(app.ui_high_contrast, app.ui_reduced_motion);
        last_contrast = app.ui_high_contrast;
        const bool changed_view = last_view != app.view;
        if (changed_view) {
            page_enter.snap(0);
            page_enter.target = 1;
            empty.enter();
            grid.enter();
            addon_grid.enter();
            downloads.enter();
            download_wheel.enter();
            if (app.view == "downloads") download_position.snap(static_cast<float>(app.download_group_sel));
            else { download_lift.snap(0); download_panel_transition.hide(); download_open_id.clear(); }
            last_view = app.view;
        }
        nav.set_active(app.nav_sel, false);
        nav.set_focused(false);
        search.label = app.view == "search" && !app.search_query.empty()
                     ? app.search_query : tr("Cerca film e serie", "Search movies and series");
        search.set_active(app.zone == "searchbox" && !modal(app) && app.view == "search");
        spinner.set_spinning(app.busy, false);

        if (app.view == "home" || app.view == "search") sync_shelves(app, app.view == "search");
        if (app.view == "discover" || app.view == "library") {
            const auto& cards = app.view == "library" ? app.lib_cards : app.disc_cards;
            if (grid_view != app.view) {
                grid_view = app.view;
                grid.style.columns = app.view == "library" ? 7 : kDiscCols;
                grid.set_count(0);
                grid.set_bounds(app.view == "library" ? Rect{kLeft - 12, 334, kWidth + 24, 636}
                                                       : Rect{kLeft - 12, 334, 1208, 636});
                grid.enter();
            }
            if (grid.count() != static_cast<int>(cards.size())) grid.set_count(static_cast<int>(cards.size()));
            grid.set_focus(app.view == "library" ? app.lib_sel : app.disc_sel, false);
            grid.set_active(app.zone == "content" && !modal(app));
            const auto& chips = app.view == "library" ? app.lib_chips : app.disc_chips;
            bool changed = filters.tabs().size() != chips.size();
            if (!changed) for (std::size_t i = 0; i < chips.size(); ++i)
                if (filters.tabs()[i].label != translated(chips[i].value)) changed = true;
            if (changed) {
                std::vector<ui::TabItem> tabs(chips.size());
                for (std::size_t i = 0; i < chips.size(); ++i) tabs[i].label = translated(chips[i].value);
                filters.set_tabs(std::move(tabs));
            }
            filters.set_active(app.view == "library" ? app.lib_chip : app.disc_chip);
            filters.set_focused(app.zone == "filters" && !modal(app));
        }
        if (app.view == "addons") {
            if (addon_grid.count() != static_cast<int>(app.addon_rows.size()))
                addon_grid.set_count(static_cast<int>(app.addon_rows.size()));
            addon_grid.set_focus(app.addon_sel, false);
            addon_grid.set_active(app.zone == "content" && !modal(app));
        }
        if (app.view == "settings") sync_settings(app);
        if (app.directory_picker_visible) sync_directories(app);
        if (app.view == "detail") sync_detail(app);
        if (app.view == "downloads") sync_downloads(app, dt);

        const std::string next_name = app.home_preview_name;
        if (next_name != hero_name) {
            old_hero_name = hero_name; old_hero_image = hero_image; old_hero_logo = hero_logo;
            old_hero_desc = hero_desc; old_hero_meta = hero_meta;
            hero_name = next_name; hero_image = app.home_preview_background; hero_logo = app.home_preview_logo;
            hero_desc = plain_text(app.home_preview_description); hero_meta = app.home_preview_meta;
            hero_mix = old_hero_name.empty() ? 1.0f : 0.0f;
        } else {
            hero_image = app.home_preview_background; hero_logo = app.home_preview_logo;
            hero_desc = plain_text(app.home_preview_description); hero_meta = app.home_preview_meta;
        }
        hero_mix = std::min(1.0f, hero_mix + dt / (app.ui_reduced_motion ? .10f : .28f));
        if (startup_revealed) {
            page_enter.snap(1);
            row_scroll.snap(static_cast<float>(app.home_row) * kRowPitch);
            hero_mix = 1;
            for (std::size_t r = 0; r < shelves.size(); ++r)
                if (static_cast<int>(r) == app.home_row) shelves[r].set_focus(app.home_col, true);
        }

        // Downloads use the full viewport, so their empty state is centered at x=960.
        empty.set_bounds(app.view == "downloads" ? Rect{(1920.0f - 1120.0f) * .5f, 398, 1120, 440}
                                                 : Rect{kLeft + 240, 398, 1120, 440});
        empty.action.clear();
        empty.title = tr("La tua prossima storia ti aspetta", "Your next story is waiting");
        empty.body = tr("I cataloghi dei tuoi add-on compariranno qui.", "Your add-on catalogs will appear here.");
        if (app.busy) {
            empty.title = tr("Caricamento dei cataloghi", "Loading catalogs");
            empty.body = tr("Stremio sta contattando i tuoi add-on.", "Stremio is contacting your add-ons.");
        } else if (app.view == "search") {
            empty.title = tr("Risultati della ricerca", "Search results");
            empty.body = translated(app.search_status.empty() ? tr("Nessun risultato disponibile.", "No results available.") : app.search_status);
        } else if (app.view == "library") {
            empty.title = tr("La tua libreria", "Your library");
            empty.body = translated(app.lib_status.empty() ? tr("Accedi al tuo account per ritrovare la tua libreria e le tue liste.",
                         "Sign in to find your library and lists.") : app.lib_status);
        } else if (app.view == "discover") {
            empty.title = tr("Esplora i tuoi cataloghi", "Explore your catalogs");
            empty.body = translated(app.disc_status.empty() ? tr("Cambia i filtri o scegli un altro catalogo.",
                         "Change the filters or choose another catalog.") : app.disc_status);
        } else if (app.view == "addons") {
            empty.title = tr("I tuoi add-on", "Your add-ons");
            empty.body = tr("Qui compaiono gli add-on sincronizzati con il tuo account Stremio.",
                            "Your Stremio account add-ons will appear here.");
        } else if (app.view == "downloads") {
            empty.title = tr("Nessun download", "No downloads");
            empty.body = tr("Seleziona una fonte nella pagina di un film o di un episodio e premi Quadrato per scaricare quel video.",
                            "Select a stream on a movie or episode page and press Square to download that video.");
        }
        sync_overlays(app);

        if (!app.toast.empty() && (app.toast_revision != last_toast_revision || app.toast != last_toast)) {
            const auto message = translated(plain_text(app.toast));
            const bool added = message.starts_with("Aggiunto") || message.starts_with("Added");
            const bool removed = message.starts_with("Rimosso") || message.starts_with("Removed");
            if (added || removed) toasts.push(added ? ui::StatusKind::success : ui::StatusKind::info, message, "", 2.5f);
            else toasts.push(ui::StatusKind::info, "Stremio Plus", message, 5);
        }
        last_toast = app.toast;
        last_toast_revision = app.toast_revision;
        if (!app.banner.empty() && app.banner != last_banner)
            toasts.push(ui::StatusKind::info, "Stremio Plus", translated(plain_text(app.banner)), 8);
        last_banner = app.banner;

        media.title = app.w_title;
        media.artist = app.w_subtitle;
        media.set_duration(seconds(app.w_duration));
        const float seek_limit = partial_seek_limit(app);
        const float position = seconds(app.w_time);
        media.set_position(seek_limit >= 0 ? std::min(position, seek_limit) : position, seek_limit >= 0);
        // This is the safely seekable prefix measured by the player. Keep the
        // full timeline, and clear the partial highlight when the file ends.
        media.set_buffered(seek_limit >= 0 ? seek_limit : 0.0f);
        media.set_playing(!app.w_paused);
        media.set_active(!app.menu_visible);
        media.style.seek_step = static_cast<float>(app.settings().seek_seconds);
        media.style.seek_max = media.style.seek_step;
        watch_alpha.target = app.info_visible ? 1.0f : 0.0f;
        if (app.w_sub_rml != sub_source) {
            sub_source = app.w_sub_rml;
            sub_text = plain_text(sub_source);
        }

        torrent_stats[0].label = tr("Peer connessi", "Connected peers");
        torrent_stats[1].label = tr("Velocità download", "Download speed");
        torrent_stats[2].label = tr("Scaricato", "Downloaded");
        torrent_stats[0].set_value(number(app.t_peers));
        torrent_stats[1].style.format = ui::CounterFormat::decimal;
        torrent_stats[1].style.decimals = 2;
        torrent_stats[1].style.suffix = " MiB/s";
        torrent_stats[1].set_value(number(app.t_speed));
        torrent_stats[2].style.format = ui::CounterFormat::decimal;
        torrent_stats[2].style.decimals = 1;
        torrent_stats[2].style.suffix = " %";
        torrent_stats[2].set_value(number(app.t_progress));
        const Rect opening{1020, app.t_visible ? 646.0f : 798.0f, 804, 152};
        launch_spinner.set_bounds({opening.x + 28, opening.y + 28, 28, 28});
        launch_spinner.set_spinning(app.launch_visible, false);
        playback_spinner.set_spinning(app.w_buffering && app.watching() && !app.launch_visible, false);
        launch_work.set_bounds({opening.x + 28, opening.y + opening.h - 25, opening.w - 56, 5});
        launch_work.style.mode = app.launch_progress >= 0 ? ui::ProgressMode::determinate : ui::ProgressMode::indeterminate;
        launch_work.set_value(std::max(app.launch_progress, 0.0f), !last_launch);

        const float omega = app.ui_reduced_motion ? 60.0f : 16.0f;
        row_scroll.update(dt, omega);
        page_enter.update(dt, omega);
        watch_alpha.update(dt, 14);
        next_enter = app.next_episode_visible ? std::min(1.0f, next_enter + dt / (app.ui_reduced_motion ? .08f : .22f)) : 0;
        next_play.update(dt); next_ignore.update(dt); next_countdown.update(dt);
        nav.update(dt); search.update(dt); filters.update(dt);
        grid.update(dt); addon_grid.update(dt); settings.update(dt); downloads.update(dt);
        download_wheel.update(dt); download_seasons.update(dt); download_panel_transition.update(dt);
        directories.update(dt); directory_path.update(dt); directory_spinner.update(dt); directory_move.update(dt);
        season.update(dt); sources.update(dt); episodes.update(dt); streams.update(dt);
        choices.update(dt); audio.update(dt); subtitles.update(dt);
        track_tabs.update(dt); subtitle_delay.update(dt); media.update(dt);
        work.update(dt); launch_work.update(dt); spinner.update(dt); launch_spinner.update(dt);
        playback_spinner.update(dt); empty.update(dt);
        dropdown.update(dt); login_retry.update(dt);
        keyboard_sheet.update(dt); keyboard.update(dt); input.update(dt);
        tracks.update(dt); dialog.update(dt); directory_notice.update(dt); loader.update(dt);
        source_info.update(dt); source_text.update(dt);
        toasts.update(dt, pending);
        for (auto& shelf : shelves) shelf.update(dt);
        for (auto& stat : torrent_stats) stat.update(dt);
    }

    void handle(App& app, const InputFrame& input_frame, Feedback& feedback) {
        if (model != &app) update(app, 0);
        AudioScope volume{feedback, feedback.cues.size(),
            app.ui_sounds ? std::clamp(static_cast<float>(app.ui_sound_volume) / 100.0f, 0.0f, 1.0f) : 0.0f};
        feedback.cues.insert(feedback.cues.end(), pending.cues.begin(), pending.cues.end());
        if (pending.rumble_strength > 0) feedback.rumble(pending.rumble_strength, pending.rumble_seconds);
        pending.clear();
        app.on_playback_stick(input_frame.stick2_x, input_frame.stick2_y,
                              input_frame.connected && !input_frame.focus_lost && !input_frame.intercepted);
        if (input_frame.focus_lost) return;
        const std::size_t before = feedback.cues.size();

        if (app.home_startup_visible()) return;
        if (app.login_visible) {
            const auto event = login_retry.handle(input_frame, feedback);
            if (event == Event::activated) app.on_button(Btn::Cross);
            if (input_frame.is_pressed(Action::back)) app.on_button(Btn::Circle);
            return;
        }

        if (app.download_directory_notice) {
            sync_overlays(app);
            const auto event = directory_notice.handle(input_frame, feedback);
            if (event == Event::activated || event == Event::cancelled) {
                app.download_directory_notice = false;
                directory_notice.close(feedback);
            }
            return;
        }

        if (app.next_episode_visible) {
            sync_overlays(app);
            if (input_frame.nav == Direction::left || input_frame.nav == Direction::right) {
                app.on_button(direction_button(input_frame.nav));
                feedback.play(hui::audio::Cue::focus);
            } else if (input_frame.is_pressed(Action::back)) {
                app.on_button(Btn::Circle);
                feedback.play(hui::audio::Cue::back);
            } else {
                auto& selected = app.next_episode_sel == 1 ? next_ignore : next_play;
                if (selected.handle(input_frame, feedback) == Event::activated) app.on_button(Btn::Cross);
            }
            return;
        }

        if (app.directory_picker_visible) {
            if (app.directory_picker_committing) return;
            sync_directories(app);
            if (!app.directory_picker_loading && !app.directory_picker_committing) {
                // The component owns focus animation; the App owns directory
                // navigation, asynchronous I/O and the selected destination.
                InputFrame list_input = input_frame;
                list_input.pressed &= hui::action_bit(Action::confirm);
                const auto event = directories.handle(list_input, feedback);
                if (event == Event::moved) app.directory_picker_sel = directories.focus();
                InputFrame actions = input_frame;
                actions.nav = input_frame.nav == Direction::left ? Direction::left : Direction::none;
                forward_input(app, actions);
            } else forward_input(app, input_frame);
            if (feedback.cues.size() == before) {
                if (input_frame.is_pressed(Action::back)) feedback.play(hui::audio::Cue::back);
                else if (input_frame.is_pressed(Action::confirm) || input_frame.is_pressed(Action::north))
                    feedback.play(hui::audio::Cue::select);
                else if (input_frame.is_pressed(Action::page_prev) || input_frame.is_pressed(Action::page_next))
                    feedback.play(hui::audio::Cue::focus);
            }
            return;
        }

        if (source_info.is_open()) {
            if (source_info.handle(input_frame, feedback) != Event::cancelled)
                source_text.handle(input_frame, feedback);
            return;
        }

        if (app.input_visible_) {
            if (!last_input) sync_overlays(app);
            keyboard.set_length(input.length());
            const Event event = keyboard.handle(input_frame, feedback);
            app.replace_input(input.text());
            if (event == Event::activated) app.input_submit();
            else if (event == Event::cancelled) app.cancel_input();
            return;
        }
        if (app.dd_visible) {
            if (!last_dropdown) sync_overlays(app);
            if (use_dialog) {
                const Event event = dialog.handle(input_frame, feedback);
                if (event == Event::moved) app.dd_sel = dialog.focus();
                if (event == Event::activated) {
                    app.dd_sel = dialog.choice();
                    app.on_button(Btn::Cross);
                } else if (event == Event::cancelled) app.on_button(Btn::Circle);
            } else {
                if (dropdown.handle(input_frame, feedback) == Event::cancelled) app.on_button(Btn::Circle);
                else {
                    const Event event = choices.handle(input_frame, feedback);
                    if (event == Event::moved) app.dd_sel = choices.focus();
                    if (event == Event::activated) {
                        app.dd_sel = choices.focus();
                        app.on_button(Btn::Cross);
                    }
                }
            }
            return;
        }
        if (app.launch_visible) {
            if (loader.handle(input_frame, feedback) == Event::cancelled) app.on_button(Btn::Circle);
            return;
        }
        if (app.watching()) {
            if (app.menu_visible) {
                if (input_frame.is_pressed(Action::jump_prev) || input_frame.is_pressed(Action::jump_next)) {
                    InputFrame step;
                    step.nav = input_frame.is_pressed(Action::jump_prev) ? Direction::left : Direction::right;
                    subtitle_delay.handle(step, feedback);
                    delay_focus = 1.4f;
                } else if (input_frame.nav == Direction::left || input_frame.nav == Direction::right ||
                           input_frame.is_pressed(Action::page_prev) || input_frame.is_pressed(Action::page_next)) {
                    const int direction = input_frame.nav == Direction::left || input_frame.is_pressed(Action::page_prev) ? -1 : 1;
                    track_tabs.step(direction, input_frame, feedback);
                    delay_focus = 0;
                } else {
                    delay_focus = 0;
                    (app.m_col == 0 ? audio : subtitles).handle(input_frame, feedback);
                }
            } else {
                InputFrame transport = input_frame;
                // The established player mapping is direct: horizontal means
                // seek, Cross means pause. MediaControls visualises that same
                // action instead of adding another focus traversal layer.
                if (transport.nav == Direction::left || transport.nav == Direction::right) {
                    media.set_focus(ui::MediaControl::scrubber);
                } else {
                    transport.nav = Direction::none;
                    if (transport.is_pressed(Action::confirm)) media.set_focus(ui::MediaControl::play);
                }
                transport.pressed &= hui::action_bit(Action::confirm);
                media.handle(transport, feedback);
                const float seek_limit = partial_seek_limit(app);
                if (seek_limit >= 0)
                    media.set_position(std::min(media.position(), seek_limit), true);
            }
            forward_input(app, input_frame);
            if (feedback.cues.size() == before && input_frame.is_pressed(Action::back))
                feedback.play(hui::audio::Cue::back);
            return;
        }

        // Top sections are display-only. The application routes L1/R1; a
        // directional boundary keeps its current content rather than creating
        // a second, competing navigation layer.
        if (app.zone == "nav") app.zone = "content";

        if (app.view == "detail") {
            if (app.d_zone == "streams" && input_frame.is_pressed(Action::north) && !app.d_streams.empty()) {
                const auto& selected = app.d_streams[static_cast<std::size_t>(bounded(app.d_stream_sel, app.d_streams.size()))];
                source_info.set_title(tr("Dettagli fonte", "Source details"));
                std::vector<ui::TextBlock> blocks;
                blocks.push_back(ui::TextBlock::heading(provider_text(selected.name), 2));
                if (!selected.addon.empty()) blocks.push_back(ui::TextBlock::key_value("Add-on", selected.addon));
                blocks.push_back(ui::TextBlock::divider());
                blocks.push_back(ui::TextBlock::paragraph(provider_text(selected.desc)));
                source_text.set_content(std::move(blocks));
                source_text.scroll_to(0, true);
                source_info.open(feedback);
                place(source_text, source_info.content_rect());
                return;
            }
            if (app.d_zone == "episodes" || app.d_zone == "seasons") episodes.handle(input_frame, feedback);
            else streams.handle(input_frame, feedback);
        } else if (app.zone == "searchbox") search.handle(input_frame, feedback);
        else if (app.zone == "filters") filters.handle(input_frame, feedback);
        else if (app.view == "home" || app.view == "search") {
            const int row = app.view == "home" ? app.home_row : app.search_row;
            if (row >= 0 && row < static_cast<int>(shelves.size()))
                shelves[static_cast<std::size_t>(row)].handle(input_frame, feedback);
        } else if (app.view == "discover" || app.view == "library") grid.handle(input_frame, feedback);
        else if (app.view == "downloads") {
            if (app.download_series_id.empty() &&
                (input_frame.is_pressed(Action::jump_prev) || input_frame.is_pressed(Action::jump_next))) {
                const int previous_sort = app.settings().download_sort;
                app.on_button(input_frame.is_pressed(Action::jump_prev) ? Btn::L2 : Btn::R2);
                if (app.settings().download_sort != previous_sort) feedback.play(hui::audio::Cue::tab);
                return;
            }
            if (!app.download_series_id.empty()) {
                if (input_frame.is_pressed(Action::page_prev) || input_frame.is_pressed(Action::page_next))
                    download_seasons.step(input_frame.is_pressed(Action::page_prev) ? -1 : 1, input_frame, feedback);
                else downloads.handle(input_frame, feedback);
            } else {
                InputFrame wheel_input = input_frame;
                if (wheel_input.nav == Direction::up) wheel_input.nav = Direction::left;
                if (wheel_input.nav == Direction::down) wheel_input.nav = Direction::right;
                download_wheel.handle(wheel_input, feedback);
            }
        }
        else if (app.view == "addons") addon_grid.handle(input_frame, feedback);
        else if (app.view == "settings") {
            if (input_frame.is_pressed(Action::jump_prev) || input_frame.is_pressed(Action::jump_next)) {
                InputFrame page;
                page.nav = input_frame.is_pressed(Action::jump_prev) ? Direction::up : Direction::down;
                page.nav_repeat = true;
                for (int i = 0; i < 5; ++i) settings.handle(page, feedback);
                if (settings.focus_id() >= 0) app.s_sel = settings.focus_id();
                return;
            }
            const Event event = settings.handle(input_frame, feedback);
            if ((event == Event::moved || event == Event::refused) &&
                (input_frame.nav == Direction::up || input_frame.nav == Direction::down)) {
                if (settings.focus_id() >= 0) app.s_sel = settings.focus_id();
                return;
            }
            if (event == Event::refused && input_frame.is_pressed(Action::confirm)) return;
            if (event == Event::changed) {
                const int row = settings.changed_id();
                const auto* changed = settings.row(row);
                if (changed && changed->kind == ui::FormRowKind::toggle)
                    app.set_setting_value(row, settings.toggle_value(row) ? 1.0f : 0.0f);
                else if (changed && changed->kind == ui::FormRowKind::slider)
                    app.set_setting_value(row, settings.slider_value(row));
                return;
            }
            // A switch at its limit owns Left/Right even if it did not change:
            // Left on an already-disabled switch must not unexpectedly leave.
            if ((input_frame.nav == Direction::left || input_frame.nav == Direction::right) &&
                settings.uses_horizontal()) return;
        }
        forward_input(app, input_frame);
        if (app.zone == "nav") app.zone = "content";
        if (app.zone == "searchbox" && app.view != "search" && app.view != "detail") app.zone = "content";
        if (feedback.cues.size() == before) {
            if (input_frame.is_pressed(Action::back)) feedback.play(hui::audio::Cue::back);
            else if (input_frame.is_pressed(Action::page_next) || input_frame.is_pressed(Action::page_prev))
                feedback.play(hui::audio::Cue::tab);
            else if (input_frame.is_pressed(Action::confirm) || input_frame.is_pressed(Action::north))
                feedback.play(hui::audio::Cue::select);
            else if (input_frame.nav != Direction::none && !input_frame.nav_repeat)
                feedback.play(hui::audio::Cue::focus, 1.0f, -.1f, .65f);
        }
    }

    void paint_background(Canvas& c, const App& app) const {
        c.list.rounded_rect({0, 0, 1920, 1080}, 0, theme.page);
        if (app.view == "detail" && !app.d_background.empty()) {
            // Use the sharp artwork supplied by App. Edge fades preserve text
            // contrast without blurring the actual backdrop.
            picture(c, app.d_background, {640, 0, 1280, 720}, 0);
            c.list.gradient_rect_h({640, 0, 940, 760}, 0, theme.page, theme.page.with_alpha(0));
            c.list.gradient_rect({0, 370, 1920, 430}, 0, theme.page.with_alpha(0), theme.page);
        } else if (app.view != "home") {
            c.list.gradient_rect_h({560, 0, 1360, 520}, 0, theme.page,
                                  Color::rgb(app.ui_high_contrast ? 0x080808 : 0x25173e));
            c.list.gradient_rect({0, 230, 1920, 330}, 0, theme.page.with_alpha(0), theme.page);
        }
        c.list.gradient_rect({0, 0, 1920, 170}, 0, theme.page, theme.page.with_alpha(0));
    }
    void paint_header(Canvas& c, const App& app) const {
        const auto mark = texture("assets/icons/logo.png");
        if (mark) c.list.image(mark, {96, 71, 48, 48}, gfx::kFullUv, Color::rgb(0xffffff), 0);
        else {
            c.list.circle(120, 95, 23, theme.primary);
            c.list.triangle({112, 82, 20, 26}, Color::rgb(0xffffff), 0, kPi * .5f);
        }
        if (app.directory_picker_visible)
            text(c, c.fonts.semibold, tr("Impostazioni / Download", "Settings / Downloads"), 196, 105, 26, theme.text, 1040);
        else if (app.view == "addons")
            text(c, c.fonts.semibold, tr("Impostazioni / Add-on", "Settings / Add-ons"), 196, 105, 26, theme.text, 1040);
        else if (app.view != "detail") nav.draw(c);
        if (app.view == "search") search.draw(c);
        text(c, c.fonts.regular, app.clock, kRight, app.date.empty() ? 107.0f : 89.0f,
             27, theme.text, 160, gfx::Align::right);
        text(c, c.fonts.regular, app.date, kRight, 121, 21, theme.text_muted, 550, gfx::Align::right);
        if (app.busy) {
            spinner.draw(c);
            if (app.view != "detail") work.draw(c);
        }
    }
    void paint_hero_version(Canvas& c, const std::string& name, const std::string& logo, const std::string& description,
                            const std::string& meta, float alpha, float dx) const {
        if (name.empty() || alpha < .001f) return;
        c.list.push_opacity(alpha);
        c.list.push_transform(1, 0, 0, dx, 0);
        if (texture(logo)) picture(c, logo, {kLeft, 196, 616, 132}, 0);
        else ui::paragraph(c.list, c.fonts.semibold, name, kLeft, 259, 64, 850, 70, theme.text, 2);
        const auto rating_start = meta.find("IMDb ");
        if (rating_start != std::string::npos) {
            const auto end = meta.find("  ·  ", rating_start);
            const std::string value = meta.substr(rating_start + 5, end == std::string::npos ? end : end-rating_start-5);
            const float badge_width = rating_badge(c, value, kLeft, 376);
            std::string other = meta.substr(0, rating_start);
            while (!other.empty() && (other.back() == ' ' || other.back() == char(0xb7) || other.back() == char(0xc2))) other.pop_back();
            if (end != std::string::npos) other += meta.substr(end);
            text(c, c.fonts.semibold, other, kLeft + badge_width + 26, 376, 23, theme.text_muted, 760 - badge_width);
        } else text(c, c.fonts.semibold, meta, kLeft, 376, 23, theme.text_muted, 800);
        if (!description.empty())
            ui::paragraph(c.list, c.fonts.regular, description, kLeft, 417, 25, 740, 33,
                          theme.text.with_alpha(.84f), 2);
        c.list.pop_transform();
        c.list.pop_opacity();
    }
    void paint_hero(Canvas& c, const App& app) const {
        const float blend = hui::tween::smoothstep(hero_mix);
        if (blend < 1 && !old_hero_image.empty()) hero_picture(c, old_hero_image, 1 - blend);
        hero_picture(c, hero_image, blend);
        // Side fades meet the page colour exactly at the artwork's boundaries.
        // The broad left scrim also keeps the title and synopsis readable.
        c.list.gradient_rect_h({640, 112, 940, 440}, 0, theme.page, theme.page.with_alpha(0));
        c.list.gradient_rect_h({1712, 112, 208, 440}, 0, theme.page.with_alpha(0), theme.page);
        if (hero_name.empty()) {
            ui::paragraph(c.list, c.fonts.semibold,
                          tr("Le storie che ami.\nTutte al loro posto.", "The stories you love.\nAll in one place."),
                          kLeft, 286, 66, 1080, 74, theme.text, 2);
            text(c, c.fonts.regular,
                 app.busy ? tr("I tuoi cataloghi stanno arrivando.", "Your catalogs are on their way.")
                          : tr("Scegli un titolo per vedere dettagli, episodi e fonti.",
                               "Choose a title to see details, episodes and sources."),
                 kLeft, 424, 26, theme.text_muted, 1060);
        } else {
            paint_hero_version(c, old_hero_name, old_hero_logo, old_hero_desc, old_hero_meta, 1 - blend, -18 * blend);
            paint_hero_version(c, hero_name, hero_logo, hero_desc, hero_meta, blend,
                               app.ui_reduced_motion ? 0 : 18 * (1 - blend));
        }
    }
    void paint_shelves(Canvas& c, const App& app) const {
        const auto& rows = search_shelves ? app.search_rows : app.home_rows;
        if (rows.empty()) {
            if (search_shelves) empty.draw(c);
            else {
                text(c, c.fonts.regular, app.busy ? tr("Caricamento...", "Loading...")
                                                : tr("Nessun catalogo disponibile. Apri Impostazioni.",
                                                     "No catalogs available. Open Settings."),
                     kLeft, 580, 27, theme.text_muted, kWidth);
            }
            return;
        }
        c.list.push_clip({kLeft - 16, shelf_top - 12, kWidth + 32, 990 - shelf_top});
        c.list.push_transform(1, 0, 0, 0, -row_scroll.value);
        for (std::size_t i = 0; i < shelves.size(); ++i) {
            const auto& shelf = shelves[i];
            const float y = shelf.bounds().y - row_scroll.value;
            if (y + shelf.preferred_height() < shelf_top - 24 || y > 994) continue;
            // A lone next-row heading reads as a misplaced poster caption.
            // Introduce that shelf when some of its artwork can be seen.
            const float first_art = y + shelf.style.title_size + shelf.style.title_gap + 15;
            if (i < rows.size() && !rows[i].cards.empty() && first_art > 950) continue;
            shelf.draw(c);
            if (!shelf.items().empty()) {
                const auto first = shelf.item_rect(0);
                const auto last = shelf.item_rect(static_cast<int>(shelf.items().size()) - 1);
                const auto& bounds = shelf.bounds();
                // Fade the viewport edge itself as well as Carousel's whole
                // peeking card. The mask ends before the selected card, and
                // disappears at either genuine end of the catalog.
                constexpr float bleed = 16, fade = 64;
                const float art_y = first.y - 24;
                const float art_h = first.h + 48;
                if (first.x < bounds.x - 1)
                    c.list.gradient_rect_h({bounds.x - bleed, art_y, fade, art_h}, 0,
                                          theme.page, theme.page.with_alpha(0));
                if (last.x + last.w > bounds.x + bounds.w + 1)
                    c.list.gradient_rect_h({bounds.x + bounds.w + bleed - fade, art_y, fade, art_h}, 0,
                                          theme.page.with_alpha(0), theme.page);
            }
            if (i < rows.size() && rows[i].cards.empty() && !rows[i].message.empty()) {
                text(c, c.fonts.regular, translated(rows[i].message), kLeft + 20,
                     shelf.bounds().y + 104, 24, theme.text_muted, kWidth - 80);
            }
        }
        c.list.pop_transform();
        c.list.pop_clip();
        c.list.gradient_rect({kLeft - 18, 950, kWidth + 36, 50}, 0,
                             theme.page.with_alpha(0), theme.page);
    }
    void paint_page_title(Canvas& c, const std::string& title, const std::string& note) const {
        text(c, c.fonts.semibold, title, kLeft, 218, 52, theme.text, kWidth);
        if (!note.empty()) text(c, c.fonts.regular, note, kLeft, 266, 25, theme.text_muted, kWidth);
    }
    void paint_downloads(Canvas& c, const App& app) const {
        paint_page_title(c, tr("Download", "Downloads"), app.download_status);
        if (app.download_groups.empty()) { empty.draw(c); return; }
        if (app.download_series_id.empty() && !download_panel_transition.visible()) {
            const char* sort = app.settings().download_sort == 0 ? tr("Ordina: Titolo A–Z", "Sort: Title A–Z") :
                app.settings().download_sort == 1 ? tr("Ordina: Anno", "Sort: Year") :
                tr("Ordina: Data download", "Sort: Download date");
            auto glyphs = ui::GlyphStyle::dark();
            glyphs.ink = theme.text;
            glyphs.label = theme.text_muted;
            ui::HintLayout layout;
            layout.cy = 202; layout.size = 30; layout.text_size = 24;
            const ui::Hint hint{ui::Button::l2, sort, ui::Button::r2};
            ui::draw_hints(c.list, c.fonts, glyphs, &hint, 1, kRight, true, layout);
        }
        const float lift = hui::tween::clamp01(download_lift.value);
        const int selected = bounded(app.download_group_sel, app.download_groups.size());
        // The cover wheel's depth follows the reference kit's Cover Wheel
        // design: every pose is a smooth function of distance from the focus.
        // Far covers are painted first, so moving across them never pops layers.
        const auto pose = [&](int index) {
            const float distance = static_cast<float>(index) - download_position.value;
            const float away = std::fabs(distance);
            const float soft = std::sqrt(distance * distance + .09f) - .3f;
            const float scale = .34f + .66f * std::exp(-soft * .9f);
            const float offset = std::copysign(536.0f * (1.0f - std::exp(-away * .88f)) + 54.0f * away, distance);
            const float width = 320.0f * scale, height = 480.0f * scale;
            const float bottom = 658.0f + 98.0f * scale;
            Rect box{960 + offset - width * .5f, bottom - height, width, height};
            if (index == download_open_index && lift > 0) {
                constexpr Rect opened{166, 298, 332, 498};
                const float transition = app.ui_reduced_motion ? (lift > .5f ? 1.0f : 0.0f) : hui::tween::smoothstep(lift);
                box = {box.x + (opened.x - box.x) * transition, box.y + (opened.y - box.y) * transition,
                       box.w + (opened.w - box.w) * transition, box.h + (opened.h - box.h) * transition};
            }
            return box;
        };
        const auto draw_cover = [&](int index) {
            const auto& group = app.download_groups[static_cast<size_t>(index)];
            const float away = std::fabs(static_cast<float>(index) - download_position.value);
            const float open = index == download_open_index ? lift : 0;
            const float alpha = hui::tween::clamp01(6.0f - away) * (index == download_open_index ? 1.0f : 1.0f - lift);
            if (alpha <= .005f) return;
            const Rect box = pose(index);
            const float illumination = .35f + .65f * std::exp(-away * .65f);
            const float radius = std::min(18.0f, box.w * .055f);
            const auto image = texture(group.image);
            ui::Painter paint(c.list, c.fonts, theme, c.glass);
            if (index == selected)
                paint.halo(box, radius, 42, theme.accent.with_alpha((.20f + .04f * std::sin(time)) * alpha));
            c.list.shadow({box.x, box.y + 14, box.w, box.h}, radius, 38,
                          Color::rgb(0x000000, .7f * alpha));
            if (image) {
                c.list.image(image, box, gfx::kFullUv,
                    Color{illumination + (1 - illumination) * open, illumination + (1 - illumination) * open,
                          illumination + (1 - illumination) * open, alpha}, radius);
                const Rect reflection{box.x, box.y + box.h + 7, box.w, std::min(62.0f, box.h * .14f)};
                c.list.image_gradient(image, reflection, {0, 1, 1, -.16f},
                    Color::rgb(0xffffff, .18f * alpha * (1 - lift)), Color::rgb(0xffffff, 0), radius);
            } else {
                c.list.gradient_rect(box, radius, theme.surface_high.with_alpha(alpha), theme.surface.with_alpha(alpha));
                icon(c, {box.cx() - 28, box.cy() - 28, 56, 56}, 18, theme.text_muted.with_alpha(alpha));
            }
            c.list.bordered_rect(box, radius, theme.surface.with_alpha(0),
                index == selected ? 2.4f : 1, theme.text.with_alpha((index == selected ? .78f : .16f) * alpha));
        };
        const int first = std::max(0, int(std::floor(download_position.value)) - 6);
        const int last = std::min(int(app.download_groups.size()) - 1, int(std::ceil(download_position.value)) + 6);
        std::array<int, 15> order{};
        const int count = std::max(0, last - first + 1);
        for (int i = 0; i < count; ++i) order[static_cast<size_t>(i)] = first + i;
        std::sort(order.begin(), order.begin() + count, [&](int a, int b) {
            return std::fabs(float(a) - download_position.value) > std::fabs(float(b) - download_position.value);
        });
        for (int i = 0; i < count; ++i) draw_cover(order[static_cast<size_t>(i)]);
        const auto& group = app.download_groups[static_cast<size_t>(selected)];
        // Move one title with the cover instead of drawing it a second time
        // when the episode panel opens or closes.
        const float title_shift = app.ui_reduced_motion ? (lift > .5f ? 1.0f : 0.0f) : hui::tween::smoothstep(lift);
        const auto title = plain_text(lift > .01f ? download_open_title : group.title);
        const float title_size = 38 - 6 * title_shift;
        const float title_width = 1296 - 916 * title_shift;
        const auto title_lines = c.fonts.semibold.font->wrap(title, title_size, title_width);
        const size_t max_title_lines = lift > .01f ? 2 : 1;
        for (size_t line = 0; line < std::min(title_lines.size(), max_title_lines); ++line) {
            const auto value = line + 1 == max_title_lines && title_lines.size() > max_title_lines
                ? c.fonts.semibold.font->fit(title_lines[line] + " …", title_size, title_width)
                : title_lines[line];
            const float centered = 960 - c.fonts.semibold.measure(value, title_size) * .5f;
            text(c, c.fonts.semibold, value, centered + (166 - centered) * title_shift,
                 824 + 24 * title_shift + float(line) * (45 - 6 * title_shift),
                 title_size, theme.text, title_width);
        }
        if (lift >= .995f) return;
        c.list.push_opacity(1 - lift);
        if (group.representative >= 0 && group.representative < int(app.download_rows.size())) {
            const auto& item = app.download_rows[static_cast<size_t>(group.representative)];
            if (item.complete) { c.list.pop_opacity(); return; }
            const Color state = item.failed ? theme.danger
                : item.paused ? theme.warning : theme.accent;
            std::string label = translated(plain_text(item.status));
            if (group.series) {
                const std::string episode = (item.season >= 0 ? std::string("S") + std::to_string(item.season) : "") +
                    (item.episode >= 0 ? std::string("E") + std::to_string(item.episode) : "");
                if (!episode.empty()) label = episode + (label.empty() ? "" : " · " + label);
            }
            text(c, c.fonts.regular, label, 960, 866, 24, state, 1296, gfx::Align::center);
            auto progress = download_progress.find(item.id);
            if (progress != download_progress.end()) {
                auto bar = progress->second;
                bar.style.height = 9;
                bar.set_bounds({520, 891, 880, 9});
                bar.draw(c);
            }
            if (item.progress >= 0) {
                char percentage[24];
                std::snprintf(percentage, sizeof(percentage), "%.1f%%", double(item.progress) * 100.0);
                text(c, c.fonts.semibold, percentage, 1458, 903, 23, state, 120);
            }
            std::string details = item.size;
            for (const auto* part : {&item.speed, &item.remaining, &item.connections})
                if (!part->empty()) details += (details.empty() ? "" : "  ·  ") + *part;
            text(c, c.fonts.regular, details, 960, 947, 22, theme.text_muted, 1500, gfx::Align::center);
        }
        c.list.pop_opacity();
    }
    void paint_download_panel(Canvas& c, const App& app) const {
        if (app.view != "downloads" || app.watching() || !download_panel_transition.visible()) return;
        download_panel_transition.begin(c, download_panel.bounds());
        download_panel.draw(c);
        download_seasons.draw(c);
        downloads.draw(c);
        download_panel_transition.end(c);
    }
    void paint_discover(Canvas& c, const App& app) const {
        paint_page_title(c, tr("Scopri", "Discover"), "");
        filters.draw(c);
        if (app.disc_cards.empty()) {
            empty.draw(c);
            return;
        }
        grid.draw(c);
        const Rect panel{1350, 332, 474, 628};
        ui::Painter paint(c.list, c.fonts, theme, c.glass);
        paint.panel(panel);
        const std::string name = !app.dp_name.empty() ? app.dp_name
            : app.disc_cards[static_cast<std::size_t>(bounded(app.disc_sel, app.disc_cards.size()))].title;
        if (texture(app.dp_logo)) picture(c, app.dp_logo, {panel.x + 24, panel.y + 22, panel.w - 48, 102}, 0);
        else ui::paragraph(c.list, c.fonts.semibold, name, panel.x + 24, panel.y + 62, 34,
                           panel.w - 48, 40, theme.text, 2);
        paint_metadata(c, app.dp_year, app.dp_runtime, app.dp_imdb, "", panel.x + 24, panel.y + 164, panel.w - 48, 21);
        float baseline = panel.y + 203;
        if (!app.dp_genres.empty()) {
            ui::paragraph(c.list, c.fonts.regular, app.dp_genres, panel.x + 24, baseline, 22,
                          panel.w - 48, 29, theme.text_muted, 2);
            baseline += 72;
        }
        if (!app.dp_directors.empty()) {
            text(c, c.fonts.semibold, tr("Regia", "Director"), panel.x + 24, baseline, 19, theme.accent, panel.w - 48);
            ui::paragraph(c.list, c.fonts.regular, app.dp_directors, panel.x + 24, baseline + 30, 22,
                          panel.w - 48, 29, theme.text, 2);
            baseline += 96;
        }
        if (!app.dp_cast.empty()) {
            text(c, c.fonts.semibold, tr("Interpreti principali", "Starring"), panel.x + 24, baseline, 19, theme.accent, panel.w - 48);
            ui::paragraph(c.list, c.fonts.regular, app.dp_cast, panel.x + 24, baseline + 30, 22,
                          panel.w - 48, 29, theme.text, 2);
            baseline += 106;
        }
        const int lines = std::max(1, int((panel.y + panel.h - 20 - baseline) / 30));
        ui::paragraph(c.list, c.fonts.regular, plain_text(app.dp_desc), panel.x + 24,
                      baseline, 23, panel.w - 48, 30, theme.text_muted, lines);
        if (!app.disc_status.empty())
            text(c, c.fonts.regular, translated(app.disc_status), kLeft, 984, 21, theme.text_muted, 1030);
    }
    void paint_library(Canvas& c, const App& app) const {
        paint_page_title(c, tr("La tua libreria", "Your library"), "");
        if (!app.lib_cards.empty())
            text(c, c.fonts.semibold, app.lib_cards[static_cast<std::size_t>(bounded(app.lib_sel, app.lib_cards.size()))].title,
                 kRight, 216, 27, theme.text_muted, 840, gfx::Align::right);
        filters.draw(c);
        if (app.lib_cards.empty()) empty.draw(c);
        else grid.draw(c);
    }
    void paint_addons(Canvas& c, const App& app) const {
        paint_page_title(c, tr("I tuoi add-on", "Your add-ons"),
                        tr("Cataloghi, fonti e sottotitoli. Tutti i tuoi servizi, in un unico posto.",
                           "Catalogs, sources and subtitles. All your services in one place."));
        text(c, c.fonts.mono, std::to_string(app.addon_rows.size()) + tr(" ADD-ON", " ADD-ONS"),
             kRight, 213, 21, theme.accent, 300, gfx::Align::right);
        if (app.addon_rows.empty()) empty.draw(c);
        else addon_grid.draw(c);
    }
    void paint_settings(Canvas& c, const App& app) const {
        paint_page_title(c, tr("Impostazioni", "Settings"), "");
        ui::Painter paint(c.list, c.fonts, theme, c.glass);
        const Rect help{kLeft, 262, 432, 692};
        paint.panel(help);
        if (!app.s_rows.empty()) {
            const auto& row = app.s_rows[static_cast<std::size_t>(bounded(app.s_sel, app.s_rows.size()))];
            const bool subtitle = row.id.starts_with("sub_") || row.id.starts_with("subtitle_") || row.id == "auto_subtitles";
            const int symbol = row.id == "account" || row.id == "exit" ? 21
                : row.id == "audio_languages" || row.id.find("sound") != std::string::npos || row.id.find("volume") != std::string::npos ? 6
                : row.id == "subtitle_font" ? 16 : subtitle ? 7
                : row.id.find("language") != std::string::npos ? 15
                : row.id == "seek_seconds" || row.id == "shoulder_seek_seconds" ? 17
                : row.id == "server_url" ? 11 : row.id == "builtin_torrents" || row.id == "torrent_speed_profile" || row.id == "download_directory" ? 9
                : row.id.find("display") != std::string::npos || row.id.find("resolution") != std::string::npos ? 19
                : row.id.find("addon") != std::string::npos ? 3 : 4;
            const Rect symbol_box{help.x + 30, help.y + 32, 76, 76};
            c.list.rounded_rect(symbol_box, 22, theme.primary.with_alpha(.16f));
            icon(c, symbol_box.inset(18), symbol, theme.accent);
            ui::paragraph(c.list, c.fonts.semibold, translated(row.label), help.x + 30, help.y + 169,
                          33, help.w - 60, 42, theme.text, 3);
            ui::paragraph(c.list, c.fonts.regular, translated(row.hint), help.x + 30, help.y + 309,
                          25, help.w - 60, 34, theme.text_muted, subtitle ? 3 : 8);
            if (subtitle) {
                text(c, c.fonts.semibold, tr("Anteprima sottotitoli", "Subtitle preview"),
                     help.x + 30, help.y + 447, 22, theme.text, help.w - 60);
                const Rect preview{help.x + 22, help.y + 471, help.w - 44, 188};
                c.list.gradient_rect_h(preview, 15, Color::rgb(0x4a5f70), Color::rgb(0x26394a));
                c.list.gradient_rect(preview, 15, Color::rgb(0x1d2840,0), Color::rgb(0x101620,.9f));
                c.list.bordered_rect(preview,15,theme.surface.with_alpha(0),1,theme.outline);
                c.list.push_clip(preview.inset(8));
                paint_caption(c, app, tr("Ogni storia merita\nla sua voce.", "Every story deserves\nits own voice."),
                              preview.cx(), preview.y + 145, preview.w - 32, .75f);
                c.list.pop_clip();
            }
        }
        settings.draw(c);
    }

    void paint_directories(Canvas& c, const App& app) const {
        paint_page_title(c, tr("Cartella dei download", "Download folder"),
            tr("Scegli dove conservare film ed episodi.", "Choose where to store movies and episodes."));
        ui::Painter paint(c.list, c.fonts, theme, c.glass);
        const Rect list_panel{kLeft, 302, 844, 652};
        const Rect detail_panel{990, 302, 834, 652};
        paint.panel(list_panel);
        paint.panel(detail_panel);
        text(c, c.fonts.semibold, tr("Cartelle", "Folders"), 122, 351, 25, theme.text, 590);
        text(c, c.fonts.regular, std::to_string(app.directory_picker_entries.size()),
             898, 351, 22, theme.text_muted, 120, gfx::Align::right);
        if (app.directory_picker_entries.empty()) {
            ui::paragraph(c.list, c.fonts.regular,
                app.directory_picker_loading ? tr("Lettura delle cartelle…", "Reading folders…")
                    : tr("Questa cartella non contiene sottocartelle.", "This folder has no subfolders."),
                156, 569, 27, 724, 37, theme.text_muted, 3);
        } else directories.draw(c);
        directory_path.draw(c);
        text(c, c.fonts.semibold, tr("Cartella visualizzata", "Current folder"), 1026, 444, 24, theme.accent, 756);
        ui::paragraph(c.list, c.fonts.regular, app.directory_picker_path,
            1026, 487, 27, 756, 36, theme.text, 3);
        c.list.rounded_rect({1026, 597, 756, 1}, 0, theme.outline);
        if (app.directory_picker_committing) {
            text(c, c.fonts.semibold, app.directory_picker_status,
                1026, 641, 25, theme.accent, 756);
            text(c, c.fonts.regular, app.directory_move_title,
                1026, 689, 25, theme.text, 756);
            const auto size = [](int64_t bytes) {
                char value[64];
                if (bytes >= (1ll << 30)) std::snprintf(value, sizeof(value), "%.2f GiB", double(bytes) / (1ll << 30));
                else std::snprintf(value, sizeof(value), "%.1f MiB", double(std::max<int64_t>(0, bytes)) / (1ll << 20));
                return std::string(value);
            };
            if (app.directory_move_total >= 0) {
                const auto bytes = size(app.directory_move_done) + " / " + size(app.directory_move_total);
                text(c, c.fonts.regular, bytes, 1026, 732, 22, theme.text_muted, 620);
                const auto percentage = app.directory_move_total > 0 ? std::clamp(
                    int(100.0 * double(app.directory_move_done) / app.directory_move_total), 0, 100) : 0;
                text(c, c.fonts.semibold, std::to_string(percentage) + "%", 1782, 732, 22,
                    theme.text, 120, gfx::Align::right);
            }
            directory_move.draw(c);
            if (app.directory_move_items_total > 0) {
                const auto items = std::to_string(app.directory_move_items_done) + " / " +
                    std::to_string(app.directory_move_items_total) + tr(" download spostati", " downloads moved");
                text(c, c.fonts.regular, items, 1026, 807, 24, theme.text_muted, 756);
            }
            ui::paragraph(c.list, c.fonts.regular,
                tr("Al termine, i download saranno disponibili nella nuova cartella.",
                   "Your downloads will be available in the new folder when the move finishes."),
                1026, 863, 24, 756, 33, theme.text_muted, 2);
            directory_spinner.draw(c);
            return;
        }
        text(c, c.fonts.semibold, tr("Destinazione in uso", "Saved destination"), 1026, 641, 22, theme.text_muted, 756);
        ui::paragraph(c.list, c.fonts.regular, app.settings().download_directory.empty()
                ? tr("Non selezionata", "Not selected") : app.settings().download_directory,
            1026, 681, 24, 756, 32, theme.text, 2);
        c.list.rounded_rect({1026, 754, 756, 1}, 0, theme.outline);
        const auto message = app.directory_picker_status.empty()
            ? tr("Quando confermi, i download vengono spostati nella cartella scelta. Le unità collegate sono disponibili in /mnt.",
                 "Confirming moves your downloads to the selected folder. Connected drives are available under /mnt.")
            : app.directory_picker_status.c_str();
        ui::paragraph(c.list, c.fonts.regular, message, 1026, 801, 25, 756, 34,
            app.directory_picker_error ? theme.danger : theme.text_muted, 3);
        directory_spinner.draw(c);
    }
    void paint_detail(Canvas& c, const App& app) const {
        if (texture(app.d_logo)) picture(c, app.d_logo, {kLeft, 160, 560, 120}, 0);
        else {
            const float title_size = app.d_name.size() > 48 ? 49.0f : 61.0f;
            text(c, c.fonts.semibold, app.d_name, kLeft, 252, title_size, theme.text, 1530);
        }
        paint_metadata(c, app.d_year, app.d_runtime, app.d_imdb, app.d_genres, kLeft, 314, 1500);
        const float copy_width = app.d_series ? 1510.0f : 824.0f;
        if (app.d_series && app.d_zone != "streams") {
            if (!app.d_episodes.empty()) {
                const auto& selected = app.d_episodes[static_cast<std::size_t>(bounded(app.d_episode_sel, app.d_episodes.size()))];
                text(c, c.fonts.semibold, selected.title, kLeft, 377, 32, theme.text, 1500);
                ui::paragraph(c.list, c.fonts.regular,
                              plain_text(selected.description.empty() ? app.d_description : selected.description),
                              kLeft, 417, 25, 1320, 34, theme.text.with_alpha(.9f), 2);
            }
            season.draw(c);
            episodes.draw(c);
            if (app.d_episodes.empty()) {
                ui::paragraph(c.list, c.fonts.regular,
                    tr("Gli episodi appariranno qui quando i metadati saranno disponibili.",
                       "Episodes will appear here when their metadata is available."),
                    kLeft + 12, 656, 27, kWidth - 24, 38, theme.text_muted, 3);
            }
            return;
        }
        ui::paragraph(c.list, c.fonts.regular, plain_text(app.d_description), kLeft, 360,
                      25, copy_width, 33, theme.text.with_alpha(.9f), app.d_series ? 2 : 8);
        if (app.d_series) {
            icon(c, {kLeft, 433, 30, 30}, 19, theme.accent);
            std::string episode = translated(app.d_season_label);
            if (!app.d_episodes.empty()) {
                const auto& selected = app.d_episodes[static_cast<std::size_t>(bounded(app.d_episode_sel, app.d_episodes.size()))];
                episode += "  ·  " + (selected.number.empty() ? "" : selected.number + ". ") + selected.title;
            }
            text(c, c.fonts.semibold, episode, kLeft + 44, 457, 27, theme.text, kWidth - 64);
        } else {
            if (app.d_pick_res)
                text(c, c.fonts.semibold, tr("Qualità", "Quality"), 1016, 377, 28, theme.text, 808);
            if (!app.d_directors.empty()) {
                text(c, c.fonts.semibold, tr("REGIA", "DIRECTOR"), kLeft, 719, 17, theme.accent, copy_width);
                text(c, c.fonts.regular, app.d_directors, kLeft, 751, 25, theme.text_muted, copy_width);
            }
            if (!app.d_cast.empty()) {
                text(c, c.fonts.semibold, "CAST", kLeft, 801, 17, theme.accent, copy_width);
                ui::paragraph(c.list, c.fonts.regular, app.d_cast, kLeft, 833, 24, copy_width, 33, theme.text_muted, 2);
            }
            if (!app.d_resume.empty())
                text(c, c.fonts.semibold, translated(app.d_resume), kLeft, 922, 25, theme.accent, copy_width);
        }
        if (!app.d_sources.empty() && !app.d_pick_res) {
            c.list.rounded_rect(sources.bounds().inset(-6), 16, theme.page.with_alpha(.82f));
            sources.draw(c);
        }
        if (!app.d_streams.empty()) streams.draw(c);
        else {
            ui::Painter paint(c.list, c.fonts, theme, c.glass);
            paint.panel(streams.bounds());
            const auto& box = streams.bounds();
            const std::string status = app.d_streams_status.empty()
                ? tr("Nessuna fonte disponibile per questo titolo.", "No source is available for this title.")
                : translated(app.d_streams_status);
            icon(c, {box.x + 34, box.y + 38, 36, 36}, 18, theme.accent);
            ui::paragraph(c.list, c.fonts.regular, status, box.x + 34, box.y + 124,
                          27, box.w - 68, 38, theme.text_muted, 5);
        }
        if (!app.d_streams.empty() && !app.d_streams_status.empty())
            text(c, c.fonts.regular, translated(app.d_streams_status), streams.bounds().x,
                 986, 20, theme.text_muted, streams.bounds().w);
    }
    void paint_login(Canvas& c, const App& app) const {
        // Account linking is its own page: never blur or draw the catalog,
        // settings, navigation or pending dialogs behind these credentials.
        c.list.rounded_rect({0, 0, 1920, 1080}, 0, theme.page);
        const auto mark = texture("assets/icons/logo.png");
        if (mark) c.list.image(mark, {96, 71, 48, 48}, gfx::kFullUv, Color::rgb(0xffffff), 0);
        else {
            c.list.circle(120, 95, 23, theme.primary);
            c.list.triangle({112, 82, 20, 26}, Color::rgb(0xffffff), 0, kPi * .5f);
        }
        text(c, c.fonts.semibold, tr("Collega account", "Link your account"),
             960, 187, 44, theme.text, 1300, gfx::Align::center);

        const bool loading = app.login_state == App::LoginState::loading;
        const bool ready = app.login_state == App::LoginState::ready;
        const bool expired = app.login_state == App::LoginState::expired;
        const Rect qr{780, 246, 360, 360};
        if (ready) {
            c.list.rounded_rect(qr.inset(-20), 14, Color::rgb(0xffffff));
            const auto image = texture(app.login_qr);
            if (image) c.list.image(image, qr, gfx::kFullUv, Color::rgb(0xffffff), 0);
            else text(c, c.fonts.regular, tr("Caricamento QR...", "Loading QR..."),
                      qr.cx(), qr.cy() + 10, 25, Color::rgb(0x282234), qr.w - 40, gfx::Align::center);
            text(c, c.fonts.regular, tr("1   Scansiona il codice QR oppure apri il link", "1   Scan the QR code or open the link"),
                 960, 683, 27, theme.text, 1500, gfx::Align::center);
            text(c, c.fonts.semibold, app.login_link,
                 960, 728, 27, theme.accent, 1500, gfx::Align::center);
            const std::string instruction = std::string(tr("2   Accedi al tuo account Stremio", "2   Sign in to your Stremio account")) +
                (app.login_code.empty() ? "" : std::string(tr("  ·  Codice: ", "  ·  Code: ")) + app.login_code);
            text(c, c.fonts.regular, instruction,
                 960, 783, 27, theme.text, 1500, gfx::Align::center);
            const int remaining = std::max(0, app.login_seconds_remaining);
            char countdown[32];
            std::snprintf(countdown, sizeof(countdown), "%02d:%02d", remaining / 60, remaining % 60);
            text(c, c.fonts.mono, std::string(tr("Tempo disponibile: ", "Time remaining: ")) + countdown,
                 960, 858, 25, theme.text_muted, 900, gfx::Align::center);
        } else {
            const Color status = loading ? theme.accent : expired ? theme.warning : theme.danger;
            c.list.circle(960, 398, 68, status.with_alpha(.12f));
            if (loading) {
                // A subtle animated arc uses the same clock as the other kit controls.
                c.list.arc(960, 398, 38, 4, time * 2, kPi * 1.45f, status);
            } else text(c, c.fonts.semibold, "!", 960, 422, 68, status, 100, gfx::Align::center);
            const char* title = loading ? tr("Preparazione del collegamento", "Preparing your link")
                : expired ? tr("Il codice è scaduto", "Your code has expired")
                          : tr("Accesso non disponibile", "Sign-in unavailable");
            text(c, c.fonts.semibold, title, 960, 534, 35, theme.text, 1400, gfx::Align::center);
            const char* instructions = loading
                ? tr("Stremio sta creando un link per il tuo account.", "Stremio is creating a link for your account.")
                : tr("Richiedi un nuovo link per collegare il tuo account.", "Request a new link to connect your account.");
            text(c, c.fonts.regular, instructions, 960, 593, 27, theme.text_muted, 1450, gfx::Align::center);
            if (!loading) {
                // Network/server errors remain visible without suggesting that
                // an expired QR or code can still be used.
                ui::paragraph(c.list, c.fonts.regular, translated(app.login_status),
                              440, 698, 25, 1040, 36, theme.text_muted, 3);
            }
        }
        login_retry.draw(c);
    }

    void paint_tracks(Canvas& c, const Rect& area) const {
        if (!model) return;
        track_tabs.draw(c);
        if (model->m_col == 0) {
            if (model->m_audio.empty()) {
                ui::paragraph(c.list, c.fonts.regular,
                    tr("Nessuna traccia audio selezionabile.", "No selectable audio track."),
                    area.x + 18, area.y + 139, 27, area.w - 36, 36, theme.text_muted, 3);
            } else audio.draw(c);
        } else subtitles.draw(c);
        c.list.line(area.x, area.y + area.h - 134, area.x + area.w, area.y + area.h - 134, 1, theme.outline);
        subtitle_delay.draw(c);
    }
    const ui::FontRef& caption_face(const Canvas& c, const App& app, const std::string& value) const {
        const auto& chosen = app.settings().sub_font == "serif" && subtitle_serif.font ? subtitle_serif
            : app.settings().sub_font == "mono" && subtitle_mono.font ? subtitle_mono
            : subtitle_font.font ? subtitle_font : c.fonts.semibold;
        if (subtitle_font.font && chosen.font != subtitle_font.font) {
            for (std::size_t at = 0; at < value.size();) {
                const auto cp = gfx::next_codepoint(value, &at);
                if (cp > 32 && !chosen.font->has_glyph(cp)) return subtitle_font;
            }
        }
        return chosen;
    }
    void paint_caption(Canvas& c, const App& app, const std::string& value,
                       float center, float bottom, float max_width, float scale = 1) const {
        if (value.empty()) return;
        const auto& face = caption_face(c, app, value);
        const auto& prefs = app.settings();
        const float size = (prefs.sub_size == "sub-s" ? 30.0f : prefs.sub_size == "sub-l" ? 45.0f : 37.0f) * scale;
        const float line = size * 1.3f;
        const auto wrapped = face.font->wrap(value, size, max_width);
        const int count = std::min(4, static_cast<int>(wrapped.size()));
        if (count == 0) return;
        const float first = bottom - static_cast<float>(count - 1) * line;
        float width = 0;
        for (int i = 0; i < count; ++i)
            width = std::max(width, face.measure(wrapped[static_cast<std::size_t>(i)], size));
        const float opacity = std::clamp(static_cast<float>(prefs.sub_background_opacity) / 100.0f, 0.0f, 1.0f);
        if (opacity > 0) {
            c.list.rounded_rect({center - width * .5f - 18*scale, first - face.font->ascent(size) - 10*scale,
                                width + 36*scale, (count-1)*line + face.font->ascent(size) + face.font->descent(size) + 20*scale},
                               8*scale, Color::rgb(0x000000, opacity));
        }
        const Color ink = prefs.sub_color == "yellow" ? Color::rgb(0xffdf57)
            : prefs.sub_color == "green" ? Color::rgb(0x80f2a0)
            : prefs.sub_color == "cyan" ? Color::rgb(0x7ee7ff) : Color::rgb(0xffffff);
        for (int i = 0; i < count; ++i) {
            const auto& text_value = wrapped[static_cast<std::size_t>(i)];
            const float baseline = first + static_cast<float>(i) * line;
            const auto layer = [&](float dx, float dy, Color color) {
                ui::text(c.list, face, text_value, center + dx*scale, baseline + dy*scale, size, color, gfx::Align::center);
            };
            if (prefs.sub_effect == "outline") {
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                    if (dx != 0 || dy != 0) layer(dx*1.8f, dy*1.8f, Color::rgb(0x000000,.95f));
            } else if (prefs.sub_effect == "shadow") {
                layer(3,4,Color::rgb(0x000000,.75f));
                layer(2,3,Color::rgb(0x000000,.75f));
            } else if (prefs.sub_effect == "raised") {
                layer(2,2,Color::rgb(0x000000,.95f));
                layer(-1,-1,Color::rgb(0xffffff,.85f));
            } else if (prefs.sub_effect == "depressed") {
                layer(-2,-2,Color::rgb(0x000000,.95f));
                layer(1,1,Color::rgb(0xffffff,.85f));
            }
            layer(0,0,ink);
        }
    }
    void paint_subtitles(Canvas& c, const App& app) const {
        if (sub_text.empty()) return;
        const float bottom = app.menu_visible ? 966.0f : watch_alpha.value > .2f ? 762.0f : 982.0f;
        paint_caption(c, app, sub_text, 960, bottom, 1600);
    }
    void paint_playback_bar(Canvas& c, const App& app) const {
        if (watch_alpha.value <= .001f || app.menu_visible || app.launch_visible || app.next_episode_visible) return;
        c.list.push_opacity(watch_alpha.value);
        // The transport and its controller legend are one surface. Their
        // common opacity also keeps both parts together when Circle hides it.
        constexpr Rect panel{96, 800, 1728, 252};
        ui::draw_overlay_panel(c, theme, panel, true, .46f, 24);
        media.draw(c);
        c.list.rounded_rect({126, 973, 1668, 1}, 0, theme.text.with_alpha(.12f));
        const std::string seek = std::to_string(app.settings().seek_seconds) + " s";
        const int shoulder_seconds = app.settings().shoulder_seek_seconds;
        const std::string shoulder = shoulder_seconds < 60 ? std::to_string(shoulder_seconds) + " s"
            : std::to_string(shoulder_seconds / 60) + " min" +
              (shoulder_seconds % 60 ? " " + std::to_string(shoulder_seconds % 60) + " s" : "");
        hint_row(c, theme, {{ui::Button::cross, app.w_paused ? tr("Riprendi", "Resume") : tr("Pausa", "Pause")},
                            {ui::Button::dpad, seek.c_str()},
                            {ui::Button::options, tr("Audio / sottotitoli", "Audio / subtitles")},
                            {ui::Button::right_stick, tr("Volume ↑ / ↓", "Volume ↑ / ↓")},
                            {ui::Button::circle, tr("Nascondi", "Hide")},
                            {ui::Button::l1, shoulder.c_str(), ui::Button::r1}}, 1011, 126, 1668);
        c.list.pop_opacity();
    }

    void paint_playback_volume(Canvas& c, const App& app) const {
        if (!app.watching() || !app.w_volume_visible || app.launch_visible || app.next_episode_visible) return;
        constexpr Rect panel{1296, 72, 528, 156};
        // A small opaque overlay needs no extra video readback or blur pass.
        c.list.rounded_rect(panel, 18, Color::rgb(0x0b0912, .90f));
        icon(c, {1320, 94, 32, 32}, 6, theme.text);
        if (app.audio_passthrough_active()) {
            text(c, c.fonts.semibold, "Audio passthrough", 1366, 119, 25, theme.text, 426);
            ui::paragraph(c.list, c.fonts.regular,
                tr("Regola il volume sulla TV o sul ricevitore.", "Adjust the volume on your TV or receiver."),
                1324, 165, 23, 472, 30, theme.text_muted, 2);
            return;
        }
        const int percent = playback_volume::clamp_percent(app.settings().media_volume);
        text(c, c.fonts.semibold, tr("Volume video", "Video volume"), 1366, 119, 25, theme.text, 294);
        text(c, c.fonts.semibold, std::to_string(percent) + "%", 1796, 119, 28,
             theme.text, 128, gfx::Align::right);
        constexpr Rect track{1324, 161, 472, 9};
        c.list.rounded_rect(track, 4.5f, theme.text.with_alpha(.18f));
        if (percent > 0)
            c.list.rounded_rect({track.x, track.y, track.w * float(percent) / 100.0f, track.h}, 4.5f, theme.accent);
        text(c, c.fonts.regular, tr("Levetta destra ↑ / ↓", "Right stick ↑ / ↓"),
             1324, 207, 19, theme.text_muted, 472);
    }

    void paint_launch(Canvas& c, const App& app) const {
        loader.draw(c);
        c.list.push_opacity(loader.opacity());
        const auto mark = texture("assets/icons/logo.png");
        if (mark) c.list.image(mark, {96, 71, 48, 48}, gfx::kFullUv, Color::rgb(0xffffff), 0);
        else {
            c.list.circle(120, 95, 23, theme.primary);
            c.list.triangle({112, 82, 20, 26}, Color::rgb(0xffffff), 0, kPi * .5f);
        }
        if (!paint_launch_logo(c, app)) {
            // Metadata may omit a transparent title logo. Keep the same
            // left/vertical alignment for a readable text fallback.
            const auto lines = c.fonts.semibold.font->wrap(plain_text(app.launch_title), 84, 900);
            const std::size_t count = std::min<std::size_t>(lines.size(), 3);
            const float first = 540 - static_cast<float>(count > 0 ? count - 1 : 0) * 47 + 28;
            for (std::size_t i = 0; i < count; ++i)
                text(c, c.fonts.semibold, lines[i], kLeft, first + static_cast<float>(i) * 94,
                     84, theme.text, 900);
        }
        const Rect opening{1020, app.t_visible ? 646.0f : 798.0f, 804, 152};
        launch_spinner.draw(c);
        const std::string status = app.launch_status.empty()
            ? tr("Apertura dello stream…", "Opening stream…") : translated(app.launch_status);
        ui::paragraph(c.list, c.fonts.regular, plain_text(status), opening.x + 78, opening.y + 48,
                      25, opening.w - 108, 32, theme.text, 3);
        launch_work.draw(c);
        if (app.t_visible) for (const auto& stat : torrent_stats) stat.draw(c);
        c.list.pop_opacity();
    }
    void paint_player(Canvas& c, const App& app) const {
        if (app.next_episode_visible) return;
        paint_subtitles(c, app);
        if (app.w_buffering && !app.launch_visible && !app.menu_visible) {
            playback_spinner.draw(c);
            const std::string status = app.w_buffer_text.empty() ? "Buffering…" : translated(app.w_buffer_text);
            // A soft text shadow remains readable over the video without
            // introducing another panel or disturbing the transport card.
            text(c, c.fonts.regular, status, 962, 568, 27,
                 Color::rgb(0x000000, .9f), 1020, gfx::Align::center);
            text(c, c.fonts.regular, status, 960, 566, 27,
                 theme.text, 1020, gfx::Align::center);
        }
        if (app.ui_show_stats && !app.w_stats.empty() && !app.launch_visible && !app.menu_visible) {
            c.list.rounded_rect({96, 68, 1190, 122}, 16, Color::rgb(0x000000, .78f));
            ui::paragraph(c.list, c.fonts.mono, plain_text(app.w_stats), 120, 112,
                          22, 1142, 30, Color::rgb(0xf6f3fc), 3);
        }
    }
    void paint_next_episode(Canvas& c, const App& app) const {
        if (!app.next_episode_visible) return;
        const float enter = hui::tween::smoothstep(next_enter);
        c.list.push_opacity(enter);
        c.list.push_transform(1, 0, 0, 0, app.ui_reduced_motion ? 0 : 18 * (1 - enter));
        ui::draw_overlay_panel(c, theme, {962, 590, 862, 368}, true, .66f, 24);
        text(c, c.fonts.semibold, tr("Prossimo episodio", "Up next"), 994, 624, 23, theme.accent, 798);
        constexpr Rect art{994, 646, 260, 146};
        if (texture(app.next_episode_thumb)) picture(c, app.next_episode_thumb, art, 12);
        else {
            c.list.gradient_rect(art, 12, theme.surface_high, theme.surface);
            icon(c, {art.cx() - 22, art.cy() - 22, 44, 44}, 18, theme.accent);
        }
        text(c, c.fonts.regular, app.next_episode_label, 1284, 665, 23, theme.text_muted, 508);
        ui::paragraph(c.list, c.fonts.semibold, app.next_episode_title, 1284, 704, 29, 508, 36, theme.text, 2);
        const auto countdown = std::string(tr("Inizia tra ", "Starts in ")) +
            std::to_string(std::max(0, app.next_episode_seconds)) + " s";
        text(c, c.fonts.regular, countdown, 1284, 791, 23, theme.text_muted, 508);
        next_play.draw(c);
        next_ignore.draw(c);
        next_countdown.draw(c);
        c.list.pop_transform();
        c.list.pop_opacity();
    }
    void paint_footer(Canvas& c, const App& app) const {
        const auto cross = ui::Button::cross, circle = ui::Button::circle;
        const auto square = ui::Button::square, triangle = ui::Button::triangle;
        const auto dpad = ui::Button::dpad;
        if (app.login_visible) {
            hint_row(c, theme, {{cross, tr("Richiedi nuovo link", "Request a new link")},
                                {circle, app.settings().auth_key.empty() ? tr("Esci", "Exit") : tr("Indietro", "Back")}});
        } else if (app.download_directory_notice) {
            hint_row(c, theme, {{cross, "OK"}});
        } else if (app.next_episode_visible) {
            hint_row(c, theme, {{dpad, tr("Scegli", "Choose")}, {cross, tr("Seleziona", "Select")},
                                {circle, tr("Ignora", "Ignore")}}, kFooter, 96, kWidth, true);
        } else if (app.directory_picker_visible) {
            if (!app.directory_picker_committing)
                hint_row(c, theme, {{cross, tr("Apri", "Open")},
                                    {triangle, tr("Usa questa cartella", "Use this folder")},
                                    {square, tr("Crea cartella", "Create folder")},
                                    {ui::Button::options, tr("Aggiorna", "Refresh")},
                                    {circle, tr("Indietro", "Back")},
                                    {ui::Button::l1, tr("Pagina", "Page"), ui::Button::r1}});
        } else if (source_info.is_open()) {
            hint_row(c, theme, {{dpad, tr("Scorri", "Scroll")}, {circle, tr("Chiudi", "Close")}});
        } else if (app.input_visible_) {
            hint_row(c, theme, {{cross, tr("Inserisci", "Type")}, {square, tr("Elimina", "Delete")},
                                {triangle, tr("Spazio", "Space")}, {ui::Button::options, tr("Fine", "Done")},
                                {circle, tr("Annulla", "Cancel")}});
        } else if (app.dd_visible) {
            hint_row(c, theme, {{dpad, tr("Scegli", "Choose")},
                                {cross, app.dd_multiselect ? tr("Seleziona / deseleziona", "Select / deselect") : tr("Conferma", "Confirm")},
                                {circle, tr("Fine", "Done")}});
        } else if (app.launch_visible) {
            hint_row(c, theme, {{circle, tr("Annulla", "Cancel")}});
        } else if (app.watching()) {
            if (app.menu_visible)
                hint_row(c, theme, {{ui::Button::l1, tr("Audio / sottotitoli", "Audio / subtitles"), ui::Button::r1},
                                    {cross, tr("Seleziona", "Select")},
                                    {ui::Button::l2, tr("Ritardo", "Delay"), ui::Button::r2},
                                    {circle, tr("Chiudi", "Close")}}, kFooter, 96, kWidth, true);
            // The ordinary playback legend is drawn inside the transport's
            // single glass panel by paint_playback_bar().
        } else if (app.view == "detail") {
            if (app.d_series && app.d_zone != "streams")
                hint_row(c, theme, {{cross, tr("Vedi fonti", "Show sources")},
                                    {ui::Button::l1, tr("Stagioni", "Seasons"), ui::Button::r1},
                                    {triangle, tr("Segna visto", "Mark watched")}, {circle, tr("Indietro", "Back")}});
            else if (app.d_pick_res)
                hint_row(c, theme, {{cross, tr("Scegli qualità", "Choose quality")}, {circle, tr("Indietro", "Back")}});
            else hint_row(c, theme, {{cross, tr("Riproduci", "Play")},
                                     {square, tr("Scarica", "Download")},
                                     {ui::Button::l1, tr("Sorgenti", "Sources"), ui::Button::r1},
                                     {triangle, tr("Dettagli fonte", "Source details")},
                                     {ui::Button::options, tr("Qualità", "Quality")},
                                     {circle, tr("Indietro", "Back")}});
        } else if (app.zone == "searchbox") {
            hint_row(c, theme, {{cross, tr("Cerca", "Search")}, {dpad, tr("Naviga", "Navigate")},
                                {circle, tr("Indietro", "Back")}});
        } else if (app.view == "settings") {
            hint_row(c, theme, {{ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
        } else if (app.view == "addons") {
            hint_row(c, theme, {{cross, tr("Gestisci", "Manage")}, {square, tr("Aggiorna", "Refresh")},
                                {circle, tr("Impostazioni", "Settings")}});
        } else if (app.view == "downloads") {
            if (app.download_rows.empty())
                hint_row(c, theme, {{triangle, tr("Cerca", "Search")},
                                    {ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
            else {
                const auto& item = app.download_rows[static_cast<std::size_t>(bounded(app.download_sel, app.download_rows.size()))];
                const bool series = app.download_group_sel >= 0 && app.download_group_sel < int(app.download_groups.size()) &&
                    app.download_groups[static_cast<size_t>(app.download_group_sel)].series;
                const char* action = item.recovery_only ? tr("Informazioni", "Details")
                    : item.complete ? tr("Riproduci", "Play")
                    : item.active ? tr("Pausa", "Pause") : item.failed ? tr("Riprova", "Retry")
                    : item.paused ? tr("Riprendi", "Resume") : tr("Pausa", "Pause");
                if (!app.download_series_id.empty()) {
                    if (item.playable_while_downloading)
                        hint_row(c, theme, {{cross, action}, {ui::Button::options, tr("Riproduci ora", "Play now")},
                            {square, tr("Elimina episodio", "Delete episode")},
                            {ui::Button::l1, tr("Stagioni", "Seasons"), ui::Button::r1}, {circle, tr("Indietro", "Back")}});
                    else hint_row(c, theme, {{cross, action}, {square, tr("Elimina episodio", "Delete episode")},
                        {ui::Button::l1, tr("Stagioni", "Seasons"), ui::Button::r1}, {circle, tr("Indietro", "Back")}});
                } else if (series) {
                    hint_row(c, theme, {{cross, tr("Episodi", "Episodes")}, {dpad, tr("Naviga", "Browse")},
                        {triangle, tr("Cerca", "Search")}, {ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
                } else if (item.playable_while_downloading)
                    hint_row(c, theme, {{ui::Button::options, tr("Riproduci ora", "Play now")},
                                        {cross, action}, {square, tr("Elimina", "Delete")},
                                        {triangle, tr("Cerca", "Search")},
                                        {ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
                else
                    hint_row(c, theme, {{cross, action},
                                        {square, item.complete || item.recovery_only ? tr("Elimina", "Delete") : tr("Annulla download", "Cancel download")},
                                        {triangle, tr("Cerca", "Search")},
                                        {ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
            }
        } else {
            const UiCard* selected = nullptr;
            bool continued = false;
            if (app.view == "home" || app.view == "search") {
                const auto& rows = app.view == "home" ? app.home_rows : app.search_rows;
                if (!rows.empty()) {
                    const auto& row = rows[static_cast<std::size_t>(bounded(app.view == "home" ? app.home_row : app.search_row, rows.size()))];
                    continued = app.view == "home" && row.continue_watching;
                    if (!row.cards.empty()) selected = &row.cards[static_cast<std::size_t>(bounded(app.view == "home" ? app.home_col : app.search_col, row.cards.size()))];
                }
            } else if (app.view == "library" && !app.lib_cards.empty()) {
                selected = &app.lib_cards[static_cast<std::size_t>(bounded(app.lib_sel, app.lib_cards.size()))];
            } else if (app.view == "discover" && !app.disc_cards.empty()) {
                selected = &app.disc_cards[static_cast<std::size_t>(bounded(app.disc_sel, app.disc_cards.size()))];
            }
            const char* library = continued ? tr("Rimuovi da Continua", "Remove from Continue")
                : app.view == "library" || (selected && selected->in_library)
                    ? tr("Rimuovi dalla libreria", "Remove from library") : tr("Aggiungi alla libreria", "Add to library");
            if (app.zone == "filters")
                hint_row(c, theme, {{cross, tr("Scegli filtro", "Choose filter")}, {triangle, tr("Cerca", "Search")},
                                    {ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
            else if (selected)
                hint_row(c, theme, {{cross, tr("Dettagli", "Details")}, {square, library}, {triangle, tr("Cerca", "Search")},
                                    {ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
            else hint_row(c, theme, {{triangle, tr("Cerca", "Search")},
                                     {ui::Button::l1, tr("Sezioni", "Sections"), ui::Button::r1}});
        }
    }
    void paint_home_startup(Canvas& c) const {
        c.list.rounded_rect({0, 0, 1920, 1080}, 0, theme.page);
        const auto mark = texture("assets/icons/logo.png");
        if (mark) c.list.image(mark, {880, 325, 160, 160}, gfx::kFullUv, Color::rgb(0xffffff));
        else {
            c.list.circle(960, 405, 65, theme.primary);
            c.list.triangle({940, 374, 46, 62}, Color::rgb(0xffffff), 0, kPi * .5f);
        }
        c.list.arc(960, 565, 23, 3, time * 2, kPi * 1.45f, theme.primary);
        text(c, c.fonts.semibold, tr("Caricamento dei tuoi cataloghi", "Loading your catalogs"),
             960, 647, 31, theme.text, 1400, gfx::Align::center);
    }
    void draw_base(const App& app, Canvas& c) const {
        if (app.login_visible) { paint_login(c, app); return; }
        if (app.home_startup_visible()) { paint_home_startup(c); return; }
        if (app.directory_picker_visible) {
            paint_background(c, app);
            paint_directories(c, app);
            paint_header(c, app);
            return;
        }
        if (app.watching()) paint_player(c, app);
        else {
            paint_background(c, app);
            if (app.view == "home") paint_hero(c, app);
            c.list.push_opacity(hui::tween::clamp01(page_enter.value));
            c.list.push_transform(1, 0, 0, 0, app.ui_reduced_motion ? 0 : 12 * (1 - page_enter.value));
            if (app.view == "home") paint_shelves(c, app);
            else if (app.view == "search") {
                paint_page_title(c, tr("Cerca", "Search"), app.search_query.empty() ?
                    tr("Cerca tra i cataloghi dei tuoi add-on.", "Search your add-on catalogs.") : "\"" + app.search_query + "\"");
                paint_shelves(c, app);
            } else if (app.view == "discover") paint_discover(c, app);
            else if (app.view == "library") paint_library(c, app);
            else if (app.view == "downloads") paint_downloads(c, app);
            else if (app.view == "addons") paint_addons(c, app);
            else if (app.view == "settings") paint_settings(c, app);
            else if (app.view == "detail") paint_detail(c, app);
            c.list.pop_transform();
            c.list.pop_opacity();

            paint_header(c, app);
            c.list.gradient_rect({0, 988, 1920, 92}, 0, theme.page.with_alpha(.88f), theme.page);
        }
    }
    void draw_overlays(const App& app, Canvas& c) const {
        if (app.login_visible) { toasts.draw(c); paint_footer(c, app); return; }
        if (app.home_startup_visible()) return;
        paint_download_panel(c, app);
        if (app.watching()) paint_playback_bar(c, app);
        if (dropdown.visible()) dropdown.draw(c);
        if (tracks.visible()) tracks.draw(c);
        if (source_info.visible()) source_info.draw(c);
        if (keyboard_sheet.visible()) keyboard_sheet.draw(c);
        if (dialog.visible()) dialog.draw(c);
        if (loader.visible()) paint_launch(c, app);
        paint_next_episode(c, app);
        paint_playback_volume(c, app);
        if (directory_notice.visible()) directory_notice.draw(c);
        toasts.draw(c);
        paint_footer(c, app);
    }
    void draw(const App& app, Canvas& c) const {
        draw_base(app, c);
        draw_overlays(app, c);
    }
};

HomeUi::HomeUi(TextureLookup textures) : impl_(std::make_unique<Impl>(std::move(textures))) {}
HomeUi::~HomeUi() = default;
HomeUi::HomeUi(HomeUi&&) noexcept = default;
HomeUi& HomeUi::operator=(HomeUi&&) noexcept = default;

void HomeUi::set_texture_lookup(TextureLookup textures) { impl_->textures = std::move(textures); }
void HomeUi::set_texture_size_lookup(TextureSizeLookup dimensions) { impl_->texture_sizes = std::move(dimensions); }
void HomeUi::set_subtitle_font(const hui::ui::FontRef& font) { impl_->subtitle_font = font; }
void HomeUi::set_subtitle_fonts(const hui::ui::FontRef& sans, const hui::ui::FontRef& serif,
                                const hui::ui::FontRef& mono) {
    impl_->subtitle_font = sans;
    impl_->subtitle_serif = serif;
    impl_->subtitle_mono = mono;
}
void HomeUi::handle(App& app, const hui::InputFrame& input, hui::ui::Feedback& feedback) {
    impl_->handle(app, input, feedback);
}
void HomeUi::update(const App& app, float dt) { impl_->update(app, dt); }
void HomeUi::draw(const App& app, hui::ui::Canvas& canvas) const { impl_->draw(app, canvas); }
void HomeUi::draw_base(const App& app, hui::ui::Canvas& canvas) const { impl_->draw_base(app, canvas); }
void HomeUi::draw_overlays(const App& app, hui::ui::Canvas& canvas) const { impl_->draw_overlays(app, canvas); }
bool HomeUi::wants_glass() const {
    if (impl_->model && (impl_->model->login_visible || impl_->model->home_startup_visible())) return false;
    return (impl_->model && impl_->model->watching() && impl_->watch_alpha.value > .001f &&
            !impl_->model->menu_visible && !impl_->model->launch_visible) ||
           (impl_->model && impl_->model->next_episode_visible) ||
           (impl_->model && !impl_->model->watching() && impl_->model->view == "downloads" &&
            impl_->download_panel_transition.visible()) ||
           impl_->dropdown.visible() || impl_->tracks.visible() ||
           impl_->keyboard_sheet.visible() || impl_->dialog.visible() || impl_->directory_notice.visible() || impl_->source_info.visible();
}
void HomeUi::snap(const App& app) {
    impl_->update(app, 0);
    for (int frame = 0; frame < 40; ++frame) impl_->update(app, .05f);
    impl_->pending.clear();
}
