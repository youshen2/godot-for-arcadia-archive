extends SceneTree

# item_skew 可视区域锚定验证：
# - 视口 300px 高、卡片 40px 高、skew=-10 时，可视容量约 8 项，span = 10 * 7 = 70。
# - 任意滚动位置 / 任意 item 总数下，可视范围内第 1 项 x≈70（贴右收），逐项 -10。
# - 增加 item 后，可视项的偏移必须完全不变。

func _dump(container: Container, label: String) -> void:
	var parts: Array[String] = []
	for i in container.get_child_count():
		var c = container.get_child(i)
		parts.append("%d:%.1f" % [i, c.get_global_transform().origin.x])
	print("[%s] %s" % [label, " ".join(parts)])

func _frames(n: int = 3) -> void:
	for i in n:
		await process_frame

func _make_items(parent: Container, count: int, size: Vector2) -> void:
	for i in count:
		var r = ColorRect.new()
		r.custom_minimum_size = size
		parent.add_child(r)

func _initialize() -> void:
	var root_control = Control.new()
	root_control.size = Vector2(800, 600)
	root.add_child(root_control)

	# 直接作为 ScrollContainer 内容，负值 skew。
	var sc1 = ScrollContainer.new()
	sc1.size = Vector2(400, 300)
	root_control.add_child(sc1)
	var vbox1 = VBoxContainer.new()
	vbox1.item_skew = -10.0
	vbox1.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sc1.add_child(vbox1)
	_make_items(vbox1, 20, Vector2(100, 40))

	# 嵌套在普通 VBox 内（带标题头），负值 skew。
	var sc2 = ScrollContainer.new()
	sc2.size = Vector2(400, 300)
	sc2.position = Vector2(0, 320)
	root_control.add_child(sc2)
	var wrap = VBoxContainer.new()
	wrap.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sc2.add_child(wrap)
	var header = ColorRect.new()
	header.custom_minimum_size = Vector2(100, 30)
	wrap.add_child(header)
	var vbox2 = VBoxContainer.new()
	vbox2.item_skew = -10.0
	wrap.add_child(vbox2)
	_make_items(vbox2, 20, Vector2(100, 40))

	await _frames()
	_dump(vbox1, "direct scroll=0")
	_dump(vbox2, "nested scroll=0")

	sc1.scroll_vertical = 85
	sc2.scroll_vertical = 85
	await _frames()
	_dump(vbox1, "direct scroll=85")
	_dump(vbox2, "nested scroll=85")

	# 动态增加 item，可视项偏移不得变化。
	_make_items(vbox1, 10, Vector2(100, 40))
	_make_items(vbox2, 10, Vector2(100, 40))
	await _frames()
	_dump(vbox1, "direct 30 items scroll=85")
	_dump(vbox2, "nested 30 items scroll=85")

	sc1.scroll_vertical = 0
	await _frames()
	_dump(vbox1, "direct 30 items scroll=0")

	quit()
