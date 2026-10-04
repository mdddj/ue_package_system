#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ue_package_systemInventory.h"
#include "ue_package_systemLibrary.generated.h"

/**
 * 背包系统蓝图接口。把 Rust 核心的能力暴露给蓝图。
 *
 * 返回约定（和 C ABI 一致）：
 * - 句柄类函数返回 `int64`：`>= 0` 成功（DefId / 句柄 / 计数），`< 0` 表示 `-错误码`；
 * - 操作类函数返回 `int32`：`0` 成功，`> 0` 为错误码；
 * - 查询类函数返回 `bool`，真正的数据在输出参数里；
 * - 传入的 `UInvInventory*` 为空或句柄无效时，句柄类返回 `-23`，操作类返回 `23`。
 *
 * 错误码见 `docs/API.md`。出错的详细文本可以用 `LastError()` 取。
 */
UCLASS()
class UE_PACKAGE_SYSTEM_API Uue_package_systemLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // ==================== 原有：自检 / 版本 ====================

    /**
     * 跑一遍 Rust 核心自检，返回多行文本报告。
     * Rust 库不可用时返回可读的中文错误提示。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Inventory Self Test"))
    static FString InventorySelfTest();

    /** Rust 核心版本号（静态字符串）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Rust Core Version"))
    static FString PluginVersion();

    /** Rust 侧记录的最后一条错误文本（线程本地）。没有错误时返回空串。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Last Error"))
    static FString LastError();

    // ==================== 目录 / 容器 / 物品 ====================

    /**
     * 注册物品定义，返回 DefId（`< 0` = `-错误码`）。
     * `ContainerParts` 为空 = 普通物品；非空则每项 (X,Y) 是这件物品自带的一块内部网格。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Define Item"))
    static int64 DefineItem(UInvInventory* Inv, const FString& Name, int32 Width, int32 Height,
        bool bRotatable, int32 MaxStack, float Weight, int32 Value, int32 TagBits,
        int32 ForbiddenContainerTypes, const TArray<FIntPoint>& ContainerParts);

    /**
     * 创建根容器，返回容器句柄（`< 0` = `-错误码`）。
     * `Parts` 非空：每项 (X,Y) 一块子网格；弹挂可以传多个 1x1 / 1x2 口。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Add Root Container"))
    static int64 AddRoot(UInvInventory* Inv, EInvContainerType Type, const FString& Label,
        const TArray<FIntPoint>& Parts);

    /** 生成物品并落到 (Container, Part, X, Y)，返回物品句柄（`< 0` = `-错误码`）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Add Item"))
    static int64 AddItem(UInvInventory* Inv, int32 DefId, int32 Stack, int32 Durability,
        int64 Container, int32 Part, int32 X, int32 Y, bool bRotated);

    /** 删除物品及其套包内的全部内容，返回被删除的件数（`< 0` = `-错误码`）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Destroy Item"))
    static int64 DestroyItem(UInvInventory* Inv, int64 Item);

    // ==================== 原子交互 ====================

    /** 移动物品。失败时状态零变化。返回 0 或错误码。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Try Move"))
    static int32 TryMove(UInvInventory* Inv, int64 Item, int64 Destination, int32 Part,
        int32 X, int32 Y, bool bRotated);

    /** 两个物品原子互换（可跨容器）。返回 0 或错误码。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Try Swap"))
    static int32 TrySwap(UInvInventory* Inv, int64 A, int64 B);

    /** 原地旋转 90 度。返回 0 或错误码。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Try Rotate"))
    static int32 TryRotate(UInvInventory* Inv, int64 Item);

    /** 把 Src 合并进 Dst。返回 0 或错误码，OutMoved 是实际转移数量。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Try Merge"))
    static int32 TryMerge(UInvInventory* Inv, int64 Src, int64 Dst, int32& OutMoved);

    /** 拆分堆叠，返回新物品句柄（`< 0` = `-错误码`）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Try Split"))
    static int64 TrySplit(UInvInventory* Inv, int64 Item, int32 Count, int64 Destination,
        int32 Part, int32 X, int32 Y, bool bRotated);

    /** 一键整理某个容器。返回 0 或错误码（整理失败时布局保持不动）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Autosort"))
    static int32 Autosort(UInvInventory* Inv, int64 Container);

    // ==================== 查询 ====================

    /** 取物品状态。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Item Info"))
    static bool GetItemInfo(UInvInventory* Inv, int64 Item, FInvItemInfo& OutInfo);

    /** 取容器汇总。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Container Info"))
    static bool GetContainerInfo(UInvInventory* Inv, int64 Container, FInvContainerInfo& OutInfo);

    /** 取某块子网格的宽高。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Container Part Size"))
    static bool GetContainerPartSize(UInvInventory* Inv, int64 Container, int32 Part,
        int32& OutWidth, int32& OutHeight);

    /** 取容器显示名。失败返回空串。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Container Label"))
    static FString GetContainerLabel(UInvInventory* Inv, int64 Container);

    /**
     * 导出某块子网格的占格表（行主序，长度 = 宽 * 高）。
     * 0 = 空格，否则是该格上压着的物品句柄。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Container Grid"))
    static bool GetContainerGrid(UInvInventory* Inv, int64 Container, int32 Part,
        TArray<int64>& OutCells);

    /** 取物品当前落在哪个容器的哪块子网格。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Item Location"))
    static bool GetItemLocation(UInvInventory* Inv, int64 Item, int64& OutContainer, int32& OutPart);

    /** 总重量（kg）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Total Weight"))
    static float GetTotalWeight(UInvInventory* Inv);

    /** 总估值。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Total Value"))
    static int64 GetTotalValue(UInvInventory* Inv);

    /** 标签掩码查询，返回命中的物品句柄列表。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Find Items By Tag"))
    static TArray<int64> FindByTag(UInvInventory* Inv, int32 TagMask);

    /** 全部容器（根在前，顺序确定）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Containers"))
    static TArray<int64> GetContainers(UInvInventory* Inv);

    /** 全部物品。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Items"))
    static TArray<int64> GetItems(UInvInventory* Inv);

    // ==================== 负重 ====================

    /** 设置总承重上限（kg）。`Kg <= 0`、NaN 或 ±inf 一律视为取消上限。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Set Max Capacity"))
    static void SetMaxCapacity(UInvInventory* Inv, float Kg);

    /** 负重比（总重 ÷ 上限）。无上限时返回 0.0，超重时 > 1.0。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Get Weight Ratio"))
    static float GetWeightRatio(UInvInventory* Inv);

    /** 是否超重。无上限时恒为 false。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Is Overloaded"))
    static bool IsOverloaded(UInvInventory* Inv);

    // ==================== 快照 ====================

    /** 导出 bincode 快照字节。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Snapshot"))
    static bool Snapshot(UInvInventory* Inv, TArray<uint8>& OutBytes);

    /** 从快照字节恢复（目录与规则保持不变；快照损坏时现有数据不受影响）。 */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Restore"))
    static bool Restore(UInvInventory* Inv, const TArray<uint8>& Bytes);
};
