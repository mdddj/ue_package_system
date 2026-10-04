#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/Texture2D.h"

#include "ue_package_systemItemTable.generated.h"

class UInvItemDefinition;

/**
 * 物品内容表的**行结构**：一行 = 一件物品的 `UInvItemDefinition` 内容。
 *
 * 字段与 `UInvItemDefinition` **一一对应**（同名 / 同类型 / 同默认值），
 * 所以表里能填的东西和资产上一模一样，两者可以互换使用。
 *
 * ---------------------------------------------------------------------------
 * RowName 就是物品的逻辑名：
 *
 * - 组件 `BeginPlay` 时按 **RowName 升序**注册表行，RowName 直接决定 DefId，
 *   而 DefId 是存档的键——**改名 = 改 DefId = 撕存档，慎改**；
 * - 行名建议**全项目唯一且稳定**（`FindDefinitionByName("AK_Mag")` 按名字取定义，
 *   行名和同名的资产名共用一个索引，冲突时以先注册的为准并打警告）。
 * ---------------------------------------------------------------------------
 *
 * 批量导入：本结构可以直接喂 UE 原生的 DataTable 导入器——
 * 选 `.json` / `.csv` 文件 → 目标类型选 DataTable → 行结构（Row Struct）选 `InvItemDefinitionRow`。
 * 可直接导入的样例见插件目录 `tools/samples/ItemTable.sample.json`。
 *
 * 注意：本结构只有内容字段，没有实例状态（谁放在哪个格子、堆叠几件），
 * 实例状态在 Rust 核心里，由 `UInvInventoryComponent` 管。
 */
USTRUCT(BlueprintType)
struct UE_PACKAGE_SYSTEM_API FInvItemDefinitionRow : public FTableRowBase
{
    GENERATED_BODY()

    /** UI 上显示的名字（本地化文本）。JSON / CSV 里直接写字符串即可。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "显示")
    FText DisplayName;

    /** 物品描述 / 说明文本。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "显示")
    FText Description;

    /**
     * 图标。软引用：只有 UI 真要画的时候才加载。
     * 表里写对象路径字符串，例如 `/Game/BackpackDemo/Icons/T_AK_Mag.T_AK_Mag`；
     * 空字符串 = 没有图标（导入时不会去找这个路径，不存在也只是运行时加载失败）。
     */
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

    /**
     * 非空 = 这件物品是容器（套包 / 弹挂）：每项一块内部网格，按 **(宽, 高)** 读。
     * JSON 里写成 `[{"X": 6, "Y": 5}]`（FIntPoint 的字段名就是 X / Y，别被 (宽, 高) 语义绕进去）。
     * 带容器规格的行必须不可堆叠（MaxStack = 1），否则注册时会被强制改成 1 并打警告。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "容器")
    TArray<FIntPoint> ContainerParts;

    /**
     * 把这一行的内容抄到一件定义对象上（组件注册表行时用）。
     *
     * 只是字段拷贝：`Icon` 拷的是软引用路径，不加载资产；`ContainerParts` 是深拷贝。
     * 不夹取值——越界字段由 `UInvInventoryComponent::RegisterDefinition` 统一夹住并打警告，
     * 这里保持“表里写什么就是什么”，免得读表的人看到被改过的值。
     */
    void ApplyToDefinition(UInvItemDefinition& Target) const;
};
