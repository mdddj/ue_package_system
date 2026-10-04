#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ue_package_systemInventory.h"
#include "ue_package_systemItemDefinition.h"
#include "ue_package_systemTypes.h"

#include "ue_package_systemComponent.generated.h"

class UDataTable;

/**
 * 背包组件：**一个组件 = 一个背包**。
 *
 * 挂在 PlayerState / Pawn 上，负责四件事：
 * 1. 建 Rust 背包实例（`UInvInventory`）并管它的生命周期；
 * 2. 把 `ItemDefinitions` / `ItemDefinitionTable` 里的内容按**确定性顺序**注册成 DefId；
 * 3. 按 `RootContainers` 建根容器（口袋 / 弹挂 / 安全箱 / 背包）；
 * 4. 把实例操作包成蓝图节点——内部全部走低层 `Uue_package_systemLibrary` 的静态函数，
 *    不直接碰 `Fue_package_systemFfi`，ABI 保持单点。
 *
 * ---------------------------------------------------------------------------
 * 注册顺序（`BeginPlay` 调 `InitializeInventory` 时执行）= DefId = 存档的键。
 * 顺序**与数组顺序无关、与“配置来源”无关**，两种来源都配时固定是：
 *
 *   1. `ItemDefinitions` 的资产：按资产名（大小写敏感）升序，同名再按对象路径决胜；
 *   2. `ItemDefinitionTable` 的表行：按 RowName 升序（`FNameLexicalLess`，跨进程稳定）。
 *
 * 也就是说：**资产在前、表行在后**。只配一种来源时，那种来源的排序结果就是全部 DefId。
 * 想换来源、或者往已经存档的项目里加第二种来源，DefId 会整体错位（老存档读不出来）——
 * **选定一种来源并长期保持稳定**。
 * ---------------------------------------------------------------------------
 *
 * 低层（`UInvInventory` / `Uue_package_systemLibrary` / `Fue_package_systemFfi`）没有改动，
 * 这一层只做组合。
 *
 * 返回约定沿用低层：句柄类 `int64`（`>= 0` 成功，`< 0` = `-错误码`，`0` = “无”）、
 * 操作类 `int32`（`0` 成功 / `> 0` 错误码）、查询类 `bool` + 出参。
 * 组件**没就绪**（`BeginPlay` 还没跑，或建实例失败）时：句柄类返回 `-23`、操作类返回 `23`、
 * 查询类返回 `false` / 空数组 / 默认值，并且打中文警告日志。
 *
 * 蓝图最小用法：把组件挂到 PlayerState 上 → 填 ItemDefinitions 与 RootContainers →
 * BeginPlay 之后 `Is Ready` 为真 → `Get Container(Backpack)` 拿句柄 → `Get Container Items View` 渲染。
 */
UCLASS(ClassGroup = (Backpack), meta = (BlueprintSpawnableComponent, DisplayName = "背包组件"), BlueprintType)
class UE_PACKAGE_SYSTEM_API UInvInventoryComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    // ==================== 生命周期 ====================

    /**
     * 建实例 → 注册定义 → 建根容器 → 设承重上限。
     * 建实例失败只置 `bReady = false` 并打中文日志，不崩。
     */
    virtual void BeginPlay() override;

    /** 清空引用。底下的 Rust 实例由 `UInvInventory` 在 GC 回收时释放，这里不主动销毁。 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /**
     * 手动初始化（**幂等**，重复调用直接返回）。BeginPlay 会调它；
     * 运行时 `NewObject` 出来的组件、自动化测试也可以自己调。
     * 返回 `true` 表示背包实例可用（`RootContainers` 为空之类的配置问题只打警告，不算失败）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Initialize Inventory"))
    bool InitializeInventory();

    // ==================== 编辑器配置 ====================

    /**
     * 内容表：BeginPlay 时全部注册。
     * 注册顺序**与数组顺序无关**（按资产名排序，见 `RegisterDefinition`），所以随便拖。
     *
     * 与 `ItemDefinitionTable` 二选一即可；两个都配就是“资产在前、表行在后”（见类注释）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "背包|内容")
    TArray<TObjectPtr<UInvItemDefinition>> ItemDefinitions;

    /**
     * 物品内容表（DataTable）：一张表定义全部物品，支持 CSV / JSON 批量导入，不用手工拖资产。
     *
     * **RowStruct 必须是 `FInvItemDefinitionRow`**（行结构选错时打中文 Error 并跳过整张表，
     * `ItemDefinitions` 那一份照常注册，不崩）。表里的每一行在注册时现场变成一件
     * `UInvItemDefinition`（行名 = 对象名 = 物品逻辑名），存进 `NamedDefinitions` 防 GC。
     *
     * 注册顺序：表行按 **RowName 升序**排在 `ItemDefinitions` 的资产之后（见类注释）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "背包|内容")
    TObjectPtr<UDataTable> ItemDefinitionTable;

    /**
     * 启动时自动创建的根容器（口袋 / 弹挂 / 安全箱 / 背包，每种最多一个）。
     * 留空则不建任何根容器，并且打中文警告日志。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "背包|内容")
    TArray<FInvRootContainerSetup> RootContainers;

    /** 总承重上限（kg）。`0` = 不限制（`Is Overloaded` 恒为 false）。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "背包|负重")
    float MaxCapacityKg = 0.f;

    // ==================== 运行时状态 ====================

    /** 底下的 Rust 背包实例；未就绪时为空。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包|状态")
    TObjectPtr<UInvInventory> Inventory;

    /**
     * 背包实例是否可用：建实例成功后立刻置 true（定义注册 / 建根容器在它之后跑，
     * 个别定义注册失败会单独打错误日志，不把整个组件判死）。
     */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包|状态")
    bool bReady = false;

    // ==================== DefId 映射 ====================

    /**
     * DefId -> 定义。DefId 是存档的键：注册顺序必须**确定**，否则同一个存档
     * 在不同机器 / 不同次启动上会解释成不同的物品。
     * 策略：先按资产名（大小写敏感）升序，同名再按对象路径决胜；运行时补注册则追加到末尾。
     */
    UPROPERTY(Transient)
    TMap<int64, TObjectPtr<UInvItemDefinition>> DefinitionById;

    /** 定义 -> DefId（反向表，查重与 UI 反查用）。 */
    UPROPERTY(Transient)
    TMap<TObjectPtr<UInvItemDefinition>, int64> IdByDefinition;

    /**
     * 逻辑名 -> 定义（`FindDefinitionByName` 用）。
     *
     * 收录两种来源的键：**表行的 RowName**、**资产的对象名（`GetName()`）**。
     * 键冲突（比如某行名和某个资产名撞了）时**以先注册的为准**并打警告——先注册的号段在前，
     * 让它赢才能保证“按名取到的定义”和“存档里的 DefId”指向同一件物品。
     * 键是 `FName`：比较**大小写不敏感**（`ak_mag` 和 `AK_Mag` 算同一个键），与 RowName 一致。
     *
     * UPROPERTY 强引用：表行现场造出来的定义没有别的地方引用它们，不这么挂着会被 GC 收走。
     * 只收录配置来源（`ItemDefinitions` / `ItemDefinitionTable`）；运行时自己 `RegisterDefinition`
     * 塞进来的定义不进这张表（那些是代码里的临时对象，没有稳定逻辑名）。
     */
    UPROPERTY(Transient)
    TMap<FName, TObjectPtr<UInvItemDefinition>> NamedDefinitions;

    /**
     * 注册一个物品定义，返回 DefId（`< 0` = `-错误码`）。
     *
     * - 已经注册过：直接返回缓存的 DefId（幂等，不会重复占用号段）；
     * - 新定义：追加到当前目录末尾（`DefineItem` 的返回值就是它的号）；
     * - `nullptr`：返回 `-22`（参数非法）并打警告。
     *
     * 越界的字段会被夹到 Rust 接受的范围并打警告（宽高夹到 1~32、MaxStack 夹到 1~65535、
     * 带容器规格的物品强制 MaxStack = 1、负重量归零），这样设计师填错不会顶掉后面的 DefId。
     *
     * **注意**：运行时补注册的号只对“本次运行”确定；要存档，就得保证读档时用同样的调用顺序
     * 重新注册（配置驱动的部分由 BeginPlay 自动保证）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Register Definition"))
    int64 RegisterDefinition(UInvItemDefinition* Definition);

    /** DefId -> 定义；没注册过返回空。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Definition"))
    UInvItemDefinition* GetDefinition(int32 DefId) const;

    /** 定义 -> DefId；没注册过返回 `-9`（UnknownDef），传空返回 `-22`。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Def Id"))
    int64 GetDefId(UInvItemDefinition* Definition) const;

    /** 物品句柄 -> 定义（UI 常用）；句柄无效返回空。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Item Definition"))
    UInvItemDefinition* GetItemDefinition(int64 ItemHandle) const;

    /**
     * 按逻辑名取定义：`FindDefinitionByName("AK_Mag")`。
     *
     * 逻辑名 = 内容表行的 RowName，或内容资产的对象名。游戏逻辑写死名字就行，
     * 不用记 DefId，也不用在蓝图里满世界拖资产引用。
     * 没这个名字返回空并打中文警告（找不到是配置问题，别让它静默变成"物品不显示"）。
     *
     * 查名字走 `FName` 的全等比较，和 RowName / TMap 一致：**大小写不敏感**
     * （`"ak_mag"` 命中 `"AK_Mag"`）。要让名字真正区分大小写，得改用 FString 键。
     */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Find Definition By Name"))
    UInvItemDefinition* FindDefinitionByName(FName Name) const;

    /**
     * 全部已注册的定义，**按 DefId 升序**（= 存档里的顺序，UI 列表 / 调试打印用）。
     * 注册失败的（`RegisterDefinition` 返回负数的）不在里面。
     */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get All Definitions"))
    TArray<UInvItemDefinition*> GetAllDefinitions() const;

    // ==================== 容器 ====================

    /** 按类型取根容器句柄；没有这种根容器返回 `0`（“无”，不是错误码）。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Container"))
    int64 GetContainer(EInvContainerType Type) const;

    /** 容器的全部子网格尺寸，每项 `(宽, 高)`；失败返回空数组。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Container Parts"))
    TArray<FIntPoint> GetContainerParts(int64 Container) const;

    /** 容器显示名；失败返回空串。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Container Label"))
    FString GetContainerLabel(int64 Container) const;

    /**
     * 某块子网格的占格表（行主序，长度 = 宽 * 高）：
     * `0` = 空格，否则是该格上压着的物品句柄。UI 画格子用。失败返回空数组。
     */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Container Grid"))
    TArray<int64> GetContainerGrid(int64 Container, int32 Part) const;

    /** 容器汇总（类型 / 子网格数 / 是否根容器 / 总格数 / 已占格数）。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Container Info"))
    bool GetContainerInfo(int64 Container, FInvContainerInfo& OutInfo) const;

    /** 一键整理。返回 `0` 或错误码（`19` = 排不下，原布局保持不动）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Autosort Container"))
    int32 AutosortContainer(int64 Container);

    // ==================== 物品 ====================

    /**
     * 生成一件物品并落到 `(Container, Part, X, Y)`，返回物品句柄（`< 0` = `-错误码`）。
     * 定义还没注册的话会就地注册（等价于先调一次 `RegisterDefinition`）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Give Item"))
    int64 GiveItem(UInvItemDefinition* Definition, int64 Container, int32 Part, int32 X, int32 Y,
        bool bRotated, int32 Stack = 1);

    /** 取物品视图（实例状态 + 定义）。句柄无效返回 `false` 并打警告。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Item View"))
    bool GetItemView(int64 ItemHandle, FInvItemView& OutView) const;

    /** 容器内**顶层**物品的视图列表（套包里的东西不算，UI 直接用这个渲染）。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Container Items View"))
    TArray<FInvItemView> GetContainerItemsView(int64 Container) const;

    /** 移动物品。失败时状态零变化。返回 `0` 或错误码。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Move Item"))
    int32 MoveItem(int64 Item, int64 Dst, int32 Part, int32 X, int32 Y, bool bRotated);

    /** 两件物品原子互换（可跨容器）。返回 `0` 或错误码。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Swap Items"))
    int32 SwapItems(int64 A, int64 B);

    /** 原地旋转 90 度。返回 `0` 或错误码（`4` = 该定义不可旋转）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Rotate Item"))
    int32 RotateItem(int64 Item);

    /**
     * 拆分堆叠：从 `Item` 拆 `Count` 件放到 `(Dst, Part, X, Y)`。
     * 返回 `0` 或错误码——**新物品句柄拿不到**，拆完用 `GetContainerItemsView` 重新取列表即可
     * （拖拽 UI 本来就要刷新）。想直接拿新句柄就用低层 `Try Split` 节点。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Split Stack"))
    int32 SplitStack(int64 Item, int32 Count, int64 Dst, int32 Part, int32 X, int32 Y, bool bRotated);

    /** 把 `Src` 合并进 `Dst`。返回 `0` 或错误码（`16` 定义不同 / `17` 目标已满）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Merge Stacks"))
    int32 MergeStacks(int64 Src, int64 Dst);

    /** 删除物品及其套包内的全部内容，返回被删除的件数（`< 0` = `-错误码`）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Destroy Item"))
    int64 DestroyItem(int64 Item);

    // ==================== 自动落位 ====================

    /**
     * 在容器的一块子网格里找**第一个**放得下 `Size` 的空位（行主序扫描，左上角优先）。
     *
     * - `Size` 读作未旋转的 `(宽, 高)`；`bRotated` 为真时实际占位是 `(高, 宽)`（与格子控件同一套约定）；
     * - 命中时 `OutCell` = 物品左上角的格坐标，返回 `true`；
     * - 「放不下」一律返回 `false` 且 `OutCell = (0,0)`：`Size` 有一边 `<= 0` / 比子网格还大、
     *   容器句柄或子网格下标无效、碎片化导致哪里都排不下。
     *
     * 只看占格表（`0` = 空格，否则是压在那格上的物品句柄）：**不排除物品自己**——
     * 要挪的那件东西自己占的格同样算障碍，`TryAutoPlace` 正是按这个口径用的。
     */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Find Free Cell"))
    bool FindFreeCell(int64 Container, int32 Part, FIntPoint Size, bool bRotated, FIntPoint& OutCell) const;

    /**
     * 把**已经落位的**物品自动挪到 `Container` 里的第一个空位（同一容器内换格、跨容器都可以）。
     *
     * 扫描顺序：子网格下标升序；每块子网格里先试**当前朝向**，放不下且定义可旋转时再试换向；
     * 首个「找得到空位且 `MoveItem` 成功」的位置就落下去，返回 `0`。
     *
     * 返回：`0` 成功；`2`（Occupied）= 所有子网格两个朝向都放不下（全满 / 碎片化）；
     * `5`（NotPlaced）= 物品还没落位（先把它放进某个容器再用）；
     * `22`（InvalidArgument）= 物品句柄非法 / 取不到定义；`23`（InvalidHandle）= 组件没就绪或容器无效；
     * 找到空位但 `MoveItem` 被拒时返回 `MoveItem` 自己的错误码（如 `15` 黑名单）。
     * **失败时物品位置零变化**（`MoveItem` 本身是原子的）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Try Auto Place"))
    int32 TryAutoPlace(int64 Item, int64 Container);

    // ==================== 负重 ====================

    /** 总重量（kg）。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Total Weight"))
    float GetTotalWeight() const;

    /** 总估值。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Total Value"))
    int64 GetTotalValue() const;

    /** 负重比（总重 ÷ 上限）。无上限时返回 `0.0`，超重时 > `1.0`。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get Weight Ratio"))
    float GetWeightRatio() const;

    /** 是否超重。无上限时恒为 `false`。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Is Overloaded"))
    bool IsOverloaded() const;

    /** 设置总承重上限（kg）。`<= 0` = 取消上限。同时更新 `MaxCapacityKg`，方便存读档。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Set Max Capacity"))
    void SetMaxCapacity(float Kg);

    // ==================== 查询 ====================

    /** 找出全部使用某个定义的物品（含套包内部的东西）。定义没注册过返回空数组。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Find Items By Definition"))
    TArray<FInvItemView> FindItemsByDefinition(UInvItemDefinition* Definition) const;

    /** 全部容器句柄（根在前，顺序确定）。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get All Containers"))
    TArray<int64> GetAllContainers() const;

    /** 全部物品句柄。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Get All Items"))
    TArray<int64> GetAllItems() const;

    // ==================== 存档 ====================

    /** 导出快照字节（bincode）。失败返回 `false` 并清空 `OutBytes`。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Save To Bytes"))
    bool SaveToBytes(TArray<uint8>& OutBytes) const;

    /**
     * 从快照字节恢复。失败返回 `false`，现有数据不受影响。
     *
     * **读档前必须已经用与存档时完全相同的顺序注册好同一批 `ItemDefinitions`**，
     * 否则低层返回错误码 `20`（CorruptSnapshot：快照与目录不匹配）——
     * DefId 是注册顺序号，快照里存的就是它，顺序对不上就解释不成同一件物品。
     * BeginPlay 会自动按确定性顺序注册，正常流程不用担心；自己重建背包、或者
     * 改了 ItemDefinitions 内容之后读老存档，就会踩到这里。
     *
     * 恢复后容器 / 物品句柄**全部变了**（快照按确定性顺序重新编号），
     * 调用方缓存的旧句柄一律作废，要重新 `GetContainer` / `GetAllItems` 取一遍。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Load From Bytes"))
    bool LoadFromBytes(const TArray<uint8>& Bytes);

private:
    // 错误码数值与 Rust 侧 InventoryError 一致（见 docs/API.md 第 5 节）。
    // 放成类成员而不是文件作用域的常量，是为了在 unity build 里不和低层的同名常量撞车。

    /** 句柄无效。 */
    static constexpr int32 ErrInvalidHandle = 23;

    /** 参数非法。 */
    static constexpr int32 ErrInvalidArgument = 22;

    /** 目标位置放不下（全满 / 被占）。 */
    static constexpr int32 ErrOccupied = 2;

    /** 物品当前未落位。 */
    static constexpr int32 ErrNotPlaced = 5;

    /** 定义没注册过。 */
    static constexpr int32 ErrUnknownDef = 9;

    /** Rust 侧 max_stack 的上限（u16），超了 catalog 判 InvalidDef。 */
    static constexpr int32 MaxStackLimit = 65535;

    /** 日志用：组件所属 Actor 的名字（多背包场景下要分清是哪一个）。 */
    static FString DescribeOwner(const UActorComponent* Component);

    /** 日志用：定义名（空定义给个可读的占位）。 */
    static FString DescribeDefinition(const UInvItemDefinition* Definition);

    /** 取低层背包实例；没就绪就打中文警告并返回空。 */
    UInvInventory* GetReadyInventory() const;

    /**
     * 按确定性顺序注册全部配置来源：先 `ItemDefinitions` 的资产（按资产名升序，同名按对象路径），
     * 再 `ItemDefinitionTable` 的表行（按 RowName 升序）。只排本地副本，不动设计师填的数组。
     */
    void RegisterConfiguredDefinitions();

    /**
     * 注册一件**配置来源**的定义：先挂进 `NamedDefinitions`（强引用，行造出来的对象靠它防 GC），
     * 再 `RegisterDefinition`；注册失败就把刚挂的名字摘掉（不会摘别人同名冲突赢下的那条）。
     */
    void RegisterConfiguredDefinition(UInvItemDefinition* Definition, FName LogicalName);

    /**
     * 注册 `ItemDefinitionTable` 的每一行：逐行现场 `NewObject<UInvItemDefinition>`（行名当对象名）
     * → 抄字段 → 注册。表没配 / 行结构不符 / 表是空的都只打中文日志并返回，
     * **不影响** `ItemDefinitions` 那一份（两个来源互不拖累）。
     */
    void RegisterConfiguredDefinitionRows();

    /**
     * 往 `NamedDefinitions` 挂一个逻辑名；键已存在时**保留先注册的**并打警告（不覆盖）。
     * `Name` 为空或定义为空时什么都不做。
     */
    void IndexDefinitionByName(FName Name, UInvItemDefinition* Definition);

    /** 按 `RootContainers` 建根容器；空数组只打警告。 */
    void CreateConfiguredRootContainers();

    /**
     * 占格表里 `Cell` 起、占 `Cells` 格的那块矩形是不是全空。
     * 越界（调用方该先夹好）直接算不空；`Occupancy` 长度不足也由调用方先校验。
     */
    static bool IsAreaFree(const TArray<int64>& Occupancy, FIntPoint PartSize, FIntPoint Cell, FIntPoint Cells);

    /** 用实例状态 + 定义拼一个视图。 */
    FInvItemView BuildView(int64 ItemHandle, const FInvItemInfo& Info) const;

    /**
     * 把子网格尺寸夹到 Rust 接受的范围（每边 1~32、最多 16 块），越界改动打警告。
     * 空数组原样返回（普通物品 / 没有子网格的容器语义）。
     */
    static TArray<FIntPoint> SanitizeParts(const TArray<FIntPoint>& Parts, const FString& Context);
};
