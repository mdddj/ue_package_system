#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// cbindgen 生成的 C ABI 头（Rust 侧 cargo build 时由 build.rs 产出到
// Source/ue_package_system-dylib/include/，Build.cs 已经把它加进 PublicIncludePaths）。
//
// ABI 方案：直接用生成头当唯一事实来源，函数指针类型全部写成
// `decltype(&uerust_xxx)`，加载时 reinterpret_cast 过去。
// 这样 Rust 侧改了签名，C++ 侧编译期立刻报错，不用手抄 30+ 个 typedef 对着维护。
// （生成头里的 `MAGAZINE` / `KEY` 等是 constexpr 常量而不是宏，且已在
//  Engine/Source/Runtime 全量检索过没有同名宏，不存在 unity build 冲突。）
#include "ue_package_system_ffi.h"

/**
 * Rust 动态库导出的 C ABI 函数表。
 * 函数名必须和 Rust 侧 #[no_mangle] extern "C" 的完全一致，
 * 也和 cbindgen 生成的 ue_package_system_ffi.h 一致。
 */
struct Fue_package_systemFfi
{
    using PluginVersionFn = decltype(&uerust_plugin_version);
    using LastErrorFn = decltype(&uerust_last_error);
    using FreeStringFn = decltype(&uerust_free_string);
    using FreeBytesFn = decltype(&uerust_free_bytes);
    using SelfTestFn = decltype(&uerust_inventory_selftest);

    using InventoryNewFn = decltype(&uerust_inventory_new);
    using InventoryFreeFn = decltype(&uerust_inventory_free);
    using DefineItemFn = decltype(&uerust_inventory_define_item);
    using AddRootFn = decltype(&uerust_inventory_add_root);
    using AddItemFn = decltype(&uerust_inventory_add_item);
    using DestroyItemFn = decltype(&uerust_inventory_destroy_item);
    using TryMoveFn = decltype(&uerust_inventory_try_move);
    using TrySwapFn = decltype(&uerust_inventory_try_swap);
    using TryRotateFn = decltype(&uerust_inventory_try_rotate);
    using TryMergeFn = decltype(&uerust_inventory_try_merge);
    using TrySplitFn = decltype(&uerust_inventory_try_split);
    using AutosortFn = decltype(&uerust_inventory_autosort);
    using ItemInfoFn = decltype(&uerust_inventory_item_info);
    using ContainerInfoFn = decltype(&uerust_inventory_container_info);
    using ContainerPartSizeFn = decltype(&uerust_inventory_container_part_size);
    using ContainerLabelFn = decltype(&uerust_inventory_container_label);
    using ContainerGridFn = decltype(&uerust_inventory_container_grid);
    using ItemLocationFn = decltype(&uerust_inventory_item_location);
    using TotalWeightFn = decltype(&uerust_inventory_total_weight);
    using TotalValueFn = decltype(&uerust_inventory_total_value);
    using SetMaxCapacityFn = decltype(&uerust_inventory_set_max_capacity);
    using WeightRatioFn = decltype(&uerust_inventory_weight_ratio);
    using IsOverloadedFn = decltype(&uerust_inventory_is_overloaded);
    using FindByTagFn = decltype(&uerust_inventory_find_by_tag);
    using ContainersFn = decltype(&uerust_inventory_containers);
    using ItemsFn = decltype(&uerust_inventory_items);
    using SnapshotFn = decltype(&uerust_inventory_snapshot);
    using RestoreFn = decltype(&uerust_inventory_restore);

    PluginVersionFn PluginVersion = nullptr;
    LastErrorFn LastError = nullptr;
    FreeStringFn FreeString = nullptr;
    FreeBytesFn FreeBytes = nullptr;
    SelfTestFn SelfTest = nullptr;

    InventoryNewFn InventoryNew = nullptr;
    InventoryFreeFn InventoryFree = nullptr;
    DefineItemFn DefineItem = nullptr;
    AddRootFn AddRoot = nullptr;
    AddItemFn AddItem = nullptr;
    DestroyItemFn DestroyItem = nullptr;
    TryMoveFn TryMove = nullptr;
    TrySwapFn TrySwap = nullptr;
    TryRotateFn TryRotate = nullptr;
    TryMergeFn TryMerge = nullptr;
    TrySplitFn TrySplit = nullptr;
    AutosortFn Autosort = nullptr;
    ItemInfoFn ItemInfo = nullptr;
    ContainerInfoFn ContainerInfo = nullptr;
    ContainerPartSizeFn ContainerPartSize = nullptr;
    ContainerLabelFn ContainerLabel = nullptr;
    ContainerGridFn ContainerGrid = nullptr;
    ItemLocationFn ItemLocation = nullptr;
    TotalWeightFn TotalWeight = nullptr;
    TotalValueFn TotalValue = nullptr;
    SetMaxCapacityFn SetMaxCapacity = nullptr;
    WeightRatioFn WeightRatio = nullptr;
    IsOverloadedFn IsOverloaded = nullptr;
    FindByTagFn FindByTag = nullptr;
    ContainersFn Containers = nullptr;
    ItemsFn Items = nullptr;
    SnapshotFn Snapshot = nullptr;
    RestoreFn Restore = nullptr;

    /**
     * 判据：全部函数指针都取到了才算可用。
     * 缺任何一个都说明 dylib 版本和 C++ 侧不匹配，宁可直接报错也别半残运行。
     */
    bool IsValid() const
    {
        return PluginVersion && LastError && FreeString && FreeBytes && SelfTest
            && InventoryNew && InventoryFree && DefineItem && AddRoot && AddItem
            && DestroyItem && TryMove && TrySwap && TryRotate && TryMerge && TrySplit
            && Autosort && ItemInfo && ContainerInfo && ContainerPartSize && ContainerLabel
            && ContainerGrid && ItemLocation && TotalWeight && TotalValue && SetMaxCapacity
            && WeightRatio && IsOverloaded && FindByTag && Containers && Items
            && Snapshot && Restore;
    }
};

/**
 * 插件模块。只干一件事：启动时 dlopen Rust 库并把函数指针取出来。
 */
class Fue_package_systemModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** 已加载的 Rust 函数表。没加载成功时 IsValid() 返回 false。 */
    static const Fue_package_systemFfi& GetFfi();

    /** true 表示 Rust 库可用 */
    static bool IsAvailable();

private:
    bool LoadRustLibrary();

    void* LibraryHandle = nullptr;
    Fue_package_systemFfi Ffi;
};
