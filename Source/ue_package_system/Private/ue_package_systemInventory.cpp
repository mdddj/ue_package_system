#include "ue_package_systemInventory.h"

#include "UObject/Package.h"

UInvInventory* UInvInventory::CreateInventory()
{
    const Fue_package_systemFfi& Ffi = Fue_package_systemModule::GetFfi();
    if (!Ffi.IsValid() || !Ffi.InventoryNew)
    {
        UE_LOG(LogTemp, Error,
            TEXT("背包系统: Rust 库不可用，无法创建背包。在插件目录跑 `uerust build .` 后重启编辑器。"));
        return nullptr;
    }

    Inventory* Raw = Ffi.InventoryNew();
    if (!Raw)
    {
        UE_LOG(LogTemp, Error, TEXT("背包系统: Rust 侧 uerust_inventory_new 返回了空指针"));
        return nullptr;
    }

    UInvInventory* Wrapper = NewObject<UInvInventory>();
    Wrapper->Handle = Raw;
    return Wrapper;
}

void UInvInventory::BeginDestroy()
{
    const Fue_package_systemFfi& Ffi = Fue_package_systemModule::GetFfi();

    // 只有库还在、句柄还在时才交回 Rust 释放；模块关停时函数表已清空，跳过即可。
    if (Handle && Ffi.InventoryFree)
    {
        Ffi.InventoryFree(Handle);
    }
    Handle = nullptr;

    Super::BeginDestroy();
}

bool UInvInventory::IsValidHandle() const
{
    return Handle != nullptr;
}

const Fue_package_systemFfi* UInvInventory::Ffi() const
{
    return &Fue_package_systemModule::GetFfi();
}
