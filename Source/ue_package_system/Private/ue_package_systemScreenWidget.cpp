#include "ue_package_systemScreenWidget.h"

#include "Application/SlateApplicationBase.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
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

    const float BarWidth = FMath::Max(MinPanelWidth - SortButtonWidth - 8.f, 80.f);

    // 顶部说明：告诉用户能干什么（无资产，纯文本）。
    TitleLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ScreenTitle"));
    if (TitleLabel)
    {
        TitleLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", TitleFontSize));
        TitleLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.90f, 0.93f, 1.f, 1.f)));
        TitleLabel->SetText(FText::FromString(TEXT("背包（拖拽移动 / R 旋转 / Esc 取消）")));
        PlaceInCanvas(TitleLabel, FVector2D(Origin.X, Origin.Y - TitleFontSize - InvUiScreenTitleGap),
            FVector2D(MinPanelWidth, TitleFontSize + 4.f));
    }

    // 负重条 + 条上的文本：条画在下面，文本压在同一条上。
    WeightBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("WeightBar"));
    if (WeightBar)
    {
        WeightBar->SetPercent(0.f);
        WeightBar->SetFillColorAndOpacity(InvUiScreenFillNormal);
        PlaceInCanvas(WeightBar, Origin, FVector2D(BarWidth, InvUiScreenBarHeight));
    }

    WeightLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("WeightLabel"));
    if (WeightLabel)
    {
        WeightLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFontSize));
        WeightLabel->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 1.f)));
        WeightLabel->SetText(FText::FromString(GetWeightText()));
        PlaceInCanvas(WeightLabel, Origin + FVector2D(6.f, 2.f), FVector2D(BarWidth - 12.f, InvUiScreenBarHeight - 4.f));
    }

    // 「整理」按钮：UButton + 里面的 UTextBlock，样式走引擎默认（零资产）。
    SortButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("SortButton"));
    if (SortButton)
    {
        SortButton->OnClicked.AddDynamic(this, &UInvInventoryScreenWidget::OnSortClicked);

        UTextBlock* ButtonLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SortButtonLabel"));
        if (ButtonLabel)
        {
            ButtonLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFontSize));
            ButtonLabel->SetText(FText::FromString(TEXT("整理")));
            SortButton->AddChild(ButtonLabel);
        }

        PlaceInCanvas(SortButton, FVector2D(Origin.X + MinPanelWidth - SortButtonWidth, Origin.Y),
            FVector2D(SortButtonWidth, InvUiScreenBarHeight));
    }

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
                Label->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFontSize));
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

void UInvInventoryScreenWidget::PlaceInCanvas(UWidget* Widget, const FVector2D& Position, const FVector2D& Size)
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
    Grid->CellSize = CellSize;
    Grid->OwnerScreen = this;

    PlaceInCanvas(Grid, FVector2D(Origin.X, Origin.Y + InvUiScreenBarHeight + 14.f), FVector2D(1.f, 1.f));

    // 小标题（`弹挂 part2 (1x2)`）：跟格子一一对应，布局时一起摆。
    UTextBlock* Caption = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
        *FString::Printf(TEXT("GridCaption_%lld_%d"), InContainer, InPart));
    if (Caption)
    {
        Caption->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", LabelFontSize));
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
                Grid->CellSize = CellSize;
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
            Grid->CellSize = CellSize;
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

void UInvInventoryScreenWidget::Refresh()
{
    // 1) 负重：条 + 文本（构造前也能算，纯读组件）。
    UpdateWeightDisplay();

    // 2) 控件树：没有就建（只建 UObject 层，无头命令集里也能建），建不出来就只留文本层。
    EnsureContentBuilt();
    if (!RootCanvas)
    {
        return;
    }

    // 3) 子格子：缺的补、多的隐藏。
    TArray<FInvGridSlotKey> Desired;
    CollectDesiredGrids(Desired);
    SyncGridWidgets(Desired);

    // 4) 布局：格子的尺寸 / 位置 / 标题。
    LayoutGrids();

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
