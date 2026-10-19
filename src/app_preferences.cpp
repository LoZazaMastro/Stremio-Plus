// Stremio - persistent, typed settings for the native homebrew UI.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"
#include "calendar_date.h"
#include "download_directory.h"
#include "ui_language.h"
#include "torrent/engine.h"
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <exception>

namespace {
enum Row {
    Account, Language, DateComponents, DateFormat, TorrentSpeed, DownloadDirectory, SubLangs, AutoSubs, SubSize,
    SubBackground, SubColor, SubEffect, SubFont, SubDelay,
    AudioLangs, Autoplay, NextEpisodeDelay, SeekStep, ShoulderSeekStep, Resolution, ReducedMotion, Contrast,
    ControllerLight, Sounds, Volume, Statistics, Addons, Reload, Exit,
    AudioPassthrough, DisplayRefresh, Count
};

struct LanguageChoice { const char* code; const char* italian; const char* english; };
constexpr LanguageChoice languages[] = {
    {"ita", "Italiano", "Italian"}, {"eng", "Inglese", "English"},
    {"fre", "Francese", "French"}, {"ger", "Tedesco", "German"},
    {"spa", "Spagnolo", "Spanish"}, {"por", "Portoghese", "Portuguese"},
    {"pob", "Portoghese (Brasile)", "Portuguese (Brazil)"},
    {"alb", "Albanese", "Albanian"}, {"ara", "Arabo", "Arabic"},
    {"bos", "Bosniaco", "Bosnian"}, {"bul", "Bulgaro", "Bulgarian"},
    {"cat", "Catalano", "Catalan"}, {"cze", "Ceco", "Czech"},
    {"chi", "Cinese", "Chinese"}, {"kor", "Coreano", "Korean"},
    {"hrv", "Croato", "Croatian"}, {"dan", "Danese", "Danish"},
    {"heb", "Ebraico", "Hebrew"}, {"est", "Estone", "Estonian"},
    {"fin", "Finlandese", "Finnish"}, {"jpn", "Giapponese", "Japanese"},
    {"gre", "Greco", "Greek"}, {"hin", "Hindi", "Hindi"},
    {"ind", "Indonesiano", "Indonesian"}, {"lav", "Lettone", "Latvian"},
    {"lit", "Lituano", "Lithuanian"}, {"mac", "Macedone", "Macedonian"},
    {"may", "Malese", "Malay"}, {"nor", "Norvegese", "Norwegian"},
    {"dut", "Olandese", "Dutch"}, {"per", "Persiano", "Persian"},
    {"pol", "Polacco", "Polish"}, {"rum", "Rumeno", "Romanian"},
    {"rus", "Russo", "Russian"}, {"srp", "Serbo", "Serbian"},
    {"slo", "Slovacco", "Slovak"}, {"slv", "Sloveno", "Slovenian"},
    {"swe", "Svedese", "Swedish"}, {"tha", "Thailandese", "Thai"},
    {"tur", "Turco", "Turkish"}, {"ukr", "Ucraino", "Ukrainian"},
    {"hun", "Ungherese", "Hungarian"}, {"vie", "Vietnamita", "Vietnamese"}
};
constexpr int next_episode_delays[] = {5, 10, 15, 30, 60, 120};

std::string relocation_error(const std::string& error, bool italian) {
    int code = 0;
    const auto marker = error.find("(errno ");
    if (marker != std::string::npos) code = static_cast<int>(std::strtol(error.c_str() + marker + 7, nullptr, 10));
    if (code == ENOSPC || code == EDQUOT)
        return italian ? "Spazio insufficiente. Libera spazio sull'unità e riprova in questa cartella."
                       : "Not enough space. Free space on the drive and retry in this folder.";
    if (code == EROFS)
        return italian ? "L'unità è di sola lettura. Rendila scrivibile e riprova in questa cartella."
                       : "The drive is read-only. Make it writable and retry in this folder.";
    if (code == ENOENT || code == ENODEV || code == ENXIO)
        return italian ? "L'unità o la cartella non è disponibile. Ricollega l'unità e riprova in questa cartella."
                       : "The drive or folder is unavailable. Reconnect the drive and retry in this folder.";
    if (code == EACCES || code == EPERM)
        return italian ? "Accesso alla cartella negato. Controlla i permessi e riprova in questa cartella."
                       : "Folder access was denied. Check its permissions and retry in this folder.";
    if (code == EBUSY || code == ETIMEDOUT)
        return italian ? "Il disco è ancora occupato o non ha risposto. Attendi e riprova in questa cartella."
                       : "The drive is still busy or did not respond. Wait and retry in this folder.";
    if (code == EEXIST)
        return italian ? "Un file nella destinazione è in conflitto. Controlla la cartella e riprova."
                       : "A file conflicts with the destination. Check the folder and retry.";
    if (code == ELOOP || code == ENOTDIR || code == EINVAL || code == ENAMETOOLONG)
        return italian ? "Scegli una cartella valida, senza collegamenti simbolici."
                       : "Choose a valid folder without symbolic links.";
    if (error.find("relocation is pending") != std::string::npos)
        return italian ? "Lo spostamento deve essere completato. Riprova nella stessa destinazione."
                       : "The move needs to be completed. Retry the same destination.";
    if (error.find("relocation is in progress") != std::string::npos)
        return italian ? "Spostamento in corso. Attendi il completamento."
                       : "Downloads are being moved. Wait for the move to finish.";
    return italian ? "Impossibile completare lo spostamento. Riprova in questa cartella."
                   : "Could not complete the move. Retry in this folder.";
}

std::string language_label(const std::string& code, bool italian) {
    for (const auto& lang : languages)
        if (code == lang.code) return italian ? lang.italian : lang.english;
    return language_name(code);
}

std::vector<std::string> selected_languages(const std::string& value) {
    std::vector<std::string> selected;
    for (const auto& part : split(value, ',')) {
        const auto code = language_to_iso639_2(part);
        if (code.empty() || code == "-" || std::find(selected.begin(), selected.end(), code) != selected.end()) continue;
        selected.push_back(code);
    }
    return selected;
}

std::string language_summary(const std::string& value, bool italian, bool subtitles = false) {
    const auto selected = selected_languages(value);
    if (selected.empty()) return subtitles ? (italian ? "Selezione manuale" : "Manual selection")
                                          : (italian ? "Predefinite del video" : "Video defaults");
    std::vector<std::string> names;
    for (const auto& code : selected) names.push_back(language_label(code, italian));
    return join(names, ", ");
}
}

void App::enter_settings() {
    settings_refresh();
}

void App::settings_refresh() {
    const bool italian = settings_.ui_language != "en";
    const auto t = [italian](const char* it, const char* en) { return std::string(italian ? it : en); };
    s_rows.assign(Count, UiSetting{});
    auto row = [&](int i, const char* id, std::string label, std::string hint, std::string value = "") -> UiSetting& {
        auto& r = s_rows[i]; r.id = id; r.label = std::move(label); r.hint = std::move(hint); r.value = std::move(value);
        return r;
    };
    auto toggle = [&](int i, bool enabled) {
        auto& r = s_rows[i]; r.kind = "toggle"; r.on = enabled; r.number = enabled ? 1.0f : 0.0f;
        r.value = enabled ? t("Attivo", "On") : t("Disattivo", "Off");
    };
    auto choice = [&](int i, std::vector<std::string> values, int selected) {
        auto& r = s_rows[i]; r.kind = "choice"; r.options = std::move(values); r.selected = selected;
        r.number = float(selected); r.value = r.options.at(size_t(selected));
    };
    auto slider = [&](int i, float value, float lo, float hi, float step, const char* unit) {
        auto& r = s_rows[i]; r.kind = "slider"; r.number = value; r.minimum = lo; r.maximum = hi; r.step = step;
        r.value = std::to_string(int(value)) + unit;
    };
    row(Account, "account", signed_in() ? t("Esci dall'account", "Sign out") : t("Account Stremio", "Stremio account"),
        signed_in() ? t("Disconnetti questa PS5 dal tuo account Stremio.", "Sign this PS5 out of your Stremio account.")
                    : t("Accedi dal telefono con QR o codice. Le password non si inseriscono sulla TV.", "Sign in from your phone with a QR or link code."),
        signed_in() ? settings_.user_email
                    : t("Accedi", "Sign in"));
    row(Language, "ui_language", t("Lingua dell'interfaccia", "Interface language"), "");
    choice(Language, {t("Automatico (sistema)", "Automatic (system)"), "English", "Italiano"},
        settings_.ui_language_mode == "en" ? 1 : settings_.ui_language_mode == "it" ? 2 : 0);
    row(DateComponents, "date_components", t("Elementi della data", "Date components"),
        t("Scegli giorno della settimana, giorno, mese e anno. Senza selezioni la data è nascosta.",
          "Choose weekday, day, month and year. No selections hides the date."),
        settings_.date_components ? calendar_date_now(settings_.ui_language, settings_.date_format, settings_.date_components)
                                  : t("Nascosta", "Hidden")).kind = "checklist";
    row(DateFormat, "date_format", t("Formato della data", "Date format"),
        t("Anteprima: ", "Preview: ") + calendar_date_now(settings_.ui_language, settings_.date_format,
            settings_.date_components ? settings_.date_components : kAllDateComponents));
    choice(DateFormat, {t("Breve", "Short"), t("Esteso", "Long"), t("Numerico", "Numeric"), "ISO"},
        settings_.date_format == "long" ? 1 : settings_.date_format == "numeric" ? 2 : settings_.date_format == "iso" ? 3 : 0);
    row(TorrentSpeed, "torrent_speed_profile", t("Profilo torrent", "Torrent profile"),
        t("Prestazioni del motore integrato. Ultra veloce è il profilo massimo.",
          "Built-in engine performance. Ultra fast is the maximum profile."));
    choice(TorrentSpeed, {t("Bilanciato", "Balanced"), t("Veloce", "Fast"), t("Ultra veloce", "Ultra fast")},
           std::clamp(settings_.torrent_speed_profile, 0, 2));
    row(DownloadDirectory, "download_directory", t("Cartella dei download", "Download folder"),
        t("Scegli dove salvare i video. Se cambi cartella, i download vengono spostati nella nuova destinazione.",
          "Choose where videos are saved. Changing the folder moves your downloads to the new destination."),
        settings_.download_directory.empty() ? t("Non selezionata", "Not selected") : settings_.download_directory);
    row(SubLangs, "subtitle_languages", t("Lingue dei sottotitoli", "Subtitle languages"),
        t("Le lingue selezionate sono preferite nell'ordine di scelta.", "Selected languages are preferred in the order you choose them."),
        language_summary(settings_.subtitle_langs, italian, true)).kind = "checklist";
    row(AutoSubs, "auto_subtitles", t("Sottotitoli automatici", "Automatic subtitles"),
        t("Scegli la prima traccia disponibile nella lingua preferita.", "Select the first available subtitle in your preferred language.")); toggle(AutoSubs, settings_.auto_subtitles);
    row(SubSize, "subtitle_size", t("Dimensione sottotitoli", "Subtitle size"), "");
    choice(SubSize, {t("Piccola", "Small"), t("Media", "Medium"), t("Grande", "Large")}, settings_.sub_size == "sub-s" ? 0 : settings_.sub_size == "sub-l" ? 2 : 1);
    row(SubBackground, "subtitle_background_opacity", t("Opacità sfondo sottotitoli", "Subtitle background opacity"),
        t("Da trasparente a coprente. L'anteprima si aggiorna subito.", "From transparent to opaque. The preview updates immediately."));
    slider(SubBackground, float(settings_.sub_background_opacity), 0, 100, 5, "%");
    row(SubColor, "subtitle_color", t("Colore sottotitoli", "Subtitle color"), "");
    choice(SubColor, {t("Bianco", "White"), t("Giallo", "Yellow"), t("Verde", "Green"), t("Ciano", "Cyan")},
        settings_.sub_color == "yellow" ? 1 : settings_.sub_color == "green" ? 2 : settings_.sub_color == "cyan" ? 3 : 0);
    row(SubEffect, "subtitle_effect", t("Effetto sottotitoli", "Subtitle effect"), "");
    choice(SubEffect, {t("Nessuno", "None"), t("Contorno", "Outline"), t("Ombra", "Shadow"), t("Rilievo", "Raised"), t("Inciso", "Depressed")},
        settings_.sub_effect == "none" ? 0 : settings_.sub_effect == "outline" ? 1 : settings_.sub_effect == "raised" ? 3 : settings_.sub_effect == "depressed" ? 4 : 2);
    row(SubFont, "subtitle_font", t("Font sottotitoli", "Subtitle font"), "");
    choice(SubFont, {"DejaVu Sans", "DejaVu Serif", "DejaVu Sans Mono"},
        settings_.sub_font == "serif" ? 1 : settings_.sub_font == "mono" ? 2 : 0);
    row(SubDelay, "subtitle_offset_ms", t("Sincronizzazione sottotitoli", "Subtitle timing"),
        t("Valori positivi ritardano il testo. Regolabile anche durante la riproduzione.", "Positive values delay subtitles. Also adjustable during playback."));
    slider(SubDelay, float(settings_.subtitle_offset_ms), -10000, 10000, 250, " ms");
    row(AudioLangs, "audio_languages", t("Lingue audio", "Audio languages"),
        t("Le lingue selezionate sono preferite nell'ordine di scelta. Senza selezioni viene usata la traccia predefinita.",
          "Selected languages are preferred in the order you choose them. No selection uses the default track."),
        language_summary(settings_.audio_langs, italian)).kind = "checklist";
    row(Autoplay, "autoplay_next", t("Prossimo episodio automatico", "Autoplay next episode"),
        t("Mostra il prossimo episodio al termine del video, con un conto alla rovescia.",
          "Show the next episode when a video ends, with a countdown.")); toggle(Autoplay, settings_.autoplay_next);
    row(NextEpisodeDelay, "next_episode_delay_seconds", t("Attesa per il prossimo episodio", "Next episode countdown"),
        t("Tempo disponibile per riprodurre subito o ignorare il prossimo episodio.",
          "Time to choose Watch now or Ignore before the next episode starts."));
    int delay_choice = 2;
    for (int i = 0; i < 6; ++i) if (settings_.next_episode_delay_seconds == next_episode_delays[i]) delay_choice = i;
    choice(NextEpisodeDelay, {"5 s", "10 s", "15 s", "30 s", "60 s", "120 s"}, delay_choice);
    s_rows[NextEpisodeDelay].enabled = settings_.autoplay_next;
    row(SeekStep, "seek_seconds", t("Salto con la croce direzionale", "Directional seek interval"),
        t("Durata di ciascun salto avanti o indietro nel video.", "Time advanced or rewound by each directional seek.")); slider(SeekStep, float(settings_.seek_seconds), 5, 60, 5, " s");
    row(ShoulderSeekStep, "shoulder_seek_seconds", t("Salto con L1 / R1", "L1 / R1 seek interval"),
        t("Durata indipendente dal salto con la croce direzionale.", "Independent from the directional seek interval."));
    slider(ShoulderSeekStep, float(settings_.shoulder_seek_seconds), 5, 300, 5, " s");
    row(Resolution, "display_resolution", t("Risoluzione di uscita", "Output resolution"),
        t("Automatico segue la risoluzione della PS5. Si applica al prossimo avvio; 1080p se non disponibile. Uscita SDR.",
          "Automatic follows the PS5 resolution. Applies next launch; 1080p if unavailable. SDR output."));
    choice(Resolution, {t("Automatico (PS5)", "Automatic (PS5)"), "1920 × 1080", "2560 × 1440", "3840 × 2160"},
           std::clamp(settings_.display_resolution + 1, 0, 3));
    row(ReducedMotion, "reduced_motion", t("Riduci animazioni", "Reduce motion"), t("Movimenti ridotti per focus e transizioni.", "Reduce movement in focus and transitions.")); toggle(ReducedMotion, settings_.reduced_motion);
    row(Contrast, "high_contrast", t("Contrasto elevato", "High contrast"), t("Testo e selezioni più leggibili.", "Increase text and focus contrast.")); toggle(Contrast, settings_.high_contrast);
    row(ControllerLight, "controller_ambient_light", t("Colore DualSense dal titolo", "DualSense title color"),
        t("La luce del controller riprende i colori dominanti della copertina. Disattiva per usare il viola Stremio.",
          "The controller light follows the cover's dominant colors. Turn off to use Stremio purple."));
    toggle(ControllerLight, settings_.controller_ambient_light);
    row(Sounds, "ui_sounds", t("Suoni dell'interfaccia", "Interface sounds"), t("Conferme sonore durante la navigazione.", "Audio feedback for navigation and actions.")); toggle(Sounds, settings_.ui_sounds);
    row(Volume, "sound_volume", t("Volume dell'interfaccia", "Interface volume"), t("Regola i suoni dei menu.", "Adjust menu sound volume.")); slider(Volume, float(settings_.sound_volume), 0, 100, 5, "%");
    row(Statistics, "show_stats", t("Statistiche di riproduzione", "Playback statistics"), t("Decoder, frame, buffer e fotogrammi scartati.", "Decoder, frame rate, buffer and dropped frames.")); toggle(Statistics, settings_.show_stats);
    row(Addons, "addons", t("Add-on", "Add-ons"),
        t("Gli add-on sincronizzati con il tuo account Stremio.", "Add-ons synchronized with your Stremio account."));
    row(Reload, "reload", t("Aggiorna add-on e cataloghi", "Refresh add-ons and catalogs"), "");
    row(Exit, "exit", t("Esci da Stremio", "Exit Stremio"), "");
    row(AudioPassthrough, "audio_passthrough", "Audio passthrough",
        t("Invia i formati audio compatibili al ricevitore HDMI. Se non disponibile, usa PCM. Si applica al prossimo video.",
          "Send supported audio formats to the HDMI receiver. Use PCM when unavailable. Applies to the next video."));
    toggle(AudioPassthrough, settings_.audio_passthrough);
    row(DisplayRefresh, "display_refresh", t("Uscita 120 Hz", "120 Hz output"),
        t("Automatico abilita l'uscita ad alta frequenza quando supportata. Solo riproduzione ripristina la frequenza di sistema nei menu.",
          "Auto enables supported high refresh output. Playback only restores the system refresh rate in menus."));
    choice(DisplayRefresh, {t("Disattivata", "Off"), t("Automatico", "Auto"), t("Solo riproduzione", "Playback only")},
        settings_.display_refresh);
    s_sel = std::clamp(s_sel, 0, Count - 1);
    dirty_all();
}

void App::set_setting_value(int row, float value) {
    if (!std::isfinite(value)) return;
    const int n = int(std::lround(value));
    switch (row) {
    case Language:
        settings_.ui_language_mode = n == 1 ? "en" : n == 2 ? "it" : "auto";
        settings_.ui_language = resolve_ui_language(settings_.ui_language_mode, system_ui_language_); break;
    case DateFormat: { const char* values[] = {"short", "long", "numeric", "iso"}; settings_.date_format = values[std::clamp(n, 0, 3)]; break; }
    case TorrentSpeed:
        settings_.torrent_speed_profile = std::clamp(n, 0, 2);
        bt::Engine::get().set_speed_profile(static_cast<bt::SpeedProfile>(settings_.torrent_speed_profile));
        break;
    case AutoSubs: settings_.auto_subtitles = n != 0; break;
    case SubSize: settings_.sub_size = n <= 0 ? "sub-s" : n >= 2 ? "sub-l" : "sub-m"; w_sub_size = settings_.sub_size; break;
    case SubBackground: settings_.sub_background_opacity = std::clamp(n, 0, 100); break;
    case SubColor: { const char* values[] = {"white", "yellow", "green", "cyan"}; settings_.sub_color = values[std::clamp(n, 0, 3)]; break; }
    case SubEffect: { const char* values[] = {"none", "outline", "shadow", "raised", "depressed"}; settings_.sub_effect = values[std::clamp(n, 0, 4)]; break; }
    case SubFont: { const char* values[] = {"sans", "serif", "mono"}; settings_.sub_font = values[std::clamp(n, 0, 2)]; break; }
    case SubDelay: settings_.subtitle_offset_ms = std::clamp(n, -10000, 10000); w_sub_delay_ = settings_.subtitle_offset_ms / 1000.0; break;
    case Autoplay: settings_.autoplay_next = n != 0; break;
    case NextEpisodeDelay: settings_.next_episode_delay_seconds = next_episode_delays[std::clamp(n, 0, 5)]; break;
    case SeekStep: settings_.seek_seconds = std::clamp(n, 5, 60); break;
    case ShoulderSeekStep: settings_.shoulder_seek_seconds = std::clamp(n, 5, 300); break;
    case Resolution: settings_.display_resolution = std::clamp(n, 0, 3) - 1; break;
    case ReducedMotion: settings_.reduced_motion = n != 0; break;
    case Contrast: settings_.high_contrast = n != 0; break;
    case ControllerLight: settings_.controller_ambient_light = n != 0; break;
    case Sounds: settings_.ui_sounds = n != 0; break;
    case Volume: settings_.sound_volume = std::clamp(n, 0, 100); break;
    case Statistics: settings_.show_stats = n != 0; break;
    case AudioPassthrough: settings_.audio_passthrough = n != 0; break;
    case DisplayRefresh: settings_.display_refresh = std::clamp(n, 0, 2); break;
    default: return;
    }
    save_settings(); settings_refresh();
}

void App::settings_button(Btn button) {
    if (button == Btn::Up) { if (s_sel > 0) --s_sel; }
    else if (button == Btn::Down) { if (s_sel + 1 < Count) ++s_sel; }
    else if (button == Btn::Circle || button == Btn::Left) { zone = "content"; }
    else if (button == Btn::Cross && s_sel >= 0 && s_sel < int(s_rows.size())) {
        const auto setting = s_rows[s_sel];
        if (setting.kind == "toggle") set_setting_value(s_sel, setting.on ? 0 : 1);
        else if (setting.kind == "choice") {
            const int row = s_sel;
            open_dropdown(setting.label, setting.options, setting.selected, [this, row](int i) { set_setting_value(row, float(i)); });
        } else switch (s_sel) {
        case Account:
            if (!signed_in()) login_start();
            else {
                const bool italian = settings_.ui_language != "en";
                open_dropdown(italian ? "Uscire dall'account Stremio?" : "Sign out of Stremio?",
                    {italian ? "Annulla" : "Cancel", italian ? "Esci dall'account" : "Sign out"},
                    0, [this](int selected) { if (selected == 1) sign_out(); });
            }
            break;
        case SubLangs:
        case AudioLangs: open_language_checklist(s_sel == AudioLangs); break;
        case DateComponents: open_date_checklist(); break;
        case DownloadDirectory: open_download_directory_picker(); break;
        case Addons: set_view("addons"); break;
        case Reload: load_addons(); if (signed_in()) load_library(); break;
        case Exit: exit_ = true; break;
        default: break;
        }
    }
    settings_refresh();
}

void App::open_download_directory_picker() {
    directory_picker_visible = true;
    directory_picker_shutdown_ = false;
    if (directory_picker_committing) { dirty_all(); return; }
    directory_picker_path.clear();
    directory_picker_entries.clear();
    directory_picker_sel = 0;
    const auto relocation = downloads_.relocation_status();
    const bool resuming = relocation.pending && !relocation.destination.empty();
    auto path = resuming ? relocation.destination : downloads_.download_directory();
    if (path.empty()) path = settings_.download_directory;
#ifdef PLATFORM_PS5_NATIVE
    if (path.empty()) path = "/mnt";
#else
    if (path.empty()) path = data_dir_;
#endif
    browse_download_directory(path, !resuming);
}

void App::close_download_directory_picker(bool shutting_down) {
    if (directory_picker_committing && !shutting_down) return;
    directory_picker_visible = false;
    directory_picker_shutdown_ = shutting_down;
    if (directory_picker_cancel_) directory_picker_cancel_->store(true);
    ++directory_picker_generation_;
    directory_picker_loading = false;
    dirty_all();
}

bool App::download_relocation_active() const {
    return directory_picker_committing || downloads_.relocation_status().active;
}

void App::directory_picker_tick() {
    if (!directory_picker_committing || directory_picker_shutdown_) return;
    const auto progress = downloads_.relocation_status();
    if (!progress.active) return;
    const bool italian = ui_language == "it";
    const std::string status = progress.phase == "moving"
        ? (italian ? "Spostamento dei download…" : "Moving downloads…")
        : progress.phase == "verifying"
            ? (italian ? "Verifica dei download spostati…" : "Verifying moved downloads…")
        : progress.phase == "saving"
            ? (italian ? "Completamento dello spostamento…" : "Finishing the move…")
            : (italian ? "Preparazione dello spostamento…" : "Preparing to move downloads…");
    const auto total = progress.phase == "preparing" && progress.bytes_total <= 0
        ? int64_t(-1) : std::max<int64_t>(0, progress.bytes_total);
    const auto done = std::max<int64_t>(0, progress.bytes_done);
    const int count = std::max(0, progress.files_total);
    const int finished = std::clamp(progress.files_done, 0, count);
    if (directory_picker_status == status && directory_move_title == progress.title &&
        directory_move_done == done && directory_move_total == total &&
        directory_move_items_done == finished && directory_move_items_total == count) return;
    directory_picker_status = status;
    directory_move_title = progress.title;
    directory_move_done = done;
    directory_move_total = total;
    directory_move_items_done = finished;
    directory_move_items_total = count;
    dirty_all();
}

void App::browse_download_directory(const std::string& path, bool nearest, const std::string& focus) {
    if (directory_picker_committing) return;
    if (directory_picker_cancel_) directory_picker_cancel_->store(true);
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    directory_picker_cancel_ = cancel;
    const int generation = ++directory_picker_generation_;
    directory_picker_loading = true;
    directory_picker_error = false;
    directory_picker_status = ui_language == "it" ? "Lettura delle cartelle…" : "Reading folders…";
    dirty_all();
    g_tasks.run<download_directory::Listing>([path, nearest, cancel]() {
        auto current = download_directory::normalize(path);
        if (current.empty()) current = "/";
        auto result = download_directory::list(current, cancel);
        while (nearest && result.error && current != "/" && !cancel->load()) {
            current = download_directory::parent(current);
            result = download_directory::list(current, cancel);
        }
        return result;
    }, [this, generation, focus](download_directory::Listing& result) {
        if (generation != directory_picker_generation_ || !directory_picker_visible || directory_picker_shutdown_) return;
        directory_picker_loading = false;
        if (result.error) {
            directory_picker_error = true;
            directory_picker_status = download_directory::error_text(result.error, ui_language == "it");
            if (directory_picker_path.empty()) directory_picker_path = result.path.empty() ? "/" : result.path;
            dirty_all();
            return;
        }
        directory_picker_path = result.path;
        directory_picker_entries = std::move(result.folders);
        if (directory_picker_path != "/") directory_picker_entries.insert(directory_picker_entries.begin(), "..");
        const auto selected = std::find(directory_picker_entries.begin(), directory_picker_entries.end(), focus);
        directory_picker_sel = selected == directory_picker_entries.end() ? 0
            : static_cast<int>(selected - directory_picker_entries.begin());
        directory_picker_error = false;
        directory_picker_status = result.limited
            ? (ui_language == "it" ? "Elenco molto grande: apri una sottocartella per restringere la scelta."
                                    : "This folder is very large. Open a subfolder to narrow the list.")
            : "";
        const auto relocation = downloads_.relocation_status();
        if (!result.limited && relocation.pending && result.path == relocation.destination)
            directory_picker_status = ui_language == "it"
                ? "Conferma questa cartella per completare lo spostamento interrotto."
                : "Confirm this folder to finish the interrupted move.";
        dirty_all();
    });
}

void App::directory_picker_button(Btn button) {
    if (!directory_picker_visible) return;
    if (directory_picker_committing) return;
    if (button == Btn::Circle) { close_download_directory_picker(); return; }
    if (button == Btn::Options) {
        browse_download_directory(directory_picker_path.empty() ? "/" : directory_picker_path);
        return;
    }
    if (directory_picker_loading) return;
    const int count = static_cast<int>(directory_picker_entries.size());
    if (button == Btn::Up || button == Btn::Down || button == Btn::L1 || button == Btn::R1) {
        const int delta = button == Btn::Up ? -1 : button == Btn::Down ? 1 : button == Btn::L1 ? -6 : 6;
        directory_picker_sel = std::clamp(directory_picker_sel + delta, 0, std::max(0, count - 1));
        dirty_all();
    } else if (button == Btn::Left && directory_picker_path != "/") {
        const auto slash = directory_picker_path.find_last_of('/');
        browse_download_directory(download_directory::parent(directory_picker_path), false,
                                  directory_picker_path.substr(slash + 1));
    } else if (button == Btn::Cross && count > 0) {
        const auto entry = directory_picker_entries[static_cast<std::size_t>(std::clamp(directory_picker_sel, 0, count - 1))];
        if (entry == "..") {
            const auto slash = directory_picker_path.find_last_of('/');
            browse_download_directory(download_directory::parent(directory_picker_path), false,
                                      directory_picker_path.substr(slash + 1));
        } else browse_download_directory(download_directory::child(directory_picker_path, entry));
    } else if (button == Btn::Square && !directory_picker_path.empty()) {
        const auto path = directory_picker_path;
        const int generation = ++directory_picker_generation_;
        const auto cancel = std::make_shared<std::atomic<bool>>(false);
        directory_picker_cancel_ = cancel;
        directory_picker_loading = true;
        directory_picker_error = false;
        directory_picker_status = ui_language == "it" ? "Creazione della cartella…" : "Creating folder…";
        dirty_all();
        g_tasks.run<download_directory::Creation>([path, cancel]() {
            if (cancel->load()) return download_directory::Creation{};
            return download_directory::create(path);
        }, [this, generation](download_directory::Creation& result) {
            if (generation != directory_picker_generation_ || !directory_picker_visible || directory_picker_shutdown_) return;
            directory_picker_loading = false;
            if (result.error || result.path.empty()) {
                directory_picker_error = true;
                directory_picker_status = download_directory::error_text(result.error, ui_language == "it");
                dirty_all();
                return;
            }
            browse_download_directory(result.path);
        });
    } else if (button == Btn::Triangle && !directory_picker_path.empty()) {
        struct Result { bool ok = false; std::string error, path; };
        const auto path = directory_picker_path;
        if (launch_cancel_) launch_cancel_->store(true);
        autoplay_pending_ = false;
        if (watching_ || launch_visible) watch_stop(false);
        else watch_cancel_next_episode();
        directory_picker_committing = true;
        directory_picker_error = false;
        directory_move_done = 0;
        directory_move_total = -1;
        directory_move_items_done = directory_move_items_total = 0;
        directory_move_title.clear();
        directory_picker_status = ui_language == "it" ? "Preparazione dello spostamento…" : "Preparing to move downloads…";
        dirty_all();
        g_tasks.run<Result>([this, path]() {
            Result result;
            try {
                result.ok = downloads_.set_download_directory(path, result.error);
            } catch (const std::exception& error) {
                dlog("Download relocation raised an exception: %s", error.what());
                result.error = "The download move was interrupted. Retry the same destination.";
            } catch (...) {
                dlog("Download relocation raised an unknown exception");
                result.error = "The download move was interrupted. Retry the same destination.";
            }
            result.path = downloads_.download_directory();
            return result;
        }, [this](Result& result) {
            directory_picker_committing = false;
            if (directory_picker_shutdown_) return;
            // The registry is authoritative even after a partially completed
            // move. Refresh the inventory before reporting any failure.
            settings_.download_directory = result.path;
            save_settings();
            settings_refresh();
            downloads_refresh();
            if (result.ok) {
                if (directory_picker_visible) close_download_directory_picker();
                show_toast(ui_language == "it" ? "Cartella dei download aggiornata" : "Download folder updated");
            } else {
                directory_picker_error = true;
                dlog("Download directory change failed: %s", result.error.c_str());
                directory_picker_status = relocation_error(result.error, ui_language == "it");
                if (!directory_picker_visible) show_toast(directory_picker_status, 8);
                dirty_all();
            }
        });
    }
}

void App::open_language_checklist(bool audio) {
    const bool italian = ui_language != "en";
    const auto selected = selected_languages(audio ? settings_.audio_langs : settings_.subtitle_langs);
    // Keep configured preferences first, including uncommon provider codes;
    // opening and closing the picker never silently removes a preference.
    auto codes = selected;
    for (const auto& lang : languages)
        if (std::find(codes.begin(), codes.end(), lang.code) == codes.end()) codes.emplace_back(lang.code);
    std::vector<std::string> labels;
    for (const auto& code : codes) labels.push_back(language_label(code, italian));
    open_dropdown(audio ? (italian ? "Lingue audio" : "Audio languages")
                        : (italian ? "Lingue dei sottotitoli" : "Subtitle languages"),
        labels, 0, [this, audio, codes](int index) {
            if (index < 0 || size_t(index) >= codes.size()) return;
            auto& value = audio ? settings_.audio_langs : settings_.subtitle_langs;
            auto selected = selected_languages(value);
            const auto it = std::find(selected.begin(), selected.end(), codes[index]);
            if (it == selected.end()) selected.push_back(codes[index]);
            else selected.erase(it);
            value = join(selected, ", ");
            for (size_t i = 0; i < dd_options.size() && i < codes.size(); ++i)
                dd_options[i].active = std::find(selected.begin(), selected.end(), codes[i]) != selected.end();
            save_settings(); settings_refresh();
        });
    dd_multiselect = true;
    for (size_t i = 0; i < dd_options.size(); ++i) {
        dd_options[i].active = std::find(selected.begin(), selected.end(), codes[i]) != selected.end();
        dd_options[i].lang = codes[i];
    }
    dirty_all();
}

void App::open_date_checklist() {
    const bool italian = settings_.ui_language != "en";
    const std::vector<std::string> labels = italian
        ? std::vector<std::string>{"Giorno della settimana", "Giorno", "Mese", "Anno"}
        : std::vector<std::string>{"Weekday", "Day", "Month", "Year"};
    open_dropdown(italian ? "Elementi della data" : "Date components", labels, 0, [this](int index) {
        if (index < 0 || index > 3) return;
        settings_.date_components ^= 1 << index;
        for (size_t i = 0; i < dd_options.size(); ++i)
            dd_options[i].active = (settings_.date_components & (1 << i)) != 0;
        save_settings(); settings_refresh();
    });
    dd_multiselect = true;
    for (size_t i = 0; i < dd_options.size(); ++i) {
        dd_options[i].active = (settings_.date_components & (1 << i)) != 0;
        dd_options[i].lang.clear();
    }
    dirty_all();
}
