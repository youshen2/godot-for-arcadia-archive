引擎版本：Godot 4.7

此引擎经过定制修改。
为引擎增加了如下接口：

- `HashCalculator`：用于Hash、MD5之类的计算，可提供计算进度
- `AssetBundle`：读取 JSON manifest 并按清单挂载目录型 AssetBundle（`bundle.json` + 随机名 chunk 文件），提供 bundle/版本/chunk 查询、同步或逐步加载进度、已缓存资源热替换、资源加载接口、基于 `HashCalculator` 的完整性校验，以及和其它 manifest 的差分查询；热替换使用 `CACHE_MODE_REPLACE`，可让新包中的资源复用已经缓存、基础包、已挂载包或同次批量加载包中的外部依赖。
- `MobilePersistentNotification`：移动端常驻通知和后台主循环单例，可在 Android 使用前台服务通知保持主线程后台运行；iOS 提供受系统限制的后台任务能力。提供 `start`、`update`、`stop`、`is_active`、`is_supported` 接口。
- `HTTPFileDownloader`：高性能 HTTP/HTTPS 文件下载节点，支持自适应或固定线程数的 Range 分段下载、批量下载、失败自动单连接回退、实时进度/字节数/总字节数/耗时/预计剩余时间/速度查询，以及 GDScript 和 Mono / C# 绑定可见的信号与状态接口。

> 请在doc/classes中寻找对应的接口文档。

删除的接口：
- `EditorFeatureProfile.FEATURE_ASSET_LIB` 枚举常量
