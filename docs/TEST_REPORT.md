# 战术网格背包系统 · 测试与基准报告

> 交付物之二（实施计划第四节）。所有数字都是本机实测，命令附在文末。
> 环境：Apple M1 Pro (MacBookPro18,3) / macOS 27.0.1 / rustc + cargo 1.99.0 /
> UE 5.8（`/Users/Shared/Epic Games/UE_5.8`）/ Xcode clang 21。

---

## 1. 测试总览

`cargo test` → **122 项测试，0 失败**（13 个测试二进制）。

| 测试目标 | 数量 | 覆盖内容 |
|---|---|---|
| src 内嵌单元测试 | 37 | 网格几何 / 位图与扫描一致 / 目录校验 / 容器 / 聚合 / 原子操作 / 成环 / 规则 / 快照 / FFI 冒烟 / 端到端冒烟 |
| `tests/grid_edges.rs` | 12 | 贴边 / 角点 / 越界 / 旋转阻塞 / 64 格位图边界 / 快慢路径逐点等价 / `can_place_pair` |
| `tests/inventory_basics.rs` | 8 | 根容器 / 定义校验 / 聚合（含嵌套）/ 标签查询 / 目录顺序确定性 / 销毁子树 |
| `tests/atomic_moves.rs` | 17 | move / swap 成功与失败路径，失败用**快照字节**证明零副作用 |
| `tests/stacking.rs` | 10 | 合并部分/填满/溢出/异构，拆分边界与耐久继承 |
| `tests/nesting.rs` | 8 | 套包 / 成环（直接、深层、自我）/ 深度上限 / 黑名单按根类型判定 |
| `tests/autosort.rs` | 7 | 守恒 / 确定性 / 复合容器跨 part / 空容器 / 失败回滚 / 黑名单保持 |
| `tests/weight.rs` | 7 | 负重：上限 / 比例 / 超重判定 / 嵌套与堆叠计重 / 几何操作不改变总重 / FFI 路径 |
| `tests/snapshot.rs` | 6 | 往返保真 / 确定性 / 典型装载 ≤ 2 KiB / 损坏数据拒绝且不破坏现状 |
| `tests/ffi_smoke.rs` | 6 | C ABI 全函数返回码语义 / 两段式缓冲 / 内存释放 / last_error |
| `tests/properties.rs` | 3 | proptest（64 cases/组）：随机操作序列原子性、位图↔扫描等价、位图↔单元格一致 |
| `tests/perf.rs` | 1 | release 性能门禁（≤ 500 ns） |

关键验证手段：**“失败零副作用”全部用 `snapshot().encode()` 字节比较**（快照是确定性编码），
不靠“看起来没变”。

## 2. 覆盖率（目标 ≥ 85%）

`cargo llvm-cov --summary-only`（llvm-tools 25.x + cargo-llvm-cov 0.9.1）：

| 文件 | 行覆盖 | 区域覆盖 |
|---|---|---|
| algo/autosort.rs | 100.00% | 99.76% |
| algo/nesting.rs | 96.49% | 95.80% |
| core/catalog.rs | 83.78% | 90.78% |
| core/container.rs | 96.36% | 95.83% |
| core/error.rs | 100.00% | 100.00% |
| core/grid.rs | 96.37% | 96.61% |
| core/ids.rs | 100.00% | 100.00% |
| core/inventory.rs | 94.25% | 91.80% |
| core/item.rs | 100.00% | 100.00% |
| core/rules.rs | 100.00% | 100.00% |
| core/snapshot.rs | 100.00% | 100.00% |
| ffi.rs | 90.64% | 87.24% |
| lib.rs | 100.00% | 100.00% |
| **TOTAL** | **94.21%** | **92.77%** |

结论：**94.21% 行覆盖，达标**（目标 85%）。最低的是 `core/catalog.rs`（83.78%，主要是
`iter()` / `is_empty()` 这类查询辅助函数未走到）与 `ffi.rs`（90.64%，防御性错误分支）。

## 3. Criterion 基准（实施计划阶段五验收）

`cargo bench --bench can_place`（release，lto）——中位数与 95% 置信区间：

| 基准 | 中位数 | 95% 区间 | 说明 |
|---|---|---|---|
| `can_place/backpack_10x10_90pct` | **5.16 ns** | 4.80 – 5.90 ns | **计划验收场景**：100 格背包 90% 满载，随机抽查 |
| `can_place/mask_8x8_90pct` | 4.40 ns | 4.35 – 4.45 ns | 真 u64 位图路径（64 格阈值内） |
| `can_place/scan_10x10_90pct` | 2.68 ns | 2.59 – 2.81 ns | 同一状态、强制逐格扫描（参考路径） |
| `can_place/scan_12x12_90pct` | 4.82 ns | 4.77 – 4.86 ns | 144 格（位图阈值外） |
| `insert_remove/10x10` | 68.5 ns | 65.9 – 72.1 ns | 落位 + 摘除往返 |
| `manager/try_move_cross_container` | 59.5 ns | 59.0 – 60.1 ns | 带规则校验的完整移动 |
| `manager/autosort_10x10` | 11.4 µs | 11.31 – 11.52 µs | 一键整理（58 件物品） |
| `snapshot/encode_10x10_90pct` | 1.43 µs | 1.41 – 1.44 µs | 917 字节编码 |

**验收判定：达标** —— 要求的 ≤ 500 ns，实测 5.16 ns（约 100 倍余量；
release 门禁测试 `tests/perf.rs` 用另一种方法学——分批复用取中位数——实测 1.3 ns/次）。

诚实的口径说明（两点）：

1. **100 格走的是逐格扫描路径**。位图阈值 `MASK_MAX_CELLS = 64`（见 `core/grid.rs`），
   10×10 = 100 格超出阈值，`can_place` 走回退路径——而它本身就是 **2.7 ns**：网格是平铺数组，
   顺序内存读 + 短路判断足够快。计划里“≤ 8×8 用 u64 位图”的建议已按原样实现
   （8×8 = 4.4 ns），**没有必要为省下 2 ns 把位图扩到 u128**（收益为零，复杂度翻倍）。
2. Criterion 的 HTML 汇总在 `target/criterion/report/index.html`；
   “火焰图”需要 pprof 采样，本项目未接入（criterion 提供的是统计报告，不是火焰图）。

## 4. 快照尺寸（目标 ≤ 2 KiB）

| 场景 | 物品 | 容器 | 编码后 |
|---|---|---|---|
| 典型单玩家装载（4 根容器 + 3 层套包 + 堆叠 + 旋转） | 37 件 | 6 个 | **755 字节** |
| 满载压力场景（10×10 背包 90% 占用） | 58 件 | 4 个 | **917 字节** |

均 ≤ 2048 字节，`tests/snapshot.rs::*` 有硬断言。恢复（restore）用同一份目录重建并在
临时实例上跑完整自检（归属链、可达性、深度上限），损坏快照返回 `CorruptSnapshot` 且不动现有数据。

## 5. 负重系统（追加交付）

需求：给玩家加“总承重上限 + 负重比”，供 UE 侧套速度 / 体力曲线。范围：**全局上限**
（不含每个容器各自的承重上限）。

新增接口：

| 层 | 接口 |
|---|---|
| 规则 | `Rules::max_capacity_kg`、`with_max_capacity(kg)`、`has_capacity_limit()` |
| 查询 | `Inventory::total_weight()`、`weight_ratio()`、`is_overloaded()` |
| FFI | `uerust_inventory_set_max_capacity` / `_weight_ratio` / `_is_overloaded`（共 33 个导出符号） |
| UE 可见性 | `Inventory Self Test` 报告新增“负重”段（总重 / 上限 / 比例 / 超重判定），已通过 dylib 实测 |

语义决定：

- 默认**无限制**（`f32::INFINITY`），不改变既有玩法与既有测试的行为；
- 传 `0` / 负数 / NaN / ±inf 一律视为**取消上限**（0 ≠ “一点都不能带”）；
- `weight_ratio()` 是比例，超重时 **> 1.0**（例 `1.25` = 超重 25%），核心不做速度惩罚，
  惩罚曲线属于游戏侧；
- 计重口径与 `aggregates().weight` 一致：单件重量 × 堆叠数量，**套包本体与内部内容都计入**。

关键性质（有测试固化）：**总重只在生成物品、销毁物品、恢复快照时变化**；
移动 / 旋转 / 互换 / 拆分 / 合并 / 一键整理**都不改变总重**
（`tests/weight.rs::geometry_ops_do_not_change_weight`）。将来若要给总重加缓存，
只需在这 3 个入口失效，不需要在每个操作里埋点。

明确不做的部分（避免默默膨胀范围）：每个容器/每件包的承重上限（需要在
`add_item` / `try_move` / `try_split` / `try_swap` 四条路径加校验并补测试，约半天）；
UE 侧速度/体力曲线（核心只给 ratio）；C++ 蓝图包装（目前插件没有持有
`Inventory*` 句柄的 UObject 层，要做完整 UI 时应先补这一层）。

## 6. 测试期间发现并修复的缺陷

**位图分支漏检两物品互换重叠（高严重度，已修复）**

- 场景：网格格数 ≤ 64（走 `u64` 位图）时，`can_place_pair` 检查“A 的新落点、B 的新落点”
  是否互相重叠。原实现把 B 新落点的掩码并进 `bits` 后与 `occ` 做按位与，而 `occ` 已把 A、B
  的当前占用剔除 —— 于是**两个新落点之间的重叠恒检测不到**（逐格扫描分支是对的）。
- 影响：同网格 `try_swap` 在 debug 下触发断言 panic，release 下会静默写入互相重叠的两个落位
  （破坏网格不变式）。
- 定位：`tests/grid_edges.rs::pair_swap_off_by_one_overlap_masked_grid`
  （最小复现：4×4 网格、两个 2×2 只差一格）。
- 修复：位图分支把“另一个新落点”单独判交（不再借助剔除后的 `occ`），见
  `src/core/grid.rs::rect_blocked`。
- 防回归：新增 `can_place_pair_matches_simulation` 单元测试——用“克隆 → 移除两物品 →
  依次落位”的模拟结果，与 `can_place_pair` 在 4×4/8×8（位图）与 9×9/10×10（扫描）上
  逐组合比对，**两条路径语义完全一致**。

其他记录（非缺陷，属口径澄清）：

- `uerust_last_error()` 永远返回非 NULL 指针（指向线程本地缓冲），判断“有无错误”要看内容
  （空串 = 无错误）——`tests/ffi_smoke.rs` 已按内容校验。
- `restore` 现在会对重建结果跑完整不变式自检（含嵌套深度 ≤ 当前规则上限、容器可达性），
  不满足即 `CorruptSnapshot` 且不落地。

## 7. UE5（C++）侧验证

- **编译**：`Build.sh gamedemoEditor Mac Development`（UE 5.8）→ **Succeeded**（30 s，
  4 个编译动作 + 链接 `libUnrealEditor-ue_package_system.dylib`）。
- **符号**：`nm -gU Binaries/Mac/libue_package_system_dylib.dylib` → **33 个 `uerust_*` 导出符号**
  （29 个背包 API + 4 个库级入口）。
- **dlopen 冒烟**（`tools/ffi_smoke/ffi_smoke.c`，`./tools/ffi_smoke/run.sh` 一键跑；
  模拟 UE 的 `GetDllHandle` + `GetDllExport` 路径）：
  加载真实 dylib → 自检报告 **30 项全过**（含套包深度、成环拦截、黑名单、同网格互换、
  负重超重判定、快照确定性回放）→ 手动驱动 define/add/rotate 验证错误码 4、
  `last_error` 文本与负重接口（0.4kg 物品 / 上限 0.2kg → 比例 2.00、判定超重）。
- **蓝图节点**：`UInvInventory` 句柄对象（`Create Inventory` / `Is Valid Handle`，
  GC 回收时自动把 Rust 实例交回释放）+ 29 个 `Uue_package_systemLibrary` 静态节点
  （自检、目录/容器/物品、原子交互、查询、负重、快照），共 31 个节点。
- **ABI 对齐**：`Fue_package_systemFfi` 含全部 33 个函数指针，类型由 cbindgen 头的
  `decltype(&uerust_*)` 推导；加载时缺任何一个即整体判定不可用（拒绝半残运行）。
- **强制重编译验证**：`touch` 两个新增 .cpp 后重跑 `Build.sh` →
  `[1/3] Compile ue_package_systemInventory.cpp`、`[2/3] Compile ue_package_systemLibrary.cpp`、
  `[3/3] Link libUnrealEditor-ue_package_system.dylib` → **Result: Succeeded**。
- **友好层（DataAsset + 组件）**：新增 5 个文件（`ue_package_systemItemDefinition.h/.cpp`、
  `ue_package_systemTypes.h`、`ue_package_systemComponent.h/.cpp`），把内容定义交给
  `UInvItemDefinition`（图标 / 名称 / 数值，可子类化）、实例操作交给
  `UInvInventoryComponent`、UI 读取交给 `FInvItemView`。
- **无头行为验证**（在真实 UE 5.8 编辑器命令行里跑，不是单元测试）：
  `./tools/ue_headless/run.sh` → **六套共 475 项检查全部通过**
  （友好层 84 + 套包展开 9 + 演示 Actor 46 + 内容管线 59 + UMG 界面 111 + 界面交互 166；覆盖初始化幂等、
  DefId 顺序与数组顺序无关、落位 / 移动 / 旋转 / 越界零副作用、套包嵌套与顶层过滤、
  套包展开（`FInvItemView.OwnContainer` 直连内部容器）、负重与超重、存读档往返、
  目录不匹配被拒、未就绪与野句柄错误路径、字段夹取与运行时补注册、演示摘要内容与幂等、
  JSON 导入端到端与注册顺序兼容性、界面坐标换算 / 落点夹取 / 拖拽状态机、
  自动落位 / 拆分一半 / 右键菜单可用性过滤与事件上报）。
  期间抓到并修复 2 个缺陷：初始化就绪闸门顺序错（配置的定义一个都没注册上）、
  尺寸填 0 时 `Footprint` 与注册值不一致。
  另补 `FInvItemView.OwnContainer`（套包展开必需）并复跑通过。
- **零配置演示 Actor**：`AInvInventoryDemoActor`（`ue_package_systemDemoActor.h/.cpp`）——
  自建 5 件内置定义与四个根容器、灌一套装载、按间隔把「网格 + 清单 + 负重」打到屏幕与日志；
  不依赖任何美术资产。`touch` 强制重编译 → `[1/2] Compile ue_package_systemDemoActor.cpp` /
  `[2/2] Link` → **Succeeded**；46 项无头检查覆盖摘要内容（件数 7 / 总重 5.380 kg /
  总估值 2510 / 负重比 0.179 / 背包网格 5 行 × 6 列）、幂等与未就绪保护。
- **内容管线（DataTable / JSON）**：新增 `FInvItemDefinitionRow`（`ue_package_systemItemTable.h/.cpp`）
  + 组件 `ItemDefinitionTable` 配置 + `Find Definition By Name` / `Get All Definitions` 两个节点。
  注册顺序：资产按名升序在前（与旧版一致，老存档不受影响）→ 表行按 RowName 升序在后。
  样例 `tools/samples/ItemTable.sample.json`（6 行）经 `AssetImportTask` **真实导入**验证
  （再导出逐字段对账全等）；演示表资产 `/ue_package_system/Demo/DT_InventoryItems`。
  59 项无头检查覆盖导入端到端、表单源 DefId 顺序、字段/图标软引用透传、向后兼容、混用顺序、
  行结构错误的容错、按名查找（大小写不敏感与冲突规则）。
- **UMG 背包界面（零资产）**：新增 `UInvGridWidget`（430 行头 + 1365 行实现，自绘格子 +
  可测拖拽状态机）与 `UInvInventoryScreenWidget`（296 + 716 行，C++ 拼子控件：格子并排 +
  负重条 + 整理按钮）；`Build.cs` 补一行 **public** 依赖 `"UMG"`
  （公开头文件派生自 `UUserWidget`，而 UMG 是 Engine 的私有依赖、不传递 —— 只补 include 会链接失败）。
  演示 Actor 默认 `bCreateUI = true`，Play 即建界面。
  111 项无头检查覆盖纯函数（坐标换算 / 旋转占位 / 落点夹取）、状态机（拖入空位成功、
  占用位自动换向重试、`CancelDrag` 无残留）、控件构造（含空配置与 `TakeWidget()`）、
  负重文本 / tooltip 文本与既有四套回归。
- **界面交互补全（右键菜单 / 快捷操作 / 事件钩子）**：组件新增 `Find Free Cell`（行主序首次命中扫描）
  与 `Try Auto Place`（part 升序、先当前朝向再换向，失败零变化）；格子控件新增
  `HandleModifiedClick`（`Ctrl+左键` = 拆一半 + 自动落位）、`HandleDeleteKey`、`SplitHalfSelected`、
  `SplitHalfItem`、`IconPadding`（像素内缩，只影响图标/占位矩形）；界面控件新增
  `EInvContextAction` 枚举、右键菜单（`UBorder` + `UVerticalBox` + 5 个 `UButton`，不可用项 `Collapsed`）、
  5 个 `BlueprintAssignable` 事件（`OnItemPickedUp` / `OnItemDropped` / `OnItemUsed` /
  `OnItemDestroyed` / `OnOperationFailed`）、3 个可选音效（默认空 = 静音）与镜像字段。
  菜单可用性规则：`Rotate`→可旋转；`SplitHalf`→`MaxStack>1 && Stack>=2`；`Use`→只广播事件；
  `Destroy`→句柄有效；`AutosortContainer`→已落位。拆分 `count = Stack/2`，
  落点「当前 part → 同容器其它 part」，失败返回 `18` 且零变化。
  **166 项无头检查**覆盖 `FindFreeCell` 边界（空/碎片/旋转/满/越界）、`TryAutoPlace`、菜单过滤与
  执行、`Ctrl+点击`、`Delete`、音效留空不崩与失败事件上报；既有五套零回归。

## 8. 复现命令

```bash
cd Source/ue_package_system-dylib

cargo test                                     # 122 项测试
cargo test --release --test perf -- --nocapture # 性能门禁（≤500ns）与实测数字
cargo bench --bench can_place                  # Criterion 基准（报告在 target/criterion/report/）

CARGO_TARGET_DIR=target/cov cargo llvm-cov --summary-only \
  --ignore-filename-regex '(tests/|benches/|\.cargo/registry)'   # 覆盖率

cd ../.. && uerust build .                     # 重新构建并把 dylib 拷到 Binaries/<平台>/

./tools/ffi_smoke/run.sh                       # FFI 冒烟（不依赖 UE，验证导出符号与 C ABI）

./tools/ue_headless/run.sh                     # 无头 UE 行为验证（六套共 475 项；需要 UE 5.8）

# UE 5.8 编译（macOS）
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  gamedemoEditor Mac Development -project="/Users/ldd/Documents/UEPROJ/gamedemo/gamedemo.uproject"
```

已知边界：性能数字是 **Apple M1 Pro 单线程**实测；位图阈值 64 格是设计选择（见 §3）；
快照格式暂无版本字段（变更需两端同步）；负重的容器级上限与 UE 侧曲线未做（见 §5）。
