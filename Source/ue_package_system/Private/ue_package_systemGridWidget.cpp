#include "ue_package_systemGridWidget.h"

// 落位成功后要通知界面刷全部子格子（`OwnerScreen->NotifyInventoryChanged()`）。
#include "ue_package_systemScreenWidget.h"

#include "Application/SlateApplicationBase.h"
#include "Blueprint/WidgetTree.h"
#include "Components/SizeBox.h"
#include "Engine/Texture2D.h"
#include "Fonts/SlateFontInfo.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"

namespace
{
    // 命名前缀统一成 InvUi*：本模块开了 unity build，匿名命名空间里的名字会跟同批 .cpp 撞车。

    /** 错误码（与组件层 / Rust 侧一致，见 docs/API.md 第 5 节）：句柄 / 参数非法。 */
    constexpr int32 InvUiErrInvalidHandle = 23;
    constexpr int32 InvUiErrInvalidArgument = 22;

    /** 目标位置放不下（拆分时「本容器所有子网格都没有空位」也报它）。 */
    constexpr int32 InvUiErrOccupied = 2;

    /** 数量不合法：拆分的件数越界（堆叠 < 2 拆不出「一半」）。 */
    constexpr int32 InvUiErrInvalidStackCount = 18;

    /** 描边厚度（像素）。 */
    constexpr float InvUiBorderThickness = 1.f;

    /**
     * 堆叠角标 / tooltip 的**基准**字号：按 64 像素的单格边长设计（实际字号随 `CellSize` 缩放，
     * 见 `UInvInventoryScreenWidget::ScaledFontSize`），不是写死的像素字号。
     */
    constexpr int32 InvUiBadgeFontBase = 16;
    constexpr int32 InvUiTooltipFontBase = 16;

    /** 估算文字宽度用的系数：一个「半角」字符 ≈ 0.6 × 字号。 */
    constexpr float InvUiGlyphWidthFactor = 0.6f;

    // 只用到这几个按键，就不为 `EKeys::*` 的静态常量多挂一个 InputCore 依赖（FKey 的构造与比较都是头内联的，
    // 名字与 EKeys 表里的完全一致）。
    const FKey InvUiKeyLeftMouse(TEXT("LeftMouseButton"));
    const FKey InvUiKeyRightMouse(TEXT("RightMouseButton"));
    const FKey InvUiKeyRotate(TEXT("R"));
    const FKey InvUiKeyCancel(TEXT("Escape"));
    const FKey InvUiKeyDelete(TEXT("Delete"));

    /** 网格底纹 / 描边配色（深浅两档，交替画成棋盘格，一眼能数格）。 */
    const FLinearColor InvUiCellColorA(0.10f, 0.11f, 0.13f, 1.f);
    const FLinearColor InvUiCellColorB(0.14f, 0.15f, 0.18f, 1.f);
    const FLinearColor InvUiGridBorderColor(0.35f, 0.37f, 0.42f, 1.f);
    const FLinearColor InvUiItemBorderColor(0.75f, 0.78f, 0.82f, 1.f);
    const FLinearColor InvUiSelectionColor(1.f, 0.85f, 0.25f, 1.f);
    const FLinearColor InvUiDropOkColor(0.25f, 1.f, 0.35f, 0.28f);
    const FLinearColor InvUiDropBadColor(1.f, 0.25f, 0.25f, 0.28f);
    const FLinearColor InvUiBadgeColor(0.f, 0.f, 0.f, 0.7f);
    const FLinearColor InvUiTooltipColor(0.05f, 0.05f, 0.07f, 0.92f);
    const FLinearColor InvUiTextColor(0.95f, 0.95f, 0.95f, 1.f);
    const FLinearColor InvUiSubTextColor(0.72f, 0.74f, 0.78f, 1.f);
    const FLinearColor InvUiInvalidColor(1.f, 0.35f, 0.35f, 1.f);

    /** 判断一个字符算不算「宽字符」（中日韩）：估算文本宽度时按两倍算。 */
    bool InvUiIsWideChar(TCHAR Char)
    {
        const uint32 Code = static_cast<uint32>(Char);
        return (Code >= 0x1100 && Code <= 0x115F)      // 韩文字母
            || (Code >= 0x2E80 && Code <= 0xA4CF)      // 中日韩部首 / 汉字 / 假名
            || (Code >= 0xAC00 && Code <= 0xD7A3)      // 韩文音节
            || (Code >= 0xF900 && Code <= 0xFAFF)      // 兼容汉字
            || (Code >= 0xFF00 && Code <= 0xFF60);     // 全角
    }

    /** 定义的显示名（空定义 / 没填名字时给个可读的占位）。 */
    FString InvUiDisplayNameOf(const UInvItemDefinition* Definition)
    {
        if (!Definition)
        {
            return FString(TEXT("未知物品"));
        }
        const FString Display = Definition->DisplayName.ToString();
        return Display.IsEmpty() ? Definition->GetName() : Display;
    }

    /** 占位图标上画的那个字：显示名的首字，没名字就退回资产名首字。 */
    FString InvUiInitialOf(const UInvItemDefinition* Definition)
    {
        if (!Definition)
        {
            return FString(TEXT("?"));
        }
        const FString Display = InvUiDisplayNameOf(Definition);
        return Display.IsEmpty() ? FString(TEXT("?")) : Display.Left(1);
    }

    /** 占位底色：按定义名的哈希取一个固定色相（同一件东西每次都是同一个颜色）。 */
    FLinearColor InvUiPlaceholderColor(const UInvItemDefinition* Definition)
    {
        const uint32 Hash = Definition ? GetTypeHash(Definition->GetFName()) : 0u;
        return FLinearColor::MakeFromHSV8(static_cast<uint8>(Hash % 256), 110, 190);
    }

    /** 把浮点数写成「0.012」这种固定小数位文本（不带千分位，日志 / tooltip 里好读）。 */
    FString InvUiFormatFloat(float Value, int32 Digits)
    {
        return FString::Printf(TEXT("%.*f"), Digits, Value);
    }
}

// ==================== 构造 ====================

UInvGridWidget::UInvGridWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 零资产：不设任何样式 / 字体资产；绘制全在 NativePaint 里自绘。
    // 可见性必须能自己吃鼠标事件（默认值在控件蓝图里常被改成 SelfHitTestInvisible）。
    SetVisibility(ESlateVisibility::Visible);

    // R / Esc 要键盘焦点；SetFocus 在鼠标按下时调（这里只是允许被聚焦）。
    SetIsFocusable(true);
}

// ==================== 纯函数 ====================

FIntPoint UInvGridWidget::LocalPositionToCell(FVector2D LocalPos, float InCellSize, FIntPoint InGridSize)
{
    const float Cell = InCellSize > 0.f ? InCellSize : DefaultCellSize;

    // 向下取整：负数在 C++ 的 int 转换里是朝零取整，所以显式走 FloorToInt。
    FIntPoint Cell2D(FMath::FloorToInt(LocalPos.X / Cell), FMath::FloorToInt(LocalPos.Y / Cell));

    // 越界夹到边界；网格有一边是 0 时该边固定 0（空配置也不会给出非法下标）。
    Cell2D.X = (InGridSize.X > 0) ? FMath::Clamp(Cell2D.X, 0, InGridSize.X - 1) : 0;
    Cell2D.Y = (InGridSize.Y > 0) ? FMath::Clamp(Cell2D.Y, 0, InGridSize.Y - 1) : 0;
    return Cell2D;
}

FIntPoint UInvGridWidget::FootprintCellsFor(const UInvItemDefinition* Definition, bool bRotated)
{
    if (!Definition)
    {
        return FIntPoint(1, 1);  // 定义缺失：按 1x1 画，至少不丢东西
    }

    const int32 Width = FMath::Max(Definition->Width, 1);
    const int32 Height = FMath::Max(Definition->Height, 1);
    return bRotated ? FIntPoint(Height, Width) : FIntPoint(Width, Height);
}

FIntPoint UInvGridWidget::FootprintCells(const FInvItemView& View)
{
    return FootprintCellsFor(View.Definition, View.bRotated);
}

FVector2D UInvGridWidget::CellToLocalPosition(FIntPoint Cell, float InCellSize)
{
    const float CellPixels = InCellSize > 0.f ? InCellSize : DefaultCellSize;
    return FVector2D(Cell.X * CellPixels, Cell.Y * CellPixels);
}

FIntPoint UInvGridWidget::ClampCellToGrid(FIntPoint Cell, FIntPoint Cells, FIntPoint GridSize)
{
    // 网格比物品还小时（配置错了）退回 (0,0)：反正放不下，别给出负数下标。
    const int32 MaxX = FMath::Max(GridSize.X - FMath::Max(Cells.X, 0), 0);
    const int32 MaxY = FMath::Max(GridSize.Y - FMath::Max(Cells.Y, 0), 0);
    return FIntPoint(FMath::Clamp(Cell.X, 0, MaxX), FMath::Clamp(Cell.Y, 0, MaxY));
}

bool UInvGridWidget::ComputeDropTarget(const FInvItemView& Dragged, FVector2D LocalPos, float InCellSize,
    FIntPoint InGridSize, FIntPoint& OutCell)
{
    const FIntPoint Cells = FootprintCells(Dragged);

    if (InGridSize.X <= 0 || InGridSize.Y <= 0 || Cells.X > InGridSize.X || Cells.Y > InGridSize.Y)
    {
        // 空网格 / 物品比网格还大：没有任何合法落点。
        OutCell = FIntPoint(0, 0);
        return false;
    }

    const FIntPoint Wanted = LocalPositionToCell(LocalPos, InCellSize, InGridSize);
    OutCell = ClampCellToGrid(Wanted, Cells, InGridSize);
    return true;
}

// ==================== 数据 ====================

float UInvGridWidget::EffectiveCellSize() const
{
    return CellSize > 0.f ? CellSize : DefaultCellSize;
}

bool UInvGridWidget::HasValidTarget() const
{
    if (!Inventory || Container <= 0 || Part < 0)
    {
        return false;
    }
    const TArray<FIntPoint> Parts = Inventory->GetContainerParts(Container);
    return Parts.IsValidIndex(Part);
}

void UInvGridWidget::LogInvalidConfigOnce(const TCHAR* Reason)
{
    // 定时刷新会高频调 Refresh：配置没变就不重复打日志，配置变了才再报一次。
    const FIntPoint Config(Container == 0 ? -1 : static_cast<int32>(Container), Part);
    if (bLoggedInvalidConfig && Config == LoggedConfig)
    {
        return;
    }
    bLoggedInvalidConfig = true;
    LoggedConfig = Config;

    UE_LOG(LogTemp, Warning,
        TEXT("背包界面: 格子控件 %s 在「%s」时发现配置不可用（组件 %s / 容器 %lld / 子网格 %d）——不画数据，先看组件是否就绪"),
        *GetName(), Reason ? Reason : TEXT("未知操作"),
        Inventory ? *Inventory->GetName() : TEXT("未绑定"), Container, Part);
}

void UInvGridWidget::Refresh()
{
    Occupancy.Reset();
    Items.Reset();
    GridSize = FIntPoint(0, 0);
    HoveredItem = 0;

    if (!HasValidTarget())
    {
        LogInvalidConfigOnce(TEXT("刷新"));
        // 目标没了：拖拽 / 选中都不该留在幽灵上。
        ClearDragState();
        SelectedItem = 0;
        ApplyDesiredSizeToRoot();
        RequestRepaint();
        return;
    }
    bLoggedInvalidConfig = false;

    // 尺寸 / 占格表 / 物品视图：三个都走组件的公开节点，UI 不再自己解析容器结构。
    const TArray<FIntPoint> Parts = Inventory->GetContainerParts(Container);
    GridSize = Parts[Part];  // FIntPoint 在这里读作 (宽, 高)
    Occupancy = Inventory->GetContainerGrid(Container, Part);

    for (const FInvItemView& View : Inventory->GetContainerItemsView(Container))
    {
        if (View.HostPart != Part)
        {
            continue;  // 别的子网格上的东西不归这块画
        }
        Items.Add(View);

        // 图标按定义缓存：软引用第一次用到才 LoadSynchronous，之后每次都命中缓存。
        if (View.Definition && !IconCache.Contains(View.Definition))
        {
            IconCache.Add(View.Definition, View.Definition->Icon.LoadSynchronous());
        }
    }

    // 选中 / 拖拽的物品可能已经不在这块子网格了（被别的格子拖走、被删掉、整理挪位）：
    // 状态归零，免得对着幽灵按 R。
    if (SelectedItem != 0)
    {
        FInvItemView SelectedView;
        if (!FindItemView(SelectedItem, SelectedView))
        {
            SelectedItem = 0;
        }
    }
    if (bDragging)
    {
        FInvItemView DraggedView;
        if (!FindItemView(DraggedItem, DraggedView))
        {
            UE_LOG(LogTemp, Log, TEXT("背包界面: 格子控件 %s 上拖拽中的物品 %lld 已经不在本子网格，收尾拖拽"),
                *GetName(), DraggedItem);
            ClearDragState();
        }
    }

    ApplyDesiredSizeToRoot();
    RequestRepaint();
}

void UInvGridWidget::SetContainer(UInvInventoryComponent* InInventory, int64 InContainer, int32 InPart)
{
    Inventory = InInventory;
    Container = InContainer;
    Part = InPart;

    // 换了目标：旧目标上的拖拽 / 选中一律作废。
    ClearDragState();
    SelectedItem = 0;
    HoveredItem = 0;
    bLoggedInvalidConfig = false;
    LoggedConfig = FIntPoint(-2, -2);  // 强制下一次非法配置能报一次

    Refresh();
}

bool UInvGridWidget::EnsureConstructed()
{
    // 没有 Slate 应用时（无头命令集 / 专用服务器）**不能**去构造 Slate 控件：
    // 构造路径里会走 FSlateApplicationBase::Get()，没有应用就直接断言。
    // 这种环境下跳过并打一条日志：控件对象本身（数据 / 状态机）照样能用。
    if (!FSlateApplicationBase::IsInitialized())
    {
        UE_LOG(LogTemp, Log,
            TEXT("背包界面: 格子控件 %s 跳过 Slate 构造——当前没有 Slate 应用（无头命令集 / 服务器）"), *GetName());
        return false;
    }

    // TakeWidget 幂等：已经构造过就直接给缓存。
    // 返回值必须自己抓一手：没人持有时 Slate 根控件会当场被回收，GetCachedWidget 立刻变空。
    if (!GetCachedWidget().IsValid())
    {
        BuiltSlateRoot = TakeWidget();
    }
    return GetCachedWidget().IsValid();
}

void UInvGridWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    BuiltSlateRoot.Reset();
    Super::ReleaseSlateResources(bReleaseChildren);
}

// ==================== 交互状态机 ====================

bool UInvGridWidget::FindItemView(int64 ItemHandle, FInvItemView& OutView) const
{
    if (ItemHandle <= 0)
    {
        return false;
    }
    for (const FInvItemView& View : Items)
    {
        if (View.Handle == ItemHandle)
        {
            OutView = View;
            return true;
        }
    }
    return false;
}

int32 UInvGridWidget::OccupancyIndex(FIntPoint Cell) const
{
    if (Cell.X < 0 || Cell.Y < 0 || Cell.X >= GridSize.X || Cell.Y >= GridSize.Y)
    {
        return INDEX_NONE;
    }
    const int32 Index = Cell.Y * GridSize.X + Cell.X;
    return Occupancy.IsValidIndex(Index) ? Index : INDEX_NONE;
}

int64 UInvGridWidget::GetItemAtCell(FIntPoint Cell) const
{
    const int32 Index = OccupancyIndex(Cell);
    return (Index == INDEX_NONE) ? 0 : Occupancy[Index];
}

bool UInvGridWidget::CanOccupy(FIntPoint Cell, FIntPoint Cells, int64 SelfItem) const
{
    for (int32 Y = 0; Y < Cells.Y; ++Y)
    {
        for (int32 X = 0; X < Cells.X; ++X)
        {
            const int64 Host = GetItemAtCell(FIntPoint(Cell.X + X, Cell.Y + Y));
            if (Host != 0 && Host != SelfItem)
            {
                return false;  // 被别的物品占着
            }
        }
    }
    return true;
}

int64 UInvGridWidget::SingleOccupantAt(FIntPoint Cell, FIntPoint Cells, int64 SelfItem) const
{
    int64 Found = 0;
    for (int32 Y = 0; Y < Cells.Y; ++Y)
    {
        for (int32 X = 0; X < Cells.X; ++X)
        {
            const int64 Host = GetItemAtCell(FIntPoint(Cell.X + X, Cell.Y + Y));
            if (Host == 0 || Host == SelfItem)
            {
                continue;
            }
            if (Found != 0 && Found != Host)
            {
                return 0;  // 涉及不止一件别人的东西：不是「交换」能解决的，交给上层报错
            }
            Found = Host;
        }
    }
    return Found;
}

FInvItemView UInvGridWidget::MakeDropView(const FInvItemView& Source) const
{
    FInvItemView View = Source;
    View.bRotated = bDragRotated;
    return View;
}

int64 UInvGridWidget::BeginDragAtItem(int64 ItemHandle)
{
    if (!HasValidTarget())
    {
        LogInvalidConfigOnce(TEXT("开始拖拽"));
        return -InvUiErrInvalidHandle;
    }
    if (ItemHandle <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 格子控件 %s 开始拖拽时句柄非法（%lld）"), *GetName(), ItemHandle);
        return -InvUiErrInvalidArgument;
    }

    FInvItemView View;
    if (!FindItemView(ItemHandle, View))
    {
        UE_LOG(LogTemp, Log, TEXT("背包界面: 物品 %lld 不在本子网格（容器 %lld 的 %d 号），不开始拖拽"),
            ItemHandle, Container, Part);
        return 0;
    }

    // 抓取偏移 = 光标相对物品左上角的像素偏移（真实鼠标）；没有光标时取物品中心，保证测试可复现。
    const FVector2D ItemTopLeft = CellToLocalPosition(FIntPoint(View.X, View.Y), EffectiveCellSize());
    const FVector2D Footprint = FootprintPixels(View);
    if (bHasCursor)
    {
        DragGrabOffset = LastCursorLocal - ItemTopLeft;
        DragGrabOffset.X = FMath::Clamp(DragGrabOffset.X, 0.f, FMath::Max(Footprint.X - 1.f, 0.f));
        DragGrabOffset.Y = FMath::Clamp(DragGrabOffset.Y, 0.f, FMath::Max(Footprint.Y - 1.f, 0.f));
    }
    else
    {
        DragGrabOffset = Footprint * DefaultGrabOffsetFactor;
    }

    DraggedItem = ItemHandle;
    SelectedItem = ItemHandle;
    bDragging = true;
    bDragRotated = View.bRotated;
    bAutoRotatedOnLastDrop = false;
    DragLocalPos = ItemTopLeft;

    const FInvItemView DropView = MakeDropView(View);
    bDropInGrid = ComputeDropTarget(DropView, DragLocalPos, EffectiveCellSize(), GridSize, DropCell);
    bDropAllowed = bDropInGrid && CanOccupy(DropCell, FootprintCells(DropView), ItemHandle);

    UE_LOG(LogTemp, Log, TEXT("背包界面: 开始拖拽物品 %lld（容器 %lld 的 %d 号子网格，抓取偏移 %.1f,%.1f；落点 %d,%d）"),
        ItemHandle, Container, Part, DragGrabOffset.X, DragGrabOffset.Y, DropCell.X, DropCell.Y);

    NotifyOwnerScreenFocused();
    if (OwnerScreen)
    {
        // 「拿起」= 开始拖拽：广播 OnItemPickedUp（音效与事件镜像都在界面侧）。
        OwnerScreen->NotifyItemPickedUp(View);
    }
    RequestRepaint();
    return ItemHandle;
}

void UInvGridWidget::UpdateDragCursor(FVector2D LocalPos)
{
    LastCursorLocal = LocalPos;
    bHasCursor = true;

    if (!bDragging)
    {
        // 没在拖：这里只更新悬停（tooltip 用），不产生别的状态。
        HoveredItem = GetItemAtCell(LocalPositionToCell(LocalPos, EffectiveCellSize(), GridSize));
        RequestRepaint();
        return;
    }

    FInvItemView View;
    if (!FindItemView(DraggedItem, View))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 格子控件 %s 上拖拽的物品 %lld 已经不在了，收尾拖拽"),
            *GetName(), DraggedItem);
        ClearDragState();
        RequestRepaint();
        return;
    }

    // 幽灵位置 = 光标 - 抓取偏移（锚点语义：物品左上角），落点再按占位尺寸夹进网格。
    DragLocalPos = LocalPos - DragGrabOffset;
    const FInvItemView DropView = MakeDropView(View);
    const FIntPoint Cells = FootprintCells(DropView);
    bDropInGrid = ComputeDropTarget(DropView, DragLocalPos, EffectiveCellSize(), GridSize, DropCell);
    bDropAllowed = bDropInGrid && CanOccupy(DropCell, Cells, DraggedItem);
    HoveredItem = 0;  // 拖拽中不弹 tooltip

    RequestRepaint();
}

int32 UInvGridWidget::PlaceItemIntoThisGrid(int64 ItemHandle, const FInvItemView& ItemView, FIntPoint Cell,
    bool bRotated)
{
    // 落点先夹到「放得下」的位置：拖到右下角时自动贴边，而不是报越界。
    const FIntPoint Cells = FootprintCellsFor(ItemView.Definition, bRotated);
    const FIntPoint Target = ClampCellToGrid(Cell, Cells, GridSize);

    int32 Result = Inventory->MoveItem(ItemHandle, Container, Part, Target.X, Target.Y, bRotated);
    if (Result == 0)
    {
        UE_LOG(LogTemp, Log, TEXT("背包界面: 落位成功——物品 %lld → 容器 %lld 的 %d 号子网格 (%d,%d)%s"),
            ItemHandle, Container, Part, Target.X, Target.Y, bRotated ? TEXT(" 旋转") : TEXT(""));
        return 0;
    }

    // 占位失败：定义可旋转的话自动换向重试一次（窄缝里换个朝向常常就能塞下）。
    if (ItemView.Definition && ItemView.Definition->bRotatable)
    {
        const bool bRetryRotated = !bRotated;
        const FIntPoint RetryCells = FootprintCellsFor(ItemView.Definition, bRetryRotated);
        const FIntPoint RetryTarget = ClampCellToGrid(Cell, RetryCells, GridSize);
        const int32 RetryResult = Inventory->MoveItem(ItemHandle, Container, Part,
            RetryTarget.X, RetryTarget.Y, bRetryRotated);
        if (RetryResult == 0)
        {
            bAutoRotatedOnLastDrop = true;
            UE_LOG(LogTemp, Log,
                TEXT("背包界面: 落位 ( %d,%d)%s 失败（错误码 %d），自动旋转成%s后在 (%d,%d) 落位成功"),
                Target.X, Target.Y, bRotated ? TEXT(" 旋转") : TEXT(""), Result,
                bRetryRotated ? TEXT("旋转") : TEXT("不旋转"), RetryTarget.X, RetryTarget.Y);
            return 0;
        }
        UE_LOG(LogTemp, Log, TEXT("背包界面: 自动旋转重试也没落下去（错误码 %d），继续试交换"), RetryResult);
        Result = RetryResult;
    }

    // 目标格被别的物品占着：改调 SwapItems（同格已有物品时交换，而不是直接报错）。
    const int64 Occupant = SingleOccupantAt(Target, Cells, ItemHandle);
    if (Occupant > 0)
    {
        const int32 SwapResult = Inventory->SwapItems(ItemHandle, Occupant);
        if (SwapResult == 0)
        {
            UE_LOG(LogTemp, Log, TEXT("背包界面: 落位改成交换——物品 %lld 与占位物品 %lld 互换成功"),
                ItemHandle, Occupant);
            return 0;
        }
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 与占位物品 %lld 交换失败（错误码 %d）"), Occupant, SwapResult);
        Result = SwapResult;
    }

    UE_LOG(LogTemp, Warning,
        TEXT("背包界面: 落位失败（错误码 %d）——物品 %lld → 容器 %lld 的 %d 号子网格 (%d,%d)%s；物品位置保持不变"),
        Result, ItemHandle, Container, Part, Target.X, Target.Y, bRotated ? TEXT(" 旋转") : TEXT(""));
    return Result;
}

int32 UInvGridWidget::DropOnThisGrid(FIntPoint Cell, bool bRotated)
{
    if (!HasValidTarget())
    {
        LogInvalidConfigOnce(TEXT("落位"));
        return InvUiErrInvalidHandle;
    }
    if (!bDragging || DraggedItem <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 格子控件 %s 上没有正在拖拽的物品，落位无效"), *GetName());
        return InvUiErrInvalidHandle;
    }

    FInvItemView View;
    if (!FindItemView(DraggedItem, View))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 拖拽中的物品 %lld 不在本子网格，落位无效"), DraggedItem);
        ClearDragState();
        RequestRepaint();
        return InvUiErrInvalidHandle;
    }

    const int64 DroppedItem = DraggedItem;
    const int32 Result = PlaceItemIntoThisGrid(DroppedItem, View, Cell, bRotated);

    if (OwnerScreen)
    {
        // 落位结束：不管成功还是失败都广播 OnItemDropped（带错误码），失败另外再报一次 OnOperationFailed。
        OwnerScreen->NotifyItemDropped(View, Result);
    }

    if (Result == 0)
    {
        FinishDropAndRefresh();
    }
    else
    {
        ReportFailureToScreen(Result,
            FString::Printf(TEXT("落位失败（错误码 %d）——物品 %lld → 容器 %lld 的 %d 号子网格 (%d,%d)，位置保持不变"),
                Result, DroppedItem, Container, Part, Cell.X, Cell.Y));

        // 拖拽收尾，但保留选中：用户通常想按 R 换个朝向 / 挪个位置再来一次。
        ClearDragState();
        NotifyOwnerScreenFocused();
        RequestRepaint();
    }
    return Result;
}

// ==================== 跨容器拖 ====================

FVector2D UInvGridWidget::ExternalDragAnchorFromScreen(const UInvGridWidget* SourceGrid, FVector2D ScreenPosition) const
{
    // 屏幕坐标 → 本格子本地坐标；抓取偏移来自源格子，按两边几何的缩放（DPI）换算，
    // 所以「抓到物品的哪个位置」在跨容器拖时和源容器里完全一致。
    const FGeometry MyGeometry = GetCachedGeometry();
    const FGeometry SourceGeometry = SourceGrid ? SourceGrid->GetCachedGeometry() : FGeometry();
    const float MyScale = FMath::Max(MyGeometry.GetAccumulatedLayoutTransform().GetScale(), KINDA_SMALL_NUMBER);
    const float SourceScale = FMath::Max(SourceGeometry.GetAccumulatedLayoutTransform().GetScale(), KINDA_SMALL_NUMBER);
    const FVector2D GrabOffset = SourceGrid ? SourceGrid->DragGrabOffset : FVector2D::ZeroVector;

    return MyGeometry.AbsoluteToLocal(ScreenPosition) - GrabOffset * (SourceScale / MyScale);
}

void UInvGridWidget::PreviewExternalDrag(UInvGridWidget* SourceGrid, FVector2D ScreenPosition, bool bRotated)
{
    // 源格子没在拖（或者就是自己）→ 没有可预览的东西，顺手把旧预览清掉。
    if (!SourceGrid || SourceGrid == this || !SourceGrid->bDragging || SourceGrid->DraggedItem <= 0)
    {
        ClearExternalDragPreview();
        return;
    }

    FInvItemView SourceView;
    if (!SourceGrid->FindItemView(SourceGrid->DraggedItem, SourceView))
    {
        ClearExternalDragPreview();
        return;
    }

    ExternalDragSource = SourceGrid;
    bExternalDragRotated = bRotated;
    ExternalDragLocalPos = ExternalDragAnchorFromScreen(SourceGrid, ScreenPosition);

    // 落点解算与自己的拖拽完全共用一套：用预览朝向 + 占格表判断放不放得下。
    FInvItemView DropView = SourceView;
    DropView.bRotated = bRotated;
    bDropInGrid = ComputeDropTarget(DropView, ExternalDragLocalPos, EffectiveCellSize(), GridSize, DropCell);
    bDropAllowed = bDropInGrid
        && CanOccupy(DropCell, FootprintCells(DropView), SourceGrid->DraggedItem);

    RequestRepaint();
}

void UInvGridWidget::ClearExternalDragPreview()
{
    if (!ExternalDragSource)
    {
        return;
    }

    ExternalDragSource = nullptr;
    ExternalDragLocalPos = FVector2D::ZeroVector;
    bExternalDragRotated = false;
    bDropInGrid = false;
    bDropAllowed = false;
    DropCell = FIntPoint(0, 0);
    RequestRepaint();
}

int32 UInvGridWidget::DropDragFromOtherGrid(UInvGridWidget* SourceGrid, FVector2D ScreenPosition, bool bRotated)
{
    if (!SourceGrid || SourceGrid == this)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 跨容器落位的源格子无效（%s）"), *GetName());
        return InvUiErrInvalidHandle;
    }
    if (!SourceGrid->bDragging || SourceGrid->DraggedItem <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 跨容器落位时源格子 %s 没有正在拖拽的物品"), *SourceGrid->GetName());
        return InvUiErrInvalidHandle;
    }
    FInvItemView ItemView;
    if (!SourceGrid->FindItemView(SourceGrid->DraggedItem, ItemView))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 跨容器落位时源格子上找不到物品 %lld 的视图"), SourceGrid->DraggedItem);
        return InvUiErrInvalidHandle;
    }
    if (!HasValidTarget())
    {
        LogInvalidConfigOnce(TEXT("跨容器落位"));
        return InvUiErrInvalidHandle;
    }

    const int64 Item = SourceGrid->DraggedItem;  // 先把句柄与视图抄下来：源格子马上要被收尾

    // 用预览那套把屏幕坐标换算成本格子的落点。
    PreviewExternalDrag(SourceGrid, ScreenPosition, bRotated);
    const FIntPoint Target = DropCell;
    const bool bTargetInGrid = bDropInGrid;
    ClearExternalDragPreview();

    if (!bTargetInGrid)
    {
        // 放不下：也要把源格子的拖拽收尾，别让它卡在「拖拽中」。
        SourceGrid->ClearDragState();
        SourceGrid->Refresh();
        SourceGrid->RequestRepaint();
        UE_LOG(LogTemp, Warning,
            TEXT("背包界面: 跨容器落位失败——物品 %lld 的占位尺寸在容器 %lld 的 %d 号子网格里放不下"),
            Item, Container, Part);
        if (OwnerScreen)
        {
            OwnerScreen->NotifyItemDropped(ItemView, 1);  // 1 = OutOfBounds
            OwnerScreen->NotifyOperationFailed(1,
                FString::Printf(TEXT("跨容器落位失败——物品 %lld 的占位尺寸在容器 %lld 的 %d 号子网格里放不下"),
                    Item, Container, Part));
        }
        return 1;  // 1 = OutOfBounds
    }

    // 先落位，再刷新两边：这样源格子拉到的就是「东西已经不在我这里」的最终状态。
    const int32 Result = PlaceItemIntoThisGrid(Item, ItemView, Target, bRotated);

    // 物品已经换容器了：源格子的拖拽状态必须收尾（它那边也要重画一遍）。
    SourceGrid->ClearDragState();
    SourceGrid->Refresh();
    SourceGrid->RequestRepaint();
    Refresh();

    if (Result == 0)
    {
        UE_LOG(LogTemp, Log, TEXT("背包界面: 跨容器落位成功——物品 %lld 从格子 %s 落到容器 %lld 的 %d 号子网格 (%d,%d)%s"),
            Item, *SourceGrid->GetName(), Container, Part, Target.X, Target.Y,
            bRotated ? TEXT(" 旋转") : TEXT(""));
    }

    NotifyOwnerScreenFocused();
    if (OwnerScreen)
    {
        OwnerScreen->NotifyItemDropped(ItemView, Result);
        if (Result != 0)
        {
            OwnerScreen->NotifyOperationFailed(Result,
                FString::Printf(TEXT("跨容器落位失败（错误码 %d）——物品 %lld → 容器 %lld 的 %d 号子网格 (%d,%d)"),
                    Result, Item, Container, Part, Target.X, Target.Y));
        }
        OwnerScreen->NotifyInventoryChanged();  // 两边容器的子格子都刷一遍
    }
    return Result;
}

int32 UInvGridWidget::RotateDraggedOrSelected()
{
    if (bDragging)
    {
        // 拖拽中：只翻幽灵朝向，不动底层数据（落位那一刻才真正旋转）。
        bDragRotated = !bDragRotated;

        FInvItemView View;
        if (FindItemView(DraggedItem, View))
        {
            const FInvItemView DropView = MakeDropView(View);
            bDropInGrid = ComputeDropTarget(DropView, DragLocalPos, EffectiveCellSize(), GridSize, DropCell);
            bDropAllowed = bDropInGrid && CanOccupy(DropCell, FootprintCells(DropView), DraggedItem);
        }

        UE_LOG(LogTemp, Log, TEXT("背包界面: 拖拽幽灵换向——物品 %lld 现在%s"), DraggedItem,
            bDragRotated ? TEXT("旋转") : TEXT("不旋转"));
        RequestRepaint();
        return 0;
    }

    if (SelectedItem > 0 && Inventory)
    {
        const int32 Result = Inventory->RotateItem(SelectedItem);
        if (Result == 0)
        {
            UE_LOG(LogTemp, Log, TEXT("背包界面: 选中物品 %lld 原地旋转成功"), SelectedItem);
            Refresh();
            NotifyOwnerScreenFocused();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("背包界面: 选中物品 %lld 原地旋转失败（错误码 %d；4 = 该定义不可旋转）"),
                SelectedItem, Result);
        }
        return Result;
    }

    UE_LOG(LogTemp, Warning, TEXT("背包界面: 格子控件 %s 既没在拖拽也没有选中物品，旋转无效"), *GetName());
    return InvUiErrInvalidHandle;
}

void UInvGridWidget::ClearDragState()
{
    DraggedItem = 0;
    bDragging = false;
    bDragRotated = false;
    DropCell = FIntPoint(0, 0);
    bDropInGrid = false;
    bDropAllowed = false;
    DragGrabOffset = FVector2D::ZeroVector;
    // 注意：`bAutoRotatedOnLastDrop` **不在这里清**——它是「上一次落位的结果」，
    // 落位成功后调用方（测试 / 蓝图）要能读到它；下一次 BeginDragAtItem 或 CancelDrag 才会清掉。

    // 跨容器预览画在**别的**格子身上：拖拽一结束就得让它们把预览（高亮 + 幽灵）清掉。
    if (OwnerScreen)
    {
        OwnerScreen->ClearCrossGridDragPreview();
    }
}

void UInvGridWidget::CancelDrag()
{
    const int64 WasDragging = DraggedItem;
    const bool bWasDragging = bDragging;

    ClearDragState();
    SelectedItem = 0;  // 主动取消 = 全清（落位失败那条路径只收尾拖拽，保留选中）
    bAutoRotatedOnLastDrop = false;

    if (bWasDragging)
    {
        UE_LOG(LogTemp, Log, TEXT("背包界面: 取消拖拽（物品 %lld）"), WasDragging);
    }
    RequestRepaint();
}

void UInvGridWidget::ReportFailureToScreen(int32 Code, const FString& Message)
{
    if (OwnerScreen)
    {
        OwnerScreen->NotifyOperationFailed(Code, Message);
    }
}

FVector2D UInvGridWidget::ClampedIconPadding(const FVector2D& Size) const
{
    // 内缩最多到「还剩 1 像素」：配置写大了也只是图标变小，不会画出负尺寸 / 反矩形。
    const float MaxX = FMath::Max((Size.X - 1.f) * 0.5f, 0.f);
    const float MaxY = FMath::Max((Size.Y - 1.f) * 0.5f, 0.f);
    return FVector2D(FMath::Clamp(IconPadding.X, 0.f, MaxX), FMath::Clamp(IconPadding.Y, 0.f, MaxY));
}

// ==================== 右键菜单 / 修饰键 / 删除 ====================

bool UInvGridWidget::IsContextMenuOpen() const
{
    if (!OwnerScreen || !OwnerScreen->IsContextMenuVisible())
    {
        return false;
    }
    // 菜单开的是「本格子里的某件物品」才算本格子的菜单（别的格子开的菜单不该影响这里的点击判定）。
    FInvItemView View;
    return FindItemView(OwnerScreen->GetContextMenuItemHandle(), View);
}

bool UInvGridWidget::NotifyContextMenuRequested(int64 ItemHandle, FVector2D ScreenPosition)
{
    if (!OwnerScreen)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("背包界面: 格子控件 %s 没绑定界面控件，右键菜单跳过（单独 Create Widget 的格子不提供菜单）"),
            *GetName());
        return false;
    }

    // 同一个物品再次右键 = 收起菜单（右键是「开 / 关」的切换）。
    if (OwnerScreen->IsContextMenuVisible() && OwnerScreen->GetContextMenuItemHandle() == ItemHandle)
    {
        OwnerScreen->CloseContextMenu();
        return false;
    }

    OwnerScreen->RequestContextMenuForItem(ItemHandle, ScreenPosition);
    return OwnerScreen->IsContextMenuVisible();
}

int32 UInvGridWidget::SplitHalfItem(int64 Handle)
{
    if (!HasValidTarget())
    {
        LogInvalidConfigOnce(TEXT("拆分一半"));
        return InvUiErrInvalidHandle;
    }

    FInvItemView View;
    if (Handle <= 0 || !FindItemView(Handle, View))
    {
        const FString Message = FString::Printf(
            TEXT("格子控件 %s 上没有物品 %lld（不在容器 %lld 的 %d 号子网格），拆分一半无效"),
            *GetName(), Handle, Container, Part);
        UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
        ReportFailureToScreen(InvUiErrInvalidArgument, Message);
        return InvUiErrInvalidArgument;  // 22 = InvalidArgument：没有可拆的目标
    }

    // 「一半」= Stack / 2；堆叠少于 2 根本没有一半可拆（4 拆 2、3 拆 1、2 拆 1）。
    if (View.Stack < 2)
    {
        const FString Message = FString::Printf(TEXT("物品 %lld 只有 %d 件（可堆叠上限 %d），拆不出「一半」"),
            Handle, View.Stack, View.Definition ? View.Definition->MaxStack : 1);
        UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
        ReportFailureToScreen(InvUiErrInvalidStackCount, Message);
        return InvUiErrInvalidStackCount;  // 18 = InvalidStackCount
    }

    const int32 Count = View.Stack / 2;
    const UInvItemDefinition* Definition = View.Definition;
    const FIntPoint Size = Definition
        ? FIntPoint(FMath::Max(Definition->Width, 1), FMath::Max(Definition->Height, 1))
        : FIntPoint(1, 1);  // 定义缺失：按 1x1 试，别因为配置问题把整条交互卡死
    const bool bCanRotate = Definition && Definition->bRotatable;

    // 落点顺序：本子网格在前，然后是同容器的其它子网格（下标升序；当前那块跳过，不重复试）。
    TArray<int32> PartOrder;
    PartOrder.Add(Part);
    const TArray<FIntPoint> Parts = Inventory->GetContainerParts(Container);
    for (int32 Index = 0; Index < Parts.Num(); ++Index)
    {
        if (Index != Part)
        {
            PartOrder.Add(Index);
        }
    }

    for (const int32 TargetPart : PartOrder)
    {
        // 朝向：先保持物品当前朝向，定义可旋转时才追一次换向。
        const int32 RotationCount = bCanRotate ? 2 : 1;
        for (int32 Rotation = 0; Rotation < RotationCount; ++Rotation)
        {
            const bool bRotated = (Rotation == 0) ? View.bRotated : !View.bRotated;

            // 定址走组件层的 FindFreeCell：占格表为准，和拖拽落点判定同一套语义。
            FIntPoint Cell;
            if (!Inventory->FindFreeCell(Container, TargetPart, Size, bRotated, Cell))
            {
                continue;  // 这块子网格排不下（或者下标无效）：换下一处
            }

            const int32 Result = Inventory->SplitStack(Handle, Count, Container, TargetPart, Cell.X, Cell.Y, bRotated);
            if (Result == 0)
            {
                UE_LOG(LogTemp, Log,
                    TEXT("背包界面: 拆分一半成功——物品 %lld 拆出 %d 件，落到容器 %lld 的 %d 号子网格 (%d,%d)%s"),
                    Handle, Count, Container, TargetPart, Cell.X, Cell.Y, bRotated ? TEXT(" 旋转") : TEXT(""));
                FinishDropAndRefresh();  // 收尾拖拽 + 刷新自己 + 通知界面刷全部子格子
                return 0;
            }

            // 有空位却拆不动（黑名单 / 嵌套 / 参数不合）：把真实错误码报出去，别咽掉。
            const FString Message = FString::Printf(
                TEXT("拆分一半失败（物品 %lld → 容器 %lld 的 %d 号子网格 (%d,%d)，错误码 %d）"),
                Handle, Container, TargetPart, Cell.X, Cell.Y, Result);
            UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
            ReportFailureToScreen(Result, Message);
            return Result;
        }
    }

    const FString Message = FString::Printf(
        TEXT("拆分一半无处可放——物品 %lld 的 %dx%d 占位在容器 %lld 的所有子网格里都没有空位"),
        Handle, Size.X, Size.Y, Container);
    UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
    ReportFailureToScreen(InvUiErrOccupied, Message);
    return InvUiErrOccupied;  // 2 = Occupied
}

int32 UInvGridWidget::HandleModifiedClick(int64 ItemHandle, bool bCtrl)
{
    if (!bCtrl)
    {
        // 不是修饰键点击：当普通点击处理（只选中），不拆。
        SelectItem(ItemHandle);
        return 0;
    }
    return SplitHalfItem(ItemHandle);  // Ctrl + 左键 = 拆一半 + 自动落位
}

int32 UInvGridWidget::SplitHalfSelected()
{
    if (SelectedItem <= 0)
    {
        const FString Message = FString::Printf(TEXT("格子控件 %s 上没有选中的物品，拆分一半无效"), *GetName());
        UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
        ReportFailureToScreen(InvUiErrInvalidArgument, Message);
        return InvUiErrInvalidArgument;  // 22
    }
    return SplitHalfItem(SelectedItem);
}

int32 UInvGridWidget::HandleDeleteKey()
{
    if (!HasValidTarget())
    {
        LogInvalidConfigOnce(TEXT("删除键"));
        return InvUiErrInvalidHandle;
    }

    if (SelectedItem <= 0)
    {
        const FString Message = FString::Printf(TEXT("格子控件 %s 上没有选中的物品，删除键无效"), *GetName());
        UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
        ReportFailureToScreen(InvUiErrInvalidArgument, Message);
        return InvUiErrInvalidArgument;  // 22 = 没选中
    }

    FInvItemView View;
    if (!FindItemView(SelectedItem, View))
    {
        // 选中的东西已经不在本子网格（被别处挪走 / 删掉）：等同于「没选中」。
        const FString Message = FString::Printf(TEXT("选中的物品 %lld 已经不在容器 %lld 的 %d 号子网格，删除键无效"),
            SelectedItem, Container, Part);
        UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
        SelectedItem = 0;
        ReportFailureToScreen(InvUiErrInvalidArgument, Message);
        return InvUiErrInvalidArgument;  // 22
    }

    const int64 Item = SelectedItem;
    const int64 Removed = Inventory->DestroyItem(Item);  // 返回删除件数（含套包内容），< 0 = -错误码
    if (Removed < 0)
    {
        const int32 Code = (int32)(-Removed);
        const FString Message = FString::Printf(TEXT("删除物品 %lld 失败（错误码 %d）"), Item, Code);
        UE_LOG(LogTemp, Warning, TEXT("背包界面: %s"), *Message);
        ReportFailureToScreen(Code, Message);
        return Code;
    }

    UE_LOG(LogTemp, Log, TEXT("背包界面: 删除键删掉了物品 %lld 及其套包内容（共 %lld 件）"), Item, Removed);

    // 先广播（视图里还带着被删物品的信息），再收尾 / 刷新。
    if (OwnerScreen)
    {
        OwnerScreen->NotifyItemDestroyed(View);
    }

    SelectedItem = 0;
    ClearDragState();
    Refresh();
    if (OwnerScreen)
    {
        OwnerScreen->NotifyInventoryChanged();
    }
    return 0;
}

void UInvGridWidget::SelectItem(int64 ItemHandle)
{
    FInvItemView View;
    if (ItemHandle > 0 && FindItemView(ItemHandle, View))
    {
        SelectedItem = ItemHandle;
    }
    else
    {
        if (ItemHandle > 0)
        {
            UE_LOG(LogTemp, Log, TEXT("背包界面: 物品 %lld 不在本子网格（容器 %lld 的 %d 号），当作清空选择"),
                ItemHandle, Container, Part);
        }
        SelectedItem = 0;
    }

    NotifyOwnerScreenFocused();
    RequestRepaint();
}

void UInvGridWidget::ClearSelection()
{
    SelectedItem = 0;
    RequestRepaint();
}

void UInvGridWidget::FinishDropAndRefresh()
{
    ClearDragState();
    NotifyOwnerScreenFocused();
    Refresh();  // 自己先按新位置重画

    if (OwnerScreen)
    {
        // 跨容器拖动时别的格子也变了：让界面把全部子格子刷一遍。
        OwnerScreen->NotifyInventoryChanged();
    }
}

// ==================== 查询 ====================

bool UInvGridWidget::GetItemViewInGrid(int64 ItemHandle, FInvItemView& OutView) const
{
    return FindItemView(ItemHandle, OutView);
}

FString UInvGridWidget::GetGridTitle() const
{
    const FString Label = (Inventory && Container > 0) ? Inventory->GetContainerLabel(Container) : FString();
    return FString::Printf(TEXT("%s part%d (%dx%d)"),
        Label.IsEmpty() ? TEXT("容器") : *Label, Part, GridSize.X, GridSize.Y);
}

FVector2D UInvGridWidget::GetGridPixelSize() const
{
    const float Cell = EffectiveCellSize();
    return FVector2D(GridSize.X * Cell, GridSize.Y * Cell);
}

float UInvGridWidget::GetEffectiveCellSize() const
{
    return EffectiveCellSize();
}

int32 UInvGridWidget::GetStackBadgeFontSize() const
{
    // 字号的缩放口径只有界面控件那一份（`ScaledFontSize`）：基准按 64 的单格边长设计。
    return UInvInventoryScreenWidget::ScaledFontSize(InvUiBadgeFontBase, EffectiveCellSize());
}

int32 UInvGridWidget::GetTooltipFontSize() const
{
    return UInvInventoryScreenWidget::ScaledFontSize(InvUiTooltipFontBase, EffectiveCellSize());
}

FString UInvGridWidget::BuildTooltipText(int64 ItemHandle) const
{
    FInvItemView View;
    if (!FindItemView(ItemHandle, View))
    {
        return FString();
    }

    const UInvItemDefinition* Definition = View.Definition;
    const FString Display = InvUiDisplayNameOf(Definition);
    const int32 MaxStack = Definition ? FMath::Max(Definition->MaxStack, 1) : 1;
    const float UnitWeight = Definition ? Definition->Weight : 0.f;
    const int64 UnitValue = Definition ? static_cast<int64>(Definition->Value) : 0;
    const FIntPoint Footprint = FootprintCells(View);

    TArray<FString> Lines;
    Lines.Add(Display);
    if (Definition && !Definition->Description.IsEmpty())
    {
        Lines.Add(Definition->Description.ToString());
    }
    Lines.Add(FString::Printf(TEXT("尺寸 %dx%d%s"), Footprint.X, Footprint.Y,
        View.bRotated ? TEXT("（旋转）") : TEXT("")));
    Lines.Add(FString::Printf(TEXT("重量 %s kg（总计 %s kg）"),
        *InvUiFormatFloat(UnitWeight, 3), *InvUiFormatFloat(UnitWeight * View.Stack, 3)));
    Lines.Add(FString::Printf(TEXT("估值 %lld（总计 %lld）"),
        UnitValue, UnitValue * View.Stack));
    Lines.Add(FString::Printf(TEXT("堆叠 %d/%d"), View.Stack, MaxStack));

    return FString::Join(Lines, TEXT("\n"));
}

UInvInventoryScreenWidget* UInvGridWidget::GetOwnerScreen() const
{
    return OwnerScreen;
}

// ==================== 尺寸 ====================

void UInvGridWidget::ApplyDesiredSizeToRoot() const
{
    if (!RootSizeBox)
    {
        return;  // 还没构造（或者被蓝图换过根）：没有尺寸盒子可改
    }

    const FVector2D Size = GetGridPixelSize();
    RootSizeBox->SetWidthOverride(Size.X > 0.f ? Size.X : 1.f);
    RootSizeBox->SetHeightOverride(Size.Y > 0.f ? Size.Y : 1.f);
}

void UInvGridWidget::RequestRepaint() const
{
    // 没构造过（NewObject 出来还没 TakeWidget）时缓存控件为空，这里是空操作。
    // InvalidateLayoutAndVolatility 是基类的非 const 节点，控件本体可变（缓存的是 Slate 侧数据），
    // 这里只是「请求重画」，不改变本控件任何可见状态。
    const_cast<UInvGridWidget*>(this)->InvalidateLayoutAndVolatility();
}

// ==================== 绘制 ====================

FVector2D UInvGridWidget::FootprintPixels(const FInvItemView& View) const
{
    const FIntPoint Cells = FootprintCells(View);
    const float Cell = EffectiveCellSize();
    return FVector2D(Cells.X * Cell, Cells.Y * Cell);
}

void UInvGridWidget::DrawRect(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
    const FVector2D& Min, const FVector2D& Size, const FLinearColor& Color) const
{
    if (Size.X <= 0.f || Size.Y <= 0.f)
    {
        return;
    }
    FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
        Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Min)),
        FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")), ESlateDrawEffect::None, Color);
}

void UInvGridWidget::DrawBorder(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
    const FVector2D& Min, const FVector2D& Size, const FLinearColor& Color, float Thickness) const
{
    if (Size.X <= 0.f || Size.Y <= 0.f || Thickness <= 0.f)
    {
        return;
    }
    DrawRect(OutDrawElements, LayerId, Geometry, Min, FVector2D(Size.X, Thickness), Color);
    DrawRect(OutDrawElements, LayerId, Geometry, FVector2D(Min.X, Min.Y + Size.Y - Thickness),
        FVector2D(Size.X, Thickness), Color);
    DrawRect(OutDrawElements, LayerId, Geometry, Min, FVector2D(Thickness, Size.Y), Color);
    DrawRect(OutDrawElements, LayerId, Geometry, FVector2D(Min.X + Size.X - Thickness, Min.Y),
        FVector2D(Thickness, Size.Y), Color);
}

float UInvGridWidget::EstimateTextWidth(const FString& Text, int32 FontSize)
{
    // 不依赖 Slate 渲染器（无头 / 命令集里没有字体度量服务），按字符宽度估：
    // 中日韩按 2 个半角宽算，其它按 1 个，再乘系数。
    float Units = 0.f;
    for (const TCHAR Char : Text)
    {
        Units += InvUiIsWideChar(Char) ? 2.f : 1.f;
    }
    return Units * FontSize * InvUiGlyphWidthFactor;
}

void UInvGridWidget::DrawTextLine(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
    const FString& Text, const FVector2D& Position, int32 FontSize, const FLinearColor& Color,
    const FVector2D& Alignment) const
{
    if (Text.IsEmpty())
    {
        return;
    }

    const FVector2D Size(EstimateTextWidth(Text, FontSize), FontSize * 1.25f);
    const FVector2D Min(Position.X - Size.X * Alignment.X, Position.Y - Size.Y * Alignment.Y);

    FSlateDrawElement::MakeText(OutDrawElements, LayerId,
        Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Min)),
        Text, FCoreStyle::GetDefaultFontStyle("Regular", FontSize),
        ESlateDrawEffect::None, Color);
}

UTexture2D* UInvGridWidget::FindCachedIcon(const UInvItemDefinition* Definition) const
{
    if (!Definition)
    {
        return nullptr;
    }
    // 只读查找：TObjectPtr 键没法从 const 指针构造，这里去掉 const 只是为了查表，不写回任何东西。
    const TObjectPtr<UTexture2D>* Found = IconCache.Find(const_cast<UInvItemDefinition*>(Definition));
    return Found ? Found->Get() : nullptr;
}

void UInvGridWidget::DrawItem(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
    const FInvItemView& View, const FVector2D& Min, const FVector2D& Size, bool bRotated, float Alpha) const
{
    if (Size.X <= 0.f || Size.Y <= 0.f)
    {
        return;
    }

    const UInvItemDefinition* Definition = View.Definition;
    UTexture2D* Icon = FindCachedIcon(Definition);

    // 图标 / 占位块按 `IconPadding` 内缩：外框、选中高亮、堆叠角标仍然贴物品的整块占位，
    // 所以「留边距」只是视觉上的，占位语义（几格就是几格）一格不差。
    const FVector2D Pad = ClampedIconPadding(Size);
    const FVector2D InnerMin = Min + Pad;
    const FVector2D InnerSize = Size - Pad * 2.f;

    if (Icon)
    {
        // 图标：软引用已加载的贴图直接铺满（内缩后的）占位矩形。
        FSlateBrush Brush;
        Brush.SetResourceObject(Icon);
        Brush.ImageSize = InnerSize;
        Brush.DrawAs = ESlateBrushDrawType::Image;
        FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
            Geometry.ToPaintGeometry(InnerSize, FSlateLayoutTransform(InnerMin)),
            &Brush, ESlateDrawEffect::None, FLinearColor::White.CopyWithNewOpacity(Alpha));
    }
    else
    {
        // 零资产回退：底色 + 名字首字。颜色按定义名哈希固定，同一件东西每次同色。
        FLinearColor Back = InvUiPlaceholderColor(Definition);
        Back.A = Alpha;
        DrawRect(OutDrawElements, LayerId, Geometry, InnerMin, InnerSize, Back);

        const FString Initial = InvUiInitialOf(Definition);
        const int32 FontSize = FMath::Max(FMath::RoundToInt(FMath::Min(InnerSize.X, InnerSize.Y) * 0.5f), 10);
        DrawTextLine(OutDrawElements, LayerId, Geometry, Initial,
            InnerMin + InnerSize * 0.5f, FontSize, FLinearColor(1.f, 1.f, 1.f, Alpha), FVector2D(0.5f, 0.5f));
    }

    // 物品外框：选中用黄色，拖拽中的原件按 alpha 压暗。
    const bool bSelected = !bDragging && SelectedItem != 0 && SelectedItem == View.Handle;
    DrawBorder(OutDrawElements, LayerId, Geometry, Min, Size,
        bSelected ? InvUiSelectionColor.CopyWithNewOpacity(Alpha) : InvUiItemBorderColor.CopyWithNewOpacity(Alpha),
        bSelected ? 2.f : InvUiBorderThickness);

    // 堆叠角标：Stack > 1 才画（字号随单格边长缩放，不写死像素）。
    if (View.Stack > 1)
    {
        const int32 BadgeFont = GetStackBadgeFontSize();
        const FString StackText = FString::FromInt(View.Stack);
        const FVector2D BadgeSize(EstimateTextWidth(StackText, BadgeFont) + 8.f, BadgeFont * 1.5f);
        const FVector2D BadgeMin(Min.X + Size.X - BadgeSize.X, Min.Y + Size.Y - BadgeSize.Y);

        DrawRect(OutDrawElements, LayerId, Geometry, BadgeMin, BadgeSize,
            InvUiBadgeColor.CopyWithNewOpacity(0.7f * Alpha));
        DrawTextLine(OutDrawElements, LayerId, Geometry, StackText,
            BadgeMin + BadgeSize * 0.5f, BadgeFont,
            FLinearColor(1.f, 1.f, 1.f, Alpha), FVector2D(0.5f, 0.5f));
    }
}

void UInvGridWidget::DrawTooltip(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry) const
{
    if (HoveredItem <= 0 || bDragging)
    {
        return;
    }

    const FString Text = BuildTooltipText(HoveredItem);
    if (Text.IsEmpty())
    {
        return;
    }

    TArray<FString> Lines;
    Text.ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
    if (Lines.Num() == 0)
    {
        return;
    }

    // 多行文本按行拆开逐行画：不依赖 Slate 的换行布局，字号 / 行高都由自己定（字号随单格边长缩放）。
    const int32 TooltipFont = GetTooltipFontSize();
    const float LineHeight = TooltipFont * 1.35f;
    float TextWidth = 0.f;
    for (const FString& Line : Lines)
    {
        TextWidth = FMath::Max(TextWidth, EstimateTextWidth(Line, TooltipFont));
    }

    const FVector2D Padding(8.f, 6.f);
    const FVector2D BoxSize(TextWidth + Padding.X * 2.f, Lines.Num() * LineHeight + Padding.Y * 2.f);
    const FVector2D BoxMin = LastCursorLocal + FVector2D(16.f, 16.f);

    DrawRect(OutDrawElements, LayerId, Geometry, BoxMin, BoxSize, InvUiTooltipColor);
    DrawBorder(OutDrawElements, LayerId, Geometry, BoxMin, BoxSize, InvUiGridBorderColor, InvUiBorderThickness);

    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const FVector2D LinePos(BoxMin.X + Padding.X, BoxMin.Y + Padding.Y + Index * LineHeight);
        // 首行是物品名，用亮色；其余信息用次级色。
        DrawTextLine(OutDrawElements, LayerId, Geometry, Lines[Index], LinePos, TooltipFont,
            Index == 0 ? InvUiTextColor : InvUiSubTextColor);
    }
}

int32 UInvGridWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
    const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
    // 基类先跑蓝图里的 On Paint（原生控件一般是空实现），我们的自绘叠在它上面。
    const int32 BaseLayer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId,
        InWidgetStyle, bParentEnabled);
    const int32 FirstLayer = BaseLayer + 1;
    int32 MaxLayer = FirstLayer - 1;

    const float Cell = EffectiveCellSize();
    const FVector2D GridPixels = GetGridPixelSize();

    if (GridSize.X <= 0 || GridSize.Y <= 0 || Occupancy.Num() != GridSize.X * GridSize.Y)
    {
        // 配置不可用：画个红框 + 一行提示，比一片空白好排查（不刷屏，日志里也只有一次）。
        const FVector2D BoxSize(FMath::Max(GridPixels.X, Cell * 3.f), FMath::Max(GridPixels.Y, Cell));
        DrawRect(OutDrawElements, FirstLayer, AllottedGeometry, FVector2D::ZeroVector, BoxSize,
            FLinearColor(0.12f, 0.06f, 0.06f, 0.9f));
        DrawBorder(OutDrawElements, FirstLayer, AllottedGeometry, FVector2D::ZeroVector, BoxSize,
            InvUiInvalidColor, 2.f);
        DrawTextLine(OutDrawElements, FirstLayer + 1, AllottedGeometry,
            TEXT("容器 / 子网格无效"), FVector2D(6.f, 6.f), GetTooltipFontSize(), InvUiInvalidColor);
        return FirstLayer + 2;
    }

    // ---- 1. 格子底纹（棋盘格）+ 网格外框 ----
    for (int32 Y = 0; Y < GridSize.Y; ++Y)
    {
        for (int32 X = 0; X < GridSize.X; ++X)
        {
            const FVector2D Min(X * Cell, Y * Cell);
            DrawRect(OutDrawElements, FirstLayer, AllottedGeometry, Min, FVector2D(Cell, Cell),
                ((X + Y) % 2 == 0) ? InvUiCellColorA : InvUiCellColorB);
        }
    }
    DrawBorder(OutDrawElements, FirstLayer, AllottedGeometry, FVector2D::ZeroVector, GridPixels,
        InvUiGridBorderColor, InvUiBorderThickness);
    MaxLayer = FirstLayer;

    // 幽灵信息：自己的拖拽与「别的格子拖到我这里」的跨容器预览共用同一套画法。
    FInvItemView GhostView;
    bool bHasGhost = false;
    if (bDragging)
    {
        if (FindItemView(DraggedItem, GhostView))
        {
            GhostView = MakeDropView(GhostView);
            bHasGhost = true;
        }
    }
    else if (ExternalDragSource)
    {
        if (ExternalDragSource->FindItemView(ExternalDragSource->DraggedItem, GhostView))
        {
            GhostView.bRotated = bExternalDragRotated;
            bHasGhost = true;
        }
    }

    // ---- 2. 落点高亮（合法绿 / 非法红） ----
    if (bHasGhost && bDropInGrid)
    {
        const FIntPoint Cells = FootprintCells(GhostView);
        const FVector2D Min(DropCell.X * Cell, DropCell.Y * Cell);
        const FVector2D Size(Cells.X * Cell, Cells.Y * Cell);
        DrawRect(OutDrawElements, FirstLayer + 1, AllottedGeometry, Min, Size,
            bDropAllowed ? InvUiDropOkColor : InvUiDropBadColor);
        DrawBorder(OutDrawElements, FirstLayer + 1, AllottedGeometry, Min, Size,
            bDropAllowed ? InvUiSelectionColor : InvUiInvalidColor, 2.f);
        MaxLayer = FMath::Max(MaxLayer, FirstLayer + 1);
    }

    // ---- 3. 物品（拖拽中的那一件压暗，让幽灵显眼） ----
    for (const FInvItemView& View : Items)
    {
        const FIntPoint Cells = FootprintCells(View);
        const FVector2D Min(View.X * Cell, View.Y * Cell);
        const FVector2D Size(Cells.X * Cell, Cells.Y * Cell);
        const bool bIsDragged = bDragging && View.Handle == DraggedItem;
        DrawItem(OutDrawElements, FirstLayer + 2, AllottedGeometry, View, Min, Size, View.bRotated,
            bIsDragged ? 0.35f : 1.f);
    }
    MaxLayer = FMath::Max(MaxLayer, FirstLayer + 2);

    // ---- 4. 拖拽幽灵（半透明，贴着落点画，比跟像素更清楚会放到哪） ----
    if (bHasGhost)
    {
        const FIntPoint Cells = FootprintCells(GhostView);
        const FVector2D Snap = CellToLocalPosition(DropCell, Cell);
        DrawItem(OutDrawElements, FirstLayer + 3, AllottedGeometry, GhostView, Snap,
            FVector2D(Cells.X * Cell, Cells.Y * Cell), GhostView.bRotated, 0.7f);
        MaxLayer = FMath::Max(MaxLayer, FirstLayer + 3);
    }

    // ---- 5. tooltip（鼠标旁，只在不拖拽时） ----
    DrawTooltip(OutDrawElements, FirstLayer + 4, AllottedGeometry);

    return FMath::Max(MaxLayer, FirstLayer + 4) + 1;
}

// ==================== Slate 回调 ====================

TSharedRef<SWidget> UInvGridWidget::RebuildWidget()
{
    // 原生控件第一次构造时 WidgetTree 还是空的：基类的 RebuildWidget 里会建它，
    // 但我们要在它之前挂上根 SizeBox（否则期望尺寸是 0，放进自动尺寸槽位什么都看不到）。
    if (!WidgetTree)
    {
        Initialize();
    }

    if (WidgetTree && !WidgetTree->RootWidget)
    {
        RootSizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("GridRootSizeBox"));
        if (RootSizeBox)
        {
            ApplyDesiredSizeToRoot();
            WidgetTree->RootWidget = RootSizeBox;
        }
    }

    // 参数（ExposeOnSpawn）在这一刻已经就位：先按它拉一次数据，再交给基类构造 Slate 控件。
    Refresh();
    return Super::RebuildWidget();
}

void UInvGridWidget::NotifyOwnerScreenFocused()
{
    if (OwnerScreen)
    {
        OwnerScreen->NotifyGridFocused(this);
    }
}

FReply UInvGridWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
    const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
    LastCursorLocal = Local;
    bHasCursor = true;

    if (InMouseEvent.GetEffectingButton() == InvUiKeyRightMouse)
    {
        NotifyOwnerScreenFocused();

        if (!HasValidTarget())
        {
            LogInvalidConfigOnce(TEXT("右键按下"));
        }
        else
        {
            const FIntPoint Cell = LocalPositionToCell(Local, EffectiveCellSize(), GridSize);
            const int64 Item = GetItemAtCell(Cell);
            if (Item > 0)
            {
                // 右键 = 开 / 关右键菜单：同一个物品再来一次就收起来（切换），换一件物品就改开到它身上。
                NotifyContextMenuRequested(Item, InMouseEvent.GetScreenSpacePosition());
            }
            else if (OwnerScreen)
            {
                OwnerScreen->CloseContextMenu();  // 空白处右键：收起菜单
            }
        }

        SetFocus();
        return FReply::Handled();
    }

    if (InMouseEvent.GetEffectingButton() == InvUiKeyLeftMouse)
    {
        NotifyOwnerScreenFocused();

        // 菜单开着时的左键：先收菜单。点在空白格上就到此为止（不顺手开始拖拽）；
        // 点在物品上则继续走「选中 + 拖拽」，用不着点两次。
        bool bMenuClosedOnEmpty = false;
        if (OwnerScreen && OwnerScreen->IsContextMenuVisible())
        {
            OwnerScreen->CloseContextMenu();
            const FIntPoint MenuCell = LocalPositionToCell(Local, EffectiveCellSize(), GridSize);
            bMenuClosedOnEmpty = (GetItemAtCell(MenuCell) <= 0);
        }

        if (!HasValidTarget())
        {
            LogInvalidConfigOnce(TEXT("左键按下"));
        }
        else if (bMenuClosedOnEmpty)
        {
            // 只收菜单：不开始拖拽，也不动选中（用户点的就是空白处）。
            UE_LOG(LogTemp, Log, TEXT("背包界面: 左键点在空白格上，只收起了右键菜单"));
        }
        else
        {
            const FIntPoint Cell = LocalPositionToCell(Local, EffectiveCellSize(), GridSize);
            const int64 Item = GetItemAtCell(Cell);
            if (Item > 0)
            {
                if (InMouseEvent.IsControlDown())
                {
                    // Ctrl + 左键 = 拆一半并自动落位（不进拖拽；成功失败都在 SplitHalfItem 里收尾 / 记日志）。
                    HandleModifiedClick(Item, true);
                }
                else
                {
                    // 选中 + 开始拖：松开时按落点决定是移动、交换还是原地不动（原地不动 = 只选了它）。
                    const int64 Dragged = BeginDragAtItem(Item);
                    if (Dragged <= 0)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("背包界面: 按下时物品 %lld 没能进入拖拽（返回 %lld），只做了选中"),
                            Item, Dragged);
                    }
                }
            }
            else
            {
                ClearSelection();
            }
        }

        // R / Esc / Delete 需要键盘焦点；没有本地玩家的上下文（无头）时 SetFocus 是空操作，不会崩。
        SetFocus();

        if (const TSharedPtr<SWidget> SafeWidget = GetCachedWidget())
        {
            return FReply::Handled().CaptureMouse(SafeWidget.ToSharedRef());
        }
        return FReply::Handled();
    }

    return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UInvGridWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
    const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
    LastCursorLocal = Local;
    bHasCursor = true;

    if (bDragging)
    {
        // 拖动中经过哪个格子，就把它记成界面上的「当前格子」（「整理」按钮按它选容器）。
        NotifyOwnerScreenFocused();

        // 鼠标已经被本格子捕获：光标跑到**别的**格子上时收不到那边的事件，
        // 所以这里把屏幕坐标转播给界面，让光标下那一格画跨容器落点预览。
        if (OwnerScreen)
        {
            OwnerScreen->UpdateCrossGridDragPreview(this, InMouseEvent.GetScreenSpacePosition(), bDragRotated);
        }
    }

    // 拖拽中更新幽灵 / 落点；没拖拽只更新悬停（tooltip）。
    UpdateDragCursor(Local);
    return FReply::Handled();
}

FReply UInvGridWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
    if (InMouseEvent.GetEffectingButton() == InvUiKeyLeftMouse && bDragging)
    {
        // 松开前按当前位置重算一次：鼠标没动过（点一下）也能拿到正确的落点。
        const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
        UpdateDragCursor(Local);

        // 跨容器拖：鼠标被本格子捕获，但光标可能在**别的**格子上——交给那一格按屏幕坐标落位。
        if (OwnerScreen && OwnerScreen->RouteDropToGridUnderCursor(this, InMouseEvent.GetScreenSpacePosition(), bDragRotated))
        {
            if (const TSharedPtr<SWidget> SafeWidget = GetCachedWidget())
            {
                return FReply::Handled().ReleaseMouseCapture();
            }
            return FReply::Handled();
        }

        const FIntPoint TargetCell = DropCell;
        const bool bTargetRotated = bDragRotated;
        const int64 DroppedItem = DraggedItem;  // 落位会清掉拖拽状态，日志先把它抄下来
        const int32 Result = DropOnThisGrid(TargetCell, bTargetRotated);

        if (Result != 0)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("背包界面: 松开鼠标落位失败（错误码 %d）——物品 %lld（容器 %lld 的 %d 号子网格）"),
                Result, DroppedItem, Container, Part);
        }

        if (const TSharedPtr<SWidget> SafeWidget = GetCachedWidget())
        {
            return FReply::Handled().ReleaseMouseCapture();
        }
        return FReply::Handled();
    }

    return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
}

void UInvGridWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
    bHasCursor = false;
    if (!bDragging)
    {
        HoveredItem = 0;  // 拖拽中鼠标离开也要继续跟踪（鼠标已捕获），悬停信息清掉
    }
    RequestRepaint();
    Super::NativeOnMouseLeave(InMouseEvent);
}

FReply UInvGridWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
    const FKey Key = InKeyEvent.GetKey();

    if (Key == InvUiKeyRotate)
    {
        RotateDraggedOrSelected();
        return FReply::Handled();
    }
    if (Key == InvUiKeyDelete)
    {
        HandleDeleteKey();
        return FReply::Handled();
    }
    if (Key == InvUiKeyCancel)
    {
        // Esc：先收右键菜单（开着的话），再取消拖拽——两件事互不冲突。
        if (OwnerScreen && OwnerScreen->IsContextMenuVisible())
        {
            OwnerScreen->CloseContextMenu();
        }
        if (bDragging)
        {
            CancelDrag();
        }
        return FReply::Handled();
    }

    return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
