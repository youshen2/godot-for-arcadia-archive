#ifndef GIF_TEXTURE_H
#define GIF_TEXTURE_H

#include "gif_reader.h"

#include "core/io/resource_loader.h"
#include "scene/resources/image_texture.h"

class GIFTexture : public Texture2D {
	GDCLASS(GIFTexture, Texture2D);

private:
	// GIF 原始数据
	PackedByteArray gif_data;
	mutable Size2i size;

	// 帧数据
	mutable TypedArray<ImageTexture> frames;						// 按需解码后的帧纹理
	mutable PackedFloat32Array frame_delays;						// 每帧延迟（秒）

	// 当前显示
	int current_frame = 0;
	mutable int frame_count = 0;

	// 全局属性（来自 GIF 文件）
	mutable int loop_count = 0;

	// 按需解码状态
	mutable Ref<GIFReader> lazy_reader;
	mutable PackedByteArray canvas;
	mutable PackedByteArray previous_canvas;
	mutable int decoded_frame_count = 0;

	void _reset_runtime_cache() const;
	bool _ensure_metadata() const;
	bool _ensure_decoder() const;
	bool _ensure_frame_decoded(int p_frame) const;
	void _decode_frame_to_cache(int p_frame) const;

protected:
	static void _bind_methods();

public:
	GIFTexture();
	~GIFTexture();

	virtual Image::Format get_format() const override;
	virtual int get_width() const override;
	virtual int get_height() const override;
	virtual bool has_alpha() const override;
	virtual RID get_rid() const override;
	virtual Ref<Image> get_image() const override;

	void set_data(const PackedByteArray& p_data);
	PackedByteArray get_data() const;

	void set_metadata_size(const Vector2i& p_size);
	Vector2i get_metadata_size() const;
	void set_metadata_frame_count(int p_frame_count);
	int get_metadata_frame_count() const;
	void set_metadata_frame_delays(const PackedFloat32Array& p_frame_delays);
	PackedFloat32Array get_metadata_frame_delays() const;
	void set_metadata_loop_count(int p_loop_count);
	int get_metadata_loop_count() const;
	void set_metadata(const Vector2i& p_size, int p_frame_count, const PackedFloat32Array& p_frame_delays, int p_loop_count);

	// 加载
	static Ref<GIFTexture> load_from_file(const String& p_path);
	static Ref<GIFTexture> load_from_buffer(const PackedByteArray& p_buffer);

	// 帧访问
	void set_frame(int p_frame);									// 设置当前显示帧
	int get_frame() const;											// 获取当前帧索引
	int get_frame_count() const;									// 获取总帧数

	Ref<ImageTexture> get_frame_texture(int p_frame) const;			// 获取指定帧纹理
	Ref<ImageTexture> get_current_texture() const;					// 获取当前帧纹理

	// 帧元数据
	float get_frame_delay(int p_frame) const;						// 获取帧延迟 (sec)
	float get_total_duration() const;								// 获取总时长 (sec)

	// 全局属性
	int get_loop_count() const;										// GIF 本身循环次数 (0=无限)
};

class ResourceFormatLoaderGIFTexture : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderGIFTexture, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool recognize_path(const String &p_path, const String &p_for_type = String()) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};

#endif // !GIF_TEXTURE_H
