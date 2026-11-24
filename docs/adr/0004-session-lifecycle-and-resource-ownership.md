# 以单一控制线程管理 Session 生命周期

Status: accepted

Device 的单一控制线程独占 Session 状态转换及解码器 Flush/Reset/Stop/Destroy；UI、USB、Surface 和解码回调只投递带 Session 代际的命令，不在回调线程同步清理。ArkTS 独占 `USBAccessoryHandle`，C++ 只借用 `accessoryFd`，绝不直接关闭或重复关闭它。Host 以采集、编码、libusb、虚拟显示器的顺序串行清理，防止跨线程等待和异步回调访问已销毁资源。

Surface 由 `SurfaceOwner` 管理；取得租约时以 `OH_NativeWindow_NativeObjectReference` 增持，控制线程完成解绑后以 `OH_NativeWindow_NativeObjectUnreference` 释放。其引用操作不是线程安全的，必须串行化，不得在同一窗口上并发增减引用。Surface 失效后停止送显；Host 收到暂停后停止采集和编码，在 30–60 秒初始宽限期内保留虚拟显示器，超时销毁，避免窗口长期滞留在不可见屏幕。

正常清理先拒绝新工作并唤醒 I/O，等待读写线程退出后通知 ArkTS 归还句柄。Gate 1 依序验证 `poll`/`eventfd`、无 `SA_RESTART` 的定向信号中断和官方关闭行为；`shutdown()` 不是 USB 字符设备的可用取消原语。如果只能由 `closeAccessory()` 唤醒阻塞 I/O，则采用经验证的“停止接收新工作 → 关闭所有者句柄唤醒 → join → 确认资源释放”分支，不能同时强求先 join 后 close。

只有健康连接下的拥塞/GOP 切换保证在途帧完整；进入 `STOPPING` 后取消双向 transfer。`active_transfers` 只在提交成功后增加、在 transfer 回调中减少；libusb 事件循环持续到归零，随后释放 transfer、接口、设备句柄和上下文。HarmonyOS 后台能否维持连接及心跳仍须 Gate 1/3 实测，超时按断连处理。
