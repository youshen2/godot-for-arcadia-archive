#include "gif_writer.h"

#include "core/error/error_macros.h"
#include "core/object/class_db.h"

#include <cstdint>

extern "C" int GifQuantizeBuffer(unsigned int Width, unsigned int Height, int *ColorMapSize, const GifByteType *RedInput, const GifByteType *GreenInput, const GifByteType *BlueInput, GifByteType *OutputBuffer, GifColorType *OutputColorMap);

void GIFWriter::_bind_methods() {
	BIND_ENUM_CONSTANT(SUCCEEDED);
	BIND_ENUM_CONSTANT(OPEN_FAILED);
	BIND_ENUM_CONSTANT(WRITER_FAILED);
	BIND_ENUM_CONSTANT(HAS_SCRN_DSCR);
	BIND_ENUM_CONSTANT(HAS_IMAG_DSCR);
	BIND_ENUM_CONSTANT(NO_COLOR_MAP);
	BIND_ENUM_CONSTANT(DATA_TOO_BIG);
	BIND_ENUM_CONSTANT(NOT_ENOUGH_MEM);
	BIND_ENUM_CONSTANT(DISK_IS_FULL);
	BIND_ENUM_CONSTANT(CLOSE_FAILED);
	BIND_ENUM_CONSTANT(NOT_WRITEABLE);
	BIND_ENUM_CONSTANT(INVALID_DIMENSIONS);
	BIND_ENUM_CONSTANT(NO_FILE_OPEN);
	BIND_ENUM_CONSTANT(PALETTE_TOO_LARGE);
	BIND_ENUM_CONSTANT(NO_SCRN_DSCR);
	BIND_ENUM_CONSTANT(NO_IMAG_DSCR);

	BIND_ENUM_CONSTANT(DISPOSAL_METHOD_UNSPECIFIED);
	BIND_ENUM_CONSTANT(DISPOSAL_METHOD_DO_NOT);
	BIND_ENUM_CONSTANT(DISPOSAL_METHOD_BACKGROUND);
	BIND_ENUM_CONSTANT(DISPOSAL_METHOD_PREVIOUS);

	ClassDB::bind_method(D_METHOD("open", "path"), &GIFWriter::open);
	ClassDB::bind_method(D_METHOD("open_from_buffer"), &GIFWriter::open_from_buffer);
	ClassDB::bind_method(D_METHOD("close"), &GIFWriter::close);
	ClassDB::bind_method(D_METHOD("get_output_buffer"), &GIFWriter::get_output_buffer);

	ClassDB::bind_method(D_METHOD("set_canvas_size", "width", "height", "background_color"), &GIFWriter::set_canvas_size, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_loop_count", "loop_count"), &GIFWriter::set_loop_count);
	ClassDB::bind_method(D_METHOD("set_global_palette", "palette"), &GIFWriter::set_global_palette);
	ClassDB::bind_method(D_METHOD("set_color_resolution", "bits"), &GIFWriter::set_color_resolution);

	ClassDB::bind_method(D_METHOD("begin_frame", "left", "top", "width", "height", "interlace"), &GIFWriter::begin_frame, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_frame_delay", "delay_ms"), &GIFWriter::set_frame_delay);
	ClassDB::bind_method(D_METHOD("set_disposal_method", "method"), &GIFWriter::set_disposal_method);
	ClassDB::bind_method(D_METHOD("set_transparent_color", "index"), &GIFWriter::set_transparent_color);
	ClassDB::bind_method(D_METHOD("set_frame_palette", "palette"), &GIFWriter::set_frame_palette);
	ClassDB::bind_method(D_METHOD("write_frame_pixels", "indices"), &GIFWriter::write_frame_pixels);
	ClassDB::bind_method(D_METHOD("write_frame_image", "image", "quantize"), &GIFWriter::write_frame_image, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("end_frame"), &GIFWriter::end_frame);

	ClassDB::bind_method(D_METHOD("write_gif", "images", "delays", "loop_count", "quantize"), &GIFWriter::write_gif, DEFVAL(0), DEFVAL(true));
	ClassDB::bind_static_method("GIFWriter", D_METHOD("save_to_file", "path", "images", "delays", "loop_count", "quantize"), &GIFWriter::save_to_file, DEFVAL(0), DEFVAL(true));
	ClassDB::bind_static_method("GIFWriter", D_METHOD("save_to_buffer", "images", "delays", "loop_count", "quantize"), &GIFWriter::save_to_buffer, DEFVAL(0), DEFVAL(true));

	ClassDB::bind_method(D_METHOD("add_comment", "comment"), &GIFWriter::add_comment);

	ClassDB::bind_method(D_METHOD("get_frame_count"), &GIFWriter::get_frame_count);
	ClassDB::bind_method(D_METHOD("get_canvas_size"), &GIFWriter::get_canvas_size);
}

GIFWriter::~GIFWriter() {
	close();
}

void GIFWriter::_reset_write_state() {
	has_screen_desc = false;
	has_image_desc = false;
	current_frame = 0;
	color_resolution = 8;
	frame_left = 0;
	frame_top = 0;
	frame_width = 0;
	frame_height = 0;
	frame_interlace = false;
	frame_delay = 10;
	frame_disposal = DISPOSAL_METHOD_UNSPECIFIED;
	frame_transparent_index = -1;

	if (frame_color_map) {
		GifFreeMapObject(frame_color_map);
		frame_color_map = nullptr;
	}
}

int GIFWriter::_mem_output_func(GifFileType* gif, const GifByteType* bytes, int size) {
	GIFWriter* writer = static_cast<GIFWriter*>(gif->UserData);
	if (!writer || !bytes || size <= 0) return 0;

	int old_size = writer->mem_output_data.size();
	writer->mem_output_data.resize(old_size + size);
	memcpy(writer->mem_output_data.ptrw() + old_size, bytes, size);

	return size;
}

int GIFWriter::_file_write_callback(GifFileType* gif, const GifByteType* bytes, int size) {
	GIFWriter* writer = static_cast<GIFWriter*>(gif->UserData);
	if (!writer || writer->fa.is_null() || !bytes || size <= 0) return 0;

	writer->fa->store_buffer(bytes, size);
	return size;
}

GIFWriter::GIFError GIFWriter::_quantize_image(const Ref<Image> &p_image, PackedByteArray &r_indices) {
	ERR_FAIL_COND_V(p_image.is_null(), DATA_TOO_BIG);
	ERR_FAIL_COND_V(p_image->is_empty(), DATA_TOO_BIG);

	Ref<Image> img = p_image;
	if (img->is_compressed() || img->get_format() != Image::FORMAT_RGBA8) {
		img = p_image->duplicate();
		if (img->is_compressed() && img->decompress() != OK) {
			return DATA_TOO_BIG;
		}
		if (img->get_format() != Image::FORMAT_RGBA8) {
			img->convert(Image::FORMAT_RGBA8);
		}
	}

	const int width = img->get_width();
	const int height = img->get_height();
	const int64_t pixel_count_64 = static_cast<int64_t>(width) * static_cast<int64_t>(height);
	ERR_FAIL_COND_V(pixel_count_64 > INT32_MAX, DATA_TOO_BIG);
	const int pixel_count = static_cast<int>(pixel_count_64);
	PackedByteArray pixels = img->get_data();
	PackedByteArray red;
	PackedByteArray green;
	PackedByteArray blue;
	PackedByteArray quantized_indices;
	red.resize(pixel_count);
	green.resize(pixel_count);
	blue.resize(pixel_count);
	quantized_indices.resize(pixel_count);

	const uint8_t *pixel_ptr = pixels.ptr();
	uint8_t *red_ptr = red.ptrw();
	uint8_t *green_ptr = green.ptrw();
	uint8_t *blue_ptr = blue.ptrw();
	bool has_transparency = false;
	for (int i = 0; i < pixel_count; i++) {
		const int pixel_offset = i * 4;
		red_ptr[i] = pixel_ptr[pixel_offset + 0];
		green_ptr[i] = pixel_ptr[pixel_offset + 1];
		blue_ptr[i] = pixel_ptr[pixel_offset + 2];
		if (pixel_ptr[pixel_offset + 3] < 128) {
			has_transparency = true;
		}
	}

	int quantized_color_count = has_transparency ? 255 : 256;
	GifColorType output_color_map[256];
	if (GifQuantizeBuffer(width, height, &quantized_color_count, red.ptr(), green.ptr(), blue.ptr(), quantized_indices.ptrw(), output_color_map) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	PackedColorArray palette;
	const int palette_offset = has_transparency ? 1 : 0;
	palette.resize(quantized_color_count + palette_offset);
	Color *palette_ptr = palette.ptrw();
	if (has_transparency) {
		palette_ptr[0] = Color(0, 0, 0, 0);
		frame_transparent_index = 0;
	}
	for (int i = 0; i < quantized_color_count; i++) {
		const GifColorType &color = output_color_map[i];
		palette_ptr[i + palette_offset] = Color(color.Red / 255.0f, color.Green / 255.0f, color.Blue / 255.0f, 1.0f);
	}

	GIFError palette_err = set_frame_palette(palette);
	if (palette_err != SUCCEEDED) {
		return palette_err;
	}

	r_indices.resize(pixel_count);
	const uint8_t *quantized_ptr = quantized_indices.ptr();
	uint8_t *indices_ptr = r_indices.ptrw();
	for (int i = 0; i < pixel_count; i++) {
		const bool transparent = has_transparency && pixel_ptr[i * 4 + 3] < 128;
		indices_ptr[i] = transparent ? 0 : uint8_t(quantized_ptr[i] + palette_offset);
	}

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::open(const String& p_path) {
	if (file_type) {
		close();
	}
	_reset_write_state();

	// 使用 Godot 的 FileAccess 打开文件 (支持中文和虚拟路径)
	fa = FileAccess::open(p_path, FileAccess::WRITE);
	if (fa.is_null()) {
		return OPEN_FAILED;
	}

	int err = 0;
	// 使用 EGifOpen 绑定自定义的回调函数
	// 注意: 这里的第一个参数是 UserData，我们传 this
	file_type = EGifOpen(this, _file_write_callback, &err);

	if (!file_type) {
		// 失败则释放文件
		fa.unref();
		_reset_write_state();
		return static_cast<GIFError>(err);
	}

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::open_from_buffer() {
	if (file_type) {
		close();
	}
	_reset_write_state();
	mem_output_data.clear();
	int err = 0;

	// 使用 EGifOpen 自定义输出函数写入到内存
	file_type = EGifOpen(this, _mem_output_func, &err);
	if (!file_type) {
		return OPEN_FAILED;
	}

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::close() {
	if (!file_type) return SUCCEEDED;

	int err = 0;
	// 这会触发最后的结尾写入
	const int result = EGifCloseFile(file_type, &err);
	file_type = nullptr;

	if (fa.is_valid()) {
		// 释放 Godot 文件句柄，这会自动关闭文件
		fa->flush();
		fa.unref();
	}

	_reset_write_state();

	return result == GIF_ERROR ? CLOSE_FAILED : SUCCEEDED;
}


PackedByteArray GIFWriter::get_output_buffer() const {
	return mem_output_data;
}

GIFWriter::GIFError GIFWriter::set_canvas_size(const int p_width, const int p_height, const int p_background_color) {
	if (!file_type) {
		return NO_FILE_OPEN;
	}
	if (has_screen_desc) {
		return HAS_SCRN_DSCR;
	}

	if (p_width <= 0 || p_width > 65535 || p_height <= 0 || p_height > 65535) {
		return INVALID_DIMENSIONS;
	}

	const int clamped_width = p_width;
	const int clamped_height = p_height;
	const int clamped_bg = CLAMP(p_background_color, 0, 255);

	// ColorMap 可以为空；如需全局调色板，必须在 set_canvas_size() 前调用 set_global_palette()。
	int err = EGifPutScreenDesc(
		file_type,
		clamped_width,
		clamped_height,
	    color_resolution,
		clamped_bg,
		file_type->SColorMap
	);

	if (err == GIF_OK) {
		has_screen_desc = true;
		return SUCCEEDED;
	} else {
		return WRITER_FAILED;
	}
}

GIFWriter::GIFError GIFWriter::set_loop_count(const int p_loop_count) {
	if (!file_type) return NO_FILE_OPEN;

	// Netscape 应用扩展格式:
	// 应用扩展标识符: 0xff
	// 应用标识符: "NETSCAPE2.0" (11字节)
	// 子块大小: 0x03
	// 循环子块标识: 0x01
	// 循环次数: 2字节小端序 (0=无限)

	// 写入应用扩展头
	if (EGifPutExtensionLeader(file_type, APPLICATION_EXT_FUNC_CODE) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	// 写入应用标识符 "NETSCAPE2.0"
	const char* app_id = "NETSCAPE2.0";
	if (EGifPutExtensionBlock(file_type, 11, app_id) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	// 写入循环参数子块
	unsigned char params[3];
	params[0] = 0x01;  												// 循环子块标识
	params[1] = p_loop_count & 0xff;								// 循环次数低字节
	params[2] = (p_loop_count >> 8) & 0xff;							// 循环次数高字节

	if (EGifPutExtensionBlock(file_type, 3, params) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	// 写入扩展尾
	if (EGifPutExtensionTrailer(file_type) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::set_global_palette(const PackedColorArray& p_palette) {
	if (!file_type) return NO_FILE_OPEN;
	if (has_screen_desc) return HAS_SCRN_DSCR;
	if (p_palette.is_empty()) return NO_COLOR_MAP;

	int color_count = p_palette.size();
	if (color_count > 256) return PALETTE_TOO_LARGE;
	int color_map_size = 2;
	while (color_map_size < color_count) {
		color_map_size <<= 1;
	}

	// 创建调色板对象 (自动计算位深度)
	ColorMapObject* color_map = GifMakeMapObject(color_map_size, nullptr);
	if (!color_map) return NOT_ENOUGH_MEM;

	// 填充颜色数据
	for (int i = 0; i < color_map_size; i++) {
		Color color = p_palette[MIN(i, color_count - 1)];
		color_map->Colors[i].Red = static_cast<GifByteType>(color.r * 255);
		color_map->Colors[i].Green = static_cast<GifByteType>(color.g * 255);
		color_map->Colors[i].Blue = static_cast<GifByteType>(color.b * 255);
	}

	// 如果已有全局调色板，先释放
	if (file_type->SColorMap) {
		GifFreeMapObject(file_type->SColorMap);
	}

	// 设置全局调色板
	file_type->SColorMap = color_map;

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::set_color_resolution(const int p_bits) {
	if (!file_type) return NO_FILE_OPEN;
	if (has_screen_desc) return HAS_SCRN_DSCR;						// 屏幕描述符已写入，无法修改

	if (p_bits < 1 || p_bits > 8) return INVALID_DIMENSIONS;
	color_resolution = p_bits;

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::begin_frame(
	const int p_left,
	const int p_top,
	const int p_width,
	const int p_height,
	const bool p_interlace
) {
	if (!file_type) return NO_FILE_OPEN;
	if (has_image_desc) return HAS_IMAG_DSCR;						// 图像描述符仍在激活
	if (p_width <= 0 || p_width > 65535 || p_height <= 0 || p_height > 65535) {
		return INVALID_DIMENSIONS;
	}

	// 保存当前帧状态
	frame_left = p_left;
	frame_top = p_top;
	frame_width = p_width;
	frame_height = p_height;
	frame_interlace = p_interlace;
	frame_delay = 10; // 重置为 100ms。
	frame_disposal = DISPOSAL_METHOD_UNSPECIFIED;
	frame_transparent_index = -1;

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::set_frame_delay(int p_delay_ms) {
	if (!file_type) return NO_FILE_OPEN;
	// GIF 延迟单位是 1/100 秒，转换毫秒（四舍五入）
	frame_delay = CLAMP((p_delay_ms + 5) / 10, 0, 65535);
	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::set_disposal_method(DisposalMethod p_method) {
	if (!file_type) return NO_FILE_OPEN;
	frame_disposal = p_method;
	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::set_transparent_color(const int p_index) {
	if (!file_type) return NO_FILE_OPEN;
	frame_transparent_index = p_index;
	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::set_frame_palette(const PackedColorArray& p_palette) {
	if (!file_type) return NO_FILE_OPEN;
	if (p_palette.is_empty()) return NO_COLOR_MAP;

	int color_count = p_palette.size();
	if (color_count > 256) return PALETTE_TOO_LARGE;
	int color_map_size = 2;
	while (color_map_size < color_count) {
		color_map_size <<= 1;
	}

	// 释放之前的局部调色板
	if (frame_color_map) {
		GifFreeMapObject(frame_color_map);
		frame_color_map = nullptr;
	}

	// 创建新的调色板对象
	frame_color_map = GifMakeMapObject(color_map_size, nullptr);
	if (!frame_color_map) return NOT_ENOUGH_MEM;

	// 填充颜色数据
	for (int i = 0; i < color_map_size; i++) {
		Color color = p_palette[MIN(i, color_count - 1)];
		frame_color_map->Colors[i].Red = static_cast<GifByteType>(color.r * 255);
		frame_color_map->Colors[i].Green = static_cast<GifByteType>(color.g * 255);
		frame_color_map->Colors[i].Blue = static_cast<GifByteType>(color.b * 255);
	}

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::write_frame_pixels(const PackedByteArray& p_indices) {
	if (!file_type) {
		return NO_FILE_OPEN;
	}
	if (!has_screen_desc) {
		return NO_SCRN_DSCR;
	}
	if (!frame_color_map && !file_type->SColorMap) {
		return NO_COLOR_MAP;
	}
	if (frame_width <= 0 || frame_height <= 0) {
		return NO_IMAG_DSCR;
	}

	// 检查数据大小
	int64_t expected_size = static_cast<int64_t>(frame_width) * static_cast<int64_t>(frame_height);
	if (expected_size > INT32_MAX || p_indices.size() < expected_size) {
		return DATA_TOO_BIG;
	}

	// 写入 GCB (图形控制块)
	GraphicsControlBlock gcb;
	gcb.DisposalMode = static_cast<int>(frame_disposal);
	gcb.UserInputFlag = false;
	gcb.DelayTime = frame_delay;
	gcb.TransparentColor = frame_transparent_index >= 0 ? frame_transparent_index : -1;

	// 打包 GCB 并写入
	char gcb_packed[4];
	EGifGCBToExtension(&gcb, reinterpret_cast<GifByteType*>(gcb_packed));

	if (EGifPutExtension(file_type, GRAPHICS_EXT_FUNC_CODE, 4, gcb_packed) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	// 写入图像描述符
	if (EGifPutImageDesc(file_type, frame_left, frame_top,
	                     frame_width, frame_height,
	                     frame_interlace, frame_color_map) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	// 逐行写入像素数据
	const uint8_t* ptr = p_indices.ptr();
	for (int i = 0; i < frame_height; i++) {
		if (EGifPutLine(file_type, const_cast<GifPixelType*>(ptr + i * frame_width), frame_width) == GIF_ERROR) {
			return WRITER_FAILED;
		}
	}

	has_image_desc = true;
	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::write_frame_image(Ref<Image> p_image, const bool p_quantize) {
	if (!file_type) return NO_FILE_OPEN;
	if (!has_screen_desc) return NO_SCRN_DSCR;
	if (p_image.is_null()) return DATA_TOO_BIG;

	// 获取图像尺寸
	int img_width = p_image->get_width();
	int img_height = p_image->get_height();
	if (img_width <= 0 || img_width > 65535 || img_height <= 0 || img_height > 65535) {
		return INVALID_DIMENSIONS;
	}

	// 更新 frame 尺寸
	frame_width = img_width;
	frame_height = img_height;

	if (p_quantize) {
		PackedByteArray indices;
		GIFError err = _quantize_image(p_image, indices);
		if (err != SUCCEEDED) {
			return err;
		}
		return write_frame_pixels(indices);
	}

	// 转换图像格式为 RGB8
	Ref<Image> img_rgb = p_image->duplicate();
	if (img_rgb->is_compressed() && img_rgb->decompress() != OK) {
		return DATA_TOO_BIG;
	}
	if (img_rgb->get_format() != Image::FORMAT_RGB8 && img_rgb->get_format() != Image::FORMAT_RGBA8) {
		img_rgb->convert(Image::FORMAT_RGB8);
	}

	PackedByteArray indices;
	indices.resize(img_width * img_height);

	// 如果没有调色板，创建一个默认的灰度调色板作为局部调色板
	if (!frame_color_map && !file_type->SColorMap) {
		PackedColorArray default_palette;
		default_palette.resize(256);
		Color *default_palette_ptr = default_palette.ptrw();
		for (int i = 0; i < 256; i++) {
			float g = i / 255.0f;
			default_palette_ptr[i] = Color(g, g, g);
		}
		GIFError err = set_frame_palette(default_palette);
		if (err != SUCCEEDED) return err;
	}

	// 将图像数据映射到调色板索引
	PackedByteArray pixels = img_rgb->get_data();
	int bpp = img_rgb->get_format() == Image::FORMAT_RGBA8 ? 4 : 3;

	// 获取使用的调色板
	ColorMapObject* cmap = frame_color_map ? frame_color_map : file_type->SColorMap;
	int color_count = cmap ? cmap->ColorCount : 256;
	uint8_t *indices_ptr = indices.ptrw();

	for (int y = 0; y < img_height; y++) {
		for (int x = 0; x < img_width; x++) {
			int idx = (y * img_width + x) * bpp;
			uint8_t r = pixels[idx];
			uint8_t g = pixels[idx + 1];
			uint8_t b = pixels[idx + 2];

			// 查找最近的调色板颜色
			int best_index = 0;
			int best_dist = 999999;

			if (cmap) {
				for (int i = 0; i < color_count; i++) {
					int dr = r - cmap->Colors[i].Red;
					int dg = g - cmap->Colors[i].Green;
					int db = b - cmap->Colors[i].Blue;
					int dist = dr * dr + dg * dg + db * db;
					if (dist < best_dist) {
						best_dist = dist;
						best_index = i;
					}
				}
			} else {
				// 无调色板，使用灰度
				best_index = (r + g + b) / 3;
			}

			indices_ptr[y * img_width + x] = best_index;
		}
	}

	return write_frame_pixels(indices);
}

GIFWriter::GIFError GIFWriter::end_frame() {
	if (!file_type) return NO_FILE_OPEN;
	if (!has_image_desc) return SUCCEEDED;  // 没有活动帧

	// 清理局部调色板
	if (frame_color_map) {
		GifFreeMapObject(frame_color_map);
		frame_color_map = nullptr;
	}

	has_image_desc = false;
	current_frame++;
	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::write_gif(
	const TypedArray<Image>& p_images,
	const PackedInt32Array& p_delays,
	const int p_loop_count,
	const bool p_quantize
) {
	if (!file_type) {
		return NO_FILE_OPEN;
	}
	if (!has_screen_desc) {
		return NO_SCRN_DSCR;
	}
	if (p_images.is_empty()) {
		return DATA_TOO_BIG;
	}

	int frame_count = p_images.size();

	// 设置循环次数
	if (p_loop_count >= 0) {
		GIFError err = set_loop_count(p_loop_count);
		if (err != SUCCEEDED) {
			return err;
		}
	}

	// 写入每一帧
	for (int i = 0; i < frame_count; i++) {
		Ref<Image> img = p_images[i];
		if (img.is_null()) {
			continue;
		}

		// 获取延迟
		int delay = (i < p_delays.size()) ? p_delays[i] : 100;

		// 开始帧
		GIFError err = begin_frame(0, 0, img->get_width(), img->get_height(), false);
		if (err != SUCCEEDED) {
			return err;
		}

		// 设置延迟
		err = set_frame_delay(delay);
		if (err != SUCCEEDED) {
			return err;
		}

		// 写入图像
		err = write_frame_image(img, p_quantize);
		if (err != SUCCEEDED) {
			return err;
		}

		// 结束帧
		err = end_frame();
		if (err != SUCCEEDED) {
			return err;
		}
	}

	return SUCCEEDED;
}

GIFWriter::GIFError GIFWriter::save_to_file(
	const String& p_path,
	const TypedArray<Image>& p_images,
	const PackedInt32Array& p_delays,
	const int p_loop_count,
	const bool p_quantize
) {
	if (p_images.is_empty()) return DATA_TOO_BIG;

	// 获取第一帧尺寸作为画布尺寸
	Ref<Image> first_img = p_images[0];
	if (first_img.is_null()) return DATA_TOO_BIG;

	// 创建 writer
	Ref<GIFWriter> writer;
	writer.instantiate();

	GIFError err = writer->open(p_path);
	if (err != SUCCEEDED) return err;

	// 设置画布尺寸
	err = writer->set_canvas_size(first_img->get_width(), first_img->get_height());
	if (err != SUCCEEDED) return err;

	// 写入所有帧
	err = writer->write_gif(p_images, p_delays, p_loop_count, p_quantize);
	if (err != SUCCEEDED) return err;

	// 关闭文件
	return writer->close();
}

Dictionary GIFWriter::save_to_buffer(
	const TypedArray<Image>& p_images,
	const PackedInt32Array& p_delays,
	const int p_loop_count,
	const bool p_quantize
) {
	Dictionary result;
	result["error"] = SUCCEEDED;
	result["data"] = PackedByteArray();

	if (p_images.is_empty()) {
		result["error"] = DATA_TOO_BIG;
		return result;
	}

	// 获取第一帧尺寸
	Ref<Image> first_img = p_images[0];
	if (first_img.is_null()) {
		result["error"] = DATA_TOO_BIG;
		return result;
	}

	// 创建 writer
	Ref<GIFWriter> writer;
	writer.instantiate();

	GIFError err = writer->open_from_buffer();
	if (err != SUCCEEDED) {
		result["error"] = err;
		return result;
	}

	// 设置画布尺寸
	err = writer->set_canvas_size(first_img->get_width(), first_img->get_height());
	if (err != SUCCEEDED) {
		result["error"] = err;
		return result;
	}

	// 写入所有帧
	err = writer->write_gif(p_images, p_delays, p_loop_count, p_quantize);
	if (err != SUCCEEDED) {
		result["error"] = err;
		return result;
	}

	// 关闭并获取缓冲区
	err = writer->close();
	if (err != SUCCEEDED) {
		result["error"] = err;
		return result;
	}

	result["error"] = SUCCEEDED;
	result["data"] = writer->get_output_buffer();
	return result;
}

GIFWriter::GIFError GIFWriter::add_comment(const String& p_comment) {
	if (!file_type) return NO_FILE_OPEN;

	CharString comment_utf8 = p_comment.utf8();
	int len = comment_utf8.length();

	if (len == 0) return SUCCEEDED;

	// 使用 EGifPutComment 写入注释 (支持多子块)
	if (EGifPutComment(file_type, comment_utf8.get_data()) == GIF_ERROR) {
		return WRITER_FAILED;
	}

	return SUCCEEDED;
}

int GIFWriter::get_frame_count() const {
	if (!file_type) return 0;
	return current_frame;
}

Vector2i GIFWriter::get_canvas_size() const {
	if (!file_type) return Vector2i(0, 0);
	if (!has_screen_desc) return Vector2i(0, 0);
	return Vector2i(file_type->SWidth, file_type->SHeight);
}
