/**************************************************************************/
/*  box_container.cpp                                                     */
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

#include "box_container.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/theme/theme_db.h"

struct _MinSizeCache {
	int min_size = 0;
	int max_size = -1;
	bool will_stretch = false;
	int final_size = 0;
};

// Returns the fractional item index whose main-axis slot contains `p_pos`.
// `p_begins` must be sorted in ascending order.
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
// fractional index of the first visible item and `r_span` the cross-axis range
// covered by the visible diagonal (never larger than the full skew extent).
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

void BoxContainer::_resort() {
	/** First pass, determine minimum size AND amount of stretchable elements */

	Size2i new_size = get_size();
	Size2 combined_max_size = get_combined_maximum_size();
	bool propagating_max_size = vertical ? is_propagating_maximum_size() && combined_max_size.height >= 0 : is_propagating_maximum_size() && combined_max_size.width >= 0;

	bool rtl = is_layout_rtl();

	bool first = true;
	int children_count = 0;
	int stretch_min = 0;
	int stretch_avail = 0;
	float stretch_ratio_total = 0.0;
	HashMap<Control *, _MinSizeCache> min_size_cache;

	for (int i = 0; i < get_child_count(); i++) {
		Control *c = as_sortable_control(get_child(i));
		if (!c) {
			continue;
		}

		if (propagating_max_size) {
			c->set_parent_maximum_size_cache(combined_max_size);
		}
		Size2i min_size = c->get_bound_minimum_size().ceil();
		Size2i max_size = c->get_combined_maximum_size();
		_MinSizeCache msc;

		if (vertical) { /* VERTICAL */
			stretch_min += min_size.height;
			msc.min_size = min_size.height;
			msc.max_size = max_size.height;
			msc.will_stretch = c->get_v_size_flags().has_flag(SIZE_EXPAND);

		} else { /* HORIZONTAL */
			stretch_min += min_size.width;
			msc.min_size = min_size.width;
			msc.max_size = max_size.width;
			msc.will_stretch = c->get_h_size_flags().has_flag(SIZE_EXPAND);
		}

		if (msc.max_size >= 0 && msc.max_size < msc.min_size) {
			msc.max_size = msc.min_size;
		}

		if (msc.will_stretch) {
			stretch_avail += msc.min_size;
			stretch_ratio_total += c->get_stretch_ratio();
		}
		msc.final_size = msc.min_size;
		min_size_cache[c] = msc;
		children_count++;
	}

	if (children_count == 0) {
		return;
	}

	int stretch_max = (vertical ? new_size.height : new_size.width) - (children_count - 1) * theme_cache.separation;
	int stretch_diff = stretch_max - stretch_min;
	if (stretch_diff < 0) {
		//avoid negative stretch space
		stretch_diff = 0;
	}

	stretch_avail += stretch_diff; //available stretch space.
	/** Second, pass successively to discard elements that can't be stretched, this will run while stretchable
		elements exist */

	if (propagating_max_size) {
		if (vertical) {
			stretch_avail = MIN(stretch_avail, combined_max_size.height);
		} else {
			stretch_avail = MIN(stretch_avail, combined_max_size.width);
		}
	}

	while (stretch_ratio_total > 0) { // first of all, don't even be here if no stretchable objects exist
		bool refit_successful = true; //assume refit-test will go well
		float error = 0.0; // Keep track of accumulated error in pixels

		for (int i = 0; i < get_child_count(); i++) {
			Control *c = as_sortable_control(get_child(i));
			if (!c) {
				continue;
			}

			ERR_FAIL_COND(!min_size_cache.has(c));
			_MinSizeCache &msc = min_size_cache[c];

			if (msc.will_stretch) { //wants to stretch
				//let's see if it can really stretch
				float stretch_ratio = c->get_stretch_ratio();
				float final_pixel_size = stretch_avail * stretch_ratio / stretch_ratio_total;
				// Add leftover fractional pixels to error accumulator
				error += final_pixel_size - (int)final_pixel_size;
				if (final_pixel_size < msc.min_size) {
					//if available stretching area is too small for widget,
					//then remove it from stretching area
					msc.will_stretch = false;
					stretch_ratio_total -= stretch_ratio;
					refit_successful = false;
					stretch_avail -= msc.min_size;
					msc.final_size = msc.min_size;
					break;
				} else if (msc.max_size >= 0 && final_pixel_size > msc.max_size) {
					// If stretching would exceed the Control's maximum size,
					// cap it and redistribute its unused share.
					msc.will_stretch = false;
					stretch_ratio_total -= stretch_ratio;
					refit_successful = false;
					stretch_avail -= msc.max_size;
					msc.final_size = msc.max_size;
					break;
				} else {
					msc.final_size = final_pixel_size;
					// Dump accumulated error if one pixel or more
					if (error >= 1 && (msc.max_size < 0 || msc.final_size < msc.max_size)) {
						msc.final_size += 1;
						error -= 1;
					}
				}
			}
		}

		if (refit_successful) { //uf refit went well, break
			break;
		}
	}

	/** Final pass, draw and stretch elements **/

	int ofs = 0;
	int final_stretch_diff = stretch_max - stretch_min;
	for (int i = 0; i < get_child_count(); i++) {
		Control *c = as_sortable_control(get_child(i));
		if (!c) {
			continue;
		}

		ERR_FAIL_COND(!min_size_cache.has(c));
		_MinSizeCache &msc = min_size_cache[c];
		final_stretch_diff -= msc.final_size - msc.min_size;
	}

	if (final_stretch_diff < 0) {
		final_stretch_diff = 0;
	}

	if (!vertical) {
		switch (alignment) {
			case ALIGNMENT_BEGIN:
				if (rtl) {
					ofs = final_stretch_diff;
				}
				break;
			case ALIGNMENT_CENTER:
				ofs = final_stretch_diff / 2;
				break;
			case ALIGNMENT_END:
				if (!rtl) {
					ofs = final_stretch_diff;
				}
				break;
		}
	} else {
		switch (alignment) {
			case ALIGNMENT_BEGIN:
				break;
			case ALIGNMENT_CENTER:
				ofs = final_stretch_diff / 2;
				break;
			case ALIGNMENT_END:
				ofs = final_stretch_diff;
				break;
		}
	}

	first = true;
	int idx = 0;

	int start;
	int end;
	int delta;
	if (!rtl || vertical) {
		start = 0;
		end = get_child_count();
		delta = +1;
	} else {
		start = get_child_count() - 1;
		end = -1;
		delta = -1;
	}

	int accumulated_size = 0;
	const float skew_extent = Math::abs(item_skew) * MAX(children_count - 1, 0);

	// The skew diagonal is anchored to the container's visible area (its own
	// rect clipped by the window and any clipping ancestors such as a
	// ScrollContainer), so adding, removing or scrolling items never changes
	// the offsets of the visible items. `skew_progress` is the fractional index
	// of the first visible item and `skew_span` the cross-axis range covered by
	// the visible diagonal; when the container is fully visible this degrades
	// to anchoring at the first item with the whole skew extent.
	float skew_progress = 0.0f;
	float skew_span = skew_extent;
	if (!Math::is_zero_approx(item_skew) && children_count > 0) {
		Rect2 visible_rect;
		if (get_visible_canvas_rect(visible_rect)) {
			Vector<float> item_begins;
			item_begins.resize(children_count);
			float acc_ofs = ofs;
			bool acc_first = true;
			int acc_idx = 0;
			for (int i = start; i != end; i += delta) {
				Control *c = as_sortable_control(get_child(i));
				if (!c) {
					continue;
				}
				if (acc_first) {
					acc_first = false;
				} else {
					acc_ofs += theme_cache.separation;
				}
				item_begins.write[acc_idx] = acc_ofs;
				acc_ofs += min_size_cache[c].final_size;
				acc_idx++;
			}
			const Rect2 own_rect = get_global_rect();
			const float vis_begin = vertical ? visible_rect.position.y - own_rect.position.y : visible_rect.position.x - own_rect.position.x;
			const float vis_end = vertical ? visible_rect.get_end().y - own_rect.position.y : visible_rect.get_end().x - own_rect.position.x;
			_skew_apply_visible_window(item_begins, vis_begin, vis_end, item_skew, skew_extent, skew_progress, skew_span);
		}
	}
	const float skew_base = item_skew < 0.0f ? skew_span : 0.0f;
	for (int i = start; i != end; i += delta) {
		Control *c = as_sortable_control(get_child(i));
		if (!c) {
			continue;
		}

		_MinSizeCache &msc = min_size_cache[c];

		if (first) {
			first = false;
		} else {
			ofs += theme_cache.separation;
		}

		int from = ofs;
		int to = ofs + msc.final_size;

		if (msc.will_stretch && idx == children_count - 1) {
			//adjust so the last one always fits perfect
			//compensating for numerical imprecision

			to = vertical ? new_size.height : new_size.width;
		}

		int size = to - from;

		Rect2 rect;

		// The skew offset is relative to the first visible item, so the diagonal
		// keeps a fixed shape inside the visible area regardless of item count
		// or scroll position.
		if (vertical) {
			rect = Rect2(skew_base + item_skew * (idx - skew_progress), from, MAX(0.0f, new_size.width - skew_span), size);
		} else {
			rect = Rect2(from, skew_base + item_skew * (idx - skew_progress), size, MAX(0.0f, new_size.height - skew_span));
		}

		if (propagating_max_size) {
			if (vertical) {
				c->set_parent_maximum_size_cache(Size2(combined_max_size.width, MAX(combined_max_size.height - accumulated_size, 0)));
			} else {
				c->set_parent_maximum_size_cache(Size2(MAX(combined_max_size.width - accumulated_size, 0), combined_max_size.height));
			}
		}

		fit_child_in_rect(c, rect);

		accumulated_size += size + theme_cache.separation;
		ofs = to;
		idx++;
	}
}

Size2 BoxContainer::_get_minimum_size(bool p_use_desired_sizes) const {
	/* Calculate MINIMUM SIZE */

	Size2i minimum;

	bool first = true;
	int children_count = 0;
	Vector<float> item_begins;

	for (int i = 0; i < get_child_count(); i++) {
		Control *c = as_sortable_control(get_child(i), SortableVisibilityMode::VISIBLE);
		if (!c) {
			continue;
		}

		Size2i size = p_use_desired_sizes ? c->get_bound_desired_size().ceil() : c->get_bound_minimum_size().ceil();

		if (!Math::is_zero_approx(item_skew)) {
			item_begins.push_back(vertical ? minimum.height : minimum.width);
		}

		if (vertical) { /* VERTICAL */

			if (size.width > minimum.width) {
				minimum.width = size.width;
			}

			minimum.height += size.height + (first ? 0 : theme_cache.separation);

		} else { /* HORIZONTAL */

			if (size.height > minimum.height) {
				minimum.height = size.height;
			}

			minimum.width += size.width + (first ? 0 : theme_cache.separation);
		}

		first = false;
		children_count++;
	}

	// The skew diagonal is anchored to the visible area, so the cross-axis
	// reservation only needs to cover the visible diagonal, not all items.
	float skew_span = Math::abs(item_skew) * MAX(children_count - 1, 0);
	if (!Math::is_zero_approx(item_skew) && children_count > 0) {
		Rect2 visible_rect;
		if (get_visible_canvas_rect(visible_rect)) {
			const Rect2 own_rect = get_global_rect();
			const float vis_begin = vertical ? visible_rect.position.y - own_rect.position.y : visible_rect.position.x - own_rect.position.x;
			const float vis_end = vertical ? visible_rect.get_end().y - own_rect.position.y : visible_rect.get_end().x - own_rect.position.x;
			float skew_progress = 0.0f;
			_skew_apply_visible_window(item_begins, vis_begin, vis_end, item_skew, skew_span, skew_progress, skew_span);
		}
	}
	if (vertical) {
		minimum.width += Math::ceil(skew_span);
	} else {
		minimum.height += Math::ceil(skew_span);
	}

	return minimum;
}

Size2 BoxContainer::get_minimum_size() const {
	return _get_minimum_size(is_fit_child_content());
}

Size2 BoxContainer::get_desired_size() const {
	return _get_minimum_size(true);
}

void BoxContainer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_SORT_CHILDREN: {
			_resort();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (!Math::is_zero_approx(item_skew)) {
				// The skew diagonal is anchored to the visible area; follow any
				// movement of the container or its ancestors (e.g. scrolling).
				queue_sort();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			update_minimum_size();
			queue_sort();
		} break;

		case NOTIFICATION_TRANSLATION_CHANGED:
		case NOTIFICATION_LAYOUT_DIRECTION_CHANGED: {
			queue_sort();
		} break;
	}
}

void BoxContainer::_validate_property(PropertyInfo &p_property) const {
	if (is_fixed && p_property.name == "vertical") {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

void BoxContainer::set_alignment(AlignmentMode p_alignment) {
	if (alignment == p_alignment) {
		return;
	}
	alignment = p_alignment;
	_resort();
}

BoxContainer::AlignmentMode BoxContainer::get_alignment() const {
	return alignment;
}

void BoxContainer::_fit_child_content_changed() {
	queue_sort();
}

void BoxContainer::_child_content_size_changed() {
	if (!is_fit_child_content()) {
		return;
	}

	update_minimum_size();
	queue_sort();
}

void BoxContainer::add_child_notify(Node *p_child) {
	Container::add_child_notify(p_child);

	Control *control = Object::cast_to<Control>(p_child);
	if (control) {
		control->connect(SNAME("_desired_size_changed"), callable_mp(this, &BoxContainer::_child_content_size_changed));
	}
}

void BoxContainer::remove_child_notify(Node *p_child) {
	Control *control = Object::cast_to<Control>(p_child);
	if (control) {
		control->disconnect(SNAME("_desired_size_changed"), callable_mp(this, &BoxContainer::_child_content_size_changed));
	}

	Container::remove_child_notify(p_child);
}

void BoxContainer::set_vertical(bool p_vertical) {
	ERR_FAIL_COND_MSG(is_fixed, "Can't change orientation of " + get_class() + ".");
	vertical = p_vertical;
	update_minimum_size();
	_resort();
}

bool BoxContainer::is_vertical() const {
	return vertical;
}

void BoxContainer::set_item_skew(float p_offset) {
	ERR_FAIL_COND(!Math::is_finite(p_offset));
	if (item_skew == p_offset) {
		return;
	}
	item_skew = p_offset;
	set_notify_transform(!Math::is_zero_approx(item_skew));
	update_minimum_size();
	queue_sort();
}

float BoxContainer::get_item_skew() const {
	return item_skew;
}

Control *BoxContainer::add_spacer(bool p_begin) {
	Control *c = memnew(Control);
	c->set_mouse_filter(MOUSE_FILTER_PASS); //allow spacer to pass mouse events

	if (vertical) {
		c->set_v_size_flags(SIZE_EXPAND_FILL);
	} else {
		c->set_h_size_flags(SIZE_EXPAND_FILL);
	}

	add_child(c);
	if (p_begin) {
		move_child(c, 0);
	}

	return c;
}

Vector<int> BoxContainer::get_allowed_size_flags_horizontal() const {
	Vector<int> flags;
	flags.append(SIZE_FILL);
	if (!vertical) {
		flags.append(SIZE_EXPAND);
	}
	flags.append(SIZE_SHRINK_BEGIN);
	flags.append(SIZE_SHRINK_CENTER);
	flags.append(SIZE_SHRINK_END);
	return flags;
}

Vector<int> BoxContainer::get_allowed_size_flags_vertical() const {
	Vector<int> flags;
	flags.append(SIZE_FILL);
	if (vertical) {
		flags.append(SIZE_EXPAND);
	}
	flags.append(SIZE_SHRINK_BEGIN);
	flags.append(SIZE_SHRINK_CENTER);
	flags.append(SIZE_SHRINK_END);
	return flags;
}

BoxContainer::BoxContainer(bool p_vertical) {
	vertical = p_vertical;
}

void BoxContainer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_spacer", "begin"), &BoxContainer::add_spacer);
	ClassDB::bind_method(D_METHOD("set_alignment", "alignment"), &BoxContainer::set_alignment);
	ClassDB::bind_method(D_METHOD("get_alignment"), &BoxContainer::get_alignment);
	ClassDB::bind_method(D_METHOD("set_vertical", "vertical"), &BoxContainer::set_vertical);
	ClassDB::bind_method(D_METHOD("is_vertical"), &BoxContainer::is_vertical);
	ClassDB::bind_method(D_METHOD("set_item_skew", "offset"), &BoxContainer::set_item_skew);
	ClassDB::bind_method(D_METHOD("get_item_skew"), &BoxContainer::get_item_skew);

	BIND_ENUM_CONSTANT(ALIGNMENT_BEGIN);
	BIND_ENUM_CONSTANT(ALIGNMENT_CENTER);
	BIND_ENUM_CONSTANT(ALIGNMENT_END);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "alignment", PROPERTY_HINT_ENUM, "Begin,Center,End"), "set_alignment", "get_alignment");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "vertical"), "set_vertical", "is_vertical");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "item_skew", PROPERTY_HINT_RANGE, "-256,256,0.1,or_less,or_greater,suffix:px"), "set_item_skew", "get_item_skew");

	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, BoxContainer, separation);
}

MarginContainer *VBoxContainer::add_margin_child(const String &p_label, Control *p_control, bool p_expand) {
	Label *l = memnew(Label);
	l->set_theme_type_variation("HeaderSmall");
	l->set_text(p_label);
	add_child(l);
	MarginContainer *mc = memnew(MarginContainer);
	mc->add_child(p_control, true);
	add_child(mc);
	if (p_expand) {
		mc->set_v_size_flags(SIZE_EXPAND_FILL);
	}
	p_control->set_accessibility_name(p_label);

	return mc;
}
