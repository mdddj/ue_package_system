# ue_package_system · 战术网格背包系统

《三角洲行动》/《逃离塔科夫》风格的网格化背包系统：**Rust 写核心逻辑，UE5 通过 C ABI 调用**。
由 `uerust` 生成的插件骨架改造而来（原来的 markdown 抓取演示已整体移除）。

## 能力

- 2D 网格碰撞与旋转（`R` 键那套），核心放置校验走 u64 位图，百纳秒级；
- 多容器：口袋 / 弹挂（复合容器，多口袋）/ 安全箱 / 背包；
- 负重：总重（含套包内部）/ 承重上限 / 负重比与超重判定，UE 侧直接套速度与体力曲线；
- 套包与嵌套：成环检测（DFS）、嵌套深度上限、按根类型的容器黑名单（大金不进安全箱）；
- 原子操作：移动 / 互换 / 旋转 / 堆叠合并 / 拆分，**失败零副作用**；
- 一键自动整理（Best-Fit Decreasing 装箱，沙盒重排后整体提交）；
- 紧凑序列化：bincode 快照，典型单玩家装载 ≤ 2 KiB；
- UE 接入：句柄对象（`Create Inventory`）+ 整套蓝图节点（放置 / 互换 / 整理 / 查询 /
  负重 / 快照），C++ 也可直调全量 C ABI；
- UE 友好层：`UInvItemDefinition`（DataAsset：图标 / 名称 / 数值，**子类化即扩展**）、
  `UInvInventoryComponent`（挂 PlayerState 即用，自动注册定义 / 建容器 / 设承重）、
  `FInvItemView`（UI 视图结构体）；
- 全链路测试：单元 + 集成 + proptest + criterion 基准（见 `docs/TEST_REPORT.md`）。

接口说明见 **`docs/API.md`**（Rust API + C ABI + 错误码表 + 性能指标）。

## 目录

```
ue_package_system/
  ue_package_system.uplugin
  docs/API.md                        接口设计文档（交付物）
  docs/TEST_REPORT.md                测试与基准报告（交付物）
  Source/
    ue_package_system/               C++ 侧（桥接层 + 友好层）
      ue_package_system.Build.cs
      Public/ue_package_system.h                 模块：dlopen + 全量函数表（decltype 对齐 ABI）
      Public/ue_package_systemInventory.h        低层句柄对象 UInvInventory + 枚举/结构体
      Public/ue_package_systemLibrary.h          低层蓝图函数库（29 个节点）
      Public/ue_package_systemItemDefinition.h   物品定义资产（DataAsset，可子类化扩展）
      Public/ue_package_systemTypes.h            视图结构体 FInvItemView / 根容器配置
      Public/ue_package_systemComponent.h        背包组件（友好层，30 个节点）
      Private/…                                  实现
    ue_package_system-dylib/         Rust 侧（背包核心）
      Cargo.toml / build.rs           cbindgen 生成 include/ue_package_system_ffi.h
      src/core/                       数据模型 + Inventory + 快照
      src/algo/                       成环分析 + 自动整理
      src/ffi.rs                      C ABI
      tests/ benches/                 集成测试 / 基准
  Binaries/<Platform>/               uerust build 把库拷到这里
```

## 接进 UE 项目

**最简单**：把 `ue_package_system/` 整个拷进 `<项目>/Plugins/` 下，UE 自动发现。

**要显式启用**：在 `MyGame.uproject` 的 `"Plugins"` 数组里加一个对象：

```json
"Plugins": [
    { "Name": "ue_package_system", "Enabled": true }
]
```

插件放在项目目录之外时，再加 `AdditionalPluginDirectories`（路径相对 `.uproject`）。

## 构建与验证

1. 构建 Rust 库（会跑 `cargo build --release` 并把产物拷到 `Binaries/<Platform>/`）：

   ```
   uerust build .
   ```

   **跳过这步 `Binaries/<Platform>/` 是空的，插件启动时加载库会失败。**

2. 重新生成项目文件并编译 C++ 模块（macOS：Xcode/UE 5.8；Windows：VS）。

3. 编辑器里「编辑 > 插件」确认已启用，然后蓝图搜 **背包系统** 分类：

   - 先跑 **Inventory Self Test**：返回逐项自检报告（容器 / 放置 / 旋转 / 互换 / 堆叠 /
     套包 / 成环拦截 / 黑名单 / 负重 / 整理 / 快照），能打出报告说明
     Rust ⇄ C ABI ⇄ 蓝图 整条链路通了；
   - 正式用法（推荐）：给 PlayerState 加 **背包组件**（`InvInventoryComponent`）→
     把物品定义资产拖进 `ItemDefinitions`、配好 `RootContainers`（口袋 / 弹挂 / 安全箱 / 背包）→
     `BeginPlay` 后 `Is Ready` 为真 → `Get Container` / `Give Item` /
     `Get Container Items View`（UI 渲染）/ `Move Item` / `Autosort Container`；
     存档 `Save To Bytes` / 读档 `Load From Bytes`；
   - 低层节点（`Create Inventory` + `Uue_package_systemLibrary` 的 29 个函数）也保留，
     需要直接控句柄时用。

   完整节点清单与返回约定见 `docs/API.md` §8.3。

4. 只想验证 Rust 侧：

   ```
   cd Source/ue_package_system-dylib
   cargo test                                        # 单元 + 集成测试
   cargo bench --bench can_place                     # 基准（can_place ≤ 500ns 验收）
   cargo test --release --test perf -- --nocapture   # 性能门禁 + 实测数字
   ```

5. 只想验证 dylib 的导出与 C ABI（不依赖 UE，任何机器可跑）：

   ```
   ./tools/ffi_smoke/run.sh
   ```

## 5 分钟看到效果（零配置演示）

项目里**不需要任何美术资产**：

1. File → New Level → Basic，随便存一个关卡；
2. Place Actors 面板里搜 **背包演示 Actor**，拖进关卡；
3. 按 Play —— 屏幕左上角（以及 Output Log）会打出背包网格图 + 物品清单 + 负重汇总。

演示 Actor 自带一套内置物品定义（弹药 / 弹匣 / 急救包 / 手雷 / 突击背包）和四个根容器
（口袋 / 弹挂 / 安全箱 / 背包），所以拖进去就能跑。想看它每次刷新的格式，把
`RefreshIntervalSeconds` 调小即可。

正式项目把演示配置换成你自己的：物品定义用 `UInvItemDefinition` 资产（可子类化扩字段）、
背包组件挂到 PlayerState/Pawn、按 `docs/API.md` §8.5 接 UI。**演示 Actor 是示例，可以随时删掉。**

## 内容表（DataTable / JSON 批量导入）

物品多了以后别再手工拖资产：用 **DataTable** 批量定义。

1. Content Browser → 右键 → Miscellaneous → Data Table → 行结构选 `Inv Item Definition Row`（`FInvItemDefinitionRow`）；
2. 右键该资产 → Reimport / Import → 选 CSV 或 JSON。样例文件：`tools/samples/ItemTable.sample.json`
   （字段与物品定义资产一一对应；`Name` 就是物品的逻辑名，**改名 = 改 DefId = 撕存档，慎改**）；
3. 把这张表填到背包组件的 **`ItemDefinitionTable`** 上 —— `BeginPlay` 会自动把全部行注册成定义；
4. 游戏逻辑按名字取定义：**Find Definition By Name**（行名或资产名都能命中），
   或用 **Get All Definitions** 拿全部（按 DefId 升序）。

注册顺序（决定 DefId，也就决定存档兼容）：**先 `ItemDefinitions` 资产（按资产名升序），再表行（按 RowName 升序）**。
只配资产的老项目顺序不变；两种来源都配也可以，但**选定一种并保持稳定**。

## 背包界面（UMG，零资产）

插件自带两个纯 C++ 控件，**不需要 Widget Blueprint、字体或贴图**（图标缺失时画占位）：

| 控件 | 用途 |
|---|---|
| `Grid Widget`（`UInvGridWidget`） | 单个容器子网格：格子底纹、物品图标、堆叠角标、拖拽（自动吸附格子）、`R` 旋转、tooltip；蓝图 `Create Widget` 后设 `Inventory` / `Container` / `Part` 即可 |
| `Inv Inventory Screen Widget`（`UInvInventoryScreenWidget`） | 把四个根容器拼成一屏 + 负重条 + 「整理」按钮；同样 `Create Widget` 后设 `Inventory` |

最快的看法：**背包演示 Actor 默认就会建这屏界面**（`bCreateUI = true`）——拖进关卡 Play 即可。

布局（默认 **居中模式**）：**背包是主面板放在屏幕正中**，弹挂 / 安全箱 / 口袋自动排到左 / 右 / 下方
（哪个存在放哪个；只存在背包时就是纯居中放大）；单格默认 **64 像素**，容器标题 / 堆叠角标 / tooltip /
负重文本的字号都随格子缩放；每块容器有半透明深色圆角底板 + 描边，标题栏带容器名，主面板标题栏还有
负重条与「整理」按钮。整屏超过屏幕 85% 时会**等比缩小**（下限 24 像素），分辨率变化后自动重算。

想回到旧布局（左上角起点 + 手动格子大小）：把 `bCenterOnViewport` 勾掉，再调 `Origin` / `CellSize`。
布局解算也能在蓝图里直接调：`Compute Screen Layout`（纯函数）。
自己做界面时：

```
Create Widget (Inv Inventory Screen Widget) → 设 Inventory = 你的背包组件 → Add to Viewport
```
或只用一个格子控件嵌进你自己的 UI（把它当普通 UMG 控件用）。

界面上能直接做的操作：

| 操作 | 行为 |
|---|---|
| 右键物品 | 弹菜单：旋转 / 拆分一半 / 使用 / 删除 / 整理该容器（不可用条目自动隐藏） |
| `Ctrl` + 左键点击 | 拆一半，自动落到同容器第一个空格 |
| `Delete` | 删除选中物品 |
| `R` / `Esc` | 旋转 / 取消拖拽 |

游戏侧接音效与特效：界面控件上有 `OnItemPickedUp / OnItemDropped / OnItemUsed / OnItemDestroyed /
OnOperationFailed` 五个蓝图事件；也可以直接填 `PickUpSound / DropSound / ErrorSound`（**默认空 = 静音**）。

## 改 Rust 代码时

改完 `src/**` 再跑一次 `uerust build .`。三步都要注意：

- 新增 FFI 函数：`#[no_mangle] pub extern "C"`，参数/返回值只能是 C 类型，
  然后按 **`docs/API.md` 第 8 节**的约定补 C++ 侧函数指针；
- 返回 `*mut c_char` / `*mut u8` 的函数必须配 `uerust_free_string` /
  `uerust_free_bytes`，C++ 侧用完立刻释放，否则泄漏；
- `include/ue_package_system_ffi.h` 是 cbindgen 产物，**建议提交进版本库**
  （C++ 编译不依赖 cargo，但它是接口速查表）。

## 设计取舍（沿用 uerust 骨架）

- **不静态链接 Rust 库**：全部走 `GetDllHandle` + `GetDllExport`，
  Windows 不需要 `.dll.lib`，三平台一份逻辑；
- **`crate-type = ["cdylib", "rlib"]`**：cdylib 是对外 ABI，rlib 让 `tests/`、`benches/` 能链接本 crate；
- **原子性靠“验证先行”**：校验阶段只读、提交阶段不可能失败，天然零副作用，
  不需要快照回滚这种重武器；
- **网格 ≤ 64 格启用 u64 位图**：放置校验退化为一次按位与；
  大网格退化为逐格扫描（仍是纯内存顺序读）。
