/**************************************************************************/
/*  markdown_text_label.h                                                 */
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

#pragma once

#include "scene/gui/control.h"
#include "scene/resources/text_paragraph.h"

#include "thirdparty/md4c/md4c.h"

class Texture2D;

class MarkdownTextLabel : public Control {
	GDCLASS(MarkdownTextLabel, Control);

private:
	enum BlockType {
		BLOCK_PARAGRAPH,
		BLOCK_HEADING,
		BLOCK_CODE,
		BLOCK_HTML,
		BLOCK_HR,
		BLOCK_TABLE,
	};

	struct InlineStyle {
		bool bold = false;
		bool italics = false;
		bool mono = false;
		bool underline = false;
		bool strikethrough = false;
		bool link = false;
		String meta;
		String tooltip;
	};

	struct TextRun {
		String text;
		InlineStyle style;
		Ref<Texture2D> image;
		Size2 image_size;
	};

	struct DrawRange {
		int from = 0;
		int to = 0;
		bool code = false;
		bool underline = false;
		bool strikethrough = false;
		bool link = false;
		String meta;
		String tooltip;
	};

	struct ImageObject {
		int key = 0;
		Ref<Texture2D> texture;
		Color color = Color(1, 1, 1, 1);
	};

	struct RenderParagraph {
		Ref<TextParagraph> paragraph;
		Vector<DrawRange> ranges;
		Vector<ImageObject> images;
		float width = -1.0;
		Size2 size;
	};

	struct TableCell {
		Vector<TextRun> runs;
		HorizontalAlignment alignment = HORIZONTAL_ALIGNMENT_LEFT;
		bool header = false;
		RenderParagraph render;
		Rect2 rect;
	};

	struct TableRow {
		Vector<TableCell> cells;
		float height = 0.0;
	};

	struct Block {
		BlockType type = BLOCK_PARAGRAPH;
		Vector<TextRun> runs;
		Vector<TableRow> rows;
		RenderParagraph render;
		Rect2 rect;
		int heading_level = 0;
		int quote_depth = 0;
		int list_depth = 0;
		String prefix;
		HorizontalAlignment alignment = HORIZONTAL_ALIGNMENT_LEFT;
	};

	struct ListState {
		bool ordered = false;
		int index = 0;
	};

	struct LinkState {
		String meta;
		String tooltip;
	};

	struct ImageState {
		String source;
		String alt_text;
	};

	struct ParseState {
		MarkdownTextLabel *label = nullptr;
		Vector<ListState> lists;
		Vector<LinkState> links;
		Vector<ImageState> images;
		Vector<bool> math_display;
		int current_block = -1;
		int current_row = -1;
		int current_cell = -1;
		int quote_depth = 0;
		int bold_depth = 0;
		int italics_depth = 0;
		int mono_depth = 0;
		int underline_depth = 0;
		int strikethrough_depth = 0;
		bool in_table = false;
		bool current_cell_header = false;
		String pending_prefix;
	};

	struct LinkRect {
		Rect2 rect;
		String meta;
		String tooltip;
	};

	mutable Vector<Block> blocks;
	mutable Vector<LinkRect> link_rects;

	String text;
	mutable String parsed_text;
	HorizontalAlignment horizontal_alignment = HORIZONTAL_ALIGNMENT_LEFT;
	TextServer::AutowrapMode autowrap_mode = TextServer::AUTOWRAP_WORD_SMART;
	BitField<TextServer::LineBreakFlag> autowrap_flags_trim = TextServer::BREAK_TRIM_START_EDGE_SPACES | TextServer::BREAK_TRIM_END_EDGE_SPACES;
	BitField<TextServer::JustificationFlag> justification_flags = TextServer::JUSTIFICATION_WORD_BOUND | TextServer::JUSTIFICATION_KASHIDA |
			TextServer::JUSTIFICATION_SKIP_LAST_LINE | TextServer::JUSTIFICATION_DO_NOT_SKIP_SINGLE_LINE;
	TextDirection text_direction = TEXT_DIRECTION_AUTO;
	TextServer::StructuredTextParser st_parser = TextServer::STRUCTURED_TEXT_DEFAULT;
	Array st_args;
	String language;
	bool fit_content = false;

	mutable bool parsed_dirty = true;
	mutable bool layout_dirty = true;
	mutable Size2 content_size;
	mutable int next_object_key = 1;

	struct ThemeCache {
		Ref<StyleBox> normal_style;
		Ref<StyleBox> focus_style;
		Ref<Font> normal_font;
		Ref<Font> bold_font;
		Ref<Font> italics_font;
		Ref<Font> bold_italics_font;
		Ref<Font> mono_font;
		int normal_font_size = 0;
		int bold_font_size = 0;
		int italics_font_size = 0;
		int bold_italics_font_size = 0;
		int mono_font_size = 0;
		Color font_color;
		Color link_color;
		Color code_bg_color;
		Color quote_bar_color;
		Color table_border_color;
		Color font_shadow_color;
		Point2 font_shadow_offset;
		Color font_outline_color;
		int outline_size = 0;
		int shadow_outline_size = 0;
		int line_separation = 0;
		int paragraph_separation = 0;
		int block_separation = 0;
		int list_indent = 0;
		int quote_indent = 0;
		int quote_bar_width = 0;
		int code_padding = 0;
		int table_h_separation = 0;
		int table_v_separation = 0;
	} theme_cache;

	void _invalidate(bool p_reparse);
	void _parse_markdown() const;
	void _ensure_layout() const;
	void _shape_block(Block &r_block, float p_width) const;
	void _shape_table(Block &r_block, float p_width) const;
	RenderParagraph _make_paragraph(const Vector<TextRun> &p_runs, const String &p_prefix, HorizontalAlignment p_alignment, int p_heading_level,
			bool p_code_block, bool p_force_bold, float p_width) const;
	InlineStyle _current_style(const ParseState &p_state) const;
	void _add_text_run(ParseState &r_state, const String &p_text) const;
	void _add_image_run(ParseState &r_state, const Ref<Texture2D> &p_texture, const String &p_source, const String &p_alt_text) const;
	Block &_add_block(BlockType p_type, ParseState &r_state) const;
	Block *_get_current_block(ParseState &r_state) const;
	TableCell *_get_current_cell(ParseState &r_state) const;
	void _append_plain_text(const String &p_text) const;
	Ref<Font> _get_font_for_style(const InlineStyle &p_style, bool p_force_bold) const;
	int _get_font_size_for_style(const InlineStyle &p_style, int p_heading_level) const;
	float _get_paragraph_width(float p_available_width) const;
	BitField<TextServer::LineBreakFlag> _get_line_break_flags() const;
	TextServer::Direction _get_text_server_direction() const;
	Vector2 _get_line_top(const RenderParagraph &p_render, int p_line, const Vector2 &p_position) const;
	Vector2 _get_line_baseline(const RenderParagraph &p_render, int p_line, const Vector2 &p_position) const;
	void _draw_render_paragraph(const RenderParagraph &p_render, const Vector2 &p_position, bool p_code_background) const;
	void _collect_link_rects(const RenderParagraph &p_render, const Vector2 &p_position) const;
	const ImageObject *_find_image(const RenderParagraph &p_render, int p_key) const;
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	static String _markdown_text_to_string(MD_TEXTTYPE p_type, const MD_CHAR *p_text, MD_SIZE p_size);
	static String _markdown_attribute_to_string(const MD_ATTRIBUTE &p_attribute);
	static int _enter_block(MD_BLOCKTYPE p_type, void *p_detail, void *p_userdata);
	static int _leave_block(MD_BLOCKTYPE p_type, void *p_detail, void *p_userdata);
	static int _enter_span(MD_SPANTYPE p_type, void *p_detail, void *p_userdata);
	static int _leave_span(MD_SPANTYPE p_type, void *p_detail, void *p_userdata);
	static int _text_callback(MD_TEXTTYPE p_type, const MD_CHAR *p_text, MD_SIZE p_size, void *p_userdata);

protected:
	virtual void _update_theme_item_cache() override;
	void _notification(int p_what);
	static void _bind_methods();

public:
	void clear();
	void parse_markdown(const String &p_markdown);
	void append_text(const String &p_markdown);

	void set_text(const String &p_text);
	String get_text() const;
	String get_parsed_text() const;

	void set_horizontal_alignment(HorizontalAlignment p_alignment);
	HorizontalAlignment get_horizontal_alignment() const;

	void set_autowrap_mode(TextServer::AutowrapMode p_mode);
	TextServer::AutowrapMode get_autowrap_mode() const;

	void set_autowrap_trim_flags(BitField<TextServer::LineBreakFlag> p_flags);
	BitField<TextServer::LineBreakFlag> get_autowrap_trim_flags() const;

	void set_justification_flags(BitField<TextServer::JustificationFlag> p_flags);
	BitField<TextServer::JustificationFlag> get_justification_flags() const;

	void set_text_direction(TextDirection p_text_direction);
	TextDirection get_text_direction() const;

	void set_language(const String &p_language);
	String get_language() const;

	void set_structured_text_bidi_override(TextServer::StructuredTextParser p_parser);
	TextServer::StructuredTextParser get_structured_text_bidi_override() const;

	void set_structured_text_bidi_override_options(const Array &p_args);
	Array get_structured_text_bidi_override_options() const;

	void set_fit_content(bool p_enabled);
	bool is_fit_content_enabled() const;

	int get_content_height() const;
	int get_content_width() const;

	virtual Size2 get_minimum_size() const override;
	virtual CursorShape get_cursor_shape(const Point2 &p_pos) const override;
	virtual String get_tooltip(const Point2 &p_pos) const override;

	MarkdownTextLabel(const String &p_text = String());
};
