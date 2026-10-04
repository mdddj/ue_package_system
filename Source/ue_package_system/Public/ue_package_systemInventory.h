#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ue_package_system.h"  // 带来 Fue_package_systemFfi 与 C ABI 的 Inventory 前置声明
#include "ue_package_systemInventory.generated.h"

/** 容器种类，取值与 Rust 侧 ContainerType 一一对应。 */
UENUM(BlueprintType)
enum class EInvContainerType : uint8
{
    /** 口袋（通常是几个 1x1 小格的复合容器）。 */
    Pockets = 0 UMETA(DisplayName = "口袋"),
    /** 弹挂。 */
    ChestRig = 1 UMETA(DisplayName = "弹挂"),
    /** 安全箱。 */
    SafeBox = 2 UMETA(DisplayName = "安全箱"),
    /** 背包。 */
    Backpack = 3 UMETA(DisplayName = "背包"),
    /** 非根容器（套包内部、弹挂内部）。 */
    Nested = 4 UMETA(DisplayName = "嵌套容器"),
};

/** 单个物品实例的完整状态。 */
USTRUCT(BlueprintType)
struct FInvItemInfo
{
    GENERATED_BODY()

    /** 物品定义 ID（DefineItem 的返回值）。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 DefId = 0;

    /** 当前堆叠数量。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 Stack = 0;

    /** 耐久度。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 Durability = 0;

    /** 自带容器（套包）：0 = 无，否则是内部容器句柄。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int64 OwnContainer = 0;

    /** 是否已落位（当前版本恒为 true，预留字段）。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    bool bPlaced = false;

    /** 所在容器句柄；未落位为 0。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int64 HostContainer = 0;

    /** 所在容器的子网格下标。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 HostPart = 0;

    /** 左上角坐标。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 X = 0;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 Y = 0;

    /** 是否旋转了 90 度。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    bool bRotated = false;
};

/** 容器状态汇总。 */
USTRUCT(BlueprintType)
struct FInvContainerInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    EInvContainerType Type = EInvContainerType::Backpack;

    /** 子网格数量。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 PartCount = 0;

    /** 是否是根容器（由 AddRoot 创建）。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    bool bRoot = false;

    /** 总格数。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 TotalCells = 0;

    /** 已占格数（按格计，不是物品件数）。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "背包系统")
    int32 UsedCells = 0;
};

/**
 * 背包实例的蓝图句柄对象。
 *
 * 内部持有一份 Rust 侧的 `Inventory*`（不透明指针）。
 * 生命周期跟随 UObject：GC 回收时在 BeginDestroy 里交回 Rust 释放，
 * 所以蓝图里只要把这个对象引用着，底下的背包数据就不会丢。
 *
 * 注意：底下的 Rust 句柄只在**单线程**内使用；要跨线程用请自己加锁。
 * 本对象的接口本身都不是线程安全的。
 */
UCLASS(BlueprintType)
class UE_PACKAGE_SYSTEM_API UInvInventory : public UObject
{
    GENERATED_BODY()

public:
    /** Rust 侧背包实例在 GC 回收时释放。 */
    virtual void BeginDestroy() override;

    /**
     * 创建一个空背包。Rust 库不可用时返回 nullptr 并打中文错误日志。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Create Inventory"))
    static UInvInventory* CreateInventory();

    /** 底下的 Rust 句柄是否有效。 */
    UFUNCTION(BlueprintPure, Category = "背包系统", meta = (DisplayName = "Is Valid Handle"))
    bool IsValidHandle() const;

    /** 给包装函数用的 ABI 表访问器（未加载时仍返回非空结构体，但 IsValid() 为 false）。 */
    const Fue_package_systemFfi* Ffi() const;

    /** 给包装函数用的原生句柄访问器（不暴露给蓝图）。 */
    Inventory* GetNativeHandle() const { return Handle; }

private:
    /** Rust 侧的背包实例；由 CreateInventory 填充，BeginDestroy 释放。 */
    Inventory* Handle = nullptr;
};
