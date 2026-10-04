#pragma once

#include "CoreMinimal.h"

#include "ue_package_systemInventory.h"
#include "ue_package_systemItemDefinition.h"

#include "ue_package_systemTypes.generated.h"

/**
 * 单个物品实例的**视图**：低层的实例状态（FInvItemInfo）+ 内容定义拼在一起。
 *
 * UI 一般直接用 GetContainerItemsView / GetItemView 拿到它渲染，不用自己去翻 DefId。
 * 定义可能为空（比如那件物品的定义在注册顺序里没对上），用之前判一下。
 *
 * 坐标约定：X 向右、Y 向下，单位是格，(0,0) 是子网格左上角。
 */
USTRUCT(BlueprintType)
struct FInvItemView
{
    GENERATED_BODY()

    /** 物品句柄（低层 ItemKey）；所有操作类节点都用它。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int64 Handle = 0;

    /** 内容定义（网格尺寸 / 图标 / 名字 / 数值都在里面）。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    TObjectPtr<UInvItemDefinition> Definition = nullptr;

    /** 当前堆叠数量。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int32 Stack = 0;

    /** 耐久度。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int32 Durability = 0;

    /** 自带容器（套包 / 弹挂）：`0` = 无；非 0 时用它展开嵌套背包的内容。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int64 OwnContainer = 0;

    /** 所在容器句柄；未落位为 0。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int64 HostContainer = 0;

    /** 所在容器的子网格下标。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int32 HostPart = 0;

    /** 左上角 X（格）。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int32 X = 0;

    /** 左上角 Y（格）。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    int32 Y = 0;

    /** 是否旋转了 90 度。 */
    UPROPERTY(BlueprintReadOnly, Category = "背包系统")
    bool bRotated = false;
};

/**
 * 一件根容器的启动配置：组件 BeginPlay 时照这个建（口袋 / 弹挂 / 安全箱 / 背包）。
 *
 * 注意：每种根容器类型**只能建一个**（Rust 侧重复创建返回错误码 12）。
 */
USTRUCT(BlueprintType)
struct FInvRootContainerSetup
{
    GENERATED_BODY()

    /** 根容器类型；Nested 是套包内部用的，填了会被跳过。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "背包系统")
    EInvContainerType Type = EInvContainerType::Backpack;

    /** 显示名（UI 标题用）。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "背包系统")
    FString Label = TEXT("背包");

    /**
     * 子网格列表，每项按 **(宽, 高)** 读，不是普通 FIntPoint 的 (X, Y) 语义。
     * 背包通常一块大的（如 (6,5)）；弹挂 / 口袋是多个小口（如四块 (1,1)）。
     * 至少一块，每边 1~32（Rust 侧上限）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "背包系统")
    TArray<FIntPoint> Parts;
};
