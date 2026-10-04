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
class UImage;
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
 * 整屏布局解算结果（`ComputeScreenLayout` 的返回值，也是 `ScreenLayout` 的镜像字段）。
 *
 * 三个值的含义：面板左上角在 viewport 坐标系里的位置、整屏尺寸（含面板四周留白）、实际生效的单格边长。
 */
USTRUCT(BlueprintType)
struct FInvScreenLayout
{
    GENERATED_BODY()

    /** 实际生效的单格边长（像素）；`<= 0` 表示还没算过。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包界面")
    float CellSize = 0.f;

    /** 整屏尺寸（像素，含面板四周留白）：面板簇 + 两倍 `PanelSpacing`。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包界面")
    FVector2D TotalSize = FVector2D::ZeroVector;

    /** 整屏左上角在 viewport 坐标系里的位置（居中时 `PanelOrigin + TotalSize / 2 = ViewportSize / 2`）。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包界面")
    FVector2D PanelOrigin = FVector2D::ZeroVector;
};

/**
 * 容器面板在整屏里的排布槽位。
 *
 * 主面板（背包）居中，其余贴上去：左边一列（弹挂）、右边一列（安全箱）、下面一行（口袋），
 * 认不出槽位的容器一律往下堆（`Extra`）。
 */
enum class EInvScreenPanelSlot : uint8
{
    /** 主面板：背包，摆在正中间。 */
    Main,

    /** 主面板左边一列：弹挂。 */
    Left,

    /** 主面板右边一列：安全箱。 */
    Right,

    /** 主面板下面一行：口袋。 */
    Bottom,

    /** 其它容器：继续往下堆。 */
    Extra,
};

/**
 * 一块容器面板在布局里的尺寸与位置（界面内部用，不反射）。
 *
 * `Position` 是**簇坐标**：主面板中心 = 原点，所以主面板的位置是 `-Size / 2`，左边面板的 X 是负的。
 */
struct FInvScreenPanelRect
{
    /** 容器句柄。 */
    int64 Container = 0;

    /** 排布槽位。 */
    EInvScreenPanelSlot Slot = EInvScreenPanelSlot::Extra;

    /** 面板尺寸（像素，含标题栏与内边距）。 */
    FVector2D Size = FVector2D::ZeroVector;

    /** 面板左上角（簇坐标）；主面板中心在原点。 */
    FVector2D Position = FVector2D::ZeroVector;
};

/**
 * 背包界面：把几个 `UInvGridWidget`（每个容器子网格一块）拼成一屏，外加负重条与「整理」按钮。
 *
 * 零资产：子控件（`UCanvasPanel` / `UImage` / `UTextBlock` / `UProgressBar` / `UButton`）全部在 C++ 里
 * `ConstructWidget` 出来，用引擎默认样式、默认字体与纯色圆角笔刷；不需要任何 Widget Blueprint、贴图或字体资产。
 * 直接 `Create Widget`（`Class = 背包界面控件`）→ 填 `Inventory` → `Add To Viewport` 就能用。
 *
 * 布局（`bCenterOnViewport` 默认开 = 居中模式）：整屏相对 viewport **水平垂直居中**，
 * **背包是主面板放在正中间**，其余容器按「存在与否」贴上去——左边一列 = 弹挂，右边一列 = 安全箱，
 * 下面一行 = 口袋，其它容器继续往下堆。只有背包时就是纯居中。
 * 每块容器面板 = 半透明深色圆角底板（`BackgroundOpacity`）+ 1px 描边 + 顶部标题栏（容器标题）；
 * 主面板的标题栏里还有负重条与负重文本，「整理」按钮在主面板右下角。
 * 同一容器的多块子网格在面板里并排，各自上方有小标题（如 `弹挂 part2 (1x2)`）。
 *
 * 自适应：按 `DesiredCellSize` 拼出来的整屏超过 viewport 的 `MaxScreenFraction`（默认 0.85）时
 * **等比缩小单格边长**（下限 `MinCellSize`，默认 24），`Origin` 则作为居中后的额外偏移。
 * 分辨率变化后在 `Refresh()` 里按当前 viewport 尺寸重算，所以定时刷新与事件驱动刷新都会跟上。
 * 关掉 `bCenterOnViewport` = **完全退回旧行为**：`Origin` 左上角起点 + 手动 `CellSize` + 每容器一行。
 *
 * 字体：容器标题 = `CellSize * 0.4`（下限 14），堆叠角标 / tooltip / 负重文本 / 小标题 = 基准字号 × `CellSize / 64`
 * （见 `ScaledFontSize`），都不写死像素字号。
 *
 * 刷新：`Refresh()` 是幂等的全量刷新（负重条 + 标题 + 每块子网格 + 布局），
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
 * 无头命令集 / 专用服务器里拿不到 viewport 尺寸（没有 Slate 应用）：跳过 Slate 那一步、不居中，
 * 布局退回 `CellSize` 的确定性口径（`Origin` 起摆），数据与节点照常可用。
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

    /**
     * 单格边长（像素）。**手动模式 / 兼容模式**（`bCenterOnViewport = false`）用它；
     * 居中模式下期望值看 `DesiredCellSize`，实际生效值看 `EffectiveCellSize`（可能被自适应缩小）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float CellSize = 64.f;

    /** 要显示的根容器类型（顺序 = 从上到下的显示顺序）；`Nested` 会被忽略。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    TArray<EInvContainerType> ShowTypes = { EInvContainerType::Pockets, EInvContainerType::ChestRig,
        EInvContainerType::SafeBox, EInvContainerType::Backpack };

    /** 定时刷新间隔（秒）；`<= 0` = 不挂定时器（只靠事件驱动刷新）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float RefreshIntervalSeconds = 0.5f;

    /**
     * 整屏左上角在屏幕上的位置（像素）。
     *
     * 居中模式（默认）下它是**居中之后的额外偏移**（默认 `(0,0)` = 正好居中）；
     * 手动模式下它就是老的「左上角起点」语义。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    FVector2D Origin = FVector2D::ZeroVector;

    /** 是否把整屏按 viewport 居中（默认开）。关掉 = 完全退回旧行为：`Origin` 左上角起点 + 手动 `CellSize`。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    bool bCenterOnViewport = true;

    /** 居中模式期望的单格边长（像素）；整屏超出 `MaxScreenFraction` 时自动等比缩小。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float DesiredCellSize = 64.f;

    /** 整屏尺寸最多占 viewport 的比例（自适应缩放的触发线）；`<= 0` 按 0.85。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float MaxScreenFraction = 0.85f;

    /** 自适应缩小的下限（像素）：缩到这里就不再缩（宁可溢出，也不把格子缩成看不清）；`<= 0` 按 24。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float MinCellSize = 24.f;

    /** 容器面板之间的间距（像素），同时作为整屏四周的留白。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    FVector2D PanelSpacing = FVector2D(24.f, 18.f);

    /** 面板底板的不透明度（0~1，默认 0.85）；压低它能让背后的游戏画面更清楚。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float BackgroundOpacity = 0.85f;

    /** 面板标题栏高度（像素，随单格边长缩放）；放不下负重条时会自动加高。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float TitleHeight = 40.f;

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

    /** 顶部说明文字（键盘提示：拖拽移动 / R 旋转 / Esc 取消）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UTextBlock> TitleLabel = nullptr;

    /** 「整理」按钮里的文本（字号随单格边长缩放）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UTextBlock> SortButtonLabel = nullptr;

    // ==================== 布局状态（只读） ====================

    /** 最近一次布局**实际生效**的单格边长（自适应缩放后的值）；还没布局过时为 `0`。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|布局状态")
    float EffectiveCellSize = 0.f;

    /**
     * 最近一次算出来的整屏布局。
     *
     * 居中模式：`PanelOrigin` / `TotalSize` / `CellSize` 三个值都有效；
     * 手动模式（`bCenterOnViewport = false`）或拿不到 viewport 尺寸时：只填 `CellSize` 与 `PanelOrigin`，
     * `TotalSize` 保持 `(0,0)`（那种路径没有「整屏尺寸」这个概念）。
     */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|布局状态")
    FInvScreenLayout ScreenLayout;

    /** 每块容器面板的底板；下标 = `PanelContainers`，**下标 0 恒为主面板（背包）**。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|布局状态")
    TArray<TObjectPtr<UImage>> PanelBoards;

    /** 每块容器面板的标题栏底条（与 `PanelBoards` 同下标）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|布局状态")
    TArray<TObjectPtr<UImage>> PanelTitleBars;

    /** 每块容器面板的标题文本（与 `PanelBoards` 同下标，字号 = `CellSize * 0.4` 且 ≥ 14）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|布局状态")
    TArray<TObjectPtr<UTextBlock>> PanelTitles;

    /** 每块容器面板对应的容器句柄（与 `PanelBoards` 同下标；`0` = 那一块没面板）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|布局状态")
    TArray<int64> PanelContainers;

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

    // ==================== 布局（纯函数 / 查询） ====================

    /**
     * 屏幕布局纯函数：**不读实例状态**，按「参考内容 = 默认背包根容器（6×5 格）+ 面板四周留白 `PanelSpacing`」
     * 解算整屏布局，供蓝图 / 测试直接断言，也是实例自适应缩放的同一套规则。
     *
     * 规则：
     * - 单格边长先取 `InDesiredCellSize`，再被可用尺寸压住：`可用 = ViewportSize * max(InMaxScreenFraction, …)`，
     *   每轴可用边长 = `(可用尺寸 - 两倍 InPanelSpacing) / 格数`，取两轴较小值；
     * - 结果再夹到 `InMinCellSize`（**下限优先**：viewport 小到连下限都放不下时宁可溢出，也不给出 `<= 0` 的值）；
     * - `TotalSize = 格数 × CellSize + 两倍 InPanelSpacing`，`PanelOrigin = (ViewportSize - TotalSize) / 2`
     *   （所以 `PanelOrigin + TotalSize / 2` 恒等于 `ViewportSize / 2`，即整屏居中）。
     *
     * 参数兜底：`InDesiredCellSize <= 0` 按 `InMinCellSize` 处理；`InMinCellSize <= 0` 按 24；
     * `InMaxScreenFraction <= 0` 按 0.85（`> 1` 夹到 1）；`InPanelSpacing` 的负分量按 0；`ViewportSize` 的负分量按 0。
     *
     * （参数名带 `In` 前缀不是随手起的：同名属性就在这个类上，UHT 不允许函数参数与类成员同名。）
     */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Compute Screen Layout"))
    static FInvScreenLayout ComputeScreenLayout(FVector2D ViewportSize, float InDesiredCellSize,
        float InMaxScreenFraction, float InMinCellSize, FVector2D InPanelSpacing);

    /**
     * 字号缩放工具：`实际字号 = 基准字号 × 单格边长 / 64`，下限 10 像素。
     * 基准字号统一按 64 像素的单格边长设计，所以同一份代码在 48 的单格边长下拿到的就是老字号（12）。
     * `InCellSize <= 0` 时按基准边长算（等于原样返回基准字号）。
     */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Scaled Font Size"))
    static int32 ScaledFontSize(int32 BaseFontSize, float InCellSize);

    /** 容器标题字号 = 单格边长 × 0.4（四舍五入），下限 14。`InCellSize <= 0` 时按基准边长 64 算（= 26）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Title Font Size"))
    static int32 TitleFontSize(float InCellSize);

    /** 最近一次布局**实际生效**的单格边长（自适应缩放后的值）；还没布局过时为 `0`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Effective Cell Size"))
    float GetEffectiveCellSize() const { return EffectiveCellSize; }

    /** 最近一次算出来的整屏布局（见 `ScreenLayout` 的说明）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Screen Layout"))
    FInvScreenLayout GetScreenLayout() const { return ScreenLayout; }

    /** 每块容器面板的底板（下标 = `Get Panel Containers`，下标 0 = 主面板）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Panel Boards"))
    TArray<UImage*> GetPanelBoards() const;

    /** 每块容器面板对应的容器句柄（与 `Get Panel Boards` 同下标；`0` = 那一块没面板）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Panel Containers"))
    TArray<int64> GetPanelContainers() const;

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

    /**
     * 子控件**基准**字号（像素）：按 64 像素的单格边长设计，实际字号 = `ScaledFontSize(基准, 单格边长)`。
     * 基准放这里而不是写死在调用处，是为了「字号随格子缩放」只有一套口径。
     * （面板标题另有一套：`TitleFontSize` = 单格边长 × 0.4、下限 14，比正文更醒目。）
     */
    static constexpr int32 LabelFontBase = 16;

    /** 面板底板的圆角半径基准（像素，按单格边长缩放）。 */
    static constexpr float PanelCornerRadiusBase = 10.f;

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

    /** 把子控件放进画布（位置 + 尺寸 + 画布层级都写死，自动布局只有这一处）。 */
    void PlaceInCanvas(UWidget* Widget, const FVector2D& Position, const FVector2D& Size, int32 ZOrder = 0);

    /** 按 `ShowTypes` 收集「应有的格子」（容器 + 子网格下标，顺序确定）。 */
    void CollectDesiredGrids(TArray<FInvGridSlotKey>& OutGrids) const;

    /** 补建缺的子格子、隐藏多出来的子格子；返回是否有结构变化。 */
    bool SyncGridWidgets(const TArray<FInvGridSlotKey>& Desired);

    /** 建一块子格子（含它的小标题），并挂进画布；`OutCaption` 回传标题控件（可能为空）。 */
    UInvGridWidget* CreateGridWidget(int64 InContainer, int32 InPart, int32 AtIndex, UTextBlock*& OutCaption);

    /** 按当前格子尺寸重排：同容器多块并排一行，容器之间换行（**手动模式的旧口径**）。 */
    void LayoutGrids();

    // ==================== 布局（居中 / 自适应） ====================

    /**
     * 算这次布局要用的单格边长，写进 `EffectiveCellSize`：
     *
     * - 手动模式（`bCenterOnViewport = false`）→ `CellSize`（`<= 0` 时退回 `DesiredCellSize`）；
     * - 居中模式但**拿不到 viewport 尺寸**（没上屏 / 无头命令集 / 专用服务器）→ 同上，保持确定性；
     * - 居中模式且尺寸已知 → 按实测「固定部分 + 每格尺寸」解算：`min(DesiredCellSize, 可用/内容)`，
     *   夹到 `MinCellSize`，再用实测兜一遍取整误差（超了就按比例再缩，最多 3 轮）。
     */
    void UpdateEffectiveCellSize();

    /** 布局总入口：居中模式走 `LayoutCentered`，手动模式走旧口径。 */
    void ApplyLayout();

    /** 居中模式：面板簇居中 + 背景板 / 标题栏 / 负重条 / 「整理」按钮 + 子格子落位。 */
    void LayoutCentered();

    /** 手动模式（兼容）：`Origin` 左上角起点、每个容器一行、`CellSize` 原样，不画面板。 */
    void LayoutManual();

    /**
     * 解算每块容器面板的尺寸与相对位置（簇坐标，主面板中心在原点）。
     *
     * `InCellSize` 传 1 与 0 两次就能分离「不随格子缩放的固定部分」与「每个格子的尺寸」——
     * 自适应缩放靠这两档实测，不靠拼常数。
     */
    void BuildPanelRects(float InCellSize, TArray<FInvScreenPanelRect>& OutPanels) const;

    /** 面板簇尺寸（不含 `PanelSpacing` 留白）：把 `BuildPanelRects` 的结果按主面板中心对称展开。 */
    FVector2D MeasureClusterSize(float InCellSize) const;

    /** 保证面板装饰（底板 / 标题栏 / 标题文本）至少 `Needed` 块，多余的收起来（控件复用，不销毁）。 */
    void SyncPanelDecor(int32 Needed);

    /** 字号按单格边长重新应用（提示 / 负重文本 / 按钮 / 菜单 / 面板标题 / 子格子小标题）。 */
    void ApplyFonts(float InCellSize);

    /** 面板装饰的可见性（没有面板时全部收起）。 */
    void SetPanelDecorVisible(bool bVisible);

    /**
     * 把标题提示 / 负重条 / 负重文本 / 「整理」按钮按 `Origin` 摆好（**旧口径**）：
     * 手动模式与「一块面板都没有」时用它。
     */
    void LayoutHeaderAtOrigin(float InCellSize);

    /** viewport 尺寸（Slate 单位）：本控件几何优先，其次游戏 viewport；拿不到返回 `(0,0)` = 尺寸未知。 */
    FVector2D ResolveViewportSize() const;

    /** 手动模式 / 尺寸未知时用的单格边长：`CellSize`，非法时退回 `DesiredCellSize`。 */
    float ManualCellSize() const;

    /** 字号缩放用的单格边长：`EffectiveCellSize`，还没布局过时退回 `ManualCellSize()`。 */
    float FontCellSize() const;

    /** 生效的面板间距（负分量按 0）。 */
    FVector2D EffectivePanelSpacing() const;

    /** 生效的整屏占比上限（`<= 0` 按 0.85，`> 1` 夹到 1）。 */
    float EffectiveMaxScreenFraction() const;

    /** 生效的自适应下限（`<= 0` 按 24）。 */
    float EffectiveMinCellSize() const;

    /** 某个容器在 `GridWidgets` 里的子格子（按 `Container` 过滤，按下标升序）。 */
    void CollectGridsOfContainer(int64 InContainer, TArray<UInvGridWidget*>& OutGrids) const;

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
