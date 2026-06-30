/**************************************************************************/
/*  markdown_text_label.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "markdown_text_label.h"

#include "core/input/input_event.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "scene/resources/texture.h"
#include "scene/theme/theme_db.h"
#include "servers/rendering/rendering_server.h"
#include "servers/text/text_server.h"

String MarkdownTextLabel::_markdown_text_to_string(MD_TEXTTYPE p_type, const MD_CHAR *p_text, MD_SIZE p_size) {
	if (p_type == MD_TEXT_NULLCHAR) {
		String ret;
		for (MD_SIZE i = 0; i < MAX((MD_SIZE)1, p_size); i++) {
			ret += String::chr(0xfffd);
		}
		return ret;
	}

	String ret = String::utf8(p_text, p_size);
	if (p_type == MD_TEXT_ENTITY) {
		ret = ret.xml_unescape();
	}
	return ret;
}

String MarkdownTextLabel::_markdown_attribute_to_string(const MD_ATTRIBUTE &p_attribute) {
	if (p_attribute.text == nullptr || p_attribute.size == 0) {
		return String();
	}

	if (p_attribute.substr_types == nullptr || p_attribute.substr_offsets == nullptr) {
		return String::utf8(p_attribute.text, p_attribute.size).xml_unescape();
	}

	String ret;
	for (int i = 0; p_attribute.substr_offsets[i] < p_attribute.size; i++) {
		const MD_OFFSET start = p_attribute.substr_offsets[i];
		const MD_OFFSET end = p_attribute.substr_offsets[i + 1];
		ret += _markdown_text_to_string(p_attribute.substr_types[i], p_attribute.text + start, end - start);
	}
	return ret;
}

void MarkdownTextLabel::_invalidate(bool p_reparse) {
	if (p_reparse) {
		parsed_dirty = true;
	}
	layout_dirty = true;
	queue_redraw();
	update_minimum_size();
}

MarkdownTextLabel::InlineStyle MarkdownTextLabel::_current_style(const ParseState &p_state) const {
	InlineStyle style;
	style.bold = p_state.bold_depth > 0;
	style.italics = p_state.italics_depth > 0;
	style.mono = p_state.mono_depth > 0;
	style.underline = p_state.underline_depth > 0;
	style.strikethrough = p_state.strikethrough_depth > 0;
	if (!p_state.links.is_empty()) {
		const LinkState &link = p_state.links[p_state.links.size() - 1];
		style.link = true;
		style.meta = link.meta;
		style.tooltip = link.tooltip;
	}
	return style;
}

void MarkdownTextLabel::_append_plain_text(const String &p_text) const {
	if (p_text.is_empty()) {
		return;
	}
	parsed_text += p_text;
}

MarkdownTextLabel::Block &MarkdownTextLabel::_add_block(BlockType p_type, ParseState &r_state) const {
	Block block;
	block.type = p_type;
	block.quote_depth = r_state.quote_depth;
	block.list_depth = r_state.lists.size();
	block.prefix = r_state.pending_prefix;
	block.alignment = horizontal_alignment;
	r_state.pending_prefix.clear();
	blocks.push_back(block);
	r_state.current_block = blocks.size() - 1;
	return blocks.write[r_state.current_block];
}

MarkdownTextLabel::Block *MarkdownTextLabel::_get_current_block(ParseState &r_state) const {
	if (r_state.current_block >= 0 && r_state.current_block < blocks.size()) {
		return &blocks.write[r_state.current_block];
	}
	return nullptr;
}

MarkdownTextLabel::TableCell *MarkdownTextLabel::_get_current_cell(ParseState &r_state) const {
	if (r_state.current_block < 0 || r_state.current_block >= blocks.size()) {
		return nullptr;
	}
	Block &block = blocks.write[r_state.current_block];
	if (block.type != BLOCK_TABLE || r_state.current_row < 0 || r_state.current_row >= block.rows.size()) {
		return nullptr;
	}
	TableRow &row = block.rows.write[r_state.current_row];
	if (r_state.current_cell < 0 || r_state.current_cell >= row.cells.size()) {
		return nullptr;
	}
	return &row.cells.write[r_state.current_cell];
}

void MarkdownTextLabel::_add_text_run(ParseState &r_state, const String &p_text) const {
	if (p_text.is_empty()) {
		return;
	}

	if (!r_state.images.is_empty()) {
		r_state.images.write[r_state.images.size() - 1].alt_text += p_text;
		return;
	}

	TextRun run;
	run.text = p_text;
	run.style = _current_style(r_state);
	if (r_state.current_cell_header) {
		run.style.bold = true;
	}

	if (TableCell *cell = _get_current_cell(r_state)) {
		cell->runs.push_back(run);
		_append_plain_text(p_text);
		return;
	}

	Block *block = _get_current_block(r_state);
	if (!block || (block->type != BLOCK_PARAGRAPH && block->type != BLOCK_HEADING && block->type != BLOCK_CODE && block->type != BLOCK_HTML)) {
		block = &_add_block(BLOCK_PARAGRAPH, r_state);
	}
	block->runs.push_back(run);
	_append_plain_text(p_text);
}

void MarkdownTextLabel::_add_image_run(ParseState &r_state, const Ref<Texture2D> &p_texture, const String &p_source, const String &p_alt_text) const {
	if (p_texture.is_null()) {
		_add_text_run(r_state, !p_alt_text.is_empty() ? p_alt_text : p_source);
		return;
	}

	TextRun run;
	run.style = _current_style(r_state);
	run.image = p_texture;
	run.image_size = p_texture->get_size();

	if (TableCell *cell = _get_current_cell(r_state)) {
		cell->runs.push_back(run);
	} else {
		Block *block = _get_current_block(r_state);
		if (!block || (block->type != BLOCK_PARAGRAPH && block->type != BLOCK_HEADING)) {
			block = &_add_block(BLOCK_PARAGRAPH, r_state);
		}
		block->runs.push_back(run);
	}
	_append_plain_text(!p_alt_text.is_empty() ? p_alt_text : p_source);
}

int MarkdownTextLabel::_enter_block(MD_BLOCKTYPE p_type, void *p_detail, void *p_userdata) {
	ParseState &state = *static_cast<ParseState *>(p_userdata);
	MarkdownTextLabel *label = state.label;

	if (!state.images.is_empty()) {
		return 0;
	}

	switch (p_type) {
		case MD_BLOCK_DOC:
			break;
		case MD_BLOCK_QUOTE:
			state.quote_depth++;
			break;
		case MD_BLOCK_UL: {
			ListState list;
			list.ordered = false;
			state.lists.push_back(list);
		} break;
		case MD_BLOCK_OL: {
			const MD_BLOCK_OL_DETAIL *detail = static_cast<const MD_BLOCK_OL_DETAIL *>(p_detail);
			ListState list;
			list.ordered = true;
			list.index = detail ? (int)detail->start - 1 : 0;
			state.lists.push_back(list);
		} break;
		case MD_BLOCK_LI: {
			String prefix;
			if (!state.lists.is_empty()) {
				ListState &list = state.lists.write[state.lists.size() - 1];
				list.index++;
				prefix = list.ordered ? String::num_int64(list.index) + ". " : String::utf8("• ");
			}
			const MD_BLOCK_LI_DETAIL *detail = static_cast<const MD_BLOCK_LI_DETAIL *>(p_detail);
			if (detail && detail->is_task) {
				prefix += (detail->task_mark == 'x' || detail->task_mark == 'X') ? "[x] " : "[ ] ";
			}
			state.pending_prefix = prefix;
		} break;
		case MD_BLOCK_HR:
			label->_add_block(BLOCK_HR, state);
			state.current_block = -1;
			label->_append_plain_text("\n");
			break;
		case MD_BLOCK_H: {
			const MD_BLOCK_H_DETAIL *detail = static_cast<const MD_BLOCK_H_DETAIL *>(p_detail);
			Block &block = label->_add_block(BLOCK_HEADING, state);
			block.heading_level = detail ? CLAMP((int)detail->level, 1, 6) : 1;
		} break;
		case MD_BLOCK_CODE: {
			label->_add_block(BLOCK_CODE, state);
			state.mono_depth++;
		} break;
		case MD_BLOCK_HTML:
			label->_add_block(BLOCK_HTML, state);
			state.mono_depth++;
			break;
		case MD_BLOCK_P:
			if (!state.in_table) {
				label->_add_block(BLOCK_PARAGRAPH, state);
			}
			break;
		case MD_BLOCK_TABLE:
			label->_add_block(BLOCK_TABLE, state);
			state.in_table = true;
			state.current_row = -1;
			state.current_cell = -1;
			break;
		case MD_BLOCK_THEAD:
		case MD_BLOCK_TBODY:
			break;
		case MD_BLOCK_TR: {
			Block *block = label->_get_current_block(state);
			if (block && block->type == BLOCK_TABLE) {
				TableRow row;
				block->rows.push_back(row);
				state.current_row = block->rows.size() - 1;
				state.current_cell = -1;
			}
		} break;
		case MD_BLOCK_TH:
		case MD_BLOCK_TD: {
			Block *block = label->_get_current_block(state);
			if (!block || block->type != BLOCK_TABLE || state.current_row < 0 || state.current_row >= block->rows.size()) {
				break;
			}

			TableCell cell;
			cell.header = p_type == MD_BLOCK_TH;
			state.current_cell_header = cell.header;
			const MD_BLOCK_TD_DETAIL *detail = static_cast<const MD_BLOCK_TD_DETAIL *>(p_detail);
			if (detail) {
				switch (detail->align) {
					case MD_ALIGN_CENTER:
						cell.alignment = HORIZONTAL_ALIGNMENT_CENTER;
						break;
					case MD_ALIGN_RIGHT:
						cell.alignment = HORIZONTAL_ALIGNMENT_RIGHT;
						break;
					case MD_ALIGN_LEFT:
					case MD_ALIGN_DEFAULT:
					default:
						cell.alignment = HORIZONTAL_ALIGNMENT_LEFT;
						break;
				}
			}
			block->rows.write[state.current_row].cells.push_back(cell);
			state.current_cell = block->rows[state.current_row].cells.size() - 1;
		} break;
	}

	return 0;
}

int MarkdownTextLabel::_leave_block(MD_BLOCKTYPE p_type, void *p_detail, void *p_userdata) {
	ParseState &state = *static_cast<ParseState *>(p_userdata);
	MarkdownTextLabel *label = state.label;

	if (!state.images.is_empty()) {
		return 0;
	}

	switch (p_type) {
		case MD_BLOCK_DOC:
			break;
		case MD_BLOCK_QUOTE:
			state.quote_depth = MAX(0, state.quote_depth - 1);
			break;
		case MD_BLOCK_UL:
		case MD_BLOCK_OL:
			if (!state.lists.is_empty()) {
				state.lists.remove_at(state.lists.size() - 1);
			}
			state.pending_prefix.clear();
			break;
		case MD_BLOCK_LI:
			state.pending_prefix.clear();
			label->_append_plain_text("\n");
			break;
		case MD_BLOCK_HR:
			break;
		case MD_BLOCK_H:
			state.current_block = -1;
			label->_append_plain_text("\n");
			break;
		case MD_BLOCK_P:
			if (!state.in_table) {
				state.current_block = -1;
				label->_append_plain_text("\n");
			}
			break;
		case MD_BLOCK_CODE:
		case MD_BLOCK_HTML:
			state.mono_depth = MAX(0, state.mono_depth - 1);
			state.current_block = -1;
			label->_append_plain_text("\n");
			break;
		case MD_BLOCK_TABLE:
			state.in_table = false;
			state.current_block = -1;
			state.current_row = -1;
			state.current_cell = -1;
			label->_append_plain_text("\n");
			break;
		case MD_BLOCK_THEAD:
		case MD_BLOCK_TBODY:
			break;
		case MD_BLOCK_TR:
			state.current_cell = -1;
			label->_append_plain_text("\n");
			break;
		case MD_BLOCK_TH:
		case MD_BLOCK_TD:
			state.current_cell = -1;
			state.current_cell_header = false;
			label->_append_plain_text("\t");
			break;
	}

	(void)p_detail;
	return 0;
}

int MarkdownTextLabel::_enter_span(MD_SPANTYPE p_type, void *p_detail, void *p_userdata) {
	ParseState &state = *static_cast<ParseState *>(p_userdata);

	if (!state.images.is_empty() && p_type != MD_SPAN_IMG) {
		return 0;
	}

	switch (p_type) {
		case MD_SPAN_EM:
			state.italics_depth++;
			break;
		case MD_SPAN_STRONG:
			state.bold_depth++;
			break;
		case MD_SPAN_A: {
			const MD_SPAN_A_DETAIL *detail = static_cast<const MD_SPAN_A_DETAIL *>(p_detail);
			LinkState link;
			link.meta = detail ? _markdown_attribute_to_string(detail->href) : String();
			link.tooltip = detail ? _markdown_attribute_to_string(detail->title) : String();
			state.links.push_back(link);
		} break;
		case MD_SPAN_IMG: {
			const MD_SPAN_IMG_DETAIL *detail = static_cast<const MD_SPAN_IMG_DETAIL *>(p_detail);
			ImageState image;
			image.source = detail ? _markdown_attribute_to_string(detail->src) : String();
			state.images.push_back(image);
		} break;
		case MD_SPAN_CODE:
			state.mono_depth++;
			break;
		case MD_SPAN_DEL:
			state.strikethrough_depth++;
			break;
		case MD_SPAN_LATEXMATH:
		case MD_SPAN_LATEXMATH_DISPLAY:
			state.math_display.push_back(p_type == MD_SPAN_LATEXMATH_DISPLAY);
			state.mono_depth++;
			state.label->_add_text_run(state, p_type == MD_SPAN_LATEXMATH_DISPLAY ? "$$" : "$");
			break;
		case MD_SPAN_WIKILINK: {
			const MD_SPAN_WIKILINK_DETAIL *detail = static_cast<const MD_SPAN_WIKILINK_DETAIL *>(p_detail);
			LinkState link;
			link.meta = detail ? _markdown_attribute_to_string(detail->target) : String();
			state.links.push_back(link);
		} break;
		case MD_SPAN_U:
			state.underline_depth++;
			break;
	}

	return 0;
}

int MarkdownTextLabel::_leave_span(MD_SPANTYPE p_type, void *p_detail, void *p_userdata) {
	ParseState &state = *static_cast<ParseState *>(p_userdata);
	MarkdownTextLabel *label = state.label;

	if (!state.images.is_empty() && p_type != MD_SPAN_IMG) {
		return 0;
	}

	switch (p_type) {
		case MD_SPAN_EM:
			state.italics_depth = MAX(0, state.italics_depth - 1);
			break;
		case MD_SPAN_STRONG:
			state.bold_depth = MAX(0, state.bold_depth - 1);
			break;
		case MD_SPAN_A:
		case MD_SPAN_WIKILINK:
			if (!state.links.is_empty()) {
				state.links.remove_at(state.links.size() - 1);
			}
			break;
		case MD_SPAN_IMG: {
			if (state.images.is_empty()) {
				break;
			}
			ImageState image = state.images[state.images.size() - 1];
			state.images.remove_at(state.images.size() - 1);
			if (!state.images.is_empty()) {
				state.images.write[state.images.size() - 1].alt_text += image.alt_text;
				break;
			}
			Ref<Texture2D> texture = ResourceLoader::load(image.source, "Texture2D");
			label->_add_image_run(state, texture, image.source, image.alt_text);
		} break;
		case MD_SPAN_CODE:
			state.mono_depth = MAX(0, state.mono_depth - 1);
			break;
		case MD_SPAN_DEL:
			state.strikethrough_depth = MAX(0, state.strikethrough_depth - 1);
			break;
		case MD_SPAN_LATEXMATH:
		case MD_SPAN_LATEXMATH_DISPLAY: {
			const bool display = !state.math_display.is_empty() && state.math_display[state.math_display.size() - 1];
			if (!state.math_display.is_empty()) {
				state.math_display.remove_at(state.math_display.size() - 1);
			}
			label->_add_text_run(state, display ? "$$" : "$");
			state.mono_depth = MAX(0, state.mono_depth - 1);
		} break;
		case MD_SPAN_U:
			state.underline_depth = MAX(0, state.underline_depth - 1);
			break;
	}

	(void)p_detail;
	return 0;
}

int MarkdownTextLabel::_text_callback(MD_TEXTTYPE p_type, const MD_CHAR *p_text, MD_SIZE p_size, void *p_userdata) {
	ParseState &state = *static_cast<ParseState *>(p_userdata);
	String text;

	if (p_type == MD_TEXT_BR) {
		text = "\n";
	} else if (p_type == MD_TEXT_SOFTBR) {
		text = " ";
	} else {
		text = _markdown_text_to_string(p_type, p_text, p_size);
	}

	state.label->_add_text_run(state, text);
	return 0;
}

void MarkdownTextLabel::_parse_markdown() const {
	if (!parsed_dirty) {
		return;
	}

	blocks.clear();
	link_rects.clear();
	parsed_text.clear();
	next_object_key = 1;

	ParseState state;
	state.label = const_cast<MarkdownTextLabel *>(this);

	MD_PARSER parser = {};
	parser.abi_version = 0;
	parser.flags = MD_DIALECT_GITHUB | MD_FLAG_PERMISSIVEATXHEADERS | MD_FLAG_LATEXMATHSPANS | MD_FLAG_WIKILINKS;
	parser.enter_block = _enter_block;
	parser.leave_block = _leave_block;
	parser.enter_span = _enter_span;
	parser.leave_span = _leave_span;
	parser.text = _text_callback;

	String markdown = text.replace("\r\n", "\n").replace("\r", "\n");
	CharString markdown_utf8 = markdown.utf8();
	int result = md_parse(markdown_utf8.get_data(), markdown_utf8.length(), &parser, &state);
	if (result != 0) {
		ERR_PRINT(vformat("MarkdownTextLabel failed to parse Markdown, error code: %d.", result));
	}

	parsed_text = parsed_text.strip_edges();
	parsed_dirty = false;
	layout_dirty = true;
}

Ref<Font> MarkdownTextLabel::_get_font_for_style(const InlineStyle &p_style, bool p_force_bold) const {
	Ref<Font> font;
	if (p_style.mono) {
		font = theme_cache.mono_font;
	} else if ((p_style.bold || p_force_bold) && p_style.italics) {
		font = theme_cache.bold_italics_font;
	} else if (p_style.bold || p_force_bold) {
		font = theme_cache.bold_font;
	} else if (p_style.italics) {
		font = theme_cache.italics_font;
	} else {
		font = theme_cache.normal_font;
	}
	if (font.is_null()) {
		font = theme_cache.normal_font;
	}
	if (font.is_null()) {
		font = ThemeDB::get_singleton()->get_fallback_font();
	}
	return font;
}

int MarkdownTextLabel::_get_font_size_for_style(const InlineStyle &p_style, int p_heading_level) const {
	int font_size = theme_cache.normal_font_size;
	if (p_style.mono) {
		font_size = theme_cache.mono_font_size;
	} else if (p_style.bold && p_style.italics) {
		font_size = theme_cache.bold_italics_font_size;
	} else if (p_style.bold) {
		font_size = theme_cache.bold_font_size;
	} else if (p_style.italics) {
		font_size = theme_cache.italics_font_size;
	}
	if (font_size <= 0) {
		font_size = theme_cache.normal_font_size;
	}
	if (font_size <= 0) {
		font_size = ThemeDB::get_singleton()->get_fallback_font_size();
	}
	if (p_heading_level > 0) {
		static const double heading_scale[6] = { 2.0, 1.5, 1.25, 1.125, 1.0, 0.875 };
		const int level = CLAMP(p_heading_level, 1, 6);
		font_size = MAX(1, (int)Math::round(font_size * heading_scale[level - 1]));
	}
	return font_size;
}

BitField<TextServer::LineBreakFlag> MarkdownTextLabel::_get_line_break_flags() const {
	BitField<TextServer::LineBreakFlag> flags = TextServer::BREAK_MANDATORY;
	switch (autowrap_mode) {
		case TextServer::AUTOWRAP_WORD_SMART:
			flags = TextServer::BREAK_WORD_BOUND | TextServer::BREAK_ADAPTIVE | TextServer::BREAK_MANDATORY;
			break;
		case TextServer::AUTOWRAP_WORD:
			flags = TextServer::BREAK_WORD_BOUND | TextServer::BREAK_MANDATORY;
			break;
		case TextServer::AUTOWRAP_ARBITRARY:
			flags = TextServer::BREAK_GRAPHEME_BOUND | TextServer::BREAK_MANDATORY;
			break;
		case TextServer::AUTOWRAP_OFF:
			break;
	}
	return flags | autowrap_flags_trim;
}

TextServer::Direction MarkdownTextLabel::_get_text_server_direction() const {
	if (text_direction == Control::TEXT_DIRECTION_INHERITED) {
		return is_layout_rtl() ? TextServer::DIRECTION_RTL : TextServer::DIRECTION_LTR;
	}
	return (TextServer::Direction)text_direction;
}

float MarkdownTextLabel::_get_paragraph_width(float p_available_width) const {
	if (autowrap_mode == TextServer::AUTOWRAP_OFF) {
		return -1.0;
	}
	return MAX(1.0, p_available_width);
}

MarkdownTextLabel::RenderParagraph MarkdownTextLabel::_make_paragraph(const Vector<TextRun> &p_runs, const String &p_prefix, HorizontalAlignment p_alignment, int p_heading_level,
		bool p_code_block, bool p_force_bold, float p_width) const {
	RenderParagraph render;
	render.width = p_width;
	render.paragraph.instantiate();
	render.paragraph->set_direction(_get_text_server_direction());
	render.paragraph->set_alignment(p_alignment);
	render.paragraph->set_width(p_width);
	render.paragraph->set_break_flags(_get_line_break_flags());
	render.paragraph->set_justification_flags(justification_flags);
	render.paragraph->set_line_spacing(theme_cache.line_separation);

	int char_offset = 0;
	InlineStyle prefix_style;
	prefix_style.bold = p_force_bold;
	if (!p_prefix.is_empty()) {
		render.paragraph->add_string(p_prefix, _get_font_for_style(prefix_style, false),
				_get_font_size_for_style(prefix_style, p_heading_level), language);
		char_offset += p_prefix.length();
	}

	String bidi_text = p_prefix;

	for (const TextRun &run : p_runs) {
		if (run.image.is_valid()) {
			Size2 image_size = run.image_size;
			if (p_width > 0 && image_size.width > p_width) {
				const float ratio = p_width / image_size.width;
				image_size.width *= ratio;
				image_size.height *= ratio;
			}

			int key = next_object_key++;
			render.paragraph->add_object(key, image_size, INLINE_ALIGNMENT_CENTER, 1);
			ImageObject object;
			object.key = key;
			object.texture = run.image;
			render.images.push_back(object);
			char_offset++;
			bidi_text += String::chr(0xfffc);
			continue;
		}

		if (run.text.is_empty()) {
			continue;
		}

		InlineStyle style = run.style;
		style.bold = style.bold || p_force_bold;
		render.paragraph->add_string(run.text, _get_font_for_style(run.style, p_force_bold),
				_get_font_size_for_style(style, p_heading_level), language);
		bidi_text += run.text;
		DrawRange range;
		range.from = char_offset;
		range.to = char_offset + run.text.length();
		range.code = run.style.mono && !p_code_block;
		range.underline = run.style.underline || run.style.link;
		range.strikethrough = run.style.strikethrough;
		range.link = run.style.link;
		range.meta = run.style.meta;
		range.tooltip = run.style.tooltip;
		if (range.code || range.underline || range.strikethrough || range.link) {
			render.ranges.push_back(range);
		}
		char_offset = range.to;
	}

	if (char_offset == 0) {
		render.paragraph->add_string(String::chr(0x200B), _get_font_for_style(prefix_style, false),
				_get_font_size_for_style(prefix_style, p_heading_level), language);
		bidi_text = String::chr(0x200B);
	}

	render.paragraph->set_bidi_override(structured_text_parser(st_parser, st_args, bidi_text));
	render.size = render.paragraph->get_size();
	return render;
}

void MarkdownTextLabel::_shape_block(Block &r_block, float p_width) const {
	const float indent = r_block.quote_depth * theme_cache.quote_indent + r_block.list_depth * theme_cache.list_indent;
	const float available_width = MAX(1.0, p_width - indent);
	const float paragraph_width = _get_paragraph_width(available_width);

	switch (r_block.type) {
		case BLOCK_HEADING:
			r_block.render = _make_paragraph(r_block.runs, r_block.prefix, r_block.alignment, r_block.heading_level, false, true, paragraph_width);
			r_block.rect.size = r_block.render.size;
			break;
		case BLOCK_CODE:
		case BLOCK_HTML:
			r_block.render = _make_paragraph(r_block.runs, r_block.prefix, HORIZONTAL_ALIGNMENT_LEFT, 0, true, false,
					paragraph_width > 0 ? MAX(1.0, paragraph_width - theme_cache.code_padding * 2) : paragraph_width);
			r_block.rect.size = r_block.render.size + Size2(theme_cache.code_padding * 2, theme_cache.code_padding * 2);
			break;
		case BLOCK_HR:
			r_block.rect.size = Size2(available_width, MAX(1, theme_cache.quote_bar_width));
			break;
		case BLOCK_TABLE:
			_shape_table(r_block, available_width);
			break;
		case BLOCK_PARAGRAPH:
		default:
			r_block.render = _make_paragraph(r_block.runs, r_block.prefix, r_block.alignment, 0, false, false, paragraph_width);
			r_block.rect.size = r_block.render.size;
			break;
	}
}

void MarkdownTextLabel::_shape_table(Block &r_block, float p_width) const {
	int column_count = 0;
	for (const TableRow &row : r_block.rows) {
		column_count = MAX(column_count, row.cells.size());
	}
	if (column_count == 0) {
		r_block.rect.size = Size2();
		return;
	}

	const float total_spacing = theme_cache.table_h_separation * MAX(0, column_count - 1);
	const float column_width = MAX(1.0, (p_width - total_spacing) / column_count);
	float y = 0.0;
	float table_width = column_width * column_count + total_spacing;

	for (TableRow &row : r_block.rows) {
		row.height = 0.0;
		for (int i = 0; i < row.cells.size(); i++) {
			TableCell &cell = row.cells.write[i];
			cell.render = _make_paragraph(cell.runs, String(), cell.alignment, 0, false, cell.header, MAX(1.0, column_width - theme_cache.table_h_separation * 2));
			cell.rect = Rect2((column_width + theme_cache.table_h_separation) * i, y, column_width, cell.render.size.height + theme_cache.table_v_separation * 2);
			row.height = MAX(row.height, cell.rect.size.height);
		}
		for (TableCell &cell : row.cells) {
			cell.rect.size.height = row.height;
		}
		y += row.height + theme_cache.table_v_separation;
	}

	if (!r_block.rows.is_empty()) {
		y -= theme_cache.table_v_separation;
	}
	r_block.rect.size = Size2(table_width, y);
}

void MarkdownTextLabel::_ensure_layout() const {
	_parse_markdown();
	if (!layout_dirty) {
		return;
	}

	link_rects.clear();
	next_object_key = 1;
	content_size = Size2();

	const Ref<StyleBox> style = theme_cache.normal_style;
	const Size2 style_minimum = style.is_valid() ? style->get_minimum_size() : Size2();
	const float available_width = MAX(1.0, get_size().width - style_minimum.width);
	float y = 0.0;
	float max_width = 0.0;

	for (Block &block : blocks) {
		_shape_block(block, available_width);
		const float indent = block.quote_depth * theme_cache.quote_indent + block.list_depth * theme_cache.list_indent;
		block.rect.position = Vector2(indent, y);
		y += block.rect.size.height + theme_cache.block_separation + theme_cache.paragraph_separation;
		max_width = MAX(max_width, indent + block.rect.size.width);
	}
	if (!blocks.is_empty()) {
		y -= theme_cache.block_separation + theme_cache.paragraph_separation;
	}
	content_size = Size2(max_width, MAX(0.0, y)) + style_minimum;

	const Vector2 base = style.is_valid() ? style->get_offset() : Vector2();
	for (const Block &block : blocks) {
		Vector2 block_pos = base + block.rect.position;
		if (block.type == BLOCK_CODE || block.type == BLOCK_HTML) {
			block_pos += Vector2(theme_cache.code_padding, theme_cache.code_padding);
		}
		if (block.type == BLOCK_TABLE) {
			for (const TableRow &row : block.rows) {
				for (const TableCell &cell : row.cells) {
					_collect_link_rects(cell.render, block_pos + cell.rect.position + Vector2(theme_cache.table_h_separation, theme_cache.table_v_separation));
				}
			}
		} else if (block.render.paragraph.is_valid()) {
			_collect_link_rects(block.render, block_pos);
		}
	}

	layout_dirty = false;
}

Vector2 MarkdownTextLabel::_get_line_top(const RenderParagraph &p_render, int p_line, const Vector2 &p_position) const {
	float y = 0.0;
	for (int i = 0; i < p_line; i++) {
		y += p_render.paragraph->get_line_ascent(i) + p_render.paragraph->get_line_descent(i) + theme_cache.line_separation;
	}

	float x = 0.0;
	const RID line_rid = p_render.paragraph->get_line_rid(p_line);
	const float line_width = TS->shaped_text_get_width(line_rid);
	const float layout_width = p_render.width > 0 ? p_render.width : p_render.paragraph->get_size().width;
	switch (p_render.paragraph->get_alignment()) {
		case HORIZONTAL_ALIGNMENT_CENTER:
			if (line_width < layout_width) {
				x = Math::floor((layout_width - line_width) / 2.0);
			}
			break;
		case HORIZONTAL_ALIGNMENT_RIGHT:
			x = layout_width - line_width;
			break;
		case HORIZONTAL_ALIGNMENT_FILL:
		case HORIZONTAL_ALIGNMENT_LEFT:
		default:
			break;
	}
	return p_position + Vector2(x, y);
}

Vector2 MarkdownTextLabel::_get_line_baseline(const RenderParagraph &p_render, int p_line, const Vector2 &p_position) const {
	return _get_line_top(p_render, p_line, p_position) + Vector2(0, p_render.paragraph->get_line_ascent(p_line));
}

void MarkdownTextLabel::_collect_link_rects(const RenderParagraph &p_render, const Vector2 &p_position) const {
	if (p_render.paragraph.is_null()) {
		return;
	}

	for (const DrawRange &range : p_render.ranges) {
		if (!range.link) {
			continue;
		}
		for (int line = 0; line < p_render.paragraph->get_line_count(); line++) {
			const Vector2i line_range = p_render.paragraph->get_line_range(line);
			const int from = MAX(range.from, line_range.x);
			const int to = MIN(range.to, line_range.y);
			if (from >= to) {
				continue;
			}

			const RID line_rid = p_render.paragraph->get_line_rid(line);
			const Vector2 baseline = _get_line_baseline(p_render, line, p_position);
			Vector<Vector2> selection = TS->shaped_text_get_selection(line_rid, from, to);
			const float ascent = p_render.paragraph->get_line_ascent(line);
			const float height = p_render.paragraph->get_line_ascent(line) + p_render.paragraph->get_line_descent(line);
			for (const Vector2 &segment : selection) {
				LinkRect link_rect;
				link_rect.rect = Rect2(baseline.x + segment.x, baseline.y - ascent, segment.y - segment.x, height);
				link_rect.meta = range.meta;
				link_rect.tooltip = range.tooltip;
				link_rects.push_back(link_rect);
			}
		}
	}
}

const MarkdownTextLabel::ImageObject *MarkdownTextLabel::_find_image(const RenderParagraph &p_render, int p_key) const {
	for (const ImageObject &image : p_render.images) {
		if (image.key == p_key) {
			return &image;
		}
	}
	return nullptr;
}

void MarkdownTextLabel::_draw_render_paragraph(const RenderParagraph &p_render, const Vector2 &p_position, bool p_code_background) const {
	if (p_render.paragraph.is_null()) {
		return;
	}

	RID ci = get_canvas_item();

	for (int line = 0; line < p_render.paragraph->get_line_count(); line++) {
		const RID line_rid = p_render.paragraph->get_line_rid(line);
		const Vector2 top = _get_line_top(p_render, line, p_position);
		const Vector2 baseline = _get_line_baseline(p_render, line, p_position);
		const float ascent = p_render.paragraph->get_line_ascent(line);
		const float descent = p_render.paragraph->get_line_descent(line);
		const float line_height = ascent + descent;

		for (const DrawRange &range : p_render.ranges) {
			if (!range.code) {
				continue;
			}
			Vector2i line_range = p_render.paragraph->get_line_range(line);
			int from = MAX(range.from, line_range.x);
			int to = MIN(range.to, line_range.y);
			if (from >= to) {
				continue;
			}
			Vector<Vector2> selection = TS->shaped_text_get_selection(line_rid, from, to);
			for (const Vector2 &segment : selection) {
				Rect2 rect = Rect2(baseline.x + segment.x - 2, baseline.y - ascent, segment.y - segment.x + 4, line_height);
				RenderingServer::get_singleton()->canvas_item_add_rect(ci, rect, theme_cache.code_bg_color);
			}
		}

		if (p_code_background) {
			RenderingServer::get_singleton()->canvas_item_add_rect(ci, Rect2(top, Size2(MAX(p_render.width, p_render.paragraph->get_size().width), line_height)), theme_cache.code_bg_color);
		}

		if (theme_cache.outline_size > 0) {
			TS->shaped_text_draw_outline(line_rid, ci, baseline, -1, -1, theme_cache.outline_size, theme_cache.font_outline_color);
		}
		if (theme_cache.font_shadow_color.a > 0) {
			TS->shaped_text_draw_outline(line_rid, ci, baseline + theme_cache.font_shadow_offset, -1, -1, theme_cache.shadow_outline_size, theme_cache.font_shadow_color);
			TS->shaped_text_draw(line_rid, ci, baseline + theme_cache.font_shadow_offset, -1, -1, theme_cache.font_shadow_color);
		}
		TS->shaped_text_draw(line_rid, ci, baseline, -1, -1, theme_cache.font_color);

		for (const DrawRange &range : p_render.ranges) {
			if (!range.link && !range.underline && !range.strikethrough) {
				continue;
			}
			Vector2i line_range = p_render.paragraph->get_line_range(line);
			int from = MAX(range.from, line_range.x);
			int to = MIN(range.to, line_range.y);
			if (from >= to) {
				continue;
			}

			Vector<Vector2> selection = TS->shaped_text_get_selection(line_rid, from, to);
			for (const Vector2 &segment : selection) {
				if (range.link) {
					TS->shaped_text_draw(line_rid, ci, baseline, segment.x, segment.y, theme_cache.link_color);
				}
				if (range.underline || range.link) {
					float y = baseline.y + TS->shaped_text_get_underline_position(line_rid);
					float h = MAX(1.0, TS->shaped_text_get_underline_thickness(line_rid));
					RenderingServer::get_singleton()->canvas_item_add_rect(
							ci,
							Rect2(Point2(baseline.x + segment.x, y), Size2(segment.y - segment.x, h)),
							range.link ? theme_cache.link_color : theme_cache.font_color);
				}
				if (range.strikethrough) {
					float y = baseline.y - ascent + line_height * 0.55;
					RenderingServer::get_singleton()->canvas_item_add_rect(ci, Rect2(Point2(baseline.x + segment.x, y), Size2(segment.y - segment.x, 1)), theme_cache.font_color);
				}
			}
		}

		Array objects = TS->shaped_text_get_objects(line_rid);
		for (int i = 0; i < objects.size(); i++) {
			const Variant key_variant = objects[i];
			const int key = (int)key_variant;
			const ImageObject *image = _find_image(p_render, key);
			if (!image || image->texture.is_null()) {
				continue;
			}
			Rect2 object_rect = TS->shaped_text_get_object_rect(line_rid, key_variant);
			image->texture->draw_rect(ci, Rect2(baseline + object_rect.position, object_rect.size), false, image->color);
		}
	}
}

void MarkdownTextLabel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_RESIZED:
			layout_dirty = true;
			queue_redraw();
			break;
		case NOTIFICATION_DRAW: {
			_ensure_layout();
			RID ci = get_canvas_item();
			if (theme_cache.normal_style.is_valid()) {
				theme_cache.normal_style->draw(ci, Rect2(Point2(), get_size()));
			}
			if (has_focus(false) && theme_cache.focus_style.is_valid()) {
				theme_cache.focus_style->draw(ci, Rect2(Point2(), get_size()));
			}

			const Vector2 base = theme_cache.normal_style.is_valid() ? theme_cache.normal_style->get_offset() : Vector2();
			for (const Block &block : blocks) {
				Vector2 pos = base + block.rect.position;
				if (block.quote_depth > 0) {
					for (int i = 0; i < block.quote_depth; i++) {
						float x = base.x + i * theme_cache.quote_indent + theme_cache.quote_bar_width;
						RenderingServer::get_singleton()->canvas_item_add_rect(ci, Rect2(Point2(x, pos.y), Size2(theme_cache.quote_bar_width, block.rect.size.height)), theme_cache.quote_bar_color);
					}
				}

				switch (block.type) {
					case BLOCK_HR: {
						float y = pos.y + block.rect.size.height / 2.0;
						RenderingServer::get_singleton()->canvas_item_add_rect(
								ci,
								Rect2(Point2(pos.x, y), Size2(block.rect.size.width, MAX(1, theme_cache.quote_bar_width))),
								theme_cache.table_border_color);
					} break;
					case BLOCK_CODE:
					case BLOCK_HTML:
						RenderingServer::get_singleton()->canvas_item_add_rect(ci, Rect2(pos, block.rect.size), theme_cache.code_bg_color);
						_draw_render_paragraph(block.render, pos + Vector2(theme_cache.code_padding, theme_cache.code_padding), false);
						break;
					case BLOCK_TABLE: {
						for (const TableRow &row : block.rows) {
							for (const TableCell &cell : row.cells) {
								Rect2 cell_rect(pos + cell.rect.position, cell.rect.size);
								const float border_width = 1.0;
								RenderingServer::get_singleton()->canvas_item_add_rect(
										ci,
										Rect2(cell_rect.position, Size2(cell_rect.size.width, border_width)),
										theme_cache.table_border_color);
								RenderingServer::get_singleton()->canvas_item_add_rect(
										ci,
										Rect2(cell_rect.position + Point2(0, cell_rect.size.height - border_width), Size2(cell_rect.size.width, border_width)),
										theme_cache.table_border_color);
								RenderingServer::get_singleton()->canvas_item_add_rect(
										ci,
										Rect2(cell_rect.position, Size2(border_width, cell_rect.size.height)),
										theme_cache.table_border_color);
								RenderingServer::get_singleton()->canvas_item_add_rect(
										ci,
										Rect2(cell_rect.position + Point2(cell_rect.size.width - border_width, 0), Size2(border_width, cell_rect.size.height)),
										theme_cache.table_border_color);
								_draw_render_paragraph(cell.render, cell_rect.position + Vector2(theme_cache.table_h_separation, theme_cache.table_v_separation), false);
							}
						}
					} break;
					case BLOCK_PARAGRAPH:
					case BLOCK_HEADING:
					default:
						_draw_render_paragraph(block.render, pos, false);
						break;
				}
			}
		} break;
	}
}

void MarkdownTextLabel::_update_theme_item_cache() {
	Control::_update_theme_item_cache();

	theme_cache.normal_style = get_theme_stylebox(CoreStringName(normal));
	theme_cache.focus_style = get_theme_stylebox("focus");
	theme_cache.normal_font = get_theme_font("normal_font");
	theme_cache.bold_font = get_theme_font("bold_font");
	theme_cache.italics_font = get_theme_font("italics_font");
	theme_cache.bold_italics_font = get_theme_font("bold_italics_font");
	theme_cache.mono_font = get_theme_font("mono_font");
	theme_cache.normal_font_size = get_theme_font_size("normal_font_size");
	theme_cache.bold_font_size = get_theme_font_size("bold_font_size");
	theme_cache.italics_font_size = get_theme_font_size("italics_font_size");
	theme_cache.bold_italics_font_size = get_theme_font_size("bold_italics_font_size");
	theme_cache.mono_font_size = get_theme_font_size("mono_font_size");
	theme_cache.font_color = get_theme_color("font_color");
	theme_cache.link_color = get_theme_color("link_color");
	theme_cache.code_bg_color = get_theme_color("code_bg_color");
	theme_cache.quote_bar_color = get_theme_color("quote_bar_color");
	theme_cache.table_border_color = get_theme_color("table_border_color");
	theme_cache.font_shadow_color = get_theme_color("font_shadow_color");
	theme_cache.font_shadow_offset = Point2(get_theme_constant("shadow_offset_x"), get_theme_constant("shadow_offset_y"));
	theme_cache.font_outline_color = get_theme_color("font_outline_color");
	theme_cache.outline_size = get_theme_constant("outline_size");
	theme_cache.shadow_outline_size = get_theme_constant("shadow_outline_size");
	theme_cache.line_separation = get_theme_constant(SceneStringName(line_separation));
	theme_cache.paragraph_separation = get_theme_constant(SceneStringName(paragraph_separation));
	theme_cache.block_separation = get_theme_constant("block_separation");
	theme_cache.list_indent = get_theme_constant("list_indent");
	theme_cache.quote_indent = get_theme_constant("quote_indent");
	theme_cache.quote_bar_width = get_theme_constant("quote_bar_width");
	theme_cache.code_padding = get_theme_constant("code_padding");
	theme_cache.table_h_separation = get_theme_constant("table_h_separation");
	theme_cache.table_v_separation = get_theme_constant("table_v_separation");

	_invalidate(false);
}

void MarkdownTextLabel::gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
		_ensure_layout();
		for (const LinkRect &link_rect : link_rects) {
			if (link_rect.rect.has_point(mb->get_position())) {
				emit_signal(SNAME("meta_clicked"), link_rect.meta);
				accept_event();
				return;
			}
		}
	}
}

Control::CursorShape MarkdownTextLabel::get_cursor_shape(const Point2 &p_pos) const {
	_ensure_layout();
	for (const LinkRect &link_rect : link_rects) {
		if (link_rect.rect.has_point(p_pos)) {
			return CURSOR_POINTING_HAND;
		}
	}
	return Control::get_cursor_shape(p_pos);
}

String MarkdownTextLabel::get_tooltip(const Point2 &p_pos) const {
	_ensure_layout();
	for (const LinkRect &link_rect : link_rects) {
		if (link_rect.rect.has_point(p_pos) && !link_rect.tooltip.is_empty()) {
			return link_rect.tooltip;
		}
	}
	return Control::get_tooltip(p_pos);
}

Size2 MarkdownTextLabel::get_minimum_size() const {
	if (!fit_content) {
		return Size2();
	}
	_ensure_layout();
	return content_size;
}

void MarkdownTextLabel::clear() {
	if (text.is_empty()) {
		return;
	}
	text.clear();
	_invalidate(true);
}

void MarkdownTextLabel::parse_markdown(const String &p_markdown) {
	set_text(p_markdown);
}

void MarkdownTextLabel::append_text(const String &p_markdown) {
	if (p_markdown.is_empty()) {
		return;
	}
	text += p_markdown;
	_invalidate(true);
}

void MarkdownTextLabel::set_text(const String &p_text) {
	if (text == p_text) {
		return;
	}
	text = p_text;
	_invalidate(true);
}

String MarkdownTextLabel::get_text() const {
	return text;
}

String MarkdownTextLabel::get_parsed_text() const {
	_parse_markdown();
	return parsed_text;
}

void MarkdownTextLabel::set_horizontal_alignment(HorizontalAlignment p_alignment) {
	if (horizontal_alignment == p_alignment) {
		return;
	}
	horizontal_alignment = p_alignment;
	_invalidate(false);
}

HorizontalAlignment MarkdownTextLabel::get_horizontal_alignment() const {
	return horizontal_alignment;
}

void MarkdownTextLabel::set_autowrap_mode(TextServer::AutowrapMode p_mode) {
	if (autowrap_mode == p_mode) {
		return;
	}
	autowrap_mode = p_mode;
	_invalidate(false);
}

TextServer::AutowrapMode MarkdownTextLabel::get_autowrap_mode() const {
	return autowrap_mode;
}

void MarkdownTextLabel::set_autowrap_trim_flags(BitField<TextServer::LineBreakFlag> p_flags) {
	BitField<TextServer::LineBreakFlag> trim_flags = p_flags & TextServer::BREAK_TRIM_MASK;
	if (autowrap_flags_trim == trim_flags) {
		return;
	}
	autowrap_flags_trim = trim_flags;
	_invalidate(false);
}

BitField<TextServer::LineBreakFlag> MarkdownTextLabel::get_autowrap_trim_flags() const {
	return autowrap_flags_trim;
}

void MarkdownTextLabel::set_justification_flags(BitField<TextServer::JustificationFlag> p_flags) {
	if (justification_flags == p_flags) {
		return;
	}
	justification_flags = p_flags;
	_invalidate(false);
}

BitField<TextServer::JustificationFlag> MarkdownTextLabel::get_justification_flags() const {
	return justification_flags;
}

void MarkdownTextLabel::set_text_direction(TextDirection p_text_direction) {
	ERR_FAIL_COND((int)p_text_direction < -1 || (int)p_text_direction > 3);
	if (text_direction == p_text_direction) {
		return;
	}
	text_direction = p_text_direction;
	_invalidate(false);
}

Control::TextDirection MarkdownTextLabel::get_text_direction() const {
	return text_direction;
}

void MarkdownTextLabel::set_language(const String &p_language) {
	if (language == p_language) {
		return;
	}
	language = p_language;
	_invalidate(false);
}

String MarkdownTextLabel::get_language() const {
	return language;
}

void MarkdownTextLabel::set_structured_text_bidi_override(TextServer::StructuredTextParser p_parser) {
	if (st_parser == p_parser) {
		return;
	}
	st_parser = p_parser;
	_invalidate(false);
}

TextServer::StructuredTextParser MarkdownTextLabel::get_structured_text_bidi_override() const {
	return st_parser;
}

void MarkdownTextLabel::set_structured_text_bidi_override_options(const Array &p_args) {
	st_args = p_args;
	_invalidate(false);
}

Array MarkdownTextLabel::get_structured_text_bidi_override_options() const {
	return st_args;
}

void MarkdownTextLabel::set_fit_content(bool p_enabled) {
	if (fit_content == p_enabled) {
		return;
	}
	fit_content = p_enabled;
	update_minimum_size();
}

bool MarkdownTextLabel::is_fit_content_enabled() const {
	return fit_content;
}

int MarkdownTextLabel::get_content_height() const {
	_ensure_layout();
	return content_size.height;
}

int MarkdownTextLabel::get_content_width() const {
	_ensure_layout();
	return content_size.width;
}

void MarkdownTextLabel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &MarkdownTextLabel::clear);
	ClassDB::bind_method(D_METHOD("parse_markdown", "markdown"), &MarkdownTextLabel::parse_markdown);
	ClassDB::bind_method(D_METHOD("append_text", "markdown"), &MarkdownTextLabel::append_text);
	ClassDB::bind_method(D_METHOD("set_text", "text"), &MarkdownTextLabel::set_text);
	ClassDB::bind_method(D_METHOD("get_text"), &MarkdownTextLabel::get_text);
	ClassDB::bind_method(D_METHOD("get_parsed_text"), &MarkdownTextLabel::get_parsed_text);
	ClassDB::bind_method(D_METHOD("set_horizontal_alignment", "alignment"), &MarkdownTextLabel::set_horizontal_alignment);
	ClassDB::bind_method(D_METHOD("get_horizontal_alignment"), &MarkdownTextLabel::get_horizontal_alignment);
	ClassDB::bind_method(D_METHOD("set_autowrap_mode", "autowrap_mode"), &MarkdownTextLabel::set_autowrap_mode);
	ClassDB::bind_method(D_METHOD("get_autowrap_mode"), &MarkdownTextLabel::get_autowrap_mode);
	ClassDB::bind_method(D_METHOD("set_autowrap_trim_flags", "autowrap_trim_flags"), &MarkdownTextLabel::set_autowrap_trim_flags);
	ClassDB::bind_method(D_METHOD("get_autowrap_trim_flags"), &MarkdownTextLabel::get_autowrap_trim_flags);
	ClassDB::bind_method(D_METHOD("set_justification_flags", "justification_flags"), &MarkdownTextLabel::set_justification_flags);
	ClassDB::bind_method(D_METHOD("get_justification_flags"), &MarkdownTextLabel::get_justification_flags);
	ClassDB::bind_method(D_METHOD("set_text_direction", "direction"), &MarkdownTextLabel::set_text_direction);
	ClassDB::bind_method(D_METHOD("get_text_direction"), &MarkdownTextLabel::get_text_direction);
	ClassDB::bind_method(D_METHOD("set_language", "language"), &MarkdownTextLabel::set_language);
	ClassDB::bind_method(D_METHOD("get_language"), &MarkdownTextLabel::get_language);
	ClassDB::bind_method(D_METHOD("set_structured_text_bidi_override", "parser"), &MarkdownTextLabel::set_structured_text_bidi_override);
	ClassDB::bind_method(D_METHOD("get_structured_text_bidi_override"), &MarkdownTextLabel::get_structured_text_bidi_override);
	ClassDB::bind_method(D_METHOD("set_structured_text_bidi_override_options", "args"), &MarkdownTextLabel::set_structured_text_bidi_override_options);
	ClassDB::bind_method(D_METHOD("get_structured_text_bidi_override_options"), &MarkdownTextLabel::get_structured_text_bidi_override_options);
	ClassDB::bind_method(D_METHOD("set_fit_content", "enabled"), &MarkdownTextLabel::set_fit_content);
	ClassDB::bind_method(D_METHOD("is_fit_content_enabled"), &MarkdownTextLabel::is_fit_content_enabled);
	ClassDB::bind_method(D_METHOD("get_content_height"), &MarkdownTextLabel::get_content_height);
	ClassDB::bind_method(D_METHOD("get_content_width"), &MarkdownTextLabel::get_content_width);

	ADD_SIGNAL(MethodInfo("meta_clicked", PropertyInfo(Variant::NIL, "meta", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "text", PROPERTY_HINT_MULTILINE_TEXT), "set_text", "get_text");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "horizontal_alignment", PROPERTY_HINT_ENUM, "Left,Center,Right,Fill"), "set_horizontal_alignment", "get_horizontal_alignment");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "autowrap_mode", PROPERTY_HINT_ENUM, "Off,Arbitrary,Word,Word (Smart)"), "set_autowrap_mode", "get_autowrap_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "autowrap_trim_flags", PROPERTY_HINT_FLAGS,
						 vformat("Trim Spaces After Break:%d,Trim Spaces Before Break:%d",
								 TextServer::BREAK_TRIM_START_EDGE_SPACES,
								 TextServer::BREAK_TRIM_END_EDGE_SPACES)),
			"set_autowrap_trim_flags", "get_autowrap_trim_flags");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "justification_flags", PROPERTY_HINT_FLAGS,
						 "Kashida Justification:1,Word Justification:2,Justify Only After Last Tab:8,Skip Last Line:32,Skip Last Line With Visible Characters:64,Do Not Skip Single Line:128"),
			"set_justification_flags", "get_justification_flags");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fit_content"), "set_fit_content", "is_fit_content_enabled");

	ADD_GROUP("BiDi", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "text_direction", PROPERTY_HINT_ENUM, "Auto,Left-to-Right,Right-to-Left,Inherited"), "set_text_direction", "get_text_direction");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "language", PROPERTY_HINT_LOCALE_ID, ""), "set_language", "get_language");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "structured_text_bidi_override", PROPERTY_HINT_ENUM, "Default,URI,File,Email,List,None,Custom"),
			"set_structured_text_bidi_override", "get_structured_text_bidi_override");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "structured_text_bidi_override_options"), "set_structured_text_bidi_override_options", "get_structured_text_bidi_override_options");

	BIND_THEME_ITEM_CUSTOM(Theme::DATA_TYPE_STYLEBOX, MarkdownTextLabel, normal_style, "normal");
	BIND_THEME_ITEM_CUSTOM(Theme::DATA_TYPE_STYLEBOX, MarkdownTextLabel, focus_style, "focus");
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT, MarkdownTextLabel, normal_font);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT, MarkdownTextLabel, bold_font);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT, MarkdownTextLabel, italics_font);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT, MarkdownTextLabel, bold_italics_font);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT, MarkdownTextLabel, mono_font);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT_SIZE, MarkdownTextLabel, normal_font_size);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT_SIZE, MarkdownTextLabel, bold_font_size);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT_SIZE, MarkdownTextLabel, italics_font_size);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT_SIZE, MarkdownTextLabel, bold_italics_font_size);
	BIND_THEME_ITEM(Theme::DATA_TYPE_FONT_SIZE, MarkdownTextLabel, mono_font_size);
	BIND_THEME_ITEM(Theme::DATA_TYPE_COLOR, MarkdownTextLabel, font_color);
	BIND_THEME_ITEM(Theme::DATA_TYPE_COLOR, MarkdownTextLabel, link_color);
	BIND_THEME_ITEM(Theme::DATA_TYPE_COLOR, MarkdownTextLabel, code_bg_color);
	BIND_THEME_ITEM(Theme::DATA_TYPE_COLOR, MarkdownTextLabel, quote_bar_color);
	BIND_THEME_ITEM(Theme::DATA_TYPE_COLOR, MarkdownTextLabel, table_border_color);
	BIND_THEME_ITEM(Theme::DATA_TYPE_COLOR, MarkdownTextLabel, font_shadow_color);
	BIND_THEME_ITEM_CUSTOM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, font_shadow_offset.x, "shadow_offset_x");
	BIND_THEME_ITEM_CUSTOM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, font_shadow_offset.y, "shadow_offset_y");
	BIND_THEME_ITEM(Theme::DATA_TYPE_COLOR, MarkdownTextLabel, font_outline_color);
	BIND_THEME_ITEM_CUSTOM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, outline_size, "outline_size");
	BIND_THEME_ITEM_CUSTOM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, shadow_outline_size, "shadow_outline_size");
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, line_separation);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, paragraph_separation);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, block_separation);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, list_indent);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, quote_indent);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, quote_bar_width);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, code_padding);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, table_h_separation);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, MarkdownTextLabel, table_v_separation);
}

MarkdownTextLabel::MarkdownTextLabel(const String &p_text) {
	set_clip_contents(true);
	set_focus_mode(FOCUS_ACCESSIBILITY);
	set_mouse_filter(MOUSE_FILTER_PASS);
	set_text(p_text);
}
