/**************************************************************************/
/*  grid_container.cpp                                                    */
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

#include "grid_container.h"

#include "core/object/class_db.h"
#include "core/templates/rb_map.h"
#include "core/templates/rb_set.h"
#include "scene/theme/theme_db.h"

// Returns the fractional row/column index whose main-axis slot contains
// `p_pos`. `p_begins` must be sorted in ascending order.
static float _skew_fractional_index(const Vector<float> &p_begins, float p_pos) {
	for (int i = 0; i < p_begins.size(); i++) {
		const float next = (i + 1 < p_begins.size()) ? p_begins[i + 1] : (p_begins[i] + (i > 0 ? p_begins[i] - p_begins[i - 1] : 0.0f));
		if (p_pos < next) {
			const float pitch = next - p_begins[i];
			return i + (pitch > 0.0f ? CLAMP((p_pos - p_begins[i]) / pitch, 0.0f, 1.0f) : 0.0f);
		}
	}
	return p_begins.size();
}

// Anchors the skew diagonal to the visible area: `r_progress` becomes the
// fractional index of the first visible row/column and `r_span` the cross-axis
// range covered by the visible diagonal (never larger than the full extent).
static void _skew_apply_visible_window(const Vector<float> &p_begins, float p_vis_begin, float p_vis_end, float p_skew, float p_extent, float &r_progress, float &r_span) {
	r_progress = 0.0f;
	r_span = p_extent;
	if (p_begins.is_empty() || Math::is_zero_approx(p_skew) || p_vis_end <= p_vis_begin) {
		return;
	}
	const float first_visible = _skew_fractional_index(p_begins, p_vis_begin);
	const float last_visible = _skew_fractional_index(p_begins, p_vis_end);
	if (last_visible <= first_visible) {
		return;
	}
	r_progress = first_visible;
	const int visible_capacity = MAX(1, (int)Math::ceil(last_visible - first_visible));
	r_span = MIN(p_extent, Math::abs(p_skew) * MAX(visible_capacity - 1, 0));
}

void GridContainer::_resort() {
	RBMap<int, int> col_minw; // Max of min_width of all controls in each col (indexed by col).
	RBMap<int, int> row_minh; // Max of min_height of all controls in each row (indexed by row).
	RBMap<int, int> col_maxw; // Min positive max_width of all controls in each col (indexed by col).
	RBMap<int, int> row_maxh; // Min positive max_height of all controls in each row (indexed by row).
	RBMap<int, int> col_fixed_size; // Final fixed width for non-expanded columns.
	RBMap<int, int> row_fixed_size; // Final fixed height for non-expanded rows.
	RBSet<int> col_expanded; // Columns which have the SIZE_EXPAND flag set.
	RBSet<int> row_expanded; // Rows which have the SIZE_EXPAND flag set.

	// Compute the per-column/per-row data.
	int valid_controls_index = 0;
	Size2i size = Size2i(get_size());
	Size2i combined_max_size = Size2i(get_combined_maximum_size());

	for (int i = 0; i < get_child_count(); i++) {
		Control *c = as_sortable_control(get_child(i));
		if (!c) {
			continue;
		}

		int row = valid_controls_index / columns;
		int col = valid_controls_index % columns;
		valid_controls_index++;

		if (is_propagating_maximum_size()) {
			c->set_parent_maximum_size_cache(combined_max_size);
		}
		Size2i ms = c->get_bound_minimum_size();
		Size2 max_size = c->get_combined_maximum_size();
		if (col_minw.has(col)) {
			col_minw[col] = MAX(col_minw[col], ms.width);
		} else {
			col_minw[col] = ms.width;
		}
		if (row_minh.has(row)) {
			row_minh[row] = MAX(row_minh[row], ms.height);
		} else {
			row_minh[row] = ms.height;
		}

		int max_width = int(max_size.width) >= 0 ? int(max_size.width) : (combined_max_size.width >= 0 ? combined_max_size.width : size.width);
		if (col_maxw.has(col)) {
			col_maxw[col] = MAX(col_maxw[col], max_width);
		} else {
			col_maxw[col] = max_width;
		}

		int max_height = int(max_size.height) >= 0 ? int(max_size.height) : (combined_max_size.height >= 0 ? combined_max_size.height : size.height);
		if (row_maxh.has(row)) {
			row_maxh[row] = MAX(row_maxh[row], max_height);
		} else {
			row_maxh[row] = max_height;
		}

		if (c->get_h_size_flags().has_flag(SIZE_EXPAND)) {
			col_expanded.insert(col);
		}
		if (c->get_v_size_flags().has_flag(SIZE_EXPAND)) {
			row_expanded.insert(row);
		}
	}

	for (const KeyValue<int, int> &E : col_maxw) {
		if (col_maxw[E.key] < col_minw[E.key]) {
			col_maxw[E.key] = col_minw[E.key];
		}
	}

	for (const KeyValue<int, int> &E : row_maxh) {
		if (row_maxh[E.key] < row_minh[E.key]) {
			row_maxh[E.key] = row_minh[E.key];
		}
	}

	int max_col = MIN(valid_controls_index, columns);
	int max_row = std::ceil((float)valid_controls_index / (float)columns);

	const float skew_x_extent = Math::abs(item_skew.x) * MAX(max_row - 1, 0);
	const float skew_y_extent = Math::abs(item_skew.y) * MAX(max_col - 1, 0);

	// The skew diagonal is anchored to the container's visible area (its own
	// rect clipped by the window and any clipping ancestors such as a
	// ScrollContainer), so adding, removing or scrolling items never changes
	// the offsets of the visible rows and columns. Compute the cross-axis range
	// covered by the visible diagonal from the minimum size pitches; when the
	// container is fully visible this is the whole skew extent.
	float skew_x_span = skew_x_extent;
	float skew_y_span = skew_y_extent;
	if (!item_skew.is_zero_approx() && valid_controls_index > 0) {
		Rect2 visible_rect;
		if (get_visible_canvas_rect(visible_rect)) {
			const Rect2 own_rect = get_global_rect();
			Vector<float> row_min_begins;
			Vector<float> col_min_begins;
			row_min_begins.resize(max_row);
			col_min_begins.resize(max_col);
			float acc = 0.0f;
			for (int r = 0; r < max_row; r++) {
				row_min_begins.write[r] = acc;
				acc += (row_minh.has(r) ? row_minh[r] : 0) + theme_cache.v_separation;
			}
			acc = 0.0f;
			for (int c = 0; c < max_col; c++) {
				col_min_begins.write[c] = acc;
				acc += (col_minw.has(c) ? col_minw[c] : 0) + theme_cache.h_separation;
			}
			float progress = 0.0f;
			if (!Math::is_zero_approx(item_skew.x)) {
				_skew_apply_visible_window(row_min_begins, visible_rect.position.y - own_rect.position.y, visible_rect.get_end().y - own_rect.position.y, item_skew.x, skew_x_extent, progress, skew_x_span);
			}
			if (!Math::is_zero_approx(item_skew.y)) {
				_skew_apply_visible_window(col_min_begins, visible_rect.position.x - own_rect.position.x, visible_rect.get_end().x - own_rect.position.x, item_skew.y, skew_y_extent, progress, skew_y_span);
			}
		}
	}

	// Consider all empty columns expanded.
	for (int i = valid_controls_index; i < columns; i++) {
		col_expanded.insert(i);
	}

	// Evaluate the remaining space for expanded columns/rows. The skew span is
	// reserved on the cross axis so the visible diagonal stays inside the grid.
	Size2 remaining_space = get_size();
	remaining_space.width = MAX(0.0f, remaining_space.width - skew_x_span);
	remaining_space.height = MAX(0.0f, remaining_space.height - skew_y_span);
	for (const KeyValue<int, int> &E : col_minw) {
		if (!col_expanded.has(E.key)) {
			remaining_space.width -= E.value;
		}
	}

	for (const KeyValue<int, int> &E : row_minh) {
		if (!row_expanded.has(E.key)) {
			remaining_space.height -= E.value;
		}
	}
	remaining_space.height -= theme_cache.v_separation * MAX(max_row - 1, 0);
	remaining_space.width -= theme_cache.h_separation * MAX(max_col - 1, 0);

	bool can_fit = false;
	while (!can_fit && col_expanded.size() > 0) {
		// Check if all minwidth constraints are OK if we use the remaining space.
		can_fit = true;
		int max_index = col_expanded.front()->get();
		for (const int &E : col_expanded) {
			if (col_minw[E] > col_minw[max_index]) {
				max_index = E;
			}
			if (can_fit && (remaining_space.width / col_expanded.size()) < col_minw[E]) {
				can_fit = false;
			}
		}

		// If not, the column with maximum minwidth is not expanded.
		if (!can_fit) {
			col_expanded.erase(max_index);
			remaining_space.width -= col_minw[max_index];
			col_fixed_size[max_index] = col_minw[max_index];
		}
	}

	can_fit = false;
	while (!can_fit && row_expanded.size() > 0) {
		// Check if all minheight constraints are OK if we use the remaining space.
		can_fit = true;
		int max_index = row_expanded.front()->get();
		for (const int &E : row_expanded) {
			if (row_minh[E] > row_minh[max_index]) {
				max_index = E;
			}
			if (can_fit && (remaining_space.height / row_expanded.size()) < row_minh[E]) {
				can_fit = false;
			}
		}

		// If not, the row with maximum minheight is not expanded.
		if (!can_fit) {
			row_expanded.erase(max_index);
			remaining_space.height -= row_minh[max_index];
			row_fixed_size[max_index] = row_minh[max_index];
		}
	}

	bool max_fit = false;
	while (!max_fit && col_expanded.size() > 0) {
		max_fit = true;
		int capped_col = -1;
		for (const int &E : col_expanded) {
			if (col_maxw.has(E) && (remaining_space.width / col_expanded.size()) > col_maxw[E]) {
				capped_col = E;
				max_fit = false;
				break;
			}
		}

		if (capped_col >= 0) {
			col_expanded.erase(capped_col);
			remaining_space.width -= col_maxw[capped_col];
			col_fixed_size[capped_col] = col_maxw[capped_col];
		}
	}

	max_fit = false;
	while (!max_fit && row_expanded.size() > 0) {
		max_fit = true;
		int capped_row = -1;
		for (const int &E : row_expanded) {
			if (row_maxh.has(E) && (remaining_space.height / row_expanded.size()) > row_maxh[E]) {
				capped_row = E;
				max_fit = false;
				break;
			}
		}

		if (capped_row >= 0) {
			row_expanded.erase(capped_row);
			remaining_space.height -= row_maxh[capped_row];
			row_fixed_size[capped_row] = row_maxh[capped_row];
		}
	}

	// Finally, fit the nodes.
	int col_remaining_pixel = 0;
	int col_expand = 0;
	if (col_expanded.size() > 0) {
		col_expand = remaining_space.width / col_expanded.size();
		col_remaining_pixel = remaining_space.width - col_expanded.size() * col_expand;
	}

	int row_remaining_pixel = 0;
	int row_expand = 0;
	if (row_expanded.size() > 0) {
		row_expand = remaining_space.height / row_expanded.size();
		row_remaining_pixel = remaining_space.height - row_expanded.size() * row_expand;
	}

	bool rtl = is_layout_rtl();

	float col_ofs = 0.0f;
	float row_ofs = 0.0f;

	// Calculate the index of rows and columns that receive the remaining pixel.
	int col_remaining_pixel_index = 0;
	for (int i = 0; i < max_col; i++) {
		if (col_remaining_pixel == 0) {
			break;
		}
		if (col_expanded.has(i)) {
			col_remaining_pixel_index = i + 1;
			col_remaining_pixel--;
		}
	}
	int row_remaining_pixel_index = 0;
	for (int i = 0; i < max_row; i++) {
		if (row_remaining_pixel == 0) {
			break;
		}
		if (row_expanded.has(i)) {
			row_remaining_pixel_index = i + 1;
			row_remaining_pixel--;
		}
	}

	// Compute the final size of each column and row once, so the placement loop,
	// the grid extents and the visible-area skew anchoring all agree.
	Vector<int> col_sizes;
	Vector<int> row_sizes;
	col_sizes.resize(max_col);
	row_sizes.resize(max_row);

	float grid_width = skew_x_span + theme_cache.h_separation * MAX(max_col - 1, 0);
	for (int i = 0; i < max_col; i++) {
		int col_size = col_expanded.has(i) ? col_expand : (col_fixed_size.has(i) ? col_fixed_size[i] : (col_minw.has(i) ? col_minw[i] : 0));
		if (col_expanded.has(i) && i < col_remaining_pixel_index) {
			col_size++;
		}
		col_sizes.write[i] = col_size;
		grid_width += col_size;
	}

	float grid_height = skew_y_span + theme_cache.v_separation * MAX(max_row - 1, 0);
	for (int i = 0; i < max_row; i++) {
		int row_size = row_expanded.has(i) ? row_expand : (row_fixed_size.has(i) ? row_fixed_size[i] : (row_minh.has(i) ? row_minh[i] : 0));
		if (row_expanded.has(i) && i < row_remaining_pixel_index) {
			row_size++;
		}
		row_sizes.write[i] = row_size;
		grid_height += row_size;
	}

	float horizontal_alignment_offset = 0.0f;
	switch (horizontal_alignment) {
		case ALIGNMENT_CENTER: {
			horizontal_alignment_offset = MAX(0.0f, get_size().width - grid_width) * 0.5f;
		} break;
		case ALIGNMENT_END: {
			horizontal_alignment_offset = MAX(0.0f, get_size().width - grid_width);
		} break;
		case ALIGNMENT_BEGIN:
		default:
			break;
	}
	if (rtl) {
		horizontal_alignment_offset = -horizontal_alignment_offset;
	}

	float vertical_alignment_offset = 0.0f;
	switch (vertical_alignment) {
		case ALIGNMENT_CENTER: {
			vertical_alignment_offset = MAX(0.0f, get_size().height - grid_height) * 0.5f;
		} break;
		case ALIGNMENT_END: {
			vertical_alignment_offset = MAX(0.0f, get_size().height - grid_height);
		} break;
		case ALIGNMENT_BEGIN:
		default:
			break;
	}

	// Compute the fractional index of the first visible row/column so the skew
	// offsets can be made relative to it. Column positions are measured from
	// the right edge in RTL layouts to keep the indices ascending.
	float skew_row_progress = 0.0f;
	float skew_col_progress = 0.0f;
	if (!item_skew.is_zero_approx() && valid_controls_index > 0) {
		Rect2 visible_rect;
		if (get_visible_canvas_rect(visible_rect)) {
			const Rect2 own_rect = get_global_rect();
			if (!Math::is_zero_approx(item_skew.x)) {
				Vector<float> row_begins;
				row_begins.resize(max_row);
				float acc = vertical_alignment_offset;
				for (int r = 0; r < max_row; r++) {
					row_begins.write[r] = acc;
					acc += (row_expanded.has(r) ? row_expand : row_minh[r]) + theme_cache.v_separation;
					if (row_expanded.has(r) && r < row_remaining_pixel_index) {
						acc += 1.0f;
					}
				}
				skew_row_progress = _skew_fractional_index(row_begins, visible_rect.position.y - own_rect.position.y);
			}
			if (!Math::is_zero_approx(item_skew.y)) {
				Vector<float> col_begins;
				col_begins.resize(max_col);
				float acc = rtl ? get_size().width + horizontal_alignment_offset : horizontal_alignment_offset;
				for (int c = 0; c < max_col; c++) {
					col_begins.write[c] = rtl ? get_size().width - acc : acc;
					acc += rtl ? -(col_sizes[c] + theme_cache.h_separation) : (col_sizes[c] + theme_cache.h_separation);
				}
				const float vis_x_begin = visible_rect.position.x - own_rect.position.x;
				const float vis_x_end = visible_rect.get_end().x - own_rect.position.x;
				skew_col_progress = _skew_fractional_index(col_begins, rtl ? get_size().width - vis_x_end : vis_x_begin);
			}
		}
	}
	const float skew_x_base = item_skew.x < 0.0f ? skew_x_span : 0.0f;
	const float skew_y_base = item_skew.y < 0.0f ? skew_y_span : 0.0f;

	int accumulated_width = 0;
	int accumulated_height = 0;
	valid_controls_index = 0;
	for (int i = 0; i < get_child_count(); i++) {
		Control *c = as_sortable_control(get_child(i));
		if (!c) {
			continue;
		}
		int row = valid_controls_index / columns;
		int col = valid_controls_index % columns;
		valid_controls_index++;

		if (col == 0) {
			if (rtl) {
				col_ofs = get_size().width + horizontal_alignment_offset;
			} else {
				col_ofs = horizontal_alignment_offset;
			}
			if (row == 0) {
				row_ofs = vertical_alignment_offset;
			} else {
				row_ofs += (row_expanded.has(row - 1) ? row_expand : row_minh[row - 1]) + theme_cache.v_separation;

				if (row_expanded.has(row - 1) && row - 1 < row_remaining_pixel_index) {
					// Apply the remaining pixel of the previous row.
					row_ofs++;
				}
			}
		}

		Size2 s(col_sizes[col], row_sizes[row]);

		if (is_propagating_maximum_size()) {
			Size2 ms = combined_max_size;
			if (ms.width >= 0) {
				ms.width -= skew_x_span;
				ms.width -= accumulated_width;
				ms.width = MAX(ms.width, 0);
			}
			if (ms.height >= 0) {
				ms.height -= skew_y_span;
				ms.height -= accumulated_height;
				ms.height = MAX(ms.height, 0);
			}
			c->set_parent_maximum_size_cache(ms);
		}

		// The skew offset is relative to the first visible row/column, so the
		// diagonal keeps a fixed shape inside the visible area regardless of
		// item count or scroll position.
		const Vector2 skew_offset(skew_x_base + item_skew.x * (row - skew_row_progress), skew_y_base + item_skew.y * (col - skew_col_progress));
		if (rtl) {
			Point2 p(col_ofs - s.width + skew_offset.x, row_ofs + skew_offset.y);
			fit_child_in_rect(c, Rect2(p, s));
			col_ofs -= s.width + theme_cache.h_separation;
		} else {
			Point2 p(col_ofs + skew_offset.x, row_ofs + skew_offset.y);
			fit_child_in_rect(c, Rect2(p, s));
			col_ofs += s.width + theme_cache.h_separation;
		}

		if (col == columns - 1) {
			accumulated_height += s.height + theme_cache.v_separation;
			accumulated_width = 0;
		} else {
			accumulated_width += s.width + theme_cache.h_separation;
		}
	}
}

void GridContainer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_SORT_CHILDREN: {
			_resort();
			update_minimum_size();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (!item_skew.is_zero_approx()) {
				// The skew diagonal is anchored to the visible area; follow any
				// movement of the container or its ancestors (e.g. scrolling).
				queue_sort();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			update_minimum_size();
		} break;

		case NOTIFICATION_TRANSLATION_CHANGED:
		case NOTIFICATION_LAYOUT_DIRECTION_CHANGED: {
			queue_sort();
		} break;
	}
}

void GridContainer::set_columns(int p_columns) {
	ERR_FAIL_COND(p_columns < 1);

	if (columns == p_columns) {
		return;
	}

	columns = p_columns;
	queue_sort();
	update_minimum_size();
}

int GridContainer::get_columns() const {
	return columns;
}

void GridContainer::set_item_skew(const Vector2 &p_offset) {
	ERR_FAIL_COND(!p_offset.is_finite());
	if (item_skew == p_offset) {
		return;
	}
	item_skew = p_offset;
	set_notify_transform(!item_skew.is_zero_approx());
	update_minimum_size();
	queue_sort();
}

Vector2 GridContainer::get_item_skew() const {
	return item_skew;
}

void GridContainer::set_horizontal_alignment(AlignmentMode p_alignment) {
	if (horizontal_alignment == p_alignment) {
		return;
	}
	horizontal_alignment = p_alignment;
	queue_sort();
}

GridContainer::AlignmentMode GridContainer::get_horizontal_alignment() const {
	return horizontal_alignment;
}

void GridContainer::set_vertical_alignment(AlignmentMode p_alignment) {
	if (vertical_alignment == p_alignment) {
		return;
	}
	vertical_alignment = p_alignment;
	queue_sort();
}

GridContainer::AlignmentMode GridContainer::get_vertical_alignment() const {
	return vertical_alignment;
}

int GridContainer::get_h_separation() const {
	return theme_cache.h_separation;
}

void GridContainer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_columns", "columns"), &GridContainer::set_columns);
	ClassDB::bind_method(D_METHOD("get_columns"), &GridContainer::get_columns);
	ClassDB::bind_method(D_METHOD("set_item_skew", "offset"), &GridContainer::set_item_skew);
	ClassDB::bind_method(D_METHOD("get_item_skew"), &GridContainer::get_item_skew);
	ClassDB::bind_method(D_METHOD("set_horizontal_alignment", "alignment"), &GridContainer::set_horizontal_alignment);
	ClassDB::bind_method(D_METHOD("get_horizontal_alignment"), &GridContainer::get_horizontal_alignment);
	ClassDB::bind_method(D_METHOD("set_vertical_alignment", "alignment"), &GridContainer::set_vertical_alignment);
	ClassDB::bind_method(D_METHOD("get_vertical_alignment"), &GridContainer::get_vertical_alignment);

	BIND_ENUM_CONSTANT(ALIGNMENT_BEGIN);
	BIND_ENUM_CONSTANT(ALIGNMENT_CENTER);
	BIND_ENUM_CONSTANT(ALIGNMENT_END);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "columns", PROPERTY_HINT_RANGE, "1,1024,1"), "set_columns", "get_columns");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "item_skew", PROPERTY_HINT_RANGE, "-256,256,0.1,or_less,or_greater,suffix:px"), "set_item_skew", "get_item_skew");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "horizontal_alignment", PROPERTY_HINT_ENUM, "Begin,Center,End"), "set_horizontal_alignment", "get_horizontal_alignment");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "vertical_alignment", PROPERTY_HINT_ENUM, "Begin,Center,End"), "set_vertical_alignment", "get_vertical_alignment");

	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, GridContainer, h_separation);
	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, GridContainer, v_separation);
}

Size2 GridContainer::_get_minimum_size(bool p_use_desired_sizes) const {
	RBMap<int, int> col_minw;
	RBMap<int, int> row_minh;

	int max_row = 0;
	int max_col = 0;

	int valid_controls_index = 0;
	for (int i = 0; i < get_child_count(); i++) {
		Control *c = as_sortable_control(get_child(i), SortableVisibilityMode::VISIBLE);
		if (!c) {
			continue;
		}
		int row = valid_controls_index / columns;
		int col = valid_controls_index % columns;
		valid_controls_index++;

		Size2i ms = p_use_desired_sizes ? c->get_bound_desired_size() : c->get_bound_minimum_size();
		if (col_minw.has(col)) {
			col_minw[col] = MAX(col_minw[col], ms.width);
		} else {
			col_minw[col] = ms.width;
		}

		if (row_minh.has(row)) {
			row_minh[row] = MAX(row_minh[row], ms.height);
		} else {
			row_minh[row] = ms.height;
		}
		max_col = MAX(col, max_col);
		max_row = MAX(row, max_row);
	}

	Size2 ms;

	for (const KeyValue<int, int> &E : col_minw) {
		ms.width += E.value;
	}

	for (const KeyValue<int, int> &E : row_minh) {
		ms.height += E.value;
	}

	ms.height += theme_cache.v_separation * max_row;
	ms.width += theme_cache.h_separation * max_col;

	// The skew diagonal is anchored to the visible area, so the reservation
	// only needs to cover the visible diagonal, not all rows and columns.
	Vector2 skew_span(Math::abs(item_skew.x) * max_row, Math::abs(item_skew.y) * max_col);
	if (!item_skew.is_zero_approx() && valid_controls_index > 0) {
		Rect2 visible_rect;
		if (get_visible_canvas_rect(visible_rect)) {
			const Rect2 own_rect = get_global_rect();
			Vector<float> row_begins;
			Vector<float> col_begins;
			row_begins.reserve(row_minh.size());
			col_begins.reserve(col_minw.size());
			float acc = 0.0f;
			for (const KeyValue<int, int> &E : row_minh) {
				row_begins.push_back(acc);
				acc += E.value + theme_cache.v_separation;
			}
			acc = 0.0f;
			for (const KeyValue<int, int> &E : col_minw) {
				col_begins.push_back(acc);
				acc += E.value + theme_cache.h_separation;
			}
			float progress = 0.0f;
			if (!Math::is_zero_approx(item_skew.x) && !row_begins.is_empty()) {
				_skew_apply_visible_window(row_begins, visible_rect.position.y - own_rect.position.y, visible_rect.get_end().y - own_rect.position.y, item_skew.x, skew_span.x, progress, skew_span.x);
			}
			if (!Math::is_zero_approx(item_skew.y) && !col_begins.is_empty()) {
				_skew_apply_visible_window(col_begins, visible_rect.position.x - own_rect.position.x, visible_rect.get_end().x - own_rect.position.x, item_skew.y, skew_span.y, progress, skew_span.y);
			}
		}
	}

	ms.width += Math::ceil(skew_span.x);
	ms.height += Math::ceil(skew_span.y);

	return ms;
}

Size2 GridContainer::get_minimum_size() const {
	return _get_minimum_size(false);
}

Size2 GridContainer::get_desired_size() const {
	return _get_minimum_size(true);
}
