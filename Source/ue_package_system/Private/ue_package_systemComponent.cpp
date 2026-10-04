#include "ue_package_systemComponent.h"

#include "ue_package_systemLibrary.h"

// Rust 侧的尺寸上限常量（MAX_DEF_DIM / MAX_PARTS）就写在 C ABI 头里，直接引用，
// 免得在 C++ 里手抄第二份数值再对着维护。
// 注意：这里只取常量；函数调用一律走 Uue_package_systemLibrary，ABI 仍然是单点。
#include "ue_package_system_ffi.h"

// 内容表来源：FInvItemDefinitionRow（行结构）+ UDataTable（表本身）。
// 注意组件头里只有 UDataTable 的前向声明（那里只需要 TObjectPtr），完整类型在这里才要。
#include "ue_package_systemItemTable.h"

#include "Engine/DataTable.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

// ==================== 生命周期 ====================

bool UInvInventoryComponent::InitializeInventory()
{
    // 幂等：重复调用直接返回，蓝图 / 测试可以放心重复调。
    if (bReady && Inventory && Inventory->IsValidHandle())
    {
        return true;
    }

    UInvInventory* NewInventory = UInvInventory::CreateInventory();
    if (!NewInventory)
    {
        bReady = false;
        UE_LOG(LogTemp, Error,
            TEXT("背包系统: %s 建背包实例失败（多半是 Rust 库没加载）。在插件目录跑 `uerust build .` 后重启编辑器。"),
            *DescribeOwner(this));
        return false;
    }

    Inventory = NewInventory;

    // 先置就绪、再做后续步骤：注册定义 / 建根容器内部都要过 GetReadyInventory 的就绪闸门。
    // 顺序写反过（置位放最后），结果是"配置里的定义一个都没注册上"，只有警告日志能看出来。
    bReady = true;

    // 顺序不能反：先注册定义（拿到 DefId），再建根容器（拿句柄），最后设承重上限。
    RegisterConfiguredDefinitions();
    CreateConfiguredRootContainers();

    if (MaxCapacityKg > 0.f)
    {
        Uue_package_systemLibrary::SetMaxCapacity(Inventory, MaxCapacityKg);
    }

    UE_LOG(LogTemp, Log, TEXT("背包系统: %s 就绪（定义 %d 个，根容器配置 %d 条，承重上限 %.2f kg）"),
        *DescribeOwner(this), DefinitionById.Num(), RootContainers.Num(), MaxCapacityKg);
    return true;
}

void UInvInventoryComponent::BeginPlay()
{
    Super::BeginPlay();

    InitializeInventory();
}

void UInvInventoryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 只清引用：底下的 Rust 实例由 UInvInventory 在 GC 回收时 BeginDestroy 里释放，
    // 这里不主动销毁（销毁了 GC 之后还会再碰一次）。ItemDefinitions 是设计师配置，不动。
    Inventory = nullptr;
    DefinitionById.Reset();
    IdByDefinition.Reset();
    NamedDefinitions.Reset();
    bReady = false;

    Super::EndPlay(EndPlayReason);
}

UInvInventory* UInvInventoryComponent::GetReadyInventory() const
{
    if (bReady && Inventory && Inventory->IsValidHandle())
    {
        return Inventory.Get();
    }

    UE_LOG(LogTemp, Warning,
        TEXT("背包系统: %s 的背包还没就绪（BeginPlay 没跑，或者建实例失败），本次调用已忽略"),
        *DescribeOwner(this));
    return nullptr;
}

// ==================== DefId 映射 ====================

int64 UInvInventoryComponent::RegisterDefinition(UInvItemDefinition* Definition)
{
    if (!Definition)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 调 RegisterDefinition 时传了空定义"), *DescribeOwner(this));
        return -(int64)ErrInvalidArgument;
    }

    // 注册过就返回缓存的号：幂等，也不会重复占用号段。
    if (const int64* Cached = IdByDefinition.Find(Definition))
    {
        return *Cached;
    }

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return -(int64)ErrInvalidHandle;
    }

    // —— 越界字段先夹住 ——
    // 一次失败的 define 不会占号，后面的定义会顶上它的 DefId，等于静默撕存档；
    // 所以这里宁可把设计师填错的值夹到合法范围，也要保证“能注册的定义一定注册得上”。
    const int32 Width = FMath::Clamp(Definition->Width, 1, (int32)MAX_DEF_DIM);
    const int32 Height = FMath::Clamp(Definition->Height, 1, (int32)MAX_DEF_DIM);
    if (Width != Definition->Width || Height != Definition->Height)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 定义 %s 的尺寸 (%d,%d) 越界，已夹到 (%d,%d)（合法范围 1~%d）"),
            *Definition->GetName(), Definition->Width, Definition->Height, Width, Height, (int32)MAX_DEF_DIM);
    }

    const TArray<FIntPoint> ContainerParts = SanitizeParts(Definition->ContainerParts, Definition->GetName());

    int32 MaxStack = FMath::Clamp(Definition->MaxStack, 1, MaxStackLimit);
    if (MaxStack != Definition->MaxStack)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 定义 %s 的 MaxStack %d 越界，已夹到 %d"),
            *Definition->GetName(), Definition->MaxStack, MaxStack);
    }

    // Rust 侧规则：能当容器的物品必须不可堆叠（套包没有“数量”概念），否则整条定义被判非法。
    if (ContainerParts.Num() > 0 && MaxStack != 1)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 定义 %s 带容器规格，MaxStack 已从 %d 改成 1（容器必须不可堆叠）"),
            *Definition->GetName(), MaxStack);
        MaxStack = 1;
    }

    float Weight = Definition->Weight;
    if (!FMath::IsFinite(Weight) || Weight < 0.f)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 定义 %s 的重量 %f 非法，已按 0 处理"), *Definition->GetName(), Weight);
        Weight = 0.f;
    }

    const int64 DefId = Uue_package_systemLibrary::DefineItem(Inv, Definition->GetName(), Width, Height,
        Definition->bRotatable, MaxStack, Weight, Definition->Value, Definition->TagBits,
        Definition->ForbiddenContainerTypes, ContainerParts);

    if (DefId < 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("背包系统: 注册定义 %s 失败（错误码 %lld）。这条定义不占号，后面的定义会顶上它的 DefId，存档会错位"),
            *Definition->GetName(), -DefId);
        return DefId;
    }

    DefinitionById.Add(DefId, Definition);
    IdByDefinition.Add(Definition, DefId);
    return DefId;
}

void UInvInventoryComponent::RegisterConfiguredDefinitions()
{
    // 注册顺序 = DefId = 存档的键，所以必须与数组顺序无关、与来源无关：
    // 先资产（按资产名大小写敏感升序，同名再按对象路径决胜——不同目录下可以有同名资产），
    // 再表行（按 RowName 升序，见 RegisterConfiguredDefinitionRows）。
    // 只排本地副本，不动设计师填的数组。
    TArray<UInvItemDefinition*> Sorted;
    Sorted.Reserve(ItemDefinitions.Num());
    for (const TObjectPtr<UInvItemDefinition>& Definition : ItemDefinitions)
    {
        if (Definition)
        {
            Sorted.Add(Definition.Get());
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 的 ItemDefinitions 里有空引用，已跳过"), *DescribeOwner(this));
        }
    }

    Sorted.Sort([](const UInvItemDefinition& A, const UInvItemDefinition& B)
        {
            const int32 ByName = A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive);
            if (ByName != 0)
            {
                return ByName < 0;
            }
            return A.GetPathName().Compare(B.GetPathName(), ESearchCase::CaseSensitive) < 0;
        });

    for (UInvItemDefinition* Definition : Sorted)
    {
        RegisterConfiguredDefinition(Definition, Definition->GetFName());
    }

    // 来源 2：内容表。**排在资产之后**——所以往已存档的项目里加一张表，老资产的 DefId 不变，
    // 新物品追加在末尾。
    RegisterConfiguredDefinitionRows();
}

void UInvInventoryComponent::RegisterConfiguredDefinition(UInvItemDefinition* Definition, FName LogicalName)
{
    // **先挂名再注册**：表行现场造出来的对象在注册期间只有 NamedDefinitions 这一处强引用
    // （UPROPERTY 防 GC），顺序反了就有被回收的风险。
    IndexDefinitionByName(LogicalName, Definition);

    if (RegisterDefinition(Definition) >= 0)
    {
        return;
    }

    // 注册失败（Rust 侧判非法之类）= 目录里根本没有它的号，不能留在按名索引里让调用方
    // 拿到一个 GetDefId 为 -9 的定义。摘之前确认那条就是我们刚挂的：同名冲突时赢的是别人，
    // 别顺手把别人的名字删了。
    const TObjectPtr<UInvItemDefinition>* Found = NamedDefinitions.Find(LogicalName);
    if (Found && Found->Get() == Definition)
    {
        NamedDefinitions.Remove(LogicalName);
    }
}

void UInvInventoryComponent::RegisterConfiguredDefinitionRows()
{
    if (!ItemDefinitionTable)
    {
        // 没配表是常态（老项目只有 ItemDefinitions），不是错误。
        return;
    }

    // 行结构选错了就整张表跳过：硬读会按错误的布局解释内存，比"少几个定义"危险得多。
    const UScriptStruct* RowStruct = ItemDefinitionTable->GetRowStruct();
    if (RowStruct != FInvItemDefinitionRow::StaticStruct())
    {
        UE_LOG(LogTemp, Error,
            TEXT("背包系统: %s 的 ItemDefinitionTable（%s）的行结构是 %s，不是 FInvItemDefinitionRow，整张表已跳过（其余来源照常注册）"),
            *DescribeOwner(this), *ItemDefinitionTable->GetName(),
            RowStruct ? *RowStruct->GetName() : TEXT("<未设置>"));
        return;
    }

    // GetRowNames 的返回顺序跟着内部 TMap 走，**不确定**；必须自己排，
    // 否则同一张表在不同次启动上会给出不同的 DefId（存档直接错位）。
    // FNameLexicalLess = 引擎自带的"字母序且跨进程稳定"比较器。
    TArray<FName> RowNames = ItemDefinitionTable->GetRowNames();
    RowNames.Sort(FNameLexicalLess());

    if (RowNames.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 的 ItemDefinitionTable（%s）是空表，没有注册任何表定义"),
            *DescribeOwner(this), *ItemDefinitionTable->GetName());
        return;
    }

    for (const FName& RowName : RowNames)
    {
        const FString Context = FString::Printf(TEXT("内容表 %s 的行 %s"),
            *ItemDefinitionTable->GetName(), *RowName.ToString());

        const FInvItemDefinitionRow* Row =
            ItemDefinitionTable->FindRow<FInvItemDefinitionRow>(RowName, Context, /*bWarnIfRowMissing=*/false);
        if (!Row)
        {
            UE_LOG(LogTemp, Error, TEXT("背包系统: %s 读不到 %s（行结构对得上却取不到，表可能被改过），已跳过这一行"),
                *DescribeOwner(this), *Context);
            continue;
        }

        // 行名当对象名：日志、调试器、Rust 侧目录里看到的都是 RowName，一眼能对上是哪一行。
        // 同名对象可能来自上一次初始化（EndPlay 后重新 InitializeInventory），先找再建，
        // 免得 NewObject 撞名。
        UInvItemDefinition* Definition = FindObjectFast<UInvItemDefinition>(this, RowName);
        if (!Definition)
        {
            Definition = NewObject<UInvItemDefinition>(this, RowName);
        }
        if (!Definition)
        {
            UE_LOG(LogTemp, Error, TEXT("背包系统: %s 造不出 %s 的定义对象，已跳过这一行"),
                *DescribeOwner(this), *Context);
            continue;
        }

        // 把行字段抄进定义（Icon 走软引用，不加载资产），挂名 + 注册。
        Row->ApplyToDefinition(*Definition);
        RegisterConfiguredDefinition(Definition, RowName);
    }
}

void UInvInventoryComponent::IndexDefinitionByName(FName Name, UInvItemDefinition* Definition)
{
    if (Name.IsNone() || !Definition)
    {
        return;
    }

    if (const TObjectPtr<UInvItemDefinition>* Existing = NamedDefinitions.Find(Name))
    {
        // 先注册的赢：它的 DefId 在前，按名取到的就得是它，否则 UI 和存档会对不上。
        if (Existing->Get() != Definition)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("背包系统: %s 的逻辑名 %s 已被 %s 占用，后注册的 %s 不覆盖（按名查找以先注册的为准）"),
                *DescribeOwner(this), *Name.ToString(),
                *DescribeDefinition(Existing->Get()), *DescribeDefinition(Definition));
        }
        return;
    }

    NamedDefinitions.Add(Name, Definition);
}

UInvItemDefinition* UInvInventoryComponent::GetDefinition(int32 DefId) const
{
    if (const TObjectPtr<UInvItemDefinition>* Found = DefinitionById.Find((int64)DefId))
    {
        return Found->Get();
    }
    return nullptr;
}

int64 UInvInventoryComponent::GetDefId(UInvItemDefinition* Definition) const
{
    if (!Definition)
    {
        return -(int64)ErrInvalidArgument;
    }

    if (const int64* Found = IdByDefinition.Find(Definition))
    {
        return *Found;
    }
    return -(int64)ErrUnknownDef;
}

UInvItemDefinition* UInvInventoryComponent::GetItemDefinition(int64 ItemHandle) const
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return nullptr;
    }

    FInvItemInfo Info;
    if (!Uue_package_systemLibrary::GetItemInfo(Inv, ItemHandle, Info))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 物品句柄 %lld 无效，取不到定义"), ItemHandle);
        return nullptr;
    }
    return GetDefinition(Info.DefId);
}

UInvItemDefinition* UInvInventoryComponent::FindDefinitionByName(FName Name) const
{
    if (const TObjectPtr<UInvItemDefinition>* Found = NamedDefinitions.Find(Name))
    {
        return Found->Get();
    }

    // 打警告而不是静默返回空：按名找不到基本都是配置问题（表行名打错 / 表没填进组件），
    // 静默的话表现是"这个物品就是不出来"，很难查。
    UE_LOG(LogTemp, Warning,
        TEXT("背包系统: %s 里没有叫 %s 的定义（找的是内容表行名或内容资产名；检查表有没有填进 ItemDefinitionTable）"),
        *DescribeOwner(this), *Name.ToString());
    return nullptr;
}

TArray<UInvItemDefinition*> UInvInventoryComponent::GetAllDefinitions() const
{
    // 按 DefId 升序 = 注册顺序 = 存档顺序。DefinitionById 是 TMap，直接遍历顺序不保证，
    // 所以先把号排出来再取。
    TArray<int64> DefIds;
    DefinitionById.GetKeys(DefIds);
    DefIds.Sort();

    TArray<UInvItemDefinition*> Out;
    Out.Reserve(DefIds.Num());
    for (const int64 DefId : DefIds)
    {
        if (const TObjectPtr<UInvItemDefinition>* Found = DefinitionById.Find(DefId))
        {
            Out.Add(Found->Get());
        }
    }
    return Out;
}

// ==================== 容器 ====================

int64 UInvInventoryComponent::GetContainer(EInvContainerType Type) const
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return -(int64)ErrInvalidHandle;
    }

    // 根容器最多 4 个，每次扫一遍比缓存句柄安全：读档（Restore）之后句柄会整体重排，
    // 缓存下来的旧句柄会变成野句柄。
    for (const int64 Handle : Uue_package_systemLibrary::GetContainers(Inv))
    {
        FInvContainerInfo Info;
        if (Uue_package_systemLibrary::GetContainerInfo(Inv, Handle, Info) && Info.bRoot && Info.Type == Type)
        {
            return Handle;
        }
    }
    return 0;  // 0 = “没有这种根容器”，不是错误码
}

TArray<FIntPoint> UInvInventoryComponent::GetContainerParts(int64 Container) const
{
    TArray<FIntPoint> Parts;

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return Parts;
    }

    FInvContainerInfo Info;
    if (!Uue_package_systemLibrary::GetContainerInfo(Inv, Container, Info))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 容器句柄 %lld 无效，取不到子网格"), Container);
        return Parts;
    }

    Parts.Reserve(Info.PartCount);
    for (int32 Part = 0; Part < Info.PartCount; ++Part)
    {
        int32 PartWidth = 0;
        int32 PartHeight = 0;
        if (Uue_package_systemLibrary::GetContainerPartSize(Inv, Container, Part, PartWidth, PartHeight))
        {
            Parts.Add(FIntPoint(PartWidth, PartHeight));
        }
    }
    return Parts;
}

FString UInvInventoryComponent::GetContainerLabel(int64 Container) const
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return FString();
    }

    const FString Label = Uue_package_systemLibrary::GetContainerLabel(Inv, Container);
    if (Label.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 容器句柄 %lld 取不到名称（句柄无效，或者创建时没给 Label）"), Container);
    }
    return Label;
}

TArray<int64> UInvInventoryComponent::GetContainerGrid(int64 Container, int32 Part) const
{
    TArray<int64> Cells;

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return Cells;
    }

    if (!Uue_package_systemLibrary::GetContainerGrid(Inv, Container, Part, Cells))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 容器 %lld 的 %d 号子网格取不到占格表（句柄或子网格下标无效）"),
            Container, Part);
        Cells.Reset();  // 失败给空表，别把半截数据丢给 UI
    }
    return Cells;
}

bool UInvInventoryComponent::GetContainerInfo(int64 Container, FInvContainerInfo& OutInfo) const
{
    OutInfo = FInvContainerInfo();

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return false;
    }

    if (!Uue_package_systemLibrary::GetContainerInfo(Inv, Container, OutInfo))
    {
        OutInfo = FInvContainerInfo();
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 容器句柄 %lld 无效"), Container);
        return false;
    }
    return true;
}

int32 UInvInventoryComponent::AutosortContainer(int64 Container)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return ErrInvalidHandle;
    }
    return Uue_package_systemLibrary::Autosort(Inv, Container);
}

// ==================== 物品 ====================

int64 UInvInventoryComponent::GiveItem(UInvItemDefinition* Definition, int64 Container, int32 Part,
    int32 X, int32 Y, bool bRotated, int32 Stack)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return -(int64)ErrInvalidHandle;
    }

    // 顺手把没注册过的定义补进目录，省得蓝图还得先手动注册一次。
    const int64 DefId = RegisterDefinition(Definition);
    if (DefId < 0)
    {
        return DefId;
    }

    // 数量下限夹到 1：Rust 侧 stack == 0 直接判 InvalidStackCount(18)；
    // 上限不夹，超了就是超了，让调用方看到错误码（比如叠到 MaxStack 以上）。
    const int64 Item = Uue_package_systemLibrary::AddItem(Inv, (int32)DefId, FMath::Max(Stack, 1), 0,
        Container, Part, X, Y, bRotated);

    if (Item < 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 把 %s 放进容器 %lld 的 %d 号子网格 (%d,%d) 失败（错误码 %lld）"),
            *DescribeDefinition(Definition), Container, Part, X, Y, -Item);
    }
    return Item;
}

bool UInvInventoryComponent::GetItemView(int64 ItemHandle, FInvItemView& OutView) const
{
    OutView = FInvItemView();

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return false;
    }

    FInvItemInfo Info;
    if (!Uue_package_systemLibrary::GetItemInfo(Inv, ItemHandle, Info))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 物品句柄 %lld 无效，取不到视图（是不是读档前的旧句柄？）"), ItemHandle);
        return false;
    }

    OutView = BuildView(ItemHandle, Info);
    return true;
}

TArray<FInvItemView> UInvInventoryComponent::GetContainerItemsView(int64 Container) const
{
    TArray<FInvItemView> Views;

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return Views;
    }

    FInvContainerInfo ContainerInfo;
    if (!Uue_package_systemLibrary::GetContainerInfo(Inv, Container, ContainerInfo))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 容器句柄 %lld 无效，取不到物品列表"), Container);
        return Views;
    }

    // 顶层物品 = HostContainer 正好是这个容器的物品。套包里的东西 HostContainer 是内部容器句柄，
    // 自然被排掉，不用再走一遍网格去重。
    const TArray<int64> Items = Uue_package_systemLibrary::GetItems(Inv);
    Views.Reserve(Items.Num());
    for (const int64 ItemHandle : Items)
    {
        FInvItemInfo Info;
        if (!Uue_package_systemLibrary::GetItemInfo(Inv, ItemHandle, Info))
        {
            continue;
        }
        if (Info.HostContainer != Container)
        {
            continue;
        }
        Views.Add(BuildView(ItemHandle, Info));
    }
    return Views;
}

int32 UInvInventoryComponent::MoveItem(int64 Item, int64 Dst, int32 Part, int32 X, int32 Y, bool bRotated)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return ErrInvalidHandle;
    }
    return Uue_package_systemLibrary::TryMove(Inv, Item, Dst, Part, X, Y, bRotated);
}

int32 UInvInventoryComponent::SwapItems(int64 A, int64 B)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return ErrInvalidHandle;
    }
    return Uue_package_systemLibrary::TrySwap(Inv, A, B);
}

int32 UInvInventoryComponent::RotateItem(int64 Item)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return ErrInvalidHandle;
    }
    return Uue_package_systemLibrary::TryRotate(Inv, Item);
}

int32 UInvInventoryComponent::SplitStack(int64 Item, int32 Count, int64 Dst, int32 Part, int32 X, int32 Y,
    bool bRotated)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return ErrInvalidHandle;
    }

    // 低层返回的是新物品句柄（< 0 = -错误码）；这一层只要成败，
    // 新句柄用 GetContainerItemsView 重取即可（拖拽 UI 本来就要刷新列表）。
    const int64 NewItem = Uue_package_systemLibrary::TrySplit(Inv, Item, Count, Dst, Part, X, Y, bRotated);
    return NewItem < 0 ? (int32)(-NewItem) : 0;
}

int32 UInvInventoryComponent::MergeStacks(int64 Src, int64 Dst)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return ErrInvalidHandle;
    }

    int32 Moved = 0;
    return Uue_package_systemLibrary::TryMerge(Inv, Src, Dst, Moved);
}

int64 UInvInventoryComponent::DestroyItem(int64 Item)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return -(int64)ErrInvalidHandle;
    }
    return Uue_package_systemLibrary::DestroyItem(Inv, Item);
}

// ==================== 自动落位 ====================

bool UInvInventoryComponent::IsAreaFree(const TArray<int64>& Occupancy, FIntPoint PartSize, FIntPoint Cell,
    FIntPoint Cells)
{
    for (int32 Y = 0; Y < Cells.Y; ++Y)
    {
        for (int32 X = 0; X < Cells.X; ++X)
        {
            const int32 GridX = Cell.X + X;
            const int32 GridY = Cell.Y + Y;
            if (GridX < 0 || GridY < 0 || GridX >= PartSize.X || GridY >= PartSize.Y)
            {
                return false;  // 越界：调用方该先夹到放得下的位置，这里只兜底
            }
            if (Occupancy[GridY * PartSize.X + GridX] != 0)
            {
                return false;  // 这一格压着东西（不管是哪一件，包括要挪的它自己）
            }
        }
    }
    return true;
}

bool UInvInventoryComponent::FindFreeCell(int64 Container, int32 Part, FIntPoint Size, bool bRotated,
    FIntPoint& OutCell) const
{
    OutCell = FIntPoint(0, 0);

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return false;
    }

    // 占位尺寸：旋转时交换长宽（与 UInvGridWidget::FootprintCells 同一套约定）。
    const FIntPoint Cells = bRotated ? FIntPoint(Size.Y, Size.X) : Size;
    if (Cells.X <= 0 || Cells.Y <= 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("背包系统: FindFreeCell 的占位尺寸 (%d,%d) 非法（每边至少 1 格）"), Size.X, Size.Y);
        return false;
    }

    // 负数下标必须在这里挡住：低层 `GetContainerPartSize` 会把负数夹成 0（那里是给蓝图容错的），
    // 对「找空位」这种查询来说静默当成 0 号子网格比报 false 更让人意外。
    if (Part < 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: FindFreeCell 的子网格下标 %d 非法（从 0 起）"), Part);
        return false;
    }

    int32 PartWidth = 0;
    int32 PartHeight = 0;
    if (!Uue_package_systemLibrary::GetContainerPartSize(Inv, Container, Part, PartWidth, PartHeight))
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: FindFreeCell 取不到容器 %lld 的 %d 号子网格（句柄或下标无效）"),
            Container, Part);
        return false;
    }
    const FIntPoint PartSize(PartWidth, PartHeight);

    if (Cells.X > PartSize.X || Cells.Y > PartSize.Y)
    {
        // 比整块子网格还大：不是「碎片化」，是这个尺寸根本放不下（不刷屏，不打日志）。
        return false;
    }

    const TArray<int64> Occupancy = GetContainerGrid(Container, Part);
    if (Occupancy.Num() != PartSize.X * PartSize.Y)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("背包系统: FindFreeCell 拿到的占格表有 %d 格，与子网格 %dx%d 不符（容器 %lld 的 %d 号）"),
            Occupancy.Num(), PartSize.X, PartSize.Y, Container, Part);
        return false;
    }

    // 行主序：先上后下、先左后右，命中即返回（左上角优先，与「第一个空位」的直觉一致）。
    for (int32 Y = 0; Y <= PartSize.Y - Cells.Y; ++Y)
    {
        for (int32 X = 0; X <= PartSize.X - Cells.X; ++X)
        {
            const FIntPoint Candidate(X, Y);
            if (IsAreaFree(Occupancy, PartSize, Candidate, Cells))
            {
                OutCell = Candidate;
                return true;
            }
        }
    }
    return false;  // 碎片化：哪块空区域都塞不下这个尺寸
}

int32 UInvInventoryComponent::TryAutoPlace(int64 Item, int64 Container)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return ErrInvalidHandle;
    }

    if (Item <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: TryAutoPlace 的物品句柄 %lld 非法"), Item);
        return ErrInvalidArgument;
    }

    FInvItemView View;
    if (!GetItemView(Item, View))  // 找不到句柄时内部已经打过中文日志
    {
        return ErrInvalidHandle;
    }

    // 没落位的物品没有占格可查，MoveItem 也会按 5（NotPlaced）拒掉：先明说，别让调用方猜。
    if (View.HostContainer <= 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("背包系统: TryAutoPlace 的物品 %lld 还没落位（NotPlaced）——先把它放进某个容器再用自动落位"), Item);
        return ErrNotPlaced;
    }

    const UInvItemDefinition* Definition = View.Definition;
    if (!Definition)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: TryAutoPlace 取不到物品 %lld 的定义（DefId 对不上），算不出占位尺寸"), Item);
        return ErrInvalidArgument;
    }

    const TArray<FIntPoint> Parts = GetContainerParts(Container);  // 容器无效时内部会打警告
    if (Parts.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: TryAutoPlace 的目标容器 %lld 没有可用子网格（句柄无效？）"), Container);
        return ErrInvalidHandle;
    }

    // 未旋转尺寸（宽, 高）；旋转后的交换由 FindFreeCell 按 bRotated 处理。
    const FIntPoint Size(FMath::Max(Definition->Width, 1), FMath::Max(Definition->Height, 1));

    int32 FirstFailure = 0;  // 「有空位但 MoveItem 拒了」时记下第一个错误码，比笼统报「满」准确

    for (int32 Part = 0; Part < Parts.Num(); ++Part)
    {
        // 朝向候选：当前朝向在前（尽量不改变玩家看到的样子）；定义可旋转时才追一个换向。
        const bool Rotations[2] = { View.bRotated, !View.bRotated };
        const int32 RotationCount = Definition->bRotatable ? 2 : 1;

        for (int32 Rotation = 0; Rotation < RotationCount; ++Rotation)
        {
            const bool bRotated = Rotations[Rotation];

            FIntPoint Cell;
            if (!FindFreeCell(Container, Part, Size, bRotated, Cell))
            {
                continue;
            }

            const int32 Result = MoveItem(Item, Container, Part, Cell.X, Cell.Y, bRotated);
            if (Result == 0)
            {
                UE_LOG(LogTemp, Log,
                    TEXT("背包系统: 自动落位成功——物品 %lld → 容器 %lld 的 %d 号子网格 (%d,%d)%s"),
                    Item, Container, Part, Cell.X, Cell.Y, bRotated ? TEXT(" 旋转") : TEXT(""));
                return 0;  // 首个命中即落位
            }

            if (FirstFailure == 0)
            {
                FirstFailure = Result;
            }
            UE_LOG(LogTemp, Log,
                TEXT("背包系统: 自动落位在容器 %lld 的 %d 号子网格 (%d,%d)%s 被拒（错误码 %d），继续找下一块"),
                Container, Part, Cell.X, Cell.Y, bRotated ? TEXT(" 旋转") : TEXT(""), Result);
        }
    }

    if (FirstFailure != 0)
    {
        // 有空位却落不下去（黑名单 / 成环 / 深度…）：原样报出去，别谎报「满」。
        return FirstFailure;
    }

    UE_LOG(LogTemp, Log, TEXT("背包系统: 自动落位失败——物品 %lld 在容器 %lld 里没有放得下的空位（全满或碎片化），位置保持不变"),
        Item, Container);
    return ErrOccupied;  // 2 = Occupied：所有子网格两个朝向都放不下
}

// ==================== 负重 ====================

float UInvInventoryComponent::GetTotalWeight() const
{
    UInvInventory* Inv = GetReadyInventory();
    return Inv ? Uue_package_systemLibrary::GetTotalWeight(Inv) : 0.f;
}

int64 UInvInventoryComponent::GetTotalValue() const
{
    UInvInventory* Inv = GetReadyInventory();
    return Inv ? Uue_package_systemLibrary::GetTotalValue(Inv) : 0;
}

float UInvInventoryComponent::GetWeightRatio() const
{
    UInvInventory* Inv = GetReadyInventory();
    return Inv ? Uue_package_systemLibrary::GetWeightRatio(Inv) : 0.f;
}

bool UInvInventoryComponent::IsOverloaded() const
{
    UInvInventory* Inv = GetReadyInventory();
    return Inv ? Uue_package_systemLibrary::IsOverloaded(Inv) : false;
}

void UInvInventoryComponent::SetMaxCapacity(float Kg)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return;
    }

    Uue_package_systemLibrary::SetMaxCapacity(Inv, Kg);

    // 同步到配置字段：UI / 存读档读“当前上限”时不用另找地方放。
    MaxCapacityKg = Kg;
}

// ==================== 查询 ====================

TArray<FInvItemView> UInvInventoryComponent::FindItemsByDefinition(UInvItemDefinition* Definition) const
{
    TArray<FInvItemView> Views;

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return Views;
    }

    const int64* FoundId = Definition ? IdByDefinition.Find(Definition) : nullptr;
    if (!FoundId)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 定义 %s 还没注册，不可能有实例"),
            *DescribeDefinition(Definition));
        return Views;
    }
    const int32 DefId = (int32)*FoundId;

    // 全量遍历按 DefId 比对：DefId 就是身份，比让设计师去填 TagBits 掩码可靠。
    for (const int64 ItemHandle : Uue_package_systemLibrary::GetItems(Inv))
    {
        FInvItemInfo Info;
        if (!Uue_package_systemLibrary::GetItemInfo(Inv, ItemHandle, Info))
        {
            continue;
        }
        if (Info.DefId != DefId)
        {
            continue;
        }
        Views.Add(BuildView(ItemHandle, Info));
    }
    return Views;
}

TArray<int64> UInvInventoryComponent::GetAllContainers() const
{
    UInvInventory* Inv = GetReadyInventory();
    return Inv ? Uue_package_systemLibrary::GetContainers(Inv) : TArray<int64>();
}

TArray<int64> UInvInventoryComponent::GetAllItems() const
{
    UInvInventory* Inv = GetReadyInventory();
    return Inv ? Uue_package_systemLibrary::GetItems(Inv) : TArray<int64>();
}

// ==================== 存档 ====================

bool UInvInventoryComponent::SaveToBytes(TArray<uint8>& OutBytes) const
{
    OutBytes.Reset();

    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return false;
    }

    if (!Uue_package_systemLibrary::Snapshot(Inv, OutBytes))
    {
        OutBytes.Reset();
        UE_LOG(LogTemp, Error, TEXT("背包系统: %s 导出快照失败"), *DescribeOwner(this));
        return false;
    }
    return true;
}

bool UInvInventoryComponent::LoadFromBytes(const TArray<uint8>& Bytes)
{
    UInvInventory* Inv = GetReadyInventory();
    if (!Inv)
    {
        return false;
    }

    if (!Uue_package_systemLibrary::Restore(Inv, Bytes))
    {
        UE_LOG(LogTemp, Error,
            TEXT("背包系统: %s 读档失败（多半是错误码 20：读档前必须按存档时的顺序注册好同一批 ItemDefinitions）"),
            *DescribeOwner(this));
        return false;
    }
    return true;
}

// ==================== 内部 ====================

void UInvInventoryComponent::CreateConfiguredRootContainers()
{
    if (RootContainers.Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("背包系统: %s 的 RootContainers 是空的，没有创建任何根容器（口袋 / 弹挂 / 安全箱 / 背包都不存在）"),
            *DescribeOwner(this));
        return;
    }

    for (const FInvRootContainerSetup& Setup : RootContainers)
    {
        if (Setup.Type == EInvContainerType::Nested)
        {
            UE_LOG(LogTemp, Error, TEXT("背包系统: %s 的根容器「%s」类型填了 Nested，Nested 是套包内部用的，已跳过"),
                *DescribeOwner(this), *Setup.Label);
            continue;
        }

        const FString Context = FString::Printf(TEXT("根容器「%s」"), *Setup.Label);
        const TArray<FIntPoint> Parts = SanitizeParts(Setup.Parts, Context);
        if (Parts.Num() == 0)
        {
            UE_LOG(LogTemp, Error, TEXT("背包系统: %s 的 %s 没填子网格（Rust 侧要求至少一块，比如背包 (6,5)），已跳过"),
                *DescribeOwner(this), *Context);
            continue;
        }

        const int64 Handle = Uue_package_systemLibrary::AddRoot(Inventory, Setup.Type, Setup.Label, Parts);
        if (Handle < 0)
        {
            UE_LOG(LogTemp, Error,
                TEXT("背包系统: %s 创建 %s 失败（错误码 %lld；同类型的根容器只能建一个，重复会返回 12）"),
                *DescribeOwner(this), *Context, -Handle);
        }
    }
}

FInvItemView UInvInventoryComponent::BuildView(int64 ItemHandle, const FInvItemInfo& Info) const
{
    FInvItemView View;
    View.Handle = ItemHandle;
    View.Definition = GetDefinition(Info.DefId);
    View.Stack = Info.Stack;
    View.Durability = Info.Durability;
    View.OwnContainer = Info.OwnContainer;
    View.HostContainer = Info.HostContainer;
    View.HostPart = Info.HostPart;
    View.X = Info.X;
    View.Y = Info.Y;
    View.bRotated = Info.bRotated;
    return View;
}

TArray<FIntPoint> UInvInventoryComponent::SanitizeParts(const TArray<FIntPoint>& Parts, const FString& Context)
{
    TArray<FIntPoint> Out;
    if (Parts.Num() == 0)
    {
        // 空 = 没有子网格（普通物品 / 没有口袋的根容器），不是错误。
        return Out;
    }

    if (Parts.Num() > (int32)MAX_PARTS)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: %s 的子网格有 %d 块，超过上限 %d 块，多余的已忽略"),
            *Context, Parts.Num(), (int32)MAX_PARTS);
    }

    const int32 Count = FMath::Min(Parts.Num(), (int32)MAX_PARTS);
    Out.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        // FIntPoint 在这里读作 (宽, 高)，每边 1~MAX_DEF_DIM 才算合法。
        const FIntPoint& Part = Parts[Index];
        const int32 PartWidth = FMath::Clamp(Part.X, 1, (int32)MAX_DEF_DIM);
        const int32 PartHeight = FMath::Clamp(Part.Y, 1, (int32)MAX_DEF_DIM);
        if (PartWidth != Part.X || PartHeight != Part.Y)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("背包系统: %s 的第 %d 块子网格 (%d,%d) 越界，已夹到 (%d,%d)（合法范围 1~%d）"),
                *Context, Index, Part.X, Part.Y, PartWidth, PartHeight, (int32)MAX_DEF_DIM);
        }
        Out.Add(FIntPoint(PartWidth, PartHeight));
    }
    return Out;
}

FString UInvInventoryComponent::DescribeOwner(const UActorComponent* Component)
{
    const AActor* Owner = Component ? Component->GetOwner() : nullptr;
    return Owner ? Owner->GetName() : FString(TEXT("<无归属组件>"));
}

FString UInvInventoryComponent::DescribeDefinition(const UInvItemDefinition* Definition)
{
    return Definition ? Definition->GetName() : FString(TEXT("<空定义>"));
}
