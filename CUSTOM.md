引擎版本：Godot 4.7

此引擎经过定制修改。
为引擎增加了如下接口：

- `HashCalculator`：用于Hash、MD5之类的计算，可提供计算进度
- `MobilePersistentNotification`：移动端常驻通知和后台主循环单例，可在 Android 使用前台服务通知保持主线程后台运行；iOS 提供受系统限制的后台任务能力。提供 `start`、`update`、`stop`、`is_active`、`is_supported` 接口。

> 请在doc/classes中寻找对应的接口文档。

删除的接口：
- `EditorFeatureProfile.FEATURE_ASSET_LIB` 枚举常量
