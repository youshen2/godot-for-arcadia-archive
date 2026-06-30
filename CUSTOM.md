引擎版本：Godot 4.7

此引擎经过定制修改。
为引擎增加了如下接口：

- `HashCalculator`：用于Hash、MD5之类的计算，可提供计算进度
- `AssetBundle`：读取 JSON manifest 并按清单挂载目录型 AssetBundle（`bundle.json` + 随机名 chunk 文件），提供 bundle/版本/chunk 查询、同步或逐步加载进度、已缓存资源热替换、资源加载接口、基于 `HashCalculator` 的完整性校验，以及和其它 manifest 的差分查询；热替换使用 `CACHE_MODE_REPLACE`，可让新包中的资源复用已经缓存、基础包、已挂载包或同次批量加载包中的外部依赖。通过 AssetBundle 或 `ProjectSettings.load_resource_pack()` 以替换模式动态挂载资源包时，脚本型 Autoload 单例会重新加载脚本并重建已有单例节点的脚本实例；该行为不新增脚本 API，GDScript 可直接生效，Mono / C# 仍受运行时程序集热重载能力限制。
- `MobilePersistentNotification`：移动端常驻通知和后台主循环单例，可在 Android 使用前台服务通知保持主线程后台运行；iOS 提供受系统限制的后台任务能力。提供 `start`、`update`、`stop`、`is_active`、`is_supported` 接口。
- `HTTPFileDownloader`：高性能 HTTP/HTTPS 文件下载节点，支持自适应或固定线程数的 Range 分段下载、批量下载、失败自动单连接回退、实时进度/字节数/总字节数/耗时/预计剩余时间/速度查询，以及 GDScript 和 Mono / C# 绑定可见的信号与状态接口。
- `MarkdownTextLabel`：独立 Markdown 显示控件，基于 md4c 解析 CommonMark/GFM Markdown，提供 `text`、`parse_markdown`、`append_text`、`clear`、纯文本、内容尺寸、链接点击信号、自动换行和 BiDi 排版接口，GDScript 和 Mono / C# 可用。
- `FileAccess.humanize_size(size)`：将字节数转换为人类可读的 IEC 存储大小字符串，供 GDScript 和 Mono / C# 调用。
- `SQLite`：内置的 godot-sqlite SQLite 数据库访问模块，提供数据库打开/关闭、SQL 查询、位置或命名参数绑定、表结构辅助创建、增删改查、备份/恢复、JSON 导入导出、扩展加载和 SQLite 返回码常量；通过 ClassDB 注册并提供中文 doc_class，GDScript 和 Mono / C# 可用。
- `VideoStreamPlayer`：新增 FFmpeg 播放相关设置接口：`enable_audio`、`audio_speed_to_sync`、`playback_speed_override`、`pitch_adjust`、`color_profile`、`debug`、`max_video_fps`、`max_video_frames_per_update`、`enable_frame_dropping`、`key_frame_only`、`accurate_seek`、`apply_rotation_metadata`、`video_track`、`loop_start`、`loop_end`。这些接口通过 ClassDB 注册，GDScript 和 Mono / C# 均可见。
- `VideoStreamPlayback`：新增播放器侧设置下发虚方法：`_set_video_track`、`_set_audio_enabled`、`_set_audio_speed_to_sync`、`_set_audio_buffering_msec`、`_set_pitch_adjust_enabled`、`_set_color_profile`、`_set_debug_enabled`、`_set_max_video_fps`、`_set_max_video_frames_per_update`、`_set_frame_dropping_enabled`、`_set_key_frame_only_enabled`、`_set_accurate_seek_enabled`、`_set_apply_rotation_metadata_enabled`，用于视频播放实现接收 `VideoStreamPlayer` 设置。GDScript 扩展和 Mono / C# 可见。
- `VideoStreamFFmpeg`：新增基于 FFmpeg 的视频资源，负责视频文件/URI、请求头、视频/音频/字幕流列表、流元数据、章节信息和时长探测，并为 `VideoStreamPlayer` 创建 FFmpeg 播放实例。GDScript 和 Mono / C# 可用。
- `AudioStreamFFmpeg`：新增基于 FFmpeg 的音频资源，供 `AudioStreamPlayer`、`AudioStreamPlayer2D`、`AudioStreamPlayer3D` 播放 FFmpeg 支持的音频文件或媒体容器音频流。支持请求头、指定流索引、ICY 元数据读取、标签和时长探测，并在播放器上提供 `parameters/loop`、`parameters/loop_start`、`parameters/loop_end`、`parameters/end_time`、`parameters/downmix_to_mono` 播放参数。GDScript 和 Mono / C# 可用。

使用时不需要ClassDB.class_exists和ClassDB.instantiate，直接当现有存在的类就行。

> 请在doc/classes中寻找对应的接口文档。

删除的接口：
- `EditorFeatureProfile.FEATURE_ASSET_LIB` 枚举常量
