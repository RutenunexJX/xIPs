# xIPs 功能与界面计划实施记录

版本：2.16.0。实现基于实际部署分支 `codex/cli-update-2142-20261021` 的 `c0a5172`，保留 2.14.4 普通资产功能边界；不包含历史 2.15.0 的 HDL/template 功能。

| 计划项 | 实现落点 | 验证 |
| --- | --- | --- |
| UI-01 概览与操作分层 | `BrowserPanel::showDetails/updateActions/showMoreActions` | 资产状态、工作/历史操作、只读及缺失状态、窄窗口 |
| UI-02 筛选与 Group 计数 | `BrowserPanel::filter`、`CatalogModel::data/visibleAssetCount` | 多 Group 结果去重，空结果复位，保留隐藏空组 |
| UI-03 独立窗口状态 | `MainWindow::saveBrowserState/restoreBrowserState/applyActivation` | 真实关闭重建、跨库隔离、显式资产优先；内嵌 state 接口不变 |
| UI-04 历史信息与目录树 | `BrowserPanel::selectVersion/rememberHistoryView`、独立 `WorkingFilesModel` | 日期/数量/备注，只读文件打开，自动刷新保留选择/展开/滚动 |
| UI-05 双版本及文本比较 | `SnapshotLibrary::compareSnapshots`、`RevisionCompareDialog` | 正反方向、同大小内容变化、缺失/删除版本、缺失对象、二进制拒绝、只读性 |
| UI-06 导入决策与重试 | `BrowserPanel::importFolders`、`SnapshotLibrary::previewImportTarget` | 实际目标绑定，预检无写入，失败保留选项，重试后只移动已归档选中项 |
| UI-07 普通保存命名 | `publishNamedSnapshot`、`saveCurrent/saveSelected/update`、`CatalogOperations` | 工作/收集资产命名、重名、同内容新名字、发布失败回滚、CLI 防篡改 |
| UI-08 固定引用比较 | `BrowserPanel::locateReference` | 选定的 pin/候选 UUID 正确，比较取消不改变 owner 或引用 |

数据格式与原生 BrowserApi v1 保持兼容。版本标签仍属于可变定义元数据，文件清单和 UUID 保持不可变；命名保存沿用 LibraryLock 和现有发布事务。失败回滚覆盖常规运行时错误，未新增跨进程崩溃恢复承诺。

比较服务只读取所选两版的元数据；打开/文本差异通过现有 `prepareFile` 验证对象。文本差异每侧限制 2 MiB、2000 行，识别 UTF-8 与 BOM 指定的 Unicode，其他编码/二进制保留文件级结果。任务切换或关闭取消过时后台读取，不把缺失对象当成相同内容。

Working files 继续仅在新增或修改时展示当前工作集；无改动、仅缺失、比较错误不展示历史文件。默认折叠、单击复选与文件浏览、不可覆盖的导入/导出、先验证归档再删除移动源等原有规则保持。

验证入口为 `tests/tst_usability.cpp`、既有 `gui_smoke`、`embedded`、`interactions`、`snapshots`、`workflows`、`catalog_model`、`working_model`、`cli`、`cli_session`、`native`、`native_layout`、`archive_host`、`archive_standalone`、`suite` 及 `tests/cli_workflow.py`。开发日志在 `build/ui-validation`；正式包另按干净提交、正式标签、BUILD_TESTING=OFF 构建并验收，发布结果记录绑定实际包校验和。

测试中修复了历史树延迟恢复覆盖新选择、窄窗口操作裁切、导入树与表单标签的深色主题问题。仅新建测试临时库、独立设置与会话目录，不修改用户资产库。

暂缓项维持原计划：完整 Project 依赖模型、收藏/最近使用、批量操作、嵌套 Group、Issues 重做、自动清理/自动更新引用、性能专项和第三方控件架构修改。

开发验收结果：上述 16 个受影响 CTest 目标已通过；公开 CLI 工作流完成 118 项操作检查。原生嵌入界面在 100%/150%/200% 缩放和 280/520/960 像素宽度、280×380 短面板上通过鼠标点击流程。正式 ZeroSlack 0.33.3 以隔离配置和公开 CLI 恢复 xIPs 面板成功，确认组件 DLL 来自指定构建、共享 Qt 来自宿主，宿主程序未修改。正式包仍需单独完成去测试钩子的构建与产物验收。
