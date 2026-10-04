#include "ue_package_systemScreenWidget.h"

#include "Application/SlateApplicationBase.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"

namespace
{
    // 命名前缀统一成 InvUiScreen*：本模块开了 unity build，匿名命名空间里的名字会跟同批 .cpp 撞车。

    /** 错误码（与组件层 / Rust 侧一致）：句柄无效。 */
    constexpr int32 InvUiScreenErrInvalidHandle = 23;

    /** 错误码：参数非法（没有目标物品 / 动作不适用）。 */
    constexpr int32 InvUiScreenErrInvalidArgument = 22;

    /** 负重条的常态 / 超重填充色。 */
    const FLinearColor InvUiScreenFillNormal(0.22f, 0.72f, 0.36f, 1.f);
    const FLinearColor InvUiScreenFillOverloaded(0.92f, 0.24f, 0.22f, 1.f);

    /** 负重条高度（像素）。 */
    constexpr float InvUiScreenBarHeight = 22.f;

    /** 同一行的两块子网格之间的间距（像素）。 */
    constexpr float InvUiScreenGridSpacing = 10.f;

    /** 标题距离负重条上方的距离（像素）。 */
    constexpr float InvUiScreenTitleGap = 6.f;

    /** 参考内容：默认背包根容器（6×5 格）——`ComputeScreenLayout` 不知道实例内容时按它算整屏尺寸。 */
    constexpr int32 InvUiScreenReferenceCellsX = 6;
    constexpr int32 InvUiScreenReferenceCellsY = 5;

    /** 布局参数的默认值（属性配成非法值时退回它们，与头文件里的默认值保持一致）。 */
    constexpr float InvUiScreenDefaultDesiredCellSize = 64.f;
    constexpr float InvUiScreenDefaultMaxScreenFraction = 0.85f;
    constexpr float InvUiScreenDefaultMinCellSize = 24.f;

    /** 字号下限：缩放后的字号不低于 10 像素，容器标题不低于 14 像素（再小就看不清了）。 */
    constexpr int32 InvUiScreenMinFontSize = 10;
    constexpr int32 InvUiScreenMinTitleFontSize = 14;

    /** 面板内边距基准（像素，按单格边长缩放）。 */
    constexpr float InvUiScreenPanelPaddingBase = 8.f;

    /** 面板底板 / 标题栏 / 描边的配色与描边宽度（零资产：纯色 + 圆角，不依赖任何贴图）。 */
    const FLinearColor InvUiScreenPanelColor(0.05f, 0.06f, 0.09f, 1.f);   // alpha 由 BackgroundOpacity 给
    const FLinearColor InvUiScreenPanelOutline(0.34f, 0.40f, 0.50f, 1.f);
    const FLinearColor InvUiScreenTitleBarColor(1.f, 1.f, 1.f, 0.07f);    // 标题栏比面板略亮一点
    constexpr float InvUiScreenPanelOutlineWidth = 1.f;

    /** 面板控件的画布层级：底板 / 标题栏压在最下面（子控件与子格子默认层 = 0，会盖在它们上面）。 */
    constexpr int32 InvUiScreenPanelZOrder = -200;

    /** 主面板标题栏里负重条的宽度占比与上下限（像素）。 */
    constexpr float InvUiScreenWeightBarWidthRatio = 0.42f;
    constexpr float InvUiScreenWeightBarMinWidth = 160.f;
    constexpr float InvUiScreenWeightBarMaxWidth = 360.f;

    /** 标题栏 / 小标题 / 按钮挨着内容留的空隙（像素）。 */
    constexpr float InvUiScreenTitleBarBottomPadding = 8.f;

    /** 「整理」按钮高度基准与宽高比（基准 88×22 出现在 64 的单格边长下）。 */
    constexpr float InvUiScreenSortButtonHeightBase = 22.f;
    constexpr float InvUiScreenSortButtonMinHeight = 20.f;
    constexpr float InvUiScreenSortButtonAspect = 4.f;

    /** 缩放基准：所有像素尺寸 / 字号都按这个单格边长设计。 */
    float InvUiScreenScaled(const float Base, const float InCellSize)
    {
        const float Cell = InCellSize > 0.f ? InCellSize : InvUiScreenDefaultDesiredCellSize;
        return Base * Cell / InvUiScreenDefaultDesiredCellSize;
    }

    /**
     * 自适应缩放的解算核心（纯函数 `ComputeScreenLayout` 与实例共用）：
     * 每轴可用边长 = `(可用尺寸 - 固定部分) / 每格尺寸`，取两轴较小值，再夹到下限（**下限优先**）。
     */
    float InvUiScreenFittedCellSize(const FVector2D& FitSize, const FVector2D& GridUnits,
        const FVector2D& Chrome, const float DesiredCellSize, const float MinCellSize)
    {
        const float UnitsX = FMath::Max(GridUnits.X, KINDA_SMALL_NUMBER);
        const float UnitsY = FMath::Max(GridUnits.Y, KINDA_SMALL_NUMBER);
        const float FitX = (FitSize.X - Chrome.X) / UnitsX;
        const float FitY = (FitSize.Y - Chrome.Y) / UnitsY;
        return FMath::Max(FMath::Min3(DesiredCellSize, FitX, FitY), MinCellSize);
    }

    /** 容器面板的标题栏高度：属性值按单格边长缩放，同时保证放得下标题字号与内容（主面板是负重条）。 */
    float InvUiScreenTitleBarHeight(const float BaseHeight, const float InCellSize, const int32 TitleFont,
        const float ContentsHeight)
    {
        return FMath::Max3(InvUiScreenScaled(BaseHeight, InCellSize), (float)TitleFont + 8.f, ContentsHeight);
    }

    /** 子格子小标题的行高（跟着字号走；基准字号 16 与 `LabelFontBase` 一致）。 */
    float InvUiScreenCaptionHeight(const float InCellSize)
    {
        return (float)UInvInventoryScreenWidget::ScaledFontSize(16, InCellSize) + 6.f;
    }

    /** 负重条高度（按单格边长缩放，保证装得下条上的文本）。 */
    float InvUiScreenWeightBarHeight(const float InCellSize)
    {
        return FMath::Max(InvUiScreenScaled(InvUiScreenBarHeight, InCellSize), 18.f);
    }

    /** 「整理」按钮尺寸（按单格边长缩放；64 的单格边长下是 88×22，与老常量一致）。 */
    FVector2D InvUiScreenSortButtonSize(const float InCellSize)
    {
        const float Height = FMath::Max(InvUiScreenScaled(InvUiScreenSortButtonHeightBase, InCellSize),
            InvUiScreenSortButtonMinHeight);
        return FVector2D(Height * InvUiScreenSortButtonAspect, Height);
    }

    /** 主面板标题栏里负重条的宽度（面板宽的一部分，夹到上下限）。 */
    float InvUiScreenWeightBarWidth(const float PanelWidth)
    {
        return FMath::Clamp(PanelWidth * InvUiScreenWeightBarWidthRatio,
            InvUiScreenWeightBarMinWidth, InvUiScreenWeightBarMaxWidth);
    }

    /**
     * 零资产圆角笔刷：纯色填充 + 1px 描边，Slate 渲染时现画（不需要任何贴图资产）。
     * `FSlateRoundedBoxBrush` 只比 `FSlateBrush` 多几个构造函数，切回基类不丢数据。
     */
    FSlateBrush InvUiScreenRoundedBrush(const FLinearColor& Fill, const FLinearColor& Outline, const float Radius)
    {
        return FSlateRoundedBoxBrush(Fill, Radius, Outline, InvUiScreenPanelOutlineWidth);
    }

    /** 右键菜单：底板配色 / 内边距（像素）/ 上下留白。 */
    const FLinearColor InvUiScreenMenuColor(0.06f, 0.07f, 0.10f, 0.96f);
    constexpr float InvUiScreenMenuPadding = 4.f;

    /** 菜单按钮的显示文本。顺序与 `EInvContextAction` 一一对应（下标 = 枚举取值）。 */
    const TCHAR* InvUiScreenActionText(const EInvContextAction Action)
    {
        switch (Action)
        {
        case EInvContextAction::Rotate:            return TEXT("旋转");
        case EInvContextAction::SplitHalf:         return TEXT("拆分一半");
        case EInvContextAction::Use:               return TEXT("使用");
        case EInvContextAction::Destroy:           return TEXT("销毁");
        case EInvContextAction::AutosortContainer: return TEXT("整理容器");
        default:                                   return TEXT("未知动作");
        }
    }

    /** 物品显示名（空定义 / 没填名字给个可读的占位），失败说明里用。 */
    FString InvUiScreenDisplayNameOf(const UInvItemDefinition* Definition)
    {
        if (!Definition)
        {
            return FString(TEXT("未知物品"));
        }
        const FString Display = Definition->DisplayName.ToString();
        return Display.IsEmpty() ? Definition->GetName() : Display;
    }
}

// ==================== 构造 / 生命周期 ====================

UInvInventoryScreenWidget::UInvInventoryScreenWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 全部子控件都在 C++ 里造（零资产）：这里只保证控件自己能吃事件、能被看到。
    SetVisibility(ESlateVisibility::Visible);
}

TSharedRef<SWidget> UInvInventoryScreenWidget::RebuildWidget()
{
    // 先保证 UObject 层的控件树在（根画布 / 静态子控件），再交给基类去构造 Slate。
    EnsureContentBuilt();

    // ExposeOnSpawn 参数在这一刻已经就位：先把数据与子格子拉起来，再交给基类构造 Slate 控件。
    Refresh();
    return Super::RebuildWidget();
}

void UInvInventoryScreenWidget::EnsureContentBuilt()
{
    // 原生控件第一次构造时 WidgetTree 还是空的：基类的 RebuildWidget 会建它，
    // 但我们要在那之前挂根画布，所以先自己初始化一次（幂等，不碰 Slate）。
    if (!WidgetTree)
    {
        Initialize();
    }
    if (!WidgetTree || WidgetTree->RootWidget)
    {
        return;  // 已经建过（或者在蓝图里配了根控件）：不重复建
    }

    RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("BackpackRootCanvas"));
    if (!RootCanvas)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 建根画布失败，界面只会是空的"), *GetName());
        return;
    }

    WidgetTree->RootWidget = RootCanvas;
    BuildStaticContent();
}

bool UInvInventoryScreenWidget::EnsureConstructed()
{
    // 没有 Slate 应用时（无头命令集 / 专用服务器）不能构造 Slate 控件：
    // 构造路径里会走 FSlateApplicationBase::Get()，没有应用就直接断言。
    // 控件树（UObject 层）照样建起来，`Refresh` 之后一样能读到子格子 / 负重文本。
    EnsureContentBuilt();
    if (!FSlateApplicationBase::IsInitialized())
    {
        UE_LOG(LogTemp, Log,
            TEXT("背包界面: 界面 %s 跳过 Slate 构造——当前没有 Slate 应用（无头命令集 / 服务器）"), *GetName());
        return false;
    }

    // 返回值必须自己抓一手：没人持有时 Slate 根控件会当场被回收，GetCachedWidget 立刻变空。
    if (!GetCachedWidget().IsValid())
    {
        BuiltSlateRoot = TakeWidget();
    }
    return GetCachedWidget().IsValid();
}

void UInvInventoryScreenWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    BuiltSlateRoot.Reset();
    Super::ReleaseSlateResources(bReleaseChildren);
}

void UInvInventoryScreenWidget::NativeConstruct()
{
    Super::NativeConstruct();

    Refresh();

    if (RefreshIntervalSeconds > 0.f)
    {
        if (UWorld* World = GetWorld())
        {
            // 定时刷新：背包被别处改动（捡 / 丢 / 别的界面整理）时屏幕也会跟上。
            // 用 this 绑定：定时器管理器对 UObject 方法持有弱引用，控件先没的话回调不会再跑。
            World->GetTimerManager().SetTimer(RefreshTimerHandle, this,
                &UInvInventoryScreenWidget::OnRefreshTimer,
                FMath::Max(RefreshIntervalSeconds, MinRefreshInterval), true);
        }
    }
}

void UInvInventoryScreenWidget::NativeDestruct()
{
    // 对称清理：定时器 + 按钮委托。
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(RefreshTimerHandle);
    }
    if (SortButton)
    {
        SortButton->OnClicked.RemoveAll(this);
    }
    for (const TObjectPtr<UButton>& MenuButton : ContextMenuButtons)
    {
        if (MenuButton)
        {
            MenuButton->OnClicked.RemoveAll(this);
        }
    }

    Super::NativeDestruct();
}

void UInvInventoryScreenWidget::OnRefreshTimer()
{
    Refresh();
}

// ==================== 内容构建 ====================

void UInvInventoryScreenWidget::BuildStaticContent()
{
    if (!WidgetTree || !RootCanvas)
    {
        return;
    }

    // 字号都按当前单格边长算（构造这一刻拿不到自适应结果，`ApplyLayout` 里会再按实际尺寸刷一遍）。
    const float Cell = FontCellSize();
    const int32 LabelFont = ScaledFontSize(LabelFontBase, Cell);

    // 顶部说明：告诉用户能干什么（无资产，纯文本）。
    TitleLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ScreenTitle"));
    if (TitleLabel)
    {
        TitleLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
        TitleLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.90f, 0.93f, 1.f, 1.f)));
        TitleLabel->SetText(FText::FromString(TEXT("背包（拖拽移动 / R 旋转 / Esc 取消）")));
    }

    // 负重条 + 条上的文本：条画在下面，文本压在同一条上。
    WeightBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("WeightBar"));
    if (WeightBar)
    {
        WeightBar->SetPercent(0.f);
        WeightBar->SetFillColorAndOpacity(InvUiScreenFillNormal);
    }

    WeightLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("WeightLabel"));
    if (WeightLabel)
    {
        WeightLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
        WeightLabel->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 1.f)));
        WeightLabel->SetText(FText::FromString(GetWeightText()));
    }

    // 「整理」按钮：UButton + 里面的 UTextBlock，样式走引擎默认（零资产）。
    SortButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("SortButton"));
    if (SortButton)
    {
        SortButton->OnClicked.AddDynamic(this, &UInvInventoryScreenWidget::OnSortClicked);

        SortButtonLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SortButtonLabel"));
        if (SortButtonLabel)
        {
            SortButtonLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
            SortButtonLabel->SetText(FText::FromString(TEXT("整理")));
            SortButton->AddChild(SortButtonLabel);
        }
    }

    // 先按旧口径摆一次（`Origin` 起）：居中模式随后的 `ApplyLayout` 会按面板重排，
    // 而只调 `EnsureContentBuilt`（比如开右键菜单）的路径下也有个像样的位置。
    LayoutHeaderAtOrigin(Cell);

    // 右键菜单（底板 + 按钮）最后建：它会盖在格子上层，初始收起来。
    BuildContextMenu();
}

void UInvInventoryScreenWidget::BuildContextMenu()
{
    if (!WidgetTree || !RootCanvas)
    {
        return;
    }

    ContextMenuBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ContextMenuBorder"));
    if (!ContextMenuBorder)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 建右键菜单底板失败，右键不会有菜单"), *GetName());
        return;
    }

    ContextMenuBorder->SetBrushColor(InvUiScreenMenuColor);
    ContextMenuBorder->SetPadding(FMargin(InvUiScreenMenuPadding));
    ContextMenuBorder->SetVisibility(ESlateVisibility::Collapsed);

    ContextMenuBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ContextMenuBox"));
    if (ContextMenuBox)
    {
        // 按钮顺序 = 枚举顺序，下标就用枚举取值：可用性判定直接按下标查，不用维护第二张顺序表。
        for (int32 Index = 0; Index <= (int32)EInvContextAction::AutosortContainer; ++Index)
        {
            const EInvContextAction Action = (EInvContextAction)Index;

            UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(),
                *FString::Printf(TEXT("ContextMenuButton%d"), Index));
            if (!Button)
            {
                ContextMenuButtons.Add(nullptr);
                continue;
            }

            UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
                *FString::Printf(TEXT("ContextMenuLabel%d"), Index));
            if (Label)
            {
                Label->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ScaledFontSize(LabelFontBase, FontCellSize())));
                Label->SetText(FText::FromString(InvUiScreenActionText(Action)));
                Button->AddChild(Label);
            }

            // 一开始全部收起来：开菜单时按可用性放开。
            Button->SetVisibility(ESlateVisibility::Collapsed);
            ContextMenuBox->AddChildToVerticalBox(Button);
            ContextMenuButtons.Add(Button);

            // `UButton::OnClicked` 不带参数（认不出是哪个按钮），所以一个动作绑一个处理函数。
            switch (Action)
            {
            case EInvContextAction::Rotate:
                Button->OnClicked.AddDynamic(this, &UInvInventoryScreenWidget::OnContextRotateClicked);
                break;
            case EInvContextAction::SplitHalf:
                Button->OnClicked.AddDynamic(this, &UInvInventoryScreenWidget::OnContextSplitHalfClicked);
                break;
            case EInvContextAction::Use:
                Button->OnClicked.AddDynamic(this, &UInvInventoryScreenWidget::OnContextUseClicked);
                break;
            case EInvContextAction::Destroy:
                Button->OnClicked.AddDynamic(this, &UInvInventoryScreenWidget::OnContextDestroyClicked);
                break;
            case EInvContextAction::AutosortContainer:
                Button->OnClicked.AddDynamic(this, &UInvInventoryScreenWidget::OnContextAutosortClicked);
                break;
            default:
                break;
            }
        }

        ContextMenuBorder->SetContent(ContextMenuBox);
    }

    PlaceInCanvas(ContextMenuBorder, Origin, FVector2D(ContextMenuWidth, ContextMenuButtonHeight));
}

void UInvInventoryScreenWidget::PlaceInCanvas(UWidget* Widget, const FVector2D& Position, const FVector2D& Size,
    int32 ZOrder)
{
    if (!RootCanvas || !Widget)
    {
        return;
    }

    // 已经挂过就复用原槽位（重复 AddChild 会先摘再挂，位置信息白丢一遍）。
    UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
    if (!CanvasSlot)
    {
        CanvasSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(Widget));
    }
    if (!CanvasSlot)
    {
        return;
    }

    CanvasSlot->SetAutoSize(false);
    CanvasSlot->SetZOrder(ZOrder);
    CanvasSlot->SetPosition(Position);
    CanvasSlot->SetSize(Size);
}

UInvGridWidget* UInvInventoryScreenWidget::CreateGridWidget(int64 InContainer, int32 InPart, int32 AtIndex,
    UTextBlock*& OutCaption)
{
    OutCaption = nullptr;

    if (!WidgetTree || !RootCanvas)
    {
        return nullptr;
    }

    UInvGridWidget* Grid = WidgetTree->ConstructWidget<UInvGridWidget>(UInvGridWidget::StaticClass(),
        *FString::Printf(TEXT("Grid_%lld_%d"), InContainer, InPart));
    if (!Grid)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 建子格子失败（容器 %lld 的 %d 号子网格）"),
            *GetName(), InContainer, InPart);
        return nullptr;
    }

    // ExposeOnSpawn 那些字段是给蓝图 Create Widget 用的；C++ 里直接赋值等价。
    Grid->Inventory = Inventory;
    Grid->Container = InContainer;
    Grid->Part = InPart;
    Grid->CellSize = EffectiveCellSize;   // 自适应之后的实际单格边长（没算过时它是 0，格子自己会兜底）
    Grid->OwnerScreen = this;

    PlaceInCanvas(Grid, FVector2D(Origin.X, Origin.Y + InvUiScreenBarHeight + 14.f), FVector2D(1.f, 1.f));

    // 小标题（`弹挂 part2 (1x2)`）：跟格子一一对应，布局时一起摆。
    UTextBlock* Caption = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
        *FString::Printf(TEXT("GridCaption_%lld_%d"), InContainer, InPart));
    if (Caption)
    {
        Caption->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ScaledFontSize(LabelFontBase, FontCellSize())));
        Caption->SetColorAndOpacity(FSlateColor(FLinearColor(0.82f, 0.85f, 0.90f, 1.f)));
        PlaceInCanvas(Caption, FVector2D(Origin.X, Origin.Y + InvUiScreenBarHeight + 14.f),
            FVector2D(MinPanelWidth, CaptionHeight));
    }
    OutCaption = Caption;

    Grid->Refresh();  // 建好立刻拉一次数据（这时几何也已经就位）

    UE_LOG(LogTemp, Log, TEXT("背包界面: 界面 %s 建了第 %d 块子格子（容器 %lld 的 %d 号子网格）"),
        *GetName(), AtIndex, InContainer, InPart);
    return Grid;
}

// ==================== 刷新 ====================

void UInvInventoryScreenWidget::CollectDesiredGrids(TArray<FInvGridSlotKey>& OutGrids) const
{
    OutGrids.Reset();
    if (!Inventory)
    {
        return;
    }

    TSet<EInvContainerType> Handled;
    for (const EInvContainerType Type : ShowTypes)
    {
        if (Type == EInvContainerType::Nested)
        {
            continue;  // Nested 是套包内部用的：它不是根容器，没有「一个」句柄可取
        }
        if (Handled.Contains(Type))
        {
            continue;  // 同一种根容器写两次也只显示一遍
        }
        Handled.Add(Type);

        const int64 Container = Inventory->GetContainer(Type);
        if (Container <= 0)
        {
            continue;  // 0 = 没有这种根容器（负数是错误码）：跳过，「存在且非空」才建格子
        }

        const TArray<FIntPoint> Parts = Inventory->GetContainerParts(Container);
        for (int32 Part = 0; Part < Parts.Num(); ++Part)
        {
            FInvGridSlotKey Key;
            Key.Container = Container;
            Key.Part = Part;
            OutGrids.Add(Key);
        }
    }
}

UInvGridWidget* UInvInventoryScreenWidget::FindReusableGrid(int64 InContainer, int32 InPart) const
{
    for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
    {
        if (Grid && Grid->Container == InContainer && Grid->Part == InPart)
        {
            return Grid;
        }
    }
    return nullptr;
}

bool UInvInventoryScreenWidget::SyncGridWidgets(const TArray<FInvGridSlotKey>& Desired)
{
    if (!RootCanvas)
    {
        return false;
    }

    // 结构没变：只让每块子格子自己刷一遍（最常见的路径：定时器 / 落位成功）。
    if (GridSignature == Desired && GridWidgets.Num() == Desired.Num())
    {
        for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
        {
            if (Grid)
            {
                Grid->Inventory = Inventory;
                Grid->CellSize = EffectiveCellSize;   // 自适应之后的实际值（0 时格子自己回退默认）
                Grid->Refresh();
            }
        }
        return false;
    }

    // 结构变了（换数据源 / 根容器增减）：能对上的复用，对不上的隐藏摘链，缺的新建。
    // 不销毁旧控件：它可能正处在鼠标事件处理里（这里只做「隐藏 + 从画布摘掉」）。
    const TArray<TObjectPtr<UInvGridWidget>> OldGrids = GridWidgets;
    const TArray<TObjectPtr<UTextBlock>> OldCaptions = CaptionLabels;
    TArray<TObjectPtr<UInvGridWidget>> NewGrids;
    TArray<TObjectPtr<UTextBlock>> NewCaptions;
    TArray<bool> Consumed;
    Consumed.Init(false, OldGrids.Num());

    for (const FInvGridSlotKey& Key : Desired)
    {
        int32 ReuseIndex = INDEX_NONE;
        for (int32 Index = 0; Index < OldGrids.Num(); ++Index)
        {
            if (!Consumed[Index] && OldGrids[Index]
                && OldGrids[Index]->Container == Key.Container && OldGrids[Index]->Part == Key.Part)
            {
                ReuseIndex = Index;
                break;
            }
        }

        if (ReuseIndex != INDEX_NONE)
        {
            Consumed[ReuseIndex] = true;
            UInvGridWidget* Grid = OldGrids[ReuseIndex];
            UTextBlock* Caption = OldCaptions.IsValidIndex(ReuseIndex) ? OldCaptions[ReuseIndex].Get() : nullptr;

            Grid->SetVisibility(ESlateVisibility::Visible);
            Grid->Inventory = Inventory;
            Grid->CellSize = EffectiveCellSize;   // 自适应之后的实际值（0 时格子自己回退默认）
            if (Caption)
            {
                Caption->SetVisibility(ESlateVisibility::Visible);
            }

            NewGrids.Add(Grid);
            NewCaptions.Add(Caption);
        }
        else
        {
            UTextBlock* NewCaption = nullptr;
            UInvGridWidget* NewGrid = CreateGridWidget(Key.Container, Key.Part, NewGrids.Num(), NewCaption);
            NewGrids.Add(NewGrid);
            NewCaptions.Add(NewCaption);
        }
    }

    // 多余的格子：藏起来并从画布摘掉（成员数组马上会被替换成新的一批）。
    for (int32 Index = 0; Index < OldGrids.Num(); ++Index)
    {
        if (Consumed.IsValidIndex(Index) && Consumed[Index])
        {
            continue;
        }
        if (UInvGridWidget* Stale = OldGrids[Index])
        {
            if (Stale->bDragging)
            {
                Stale->CancelDrag();
            }
            Stale->SetVisibility(ESlateVisibility::Collapsed);
            RootCanvas->RemoveChild(Stale);
        }
        if (OldCaptions.IsValidIndex(Index) && OldCaptions[Index])
        {
            OldCaptions[Index]->SetVisibility(ESlateVisibility::Collapsed);
            RootCanvas->RemoveChild(OldCaptions[Index]);
        }
    }

    GridWidgets = NewGrids;
    CaptionLabels = NewCaptions;
    GridSignature = Desired;

    // 焦点格子被摘了就清掉悬停记录（「整理」会退回背包）。
    if (FocusedGrid && !FindReusableGrid(FocusedGrid->Container, FocusedGrid->Part))
    {
        FocusedGrid = nullptr;
    }

    return true;
}

void UInvInventoryScreenWidget::LayoutGrids()
{
    if (!RootCanvas)
    {
        return;
    }

    // 行 = 一个容器：同一个容器的多块子网格并排一行（小标题在各自格子上方），容器之间换行。
    float RowTop = Origin.Y + InvUiScreenBarHeight + 14.f;
    int32 Index = 0;

    while (Index < GridWidgets.Num())
    {
        UInvGridWidget* Grid = GridWidgets[Index];
        if (!Grid || Grid->GetVisibility() != ESlateVisibility::Visible)
        {
            ++Index;
            continue;
        }

        const int64 RowContainer = Grid->Container;

        // 收集这一行（同一容器的连续一段）。
        TArray<int32> RowIndices;
        float RowWidth = 0.f;
        float RowGridHeight = 0.f;
        for (int32 Cursor = Index; Cursor < GridWidgets.Num(); ++Cursor)
        {
            UInvGridWidget* Candidate = GridWidgets[Cursor];
            if (!Candidate || Candidate->GetVisibility() != ESlateVisibility::Visible
                || Candidate->Container != RowContainer)
            {
                break;
            }
            RowIndices.Add(Cursor);
            const FVector2D Size = Candidate->GetGridPixelSize();
            RowWidth += Size.X + (RowIndices.Num() > 1 ? InvUiScreenGridSpacing : 0.f);
            RowGridHeight = FMath::Max(RowGridHeight, Size.Y);
        }

        float CursorX = Origin.X;
        for (const int32 RowIndex : RowIndices)
        {
            UInvGridWidget* RowGrid = GridWidgets[RowIndex];
            const FVector2D GridSize = RowGrid->GetGridPixelSize();

            if (UTextBlock* Caption = CaptionLabels.IsValidIndex(RowIndex) ? CaptionLabels[RowIndex].Get() : nullptr)
            {
                Caption->SetText(FText::FromString(RowGrid->GetGridTitle()));
                PlaceInCanvas(Caption, FVector2D(CursorX, RowTop), FVector2D(FMath::Max(GridSize.X, 64.f), CaptionHeight));
            }

            PlaceInCanvas(RowGrid, FVector2D(CursorX, RowTop + CaptionHeight), GridSize);
            CursorX += GridSize.X + InvUiScreenGridSpacing;
        }

        RowTop += CaptionHeight + RowGridHeight + RowSpacing;
        Index = RowIndices.Num() > 0 ? RowIndices.Last() + 1 : Index + 1;
    }
}

void UInvInventoryScreenWidget::UpdateWeightDisplay()
{
    if (WeightLabel)
    {
        WeightLabel->SetText(FText::FromString(GetWeightText()));
    }

    if (WeightBar)
    {
        WeightBar->SetPercent(FMath::Clamp(GetWeightRatio(), 0.f, 1.f));
        WeightBar->SetFillColorAndOpacity(IsOverloaded() ? InvUiScreenFillOverloaded : InvUiScreenFillNormal);
    }
}

// ==================== 布局：纯函数 ====================

int32 UInvInventoryScreenWidget::ScaledFontSize(int32 BaseFontSize, float InCellSize)
{
    const float Cell = InCellSize > 0.f ? InCellSize : InvUiScreenDefaultDesiredCellSize;
    const float Scale = Cell / InvUiScreenDefaultDesiredCellSize;
    return FMath::Max(FMath::RoundToInt(BaseFontSize * Scale), InvUiScreenMinFontSize);
}

int32 UInvInventoryScreenWidget::TitleFontSize(float InCellSize)
{
    const float Cell = InCellSize > 0.f ? InCellSize : InvUiScreenDefaultDesiredCellSize;
    return FMath::Max(FMath::RoundToInt(Cell * 0.4f), InvUiScreenMinTitleFontSize);
}

FInvScreenLayout UInvInventoryScreenWidget::ComputeScreenLayout(FVector2D ViewportSize, float InDesiredCellSize,
    float InMaxScreenFraction, float InMinCellSize, FVector2D InPanelSpacing)
{
    FInvScreenLayout Layout;

    // 参数兜底：非法值一律退回默认，保证结果恒为正、可预期。
    const float MinCell = InMinCellSize > 0.f ? InMinCellSize : InvUiScreenDefaultMinCellSize;
    const float Fraction = InMaxScreenFraction > 0.f
        ? FMath::Min(InMaxScreenFraction, 1.f)
        : InvUiScreenDefaultMaxScreenFraction;
    const float Desired = InDesiredCellSize > 0.f ? InDesiredCellSize : MinCell;
    const FVector2D Spacing(FMath::Max(InPanelSpacing.X, 0.f), FMath::Max(InPanelSpacing.Y, 0.f));
    const FVector2D Viewport(FMath::Max(ViewportSize.X, 0.f), FMath::Max(ViewportSize.Y, 0.f));

    // 参考内容 = 默认背包 6×5 格；固定部分 = 面板四周留白（两倍 PanelSpacing）——实例走同一套规则。
    const FVector2D Cells(InvUiScreenReferenceCellsX, InvUiScreenReferenceCellsY);
    const FVector2D Chrome = Spacing * 2.f;

    Layout.CellSize = InvUiScreenFittedCellSize(Viewport * Fraction, Cells, Chrome, Desired, MinCell);
    Layout.TotalSize = Cells * Layout.CellSize + Chrome;
    Layout.PanelOrigin = (Viewport - Layout.TotalSize) * 0.5f;
    return Layout;
}

TArray<UImage*> UInvInventoryScreenWidget::GetPanelBoards() const
{
    TArray<UImage*> Result;
    Result.Reserve(PanelBoards.Num());
    for (const TObjectPtr<UImage>& Board : PanelBoards)
    {
        Result.Add(Board.Get());
    }
    return Result;
}

TArray<int64> UInvInventoryScreenWidget::GetPanelContainers() const
{
    return PanelContainers;
}

// ==================== 布局：实例 ====================

float UInvInventoryScreenWidget::ManualCellSize() const
{
    return CellSize > 0.f ? CellSize : DesiredCellSize;
}

float UInvInventoryScreenWidget::FontCellSize() const
{
    return EffectiveCellSize > 0.f ? EffectiveCellSize : ManualCellSize();
}

FVector2D UInvInventoryScreenWidget::EffectivePanelSpacing() const
{
    return FVector2D(FMath::Max(PanelSpacing.X, 0.f), FMath::Max(PanelSpacing.Y, 0.f));
}

float UInvInventoryScreenWidget::EffectiveMaxScreenFraction() const
{
    return MaxScreenFraction > 0.f ? FMath::Min(MaxScreenFraction, 1.f) : InvUiScreenDefaultMaxScreenFraction;
}

float UInvInventoryScreenWidget::EffectiveMinCellSize() const
{
    return MinCellSize > 0.f ? MinCellSize : InvUiScreenDefaultMinCellSize;
}

FVector2D UInvInventoryScreenWidget::ResolveViewportSize() const
{
    // 优先用本控件自己的几何：它就是「整屏」的实际尺寸（Slate 单位，与子控件坐标同一套）。
    const FVector2D LocalSize = GetCachedGeometry().GetLocalSize();
    if (LocalSize.X > 0.f && LocalSize.Y > 0.f)
    {
        return LocalSize;
    }

    // 几何还没算出来：只有在真的上屏了（Slate 控件已经构造出来）时才相信游戏 viewport 的像素尺寸。
    // 无头命令集 / 专用服务器里没有 Slate：尺寸按「未知」处理，布局退回确定性口径（不居中、不缩放）。
    if (!GetCachedWidget().IsValid())
    {
        return FVector2D::ZeroVector;
    }

    if (const UWorld* World = GetWorld())
    {
        if (const UGameViewportClient* GameViewport = World->GetGameViewport())
        {
            if (GameViewport->Viewport)
            {
                const FIntPoint Size = GameViewport->Viewport->GetSizeXY();
                return FVector2D(Size.X, Size.Y);
            }
        }
    }
    return FVector2D::ZeroVector;
}

void UInvInventoryScreenWidget::UpdateEffectiveCellSize()
{
    const float Manual = ManualCellSize();

    if (!bCenterOnViewport)
    {
        EffectiveCellSize = Manual;   // 兼容模式：手动值原样用，不缩放
        return;
    }

    const FVector2D Viewport = ResolveViewportSize();
    if (!Inventory || Viewport.X <= 0.f || Viewport.Y <= 0.f)
    {
        // 没数据 / 拿不到 viewport 尺寸（没上屏 / 无头命令集 / 专用服务器）：不缩放，按手动值走。
        EffectiveCellSize = Manual;
        return;
    }

    // 两档实测分离「固定部分」与「每格尺寸」：面板尺寸 = 固定部分 + 格数 × 单格边长，
    // 所以 S(1) 与 S(2) 两个采样就能解出这两项（不靠把内边距 / 标题栏高度拼成常数）。
    const FVector2D Spacing = EffectivePanelSpacing();
    const FVector2D OneCell = MeasureClusterSize(1.f);
    const FVector2D TwoCells = MeasureClusterSize(2.f);
    const FVector2D GridUnits(FMath::Max(TwoCells.X - OneCell.X, 0.f), FMath::Max(TwoCells.Y - OneCell.Y, 0.f));
    const FVector2D Chrome = OneCell * 2.f - TwoCells + Spacing * 2.f;

    const float FitX = Viewport.X * EffectiveMaxScreenFraction();
    const float FitY = Viewport.Y * EffectiveMaxScreenFraction();
    const float MinCell = EffectiveMinCellSize();
    const float Desired = DesiredCellSize > 0.f ? DesiredCellSize : Manual;

    float Cell = InvUiScreenFittedCellSize(FVector2D(FitX, FitY), GridUnits, Chrome, Desired, MinCell);

    // 字号 / 标题栏高度是四舍五入的，实测可能比线性估计大一点点：超了按比例再缩（最多 3 轮）。
    for (int32 Pass = 0; Pass < 3 && Cell > MinCell; ++Pass)
    {
        const FVector2D Total = MeasureClusterSize(Cell) + Spacing * 2.f;
        if (Total.X <= FitX + 0.5f && Total.Y <= FitY + 0.5f)
        {
            break;
        }
        const float Scale = FMath::Min(FitX / FMath::Max(Total.X, 1.f), FitY / FMath::Max(Total.Y, 1.f));
        Cell = FMath::Max(MinCell, Cell * Scale * 0.995f);
    }

    if (!FMath::IsNearlyEqual(Cell, EffectiveCellSize))
    {
        UE_LOG(LogTemp, Log,
            TEXT("背包界面: 界面 %s 自适应单格边长 %.1f → %.1f（viewport %.0fx%.0f / 上限比例 %.2f / 下限 %.1f）"),
            *GetName(), EffectiveCellSize, Cell, Viewport.X, Viewport.Y, EffectiveMaxScreenFraction(), MinCell);
    }
    EffectiveCellSize = Cell;
}

void UInvInventoryScreenWidget::CollectGridsOfContainer(int64 InContainer, TArray<UInvGridWidget*>& OutGrids) const
{
    OutGrids.Reset();
    for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
    {
        if (Grid && Grid->GetVisibility() == ESlateVisibility::Visible && Grid->Container == InContainer)
        {
            OutGrids.Add(Grid);
        }
    }
}

void UInvInventoryScreenWidget::BuildPanelRects(float InCellSize, TArray<FInvScreenPanelRect>& OutPanels) const
{
    OutPanels.Reset();
    if (!Inventory)
    {
        return;
    }

    const float Cell = FMath::Max(InCellSize, 0.f);
    const FVector2D Spacing = EffectivePanelSpacing();
    const float Padding = InvUiScreenScaled(InvUiScreenPanelPaddingBase, Cell);
    const float CaptionH = InvUiScreenCaptionHeight(Cell);

    /** 面板 + 它里面子格子区域的总尺寸（`GridsSize` 是排布用的外框，不是格子本身的和）。 */
    struct FLocalPanelSpec
    {
        int64 Container = 0;
        EInvScreenPanelSlot Slot = EInvScreenPanelSlot::Extra;
        FVector2D GridsSize = FVector2D::ZeroVector;
        FVector2D Size = FVector2D::ZeroVector;
        FVector2D Position = FVector2D::ZeroVector;
    };

    // 1) `ShowTypes` 里存在的根容器（顺序确定，与子格子的收集口径一致）。
    TArray<int64> Containers;
    TSet<EInvContainerType> Handled;
    for (const EInvContainerType Type : ShowTypes)
    {
        if (Type == EInvContainerType::Nested || Handled.Contains(Type))
        {
            continue;
        }
        Handled.Add(Type);

        const int64 Container = Inventory->GetContainer(Type);
        if (Container > 0)
        {
            Containers.Add(Container);
        }
    }

    // 2) 槽位：主面板 = 背包（显示里没有背包时退到第一个存在的容器），其余按类型贴上去。
    const int64 Backpack = Inventory->GetContainer(EInvContainerType::Backpack);
    const int64 MainContainer = Containers.Contains(Backpack)
        ? Backpack
        : (Containers.Num() > 0 ? Containers[0] : 0);

    TArray<FLocalPanelSpec> Specs;
    TSet<int64> Added;
    const auto AddPanel = [&](const int64 Container, const EInvScreenPanelSlot Slot)
    {
        if (Container <= 0 || Added.Contains(Container))
        {
            return;
        }

        const TArray<FIntPoint> Parts = Inventory->GetContainerParts(Container);
        if (Parts.Num() <= 0)
        {
            return;   // 容器还在但一块子网格都没有（理论上不会）：不画面板
        }

        FLocalPanelSpec Spec;
        Spec.Container = Container;
        Spec.Slot = Slot;
        for (int32 Part = 0; Part < Parts.Num(); ++Part)
        {
            const FVector2D PartSize(Parts[Part].X * Cell, Parts[Part].Y * Cell);
            Spec.GridsSize.X += PartSize.X + (Part > 0 ? InvUiScreenGridSpacing : 0.f);
            Spec.GridsSize.Y = FMath::Max(Spec.GridsSize.Y, PartSize.Y);
        }

        Added.Add(Container);
        Specs.Add(Spec);
    };

    AddPanel(MainContainer, EInvScreenPanelSlot::Main);
    AddPanel(Inventory->GetContainer(EInvContainerType::ChestRig), EInvScreenPanelSlot::Left);
    AddPanel(Inventory->GetContainer(EInvContainerType::SafeBox), EInvScreenPanelSlot::Right);
    AddPanel(Inventory->GetContainer(EInvContainerType::Pockets), EInvScreenPanelSlot::Bottom);
    for (const int64 Container : Containers)
    {
        AddPanel(Container, EInvScreenPanelSlot::Extra);   // 认不出槽位的容器继续往下堆
    }

    // 3) 面板尺寸：内边距 + 标题栏 + 小标题行 + 子格子区域（主面板多一条放「整理」按钮的底边）。
    for (FLocalPanelSpec& Spec : Specs)
    {
        const bool bMain = Spec.Slot == EInvScreenPanelSlot::Main;
        const float TitleContents = bMain
            ? InvUiScreenWeightBarHeight(Cell) + InvUiScreenTitleBarBottomPadding
            : 0.f;
        const float TitleBarH = InvUiScreenTitleBarHeight(TitleHeight, Cell, TitleFontSize(Cell), TitleContents);
        const float FooterH = bMain
            ? InvUiScreenSortButtonSize(Cell).Y + InvUiScreenTitleBarBottomPadding
            : 0.f;

        Spec.Size = FVector2D(
            Padding * 2.f + Spec.GridsSize.X,
            Padding * 2.f + TitleBarH + CaptionH + Spec.GridsSize.Y + FooterH);
    }

    // 4) 相对位置：主面板中心 = 原点，其余贴上去（左 / 右竖直居中于主面板，下方一行居中）。
    const FLocalPanelSpec* Main = Specs.FindByPredicate([](const FLocalPanelSpec& Spec)
    {
        return Spec.Slot == EInvScreenPanelSlot::Main;
    });
    if (!Main)
    {
        return;   // 一块主面板都没有（没有任何根容器）：调用方按「没有面板」处理
    }
    const FVector2D MainSize = Main->Size;

    float LeftCursor = 0.f;
    float RightCursor = 0.f;
    float BottomCursor = 0.f;
    float ExtraCursor = 0.f;
    for (FLocalPanelSpec& Spec : Specs)
    {
        switch (Spec.Slot)
        {
        case EInvScreenPanelSlot::Main:
            Spec.Position = -Spec.Size * 0.5f;
            break;

        case EInvScreenPanelSlot::Left:
            Spec.Position = FVector2D(
                -(MainSize.X * 0.5f + Spacing.X + Spec.Size.X),
                LeftCursor - Spec.Size.Y * 0.5f);
            LeftCursor += Spec.Size.Y + Spacing.Y;
            break;

        case EInvScreenPanelSlot::Right:
            Spec.Position = FVector2D(
                MainSize.X * 0.5f + Spacing.X,
                RightCursor - Spec.Size.Y * 0.5f);
            RightCursor += Spec.Size.Y + Spacing.Y;
            break;

        case EInvScreenPanelSlot::Bottom:
            Spec.Position = FVector2D(
                BottomCursor - Spec.Size.X * 0.5f,
                MainSize.Y * 0.5f + Spacing.Y);
            BottomCursor += Spec.Size.X + Spacing.X;
            break;

        default:
            Spec.Position = FVector2D(
                -Spec.Size.X * 0.5f,
                MainSize.Y * 0.5f + Spacing.Y + ExtraCursor);
            ExtraCursor += Spec.Size.Y + Spacing.Y;
            break;
        }
    }

    OutPanels.Reserve(Specs.Num());
    for (const FLocalPanelSpec& Spec : Specs)
    {
        FInvScreenPanelRect Rect;
        Rect.Container = Spec.Container;
        Rect.Slot = Spec.Slot;
        Rect.Size = Spec.Size;
        Rect.Position = Spec.Position;
        OutPanels.Add(Rect);
    }
}

FVector2D UInvInventoryScreenWidget::MeasureClusterSize(float InCellSize) const
{
    TArray<FInvScreenPanelRect> Panels;
    BuildPanelRects(InCellSize, Panels);

    // 主面板中心 = 原点：簇按它对称展开，所以主面板一定落在整屏正中间（左右面板宽度不同也不跑偏）。
    FVector2D Half = FVector2D::ZeroVector;
    for (const FInvScreenPanelRect& Panel : Panels)
    {
        Half.X = FMath::Max(Half.X, FMath::Max(-Panel.Position.X, Panel.Position.X + Panel.Size.X));
        Half.Y = FMath::Max(Half.Y, FMath::Max(-Panel.Position.Y, Panel.Position.Y + Panel.Size.Y));
    }
    return Half * 2.f;
}

void UInvInventoryScreenWidget::SyncPanelDecor(int32 Needed)
{
    if (!WidgetTree || !RootCanvas)
    {
        return;
    }

    // 缺的补：底板（圆角 + 描边）/ 标题栏底条 / 标题文本，三块一组，下标 = 面板下标。
    while (PanelBoards.Num() < Needed)
    {
        const int32 Index = PanelBoards.Num();
        const float Radius = InvUiScreenScaled(PanelCornerRadiusBase, FontCellSize());

        UImage* Board = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(),
            *FString::Printf(TEXT("PanelBoard%d"), Index));
        if (Board)
        {
            Board->SetBrush(InvUiScreenRoundedBrush(
                FLinearColor(InvUiScreenPanelColor.R, InvUiScreenPanelColor.G, InvUiScreenPanelColor.B,
                    FMath::Clamp(BackgroundOpacity, 0.f, 1.f)),
                InvUiScreenPanelOutline, Radius));
            Board->SetVisibility(ESlateVisibility::Collapsed);
            PlaceInCanvas(Board, FVector2D::ZeroVector, FVector2D::ZeroVector, InvUiScreenPanelZOrder + Index * 2);
        }

        UImage* TitleBar = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(),
            *FString::Printf(TEXT("PanelTitleBar%d"), Index));
        if (TitleBar)
        {
            TitleBar->SetBrush(InvUiScreenRoundedBrush(InvUiScreenTitleBarColor, FLinearColor::Transparent, Radius));
            TitleBar->SetVisibility(ESlateVisibility::Collapsed);
            PlaceInCanvas(TitleBar, FVector2D::ZeroVector, FVector2D::ZeroVector, InvUiScreenPanelZOrder + Index * 2 + 1);
        }

        UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
            *FString::Printf(TEXT("PanelTitle%d"), Index));
        if (Title)
        {
            Title->SetColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.95f, 1.f, 1.f)));
            Title->SetVisibility(ESlateVisibility::Collapsed);
            PlaceInCanvas(Title, FVector2D::ZeroVector, FVector2D::ZeroVector);
        }

        PanelBoards.Add(Board);
        PanelTitleBars.Add(TitleBar);
        PanelTitles.Add(Title);
    }

    // 多的收起来（不销毁：它可能正处在鼠标事件处理里；成员数组保持与 `PanelContainers` 同长）。
    for (int32 Index = Needed; Index < PanelBoards.Num(); ++Index)
    {
        if (PanelBoards[Index])
        {
            PanelBoards[Index]->SetVisibility(ESlateVisibility::Collapsed);
        }
        if (PanelTitleBars[Index])
        {
            PanelTitleBars[Index]->SetVisibility(ESlateVisibility::Collapsed);
        }
        if (PanelTitles[Index])
        {
            PanelTitles[Index]->SetVisibility(ESlateVisibility::Collapsed);
        }
    }

    PanelContainers.SetNum(FMath::Max(Needed, 0));
    if (Needed <= 0)
    {
        PanelContainers.Reset();
    }
}

void UInvInventoryScreenWidget::SetPanelDecorVisible(bool bVisible)
{
    const ESlateVisibility Visibility = bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
    for (int32 Index = 0; Index < PanelBoards.Num(); ++Index)
    {
        const bool bShown = bVisible && PanelContainers.IsValidIndex(Index) && PanelContainers[Index] > 0;
        if (PanelBoards[Index])
        {
            PanelBoards[Index]->SetVisibility(bShown ? Visibility : ESlateVisibility::Collapsed);
        }
        if (PanelTitleBars[Index])
        {
            PanelTitleBars[Index]->SetVisibility(bShown ? Visibility : ESlateVisibility::Collapsed);
        }
        if (PanelTitles[Index])
        {
            PanelTitles[Index]->SetVisibility(bShown ? Visibility : ESlateVisibility::Collapsed);
        }
    }
}

void UInvInventoryScreenWidget::ApplyFonts(float InCellSize)
{
    // 小字号文本（提示 / 负重 / 按钮 / 菜单 / 子格子小标题）共用一套基准；面板标题走 `CellSize × 0.4`。
    const int32 LabelFont = ScaledFontSize(LabelFontBase, InCellSize);
    const int32 PanelFont = TitleFontSize(InCellSize);

    if (TitleLabel)
    {
        TitleLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
    }
    if (WeightLabel)
    {
        WeightLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
    }
    if (SortButtonLabel)
    {
        SortButtonLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
    }

    // 右键菜单按钮里的文本（`UButton::OnClicked` 不带参数，所以按钮是数组、文本取它唯一的孩子）。
    for (const TObjectPtr<UButton>& MenuButton : ContextMenuButtons)
    {
        if (MenuButton)
        {
            if (UTextBlock* MenuLabel = Cast<UTextBlock>(MenuButton->GetChildAt(0)))
            {
                MenuLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
            }
        }
    }

    for (const TObjectPtr<UTextBlock>& PanelTitle : PanelTitles)
    {
        if (PanelTitle)
        {
            PanelTitle->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", PanelFont));
        }
    }

    for (const TObjectPtr<UTextBlock>& Caption : CaptionLabels)
    {
        if (Caption)
        {
            Caption->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFont));
        }
    }
}

void UInvInventoryScreenWidget::LayoutHeaderAtOrigin(float InCellSize)
{
    if (!RootCanvas)
    {
        return;
    }

    // 旧口径：提示行在 `Origin` 上方，负重条 / 条上文本贴着 `Origin`，「整理」按钮在条的右边。
    const int32 LabelFont = ScaledFontSize(LabelFontBase, InCellSize);
    const float BarWidth = FMath::Max(MinPanelWidth - SortButtonWidth - 8.f, 80.f);

    if (TitleLabel)
    {
        PlaceInCanvas(TitleLabel, FVector2D(Origin.X, Origin.Y - LabelFont - InvUiScreenTitleGap),
            FVector2D(MinPanelWidth, LabelFont + 4.f));
    }
    if (WeightBar)
    {
        PlaceInCanvas(WeightBar, Origin, FVector2D(BarWidth, InvUiScreenBarHeight));
    }
    if (WeightLabel)
    {
        PlaceInCanvas(WeightLabel, Origin + FVector2D(6.f, 2.f),
            FVector2D(BarWidth - 12.f, InvUiScreenBarHeight - 4.f));
    }
    if (SortButton)
    {
        PlaceInCanvas(SortButton, FVector2D(Origin.X + MinPanelWidth - SortButtonWidth, Origin.Y),
            FVector2D(SortButtonWidth, InvUiScreenBarHeight));
    }
}

void UInvInventoryScreenWidget::LayoutManual()
{
    if (!RootCanvas)
    {
        return;
    }

    // 兼容模式：完全退回旧行为——子格子从 `Origin` 左上角起、每容器一行、`CellSize` 原样（不缩放不居中）。
    const float Cell = ManualCellSize();
    SyncPanelDecor(0);
    SetPanelDecorVisible(false);
    ApplyFonts(Cell);
    LayoutHeaderAtOrigin(Cell);
    LayoutGrids();

    ScreenLayout.CellSize = Cell;
    ScreenLayout.TotalSize = FVector2D::ZeroVector;   // 旧口径没有「整屏尺寸」这个概念
    ScreenLayout.PanelOrigin = Origin;
}

void UInvInventoryScreenWidget::LayoutCentered()
{
    if (!RootCanvas)
    {
        return;
    }

    const float Cell = FontCellSize();
    const FVector2D Spacing = EffectivePanelSpacing();

    TArray<FInvScreenPanelRect> Panels;
    BuildPanelRects(Cell, Panels);

    SyncPanelDecor(Panels.Num());

    if (Panels.Num() <= 0)
    {
        // 一块面板都没有（没绑定组件 / 没有任何根容器）：退回旧口径的提示 + 负重条，不画底板。
        SetPanelDecorVisible(false);
        ApplyFonts(Cell);
        LayoutHeaderAtOrigin(Cell);

        ScreenLayout.CellSize = Cell;
        ScreenLayout.TotalSize = FVector2D::ZeroVector;
        ScreenLayout.PanelOrigin = Origin;
        return;
    }

    const FVector2D ClusterSize = MeasureClusterSize(Cell);
    const FVector2D TotalSize = ClusterSize + Spacing * 2.f;
    const FVector2D Viewport = ResolveViewportSize();
    const bool bSizeKnown = Viewport.X > 0.f && Viewport.Y > 0.f;

    // 居中：整屏相对 viewport 正中（主面板中心 = 整屏中心）；尺寸未知（没上屏 / 无头命令集）时
    // 退回「从 `Origin` 起摆」，此时 `Origin` 就是整屏左上角（旧语义），不做居中偏移。
    const FVector2D Centering = bSizeKnown ? (Viewport - TotalSize) * 0.5f : FVector2D::ZeroVector;
    const FVector2D PanelOrigin = Centering + Origin;
    const FVector2D ClusterCenter = PanelOrigin + Spacing + ClusterSize * 0.5f;

    ScreenLayout.CellSize = Cell;
    ScreenLayout.TotalSize = bSizeKnown ? TotalSize : FVector2D::ZeroVector;
    ScreenLayout.PanelOrigin = PanelOrigin;

    ApplyFonts(Cell);

    const float Padding = InvUiScreenScaled(InvUiScreenPanelPaddingBase, Cell);
    const float CaptionH = InvUiScreenCaptionHeight(Cell);
    const int32 PanelFont = TitleFontSize(Cell);
    const float Radius = InvUiScreenScaled(PanelCornerRadiusBase, Cell);
    const float PanelOpacity = FMath::Clamp(BackgroundOpacity, 0.f, 1.f);
    const float LabelFont = (float)ScaledFontSize(LabelFontBase, Cell) * 1.25f;

    for (int32 Index = 0; Index < Panels.Num(); ++Index)
    {
        const FInvScreenPanelRect& Panel = Panels[Index];
        const FVector2D PanelPos = ClusterCenter + Panel.Position;
        const bool bMain = Panel.Slot == EInvScreenPanelSlot::Main;

        if (PanelContainers.IsValidIndex(Index))
        {
            PanelContainers[Index] = Panel.Container;
        }

        // 底板（圆角 + 描边）：不透明度跟着 `BackgroundOpacity` 走。
        if (PanelBoards.IsValidIndex(Index) && PanelBoards[Index])
        {
            PanelBoards[Index]->SetBrush(InvUiScreenRoundedBrush(
                FLinearColor(InvUiScreenPanelColor.R, InvUiScreenPanelColor.G, InvUiScreenPanelColor.B, PanelOpacity),
                InvUiScreenPanelOutline, Radius));
            PanelBoards[Index]->SetVisibility(ESlateVisibility::Visible);
            PlaceInCanvas(PanelBoards[Index], PanelPos, Panel.Size, InvUiScreenPanelZOrder + Index * 2);
        }

        // 标题栏：底条 + 容器标题（主面板的标题栏里还有负重条与文本）。
        const float TitleContents = bMain
            ? InvUiScreenWeightBarHeight(Cell) + InvUiScreenTitleBarBottomPadding
            : 0.f;
        const float TitleBarH = InvUiScreenTitleBarHeight(TitleHeight, Cell, PanelFont, TitleContents);
        const float BarWidth = bMain ? InvUiScreenWeightBarWidth(Panel.Size.X) : 0.f;

        if (PanelTitleBars.IsValidIndex(Index) && PanelTitleBars[Index])
        {
            PanelTitleBars[Index]->SetBrush(
                InvUiScreenRoundedBrush(InvUiScreenTitleBarColor, FLinearColor::Transparent, Radius));
            PanelTitleBars[Index]->SetVisibility(ESlateVisibility::Visible);
            PlaceInCanvas(PanelTitleBars[Index], PanelPos, FVector2D(Panel.Size.X, TitleBarH),
                InvUiScreenPanelZOrder + Index * 2 + 1);
        }

        if (PanelTitles.IsValidIndex(Index) && PanelTitles[Index])
        {
            const FString Label = Inventory ? Inventory->GetContainerLabel(Panel.Container) : FString();
            const float TitleWidth = FMath::Max(
                Panel.Size.X - Padding * 2.f - (bMain ? BarWidth + Padding : 0.f), 40.f);

            PanelTitles[Index]->SetText(FText::FromString(Label.IsEmpty()
                ? FString::Printf(TEXT("容器 %lld"), Panel.Container)
                : Label));
            PanelTitles[Index]->SetVisibility(ESlateVisibility::Visible);
            PlaceInCanvas(PanelTitles[Index],
                PanelPos + FVector2D(Padding, FMath::Max((TitleBarH - LabelFont) * 0.5f, 0.f)),
                FVector2D(TitleWidth, FMath::Max(LabelFont, 1.f)));
        }

        if (bMain)
        {
            // 负重条 + 条上的文本：摆在标题栏右侧。
            const float BarH = InvUiScreenWeightBarHeight(Cell);
            const FVector2D BarPos(
                PanelPos.X + Panel.Size.X - Padding - BarWidth,
                PanelPos.Y + FMath::Max((TitleBarH - BarH) * 0.5f, 0.f));

            if (WeightBar)
            {
                PlaceInCanvas(WeightBar, BarPos, FVector2D(BarWidth, BarH));
            }
            if (WeightLabel)
            {
                PlaceInCanvas(WeightLabel, BarPos + FVector2D(6.f, 2.f), FVector2D(BarWidth - 12.f, BarH - 4.f));
            }

            // 「整理」按钮：主面板右下角（占面板底边预留的那一条）。
            if (SortButton)
            {
                const FVector2D ButtonSize = InvUiScreenSortButtonSize(Cell);
                PlaceInCanvas(SortButton, FVector2D(
                    PanelPos.X + Panel.Size.X - Padding - ButtonSize.X,
                    PanelPos.Y + Panel.Size.Y - Padding - ButtonSize.Y), ButtonSize);
            }
        }

        // 子格子：同一容器的多块并排，各自上方一行小标题。
        TArray<UInvGridWidget*> PanelGrids;
        CollectGridsOfContainer(Panel.Container, PanelGrids);

        const float ContentTop = PanelPos.Y + Padding + TitleBarH;
        float CursorX = PanelPos.X + Padding;
        for (UInvGridWidget* Grid : PanelGrids)
        {
            const FVector2D GridSize = Grid->GetGridPixelSize();

            const int32 FlatIndex = GridWidgets.IndexOfByPredicate(
                [Grid](const TObjectPtr<UInvGridWidget>& Candidate) { return Candidate == Grid; });
            if (CaptionLabels.IsValidIndex(FlatIndex) && CaptionLabels[FlatIndex])
            {
                UTextBlock* Caption = CaptionLabels[FlatIndex];
                Caption->SetText(FText::FromString(Grid->GetGridTitle()));
                PlaceInCanvas(Caption, FVector2D(CursorX, ContentTop),
                    FVector2D(FMath::Max(GridSize.X, 64.f), CaptionH));
            }

            PlaceInCanvas(Grid, FVector2D(CursorX, ContentTop + CaptionH), GridSize);
            CursorX += GridSize.X + InvUiScreenGridSpacing;
        }
    }

    SetPanelDecorVisible(true);
}

void UInvInventoryScreenWidget::ApplyLayout()
{
    if (!RootCanvas)
    {
        return;
    }

    if (bCenterOnViewport)
    {
        LayoutCentered();
    }
    else
    {
        LayoutManual();
    }
}

void UInvInventoryScreenWidget::Refresh()
{
    // 1) 负重：条 + 文本（构造前也能算，纯读组件）。
    UpdateWeightDisplay();

    // 2) 这次要用的单格边长：居中模式下按当前 viewport 自适应缩放（分辨率变了这里就跟着变）。
    UpdateEffectiveCellSize();

    // 3) 控件树：没有就建（只建 UObject 层，无头命令集里也能建），建不出来就只留文本层。
    EnsureContentBuilt();
    if (!RootCanvas)
    {
        return;
    }

    // 4) 子格子：缺的补、多的隐藏（尺寸用第 2 步算出来的实际边长）。
    TArray<FInvGridSlotKey> Desired;
    CollectDesiredGrids(Desired);
    SyncGridWidgets(Desired);

    // 5) 布局：面板（居中模式）/ 逐行（手动模式）+ 格子尺寸 / 位置 / 标题。
    ApplyLayout();

    InvalidateLayoutAndVolatility();
}

void UInvInventoryScreenWidget::SetInventory(UInvInventoryComponent* InInventory)
{
    Inventory = InInventory;

    // 换数据源：旧格子引用的还是旧组件，全部摘掉重建（走 SyncGridWidgets 的「结构变了」分支）。
    GridSignature.Reset();
    FocusedGrid = nullptr;

    Refresh();
}

// ==================== 负重 ====================

FString UInvInventoryScreenWidget::GetWeightText() const
{
    if (!Inventory)
    {
        return FString(TEXT("总重 -- / 上限 --（未绑定背包组件）"));
    }

    const float Weight = Inventory->GetTotalWeight();
    if (Inventory->MaxCapacityKg > 0.f)
    {
        return FString::Printf(TEXT("总重 %.2f kg / 上限 %.0f kg%s"),
            Weight, Inventory->MaxCapacityKg, Inventory->IsOverloaded() ? TEXT("（超重）") : TEXT(""));
    }
    return FString::Printf(TEXT("总重 %.2f kg / 上限 无限制"), Weight);
}

float UInvInventoryScreenWidget::GetWeightRatio() const
{
    return Inventory ? Inventory->GetWeightRatio() : 0.f;
}

bool UInvInventoryScreenWidget::IsOverloaded() const
{
    return Inventory ? Inventory->IsOverloaded() : false;
}

float UInvInventoryScreenWidget::GetWeightBarPercent() const
{
    // 直接读条控件本身（没建出来时给 0），这样测试断的就是「条真的被刷了」。
    return WeightBar ? WeightBar->GetPercent() : 0.f;
}

FLinearColor UInvInventoryScreenWidget::GetWeightBarColor() const
{
    return WeightBar ? WeightBar->GetFillColorAndOpacity() : FLinearColor::Transparent;
}

// ==================== 交互 ====================

int32 UInvInventoryScreenWidget::AutosortFocusedContainer()
{
    if (!Inventory)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 没绑定背包组件，「整理」跳过"), *GetName());
        return InvUiScreenErrInvalidHandle;
    }

    // 优先当前悬停 / 选中的格子所在容器；没有的话退回背包。
    int64 Target = (FocusedGrid && FocusedGrid->Container > 0) ? FocusedGrid->Container : 0;
    if (Target <= 0)
    {
        Target = Inventory->GetContainer(EInvContainerType::Backpack);
        UE_LOG(LogTemp, Log, TEXT("背包界面: 界面 %s 没有当前格子，「整理」对背包容器（句柄 %lld）执行"),
            *GetName(), Target);
    }

    if (Target <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 找不到可整理的容器（背包根容器不存在），「整理」跳过"), *GetName());
        return InvUiScreenErrInvalidHandle;
    }

    const int32 Result = Inventory->AutosortContainer(Target);
    if (Result == 0)
    {
        UE_LOG(LogTemp, Log, TEXT("背包界面: 界面 %s 整理了容器 %lld"), *GetName(), Target);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 整理容器 %lld 失败（错误码 %d；19 = 排不下，原布局不变）"),
            *GetName(), Target, Result);
    }

    Refresh();  // 整理是整体重排：全刷一遍（含负重文本）
    return Result;
}

void UInvInventoryScreenWidget::OnSortClicked()
{
    AutosortFocusedContainer();
}

void UInvInventoryScreenWidget::NotifyGridFocused(UInvGridWidget* Grid)
{
    if (Grid && Grid->GetOwnerScreen() != this)
    {
        // 子格子的回归指针没指到自己：补上（手工 Create Widget 拼界面时可能没设）。
        Grid->OwnerScreen = this;
    }
    FocusedGrid = Grid;
}

void UInvInventoryScreenWidget::NotifyInventoryChanged()
{
    Refresh();
}

TArray<UInvGridWidget*> UInvInventoryScreenWidget::GetGridWidgets() const
{
    TArray<UInvGridWidget*> Result;
    Result.Reserve(GridWidgets.Num());
    for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
    {
        Result.Add(Grid.Get());
    }
    return Result;
}

// ==================== 右键菜单 ====================

TArray<EInvContextAction> UInvInventoryScreenWidget::GetAvailableContextActions(int64 ItemHandle) const
{
    TArray<EInvContextAction> Actions;

    FInvItemView View;
    if (!Inventory || !Inventory->GetItemView(ItemHandle, View))
    {
        return Actions;  // 没绑定 / 句柄无效：一件都做不了（GetItemView 内部会打中文日志）
    }

    // 固定顺序 = 枚举顺序 = 菜单里的按钮顺序。
    if (View.Definition && View.Definition->bRotatable)
    {
        Actions.Add(EInvContextAction::Rotate);
    }
    if (View.Definition && View.Definition->MaxStack > 1 && View.Stack >= 2)
    {
        Actions.Add(EInvContextAction::SplitHalf);  // 堆叠 1 件时拆不动，菜单里就不出现
    }
    if (View.Definition)
    {
        Actions.Add(EInvContextAction::Use);
    }
    Actions.Add(EInvContextAction::Destroy);  // 句柄有效就能销毁

    if (View.HostContainer > 0)
    {
        Actions.Add(EInvContextAction::AutosortContainer);  // 未落位就没有「它所在的容器」可整理
    }
    return Actions;
}

void UInvInventoryScreenWidget::RequestContextMenuForItem(int64 Item, FVector2D ScreenPosition)
{
    if (!Inventory)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 没绑定背包组件，右键菜单跳过"), *GetName());
        return;
    }

    FInvItemView View;
    if (!Inventory->GetItemView(Item, View))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 收到右键请求，但物品句柄 %lld 无效，菜单不开"),
            *GetName(), Item);
        return;
    }

    // 控件树可能在无头 / 刚 NewObject 时还没建：先保证它建起来（只建 UObject 层，不碰 Slate）。
    EnsureContentBuilt();
    if (!ContextMenuBorder)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s 没有右键菜单底板（控件树没建起来），菜单不开"), *GetName());
        return;
    }

    const TArray<EInvContextAction> Available = GetAvailableContextActions(Item);

    // 按钮下标 = 枚举取值：可用的放开、不可用的收起来（不会出现「点了才报错」的按钮）。
    for (int32 Index = 0; Index < ContextMenuButtons.Num(); ++Index)
    {
        UButton* Button = ContextMenuButtons[Index];
        if (!Button)
        {
            continue;
        }
        const bool bEnabled = Available.Contains((EInvContextAction)Index);
        Button->SetVisibility(bEnabled ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }

    ContextMenuItem = Item;
    bContextMenuVisible = true;

    // 屏幕坐标 → 根画布本地坐标（根画布没缩放时两者相等；无头命令集里几何是零尺寸，直接用传入值）。
    FVector2D MenuPosition = ScreenPosition;
    if (RootCanvas)
    {
        const FGeometry CanvasGeometry = RootCanvas->GetCachedGeometry();
        if (!CanvasGeometry.GetLocalSize().IsNearlyZero())
        {
            MenuPosition = CanvasGeometry.AbsoluteToLocal(ScreenPosition);
        }
    }
    ContextMenuPosition = MenuPosition;

    // 菜单高度按**可见按钮数**算：收起按钮不占位（`UVerticalBox` 里 Collapsed 的子控件本来就不占）。
    const float MenuHeight = FMath::Max((float)Available.Num(), 1.f) * ContextMenuButtonHeight
        + InvUiScreenMenuPadding * 2.f;
    PlaceInCanvas(ContextMenuBorder, MenuPosition, FVector2D(ContextMenuWidth, MenuHeight));
    ContextMenuBorder->SetVisibility(ESlateVisibility::Visible);

    UE_LOG(LogTemp, Log, TEXT("背包界面: 界面 %s 在 (%.0f,%.0f) 对物品 %lld 开了右键菜单（可用动作 %d 个）"),
        *GetName(), MenuPosition.X, MenuPosition.Y, Item, Available.Num());
}

void UInvInventoryScreenWidget::CloseContextMenu()
{
    const bool bWasVisible = bContextMenuVisible;

    bContextMenuVisible = false;
    ContextMenuItem = 0;
    if (ContextMenuBorder)
    {
        ContextMenuBorder->SetVisibility(ESlateVisibility::Collapsed);
    }

    if (bWasVisible)
    {
        UE_LOG(LogTemp, Log, TEXT("背包界面: 界面 %s 收起了右键菜单"), *GetName());
    }
}

bool UInvInventoryScreenWidget::IsContextMenuVisible() const
{
    return bContextMenuVisible;
}

int64 UInvInventoryScreenWidget::GetContextMenuItemHandle() const
{
    return ContextMenuItem;
}

UInvGridWidget* UInvInventoryScreenWidget::FindGridOwningItem(int64 Item) const
{
    if (Item <= 0)
    {
        return nullptr;
    }
    for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
    {
        FInvItemView View;
        if (Grid && Grid->GetItemViewInGrid(Item, View))
        {
            return Grid;
        }
    }
    return nullptr;
}

int32 UInvInventoryScreenWidget::InvokeContextAction(EInvContextAction Action)
{
    const int64 Item = ContextMenuItem;
    if (Item <= 0)
    {
        const FString Message = FString::Printf(TEXT("右键菜单没有目标物品（句柄 %lld），动作已忽略"), Item);
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s %s"), *GetName(), *Message);
        NotifyOperationFailed(InvUiScreenErrInvalidArgument, Message);
        return InvUiScreenErrInvalidArgument;  // 22
    }

    FInvItemView View;
    if (!Inventory || !Inventory->GetItemView(Item, View))
    {
        const FString Message = FString::Printf(TEXT("物品句柄 %lld 无效，动作已忽略"), Item);
        UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s %s"), *GetName(), *Message);
        NotifyOperationFailed(InvUiScreenErrInvalidHandle, Message);
        return InvUiScreenErrInvalidHandle;  // 23
    }

    const FString Display = InvUiScreenDisplayNameOf(View.Definition);

    switch (Action)
    {
    case EInvContextAction::Rotate:
    {
        const int32 Result = Inventory->RotateItem(Item);
        if (Result == 0)
        {
            CloseContextMenu();
            Refresh();
        }
        else
        {
            NotifyOperationFailed(Result, FString::Printf(
                TEXT("旋转「%s」（物品 %lld）失败（错误码 %d；4 = 该定义不可旋转）"), *Display, Item, Result));
        }
        return Result;
    }

    case EInvContextAction::SplitHalf:
    {
        // 拆分核心在子格子上（落点顺序 / 朝向 / 自动落位都在那里）：界面只负责找到「显示这件物品」的格子。
        UInvGridWidget* Grid = FindGridOwningItem(Item);
        if (!Grid)
        {
            const FString Message = FString::Printf(
                TEXT("物品「%s」（%lld）不在任何子网格上（没落位 / 界面没建格子），拆分一半无效"), *Display, Item);
            UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s %s"), *GetName(), *Message);
            NotifyOperationFailed(InvUiScreenErrInvalidArgument, Message);
            return InvUiScreenErrInvalidArgument;  // 22
        }

        Grid->SelectItem(Item);  // 菜单操作的目标 = 菜单上那一件，先把选中对齐
        const int32 Result = Grid->SplitHalfItem(Item);
        if (Result == 0)
        {
            CloseContextMenu();
        }
        else if (Grid->GetOwnerScreen() != this)
        {
            // 格子没回指到本界面（手工拼的控件树）：它的失败广播发不到这里，兜底补一次，
            // 已经回指的情况不重复报（格子自己会走 NotifyOperationFailed）。
            NotifyOperationFailed(Result, FString::Printf(
                TEXT("拆分一半失败（物品「%s」%lld，错误码 %d）"), *Display, Item, Result));
        }
        return Result;
    }

    case EInvContextAction::Use:
    {
        if (!View.Definition)
        {
            const FString Message = FString::Printf(TEXT("物品 %lld 没有定义（DefId 对不上），无法使用"), Item);
            UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s %s"), *GetName(), *Message);
            NotifyOperationFailed(InvUiScreenErrInvalidArgument, Message);
            return InvUiScreenErrInvalidArgument;  // 22
        }

        // 背包不实现「使用」的效果：广播出去，交给玩法侧（技能 / 吃喝 / 装备）。
        NotifyItemUsed(View);
        CloseContextMenu();
        return 0;
    }

    case EInvContextAction::Destroy:
    {
        const int64 Removed = Inventory->DestroyItem(Item);  // 返回删除件数（含套包内容），< 0 = -错误码
        if (Removed < 0)
        {
            const int32 Code = (int32)(-Removed);
            NotifyOperationFailed(Code, FString::Printf(
                TEXT("销毁「%s」（物品 %lld）失败（错误码 %d）"), *Display, Item, Code));
            return Code;
        }

        UE_LOG(LogTemp, Log, TEXT("背包界面: 界面 %s 销毁了物品「%s」（%lld，共 %lld 件）"),
            *GetName(), *Display, Item, Removed);

        // 先广播（视图里还带着内容），再刷新 —— 销毁之后这个句柄就失效了。
        NotifyItemDestroyed(View);
        CloseContextMenu();
        Refresh();
        return 0;
    }

    case EInvContextAction::AutosortContainer:
    {
        if (View.HostContainer <= 0)
        {
            const FString Message = FString::Printf(TEXT("物品「%s」（%lld）还没落位，没有可整理的容器"),
                *Display, Item);
            UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s %s"), *GetName(), *Message);
            NotifyOperationFailed(InvUiScreenErrInvalidArgument, Message);
            return InvUiScreenErrInvalidArgument;  // 22
        }

        const int32 Result = Inventory->AutosortContainer(View.HostContainer);
        if (Result == 0)
        {
            CloseContextMenu();
            Refresh();
        }
        else
        {
            NotifyOperationFailed(Result, FString::Printf(
                TEXT("整理「%s」所在的容器 %lld 失败（错误码 %d；19 = 排不下，原布局不变）"),
                *Display, View.HostContainer, Result));
        }
        return Result;
    }

    default:
        break;
    }

    const FString Message = FString::Printf(TEXT("未知的右键菜单动作（枚举值 %d）"), (int32)Action);
    UE_LOG(LogTemp, Warning, TEXT("背包界面: 界面 %s %s"), *GetName(), *Message);
    NotifyOperationFailed(InvUiScreenErrInvalidArgument, Message);
    return InvUiScreenErrInvalidArgument;  // 22
}

void UInvInventoryScreenWidget::OnContextRotateClicked()
{
    InvokeContextAction(EInvContextAction::Rotate);
}

void UInvInventoryScreenWidget::OnContextSplitHalfClicked()
{
    InvokeContextAction(EInvContextAction::SplitHalf);
}

void UInvInventoryScreenWidget::OnContextUseClicked()
{
    InvokeContextAction(EInvContextAction::Use);
}

void UInvInventoryScreenWidget::OnContextDestroyClicked()
{
    InvokeContextAction(EInvContextAction::Destroy);
}

void UInvInventoryScreenWidget::OnContextAutosortClicked()
{
    InvokeContextAction(EInvContextAction::AutosortContainer);
}

// ==================== 事件 ====================

void UInvInventoryScreenWidget::PlayUiSound(const TObjectPtr<USoundBase>& Sound) const
{
    // 三个音效默认都留空 = 全程静音：零资产插件不该因为「没配音效」出一句警告或者崩。
    if (!bPlaySounds || !Sound)
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        UGameplayStatics::PlaySound2D(World, Sound);
    }
}

void UInvInventoryScreenWidget::NotifyItemPickedUp(const FInvItemView& Item)
{
    ++PickedUpEventCount;
    PlayUiSound(PickUpSound);
    OnItemPickedUp.Broadcast(Item);
}

void UInvInventoryScreenWidget::NotifyItemDropped(const FInvItemView& Item, int32 ResultCode)
{
    ++DroppedEventCount;
    // 落位成功 = 放下音效；失败另有 OnOperationFailed（带失败音效），这里不重复报。
    PlayUiSound(ResultCode == 0 ? DropSound : ErrorSound);
    OnItemDropped.Broadcast(Item, ResultCode);
}

void UInvInventoryScreenWidget::NotifyItemUsed(const FInvItemView& Item)
{
    ++UsedEventCount;
    LastUsedItemHandle = Item.Handle;
    OnItemUsed.Broadcast(Item);
}

void UInvInventoryScreenWidget::NotifyItemDestroyed(const FInvItemView& Item)
{
    ++DestroyedEventCount;
    LastDestroyedItemHandle = Item.Handle;  // 记在广播之前：销毁后句柄就失效了
    PlayUiSound(DropSound);
    OnItemDestroyed.Broadcast(Item);
}

void UInvInventoryScreenWidget::NotifyOperationFailed(int32 Code, const FString& Message)
{
    ++FailedEventCount;
    LastFailedCode = Code;
    LastFailedMessage = Message;
    PlayUiSound(ErrorSound);
    OnOperationFailed.Broadcast(Code, Message);
}

// ==================== 跨容器拖（口袋 ↔ 弹挂 ↔ 背包） ====================

UInvGridWidget* UInvInventoryScreenWidget::FindGridUnderCursor(FVector2D ScreenPosition, UInvGridWidget* Exclude) const
{
    for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
    {
        if (!Grid || Grid == Exclude || Grid->GetVisibility() != ESlateVisibility::Visible)
        {
            continue;
        }

        // 用 Slate 算好的几何判定：没绘制过的格子几何是零尺寸，IsUnderLocation 自然为假
        // （真实 PIE 里几何一定有效）。
        const FGeometry Geometry = Grid->GetCachedGeometry();
        if (Geometry.IsUnderLocation(ScreenPosition))
        {
            return Grid;
        }
    }
    return nullptr;
}

void UInvInventoryScreenWidget::UpdateCrossGridDragPreview(UInvGridWidget* SourceGrid, FVector2D ScreenPosition,
    bool bRotated)
{
    UInvGridWidget* Under = FindGridUnderCursor(ScreenPosition, SourceGrid);
    for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
    {
        if (!Grid || Grid == SourceGrid)
        {
            continue;
        }
        if (Grid == Under)
        {
            Grid->PreviewExternalDrag(SourceGrid, ScreenPosition, bRotated);
        }
        else if (Grid->ExternalDragSource)
        {
            Grid->ClearExternalDragPreview();
        }
    }
}

bool UInvInventoryScreenWidget::RouteDropToGridUnderCursor(UInvGridWidget* SourceGrid, FVector2D ScreenPosition,
    bool bRotated)
{
    UInvGridWidget* Under = FindGridUnderCursor(ScreenPosition, SourceGrid);
    if (!Under)
    {
        return false;  // 光标还在源格子上（或者不在任何格子上）：让源格子按老路子落位
    }

    // 交接：目标格子按「屏幕坐标 + 源格子抓取偏移」算落点，并收尾源格子的拖拽状态。
    Under->DropDragFromOtherGrid(SourceGrid, ScreenPosition, bRotated);
    return true;
}

void UInvInventoryScreenWidget::ClearCrossGridDragPreview()
{
    for (const TObjectPtr<UInvGridWidget>& Grid : GridWidgets)
    {
        if (Grid && Grid->ExternalDragSource)
        {
            Grid->ClearExternalDragPreview();
        }
    }
}
