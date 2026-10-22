# PICO 4 构建与安装指南（简体中文）

> 重要：即使 APK 编译成功，也**不代表**能在 PICO 4 上正常运行。必须在真实的 PICO 4 头显上测试。

## 1. 打开 GitHub Actions
打开 `https://github.com/IvanChen-ctrl/RTCWQuest`，点击顶部的 **Actions** 标签页。

## 2. 手动触发构建
Actions → 左侧选择 **Android Debug APK** → 右侧 **Run workflow** → 选择分支（通常是 `master`）→ 点击绿色的 **Run workflow**。
（该工作流只支持手动触发，不会自动运行。如果看不到它，请先确认含有该工作流的 PR 已合并到默认分支。）

## 3. 查看成功或失败的运行记录
Actions 页面的运行列表中：绿色对勾 = 成功，红色叉 = 失败。点进运行记录，再点击 **build** 任务，可展开每一步的日志。失败时请展开红色步骤，复制最后约 50 行错误信息。成功时页面底部的 **Summary** 会显示 APK 大小和 SHA-256。

## 4. 下载 APK
运行记录页面底部 **Artifacts** 区域，下载 `RTCWQuest-PICO4-Debug-ARM64`（保留 2 天，过期需重新构建）。下载后在 Windows 上右键解压，得到 `rtcwquest-debug.apk` 和 `.sha256` 文件。可选校验：
```powershell
Get-FileHash .\rtcwquest-debug.apk -Algorithm SHA256
```

## 5. 使用 ADB 在 Windows 上安装
先安装 Android platform-tools（含 adb）。然后在解压目录运行：
```powershell
adb install -r -d .\rtcwquest-debug.apk
```

## 6. 检查 PICO 4 USB 调试连接
1. PICO 4：设置 → 通用 → 关于本机 → 连续点击软件版本号，开启开发者模式；然后在 开发者选项 中开启 **USB 调试**。
2. 用 USB 线连接电脑，戴上头显，允许“USB 调试”授权弹窗。
3. 运行 `adb devices`，应看到设备状态为 `device`（`unauthorized` 表示尚未授权，`offline` 请重新插拔）。

## 7. 抓取 Android 和 OpenXR 日志
```powershell
adb logcat -c
adb shell monkey -p com.drbeef.rtcwquest -c android.intent.category.LAUNCHER 1
adb logcat -d > logcat.txt
```
筛选可疑信息：
```powershell
adb logcat -d | findstr /i "rtcw openxr xr pico FATAL AndroidRuntime"
```
闪退时请保存完整的 `logcat.txt`。

## 8. 确认应用包名
预期包名为 `com.drbeef.rtcwquest`（来自构建配置，需在真机验证）。检查是否已安装：
```powershell
adb shell pm list packages | findstr rtcw
```

## 9. 放置你合法拥有的 RTCW 游戏文件
本项目**不包含**商业游戏数据，也不会提供。请使用你合法购买的《重返德军总部》文件：先启动一次应用以创建目录 `/sdcard/RTCWQuest/Main`，然后复制 `.pk3` 文件（如 `pak0.pk3`）：
```powershell
adb push .\pak0.pk3 /sdcard/RTCWQuest/Main/
```
具体要求请以仓库 `README.md` 为准。

## 10. 如何汇报运行问题
请提供：
- 使用的 Actions 运行链接和提交号（commit）
- PICO 4 系统版本
- 具体现象（黑屏/闪退/画面异常/手柄无反应等）和操作步骤
- 完整 `logcat.txt`
- 如有可能，附上照片或录屏

这些信息将用于下一个里程碑的修复工作。
