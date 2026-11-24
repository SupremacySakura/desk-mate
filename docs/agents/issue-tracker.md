# 议题跟踪器：本地 Markdown 文件

此仓库的议题和规格以 Markdown 文件形式保存在 `.scratch/` 中。

## 约定

- 每个目录对应一个功能：`.scratch/<feature-slug>/`
- 规格文件为 `.scratch/<feature-slug>/spec.md`
- 每个实现任务单单独保存为 `.scratch/<feature-slug>/issues/<NN>-<slug>.md`，从 `01` 开始编号；不要将多个任务单合并到一个文件中
- 在每个议题文件顶部附近用 `Status:` 行记录分诊状态（角色字符串见 `triage-labels.md`）
- 在文件末尾的 `## Comments` 标题下追加评论和对话记录

## 当技能要求“发布到议题跟踪器”时

在 `.scratch/<feature-slug>/` 下创建一个新文件（必要时先创建目录）。

## 当技能要求“获取相关任务单”时

读取引用路径中的文件。用户通常会直接提供路径或议题编号。

## Wayfinder 操作

由 `/wayfinder` 使用。**地图**是一个文件，每个任务单对应一个**子文件**。

- **地图**：`.scratch/<effort>/map.md`（包含 Notes / Decisions-so-far / Fog 正文）。
- **子任务单**：`.scratch/<effort>/issues/NN-<slug>.md`，从 `01` 开始编号，正文中写问题。`Type:` 行记录任务单类型（`research`/`prototype`/`grilling`/`task`）；`Status:` 行记录 `claimed`/`resolved`。
- **阻塞关系**：在文件顶部附近添加 `Blocked by: NN, NN` 行。当列出的每个文件都标记为 `resolved` 时，任务单才解除阻塞。
- **当前边界**：扫描 `.scratch/<effort>/issues/`，查找尚未关闭、未阻塞且未认领的文件；优先选择编号最小的文件。
- **认领**：开始任何工作之前，设置 `Status: claimed` 并保存。
- **解决**：在 `## Answer` 标题下追加答案，设置 `Status: resolved`，然后在 `map.md` 的 Decisions-so-far 中追加上下文链接（摘要 + 链接）。
