#pragma once

#include <map>
#include <string>
#include <vector>

// Where a caption is drawn: the row of its ASS alignment (\an / \a or its
// style). Horizontal alignment and \pos stay with the centred renderer.
enum class CuePlace { bottom, middle, top };

struct Cue {
	double start = 0, end = 0;
	std::string rml;  // escaped, with <em>/<strong> and <br/>
	CuePlace place = CuePlace::bottom;
};

// Parses SubRip, WebVTT, SubStation Alpha and MicroDVD text (UTF-8 already).
// Returns cues sorted by start time.
std::vector<Cue> parse_subtitle_file(const std::string& text);

// Turns one cue's text (SRT/VTT-style markup or ASS override tags, "\N"
// line breaks) into RML, and reads its placement from \an / \a.
struct CueText {
	std::string rml;
	CuePlace place = CuePlace::bottom;
};
CueText subtitle_text_to_cue(const std::string& text);
std::string subtitle_text_to_rml(const std::string& text);

// ASS script header ([V4+ Styles] or legacy [V4 Styles]): each style's
// alignment as a numpad value (1-9), legacy values converted.
std::map<std::string, int> ass_style_alignments(const std::string& header);

// The Text field of an ASS event, ready for subtitle_text_to_cue: comment
// blocks ({...} without a tag) and drawing-mode text (\p1 ... \p0) are
// removed, as libass hides them, and a top or middle style alignment becomes
// a leading \an tag unless the line has its own.
std::string ass_event_text(const std::string& text, const std::string& style,
                           const std::map<std::string, int>& styles);

// Text of the cues active at `t` joined with line breaks, "" when none.
// Bottom cues are plain; top and middle ones are wrapped in <sub-top> and
// <sub-middle> so one string still carries every row.
std::string cues_at(const std::vector<Cue>& cues, double t);

// Splits a cues_at() string back into its rows.
struct CaptionRows {
	std::string top, middle, bottom;
};
CaptionRows caption_rows(const std::string& rml);
