#include "ue_package_systemLibrary.h"

#include "ue_package_system.h"
#include "Containers/StringConv.h"
#include "HAL/UnrealMemory.h"

namespace
{
    /** FFI 层错误码：句柄无效（与 Rust 侧 InventoryError::InvalidHandle 一致）。 */
    constexpr int32 ErrInvalidHandle = 23;

    /** 校验背包包装对象并取出 ABI 表；不合法返回 nullptr。 */
    const Fue_package_systemFfi* ResolveFfi(const UInvInventory* Inv)
    {
        if (!Inv || !Inv->IsValidHandle())
        {
            return nullptr;
        }
        const Fue_package_systemFfi* Ffi = Inv->Ffi();
        return (Ffi && Ffi->IsValid()) ? Ffi : nullptr;
    }

    void ReportInvalidHandle()
    {
        UE_LOG(LogTemp, Warning,
            TEXT("背包系统: 传入的背包句柄无效（UInvInventory 为空，或没持有 Rust 实例）"));
    }

    /**
     * TArray<FIntPoint> -> C 侧的 (宽,高) 字节数组，每点两个字节。
     * 空数组返回 nullptr / 0（表示“普通物品”，或让 Rust 侧按参数非法处理）。
     * Scratch 必须在调用期间存活。
     */
    const uint8* PackDims(const TArray<FIntPoint>& Parts, TArray<uint8>& Scratch, uint32& OutPartCount)
    {
        OutPartCount = (uint32)Parts.Num();
        if (Parts.Num() == 0)
        {
            return nullptr;
        }
        Scratch.Reset();
        Scratch.Reserve(Parts.Num() * 2);
        for (const FIntPoint& P : Parts)
        {
            Scratch.Add((uint8)FMath::Clamp(P.X, 0, 255));
            Scratch.Add((uint8)FMath::Clamp(P.Y, 0, 255));
        }
        return Scratch.GetData();
    }

    /**
     * 两段式 u64 数组调用：先 cap=0 问数量，再按数量取。
     * 失败时 Out 清空。Call 的签名必须是 (uint64* Out, uint32 Cap) -> int64。
     */
    template <typename TCall>
    bool CollectU64(TCall&& Call, TArray<int64>& Out)
    {
        Out.Reset();

        const int64 Count = Call(nullptr, 0u);
        if (Count < 0)
        {
            return false;
        }
        if (Count == 0)
        {
            return true;
        }

        Out.SetNumUninitialized((int32)Count);
        const int64 Got = Call(reinterpret_cast<uint64*>(Out.GetData()), (uint32)Count);
        if (Got < 0)
        {
            Out.Reset();
            return false;
        }
        Out.SetNum((int32)Got);
        return true;
    }
}

// ==================== 原有：自检 / 版本 ====================

FString Uue_package_systemLibrary::InventorySelfTest()
{
    const Fue_package_systemFfi& Ffi = Fue_package_systemModule::GetFfi();

    if (!Ffi.IsValid())
    {
        return TEXT("Rust 库没加载成功。在插件目录跑 `uerust build .`，然后重启编辑器。");
    }

    char* Raw = Ffi.SelfTest();
    if (!Raw)
    {
        // 拿不到报告时，尽量把 Rust 侧记录的最后一条错误带出来
        const char* LastErr = Ffi.LastError();
        if (LastErr && LastErr[0] != '\0')
        {
            return FString(UTF8_TO_TCHAR(LastErr));
        }
        return TEXT("Rust 侧返回了空指针");
    }

    FString Result(UTF8_TO_TCHAR(Raw));
    // Rust 堆上的内存必须立刻交回 Rust 释放，不能在 C++ 侧 delete
    Ffi.FreeString(Raw);
    return Result;
}

FString Uue_package_systemLibrary::PluginVersion()
{
    const Fue_package_systemFfi& Ffi = Fue_package_systemModule::GetFfi();

    if (!Ffi.IsValid())
    {
        return TEXT("Rust 库没加载成功。在插件目录跑 `uerust build .`，然后重启编辑器。");
    }

    // 版本号是静态字符串，不需要释放
    return FString(UTF8_TO_TCHAR(Ffi.PluginVersion()));
}

FString Uue_package_systemLibrary::LastError()
{
    const Fue_package_systemFfi& Ffi = Fue_package_systemModule::GetFfi();
    if (!Ffi.IsValid() || !Ffi.LastError)
    {
        return FString();
    }
    const char* Raw = Ffi.LastError();
    // uerust_last_error 返回的是线程本地 CString，随时可能被下一次调用覆盖，不需要也不能释放
    return Raw ? FString(UTF8_TO_TCHAR(Raw)) : FString();
}

// ==================== 目录 / 容器 / 物品 ====================

int64 Uue_package_systemLibrary::DefineItem(UInvInventory* Inv, const FString& Name, int32 Width,
    int32 Height, bool bRotatable, int32 MaxStack, float Weight, int32 Value, int32 TagBits,
    int32 ForbiddenContainerTypes, const TArray<FIntPoint>& ContainerParts)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return -(int64)ErrInvalidHandle;
    }

    FTCHARToUTF8 NameUtf8(*Name);

    TArray<uint8> DimsScratch;
    uint32 PartCount = 0;
    const uint8* Dims = PackDims(ContainerParts, DimsScratch, PartCount);

    return Ffi->DefineItem(
        Inv->GetNativeHandle(),
        NameUtf8.Get(),
        (uint8)FMath::Clamp(Width, 0, 255),
        (uint8)FMath::Clamp(Height, 0, 255),
        bRotatable ? 1u : 0u,
        (uint32)FMath::Max(MaxStack, 0),
        Weight,
        (uint32)FMath::Max(Value, 0),
        (uint32)FMath::Max(TagBits, 0),
        (uint32)FMath::Clamp(ForbiddenContainerTypes, 0, 255),
        Dims,
        PartCount);
}

int64 Uue_package_systemLibrary::AddRoot(UInvInventory* Inv, EInvContainerType Type,
    const FString& Label, const TArray<FIntPoint>& Parts)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return -(int64)ErrInvalidHandle;
    }

    FTCHARToUTF8 LabelUtf8(*Label);

    TArray<uint8> DimsScratch;
    uint32 PartCount = 0;
    const uint8* Dims = PackDims(Parts, DimsScratch, PartCount);

    return Ffi->AddRoot(Inv->GetNativeHandle(), (uint32)Type, LabelUtf8.Get(), Dims, PartCount);
}

int64 Uue_package_systemLibrary::AddItem(UInvInventory* Inv, int32 DefId, int32 Stack,
    int32 Durability, int64 Container, int32 Part, int32 X, int32 Y, bool bRotated)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return -(int64)ErrInvalidHandle;
    }

    return Ffi->AddItem(
        Inv->GetNativeHandle(),
        (uint32)FMath::Max(DefId, 0),
        (uint32)FMath::Max(Stack, 0),
        (uint32)FMath::Max(Durability, 0),
        (uint64)Container,
        (uint32)FMath::Max(Part, 0),
        (uint32)FMath::Max(X, 0),
        (uint32)FMath::Max(Y, 0),
        bRotated ? 1u : 0u);
}

int64 Uue_package_systemLibrary::DestroyItem(UInvInventory* Inv, int64 Item)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return -(int64)ErrInvalidHandle;
    }

    return Ffi->DestroyItem(Inv->GetNativeHandle(), (uint64)Item);
}

// ==================== 原子交互 ====================

int32 Uue_package_systemLibrary::TryMove(UInvInventory* Inv, int64 Item, int64 Destination,
    int32 Part, int32 X, int32 Y, bool bRotated)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return ErrInvalidHandle;
    }

    return Ffi->TryMove(
        Inv->GetNativeHandle(),
        (uint64)Item,
        (uint64)Destination,
        (uint32)FMath::Max(Part, 0),
        (uint32)FMath::Max(X, 0),
        (uint32)FMath::Max(Y, 0),
        bRotated ? 1u : 0u);
}

int32 Uue_package_systemLibrary::TrySwap(UInvInventory* Inv, int64 A, int64 B)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return ErrInvalidHandle;
    }

    return Ffi->TrySwap(Inv->GetNativeHandle(), (uint64)A, (uint64)B);
}

int32 Uue_package_systemLibrary::TryRotate(UInvInventory* Inv, int64 Item)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return ErrInvalidHandle;
    }

    return Ffi->TryRotate(Inv->GetNativeHandle(), (uint64)Item);
}

int32 Uue_package_systemLibrary::TryMerge(UInvInventory* Inv, int64 Src, int64 Dst, int32& OutMoved)
{
    OutMoved = 0;

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return ErrInvalidHandle;
    }

    uint32 Moved = 0;
    const int32 Rc = Ffi->TryMerge(Inv->GetNativeHandle(), (uint64)Src, (uint64)Dst, &Moved);
    OutMoved = (int32)Moved;
    return Rc;
}

int64 Uue_package_systemLibrary::TrySplit(UInvInventory* Inv, int64 Item, int32 Count,
    int64 Destination, int32 Part, int32 X, int32 Y, bool bRotated)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return -(int64)ErrInvalidHandle;
    }

    return Ffi->TrySplit(
        Inv->GetNativeHandle(),
        (uint64)Item,
        (uint32)FMath::Max(Count, 0),
        (uint64)Destination,
        (uint32)FMath::Max(Part, 0),
        (uint32)FMath::Max(X, 0),
        (uint32)FMath::Max(Y, 0),
        bRotated ? 1u : 0u);
}

int32 Uue_package_systemLibrary::Autosort(UInvInventory* Inv, int64 Container)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return ErrInvalidHandle;
    }

    return Ffi->Autosort(Inv->GetNativeHandle(), (uint64)Container);
}

// ==================== 查询 ====================

bool Uue_package_systemLibrary::GetItemInfo(UInvInventory* Inv, int64 Item, FInvItemInfo& OutInfo)
{
    OutInfo = FInvItemInfo{};

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return false;
    }

    FfiItemInfo Raw{};
    if (Ffi->ItemInfo(Inv->GetNativeHandle(), (uint64)Item, &Raw) != 0)
    {
        return false;
    }

    OutInfo.DefId = (int32)Raw.def_id;
    OutInfo.Stack = (int32)Raw.stack;
    OutInfo.Durability = (int32)Raw.durability;
    OutInfo.OwnContainer = (int64)Raw.own_container;
    OutInfo.bPlaced = Raw.is_placed != 0;
    OutInfo.HostContainer = (int64)Raw.host_container;
    OutInfo.HostPart = (int32)Raw.host_part;
    OutInfo.X = (int32)Raw.x;
    OutInfo.Y = (int32)Raw.y;
    OutInfo.bRotated = Raw.rotated != 0;
    return true;
}

bool Uue_package_systemLibrary::GetContainerInfo(UInvInventory* Inv, int64 Container,
    FInvContainerInfo& OutInfo)
{
    OutInfo = FInvContainerInfo{};

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return false;
    }

    FfiContainerInfo Raw{};
    if (Ffi->ContainerInfo(Inv->GetNativeHandle(), (uint64)Container, &Raw) != 0)
    {
        return false;
    }

    OutInfo.Type = (EInvContainerType)FMath::Clamp((int32)Raw.ctype, 0, 4);
    OutInfo.PartCount = (int32)Raw.part_count;
    OutInfo.bRoot = Raw.is_root != 0;
    OutInfo.TotalCells = (int32)Raw.total_cells;
    OutInfo.UsedCells = (int32)Raw.used_cells;
    return true;
}

bool Uue_package_systemLibrary::GetContainerPartSize(UInvInventory* Inv, int64 Container,
    int32 Part, int32& OutWidth, int32& OutHeight)
{
    OutWidth = 0;
    OutHeight = 0;

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return false;
    }

    uint32 W = 0;
    uint32 H = 0;
    if (Ffi->ContainerPartSize(Inv->GetNativeHandle(), (uint64)Container,
            (uint32)FMath::Max(Part, 0), &W, &H) != 0)
    {
        return false;
    }

    OutWidth = (int32)W;
    OutHeight = (int32)H;
    return true;
}

FString Uue_package_systemLibrary::GetContainerLabel(UInvInventory* Inv, int64 Container)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return FString();
    }

    char* Raw = Ffi->ContainerLabel(Inv->GetNativeHandle(), (uint64)Container);
    if (!Raw)
    {
        return FString();
    }

    FString Result(UTF8_TO_TCHAR(Raw));
    // Rust 堆上的字符串必须交回 Rust 释放
    Ffi->FreeString(Raw);
    return Result;
}

bool Uue_package_systemLibrary::GetContainerGrid(UInvInventory* Inv, int64 Container, int32 Part,
    TArray<int64>& OutCells)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        OutCells.Reset();
        return false;
    }

    Inventory* Handle = Inv->GetNativeHandle();
    const uint64 ContainerKey = (uint64)Container;
    const uint32 PartIndex = (uint32)FMath::Max(Part, 0);

    return CollectU64(
        [Ffi, Handle, ContainerKey, PartIndex](uint64* Out, uint32 Cap) -> int64
        {
            return Ffi->ContainerGrid(Handle, ContainerKey, PartIndex, Out, Cap);
        },
        OutCells);
}

bool Uue_package_systemLibrary::GetItemLocation(UInvInventory* Inv, int64 Item,
    int64& OutContainer, int32& OutPart)
{
    OutContainer = 0;
    OutPart = 0;

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return false;
    }

    uint64 RawContainer = 0;
    uint32 RawPart = 0;
    if (Ffi->ItemLocation(Inv->GetNativeHandle(), (uint64)Item, &RawContainer, &RawPart) != 0)
    {
        return false;
    }

    OutContainer = (int64)RawContainer;
    OutPart = (int32)RawPart;
    return true;
}

float Uue_package_systemLibrary::GetTotalWeight(UInvInventory* Inv)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return 0.0f;
    }

    double Out = 0.0;
    if (Ffi->TotalWeight(Inv->GetNativeHandle(), &Out) != 0)
    {
        return 0.0f;
    }
    return (float)Out;
}

int64 Uue_package_systemLibrary::GetTotalValue(UInvInventory* Inv)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return 0;
    }

    uint64 Out = 0;
    if (Ffi->TotalValue(Inv->GetNativeHandle(), &Out) != 0)
    {
        return 0;
    }
    return (int64)Out;
}

TArray<int64> Uue_package_systemLibrary::FindByTag(UInvInventory* Inv, int32 TagMask)
{
    TArray<int64> Result;

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return Result;
    }

    Inventory* Handle = Inv->GetNativeHandle();
    const uint32 Mask = (uint32)FMath::Max(TagMask, 0);

    CollectU64(
        [Ffi, Handle, Mask](uint64* Out, uint32 Cap) -> int64
        {
            return Ffi->FindByTag(Handle, Mask, Out, Cap);
        },
        Result);

    return Result;
}

TArray<int64> Uue_package_systemLibrary::GetContainers(UInvInventory* Inv)
{
    TArray<int64> Result;

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return Result;
    }

    Inventory* Handle = Inv->GetNativeHandle();

    CollectU64(
        [Ffi, Handle](uint64* Out, uint32 Cap) -> int64
        {
            return Ffi->Containers(Handle, Out, Cap);
        },
        Result);

    return Result;
}

TArray<int64> Uue_package_systemLibrary::GetItems(UInvInventory* Inv)
{
    TArray<int64> Result;

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return Result;
    }

    Inventory* Handle = Inv->GetNativeHandle();

    CollectU64(
        [Ffi, Handle](uint64* Out, uint32 Cap) -> int64
        {
            return Ffi->Items(Handle, Out, Cap);
        },
        Result);

    return Result;
}

// ==================== 负重 ====================

void Uue_package_systemLibrary::SetMaxCapacity(UInvInventory* Inv, float Kg)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return;
    }

    const int32 Rc = Ffi->SetMaxCapacity(Inv->GetNativeHandle(), Kg);
    if (Rc != 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("背包系统: 设置承重上限失败，错误码 %d"), Rc);
    }
}

float Uue_package_systemLibrary::GetWeightRatio(UInvInventory* Inv)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return 0.0f;
    }

    float Out = 0.0f;
    if (Ffi->WeightRatio(Inv->GetNativeHandle(), &Out) != 0)
    {
        return 0.0f;
    }
    return Out;
}

bool Uue_package_systemLibrary::IsOverloaded(UInvInventory* Inv)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return false;
    }

    // 1 = 超重，0 = 正常，< 0 = -错误码
    return Ffi->IsOverloaded(Inv->GetNativeHandle()) > 0;
}

// ==================== 快照 ====================

bool Uue_package_systemLibrary::Snapshot(UInvInventory* Inv, TArray<uint8>& OutBytes)
{
    OutBytes.Reset();

    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return false;
    }

    uint8* RawPtr = nullptr;
    uint32 RawLen = 0;
    if (Ffi->Snapshot(Inv->GetNativeHandle(), &RawPtr, &RawLen) != 0)
    {
        return false;
    }

    if (RawPtr && RawLen > 0)
    {
        OutBytes.SetNumUninitialized((int32)RawLen);
        FMemory::Memcpy(OutBytes.GetData(), RawPtr, RawLen);
    }

    // Rust 堆上的字节缓冲区必须交回 Rust 释放
    Ffi->FreeBytes(RawPtr, RawLen);
    return true;
}

bool Uue_package_systemLibrary::Restore(UInvInventory* Inv, const TArray<uint8>& Bytes)
{
    const Fue_package_systemFfi* Ffi = ResolveFfi(Inv);
    if (!Ffi)
    {
        ReportInvalidHandle();
        return false;
    }

    if (Bytes.Num() == 0)
    {
        return false;
    }

    return Ffi->Restore(Inv->GetNativeHandle(), Bytes.GetData(), (uint32)Bytes.Num()) == 0;
}
