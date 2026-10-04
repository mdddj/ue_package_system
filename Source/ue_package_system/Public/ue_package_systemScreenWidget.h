#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ue_package_systemComponent.h"
#include "ue_package_systemGridWidget.h"
#include "ue_package_systemTypes.h"

#include "ue_package_systemScreenWidget.generated.h"

class UBorder;
class UButton;
class UCanvasPanel;
class UProgressBar;
class USoundBase;
class UTextBlock;
class UVerticalBox;

/** 「容器句柄 + 子网格下标」对：界面用来判断格子拓扑有没有变（要补建 / 隐藏哪些子格子）。 */
USTRUCT(BlueprintType)
struct FInvGridSlotKey
{
    GENERATED_BODY()

    /** 容器句柄（`Get Container` 的返回值）。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包界面")
    int64 Container = 0;

    /** 子网格下标。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包界面")
    int32 Part = 0;

    bool operator==(const FInvGridSlotKey& Other) const
    {
        return Container == Other.Container && Part == Other.Part;
    }
};

/**
 * 右键菜单里的动作。**枚举顺序 = 菜单里按钮的顺序**，也是 `GetAvailableContextActions` 的返回顺序。
 */
UENUM(BlueprintType)
enum class EInvContextAction : uint8
{
    /** 原地旋转 90 度（定义 `bRotatable` 才可用）。 */
    Rotate UMETA(DisplayName = "旋转"),

    /** 拆一半并自动落到本容器的空位（`MaxStack > 1` 且当前堆叠 `>= 2` 才可用）。 */
    SplitHalf UMETA(DisplayName = "拆分一半"),

    /** 使用（定义存在就可用；背包只管广播 `OnItemUsed`，具体效果交给玩法侧）。 */
    Use UMETA(DisplayName = "使用"),

    /** 销毁这件物品（含它套包里的东西；句柄有效就可用）。 */
    Destroy UMETA(DisplayName = "销毁"),

    /** 整理这件物品**所在的容器**（已落位才可用）。 */
    AutosortContainer UMETA(DisplayName = "整理容器"),
};

/** 拿起一件物品（开始拖拽）时广播。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FInvItemPickedUpEvent, FInvItemView, Item);

/** 落位结束时广播：成功与失败都会来，`ResultCode` 是 `0` 或错误码（失败另有 `OnOperationFailed`）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FInvItemDroppedEvent, FInvItemView, Item, int32, ResultCode);

/** 「使用」动作广播（背包不实现使用效果，接这个事件去做）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FInvItemUsedEvent, FInvItemView, Item);

/** 物品被销毁后广播（视图里还带着被删物品的信息：句柄此时已经失效，只用来看内容）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FInvItemDestroyedEvent, FInvItemView, Item);

/** 任何一次操作失败时广播（拆不动 / 落不下 / 没目标 / 整理排不下…）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FInvOperationFailedEvent, int32, Code, FString, Message);

/**
 * 背包界面：把几个 `UInvGridWidget`（每个容器子网格一块）拼成一屏，外加负重条与「整理」按钮。
 *
 * 零资产：子控件（`UCanvasPanel` / `UTextBlock` / `UProgressBar` / `UButton`）全部在 C++ 里
 * `ConstructWidget` 出来，用引擎默认样式与默认字体；不需要任何 Widget Blueprint、贴图或字体资产。
 * 直接 `Create Widget`（`Class = 背包界面控件`）→ 填 `Inventory` → `Add To Viewport` 就能用。
 *
 * 自动布局：`ShowTypes` 里每个**存在**的根容器 → 它的每块子网格一块 `UInvGridWidget`
 * （同一容器的多块并排一行，行上方有小标题如 `弹挂 part2 (1x2)`），容器之间换行；
 * 左上角从 `Origin` 开始摆。
 *
 * 刷新：`Refresh()` 是幂等的全量刷新（负重条 + 标题 + 每块子网格），
 * 定时器按 `RefreshIntervalSeconds`（`<= 0` = 关掉定时，只留事件驱动）重复调它；
 * 子格子落位成功、点了「整理」之后也会自动调（事件驱动）。
 *
 * 跨容器拖（口袋 ↔ 弹挂 ↔ 背包）：鼠标被源格子捕获，所以源格子把屏幕坐标转播过来，
 * 由界面把「光标下那一块」找出来（`FindGridUnderCursor`，用 Slate 的控件几何判定），
 * 拖动中让它画预览、松开时让它接管落位。光标不在任何子格子上时返回 `false`，
 * 源格子按老路子（自己这一格）落位。
 *
 * 右键菜单：子格子右键 → `RequestContextMenuForItem` 在这里开菜单（`UBorder` + `UVerticalBox` + 5 个 `UButton`，
 * 顺序 = `EInvContextAction` 枚举顺序，不可用的按钮 `Collapsed`）；动作经 `InvokeContextAction` 执行，
 * 「拆分一半」复用子格子的 `SplitHalfItem`（拆分核心只此一份）。
 *
 * 事件：`OnItemPickedUp` / `OnItemDropped` / `OnItemUsed` / `OnItemDestroyed` / `OnOperationFailed`
 * 五个 `BlueprintAssignable` 委托，外加 `*EventCount` / `LastFailedCode` / … 镜像字段（无头测试断言用）。
 * 三个可选音效（`PickUpSound` / `DropSound` / `ErrorSound`）**默认全空 = 静音**，不依赖任何音频资产。
 *
 * 组件没绑定 / 没就绪时：文本显示「未绑定」/ 空数据，打中文警告，不崩。
 */
UCLASS(BlueprintType, meta = (DisplayName = "背包界面控件"))
class UE_PACKAGE_SYSTEM_API UInvInventoryScreenWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UInvInventoryScreenWidget(const FObjectInitializer& ObjectInitializer);

    // ==================== ExposeOnSpawn 配置 ====================

    /** 数据来源；为空时界面画空壳并打中文警告（不崩）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    TObjectPtr<UInvInventoryComponent> Inventory = nullptr;

    /** 单格边长（像素），透传给子格子。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float CellSize = 48.f;

    /** 要显示的根容器类型（顺序 = 从上到下的显示顺序）；`Nested` 会被忽略。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    TArray<EInvContainerType> ShowTypes = { EInvContainerType::Pockets, EInvContainerType::ChestRig,
        EInvContainerType::SafeBox, EInvContainerType::Backpack };

    /** 定时刷新间隔（秒）；`<= 0` = 不挂定时器（只靠事件驱动刷新）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float RefreshIntervalSeconds = 0.5f;

    /** 界面左上角在屏幕上的位置（像素）；默认放在屏幕左上偏下的位置，避开左上角的调试文字。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    FVector2D Origin = FVector2D(40.f, 140.f);

    /**
     * 拿起 / 放下 / 操作失败三个音效。**默认全空 = 全程静音**（零资产插件不要求配任何 Sound 资产）。
     * 三个都是普通 `USoundBase` 引用：留空就走静音分支，不会去加载任何东西。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面|音效", meta = (ExposeOnSpawn = "true"))
    TObjectPtr<USoundBase> PickUpSound = nullptr;

    /** 落位成功音效；失败走 `ErrorSound`。空 = 静音。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面|音效", meta = (ExposeOnSpawn = "true"))
    TObjectPtr<USoundBase> DropSound = nullptr;

    /** 失败音效（落位失败 / 拆不动 / 没目标…）。空 = 静音。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面|音效", meta = (ExposeOnSpawn = "true"))
    TObjectPtr<USoundBase> ErrorSound = nullptr;

    /** 音效总开关；关掉后三个音效一律不播（默认开，但三个都留空时等于静音）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面|音效", meta = (ExposeOnSpawn = "true"))
    bool bPlaySounds = true;

    // ==================== 运行时状态（只读） ====================

    /** 当前存在的子格子控件（与 `CaptionLabels` 同下标一一对应）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TArray<TObjectPtr<UInvGridWidget>> GridWidgets;

    /** 每块子格子上方的小标题（`弹挂 part2 (1x2)`）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TArray<TObjectPtr<UTextBlock>> CaptionLabels;

    /** 最近悬停 / 操作过的子格子；「整理」按钮按它挑容器。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UInvGridWidget> FocusedGrid = nullptr;

    /** 根画布（所有子控件都挂在它上面）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UCanvasPanel> RootCanvas = nullptr;

    /** 负重条。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UProgressBar> WeightBar = nullptr;

    /** 负重文本（`总重 5.38 kg / 上限 30 kg`）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UTextBlock> WeightLabel = nullptr;

    /** 「整理」按钮。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UButton> SortButton = nullptr;

    /** 顶部说明文字。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UTextBlock> TitleLabel = nullptr;

    // ==================== 右键菜单状态 ====================

    /** 右键菜单的底板（里面竖排按钮）；没开菜单时 `Collapsed`。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UBorder> ContextMenuBorder = nullptr;

    /** 菜单按钮的竖排容器。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UVerticalBox> ContextMenuBox = nullptr;

    /** 菜单按钮：**下标 = `EInvContextAction` 的取值**（顺序固定）；当前不可用的按钮是 `Collapsed`。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TArray<TObjectPtr<UButton>> ContextMenuButtons;

    /** 菜单是否开着。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    bool bContextMenuVisible = false;

    /** 菜单当前针对的物品句柄；`0` = 没有目标。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    int64 ContextMenuItem = 0;

    /** 菜单实际落在画布上的位置（像素，根画布本地坐标）；`CloseContextMenu` 后保留最后位置。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    FVector2D ContextMenuPosition = FVector2D::ZeroVector;

    // ==================== 事件 ====================

    /** 拿起一件物品（开始拖拽）。 */
    UPROPERTY(BlueprintAssignable, Category = "背包界面|事件")
    FInvItemPickedUpEvent OnItemPickedUp;

    /** 落位结束（成功与失败都来；失败时 `ResultCode` 非 0）。 */
    UPROPERTY(BlueprintAssignable, Category = "背包界面|事件")
    FInvItemDroppedEvent OnItemDropped;

    /** 「使用」动作。 */
    UPROPERTY(BlueprintAssignable, Category = "背包界面|事件")
    FInvItemUsedEvent OnItemUsed;

    /** 物品被销毁（右键菜单的「销毁」/ 子格子的 `Delete`）。 */
    UPROPERTY(BlueprintAssignable, Category = "背包界面|事件")
    FInvItemDestroyedEvent OnItemDestroyed;

    /** 操作失败（错误码 + 中文说明）。 */
    UPROPERTY(BlueprintAssignable, Category = "背包界面|事件")
    FInvOperationFailedEvent OnOperationFailed;

    // 事件的**镜像字段**：无头命令集里没法给动态委托绑回调（Python 侧绑不了），
    // 断言「事件到底发过没有、发的什么」就看这几个计数与最后值。

    /** `OnItemPickedUp` 累计广播次数。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int32 PickedUpEventCount = 0;

    /** `OnItemDropped` 累计广播次数。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int32 DroppedEventCount = 0;

    /** `OnItemUsed` 累计广播次数。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int32 UsedEventCount = 0;

    /** `OnItemDestroyed` 累计广播次数。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int32 DestroyedEventCount = 0;

    /** `OnOperationFailed` 累计广播次数。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int32 FailedEventCount = 0;

    /** 最近一次失败的错误码（`OnOperationFailed` 的第一个参数）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int32 LastFailedCode = 0;

    /** 最近一次失败的中文说明（`OnOperationFailed` 的第二个参数）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    FString LastFailedMessage;

    /** 最近一次「使用」的物品句柄。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int64 LastUsedItemHandle = 0;

    /** 最近一次被销毁的物品句柄（销毁前记下：销毁后这个句柄就失效了）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|事件")
    int64 LastDestroyedItemHandle = 0;

    // ==================== 刷新 ====================

    /**
     * 全量刷新：负重条 / 文本 + 每个容器的子格子（缺的补、多的隐藏）+ 布局。
     * 幂等；没构造过（还没 `TakeWidget`）时只更新文本层，不碰 Slate。不崩。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Refresh"))
    void Refresh();

    /** 换数据源（会强制重建子格子）。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Set Inventory"))
    void SetInventory(UInvInventoryComponent* InInventory);

    /**
     * 强制走一遍 Slate 构造（取 Slate 控件 → 跑构造回调 → 建子控件 → 拉数据），
     * 返回是否拿到了 Slate 控件。平时不用手动调。
     *
     * **没有 Slate 应用时**（无头命令集 / 专用服务器）会跳过 Slate 那一步并打一条中文日志、返回 `false`——
     * 那种环境下构造 Slate 控件会踩到引擎断言（`SlateApplicationBase.h` 的 `CurrentBaseApplication`）；
     * 但控件树本身（根画布 / 子格子 / 标题 / 负重条）仍然会建起来，`Refresh()` 一样能跑。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Ensure Constructed"))
    bool EnsureConstructed();

    // ==================== 负重 ====================

    /** 负重文本：`总重 X.XX kg / 上限 Y kg`（没上限时写「无限制」）。组件没绑定时给提示文本。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Weight Text"))
    FString GetWeightText() const;

    /** 负重比（总重 ÷ 上限）；没绑定或没上限返回 `0`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Weight Ratio"))
    float GetWeightRatio() const;

    /** 是否超重；没绑定返回 `false`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Is Overloaded"))
    bool IsOverloaded() const;

    /** 负重条当前填充比例（直接读条控件本身，没建出来时给 0）；测试 / 蓝图自查用。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Weight Bar Percent"))
    float GetWeightBarPercent() const;

    /** 负重条当前填充色（超重红 / 常态绿）；测试 / 蓝图自查用。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Weight Bar Color"))
    FLinearColor GetWeightBarColor() const;

    // ==================== 交互 ====================

    /**
     * 「整理」按钮的入口：对**当前格子所在的容器**调 `AutosortContainer`；
     * 没有当前格子时对**背包**（`EInvContainerType::Backpack`）调。
     * 返回 `0` 或错误码（`19` = 整理排不下；`23` = 没有可整理的容器 / 组件没就绪）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Autosort Focused Container"))
    int32 AutosortFocusedContainer();

    /** 子格子把自己的悬停 / 选中报给界面（「整理」按它找容器）。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Grid Focused"))
    void NotifyGridFocused(UInvGridWidget* Grid);

    /** 背包数据变了（子格子落位成功）：把全部子格子刷一遍。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Inventory Changed"))
    void NotifyInventoryChanged();

    /** 全部子格子控件（含被隐藏的；显示中的看 `GetVisibility`）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Grid Widgets"))
    TArray<UInvGridWidget*> GetGridWidgets() const;

    /** 最近悬停 / 操作过的子格子。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Focused Grid"))
    UInvGridWidget* GetFocusedGrid() const { return FocusedGrid; }

    // ==================== 右键菜单 ====================

    /**
     * 某件物品当前可用的动作，**按 `EInvContextAction` 的枚举顺序**返回（= 菜单里的按钮顺序）。
     *
     * 规则：旋转 → 定义存在且可旋转；拆分一半 → `MaxStack > 1` 且当前堆叠 `>= 2`；
     * 使用 → 定义存在；销毁 → 句柄有效；整理容器 → 已落位（`HostContainer > 0`）。
     * 组件没绑定 / 句柄无效 → 空数组（`GetItemView` 会打中文日志）。
     */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Available Context Actions"))
    TArray<EInvContextAction> GetAvailableContextActions(int64 ItemHandle) const;

    /**
     * 在屏幕坐标 `ScreenPosition` 处对 `Item` 开右键菜单，并**按可用性收起**不可用的按钮
     * （`Collapsed`，所以「不可堆叠」不会出现一个点了报错的「拆分一半」）。
     *
     * 句柄无效 / 组件没绑定 → 只打中文日志，不开菜单。对同一件物品重复调用 = 重新打开（幂等）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Request Context Menu For Item"))
    void RequestContextMenuForItem(int64 Item, FVector2D ScreenPosition);

    /** 收起右键菜单（目标清 0、底板 `Collapsed`）。没开过也是空操作。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Close Context Menu"))
    void CloseContextMenu();

    /** 右键菜单是否开着。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Is Context Menu Visible"))
    bool IsContextMenuVisible() const;

    /** 菜单当前针对的物品句柄；没开菜单（或没目标）返回 `0`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Context Menu Item Handle"))
    int64 GetContextMenuItemHandle() const;

    /**
     * 执行菜单里的动作，返回 `0` 或错误码：
     *
     * - 旋转 → `RotateItem`（不可旋转的定义 → `4`）；
     * - 拆分一半 → 交给子格子的 `SplitHalfItem`（拆分核心只此一份，落点策略见那里）；
     * - 使用 → 广播 `OnItemUsed` + 记 `LastUsedItemHandle`（背包不实现使用效果）；
     * - 销毁 → `DestroyItem` + 广播 `OnItemDestroyed` + 记 `LastDestroyedItemHandle`；
     * - 整理容器 → 对这件物品**所在容器**调 `AutosortContainer`（没落位 → `22`）。
     *
     * **没有目标**（菜单没开过 / 已关掉）→ `22`（参数非法）+ 广播 `OnOperationFailed`。
     * 动作成功 → 顺手收起菜单；失败 → 菜单留着（让用户换个动作）+ 广播 `OnOperationFailed`。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Invoke Context Action"))
    int32 InvokeContextAction(EInvContextAction Action);

    // ==================== 事件（子格子内部调它们；测试 / 蓝图也可以直接驱动） ====================

    /** 拿起一件物品：广播 `OnItemPickedUp` + 计数 + 拿起音效。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Item Picked Up"))
    void NotifyItemPickedUp(const FInvItemView& Item);

    /** 落位结束（成功 / 失败都来）：广播 `OnItemDropped` + 计数 + 放下 / 失败音效。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Item Dropped"))
    void NotifyItemDropped(const FInvItemView& Item, int32 ResultCode);

    /** 使用一件物品：广播 `OnItemUsed` + 计数 + 记 `LastUsedItemHandle`。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Item Used"))
    void NotifyItemUsed(const FInvItemView& Item);

    /** 一件物品被销毁：广播 `OnItemDestroyed` + 计数 + 记 `LastDestroyedItemHandle` + 放下音效。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Item Destroyed"))
    void NotifyItemDestroyed(const FInvItemView& Item);

    /**
     * 报告一次失败：广播 `OnOperationFailed` + 记 `FailedEventCount` / `LastFailedCode` / `LastFailedMessage`
     * + 失败音效。子格子与菜单的失败路径都汇到这里（界面侧只此一处，不重复计数）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Operation Failed"))
    void NotifyOperationFailed(int32 Code, const FString& Message);

    // ==================== 跨容器拖（口袋 ↔ 弹挂 ↔ 背包） ====================

    /**
     * 源格子拖动时把屏幕坐标转播过来：光标下的子格子画跨容器落点预览，其余的清掉。
     * （鼠标被源格子捕获，别的格子收不到事件，所以只能由源格子转播。）
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Update Cross Grid Drag Preview"))
    void UpdateCrossGridDragPreview(UInvGridWidget* SourceGrid, FVector2D ScreenPosition, bool bRotated);

    /**
     * 源格子松开鼠标时调它：光标下的**别的**子格子接管这次落位（跨容器拖）。
     * 返回 `true` = 已经交给那一格处理（源格子不必再自己落位）；`false` = 光标不在别的格子上。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Route Drop To Grid Under Cursor"))
    bool RouteDropToGridUnderCursor(UInvGridWidget* SourceGrid, FVector2D ScreenPosition, bool bRotated);

    /** 清掉所有子格子的跨容器落点预览（拖拽结束 / 取消时调）。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Clear Cross Grid Drag Preview"))
    void ClearCrossGridDragPreview();

    // ==================== Slate 生命周期 ====================

    /** 释放 Slate 资源时把 `EnsureConstructed` 抓的那手引用也放掉（不然会吊着 Slate 控件不放）。 */
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;

    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

protected:
    /** 子控件（标题 / 负重条 / 整理按钮）的高度与间距（像素）。 */
    static constexpr float HeaderHeight = 24.f;
    static constexpr float CaptionHeight = 18.f;
    static constexpr float RowSpacing = 12.f;
    static constexpr float SortButtonWidth = 88.f;
    static constexpr float MinPanelWidth = 320.f;

    /** 右键菜单的宽度与每个按钮的高度（像素）；菜单总高 = 可见按钮数 × 按钮高 + 内边距。 */
    static constexpr float ContextMenuWidth = 132.f;
    static constexpr float ContextMenuButtonHeight = 26.f;

    /** 定时刷新的下限（秒），免得配置成 0 之外的极小值把定时器退化成逐帧。 */
    static constexpr float MinRefreshInterval = 0.05f;

    /** 子控件的默认字号（像素）。 */
    static constexpr int32 TitleFontSize = 14;
    static constexpr int32 LabelFontSize = 12;

private:
    /** 已经建过的「容器 + 子网格」对（与 `GridWidgets` 同下标）：容器拓扑变了才补 / 隐藏格子。 */
    TArray<FInvGridSlotKey> GridSignature;

    /**
     * `EnsureConstructed` 抓的一手 Slate 根控件引用。
     * `TakeWidget()` 的返回值没人持有的话会当场被回收，控件的缓存立刻失效——所以这里自己拿着，
     * `ReleaseSlateResources` 时再放掉。
     */
    TSharedPtr<SWidget> BuiltSlateRoot;

    /** 刷新定时器；`NativeDestruct` 清掉。 */
    FTimerHandle RefreshTimerHandle;

    /** 建静态子控件（标题 / 负重条 / 整理按钮）与根画布。 */
    void BuildStaticContent();

    /**
     * 建右键菜单（底板 + 竖排 5 个按钮，下标 = `EInvContextAction` 取值），初始 `Collapsed`。
     * 按钮点击各绑一个动态委托（`UButton::OnClicked` 不带参数，所以只能一个动作一个处理函数）。
     */
    void BuildContextMenu();

    /** 找「哪块子格子正显示着物品 `Item`」；没有返回空（拆分要靠它复用子格子的拆分核心）。 */
    UInvGridWidget* FindGridOwningItem(int64 Item) const;

    /** 播一个 2D 音效：`bPlaySounds` 关掉 / 音效留空 / 没有世界时都是空操作（默认三个都空 = 静音）。 */
    void PlayUiSound(const TObjectPtr<USoundBase>& Sound) const;

    /**
     * 保证控件树已经建起来（幂等）：没有 `WidgetTree` 就先 `Initialize`，然后挂根画布 + 静态子控件。
     * **只建 UObject 层的控件**（不碰 Slate），所以无头命令集里也能跑——`Refresh` 也走它。
     */
    void EnsureContentBuilt();

    /** 把子控件放进画布（位置 + 尺寸都写死，自动布局只有这一处）。 */
    void PlaceInCanvas(UWidget* Widget, const FVector2D& Position, const FVector2D& Size);

    /** 按 `ShowTypes` 收集「应有的格子」（容器 + 子网格下标，顺序确定）。 */
    void CollectDesiredGrids(TArray<FInvGridSlotKey>& OutGrids) const;

    /** 补建缺的子格子、隐藏多出来的子格子；返回是否有结构变化。 */
    bool SyncGridWidgets(const TArray<FInvGridSlotKey>& Desired);

    /** 建一块子格子（含它的小标题），并挂进画布；`OutCaption` 回传标题控件（可能为空）。 */
    UInvGridWidget* CreateGridWidget(int64 InContainer, int32 InPart, int32 AtIndex, UTextBlock*& OutCaption);

    /** 按当前格子尺寸重排：同容器多块并排一行，容器之间换行。 */
    void LayoutGrids();

    /** 负重条与文本的刷新。 */
    void UpdateWeightDisplay();

    /** 定时器回调。 */
    void OnRefreshTimer();

    /** 「整理」按钮的点击处理（动态委托，必须是 UFUNCTION）。 */
    UFUNCTION()
    void OnSortClicked();

    /** 右键菜单 5 个按钮的点击处理（`UButton::OnClicked` 不带参数，只能一动作一个函数）。 */
    UFUNCTION()
    void OnContextRotateClicked();

    UFUNCTION()
    void OnContextSplitHalfClicked();

    UFUNCTION()
    void OnContextUseClicked();

    UFUNCTION()
    void OnContextDestroyClicked();

    UFUNCTION()
    void OnContextAutosortClicked();

    /** 同一块子网格是否已经在 `GridWidgets` 里（复用而不是重建）。 */
    UInvGridWidget* FindReusableGrid(int64 InContainer, int32 InPart) const;

    /**
     * 光标下（排除 `Exclude`）的可见子格子；隐藏的、以及还没绘制过（没有几何）的格子跳过。
     * 返回空 = 光标不在任何别的子格子上。
     */
    UInvGridWidget* FindGridUnderCursor(FVector2D ScreenPosition, UInvGridWidget* Exclude) const;
};
