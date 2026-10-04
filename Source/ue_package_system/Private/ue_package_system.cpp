#include "ue_package_system.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "Fue_package_systemModule"

namespace
{
    /** 当前平台的库文件名。和 Build.cs 里算出来的名字必须一致。 */
    const TCHAR* RustLibraryFileName()
    {
#if PLATFORM_WINDOWS
        return TEXT("ue_package_system_dylib.dll");
#elif PLATFORM_MAC
        return TEXT("libue_package_system_dylib.dylib");
#else
        return TEXT("libue_package_system_dylib.so");
#endif
    }

    /** 取导出符号并转成 Fue_package_systemFfi 里声明的函数指针类型。 */
    template <typename FnPtrType>
    FnPtrType LoadSymbol(void* Handle, const TCHAR* SymbolName)
    {
        return reinterpret_cast<FnPtrType>(
            FPlatformProcess::GetDllExport(Handle, SymbolName));
    }
}

bool Fue_package_systemModule::LoadRustLibrary()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ue_package_system"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("ue_package_system: 找不到插件本体，IPluginManager 里没有 ue_package_system"));
        return false;
    }

    const FString LibraryPath = FPaths::Combine(
        Plugin->GetBaseDir(),
        TEXT("Binaries"),
        FPlatformProcess::GetBinariesSubdirectory(),
        RustLibraryFileName());

    if (!FPaths::FileExists(LibraryPath))
    {
        UE_LOG(LogTemp, Error,
            TEXT("ue_package_system: 找不到 Rust 库 %s —— 在插件目录里跑 `uerust build .` 再重启编辑器"),
            *LibraryPath);
        return false;
    }

    LibraryHandle = FPlatformProcess::GetDllHandle(*LibraryPath);
    if (!LibraryHandle)
    {
        UE_LOG(LogTemp, Error, TEXT("ue_package_system: 加载失败 %s"), *LibraryPath);
        return false;
    }

    // 逐个取符号。名字必须和 Rust 侧 #[no_mangle] 的完全一致。
    Ffi.PluginVersion = LoadSymbol<Fue_package_systemFfi::PluginVersionFn>(LibraryHandle, TEXT("uerust_plugin_version"));
    Ffi.LastError = LoadSymbol<Fue_package_systemFfi::LastErrorFn>(LibraryHandle, TEXT("uerust_last_error"));
    Ffi.FreeString = LoadSymbol<Fue_package_systemFfi::FreeStringFn>(LibraryHandle, TEXT("uerust_free_string"));
    Ffi.FreeBytes = LoadSymbol<Fue_package_systemFfi::FreeBytesFn>(LibraryHandle, TEXT("uerust_free_bytes"));
    Ffi.SelfTest = LoadSymbol<Fue_package_systemFfi::SelfTestFn>(LibraryHandle, TEXT("uerust_inventory_selftest"));

    Ffi.InventoryNew = LoadSymbol<Fue_package_systemFfi::InventoryNewFn>(LibraryHandle, TEXT("uerust_inventory_new"));
    Ffi.InventoryFree = LoadSymbol<Fue_package_systemFfi::InventoryFreeFn>(LibraryHandle, TEXT("uerust_inventory_free"));
    Ffi.DefineItem = LoadSymbol<Fue_package_systemFfi::DefineItemFn>(LibraryHandle, TEXT("uerust_inventory_define_item"));
    Ffi.AddRoot = LoadSymbol<Fue_package_systemFfi::AddRootFn>(LibraryHandle, TEXT("uerust_inventory_add_root"));
    Ffi.AddItem = LoadSymbol<Fue_package_systemFfi::AddItemFn>(LibraryHandle, TEXT("uerust_inventory_add_item"));
    Ffi.DestroyItem = LoadSymbol<Fue_package_systemFfi::DestroyItemFn>(LibraryHandle, TEXT("uerust_inventory_destroy_item"));
    Ffi.TryMove = LoadSymbol<Fue_package_systemFfi::TryMoveFn>(LibraryHandle, TEXT("uerust_inventory_try_move"));
    Ffi.TrySwap = LoadSymbol<Fue_package_systemFfi::TrySwapFn>(LibraryHandle, TEXT("uerust_inventory_try_swap"));
    Ffi.TryRotate = LoadSymbol<Fue_package_systemFfi::TryRotateFn>(LibraryHandle, TEXT("uerust_inventory_try_rotate"));
    Ffi.TryMerge = LoadSymbol<Fue_package_systemFfi::TryMergeFn>(LibraryHandle, TEXT("uerust_inventory_try_merge"));
    Ffi.TrySplit = LoadSymbol<Fue_package_systemFfi::TrySplitFn>(LibraryHandle, TEXT("uerust_inventory_try_split"));
    Ffi.Autosort = LoadSymbol<Fue_package_systemFfi::AutosortFn>(LibraryHandle, TEXT("uerust_inventory_autosort"));
    Ffi.ItemInfo = LoadSymbol<Fue_package_systemFfi::ItemInfoFn>(LibraryHandle, TEXT("uerust_inventory_item_info"));
    Ffi.ContainerInfo = LoadSymbol<Fue_package_systemFfi::ContainerInfoFn>(LibraryHandle, TEXT("uerust_inventory_container_info"));
    Ffi.ContainerPartSize = LoadSymbol<Fue_package_systemFfi::ContainerPartSizeFn>(LibraryHandle, TEXT("uerust_inventory_container_part_size"));
    Ffi.ContainerLabel = LoadSymbol<Fue_package_systemFfi::ContainerLabelFn>(LibraryHandle, TEXT("uerust_inventory_container_label"));
    Ffi.ContainerGrid = LoadSymbol<Fue_package_systemFfi::ContainerGridFn>(LibraryHandle, TEXT("uerust_inventory_container_grid"));
    Ffi.ItemLocation = LoadSymbol<Fue_package_systemFfi::ItemLocationFn>(LibraryHandle, TEXT("uerust_inventory_item_location"));
    Ffi.TotalWeight = LoadSymbol<Fue_package_systemFfi::TotalWeightFn>(LibraryHandle, TEXT("uerust_inventory_total_weight"));
    Ffi.TotalValue = LoadSymbol<Fue_package_systemFfi::TotalValueFn>(LibraryHandle, TEXT("uerust_inventory_total_value"));
    Ffi.SetMaxCapacity = LoadSymbol<Fue_package_systemFfi::SetMaxCapacityFn>(LibraryHandle, TEXT("uerust_inventory_set_max_capacity"));
    Ffi.WeightRatio = LoadSymbol<Fue_package_systemFfi::WeightRatioFn>(LibraryHandle, TEXT("uerust_inventory_weight_ratio"));
    Ffi.IsOverloaded = LoadSymbol<Fue_package_systemFfi::IsOverloadedFn>(LibraryHandle, TEXT("uerust_inventory_is_overloaded"));
    Ffi.FindByTag = LoadSymbol<Fue_package_systemFfi::FindByTagFn>(LibraryHandle, TEXT("uerust_inventory_find_by_tag"));
    Ffi.Containers = LoadSymbol<Fue_package_systemFfi::ContainersFn>(LibraryHandle, TEXT("uerust_inventory_containers"));
    Ffi.Items = LoadSymbol<Fue_package_systemFfi::ItemsFn>(LibraryHandle, TEXT("uerust_inventory_items"));
    Ffi.Snapshot = LoadSymbol<Fue_package_systemFfi::SnapshotFn>(LibraryHandle, TEXT("uerust_inventory_snapshot"));
    Ffi.Restore = LoadSymbol<Fue_package_systemFfi::RestoreFn>(LibraryHandle, TEXT("uerust_inventory_restore"));

    if (!Ffi.IsValid())
    {
        UE_LOG(LogTemp, Error,
            TEXT("ue_package_system: 库打开了但符号没找齐。检查 Rust 侧是不是 #[no_mangle] pub extern \"C\"，函数名有没有拼错"));
        FPlatformProcess::FreeDllHandle(LibraryHandle);
        LibraryHandle = nullptr;
        Ffi = Fue_package_systemFfi{};
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("ue_package_system: Rust 库就绪 %s (%s)"),
        *LibraryPath, UTF8_TO_TCHAR(Ffi.PluginVersion()));
    return true;
}

void Fue_package_systemModule::StartupModule()
{
    if (!LoadRustLibrary())
    {
        UE_LOG(LogTemp, Error, TEXT("ue_package_system: Rust 库不可用，插件功能全部不可用"));
    }
}

void Fue_package_systemModule::ShutdownModule()
{
    // 先清函数表再卸库，否则留着悬空指针
    Ffi = Fue_package_systemFfi{};

    if (LibraryHandle)
    {
        FPlatformProcess::FreeDllHandle(LibraryHandle);
        LibraryHandle = nullptr;
    }
}

const Fue_package_systemFfi& Fue_package_systemModule::GetFfi()
{
    static const Fue_package_systemFfi Empty;
    Fue_package_systemModule* Module = FModuleManager::GetModulePtr<Fue_package_systemModule>(TEXT("ue_package_system"));
    return Module ? Module->Ffi : Empty;
}

bool Fue_package_systemModule::IsAvailable()
{
    return GetFfi().IsValid();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(Fue_package_systemModule, ue_package_system)
