// Stremio PS5 - Enrich a settled catalog selection with provider artwork.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"
#include "http.h"
#include <algorithm>

namespace {
std::string metadata_key(const Item& item) {
    return item.type + "\n" + item.id + "\n" + item.metadata_addon_url;
}
void fill_missing(Item& item, const Item& data) {
    if (item.logo.empty()) item.logo = data.logo;
    if (item.background.empty()) item.background = data.background;
    if (item.poster.empty()) item.poster = data.poster;
    if (item.description.empty()) item.description = data.description;
    if (item.release_info.empty()) item.release_info = data.release_info;
    if (item.runtime.empty()) item.runtime = data.runtime;
    if (item.imdb_rating.empty()) item.imdb_rating = data.imdb_rating;
    if (item.genres.empty()) item.genres = data.genres;
    if (item.cast.empty()) item.cast = data.cast;
    if (item.directors.empty()) item.directors = data.directors;
}

std::string bounded_text(const json& object, const char* key, size_t limit, bool is_url = false) {
    auto field = object.find(key);
    if (field == object.end() || !field->is_string()) return {};
    const auto& text = field->get_ref<const std::string&>();
    if (text.size() <= limit) return text;
    if (is_url) return {}; // Truncating a signed URL would create a different resource.
    size_t length = limit;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
    return text.substr(0, length);
}

Item artwork_metadata(const json& meta) {
    Item item;
    item.logo = bounded_text(meta, "logo", 8192, true);
    item.background = bounded_text(meta, "background", 8192, true);
    item.poster = bounded_text(meta, "poster", 8192, true);
    item.description = bounded_text(meta, "description", 16384);
    item.release_info = bounded_text(meta, "releaseInfo", 256);
    if (item.release_info.empty()) item.release_info = bounded_text(meta, "year", 256);
    item.runtime = bounded_text(meta, "runtime", 64);
    item.imdb_rating = bounded_text(meta, "imdbRating", 32);
    const auto list = [&](const char* key, size_t limit) {
        std::vector<std::string> values;
        const auto& input = jobj(meta, key);
        if (input.is_string()) {
            const auto value = bounded_text(meta, key, 256);
            if (!value.empty()) values.push_back(value);
        } else if (input.is_array()) {
            for (const auto& value : input) {
                if (values.size() == limit) break;
                if (value.is_string()) {
                    const auto& text = value.get_ref<const std::string&>();
                    if (!text.empty() && text.size() <= 256) values.push_back(text);
                }
            }
        }
        return values;
    };
    item.genres = list("genres", 8);
    item.cast = list("cast", 5);
    item.directors = list("director", 4);
    return item;
}

bool preview_complete(const Item& item, bool discover) {
    if (discover)
        return !item.logo.empty() && !item.description.empty() && !item.runtime.empty() &&
               !item.genres.empty() && !item.cast.empty() && !item.directors.empty();
    return !item.logo.empty() && !item.background.empty();
}
}

void App::update_preview_metadata() {
    Item* selected = nullptr;
    if (!offline_ && !login_visible && !watching_ && !launch_visible) {
        if (view == "home" || view == "search") {
            auto& rows = view == "home" ? board_ : search_;
            const auto& mapping = view == "home" ? home_map_ : search_map_;
            const int row = view == "home" ? home_row : search_row;
            const int col = view == "home" ? home_col : search_col;
            if (row >= 0 && row < int(mapping.size()) && mapping[row] >= 0 && mapping[row] < int(rows.size())) {
                auto& items = rows[mapping[row]].items;
                if (col >= 0 && col < int(items.size())) selected = &items[col];
            }
        } else if (view == "discover" && disc_sel >= 0 && disc_sel < int(disc_items_.size())) {
            selected = &disc_items_[disc_sel];
        }
    }
    const auto key = selected ? metadata_key(*selected) : std::string();
    const double now = now_seconds();
    if (key != preview_metadata_key_) {
        if (preview_metadata_cancel_) preview_metadata_cancel_->store(true);
        ++preview_metadata_generation_;
        preview_metadata_key_ = key;
        preview_metadata_selected_at_ = now;
        preview_metadata_loading_ = false;
    }
    if (!selected || selected->id.empty() || selected->type.empty() || selected->id.size() > 8192 ||
        selected->type.size() > 256 || selected->metadata_addon_url.size() > 65536) return;
    auto cached = preview_metadata_.find(key);
    if (cached != preview_metadata_.end()) {
        const bool artwork_changed = !preview_complete(*selected, view == "discover");
        fill_missing(*selected, cached->second.item);
        cached->second.used_at = now;
        if (artwork_changed && view == "discover") discover_preview();
        if (now < cached->second.retry_at) return;
    }
    if (preview_complete(*selected, view == "discover") || preview_metadata_loading_ ||
        now - preview_metadata_selected_at_ < .24) return;

    struct Provider { std::string url, identity; };
    std::vector<Provider> providers;
    for (const auto& addon : addons_) {
        if (addon->supports("meta", selected->type, selected->id))
            providers.push_back({addon->resource_url("meta", selected->type, selected->id), addon->transport_url});
    }
    std::stable_partition(providers.begin(), providers.end(), [&](const Provider& provider) {
        return provider.identity == selected->metadata_addon_url;
    });
    // A preview never exhausts a long provider list while the user browses.
    // The detail page still uses its complete metadata resolution path.
    if (providers.size() > 3) providers.resize(3);
    if (providers.empty()) return;
    // A catalog entry may contain a large inline episode list. Preview
    // enrichment only needs its presentation fields, never a copy of that JSON.
    Item original;
    fill_missing(original, *selected);
    const int generation = preview_metadata_generation_, account = account_generation_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    preview_metadata_cancel_ = cancel;
    preview_metadata_loading_ = true;
    const bool discover = view == "discover";
    g_tasks.run<Item>([providers, original = std::move(original), cancel, discover] {
        Item result;
        Item combined = original;
        for (const auto& provider : providers) {
            if (cancel->load()) break;
            json document;
            std::string error;
            if (!fetch_json(provider.url, document, error, 6, cancel.get())) continue;
            const auto& meta = jobj(document, "meta");
            if (!meta.is_object() || meta.empty()) continue;
            fill_missing(result, artwork_metadata(meta));
            fill_missing(combined, result);
            if (preview_complete(combined, discover)) break;
        }
        return result;
    }, [this, key, generation, account, cancel](Item& result) {
        if (cancel->load() || generation != preview_metadata_generation_ || account != account_generation_) return;
        preview_metadata_loading_ = false;
        const double completed = now_seconds();
        if (preview_metadata_.size() >= 96 && !preview_metadata_.count(key)) {
            auto oldest = std::min_element(preview_metadata_.begin(), preview_metadata_.end(),
                [](const auto& a, const auto& b) { return a.second.used_at < b.second.used_at; });
            if (oldest != preview_metadata_.end()) preview_metadata_.erase(oldest);
        }
        preview_metadata_[key] = {result, completed + 300, completed};
        auto apply = [&](Item& item) { if (metadata_key(item) == key) fill_missing(item, result); };
        for (auto& row : board_) for (auto& item : row.items) apply(item);
        for (auto& row : search_) for (auto& item : row.items) apply(item);
        for (auto& item : disc_items_) apply(item);
        if (view == "discover") discover_preview();
        else refresh_home_preview();
        dirty_all();
    });
}
