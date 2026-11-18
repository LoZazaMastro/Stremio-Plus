#include "subtitles.h"

#include <algorithm>
#include <cstdlib>

#include "bidi.h"
#include "util.h"

// "00:01:02,345" / "01:02.345" / "0:01:02.34" -> seconds; -1 if not a time.
static double parse_timestamp(const std::string& s) {
	std::vector<double> parts;
	std::string cur;
	for (char c : s) {
		if (c == ':') {
			if (cur.empty()) return -1;
			parts.push_back(atof(cur.c_str()));
			cur.clear();
		} else if (isdigit((unsigned char)c) || c == '.' || c == ',') {
			cur += (c == ',') ? '.' : c;
		} else {
			return -1;
		}
	}
	if (cur.empty()) return -1;
	parts.push_back(atof(cur.c_str()));
	if (parts.size() < 2 || parts.size() > 3) return -1;
	double t = 0;
	for (double p : parts) t = t * 60 + p;
	return t;
}

// Legacy SSA \a values (1-3 bottom, +4 top, +8 middle) as numpad values.
static int legacy_alignment(int a) {
	if (a >= 1 && a <= 3) return a;
	if (a >= 5 && a <= 7) return a + 2;
	if (a >= 9 && a <= 11) return a - 5;
	return 0;
}

// The first \an<1-9> or legacy \a<n> in override tags as a numpad value, 0 if
// none. \alpha and the numbered \1a..\4a alpha tags are not alignment.
static int tag_alignment(const std::string& tags) {
	for (size_t j = tags.find('\\'); j != std::string::npos; j = tags.find('\\', j + 1)) {
		if (tags.compare(j + 1, 2, "an") == 0 && j + 3 < tags.size() && tags[j + 3] >= '1' && tags[j + 3] <= '9')
			return tags[j + 3] - '0';
		if (j + 2 < tags.size() && tags[j + 1] == 'a' && isdigit((unsigned char)tags[j + 2]))
			return legacy_alignment(atoi(tags.c_str() + j + 2));
	}
	return 0;
}

static CuePlace place_of(int numpad) {
	return numpad >= 7 ? CuePlace::top : numpad >= 4 ? CuePlace::middle : CuePlace::bottom;
}

// Characters the subtitle atlas would draw as '?' although nothing should
// show: zero-width and direction marks vanish (after bidi reordering, which
// is the only thing they affect) and unusual spaces become plain spaces.
static std::string drop_invisible(const std::string& in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size();) {
		const auto b = [&](size_t k) { return (unsigned char)in[i + k]; };
		if (b(0) == 0xC2 && i + 1 < in.size() && b(1) == 0xA0) {  // U+00A0
			out += ' ';
			i += 2;
			continue;
		}
		if (i + 2 < in.size()) {
			const unsigned cp = ((b(0) & 0x0Fu) << 12) | ((b(1) & 0x3Fu) << 6) | (b(2) & 0x3Fu);
			if ((b(0) & 0xF0) == 0xE0 && (b(1) & 0xC0) == 0x80 && (b(2) & 0xC0) == 0x80) {
				if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
				    (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF) {
					i += 3;
					continue;
				}
				if (cp == 0x2007 || cp == 0x202F || cp == 0x3000) {
					out += ' ';
					i += 3;
					continue;
				}
				if (cp == 0x2028 || cp == 0x2029) {
					out += "<br/>";
					i += 3;
					continue;
				}
			}
		}
		out += in[i++];
	}
	return out;
}

std::string subtitle_text_to_rml(const std::string& in) { return subtitle_text_to_cue(in).rml; }

CueText subtitle_text_to_cue(const std::string& in) {
	std::string out;
	bool italic = false, bold = false;
	int alignment = 0;
	size_t i = 0;
	while (i < in.size()) {
		char c = in[i];
		if (c == '{' && i + 1 < in.size() && in[i + 1] == '\\') {
			// ASS override block: honour italics/bold, drop the rest.
			size_t end = in.find('}', i);
			if (end == std::string::npos) break;
			std::string tags = in.substr(i, end - i);
			if (!alignment) alignment = tag_alignment(tags);  // the first one applies, as in libass
			if (tags.find("\\i1") != std::string::npos && !italic) out += "<em>", italic = true;
			if (tags.find("\\i0") != std::string::npos && italic) out += "</em>", italic = false;
			if (tags.find("\\b1") != std::string::npos && !bold) out += "<strong>", bold = true;
			if (tags.find("\\b0") != std::string::npos && bold) out += "</strong>", bold = false;
			i = end + 1;
			continue;
		}
		if (c == '{' && i + 1 < in.size() && in[i + 1] == 'y' && in.find('}', i) != std::string::npos) {
			// MicroDVD {y:i}
			size_t end = in.find('}', i);
			if (in.compare(i, 4, "{y:i") == 0 && !italic) out += "<em>", italic = true;
			i = end + 1;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && (in[i + 1] == 'N' || in[i + 1] == 'n')) {
			out += "<br/>";
			i += 2;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && in[i + 1] == 'h') {
			out += ' ';
			i += 2;
			continue;
		}
		if (c == '<') {
			size_t end = in.find('>', i);
			if (end != std::string::npos) {
				std::string tag = lower(in.substr(i + 1, end - i - 1));
				if (tag == "i" && !italic) out += "<em>", italic = true;
				else if (tag == "/i" && italic) out += "</em>", italic = false;
				else if (tag == "b" && !bold) out += "<strong>", bold = true;
				else if (tag == "/b" && bold) out += "</strong>", bold = false;
				else if (tag == "br" || tag == "br/" || tag == "br /") out += "<br/>";
				// <font>, <u>, <c.yellow>, <v Name>, VTT timestamps: dropped.
				i = end + 1;
				continue;
			}
		}
		if (c == '\r') {
			i++;
			continue;
		}
		if (c == '\n') {
			out += "<br/>";
			i++;
			continue;
		}
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '{': out += "&#123;"; break;
		case '}': out += "&#125;"; break;
		default: out += c;
		}
		i++;
	}
	if (bold) out += "</strong>";
	if (italic) out += "</em>";
	// Collapse leading/trailing breaks.
	while (starts_with(out, "<br/>")) out = out.substr(5);
	while (ends_with(out, "<br/>")) out = out.substr(0, out.size() - 5);
	// Arabic / Hebrew lines into display order, letters joined (bidi.h).
	CueText cue;
	cue.rml = drop_invisible(bidi_visual_rml(out));
	while (starts_with(cue.rml, "<br/>")) cue.rml = cue.rml.substr(5);
	while (ends_with(cue.rml, "<br/>")) cue.rml = cue.rml.substr(0, cue.rml.size() - 5);
	cue.place = place_of(alignment);
	return cue;
}

static std::string style_key(const std::string& name) {
	std::string key = trim(name);
	while (!key.empty() && key.front() == '*') key.erase(key.begin());  // libass ignores leading '*'
	return key;
}

std::map<std::string, int> ass_style_alignments(const std::string& header) {
	std::map<std::string, int> styles;
	enum { none, modern, legacy } section = none;
	int name_field = 0, align_field = 18;
	size_t at = 0;
	while (at < header.size()) {
		size_t nl = header.find('\n', at);
		if (nl == std::string::npos) nl = header.size();
		std::string line = trim(header.substr(at, nl - at));
		at = nl + 1;
		if (starts_with(line, "[")) {
			const std::string name = lower(line);
			section = name == "[v4+ styles]" ? modern : name == "[v4 styles]" ? legacy : none;
			name_field = 0;
			align_field = section == legacy ? 12 : 18;  // the specification's default Format
			continue;
		}
		if (section == none) continue;
		if (starts_with(line, "Format:")) {
			const auto fields = split(line.substr(7), ',');
			for (size_t k = 0; k < fields.size(); k++) {
				const std::string f = lower(trim(fields[k]));
				if (f == "name") name_field = int(k);
				else if (f == "alignment") align_field = int(k);
			}
			continue;
		}
		if (!starts_with(line, "Style:")) continue;
		const auto fields = split(line.substr(6), ',');
		if (int(fields.size()) <= std::max(name_field, align_field)) continue;
		int alignment = atoi(trim(fields[align_field]).c_str());
		if (section == legacy) alignment = legacy_alignment(alignment);
		if (alignment >= 1 && alignment <= 9) styles[style_key(fields[name_field])] = alignment;
	}
	return styles;
}

std::string ass_event_text(const std::string& text, const std::string& style,
                           const std::map<std::string, int>& styles) {
	std::string out;
	out.reserve(text.size());
	bool drawing = false, aligned = false;
	for (size_t i = 0; i < text.size();) {
		if (text[i] == '{') {
			const size_t end = text.find('}', i);
			if (end == std::string::npos) {
				if (!drawing) out += text.substr(i);
				break;
			}
			const std::string block = text.substr(i, end - i + 1);
			i = end + 1;
			if (block.find('\\') == std::string::npos) continue;  // a comment, hidden
			for (size_t j = block.find('\\'); j != std::string::npos; j = block.find('\\', j + 1))
				if (j + 2 < block.size() && block[j + 1] == 'p' && isdigit((unsigned char)block[j + 2]))
					drawing = atoi(block.c_str() + j + 2) > 0;  // \p0 ends a drawing, \pos is no drawing
			aligned = aligned || tag_alignment(block) != 0;
			out += block;
			continue;
		}
		if (!drawing) out += text[i];
		i++;
	}
	if (!aligned) {
		auto found = styles.find(style_key(style));
		if (found == styles.end()) found = styles.find("Default");  // libass falls back to it too
		if (found != styles.end() && found->second >= 4) out = "{\\an" + std::to_string(found->second) + "}" + out;
	}
	return out;
}

static std::vector<std::string> lines_of(const std::string& text) {
	std::vector<std::string> lines;
	std::string cur;
	for (char c : text) {
		if (c == '\n') {
			if (!cur.empty() && cur.back() == '\r') cur.pop_back();
			lines.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) lines.push_back(cur);
	return lines;
}

static std::vector<Cue> parse_srt_vtt(const std::vector<std::string>& lines) {
	std::vector<Cue> cues;
	for (size_t i = 0; i < lines.size(); i++) {
		size_t arrow = lines[i].find("-->");
		if (arrow == std::string::npos) continue;
		std::string a = trim(lines[i].substr(0, arrow));
		std::string b = trim(lines[i].substr(arrow + 3));
		size_t sp = b.find_first_of(" \t");  // VTT cue settings
		if (sp != std::string::npos) b = b.substr(0, sp);
		double start = parse_timestamp(a), end = parse_timestamp(b);
		if (start < 0 || end < 0) continue;
		std::string text;
		size_t j = i + 1;
		for (; j < lines.size(); j++) {
			if (trim(lines[j]).empty()) break;
			if (lines[j].find("-->") != std::string::npos) break;  // missing blank line
			if (!text.empty()) text += "\n";
			text += lines[j];
		}
		// A cue number right before the next timing line belongs to it, not to us.
		if (j < lines.size() && lines[j].find("-->") != std::string::npos && !text.empty()) {
			size_t nl = text.find_last_of('\n');
			std::string last = nl == std::string::npos ? text : text.substr(nl + 1);
			if (!last.empty() && last.find_first_not_of("0123456789") == std::string::npos)
				text = nl == std::string::npos ? "" : text.substr(0, nl);
		}
		i = j - 1;
		Cue c;
		c.start = start;
		c.end = end;
		auto converted = subtitle_text_to_cue(text);
		c.rml = std::move(converted.rml);
		c.place = converted.place;
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

static std::vector<Cue> parse_ass(const std::vector<std::string>& lines, const std::map<std::string, int>& styles) {
	std::vector<Cue> cues;
	int text_field = 9, start_field = 1, end_field = 2, style_field = 3;
	for (auto& line : lines) {
		if (starts_with(line, "Format:") && !cues.size()) {
			auto fields = split(line.substr(7), ',');
			for (size_t k = 0; k < fields.size(); k++) {
				std::string f = lower(trim(fields[k]));
				if (f == "start") start_field = int(k);
				else if (f == "end") end_field = int(k);
				else if (f == "text") text_field = int(k);
				else if (f == "style") style_field = int(k);
			}
			continue;
		}
		if (!starts_with(line, "Dialogue:")) continue;
		std::string rest = line.substr(9);
		std::vector<std::string> fields;
		size_t pos = 0;
		for (int k = 0; k < text_field; k++) {
			size_t comma = rest.find(',', pos);
			if (comma == std::string::npos) break;
			fields.push_back(rest.substr(pos, comma - pos));
			pos = comma + 1;
		}
		if (int(fields.size()) < text_field) continue;
		Cue c;
		c.start = parse_timestamp(trim(fields[start_field]));
		c.end = parse_timestamp(trim(fields[end_field]));
		if (c.start < 0 || c.end < 0) continue;
		const std::string style = style_field < int(fields.size()) ? fields[style_field] : "";
		auto text = subtitle_text_to_cue(ass_event_text(rest.substr(pos), style, styles));
		c.rml = std::move(text.rml);
		c.place = text.place;
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

static std::vector<Cue> parse_microdvd(const std::vector<std::string>& lines) {
	// {start}{end}text with frame numbers; assume 23.976 fps unless {1}{1}fps.
	std::vector<Cue> cues;
	double fps = 23.976;
	for (auto& line : lines) {
		if (line.size() < 6 || line[0] != '{') continue;
		size_t a = line.find('}');
		if (a == std::string::npos || a + 1 >= line.size() || line[a + 1] != '{') continue;
		size_t b = line.find('}', a + 1);
		if (b == std::string::npos) continue;
		long f0 = atol(line.substr(1, a - 1).c_str());
		long f1 = atol(line.substr(a + 2, b - a - 2).c_str());
		std::string text = line.substr(b + 1);
		if (f0 == 1 && f1 == 1) {
			double v = atof(text.c_str());
			if (v > 1) fps = v;
			continue;
		}
		Cue c;
		c.start = f0 / fps;
		c.end = f1 / fps;
		c.rml = subtitle_text_to_rml(replace_all(text, "|", "\n"));
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

std::vector<Cue> parse_subtitle_file(const std::string& text) {
	auto lines = lines_of(text);
	std::vector<Cue> cues;
	bool ass = text.find("[Script Info]") != std::string::npos || text.find("\nDialogue:") != std::string::npos;
	if (ass) cues = parse_ass(lines, ass_style_alignments(text));
	if (cues.empty()) cues = parse_srt_vtt(lines);
	if (cues.empty()) cues = parse_microdvd(lines);
	std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
	return cues;
}

std::string cues_at(const std::vector<Cue>& cues, double t) {
	// Cues are sorted by start; find the last one that started, then look
	// back for overlapping ones (rarely more than two).
	auto it = std::upper_bound(cues.begin(), cues.end(), t, [](double v, const Cue& c) { return v < c.start; });
	std::string rows[3];  // indexed by CuePlace: bottom, middle, top
	int looked = 0;
	while (it != cues.begin() && looked < 12) {
		--it;
		looked++;
		if (it->end <= t) continue;
		auto& out = rows[int(it->place)];
		out = out.empty() ? it->rml : it->rml + "<br/>" + out;
	}
	std::string out;
	if (!rows[int(CuePlace::top)].empty()) out += "<sub-top>" + rows[int(CuePlace::top)] + "</sub-top>";
	if (!rows[int(CuePlace::middle)].empty()) out += "<sub-middle>" + rows[int(CuePlace::middle)] + "</sub-middle>";
	return out + rows[int(CuePlace::bottom)];
}

CaptionRows caption_rows(const std::string& rml) {
	CaptionRows rows;
	std::string rest = rml;
	const auto take = [&rest](const std::string& name, std::string& into) {
		const std::string open = "<" + name + ">", close = "</" + name + ">";
		if (!starts_with(rest, open)) return;
		const size_t end = rest.find(close);
		if (end == std::string::npos) return;
		into = rest.substr(open.size(), end - open.size());
		rest = rest.substr(end + close.size());
	};
	take("sub-top", rows.top);
	take("sub-middle", rows.middle);
	rows.bottom = rest;
	return rows;
}
