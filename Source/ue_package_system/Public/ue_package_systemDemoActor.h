#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ue_package_systemComponent.h"
#include "ue_package_systemItemDefinition.h"
#include "ue_package_systemTypes.h"

#include "ue_package_systemDemoActor.generated.h"

class UBillboardComponent;
class UInvInventoryScreenWidget;

/**
 * 背包演示 Actor：**演示 / 冒烟用，正式项目可以直接删**。
 *
 * 拖进任意关卡 → Play，屏幕上和日志里会打出同一份文本摘要：
 *
 *     ===== 背包演示 =====
 *     [口袋] 4 块子网格
 *       part0 (1x1): A
 *       part1 (1x1): B
 *       ...
 *     [背包] 1 块子网格
 *       part0 (6x5):
 *         EEE...
 *         ......
 *     A = 弹药 ×60 (1x1)   B = 急救包 ×3 (1x1)
 *     物品 7 件 / 总重 5.380 kg / 总估值 2510 / 负重比 0.179（上限 30 kg）
 *
 * 零配置：`DemoDefinitions` 留空时，`InitializeDemoInventory()` 会现场 `NewObject` 造 5 件内置定义
 * （弹药 / 弹匣 / 急救包 / 手雷 / 突击背包），把组件的 `RootContainers` / `ItemDefinitions`
 * 填成默认值，再灌一套演示装载。**不需要任何美术资产**（内置定义不带图标，只有宽高 / 堆叠 / 数值）。
 *
 * 两个使用上的注意点：
 * - `InitializeDemoInventory()` 不需要世界，无头测试直接 `NewObject` 出 Actor 就能调；
 * - 它是**幂等**的：已经灌过就跳过配置与装载，只按当前状态重新生成一份摘要返回
 *   （根容器重复创建会被 Rust 侧拒绝（错误码 12），物品也会翻倍，所以装载只能做一次）。
 */
UCLASS(ClassGroup = (Backpack), meta = (DisplayName = "背包演示 Actor"))
class UE_PACKAGE_SYSTEM_API AInvInventoryDemoActor : public AActor
{
    GENERATED_BODY()

public:
    /** 建根组件 + 广告牌 + 背包组件；其余配置全是默认值，拖进关卡就能跑。 */
    AInvInventoryDemoActor();

    // ==================== 生命周期 ====================

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // ==================== 组件 ====================

    /** 编辑器图标：关卡里一眼能找到这个 Actor（打包构建里会被剥掉，可能是空指针）。 */
    UPROPERTY(VisibleAnywhere, Category = "演示")
    TObjectPtr<UBillboardComponent> Billboard;

    /** 演示用的背包组件：装载、网格、摘要全在它上面。 */
    UPROPERTY(VisibleAnywhere, Category = "演示")
    TObjectPtr<UInvInventoryComponent> InventoryComponent;

    // ==================== 配置 ====================

    /** BeginPlay 时自动灌演示装载；关掉就只留给蓝图 / 测试手动调「Initialize Demo Inventory」。 */
    UPROPERTY(EditAnywhere, Category = "演示")
    bool bAutoFillOnBeginPlay = true;

    /** 是否把摘要打到屏幕上（日志里始终有全文）。 */
    UPROPERTY(EditAnywhere, Category = "演示")
    bool bDrawOnScreen = true;

    /** 屏幕刷新间隔（秒）。定时器按它重复触发：背包被改动后，屏幕上的网格会跟着变。 */
    UPROPERTY(EditAnywhere, Category = "演示")
    float RefreshIntervalSeconds = 3.f;

    /**
     * 自定义内容表。
     * - 留空（默认）：`InitializeDemoInventory()` 现场造 5 件内置定义；
     * - 填了：直接填进组件的 `ItemDefinitions`；但演示装载仍然用**内置定义**——
     *   摆位（尺寸 / 堆叠 / 子网格）都是按内置定义算的，换成任意资产摆不下。
     */
    UPROPERTY(EditAnywhere, Category = "演示")
    TArray<TObjectPtr<UInvItemDefinition>> DemoDefinitions;

    /**
     * BeginPlay 时是否自动建背包界面（`UInvInventoryScreenWidget`）并加进视口——
     * 零资产，Play 就能拖物品看效果。
     *
     * 没有 Player Controller 的场合（无头命令集 / 纯服务器）**不会报错**：只打一条 Info 日志跳过。
     */
    UPROPERTY(EditAnywhere, Category = "演示")
    bool bCreateUI = true;

    // ==================== 入口 ====================

    /**
     * 灌一套演示数据，并返回**文本摘要**（屏幕与日志用的就是这一份）。
     *
     * 步骤：准备内置定义 → 填组件配置 → `InitializeInventory()` → 承重上限 30 kg
     * → 演示装载 → 生成摘要。建实例失败时返回可读的中文错误文本，不崩。
     */
    UFUNCTION(BlueprintCallable, Category = "背包系统", meta = (DisplayName = "Initialize Demo Inventory"))
    FString InitializeDemoInventory();

private:
    /** 内置定义在 BuiltinDefinitions 里的固定下标（`EnsureBuiltinDefinitions` 按这个顺序建）。 */
    enum class EBuiltinDefinition : int32
    {
        Ammo = 0,
        Mag,
        Medkit,
        Grenade,
        Backpack,
    };

    /**
     * 内置演示定义（`DemoDefinitions` 留空时现场 `NewObject` 造）。
     * UPROPERTY 强引用防 GC——这些资产没有别的地方引用它们。
     */
    UPROPERTY(Transient, BlueprintReadOnly, Category = "演示", meta = (AllowPrivateAccess = "true"))
    TArray<TObjectPtr<UInvItemDefinition>> BuiltinDefinitions;

    /** 演示是否已经灌过（幂等闸门）。 */
    bool bDemoInitialized = false;

    /** 上次演示装载没落位的件数（> 0 时摘要里会多一行提示）。 */
    int32 LoadoutFailures = 0;

    /** 屏幕消息的固定 key：同一个 Actor 反复刷新只占一条消息，不会刷屏。 */
    uint64 ScreenMessageKey = 0;

    /** 屏幕刷新定时器；EndPlay 清掉。 */
    FTimerHandle RefreshTimerHandle;

    /**
     * 运行时建的背包界面控件（`bCreateUI` 打开时）。
     * `UPROPERTY` 强引用防 GC；EndPlay 从视口摘掉。
     */
    UPROPERTY(Transient)
    TObjectPtr<UInvInventoryScreenWidget> ScreenWidget = nullptr;

    /** `bCreateUI` 且当前有 Player Controller 时建界面并加进视口；否则只打日志。 */
    void CreateInventoryUI();

    /** EndPlay 把界面从视口摘掉（没建过是空操作）。 */
    void DestroyInventoryUI();

    /** 现场造 5 件内置定义（幂等）。 */
    void EnsureBuiltinDefinitions();

    /** 按固定下标取内置定义；没建起来返回空。 */
    UInvItemDefinition* GetBuiltinDefinition(EBuiltinDefinition Which) const;

    /** 组件上的 `RootContainers` / `ItemDefinitions` 为空时填默认值。 */
    void ApplyDefaultSetup();

    /** 灌演示装载；返回没落位的件数（失败只打中文警告，不中断其余装载）。 */
    int32 FillDemoLoadout();

    /** 落一件演示物品；失败记进 `InOutFailures` 并打中文警告。 */
    int64 PlaceDemoItem(UInvItemDefinition* Definition, int32 Stack, int64 Container, int32 Part,
        int32 X, int32 Y, bool bRotated, int32& InOutFailures);

    /** 生成摘要文本（网格 + 物品清单 + 负重汇总）：只读当前状态，随时可调。 */
    FString BuildSummaryText() const;

    /** 定时器回调：把当前摘要打到屏幕上（固定 key，覆盖旧消息）。 */
    void RefreshOnScreen();

    /** 刷新间隔（秒）；配置值 <= 0 时夹到下限。 */
    float ClampedRefreshInterval() const;
};
