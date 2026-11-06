# xIPs 功能与界面实施计划

实施状态：阶段 A、B（含 B2 文本差异）、C、D 已在 2.16.0 完成。实现与验证记录见 [实施记录](UI-FUNCTION-IMPLEMENTATION-20261107.md)。以下保留制定计划时的源码基线与范围约束。

日期：2026-11-07。状态：基于源码审查的待实施计划。

## 1. 基线与结论

本计划以 `D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021` 为源码根目录，分支 `codex/cli-update-2142-20261021`，提交 `c0a51723e04b18e4cf85bb4b9731b60ca38bb13c`，版本 **2.14.4**。编写前工作树无未提交改动。`D:/AppSuite/xIPs` 的 main 停留在另一条 2.15.0 历史线上，不作为本计划的实施基线。

目标是改善查找资产、辨认工作状态、浏览和比较版本、导入归档的使用流程。范围为功能与 UI；不安排性能优化、HDL 功能迁移或第三方控件整体重构。

源码表明，不应重新开发“无改动时 Working files 留空”“状态摘要”“隐藏空分组”“筛选清除”“版本重命名”“拖入后复制或移动并归档”等已有功能。主要缺口是信息仅在悬浮提示中呈现、操作层级不清、历史版本缺少比较入口、普通归档不能直接命名，以及独立窗口未接入现成的浏览状态保存接口。

本轮证据为实际源码和测试源码阅读，未启动 GUI、执行测试或测量用户操作。下文的交互收益属于设计建议，验收项均为实施后的要求，不代表已经验证通过。

## 2. 已核实的代码现状

| 编号 | 现状与证据 | 对计划的影响 |
| --- | --- | --- |
| E01 | [BrowserPanel 构造](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:451)集中创建顶部工具栏、分组树、资产标题及动作、Working files/Versions 两页。大部分次级操作由 `actionButton()` 创建为 28×28 纯图标按钮。 | 调整现有布局与动作展示，不另造一套浏览器。 |
| E02 | [showDetails](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:1955)把类型、描述、索引放入 tooltip；正文主要展示问题和并行版本提示。[CatalogModel::assetRow](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/CatalogModel.cpp:128)也只把类型、引用信息放入 tooltip。 | 增加可见的资产信息摘要，有具体数据来源，无需增加资产元数据。 |
| E03 | [readDetails](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:1862)调用 `previewWorking()`，已有新增、修改、缺失数量及错误状态。[showDetails](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:2014)在 clean 状态传空文件集，并默认折叠目录。仅缺失文件时仍留空，并行 heads 有单独处理。 | 保留判断规则，只改状态的可见位置与呈现。不能把“选中历史版本”当作“正在编辑”。 |
| E04 | [版本表创建](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:809)只有 Version、Status 两列；[填充版本](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:1977)已读取日期、文件数、备注，但仅用于 tooltip。[selectVersion](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:2084)将完整相对路径填进平面列表。 | 可直接完善版本摘要和文件树，无需修改归档格式。 |
| E05 | [Snapshot](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/library/SnapshotLibrary.h:12)已有 UUID、日期、备注、文件列表、父版本和对象映射；[comparePreview](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/library/SnapshotLibrary.cpp:2196)能比较待保存文件与最近归档，但没有任意两个历史版本的比较 API 或 GUI。 | 历史比较是真正的新能力；复用对象比较语义，不能只给现有预览按钮换名。 |
| E06 | [importFolders](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:1345)已有新建/已有目标、三种类型、勾选、复制/移动、版本名、备注及归档确认。当前摘要仅列文件数量，文件树初始全部展开。[savePrepared](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:2662)只有备注，没有版本名。 | 导入侧重组织信息；普通归档补齐命名能力。无需重新实现复制、移动、归档事务。 |
| E07 | [filter](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:1715)支持查询、类型、索引及隐藏空组；激活的筛选数量只在图标 tooltip/可访问名称中。现有 Clear 只清类型和索引，搜索框有自己的清除按钮。[分组显示](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/CatalogModel.cpp:43)已有主题色和加粗。 | 补充可见筛选条件和统一复位入口，保留现有查询语义及分组操作。 |
| E08 | [saveState/restoreState](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:3253)已有选中项、页签、筛选、分组折叠、工作文件勾选/展开、横竖分栏比例。[MainWindow](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/MainWindow.cpp:8)创建面板后只设上下文，关闭时没有保存这些状态；[main](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/main.cpp:131)也未接入持久保存。 | 应补独立版的保存/恢复调用，而非重复开发状态序列化。内嵌版继续由宿主管理。 |
| E09 | [showIssues](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:3103)已有原因、影响、重查、定位、移除及折叠技术详情；[runGroup](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:2343)成功后更新模型，不再发持续的 group saved 消息。 | 不把这些已完成的修复列为新增工作，不重写问题处理系统。 |
| E10 | [locateReference](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/app/BrowserPanel.cpp:2822)能选择 owner、检查历史、改变固定版本；核心 [referenceTarget/changeReference](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/library/SnapshotLibrary.cpp:2402)核验资产身份及归档。[CatalogDefinition](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/library/SnapshotLibrary.h:59)没有 Project→依赖资产关系字段，[CatalogGroup](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/library/CatalogGroups.h:7)没有父组字段。 | 可在现有引用切换中加入比较；完整 Project 依赖页和嵌套 Group 属于新数据模型，暂不实施。 |

## 3. 实施边界与复用方式

实际调用链为：`MainWindow/原生内嵌入口 → BrowserPanel → SnapshotLibrary/CatalogGroups → 资产定义、不可变归档及 ContentStore`。结构化 CLI 经 `CatalogOperations` 调用相同核心服务。

- `BrowserPanel` 管选择、界面及后台请求生命周期；`CatalogModel`、`WorkingFilesModel` 管展示。新摘要、筛选标签、比较选择均属于界面派生状态。
- 版本、来源、标签和固定引用以 `SnapshotLibrary` 数据为准。分组以 `CatalogGroups` 为准。不得在 UI 新增一套“正在编辑版本”持久标记。
- [LibraryTransaction](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/library/LibraryTransaction.cpp:14)提供同线程可重入的目录写锁，不提供自动回滚。扩展版本命名时必须沿用已有校验、发布和回滚逻辑，不能把加锁等同于事务完整性。
- 历史文件通过 [prepareFile](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/src/library/SnapshotLibrary.cpp:1221)校验并准备到库外只读缓存。比较页不得直接打开或修改归档对象文件。
- 新对话框保留现有 `Form` 的宿主销毁、上下文切换关闭行为。异步结果继续校验资产 ID、版本 UUID 和请求代次，过期结果不得覆盖新选择。
- 延续英文 UI、Ela 控件和现有 28/30 像素行高体系。仅视觉调整不新增镜像实现的测试；涉及选择、持久化及业务状态时补行为测试。不新增应用快捷键。

必须保持的行为：普通拖组移动成员关系、Ctrl 拖组增加关联，均不移动源文件；无新增或修改时工作文件区域保持空白；读取失败不得报成干净；历史内容只读；复制不覆盖不同内容；移动在归档校验成功后才清理所选来源；固定引用不得自动前移。

## 4. 分阶段实施

### 阶段 A：信息层级与浏览连续性

#### UI-01 资产概览及动作层级，优先级 P1

**问题：** 类型、说明、索引主要藏在 tooltip，工作状态只在 Working files 页内可见；多个资产动作排列为含义不直观的图标。依据 E01–E03。

**实施：**

1. 详情顶部保持紧凑两层：名称与当前页主操作；类型、来源方式和工作状态摘要。摘要中的“归档只读”与“当前工作文件状态”分别描述，避免混为一个状态。
2. 新增可折叠的 Details 区，呈现说明、索引、所属 Groups、工作目录或引用 owner。根据 `referencePath`、`discovered`、`document.workingArea` 区分引用、注册来源、应用创建的工作目录和仅归档资产。只显示确有数据的字段。
3. `Create version` 作为 Working files 的主操作；Versions 显示 `Copy to project`。来源打开与编辑保留可见文字入口，引用、移除等低频操作放入 More 菜单；仅归档资产仍有 New revision 入口，legacy 转换入口保留。菜单与按钮复用同一启用条件及处理函数。
4. 将 `Search IPs`、`My IPs`、分组空态中遗留的 IP 专用表述调整为覆盖 Module/IP/Project 的资产用语。保持库路径可查看、可复制。
5. 复用当前 580/360 宽度适配，依据详情区实际可用宽度安排动作。独立窄窗口和内嵌面板均检查，不能只调用当前仅对内嵌生效的 `adaptEmbeddedLayout()`。

**修改位置：** `BrowserPanel.{h,cpp}` 的构造、`showDetails`、`readDetails`、`updateActions`、`adaptEmbeddedLayout`；图标和样式仅在需要时调整 `UiSupport`。不改归档数据格式。

**验收：** 切换资产、页签或库时摘要与动作始终对应当前资产；检查中、干净、缺失、失败和并行版本可区分；无改动时文件区仍为空；引用不出现可写操作；720×420 独立窗口和 280×380 内嵌窗口的操作均可到达。

#### UI-02 可见筛选条件与分组计数，优先级 P1

**问题：** 筛选面板收起后，用户主要依赖图标和 tooltip 判断列表为何减少。依据 E07。

**实施：** 在有条件时显示紧凑摘要，例如 `Type: Project ×`、`Tag: uart ×` 和结果数量；逐项移除类型/索引，并提供 `Reset search and filters` 同时清搜索、类型、索引。隐藏空组仍是独立偏好，不被复位。现有 Clear 更名为 `Clear filters`，明确其范围。

分组名旁显示匹配数/当前已加载成员总数。按资产 ID 计数，多个 Group 中的同一资产不得重复计入全库结果数。沿用现有分组颜色和未分组资产展示，不引入嵌套组或移动目录语义。

**修改位置：** `BrowserPanel::filter/refreshIndexes`、筛选控件及信号；`CatalogModel::data/filterChildren`。界面摘要从现有 `m_search/m_category/m_indexTerm` 派生，不再保存副本。

**验收：** 收起筛选面板后仍能知道有效条件；带引号的索引查询不变；无结果时一键恢复；筛选前后选择恢复、空组开关及拖组行为不变。按实际差异扩展 `tst_catalog_model`、`gui_smoke`。

#### UI-03 独立版恢复上次浏览位置，优先级 P1

**问题：** 面板已有完整状态接口，独立窗口没有保存/加载调用。依据 E08，属于源码确认的接入缺口。

**实施：** 在 `MainWindow` 层使用本机 `QSettings` 保存窗口几何及按库隔离的 `BrowserPanel::saveState()`；正常关闭和完成库切换时保存，启动/切换目标库后恢复。显式 `--library`、`--open-asset`、搜索激活和宿主提供的上下文优先于旧状态。利用现有 pending state 机制，等扫描可消费状态时恢复。

需要切库前通知时，为 `BrowserPanel` 增加有明确前后时机的信号，不在视图层直接写全局配置。保存库 A 状态后才开始库 B 切换，避免库键与内容错配。新信号不改变公开 native C ABI。状态放本机配置，不写入坚果云共享的 `.xips`；盘符变更后重新选择根目录即可，不能让旧状态把新根目录切回去。

**修改位置：** `MainWindow.{h,cpp}`、必要的 `BrowserPanel` 上下文信号；核对 `main.cpp` 激活次序。旧设置缺失或无效时回退默认布局。内嵌面板不接入独立窗口设置。

**验收：** 实际创建、关闭、重建独立窗口，恢复库内选中资产/版本、筛选、空组开关、折叠和分栏；两个库不串状态；显式打开另一资产生效；已删除资产/版本安全回退；拒绝关闭活跃归档窗口时不误当作完成关闭。测试使用临时配置目录。

### 阶段 B：版本浏览与比较

#### UI-04 版本摘要与历史文件树，优先级 P1，依赖 UI-01

**实施：**

1. 将现有 `Snapshot.created/note/files` 提升为可见信息：宽布局显示版本、日期、文件数，选中行下显示完整备注；窄布局保留版本名和摘要行，完整信息可展开。统一的只读提示放在版本区，不让每行相同的 Archived 占据主要空间。
2. 标明选中的历史版本和并行 heads。选择、重命名、删除、复制仍以 UUID 为准；标签相同或并行序号相同不能选错版本。不把列表排序或时间先后解释为祖先关系。
3. 将历史路径平面列表改为默认折叠的只读目录树。可复用独立的 `WorkingFilesModel` 实例，以 `editable=false` 构建；将该实例表头改为 Files。保持与工作区勾选状态隔离。
4. 同步修改 `selectVersion/openFile`：相对路径取 `Qt::UserRole`，只有 `Qt::UserRole+1` 表示文件时可打开，不能把目录节点或显示的文件名当完整路径。按资产 UUID＋版本 UUID 保存当前会话中的历史树选择和展开状态。

**修改位置：** `BrowserPanel` 的版本构建、`showDetails/selectVersion/openFile/updateActions`；复用 `WorkingFilesModel::setFiles/pathIndex`。已有字段不变，只有展示变化。

**验收：** 长版本名、空备注、多层同名文件、日期缺失均可辨认；历史目录不能当文件打开；刷新后仍打开所选版本的正确文件；重命名不改变 UUID、内容及引用。更新现有断言固定为“两列”的 UI 测试，使其验证实际信息和操作，而非固定列数。

#### UI-05 比较两个历史版本，优先级 P1，依赖 UI-04

**最小完整范围：** 选择基准版本和目标版本，列出新增、修改、仅基准存在、相同文件及数量；明确比较方向，允许交换两端。选择一个版本后进入 Compare，通过第二个选择器选基准，不为此把整个资产树改成多选。

**核心实施：**

- 在 `SnapshotLibrary` 增加只读的历史比较接口，接受资产及两个准确的 revision UUID，返回两端身份和逐文件结果。读取时复核资产和版本有效性，禁止将 `current` 冒充历史版本。
- 从现有 `comparePreview()` 提取对象映射差异计算，让保存预览和历史比较复用；保留保存预览当前的基准选择和 heads 校验语义。`ContentObject` 已有 hash、size，普通新格式归档可直接据此比较文件内容标识。
- 对缺少对象映射的旧归档按现有验证/指纹回退读取；不支持的 legacy 状态明确提示转换，不能返回空差异。移动/改名初版按旧路径缺失和新路径新增呈现，不进行推测式重命名识别。
- 比较结果只证明清单中记录的内容标识差异；不能据此声称对象已通过完整性校验。用户打开任一侧文件时继续调用 `prepareFile()`。
- 默认基准仅在选中版本有唯一可用父版本时预选；并行父版本或已删除父版本要求显式选择，避免用“上一行”替代父版本。

**UI 实施：** 新增专用 `RevisionCompareDialog.{h,cpp}`，文件表显示状态、相对路径、两侧大小，支持按状态筛选及打开两侧只读文件。它只持有本次比较结果，不保存第二份历史。关闭对话框、切库、销毁宿主时取消/丢弃过期请求。

**文本差异后续子步：** 在文件级比较验收后增加只读文本差异显示，复用两侧已校验缓存；明确编码、换行和二进制判定。设置可解释的文本预览边界，超出时保留文件级比较和只读打开，不显示伪造的空差异。该子步可独立交付，不作为文件级比较的隐性前置条件。

**修改位置：** `SnapshotLibrary.{h,cpp}`，必要的纯差异计算文件；新增比较对话框，`BrowserPanel` 的 Compare 入口及 `src/CMakeLists.txt` 注册。现有 CLI/原生接口不因界面比较而强制增加新协议。

**验收：** 相同版本、单边新增/缺失、同大小不同内容、中文与空格路径、并行 heads、被删版本、缺失或损坏对象；两侧可打开文件的内容与 UUID 对应；比较与取消不改变源文件、定义、归档和引用。核心补 `tst_snapshots` 行为测试，界面补 `gui_smoke` 和实际内嵌关闭场景。

### 阶段 C：导入与命名一致性

#### UI-06 重整导入表单，优先级 P2

**实施：** 保留当前一次确认完成导入并归档的流程，在同一对话框中分为目标、文件选择、归档设置三个紧凑区域，不强制拆成三个模态窗口。现有流程已经完整，调整目的在于减少信息混杂。

- 目标区明确新建/已有、名称、类型及实际目标工作目录；已有目标以稳定 ID 选择。同名资产显示类型及相对路径辅助区分。
- 文件区默认折叠，提供 All 和按需展开，显示已选数量/大小；目录勾选仍作用于子项。
- 归档区有固定 Version/Note 标签，不只依赖 placeholder；复制/移动说明始终可见。
- 确认摘要展示目标、版本名或自动名称、已选数量/大小、Copy/Move，以及成功后立即创建版本。显式显示新资产将加入的当前 Group。
- 格式错误和重复名称在原表单内提示并保留选择。目标冲突预检必须落在核心服务并复用 `importInputs/stageImports` 的规则；最终提交仍复核文件指纹、目标及版本，预检通过不代表允许覆盖。

**修改位置：** `BrowserPanel::importFolders`；需要目标预检时扩展 `SnapshotLibrary` 的导入预览能力。表单确需独立文件时，只抽出被复用的 Form/字段组件，保留销毁和上下文关闭语义，不顺带拆分整个 BrowserPanel。

**验收：** 新建/已有三种类型、空选择、嵌套目录、隐藏文件、Copy/Move、重名版本、目标冲突、预览后内容变化、取消、部分来源无法删除；确认前不写入或移动文件，失败后原选择可修改重试。复用 `FolderImportGui.inc` 和导入事务测试。

#### UI-07 普通创建版本支持直接命名，优先级 P2，可与 UI-06 分开实施

**实施：** 给 `savePrepared` 增加 Version 字段，与导入共用名称规则：空值自动 revN、去除首尾空白、长度与控制字符限制、同资产内不区分大小写的重名校验。已归档版本的 Edit version 继续只改显示名。

这不是仅增加一个文本框。当前 `saveSelected/saveCurrent/update` 无版本名参数，需要增加向后兼容的可选参数或请求对象，并在同一次核心保存流程中完成版本清单发布和 `revisionLabels` 写入。复用 `checkedRevisionLabel/setRevisionLabel` 及导入中已有的失败回滚顺序；不能先保存成功，再从 UI 调用 rename，把半成功当作成功。

同内容且空名保持“不创建重复版本”；同内容且给出不同的合法新名，与现有导入行为一致，创建独立版本而不是重命名旧版本。相同标签应遵循现有导入复用/冲突规则。继续核验预览期望、父版本及定义，保留 immutable manifest 不写显示名的约束。

同步调整直接调用方。结构化 CLI 的 `save` 若接入该可选字段，必须同时更新 `CatalogOperations` 字段白名单、规范化预览、应用调用和 `docs/structured-cli.md`；旧请求默认行为不变。不得只修改执行端而让预览忽略版本名。

**修改位置：** `BrowserPanel::savePrepared`；`SnapshotLibrary` 的保存接口、`saveSourceRevision/update`；必要的 `CatalogOperations` 接入。锁继续用 `LibraryTransaction`，标签继续用既有定义格式。

**验收：** 自动命名、自定义名、重复/非法名、同内容新名、并行版本、预览后修改、定义写入失败、归档发布失败；失败不遗留对用户可见的半成品版本或标签，旧归档不可变，引用不变化。保留未引用对象的现有允许行为，不扩大为对象清理工程。

### 阶段 D：引用切换前查看差异

#### UI-08 固定引用的信息与比较，优先级 P2，依赖 UI-05

在现有 `locateReference` 流程显示当前固定版本的标签/UUID、owner 和候选版本摘要，增加“比较当前固定版本与候选版本”。比较使用 `referenceTarget()` 返回的原始资产；确认后仍调用 `changeReference()`，不修改 owner 或自动推进引用。

owner 不可用或当前版本不可读取时明确显示状态，继续提供原有定位/移除入口，不能把无法比较显示为无变化。并行 heads 显示多个候选，不使用含义不明的“自动更新到最新”。

**验收：** 更换盘符后的 owner 定位、资产 ID 不匹配、固定版本丢失、多个候选版本、取消及明确确认；owner 文件和其他引用不变。复用引用生命周期测试和比较服务测试。

## 5. 验证与交付顺序

建议分为四个可验收批次，不一次重做所有页面：

| 批次 | 内容 | 完成判据 |
| --- | --- | --- |
| A | UI-01、UI-02、UI-03 | 状态与当前操作可辨认；筛选可解释；独立版关闭重开恢复正确；既有 Working files 规则不变。 |
| B | UI-04、UI-05 文件级比较 | 看得见版本信息，能准确打开历史文件，能选定两版查看文件差异。文本差异可作为 B2 单独交付。 |
| C | UI-06、UI-07 | 导入决策明确，普通保存和导入命名一致，失败及取消无半成功。 |
| D | UI-08 | 固定引用切换前可检查具体候选版本差异，owner 不被修改。 |

按影响选择现有测试，不机械地每批全量重跑：

| 影响 | 已存在的验证落点 | 必须补充的行为 |
| --- | --- | --- |
| 工作状态和界面操作 | [WorkingVisibilityGui.inc](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/tests/WorkingVisibilityGui.inc:1)、`tst_gui_smoke.cpp`、`tst_interactions.cpp` | 状态移位后仍对应当前选择；无改动/仅缺失/错误时不露出历史文件；菜单入口有效。 |
| 筛选、Group | `tst_catalog_model.cpp`、`groupsCanBeCreatedAndOrganized`、拖组用例 | 结果去重计数、摘要移除、统一复位、不改变成员身份。 |
| 独立版状态 | `MainWindow` 新的临时设置场景；现有面板 state 测试作参照 | 真正关闭再创建窗口、跨库隔离、启动显式目标优先，不止直接调用 save/restore 自证。 |
| 历史文件/比较 | `tst_snapshots.cpp`、`VersionNamesGui.inc`、`ArchivedViewGui.inc` | 双版本方向、UUID 选择、对象损坏、只读打开、旧格式回退。 |
| 导入/命名 | `tst_workflows.cpp`、`FolderImportGui.inc`、`tst_cli.cpp`、相关结构化 CLI 用例 | 预览绑定版本名、重名和同内容规则、核心发布失败回滚。 |
| 内嵌与切换 | [NarrowLayoutGui.inc](D:/AppSuite/_worktrees/xIPs-cli-update-2142-20261021/tests/NarrowLayoutGui.inc:2)、`tst_embedded.cpp` | 280/520/960 宽度及 280×380 短面板；弹窗中切上下文、销毁宿主；宿主状态不被独立设置覆盖。 |

视觉验收使用实际宽/窄界面、浅/深主题、长名称/长路径，以及 100%/150%/200% 显示缩放。检查按钮点击区、文字截断、滚动和空态；颜色之外保留文字与图标表达状态。不以 offscreen 截图代替真实宿主的操作验证。

每批实施前复核本计划对应源码是否变化；实施后记录代码差异、实际运行的检查、未通过项和界面证据。用户当前要求为编写计划，本轮不进行功能实现或发布。后续实施交付的提交、push、打包和正式包替换遵循既有授权及当前统筹发布分工。

## 6. 后续候选与覆盖限制

- **完整 Project 依赖管理：暂缓。** 先定义 Project→资产→固定 revision 的归属模型、迁移、导出与更新规则。现有普通引用记录和 receipt 不能直接证明某个 Project 的完整依赖关系。
- **收藏、最近使用、批量分组/导出：暂缓。** 当前资产树为单选，批处理还需要明确选择范围、部分失败和重复成员语义；最近/收藏需要确定本机或共享归属。这些不是简单增加按钮。
- **嵌套 Group：暂缓。** 当前 Group 只有 id/name/members。分组视觉层级已存在，增加真正嵌套需单独的数据模型与兼容设计。
- **Issues 重构：不重复安排。** 当前已提供分类原因、影响、恢复动作和折叠技术详情。可在后续实际使用中收集剩余问题后定向修改。
- **自动恢复、自动清理、自动更新引用：不纳入本计划。** 本计划保持用户明确选择内容和版本的既有流程。

本次已审范围是上述浏览、筛选、版本、导入、命名、引用和状态持久化调用链；测试仅检查源码覆盖点。Vivado Archive Project 引擎、第三方 Ela 内部、历史 AssetLibraryService 全实现和 ZeroSlack 宿主自身的保存逻辑不属于本次全面审查范围。内嵌集成以已公开接口及现有测试为依据，实施时仍需在实际宿主验证。
