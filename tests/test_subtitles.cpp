// Subtitle text, ASS cleanup and caption placement, without a player or UI.
#include "subtitles.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void expect(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
std::map<std::string, int> no_styles;
std::string event(const std::string& text, const std::string& style = "Default",
                  const std::map<std::string, int>& styles = no_styles) {
    return subtitle_text_to_rml(ass_event_text(text, style, styles));
}
CuePlace place(const std::string& text, const std::string& style = "Default",
               const std::map<std::string, int>& styles = no_styles) {
    return subtitle_text_to_cue(ass_event_text(text, style, styles)).place;
}
}  // namespace

int main() {
    try {
        // Unchanged basics.
        expect(subtitle_text_to_rml("{\\i1}Italic{\\i0} normal") == "<em>Italic</em> normal", "italics still map to <em>");
        expect(subtitle_text_to_rml("Line one\\NLine two") == "Line one<br/>Line two", "\\N is a line break");
        expect(subtitle_text_to_rml("Tom & Jerry <3") == "Tom &amp; Jerry &lt;3", "text stays escaped");
        expect(subtitle_text_to_cue("Plain line").place == CuePlace::bottom, "plain lines stay at the bottom");

        // Comments and drawings are hidden, as libass hides them.
        expect(event("{TL note: senpai = upperclassman}Senpai!") == "Senpai!", "ASS comment blocks are hidden");
        expect(event("Hello{ inline note }there") == "Hellothere", "a comment between words is hidden");
        expect(event("{\\p1}m 0 0 l 100 0 100 100 0 100{\\p0}").empty(), "a vector drawing produces no caption");
        expect(event("{\\p1}m 0 0 l 10 0{\\p0}Sign text") == "Sign text", "text after \\p0 is shown again");
        expect(event("{\\pos(640,100)}Positioned") == "Positioned", "\\pos is not drawing mode");
        expect(event("{\\pbo10}Offset") == "Offset", "\\pbo is not drawing mode");
        expect(event("{\\i1 kept tags}Text") == "<em>Text</em>", "a block with tags still applies them");
        expect(subtitle_text_to_rml("{not a tag} in SRT") == "&#123;not a tag&#125; in SRT",
               "outside ASS, literal braces are still text");

        // Placement from override tags, the first one applying.
        expect(place("{\\an8}Top") == CuePlace::top, "\\an8 is top");
        expect(place("{\\an7}Top left") == CuePlace::top && place("{\\an9}Top right") == CuePlace::top, "\\an7 and \\an9 are top");
        expect(place("{\\an5}Middle") == CuePlace::middle && place("{\\an4}x") == CuePlace::middle, "\\an4-6 are middle");
        expect(place("{\\an2}Bottom") == CuePlace::bottom && place("{\\an1}x") == CuePlace::bottom, "\\an1-3 are bottom");
        expect(place("{\\a6}Legacy top") == CuePlace::top, "legacy \\a6 is top");
        expect(place("{\\a10}Legacy middle") == CuePlace::middle, "legacy \\a10 is middle");
        expect(place("{\\a2}Legacy bottom") == CuePlace::bottom, "legacy \\a2 is bottom");
        expect(place("{\\an8}First{\\an2}second") == CuePlace::top, "the first alignment tag applies");
        expect(place("{\\alpha&H80&\\1a&HFF&}Faded") == CuePlace::bottom, "alpha tags are not alignment");
        expect(place("{\\fad(200,200)\\an8\\pos(640,40)}Sign") == CuePlace::top, "\\an inside a longer block");
        expect(subtitle_text_to_cue("{\\an8}SRT top line").place == CuePlace::top, "SRT {\\an8} is placed too");

        // Placement from styles, with the line's own tag winning.
        const std::string header =
            "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\n"
            "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
            "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
            "MarginR, MarginV, Encoding\n"
            "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
            "Style: Top,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,8,10,10,10,1\n"
            "Style: Sign,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,5,10,10,10,1\n"
            "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        const auto styles = ass_style_alignments(header);
        expect(styles.size() == 3 && styles.at("Top") == 8 && styles.at("Sign") == 5 && styles.at("Default") == 2,
               "V4+ style alignments are read");
        expect(place("Top style", "Top", styles) == CuePlace::top, "a top style places the line at the top");
        expect(place("Sign style", "Sign", styles) == CuePlace::middle, "a middle style places the line in the middle");
        expect(place("Default style", "Default", styles) == CuePlace::bottom, "the default style stays at the bottom");
        expect(place("{\\an2}Pinned", "Top", styles) == CuePlace::bottom, "the line's own \\an beats its style");
        expect(place("Starred", "*Top", styles) == CuePlace::top, "a leading * in the style name is ignored");
        expect(place("Unknown", "Missing", styles) == CuePlace::bottom, "an unknown style falls back to Default");
        const auto legacy = ass_style_alignments(
            "[V4 Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, TertiaryColour, BackColour, "
            "Bold, Italic, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding\n"
            "Style: Top,Arial,20,65535,65535,65535,0,0,0,1,2,2,6,10,10,10,0,1\n");
        expect(legacy.at("Top") == 8, "legacy SSA style alignment 6 converts to \\an8");

        // Characters the atlas would draw as '?'.
        expect(subtitle_text_to_rml("\xE2\x80\x8BZero width") == "Zero width", "zero-width space is removed");
        expect(subtitle_text_to_rml("\xEF\xBB\xBF" "BOM line") == "BOM line", "a byte order mark is removed");
        expect(subtitle_text_to_rml("\xE2\x80\x8EMarked\xE2\x80\x8F") == "Marked", "direction marks are removed");
        expect(subtitle_text_to_rml("No\xC2\xA0" "break") == "No break", "a no-break space becomes a space");
        expect(subtitle_text_to_rml("Wide\xE3\x80\x80space") == "Wide space", "an ideographic space becomes a space");
        expect(subtitle_text_to_rml("\xE2\x99\xAA Lyrics \xE2\x99\xAA") == "\xE2\x99\xAA Lyrics \xE2\x99\xAA",
               "music notes are kept for the atlas to draw");
        expect(subtitle_text_to_rml("caf\xC3\xA9") == "caf\xC3\xA9", "ordinary accented text is untouched");

        // Rows: top and middle cues are carried separately from the bottom.
        std::vector<Cue> cues(3);
        cues[0] = {1, 5, "Dialogue", CuePlace::bottom};
        cues[1] = {2, 4, "Sign", CuePlace::top};
        cues[2] = {2, 3, "Centre", CuePlace::middle};
        const std::string both = cues_at(cues, 2.5);
        expect(both == "<sub-top>Sign</sub-top><sub-middle>Centre</sub-middle>Dialogue", "active cues are grouped by row");
        const auto rows = caption_rows(both);
        expect(rows.top == "Sign" && rows.middle == "Centre" && rows.bottom == "Dialogue", "rows split back exactly");
        expect(cues_at(cues, 4.5) == "Dialogue", "bottom-only output is unchanged from before");
        const auto plain = caption_rows("Just<br/>bottom");
        expect(plain.top.empty() && plain.middle.empty() && plain.bottom == "Just<br/>bottom", "plain text is all bottom");
        cues.push_back({1.5, 5, "Second line", CuePlace::bottom});
        expect(cues_at(cues, 4.5) == "Dialogue<br/>Second line", "overlapping bottom cues keep their order");

        // A whole file: styles, comments, drawings and Comment events.
        const std::string file = header +
            "Dialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,{\\i1}Hello{\\i0} there\n"
            "Dialogue: 0,0:00:01.00,0:00:04.00,Top,,0,0,0,,Opening song credits\n"
            "Dialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,{\\an8\\pos(640,40)\\c&H00FFFF&}Sign text\n"
            "Dialogue: 5,0:00:01.00,0:00:04.00,Sign,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100{\\p0}\n"
            "Dialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,{TL note}Senpai\xE2\x80\x8B!\n"
            "Comment: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,never shown\n";
        const auto parsed = parse_subtitle_file(file);
        expect(parsed.size() == 4, "the drawing and the Comment event produce no cue");
        const auto at = caption_rows(cues_at(parsed, 2));
        expect(at.top == "Opening song credits<br/>Sign text", "style and tag placed lines are on top");
        expect(at.bottom == "<em>Hello</em> there<br/>Senpai!", "dialogue stays at the bottom, cleaned");
        expect(at.middle.empty(), "nothing is in the middle row");
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
    std::printf("Subtitles: %d checks passed\n", checks);
    return 0;
}
