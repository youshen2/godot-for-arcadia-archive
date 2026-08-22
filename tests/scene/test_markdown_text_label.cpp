/**************************************************************************/
/*  test_markdown_text_label.cpp                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_markdown_text_label)

#include "modules/modules_enabled.gen.h" // IWYU pragma: keep. Needed for MODULE_TEXT_SERVER_FB_ENABLED and MODULE_TEXT_SERVER_ADV_ENABLED definitions.

#if defined(MODULE_TEXT_SERVER_FB_ENABLED) || defined(MODULE_TEXT_SERVER_ADV_ENABLED)

#include "scene/gui/markdown_text_label.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

namespace TestMarkdownTextLabel {

TEST_CASE("[SceneTree][MarkdownTextLabel] Fit content updates after width changes") {
	MarkdownTextLabel *test_label = memnew(MarkdownTextLabel);
	Window *root = SceneTree::get_singleton()->get_root();
	root->add_child(test_label);

	test_label->set_fit_content(true);
	test_label->set_autowrap_mode(TextServer::AUTOWRAP_ARBITRARY);
	test_label->set_text(String("This is a long Markdown text. ").repeat(20));
	test_label->set_size(Size2(20, 1));
	SceneTree::get_singleton()->process(0);

	const Size2 narrow_minimum = test_label->get_combined_minimum_size();
	test_label->set_size(Size2(400, narrow_minimum.y));
	SceneTree::get_singleton()->process(0);

	const Size2 wide_minimum = test_label->get_combined_minimum_size();
	CHECK_MESSAGE(
			wide_minimum.y < narrow_minimum.y,
			"Fit content minimum height should decrease when the label becomes wider.");
	CHECK_MESSAGE(
			Math::is_equal_approx(wide_minimum.y, (real_t)test_label->get_content_height()),
			"Fit content minimum height should match the layout for the current width.");

	memdelete(test_label);
}

} // namespace TestMarkdownTextLabel

#endif // MODULE_TEXT_SERVER_FB_ENABLED || MODULE_TEXT_SERVER_ADV_ENABLED
