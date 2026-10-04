#include "ue_package_systemDemoActor.h"

// 标签位掩码（AMMO / MAGAZINE / MEDKIT / GRENADE）的唯一定义处就在 C ABI 头里，直接用，
// 免得在 C++ 里手抄一份数值。只取常量，函数调用一律走友好层。
#include "ue_package_system_ffi.h"
#include "ue_package_systemScreenWidget.h"

#include "Components/BillboardComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    /** 演示用的承重上限（kg）：给个 30 kg，负重比那一行才有数看。 */
    constexpr float DemoMaxCapacityKg = 30.f;

    /** 屏幕刷新间隔的下限（秒）：配置 <= 0 时按它处理，免得定时器退化成逐帧。 */
    constexpr float MinRefreshIntervalSeconds = 0.25f;

    /** 套包内部最多展开几段：配置出环时别把摘要刷爆。 */
    constexpr int32 MaxNestedSections = 16;

    /** 物品清单每行的目标长度（字符）：超了就换行，屏幕 / 日志都不用横向拖。 */
    constexpr int32 ItemListLineWidth = 76;

    /** 第 Index 件（从 0 起）物品的字母：0 = A … 25 = Z、26 = AA…，超过 26 件也不会撞名。 */
    FString LetterForIndex(int32 Index)
    {
        FString Letter;
        int32 Value = FMath::Max(Index, 0);
        do
        {
            Letter.InsertAt(0, static_cast<TCHAR>('A' + (Value % 26)));
            Value = Value / 26 - 1;
        } while (Value >= 0);
        return Letter;
    }

    /** 摘要里显示的名字：优先 DisplayName（内置定义是中文），没填才退回资产名。 */
    FString DisplayNameOf(const UInvItemDefinition* Definition)
    {
        if (!Definition)
        {
            return TEXT("未知物品");
        }
        const FString Display = Definition->DisplayName.ToString();
        return Display.IsEmpty() ? Definition->GetName() : Display;
    }

    /** 取一件物品的字母；没见过就按“首次出现”的顺序发一个新的，并记进 Discovered。 */
    FString LetterOf(int64 ItemHandle, const UInvInventoryComponent* Component,
        TMap<int64, FString>& LetterByHandle, TArray<FInvItemView>& Discovered)
    {
        if (const FString* Existing = LetterByHandle.Find(ItemHandle))
        {
            return *Existing;
        }

        FInvItemView View;
        if (!Component->GetItemView(ItemHandle, View))
        {
            return TEXT("?");  // 句柄取不到视图：留个问号，别把格子吞掉
        }

        const FString Letter = LetterForIndex(Discovered.Num());
        LetterByHandle.Add(ItemHandle, Letter);
        Discovered.Add(View);
        return Letter;
    }

    /**
     * 画一个容器的网格，顺手给新出现的物品发字母。
     * `bPrintHeader` = true 时先打一行 `[标签] N 块子网格`（根容器用）；
     * 套包内部由调用方先打自己的标题，这里就只画网格：
     *
     *     [口袋] 4 块子网格
     *       part0 (1x1): A           <- 高度 1 的子网格跟标题写同一行
     *       part1 (6x5):             <- 多行的缩进四格逐行画
     *         XXX...
     *         ......
     */
    void AppendContainerGrid(const UInvInventoryComponent* Component, FString& Out, int64 Container,
        TMap<int64, FString>& LetterByHandle, TArray<FInvItemView>& Discovered, bool bPrintHeader = true)
    {
        const TArray<FIntPoint> Parts = Component->GetContainerParts(Container);

        if (bPrintHeader)
        {
            const FString Label = Component->GetContainerLabel(Container);
            Out += FString::Printf(TEXT("[%s] %d 块子网格\n"), Label.IsEmpty() ? TEXT("容器") : *Label, Parts.Num());
        }

        for (int32 Part = 0; Part < Parts.Num(); ++Part)
        {
            const FIntPoint Size = Parts[Part];  // 这里 FIntPoint 读作 (宽, 高)
            const TArray<int64> Cells = Component->GetContainerGrid(Container, Part);

            if (Cells.Num() != Size.X * Size.Y)
            {
                Out += FString::Printf(TEXT("  part%d (%dx%d): 取不到占格表（容器句柄或子网格下标无效）\n"),
                    Part, Size.X, Size.Y);
                continue;
            }

            const bool bSingleRow = Size.Y <= 1;
            Out += FString::Printf(TEXT("  part%d (%dx%d):%s"), Part, Size.X, Size.Y,
                bSingleRow ? TEXT(" ") : TEXT("\n"));

            for (int32 Y = 0; Y < Size.Y; ++Y)
            {
                if (!bSingleRow)
                {
                    Out += TEXT("    ");
                }
                for (int32 X = 0; X < Size.X; ++X)
                {
                    const int64 ItemHandle = Cells[Y * Size.X + X];
                    Out += (ItemHandle == 0)
                        ? FString(TEXT("."))
                        : LetterOf(ItemHandle, Component, LetterByHandle, Discovered);
                }
                Out += TEXT("\n");
            }
        }
    }
}

// ==================== 构造 / 生命周期 ====================

AInvInventoryDemoActor::AInvInventoryDemoActor()
{
    // 演示不需要逐帧 Tick：屏幕刷新走定时器。
    PrimaryActorTick.bCanEverTick = false;

    USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    RootComponent = SceneRoot;

    // 广告牌：编辑器里在关卡中一眼能找到它（打包构建里会被剥掉，所以可能是空指针）。
    Billboard = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Billboard"));
    if (Billboard)
    {
        // 图标用引擎自带的目标点贴图。命令集（cook / 无头测试）里跳过，免得去加载编辑器资产。
        if (!IsRunningCommandlet())
        {
            static ConstructorHelpers::FObjectFinderOptional<UTexture2D> BillboardSprite(
                TEXT("/Engine/EditorResources/S_TargetPoint"));
            Billboard->Sprite = BillboardSprite.Get();
        }
        Billboard->SetupAttachment(SceneRoot);
    }

    // 演示的核心就是一个普通背包组件：配置 / 装载 / 摘要全部走它的公开接口。
    InventoryComponent = CreateDefaultSubobject<UInvInventoryComponent>(TEXT("InventoryComponent"));
}

void AInvInventoryDemoActor::BeginPlay()
{
    // 顺序很重要：先把组件的配置（定义表 + 根容器）填好，再让 AActor::BeginPlay 去跑组件的 BeginPlay。
    // 反过来的话，组件会带着空配置初始化：一个根容器都没有，演示就只剩个空壳。
    if (bAutoFillOnBeginPlay)
    {
        const FString Summary = InitializeDemoInventory();
        UE_LOG(LogTemp, Log, TEXT("背包系统: %s\n%s"), *GetName(), *Summary);
    }

    Super::BeginPlay();

    // 界面要等组件初始化完（上面先灌了演示数据）再建；有 Player Controller 才会真的建起来。
    if (bCreateUI)
    {
        CreateInventoryUI();
    }

    if (bAutoFillOnBeginPlay && bDrawOnScreen && GetWorld())
    {
        // 先立刻画一次（不用等第一个间隔），再挂定时刷新：演示过程中背包被改动（捡 / 丢 / 整理），
        // 屏幕上的网格会跟着变。
        RefreshOnScreen();
        GetWorldTimerManager().SetTimer(RefreshTimerHandle, this, &AInvInventoryDemoActor::RefreshOnScreen,
            ClampedRefreshInterval(), true);
    }
}

void AInvInventoryDemoActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 定时器对称清理：Actor 销毁 / 关卡切换之后回调不该再跑。
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(RefreshTimerHandle);
    }

    if (bDrawOnScreen && GEngine && ScreenMessageKey != 0)
    {
        GEngine->RemoveOnScreenDebugMessage(ScreenMessageKey);
    }

    // 界面也是对称清理：Actor 没了，视口上不该再挂着它的控件（引用一并放掉，交给 GC）。
    DestroyInventoryUI();

    Super::EndPlay(EndPlayReason);
}

// ==================== 入口 ====================

FString AInvInventoryDemoActor::InitializeDemoInventory()
{
    if (!InventoryComponent)
    {
        // 构造函数一定会建；防的是派生类 / 蓝图里把组件删掉。
        return TEXT("背包演示：这个 Actor 上没有 InventoryComponent，没法初始化");
    }

    // 幂等：已经灌过就跳过配置与装载，只按当前状态重新生成摘要。
    // 根容器重复创建会被 Rust 侧拒绝（错误码 12），物品也会翻倍，所以装载只能做一次。
    if (!bDemoInitialized)
    {
        ApplyDefaultSetup();

        if (!InventoryComponent->InitializeInventory())
        {
            return FString::Printf(
                TEXT("背包演示：背包实例建不起来（组件 %s）。先在插件目录跑 `uerust build .`，再重启编辑器。"),
                *InventoryComponent->GetName());
        }

        // 30 kg 上限：让负重比那一行有数看；正式项目按自己的玩法数值设。
        InventoryComponent->SetMaxCapacity(DemoMaxCapacityKg);

        LoadoutFailures = FillDemoLoadout();
        bDemoInitialized = true;

        UE_LOG(LogTemp, Log, TEXT("背包系统: %s 演示数据就绪（装载失败 %d 件）"), *GetName(), LoadoutFailures);
    }

    return BuildSummaryText();
}

// ==================== 演示内容 ====================

void AInvInventoryDemoActor::EnsureBuiltinDefinitions()
{
    if (BuiltinDefinitions.Num() > 0)
    {
        return;  // 幂等：再造一遍会多出一批同名定义
    }

    // 名字固定成 DemoDef_*：组件注册定义时按资产名排序，名字固定 => DefId 固定，存档才可复现。
    // Outer 给 this：关卡里放两个演示 Actor，各自有一份自己的定义，不会跨 Actor 撞名。
    // （不用 MakeUniqueObjectName：它按约定一定会补一个数字后缀，名字就不固定了。）
    const auto MakeDefinition = [this](const TCHAR* Name) -> UInvItemDefinition*
    {
        return NewObject<UInvItemDefinition>(this, Name);
    };

    // 按固定下标入数组（EBuiltinDefinition 的顺序）；建失败也照加，免得后面串下标。
    BuiltinDefinitions.Add(MakeDefinition(TEXT("DemoDef_Ammo")));      // 0 = Ammo
    BuiltinDefinitions.Add(MakeDefinition(TEXT("DemoDef_Mag")));       // 1 = Mag
    BuiltinDefinitions.Add(MakeDefinition(TEXT("DemoDef_Medkit")));    // 2 = Medkit
    BuiltinDefinitions.Add(MakeDefinition(TEXT("DemoDef_Grenade")));   // 3 = Grenade
    BuiltinDefinitions.Add(MakeDefinition(TEXT("DemoDef_Backpack")));  // 4 = Backpack

    // 弹药：1x1、可叠 60。重量与估值都是**单件**值（Rust 侧总重 = 单件 × 堆叠）。
    if (UInvItemDefinition* Ammo = GetBuiltinDefinition(EBuiltinDefinition::Ammo))
    {
        Ammo->DisplayName = FText::FromString(TEXT("弹药"));
        Ammo->Width = 1;
        Ammo->Height = 1;
        Ammo->MaxStack = 60;
        Ammo->Weight = 0.012f;
        Ammo->Value = 3;
        Ammo->TagBits = static_cast<int32>(AMMO);
    }

    // 弹匣：1x2、可旋转（旋转后占 2x1）、不可堆叠（MaxStack 默认 1）。
    if (UInvItemDefinition* Mag = GetBuiltinDefinition(EBuiltinDefinition::Mag))
    {
        Mag->DisplayName = FText::FromString(TEXT("弹匣"));
        Mag->Width = 1;
        Mag->Height = 2;
        Mag->bRotatable = true;
        Mag->Weight = 0.2f;
        Mag->Value = 120;
        Mag->TagBits = static_cast<int32>(MAGAZINE);
    }

    // 急救包：1x1、最多叠 3。
    if (UInvItemDefinition* Medkit = GetBuiltinDefinition(EBuiltinDefinition::Medkit))
    {
        Medkit->DisplayName = FText::FromString(TEXT("急救包"));
        Medkit->Width = 1;
        Medkit->Height = 1;
        Medkit->MaxStack = 3;
        Medkit->Weight = 0.5f;
        Medkit->Value = 300;
        Medkit->TagBits = static_cast<int32>(MEDKIT);
    }

    // 手雷：1x1、不可堆叠。
    if (UInvItemDefinition* Grenade = GetBuiltinDefinition(EBuiltinDefinition::Grenade))
    {
        Grenade->DisplayName = FText::FromString(TEXT("手雷"));
        Grenade->Width = 1;
        Grenade->Height = 1;
        Grenade->Weight = 0.4f;
        Grenade->Value = 200;
        Grenade->TagBits = static_cast<int32>(GRENADE);
    }

    // 突击背包：本体 3x3，自带一块 6x5 的内部网格（套包）。带容器规格的物品必须不可堆叠。
    if (UInvItemDefinition* Backpack = GetBuiltinDefinition(EBuiltinDefinition::Backpack))
    {
        Backpack->DisplayName = FText::FromString(TEXT("突击背包"));
        Backpack->Width = 3;
        Backpack->Height = 3;
        Backpack->Weight = 2.0f;
        Backpack->Value = 900;
        Backpack->ContainerParts.Add(FIntPoint(6, 5));
    }

    UE_LOG(LogTemp, Log, TEXT("背包系统: %s 建了 %d 件内置演示定义"), *GetName(), BuiltinDefinitions.Num());
}

UInvItemDefinition* AInvInventoryDemoActor::GetBuiltinDefinition(EBuiltinDefinition Which) const
{
    const int32 Index = static_cast<int32>(Which);
    return BuiltinDefinitions.IsValidIndex(Index) ? BuiltinDefinitions[Index].Get() : nullptr;
}

void AInvInventoryDemoActor::ApplyDefaultSetup()
{
    EnsureBuiltinDefinitions();  // 演示装载要用内置定义，所以不管配没配都先建出来

    if (InventoryComponent->ItemDefinitions.Num() == 0)
    {
        // 内容表：设计师填了 DemoDefinitions 就先用它，否则用内置的 5 件。
        TArray<TObjectPtr<UInvItemDefinition>> Definitions;
        for (const TObjectPtr<UInvItemDefinition>& Candidate : DemoDefinitions)
        {
            if (Candidate)
            {
                Definitions.Add(Candidate);
            }
        }

        if (Definitions.Num() == 0)
        {
            Definitions = BuiltinDefinitions;
        }

        InventoryComponent->ItemDefinitions = Definitions;
    }

    if (InventoryComponent->RootContainers.Num() == 0)
    {
        // 四种根容器各一个（同类型只能建一个）。子网格尺寸按下面的演示装载选的：
        // 口袋四个 1x1、弹挂两个 1x1 + 一个 1x2、安全箱 4x3、背包 6x5。
        const auto MakeRoot = [](EInvContainerType Type, const TCHAR* Label, const TArray<FIntPoint>& Parts)
        {
            FInvRootContainerSetup Setup;
            Setup.Type = Type;
            Setup.Label = Label;
            Setup.Parts = Parts;
            return Setup;
        };

        InventoryComponent->RootContainers.Add(MakeRoot(EInvContainerType::Pockets, TEXT("口袋"),
            { FIntPoint(1, 1), FIntPoint(1, 1), FIntPoint(1, 1), FIntPoint(1, 1) }));
        InventoryComponent->RootContainers.Add(MakeRoot(EInvContainerType::ChestRig, TEXT("弹挂"),
            { FIntPoint(1, 1), FIntPoint(1, 1), FIntPoint(1, 2) }));
        InventoryComponent->RootContainers.Add(MakeRoot(EInvContainerType::SafeBox, TEXT("安全箱"),
            { FIntPoint(4, 3) }));
        InventoryComponent->RootContainers.Add(MakeRoot(EInvContainerType::Backpack, TEXT("背包"),
            { FIntPoint(6, 5) }));
    }
}

int64 AInvInventoryDemoActor::PlaceDemoItem(UInvItemDefinition* Definition, int32 Stack, int64 Container,
    int32 Part, int32 X, int32 Y, bool bRotated, int32& InOutFailures)
{
    if (!Definition)
    {
        ++InOutFailures;
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 演示装载失败：定义是空的（内置定义没建起来？）"), *GetName());
        return 0;
    }

    if (Container == 0)
    {
        ++InOutFailures;
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 演示装载 %s 失败：目标容器不存在（句柄 0）"),
            *GetName(), *Definition->GetName());
        return 0;
    }

    const int64 Item = InventoryComponent->GiveItem(Definition, Container, Part, X, Y, bRotated, Stack);
    if (Item <= 0)
    {
        ++InOutFailures;
        UE_LOG(LogTemp, Warning,
            TEXT("背包系统: %s 演示装载 %s ×%d 到容器 %lld 的 %d 号子网格 (%d,%d) 失败（返回 %lld）"),
            *GetName(), *Definition->GetName(), Stack, Container, Part, X, Y, Item);
    }
    return Item;
}

int32 AInvInventoryDemoActor::FillDemoLoadout()
{
    UInvInventoryComponent* Comp = InventoryComponent;
    int32 Failures = 0;

    const int64 Pockets = Comp->GetContainer(EInvContainerType::Pockets);
    const int64 ChestRig = Comp->GetContainer(EInvContainerType::ChestRig);
    const int64 Backpack = Comp->GetContainer(EInvContainerType::Backpack);

    // 口袋：0 号格弹药 ×60、1 号格急救包 ×3（剩下两个 1x1 空格留着看“.”）
    PlaceDemoItem(GetBuiltinDefinition(EBuiltinDefinition::Ammo), 60, Pockets, 0, 0, 0, false, Failures);
    PlaceDemoItem(GetBuiltinDefinition(EBuiltinDefinition::Medkit), 3, Pockets, 1, 0, 0, false, Failures);

    // 弹挂：0 号格手雷 ×1、第 3 块子网格（1x2 那个）弹匣 ×1（不旋转正好塞满）
    PlaceDemoItem(GetBuiltinDefinition(EBuiltinDefinition::Grenade), 1, ChestRig, 0, 0, 0, false, Failures);
    PlaceDemoItem(GetBuiltinDefinition(EBuiltinDefinition::Mag), 1, ChestRig, 2, 0, 0, false, Failures);

    // 背包：突击背包 3x3 落在 (0,0)
    const int64 BackpackItem = PlaceDemoItem(GetBuiltinDefinition(EBuiltinDefinition::Backpack), 1,
        Backpack, 0, 0, 0, false, Failures);

    // 套包内部：弹药 ×30，以及**旋转态**的弹匣（1x2 旋转后横向占 2 格，网格里一眼能看出宽高互换）
    FInvItemView BackpackView;
    if (BackpackItem > 0 && Comp->GetItemView(BackpackItem, BackpackView) && BackpackView.OwnContainer != 0)
    {
        const int64 Inner = BackpackView.OwnContainer;
        PlaceDemoItem(GetBuiltinDefinition(EBuiltinDefinition::Ammo), 30, Inner, 0, 0, 0, false, Failures);
        PlaceDemoItem(GetBuiltinDefinition(EBuiltinDefinition::Mag), 1, Inner, 0, 1, 0, true, Failures);
    }
    else
    {
        Failures += 2;  // 套包没落位 / 拿不到内部容器句柄，里面那两件也就不用试了
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 的突击背包没落位，套包内部的弹药与弹匣跳过"), *GetName());
    }

    return Failures;
}

// ==================== 摘要文本 ====================

FString AInvInventoryDemoActor::BuildSummaryText() const
{
    FString Out(TEXT("===== 背包演示 =====\n"));

    const UInvInventoryComponent* Comp = InventoryComponent;
    if (!Comp || !Comp->bReady)
    {
        Out += TEXT("（背包还没就绪：BeginPlay 没跑，或者初始化失败——看日志里的中文错误）\n");
        return Out;
    }

    // 字母表：按“首次出现”的顺序发 A、B、C…；同一件物品在所有网格里共用同一个字母。
    TMap<int64, FString> LetterByHandle;
    TArray<FInvItemView> Discovered;

    // 根容器按固定顺序画（口袋 → 弹挂 → 安全箱 → 背包）：同一份数据每次输出同一段文本，测试才好逐字比。
    static const EInvContainerType RootOrder[] = {
        EInvContainerType::Pockets, EInvContainerType::ChestRig,
        EInvContainerType::SafeBox, EInvContainerType::Backpack };
    for (const EInvContainerType Type : RootOrder)
    {
        const int64 Container = Comp->GetContainer(Type);
        if (Container <= 0)
        {
            continue;  // 0 = 没有这种根容器；负数是错误码
        }
        AppendContainerGrid(Comp, Out, Container, LetterByHandle, Discovered);
    }

    // 套包内部：按物品首次出现的顺序展开自带的容器。
    // RenderedContainers 防重复展开；MaxNestedSections 兜住配置出环的极端情况。
    TSet<int64> RenderedContainers;
    int32 NestedSections = 0;
    for (int32 Index = 0; Index < Discovered.Num() && NestedSections < MaxNestedSections; ++Index)
    {
        const int64 Inner = Discovered[Index].OwnContainer;
        if (Inner <= 0 || RenderedContainers.Contains(Inner))
        {
            continue;
        }
        RenderedContainers.Add(Inner);
        ++NestedSections;

        // 套包内部：标题用物品名（内部容器的 Label 是资产名，打出来没人看得懂），网格不再重复打标题
        Out += FString::Printf(TEXT("[套包内部] %s\n"), *DisplayNameOf(Discovered[Index].Definition));
        AppendContainerGrid(Comp, Out, Inner, LetterByHandle, Discovered, /*bPrintHeader=*/false);
    }

    // 物品清单：A = 弹药 ×60 (1x1)，一行放不下就换行
    FString ItemList;
    int32 ItemListLineLength = 0;
    for (const FInvItemView& View : Discovered)
    {
        const FString* Letter = LetterByHandle.Find(View.Handle);
        if (!Letter)
        {
            continue;
        }

        const UInvItemDefinition* Definition = View.Definition;
        // 尺寸写的是定义的原始宽高，旋转态只在后面标一句：网格里已经能看出宽高换过位了。
        const FString Size = Definition
            ? FString::Printf(TEXT("%dx%d%s"), Definition->Width, Definition->Height,
                View.bRotated ? TEXT(", 旋转") : TEXT(""))
            : FString(TEXT("定义缺失"));
        const FString Entry = FString::Printf(TEXT("%s = %s ×%d (%s)"),
            **Letter, *DisplayNameOf(Definition), View.Stack, *Size);

        if (ItemListLineLength > 0 && ItemListLineLength + Entry.Len() + 3 > ItemListLineWidth)
        {
            ItemList += TEXT("\n");
            ItemListLineLength = 0;
        }
        else if (ItemListLineLength > 0)
        {
            ItemList += TEXT("   ");
            ItemListLineLength += 3;
        }
        ItemList += Entry;
        ItemListLineLength += Entry.Len();
    }
    if (!ItemList.IsEmpty())
    {
        Out += ItemList + TEXT("\n");
    }

    // 汇总：件数含套包内部的东西（套包里的弹药 ×30 也算 1 件）
    const FString CapacityText = (Comp->MaxCapacityKg > 0.f)
        ? FString::Printf(TEXT("上限 %.0f kg"), Comp->MaxCapacityKg)
        : FString(TEXT("无上限"));
    Out += FString::Printf(TEXT("物品 %d 件 / 总重 %.3f kg / 总估值 %lld / 负重比 %.3f（%s）\n"),
        Comp->GetAllItems().Num(), Comp->GetTotalWeight(), Comp->GetTotalValue(), Comp->GetWeightRatio(),
        *CapacityText);

    if (LoadoutFailures > 0)
    {
        Out += FString::Printf(TEXT("注意：有 %d 件演示物品没落位（原因见日志）\n"), LoadoutFailures);
    }

    return Out;
}

// ==================== 屏幕刷新 ====================

float AInvInventoryDemoActor::ClampedRefreshInterval() const
{
    return FMath::Max(RefreshIntervalSeconds, MinRefreshIntervalSeconds);
}

void AInvInventoryDemoActor::RefreshOnScreen()
{
    if (!bDrawOnScreen || !GEngine)
    {
        return;
    }

    // 固定 key：每次刷新覆盖同一条消息（不会刷屏），多个演示 Actor 之间也各占一条。
    if (ScreenMessageKey == 0)
    {
        ScreenMessageKey = static_cast<uint64>(GetUniqueID());
        if (ScreenMessageKey == 0)
        {
            ScreenMessageKey = 1;  // 兜一手：GetUniqueID 理论上不会给 0
        }
    }

    const FString Text = bDemoInitialized
        ? BuildSummaryText()
        : FString(TEXT("背包演示：还没初始化（bAutoFillOnBeginPlay 关着，或者初始化失败——看日志）"));

    // 显示时长给两倍刷新间隔：下一次刷新会覆盖它；万一定时器停了，消息也不会立刻消失。
    GEngine->AddOnScreenDebugMessage(ScreenMessageKey, ClampedRefreshInterval() * 2.f, FColor(140, 255, 140), Text);
}

// ==================== 界面 ====================

void AInvInventoryDemoActor::CreateInventoryUI()
{
    // 命令集（无头）与专用服务器都没有本地 Player Controller：这不是错误，跳过就行。
    // 判定放在最前面，保证「没有 PC」这条路一条日志、零报错。
    UWorld* World = GetWorld();
    APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
    if (!PlayerController)
    {
        UE_LOG(LogTemp, Log,
            TEXT("背包系统: %s 想建背包界面，但当前没有 Player Controller（无头命令集 / 服务器），跳过"),
            *GetName());
        return;
    }

    if (!InventoryComponent)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 上没有 InventoryComponent，背包界面不建"), *GetName());
        return;
    }

    ScreenWidget = CreateWidget<UInvInventoryScreenWidget>(PlayerController,
        UInvInventoryScreenWidget::StaticClass(), TEXT("InventoryScreen"));
    if (!ScreenWidget)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 建背包界面控件失败（返回空）"), *GetName());
        return;
    }

    // 数据来源 = 自己的背包组件；其余布局参数走控件默认值。
    ScreenWidget->Inventory = InventoryComponent;
    ScreenWidget->AddToViewport();

    UE_LOG(LogTemp, Log, TEXT("背包系统: %s 建了背包界面 %s 并加进视口（拖拽移动 / R 旋转 / Esc 取消）"),
        *GetName(), *ScreenWidget->GetClass()->GetName());
}

void AInvInventoryDemoActor::DestroyInventoryUI()
{
    if (!ScreenWidget)
    {
        return;
    }

    ScreenWidget->RemoveFromParent();
    ScreenWidget = nullptr;
}
