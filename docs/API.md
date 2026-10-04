# 战术网格背包系统 · 接口设计文档

> 适用版本：Rust 核心 `v0.2.0`（crate：`ue_package_system-dylib`）
> 错误码与 C ABI 属于**公开契约**：只增不改。

---

## 1. 分层与设计红线

```
ue_package_system-dylib/          Rust cdylib（UE 侧 dlopen 加载）
  src/core/                       数据模型 + 中心化管理器
    ids.rs        ItemKey / ContainerKey / DefId
    error.rs      InventoryError（错误码 1..=26，跨 FFI 稳定）
    item.rs       ItemDef（静态定义）/ ItemInstance（运行时实例）/ tags
    catalog.rs    Catalog（定义表，DefId = 下标）
    grid.rs       GridContainer（平铺网格 + u64 位图快路径）
    container.rs  Container（1 块 = 矩形，多块 = 复合弹挂）/ ContainerType
    rules.rs      Rules（嵌套深度上限）
    inventory.rs  Inventory（SlotMap 集中持有 + 原子操作 + 快照）
    snapshot.rs   Snapshot（紧凑 bincode 格式）
  src/algo/
    nesting.rs    成环检测 / 子树高度 / 可达容器枚举（迭代 DFS）
    autosort.rs   二维装箱（Best-Fit Decreasing，多策略）
  src/ffi.rs                      C ABI（UE C++ / 蓝图入口）
  include/ue_package_system_ffi.h cbindgen 产物（提交进版本库，供参考）
```

红线（实施计划第二节）：

- **禁止**裸指针 / `Rc<RefCell<T>>` / 跨结构体引用构建循环图；
- **必须**统一 ID 间接寻址：物品 `ItemKey`、容器 `ContainerKey`、定义 `DefId`
  （均为 slotmap 代际索引，旧 ID 永远不会误指向新对象）；
- 数据由 `Inventory` **单一持有**，位置信息只存一份（在 `GridContainer` 里）。

## 2. 概念模型

| 概念 | 类型 | 说明 |
|---|---|---|
| 物品定义 | `ItemDef` | 尺寸 `w×h`、可旋转、堆叠上限、重量、估值、标签位掩码、禁入容器掩码、可选内部容器规格 |
| 物品实例 | `ItemInstance` | 定义 ID + 堆叠数量 + 耐久 + 自有容器 ID（套包） |
| 网格 | `GridContainer` | 宽高 + 平铺 `Vec<Option<ItemKey>>` + 落位表 + 位图（≤64 格自动启用） |
| 容器 | `Container` | 1..16 块子网格（`parts`）。1 块 = 背包/安全箱；多块 = 弹挂多个口袋 |
| 根容器 | `ContainerType::{Pockets, ChestRig, SafeBox, Backpack}` | 每类最多一个；`Nested` 表示套包内部 |
| 玩家 | `Inventory` | 目录 + 物品表 + 容器表 + 根表 + 规则 |

坐标约定：`(x, y)` 为格坐标，`x ∈ [0, w)`、`y ∈ [0, h)`；物品落位由
`(x, y, rotated, base_w, base_h)` 描述，旋转后有效尺寸为 `(base_h, base_w)`。

## 3. 不变式（`Inventory::check_invariants()` 全量校验）

1. 每个物品**恰好落位一次**（网格单元格与落位表双向一致，无幽灵物品）；
2. 容器归属链自洽：根容器无归属；嵌套容器必有归属物品且反向引用一致；
3. 每类根容器最多一个，且 `roots` 登记与容器类型一致；
4. 所有容器从根可达（无孤儿、无环）；
5. 嵌套深度 ≤ `Rules::max_nesting_depth`（根 = 1 层，默认上限 4）；
6. 堆叠数量 ∈ `1..=ItemDef::max_stack`。

## 4. 核心 API（Rust）

### 4.1 目录

```rust
let id = inv.define_item(
    ItemDef::new("AK 弹匣", 1, 2, 1)      // 名称, 宽, 高, 堆叠上限
        .rotatable(true)                   // 可旋转
        .weight(0.2).value(120)            // 单件重量 / 估值
        .tags(tags::MAGAZINE)              // 标签位掩码
        .forbid(ContainerType::SafeBox.bit())  // 禁止进安全箱
)?;                                        // → DefId
```

校验规则：尺寸 `1..=32`、`weight` 有限且非负、`max_stack ∈ 1..=65535`；
声明了内部容器规格的物品 `max_stack` 必须为 1，且自动打上 `tags::CONTAINER`。

### 4.2 容器

```rust
let pockets = inv.add_root(ContainerType::Pockets, "口袋", &[(1,1),(1,1),(1,2),(1,2)])?;  // 复合
let backpack = inv.add_root(ContainerType::Backpack, "背包", &[(6,5)])?;                  // 矩形
```

每类根容器只允许一个（重复 → `AlreadyExists`）；套包/弹挂在物品落位时**自动**创建，
通过 `inv.item(key)?.container` 拿句柄。

### 4.3 网格几何（`GridContainer`）

```rust
g.can_place(x, y, w, h) -> bool                       // 越界 + 重叠（位图路径 O(1)）
g.can_place_excluding(x, y, w, h, &[item]) -> bool    // 忽略指定物品（同网格挪位用）
g.can_place_slow(x, y, w, h, &[item]) -> bool         // 逐格扫描参考实现（基准对比）
g.can_place_pair(a, &meta_a, b, &meta_b) -> bool      // 两物品互换落点定案
g.place(key, meta)? / g.remove(key)? -> PlacedMeta    // 落位 / 摘除
g.rotate(key)?                                        // 原地旋转（越界或碰撞 → RotationBlocked）
g.occupants() / g.slot_of(key) / g.cells() / g.occupancy_mask()
```

### 4.4 交互操作（`Inventory`，全部原子）

| 操作 | 签名 | 语义 |
|---|---|---|
| 落位生成 | `add_item(def, stack, durability, dst, part, x, y, rotated) -> ItemKey` | 校验后生成 + 落位；套包同时建内部容器 |
| 移动 | `try_move(item, dst, part, x, y, rotated) -> ()` | 跨容器 / 容器内挪位 |
| 互换 | `try_swap(a, b) -> ()` | 原子互换；各自优先保持朝向，放不下再翻转 |
| 旋转 | `try_rotate(item) -> ()` | 原地旋转 |
| 合并 | `try_merge(src, dst) -> u32` | 同定义堆叠合并，返回实际转移量；`src` 空了自动销毁 |
| 拆分 | `try_split(item, count, dst, part, x, y, rotated) -> ItemKey` | `count ∈ 1..item.stack` |
| 整理 | `autosort(container) -> ()` | 沙盒重排 + 整体提交；失败 → `SortFailed`，原布局不动 |
| 删除 | `destroy_item(item) -> u32` | 连同套包内的全部内容，返回件数 |

**原子性实现**：所有操作分两阶段 —— 先只读校验（越界 / 重叠 / 成环 / 深度 /
黑名单 / 堆叠上限），全部通过后走“不可能失败”的提交路径（`place_validated`）。
校验不过直接返回错误，**一个字节都不改**；不存在需要回滚的中间态。

### 4.5 统计与查询

```rust
inv.aggregates() -> Aggregates { weight: f64, value: u64, item_count: u32 }  // 含套包内部
inv.total_weight() -> f64                             // = aggregates().weight（kg）
inv.weight_ratio() -> f32                             // 总重 ÷ 承重上限；无上限 → 0.0；超重 > 1.0
inv.is_overloaded() -> bool                           // 负重比 > 1.0（无上限时恒 false）
inv.find_by_tag(tags::MAGAZINE) -> Vec<ItemKey>       // 递归命中套包内部
inv.items_in(container) / inv.all_items() / inv.all_containers()             // 确定性顺序
inv.container_of_item(item) -> Option<(ContainerKey, u8, PlacedMeta)>
inv.root_type_of(c) / inv.depth_of(c)                 // 根类型（黑名单判定）/ 嵌套深度
```

### 4.6 规则

```rust
inv.set_rules(Rules::default().with_max_depth(4));        // 嵌套深度 1..=16，默认 4
inv.set_rules(Rules::default().with_max_capacity(30.0));  // 承重上限 kg；0/NaN/±inf = 无限制（默认）
Rules::default().has_capacity_limit()                     // 是否设了上限
```

负重语义：`weight_ratio()` 是**比例**（不是剩余可携带量）；超重时大于 1.0
（例：`1.25` = 超重 25%），游戏侧直接拿它套速度 / 体力曲线即可。
总重只在生成物品、销毁物品、恢复快照时变化 —— 移动 / 旋转 / 互换 / 合并 / 拆分 / 整理
都不改变总重（有测试固化这条性质）。

黑名单是**按根类型**判定的：把物品放进“安全箱里的包”，根类型仍是安全箱 → 同样被拒。

## 5. 错误码表（跨 FFI 稳定）

| 码 | 变体 | 触发条件 |
|---|---|---|
| 0 | （成功） | — |
| 1 | `OutOfBounds` | 目标位置越界 |
| 2 | `Occupied` | 目标位置与已有物品重叠 |
| 3 | `RotationBlocked` | 旋转后越界 / 碰撞 |
| 4 | `NotRotatable` | 定义不可旋转 |
| 5 | `NotPlaced` | 物品当前未落位 |
| 6 | `ItemNotFound` | 物品句柄失效 |
| 7 | `ContainerNotFound` | 容器句柄失效 |
| 8 | `PartOutOfRange` | 子网格下标越界 |
| 9 | `UnknownDef` | 定义 ID 不存在 |
| 10 | `InvalidDef` | 定义 / 容器尺寸参数非法 |
| 11 | `AlreadyPlaced` | 同一物品重复落位 |
| 12 | `AlreadyExists` | 槽位已存在（如根容器重复创建） |
| 13 | `WouldCycle` | 嵌套成环（套包放进自己的子树） |
| 14 | `NestingTooDeep` | 超过嵌套深度上限 |
| 15 | `ForbiddenContainer` | 命中根容器黑名单 |
| 16 | `StackUnmergeable` | 定义不同 / 不可堆叠 |
| 17 | `StackFull` | 目标堆叠已满 |
| 18 | `InvalidStackCount` | 数量 0 / 超上限 / 拆分越界 |
| 19 | `SortFailed` | 自动整理无法不丢物品重排（原布局保持） |
| 20 | `CorruptSnapshot` | 快照损坏 / 与目录不匹配 |
| 21 | `CorruptState` | 内部状态自检失败（出现即 bug） |
| 22 | `InvalidArgument` | 参数组合非法（如 swap 同一物品） |
| 23 | `InvalidHandle` | FFI 句柄为空 |
| 24 | `InvalidUtf8` | C 字符串非 UTF-8 |
| 25 | `BufferTooSmall` | 输出缓冲区不足 |
| 26 | `Panic` | Rust 侧 panic 被 FFI 边界捕获 |

## 6. 自动整理（`autosort`）

1. 提取容器全部顶层物品，按**面积降序**（兜底策略：长边降序）排序；
2. Best-Fit Decreasing 贪心：枚举所有子网格的候选位置 × 两种朝向，
   按“贴合度”（接触墙 / 已有物品的单位边数）选最优，平局按 `(y, x, part, 不旋转)` 决胜；
3. 三种策略依次尝试，全部失败 → `SortFailed`；
4. 方案先在一套**全新沙盒网格**里完整重排并通过 `check()` 自检 + 数量守恒检查，
   全部通过才整体替换（`replace_parts`），原子提交。

确定性：同分物品的最终决胜键是 `(key, def)`，任意调用顺序结果一致（可做快照回归）。

## 7. 序列化（快照 / 网络包）

- 格式：`bincode` 1.3（定长字段，无字段名开销）；
- 键不入包：容器按确定性顺序编号（根固定顺序 + 子树前序 DFS），物品用 `u16` 下标互引；
- 尺寸 / 可旋转性从目录推导，只存 `rotated`；堆叠 `u32`、耐久 `u16`；
- **快照不含目录**：物品定义属于内容数据，收发双方必须使用同一份定义表；
- 目标：典型单玩家装载（30~40 件物品 + 8 个容器）≤ 2 KiB（实测见测试报告）；
- `restore` 先在临时状态完整重建并校验（定义存在、尺寸合法、位置不重叠、归属完整），
  全部通过才整体替换 —— 损坏快照不会破坏现有数据。

```rust
let bytes: Vec<u8> = inv.snapshot().encode()?;   // 存档 / 网络发送
let snap = Snapshot::decode(&bytes)?;
inv.restore(&snap)?;                             // 幂等：restore 后再导出字节一致
```

限制：当前快照格式**没有版本字段**，后续若变更字段布局，必须两端同步升级。

## 8. C ABI 参考（UE 侧）

头文件：`include/ue_package_system_ffi.h`（cbindgen 由 `cargo build` 自动生成）。

### 8.1 通用约定

- 返回值 `i32`：`0` 成功；`> 0` 为错误码（见第 5 节）。
- 返回值 `i64`：`>= 0` 为结果（DefId / 句柄 / 数量）；`< 0` 为 `-错误码`。
  句柄是 slotmap key 的 u64 编码，活跃句柄永不为 0（`0` 当“无”的哨兵）。
- 数组输出**两段式**：先 `out = NULL` 或 `cap = 0` 拿所需数量，再按数量传缓冲区；
  缓冲区不足返回 `-25`（`BufferTooSmall`）。
- 内存：`*mut c_char` 用 `uerust_free_string` 释放；`*mut u8` 用 `uerust_free_bytes(ptr, len)`
  释放（`len` 必须与取出时一致）。禁止在 C++ 侧 `free` / `delete`。
- 错误详情：`uerust_last_error()` 返回线程本地的可读文本（不需要释放）。
- 线程模型：句柄按**单线程**使用；跨线程共享需外部加锁。
- 所有入口 `catch_unwind`：Rust panic 不会穿过 FFI（返回错误码 `26`）。

### 8.2 函数清单

库级：

| 函数 | 说明 |
|---|---|
| `uerust_plugin_version() -> const char*` | 版本字符串（静态，不需释放） |
| `uerust_last_error() -> const char*` | 最近一次失败原因（线程本地） |
| `uerust_free_string(char*)` | 释放本库返回的字符串 |
| `uerust_free_bytes(uint8_t*, uint32_t)` | 释放快照字节缓冲区 |
| `uerust_inventory_selftest() -> char*` | 跑内置端到端场景，返回逐项报告（蓝图节点用） |

生命周期：

| 函数 | 说明 |
|---|---|
| `uerust_inventory_new() -> Inventory*` | 创建实例（NULL = 失败） |
| `uerust_inventory_free(Inventory*)` | 销毁实例 |

目录 / 容器 / 物品：

| 函数 | 说明 |
|---|---|
| `uerust_inventory_define_item(inv, name, width, height, rotatable, max_stack, weight, value, tag_bits, forbidden_container_types, container_dims, container_part_count) -> i64` | 注册定义，返回 `DefId`；`container_dims` 每 2 个 u8 一组 `(w,h)` |
| `uerust_inventory_add_root(inv, ctype, label, dims, part_count) -> i64` | 建根容器（0=口袋 1=弹挂 2=安全箱 3=背包），返回句柄 |
| `uerust_inventory_add_item(inv, def, stack, durability, dst, part, x, y, rotated) -> i64` | 生成并落位，返回物品句柄 |
| `uerust_inventory_destroy_item(inv, item) -> i64` | 删除子树，返回件数 |

交互：

| 函数 | 说明 |
|---|---|
| `uerust_inventory_try_move(inv, item, dst, part, x, y, rotated) -> i32` | 移动 |
| `uerust_inventory_try_swap(inv, a, b) -> i32` | 原子互换 |
| `uerust_inventory_try_rotate(inv, item) -> i32` | 旋转 |
| `uerust_inventory_try_merge(inv, src, dst, out_moved) -> i32` | 合并（`out_moved` 收实际数量） |
| `uerust_inventory_try_split(inv, item, count, dst, part, x, y, rotated) -> i64` | 拆分，返回新句柄 |
| `uerust_inventory_autosort(inv, container) -> i32` | 一键整理 |

查询：

| 函数 | 说明 |
|---|---|
| `uerust_inventory_item_info(inv, item, FfiItemInfo*) -> i32` | 定义 / 堆叠 / 耐久 / 位置 / 朝向 / 自有容器 |
| `uerust_inventory_container_info(inv, container, FfiContainerInfo*) -> i32` | 类型 / part 数 / 总格数与已占格数（按格计） |
| `uerust_inventory_container_part_size(inv, container, part, out_w, out_h) -> i32` | 子网格尺寸 |
| `uerust_inventory_container_label(inv, container) -> char*` | 显示名（需释放） |
| `uerust_inventory_container_grid(inv, container, part, out_cells, cap) -> i64` | 占格表（行主序，0=空，否则物品句柄）——UI 渲染用 |
| `uerust_inventory_item_location(inv, item, out_container, out_part) -> i32` | 物品在哪 |
| `uerust_inventory_total_weight(inv, double*) -> i32` | 总负重 |
| `uerust_inventory_total_value(inv, uint64_t*) -> i32` | 总估值 |
| `uerust_inventory_set_max_capacity(inv, float kg) -> i32` | 设置总承重上限；`<=0` / NaN / ±inf = 取消上限 |
| `uerust_inventory_weight_ratio(inv, float*) -> i32` | 负重比（无上限写 0.0；超重 > 1.0） |
| `uerust_inventory_is_overloaded(inv) -> i64` | `1` = 超重，`0` = 正常，`< 0` = `-错误码` |
| `uerust_inventory_find_by_tag(inv, tag_mask, out_items, cap) -> i64` | 标签查询 |
| `uerust_inventory_containers(inv, out, cap) -> i64` | 全部容器（根在前） |
| `uerust_inventory_items(inv, out, cap) -> i64` | 全部物品 |

序列化：

| 函数 | 说明 |
|---|---|
| `uerust_inventory_snapshot(inv, uint8_t** out_ptr, uint32_t* out_len) -> i32` | 导出 bincode 快照（用 `uerust_free_bytes` 释放） |
| `uerust_inventory_restore(inv, uint8_t* bytes, uint32_t len) -> i32` | 从快照恢复（目录不变；损坏不动现有数据） |

### 8.3 C++ 调用示例

```cpp
const Fue_package_systemFfi& Ffi = Fue_package_systemModule::GetFfi();
if (!Ffi.IsValid()) { /* 跑 `uerust build .` 后重启编辑器 */ }

// 自检报告
char* Report = Ffi.SelfTest();
if (Report) { FString Text = UTF8_TO_TCHAR(Report); Ffi.FreeString(Report); }

// 句柄 + 典型数据流：注册定义 → 建根容器 → 放物品 → 查占格表渲染
Inventory* Inv = Ffi.InventoryNew();                        // 蓝图里 = Create Inventory
int64 Def  = Ffi.DefineItem(Inv, "AK 弹匣", 1, 2, /*rotatable*/1, /*max_stack*/1,
                            0.2f, 120, /*tag_bits*/1, /*forbid*/0, nullptr, 0);
const uint8 Dims[2] = {6, 5};
int64 Bag  = Ffi.AddRoot(Inv, /*Backpack*/3, "背包", Dims, 1);
int64 Item = Ffi.AddItem(Inv, (uint32)Def, 1, 0, (uint64)Bag, 0, 0, 0, 0);
int32 Code = Ffi.TryRotate(Inv, (uint64)Item);              // 0 = 成功；4 = 不可旋转
Ffi.InventoryFree(Inv);                                     // 蓝图里由 GC 的 BeginDestroy 自动调
```

### 8.4 蓝图 / C++ 接入

整套 API 已包成蓝图节点（`Uue_package_systemLibrary`，全部同步、无网络依赖）：

**句柄对象**：`Create Inventory` 返回 `UInvInventory` —— 内部持有 Rust 的 `Inventory*`，
GC 回收时在 `BeginDestroy` 里自动释放。蓝图里把它存进变量即可；句柄只在**单线程**内使用。

| 分组 | 节点 |
|---|---|
| 自检 | `Inventory Self Test`、`Rust Core Version`、`Last Error` |
| 目录/容器/物品 | `Define Item`、`Add Root Container`、`Add Item`、`Destroy Item` |
| 原子交互 | `Try Move`、`Try Swap`、`Try Rotate`、`Try Merge`（出参 OutMoved）、`Try Split`、`Autosort` |
| 查询 | `Get Item Info`、`Get Container Info`、`Get Container Part Size`、`Get Container Label`、`Get Container Grid`、`Get Item Location`、`Get Total Weight`、`Get Total Value`、`Find Items By Tag`、`Get Containers`、`Get Items` |
| 负重 | `Set Max Capacity`、`Get Weight Ratio`、`Is Overloaded` |
| 存档 | `Snapshot`、`Restore`（bincode 字节串） |

返回约定与 C ABI 一致：句柄类 `int64`（`<0` = `-错误码`）、操作类 `int32`（`0` 成功 / `>0` 错误码）、
查询类 `bool` + 出参；错误细节用 `Last Error`。传入无效句柄时句柄类返回 `-23`、操作类返回 `23`。

最小蓝图数据流：

```
Create Inventory
  → Define Item（弹匣，1×2，可旋转，MaxStack 1）        → DefId
  → Add Root Container（Backpack，Parts=[(6,5)]）      → 背包句柄
  → Add Item（DefId, Stack 1, Container=背包, 0,0,0）  → 物品句柄
  → Try Rotate / Try Move / Try Swap / Autosort        → 0 = 成功
  → Get Container Grid（行主序，0 = 空格）              → 喂给 UI 画格子
存档：Snapshot → 字节串；Restore ← 字节串。负重条：Get Weight Ratio / Is Overloaded。
```

C++ 侧（不走蓝图）：`Fue_package_systemFfi` 已含全部 33 个函数指针，类型由 cbindgen 头的
`decltype(&uerust_*)` 推导 —— Rust 侧改签名会在 C++ **编译期**报错，不用手抄 typedef。

### 8.5 友好层（推荐用法）

低层 ABI 之上有一层给使用者用的封装（`UInvInventory` 那层是给高级用途的）：

| 层 | 类 | 说明 |
|---|---|---|
| 内容定义 | `UInvItemDefinition : UDataAsset` | 编辑器里填：显示名 / 描述 / 图标 / 宽高 / 可旋转 / 堆叠 / 重量 / 估值 / 标签 / 禁入容器 / 内部网格。**扩展点就是子类化它**（如 `UMyWeaponDefinition` 加伤害/射速） |
| 宿主 | `UInvInventoryComponent : UActorComponent` | 一个组件 = 一个背包；挂 PlayerState / Pawn。配置 `ItemDefinitions`（内容表）+ `RootContainers`（口袋/弹挂/安全箱/背包）+ `MaxCapacityKg`；`BeginPlay` 自动初始化 |
| 视图 | `FInvItemView` | 实例状态 + 定义拼好的只读结构（Handle / Definition / Stack / Durability / OwnContainer / HostContainer / HostPart / X / Y / bRotated），UI 直接遍历 |

组件节点（32 个，Category = 背包系统）：目录（`Register Definition` / `Get Definition` / `Get Def Id` /
`Get Item Definition` / `Find Definition By Name` / `Get All Definitions`）、
生命周期（`Initialize Inventory`）、容器（`Get Container` / `Get Container Parts` /
`Get Container Label` / `Get Container Grid` / `Get Container Info` / `Autosort Container`）、
物品（`Give Item` / `Get Item View` / `Get Container Items View` / `Move Item` / `Swap Items` /
`Rotate Item` / `Split Stack` / `Merge Stacks` / `Destroy Item`）、负重（`Get Total Weight` /
`Get Total Value` / `Get Weight Ratio` / `Is Overloaded` / `Set Max Capacity`）、
查询（`Find Items By Definition` / `Get All Containers` / `Get All Items`）、
存档（`Save To Bytes` / `Load From Bytes`）。

关键约定：

- **DefId 顺序**：注册顺序与 `ItemDefinitions` 数组顺序**无关**（先按资产名排序再注册）——
  设计师重排数组不会撕存档。运行时补注册（`Register Definition` / `Give Item` 自动补）追加到末尾，
  其顺序只对“本次运行”确定，要存档就得保证读档时用同样的顺序重建。
- **越界字段自动夹取**（宽高 1~32、MaxStack 1~65535、带容器规格强制 MaxStack=1、负重量归零）：
  一次失败的 `DefineItem` 不占号、会让后面的 DefId 整体前移（等于撕存档），所以必须在这里挡住。
- **读档**：`Load From Bytes` 前必须已注册同一批定义（`BeginPlay` 自动做）；恢复后容器 / 物品
  句柄**全部重排**，旧句柄作废，要重新 `Get Container` / `Get All Items` 取一遍。
- 组件未就绪（`bReady == false`）时：句柄类返回 `-23`、操作类返回 `23`、查询类返回 `false` / 空，且打中文日志。

典型流程：

```
（编辑器）建 UInvItemDefinition 资产（DA_AKMag / DA_9x19 …）→ 拖进组件的 ItemDefinitions
          配 RootContainers：Pockets [(1,1)×4]、Backpack [(6,5)]、SafeBox [(4,3)]
（运行时）BeginPlay 自动初始化 → Get Container(Backpack) → Give Item(DA_AKMag, …)
          → Get Container Items View(Backpack) 渲染（Definition->Icon / DisplayName / Footprint）
          → Move Item / Swap Items / Rotate Item / Split Stack / Autosort Container
（存读档）Save To Bytes → 写进 SaveGame；Load From Bytes ← 读回
```

**内容来源（DefId 的分配顺序 = 存档兼容的关键）**：

1. `ItemDefinitions` 资产：按资产名升序（同名按对象路径）——与旧版本完全一致；
2. `ItemDefinitionTable`（DataTable，行结构 `FInvItemDefinitionRow`）：按 **RowName 升序**，排在资产之后。

两种来源都配就是“资产在前、表行在后”；**选定一种并保持稳定**。表行注册时逐行
`NewObject<UInvItemDefinition>`（行名 = 对象名），靠 `NamedDefinitions` 强引用防 GC。
逻辑名索引同时收录表行 RowName 与资产对象名，冲突时先注册者胜并打警告。

样例数据：`tools/samples/ItemTable.sample.json`（6 行，字段格式已用真实导入验证）；
插件里另存了一张演示表 `/ue_package_system/Demo/DT_InventoryItems`。

**零配置演示**：`AInvInventoryDemoActor`（Place Actors 搜「背包演示 Actor」）——
拖进任意关卡按 Play，就会自建 5 件内置定义与四个根容器、灌一套装载，
并按间隔把「网格图 + 物品清单 + 负重汇总」打到屏幕和日志上；不需要任何美术资产。
节点：`Initialize Demo Inventory`（幂等，返回摘要文本）。演示用，正式项目可直接删。

### 8.6 UMG 界面（零资产）

两个纯 C++ 控件（`UCLASS(BlueprintType)`，蓝图面板 **Create Widget** 直接用；不需要 Widget Blueprint / 字体 / 贴图）：

| 控件 | 说明 |
|---|---|
| `Grid Widget`（`UInvGridWidget`） | 单个容器子网格。`ExposeOnSpawn`：`Inventory` / `Container` / `Part` / `CellSize`。**自绘**（NativePaint）：棋盘底纹、物品图标（软引用同步加载并按定义缓存，缺图标画底色 + 名字首字）、`Stack > 1` 堆叠角标、半透明拖拽幽灵、绿/红落点高亮、鼠标旁 tooltip（名字 / 描述 / 尺寸 / 重量 / 估值 / 堆叠） |
| `Inv Inventory Screen Widget`（`UInvInventoryScreenWidget`） | 一屏背包：按 `ShowTypes` 为每个存在的根容器自动建格子（多 part 并排 + 小标题）、负重条（比例 + 超重变红）、「整理」按钮（对当前格子所在容器 `AutosortContainer`，无选中退回背包） |

交互：左键按下 = 选中 + 开始拖；移动 = 吸附格子 + 幽灵跟随；松开 = 落位
（目标占位恰好只有一件别的物品时自动改 `SwapItems`；落位失败且物品可旋转时**自动换向重试一次**，
结果可读 `bAutoRotatedOnLastDrop`）；`R` = 旋转（拖拽中翻幽灵 / 选中时原地旋转）；`Esc` = 取消。
跨容器拖动：源格子把屏幕坐标转播给界面 → 界面找光标下那一格 → 共用同一套落位策略。

可测接口（无头测试直接驱动，不依赖鼠标）：

- 纯函数：`LocalPositionToCell` / `FootprintCells` / `CellToLocalPosition` / `ComputeDropTarget`（越界夹取）；
- 状态机：`BeginDragAtItem` / `UpdateDragCursor` / `DropOnThisGrid` / `RotateDraggedOrSelected` / `CancelDrag`；
- 查询与刷新：`Refresh` / `SetContainer` / `GetItemViewInGrid` / `GetGridTitle` / `BuildTooltipText` /
  `GetWeightText` / `GetWeightRatio` / `GetWeightBarPercent` / `AutosortFocusedContainer`。

演示 Actor 默认 `bCreateUI = true`：Play 时自动建这屏界面并 `AddToViewport`
（没有 PlayerController 时只打一条 Info 日志跳过，不报错）。

**交互补全**（右键菜单 / 快捷操作 / 事件钩子）：

- 组件扩展：`Find Free Cell`（`FindFreeCell(Container, Part, Size, bRotated, OutCell)` —— 扫描子网格找
  第一个能放下的位置）与 `Try Auto Place`（`TryAutoPlace(Item, Container)` —— 自动落位，先按当前朝向、
  放不下且可旋转再试换向）；
- 右键菜单（界面控件）：`RequestContextMenuForItem(Item, ScreenPos)` / `CloseContextMenu()` /
  `IsContextMenuVisible()` / `GetContextMenuItemHandle()` / `GetAvailableContextActions(Item)`
  （返回 `EInvContextAction { Rotate, SplitHalf, Use, Destroy, AutosortContainer }` 的可用子集）/
  `InvokeContextAction(Action)`；
- 快捷操作（格子控件）：`HandleModifiedClick(Item, bCtrl)`（`Ctrl` = 拆一半 + 自动落位）、
  `HandleDeleteKey()`、`SplitHalfSelected()`；
- 事件（界面控件，`BlueprintAssignable`）：`OnItemPickedUp` / `OnItemDropped(Item, ResultCode)` /
  `OnItemUsed` / `OnItemDestroyed` / `OnOperationFailed(Code, Message)`；音效位
  `PickUpSound` / `DropSound` / `ErrorSound`（默认空 = 静音）+ `bPlaySounds`；
- 图标微调：格子控件 `IconPadding`（像素内缩）。

语义：拆分一半 = `Stack / 2`（`Stack >= 2` 才可用），落点用 `Find Free Cell`（先本 part 再同容器其它 part），
失败时零副作用返回错误码；「使用」只广播事件（玩法效果由游戏侧实现）；「删除」走 `DestroyItem`。
所有失败都会经 `OnOperationFailed` 带错误码与中文文本。

## 9. 性能指标与验收

| 指标 | 目标 | 说明 |
|---|---|---|
| `can_place`（10×10，90% 满载） | ≤ 500 ns | u64 位图路径，见 `benches/can_place.rs` |
| 逐格扫描参考路径 | 同步报告 | 大网格（> 64 格）走此路径 |
| 单玩家快照 | ≤ 2 KiB | 30~40 件典型装载，`tests/snapshot.rs` 断言 |
| 失败操作 | 0 副作用 | `tests/atomic_moves.rs` / `tests/properties.rs` 用快照字节比较 |

复现：`cargo bench --bench can_place`；`cargo test`；`cargo test --release --test perf -- --nocapture`。

## 10. 兼容性承诺

- 错误码数值只增不改（新增错误从 27 起）；
- C ABI 函数只增不改签名；新增参数请加新函数；
- 快照格式：目前无版本字段，变更需要两端同步（排期内的改进项）。
