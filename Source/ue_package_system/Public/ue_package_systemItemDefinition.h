#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/Texture2D.h"

#include "ue_package_systemItemDefinition.generated.h"

/**
 * 背包物品的**内容定义**（内容资产，设计师在编辑器里填）。
 *
 * 这一层只描述“一件物品长什么样”，不含任何实例状态：
 * 实例数据（谁放在哪个格子、堆叠几件、耐久多少）在 Rust 核心里，由 UInvInventoryComponent 管，
 * 两边用 DefId（注册顺序号）对应，见 UInvInventoryComponent::RegisterDefinition。
 *
 * ---------------------------------------------------------------------------
 * 扩展点：**就是子类化本资产**。
 *
 * 想加武器数值（伤害 / 射速 / 弹匣容量 / 后坐力曲线）、图片动画、音效、稀有度表现之类的
 * 专有字段，派生一个 C++ 子类（或直接蓝图子类化本资产）即可：
 *
 *     UCLASS(BlueprintType, meta = (DisplayName = "武器定义"))
 *     class UMyWeaponDefinition : public UInvItemDefinition
 *     {
 *         GENERATED_BODY()
 *     public:
 *         UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "武器")
 *         float Damage = 0.f;
 *     };
 *
 * 本资产自带的字段（宽高 / 可旋转 / 堆叠 / 重量 / 估值 / 标签 / 容器规格）会被
 * UInvInventoryComponent 原样喂给 Rust 核心；**子类新增的字段不会**，它们是给你自己的
 * 玩法逻辑、UI 和数值表读的。所以“能影响背包布局的字段”必须留在基类或自己调用低层 API。
 * ---------------------------------------------------------------------------
 *
 * 注意：DefId 由注册顺序决定，与资产路径无关；设计师重排组件上的 ItemDefinitions 数组
 * 不会改变 DefId，因此不会撕存档。
 */
UCLASS(BlueprintType, meta = (DisplayName = "背包物品定义"))
class UE_PACKAGE_SYSTEM_API UInvItemDefinition : public UDataAsset
{
    GENERATED_BODY()

public:
    /** UI 上显示的名字（本地化文本）。Rust 侧目录用的是资产名，与这里无关。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "显示")
    FText DisplayName;

    /** 物品描述 / 说明文本。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "显示")
    FText Description;

    /** 图标。软引用：只有 UI 真要画的时候才加载。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "显示")
    TSoftObjectPtr<UTexture2D> Icon;

    /** 未旋转时占的格宽。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "格子")
    int32 Width = 1;

    /** 未旋转时占的格高。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "格子")
    int32 Height = 1;

    /** 是否允许旋转 90 度（旋转后占位变成 高 x 宽）。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "格子")
    bool bRotatable = false;

    /** 单堆叠上限（1 = 不可堆叠）。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "堆叠")
    int32 MaxStack = 1;

    /** 单件重量（kg），负重统计用。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "数值")
    float Weight = 0.f;

    /** 单件估值，背包总估值用。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "数值")
    int32 Value = 0;

    /** 标签位掩码（位掩码，见 docs/API.md 与 C ABI 头里的 MAGAZINE / AMMO / ... 常量）。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "数值")
    int32 TagBits = 0;

    /** 禁止放入的根容器类型掩码：位 n 对应 EInvContainerType 取值 n。0 = 不限制。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "规则")
    int32 ForbiddenContainerTypes = 0;

    /** 非空 = 这件物品是容器（套包 / 弹挂）：每项一块内部网格。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "容器")
    TArray<FIntPoint> ContainerParts;

    /** 这件物品是不是容器（套包 / 弹挂）。 */
    UFUNCTION(BlueprintPure, Category = "背包系统")
    bool HasContainer() const;

    /** 占地尺寸 `(宽, 高)`；旋转 90 度后交换长宽，面积不变。 */
    UFUNCTION(BlueprintPure, Category = "背包系统")
    FIntPoint Footprint(bool bRotated) const;
};
