#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ue_package_systemComponent.h"
#include "ue_package_systemTypes.h"

#include "ue_package_systemGridWidget.generated.h"

class UInvInventoryScreenWidget;
class USizeBox;

/**
 * 一个**容器子网格**的显示 + 交互控件（零资产依赖，可直接从蓝图面板 `Create Widget`）。
 *
 * 显示：格子底纹 + 物品图标（`UInvItemDefinition::Icon` 软引用，用时 `LoadSynchronous` 并**按定义缓存**；
 * 没有图标就画「底色 + 名字首字」的占位）+ 堆叠数量角标（`Stack > 1` 时才画）+ 拖拽幽灵（半透明）
 * + 合法 / 非法落点高亮 + 鼠标旁 tooltip。全部**自绘**（`NativePaint` 里画滑动元素）：
 * 没有建任何子控件、没有蓝图资产、没有字体资产，文字走 `FCoreStyle::GetDefaultFontStyle`。
 *
 * 交互：左键按下选中并开始拖拽 → 移动更新幽灵与落点 → 松开落位（同格已有物品时换成 `SwapItems`）。
 * `R` 旋转（拖拽中翻幽灵朝向；只选中时原地旋转）、`Esc` 取消拖拽（同时收起右键菜单）、
 * `Delete` 删除选中物品、`Ctrl + 左键` 拆一半并自动落位、**右键**开 / 关右键菜单（菜单本体在界面控件上）。
 * 鼠标事件只是事件的搬运工，**状态机本身（Begin/Update/Drop/Rotate/Cancel/SplitHalf/Delete）是可被测试直接驱动的公开节点**，
 * 不需要真的发鼠标消息。
 *
 * 跨容器拖（口袋 ↔ 弹挂 ↔ 背包）：鼠标被源格子捕获，所以源格子负责把屏幕坐标转播给界面，
 * 由界面上「光标下」的那一格画预览（`PreviewExternalDrag`）并在松开时接管落位
 * （`DropDragFromOtherGrid`，落点用屏幕坐标 + 源格子抓取偏移算，落位策略与同格落位完全一致）。
 *
 * 尺寸：构造时挂一个 `USizeBox` 根，期望尺寸 = `格子数 × CellSize`，所以
 * 「`Create Widget` → `Add To Viewport`」或放进 `Canvas Panel` 的自动尺寸槽位都能拿到正确大小；
 * 放进固定尺寸槽位时以槽位尺寸为准（重绘按槽位矩形铺格子）。
 *
 * 坐标约定：本地像素坐标原点 = 本控件左上角；格坐标 `(X 向右, Y 向下)`，`(0,0)` = 子网格左上角。
 * 返回约定沿用插件其它层：句柄类 `int64`（`> 0` 成功返回句柄、`0` = “无/不在本网格”、`< 0` = `-错误码`）、
 * 操作类 `int32`（`0` 成功 / `> 0` 错误码）、查询类 `bool` + 出参。
 * 组件没就绪 / 没绑定（`Inventory` 为空、容器或子网格下标无效）时：什么都不做、打一次中文警告日志，不崩。
 */
UCLASS(BlueprintType, meta = (DisplayName = "背包格子控件"))
class UE_PACKAGE_SYSTEM_API UInvGridWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UInvGridWidget(const FObjectInitializer& ObjectInitializer);

    // ==================== ExposeOnSpawn 配置 ====================

    /** 数据来源；为空时控件画空格子并打一次中文警告（不崩）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    TObjectPtr<UInvInventoryComponent> Inventory = nullptr;

    /** 容器句柄（`GetContainer` 的返回值）；`<= 0` 视作没配置。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    int64 Container = 0;

    /** 子网格下标；越界视作没配置。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    int32 Part = 0;

    /** 单格边长（像素）。<= 0 时按默认 48 处理。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    float CellSize = 48.f;

    /**
     * 图标 / 占位色块的**内缩**（像素，X = 左右、Y = 上下）。
     *
     * 只内缩图标与占位矩形：物品外框、选中高亮、堆叠角标、幽灵仍然贴物品占位的整块矩形，
     * 所以叶子格子看起来「图标留了点边距」但占位语义一格不差。
     * 配得过大时会被夹到「内缩后还剩 1 像素」，不会画成负尺寸。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "背包界面", meta = (ExposeOnSpawn = "true"))
    FVector2D IconPadding = FVector2D(2.f, 2.f);

    // ==================== 运行时状态（只读） ====================

    /** 本子网格里的物品视图（只含落在这块子网格上的顶层物品），`Refresh` 时拉取。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TArray<FInvItemView> Items;

    /** 本子网格的占格表（行主序，长度 = 宽 × 高）：`0` = 空格，否则是压在这格上的物品句柄。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TArray<int64> Occupancy;

    /** 本子网格尺寸，读作 `(宽, 高)`；取不到时为 `(0,0)`。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    FIntPoint GridSize = FIntPoint(0, 0);

    /** 当前选中的物品句柄；`0` = 没选中。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    int64 SelectedItem = 0;

    /** 正在拖拽的物品句柄；`0` = 没在拖。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    int64 DraggedItem = 0;

    /** 是否正在拖拽。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    bool bDragging = false;

    /** 拖拽幽灵当前朝向（`R` 切换）；开始拖拽时取物品当前朝向。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    bool bDragRotated = false;

    /** 拖拽幽灵锚点（物品左上角）吸附到的格；`bDragging` 为假时无意义。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    FIntPoint DropCell = FIntPoint(0, 0);

    /** 幽灵是否落在网格内（`ComputeDropTarget` 的返回值：占位尺寸放得下）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    bool bDropInGrid = false;

    /** 落点是否可放：在网格内、且涉及到的格子要么空、要么就是被拖的那件物品。画高亮用它。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    bool bDropAllowed = false;

    /** 上一次 `DropOnThisGrid` 是不是靠**自动换向重试**才成功的（给测试 / 日志用）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    bool bAutoRotatedOnLastDrop = false;

    /** 界面控件（`UInvInventoryScreenWidget`）；由界面创建时回填，用于「整理」找当前悬停的容器。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UInvInventoryScreenWidget> OwnerScreen = nullptr;

    /** 跨容器预览：正在拖东西的**源格子**（不属于本格子）；为空 = 没在预览。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    TObjectPtr<UInvGridWidget> ExternalDragSource = nullptr;

    /** 跨容器预览的朝向（源格子按 R 翻的是这个）。 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "背包界面|状态")
    bool bExternalDragRotated = false;

    // ==================== 纯函数（静态，不碰实例状态） ====================

    /**
     * 本地像素坐标 → 格坐标：`floor(LocalPos / InCellSize)`，越界夹到 `[0, InGridSize-1]`。
     * `InCellSize <= 0` 按默认值 48 处理；`InGridSize` 有一边是 0 时该边返回 0。
     */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Local Position To Cell"))
    static FIntPoint LocalPositionToCell(FVector2D LocalPos, float InCellSize, FIntPoint InGridSize);

    /** 物品占位尺寸：`bRotated` 为真时返回 `(高, 宽)`，否则 `(宽, 高)`；定义缺失时按 `(1,1)`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Footprint Cells"))
    static FIntPoint FootprintCells(const FInvItemView& View);

    /** 格坐标 → 该格左上角的本地像素坐标（`CellToLocalPosition(LocalPositionToCell(p))` 是格左上角）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Cell To Local Position"))
    static FVector2D CellToLocalPosition(FIntPoint Cell, float InCellSize);

    /**
     * 落点解算：把「光标位置 - 抓取偏移」得到的**物品左上角锚点**吸到格上，并夹取到物品放得下的范围。
     *
     * - `LocalPos` 传的是锚点（物品左上角想要的本地像素位置），抓取偏移由调用方（`UpdateDragCursor`）先减掉；
     * - 返回 `false` = 这件物品的占位尺寸在这个网格里根本放不下（`OutCell` 给 `(0,0)`）；
     * - 返回 `true` 时 `OutCell` 保证 `Outlet + 占位尺寸` **不超出右 / 下边界**（也保证不小于 0）。
     */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Compute Drop Target"))
    static bool ComputeDropTarget(const FInvItemView& Dragged, FVector2D LocalPos, float InCellSize,
        FIntPoint InGridSize, FIntPoint& OutCell);

    // ==================== 数据 ====================

    /**
     * 重新拉数据（子网格尺寸 + 占格表 + 物品视图 + 图标缓存），并按新几何重算布局。
     *
     * 配置非法（没绑定组件 / 容器或子网格下标无效）时清空数据、按 **配置变化** 打一次中文警告日志
     * （定时刷新不会刷屏），不崩。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Refresh"))
    void Refresh();

    /** 换一个子网格：改配置 → `Refresh`。传空组件等价于「解绑并清空」。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Set Container"))
    void SetContainer(UInvInventoryComponent* InInventory, int64 InContainer, int32 InPart);

    /**
     * 强制走一遍 Slate 构造（取 Slate 控件 → 跑构造回调 → 拉一次数据），返回是否拿到了 Slate 控件。
     *
     * 平时不用手动调（加进视口 / 父控件时会自动构造）；无头命令集里用它把「构造路径」跑通。
     * **没有 Slate 应用时**（无头命令集 / 专用服务器）会直接跳过并打一条中文日志、返回 `false`：
     * 那种环境下构造 Slate 控件会踩到引擎断言（`SlateApplicationBase.h` 的 `CurrentBaseApplication`）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Ensure Constructed"))
    bool EnsureConstructed();

    // ==================== 交互状态机（鼠标事件内部调它们，测试也可以直接调） ====================

    /**
     * 选中并开始拖拽一件**本子网格里**的物品，记录抓取偏移（光标相对物品左上角，像素）。
     *
     * 返回：`> 0` = 开始拖拽的物品句柄；`0` = 该句柄不在本子网格（没开始拖）；`< 0` = `-错误码`
     * （`-23` = 组件没绑定 / 句柄非法）。
     * 测试直接调它时没有真实光标，抓取偏移取「半格」，保证可复现。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Begin Drag At Item"))
    int64 BeginDragAtItem(int64 ItemHandle);

    /**
     * 更新拖拽：幽灵位置 = `LocalPos - 抓取偏移`，落点 = `ComputeDropTarget(幽灵锚点)`，
     * 再按占格表判定 `bDropAllowed`（涉及格要么空、要么是自己）。
     * 没在拖拽时只更新悬停格（tooltip 用），不产生别的状态。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Update Drag Cursor"))
    void UpdateDragCursor(FVector2D LocalPos);

    /**
     * 把拖拽中的物品落到本子网格的 `Cell`（物品左上角格坐标；越界会被夹到放得下的位置）。
     *
     * 顺序：`MoveItem` → 失败且定义可旋转时**自动换向重试一次**（`bAutoRotatedOnLastDrop` 记下来）
     * → 仍失败时，若目标格被**恰好一件**别的物品占着，改调 `SwapItems`（交换）。
     * 返回 `0` 或错误码（沿用组件层：`1` 越界 / `2` 被占 / `3` 旋转受阻 …）。
     * 没在拖拽 / 组件没就绪时返回 `23`（无效句柄）并打中文警告，状态不变。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Drop On This Grid"))
    int32 DropOnThisGrid(FIntPoint Cell, bool bRotated);

    /**
     * 旋转：拖拽中翻幽灵朝向（不动底层数据）；只选中没拖拽时对选中物品调 `RotateItem`（原地旋转）。
     * 返回 `0` 或错误码（`4` = 该定义不可旋转；两边都没有 → `23` 无效句柄 + 中文日志）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Rotate Dragged Or Selected"))
    int32 RotateDraggedOrSelected();

    /** 取消拖拽：幽灵 / 落点 / 选中全部清零（Esc 与落位失败收尾都走它）。没在拖也无害。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Cancel Drag"))
    void CancelDrag();

    // ==================== 右键菜单 / 修饰键 / 删除 ====================

    /**
     * 在某件物品上按右键：把请求转给界面控件（`OwnerScreen->RequestContextMenuForItem`）。
     *
     * - 同一个物品**再次**右键 = 收起菜单（右键是「开 / 关」的切换）；
     * - 没绑定界面控件（单独 `Create Widget` 用格子）时什么都不做，只打一条中文日志。
     *
     * 返回 `true` = 调用后菜单是**开着**的（这次开了菜单）；`false` = 收起了 / 没开成。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Notify Context Menu Requested"))
    bool NotifyContextMenuRequested(int64 ItemHandle, FVector2D ScreenPosition);

    /**
     * 修饰键点击（鼠标事件内部调它，测试 / 蓝图也可以直接调）：
     * `bCtrl` 为真 = `Ctrl + 左键` → **拆一半 + 自动落位**（复用 `SplitHalfItem`）；
     * `bCtrl` 为假 = 不是修饰点击 → 只做选中（等价 `SelectItem`），返回 `0`。
     *
     * 返回 `0` 或错误码（`22` 物品不在本子网格、`18` 堆叠数 < 2 拆不动、拆分落位失败的错误码…）；
     * **失败时物品状态零变化**。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Handle Modified Click"))
    int32 HandleModifiedClick(int64 ItemHandle, bool bCtrl);

    /** `Delete` 键：删掉**当前选中**的物品（含它套包里的东西）。返回 `0` 或错误码（`22` = 没有选中）。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Handle Delete Key"))
    int32 HandleDeleteKey();

    /** 拆一半**选中的**物品（右键菜单的「拆分一半」与 `Ctrl + 左键` 都汇到 `SplitHalfItem`）。没选中 → `22`。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Split Half Selected"))
    int32 SplitHalfSelected();

    /**
     * 拆分核心：从 `Handle` 这件物品里拆出**一半**（`Stack / 2`），并把新的那一半自动落位。
     *
     * 落点顺序 = **本子网格 → 同容器其它子网格（下标升序）**；每个落点先按物品**当前朝向**、
     * 定义可旋转时再试换向；定址走组件层的 `FindFreeCell`，落位走 `SplitStack`。首个成功即返回。
     *
     * 返回 `0` 或错误码：`22` 物品不在本子网格（句柄为空也算）、`18` 堆叠数 < 2（拆不出「一半」）、
     * `2` 本容器所有子网格都放不下这一半、其余沿用 `SplitStack` 的错误码。**失败时状态零变化**。
     * 界面控件的右键菜单复用这个函数，避免两份拆分逻辑。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Split Half Item"))
    int32 SplitHalfItem(int64 Handle);

    /** 本格子上是不是正开着右键菜单（界面没绑定 / 菜单没开时为 false）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Is Context Menu Open"))
    bool IsContextMenuOpen() const;

    // ==================== 选中 ====================

    /** 选中一件物品（不在本子网格里就当作清空选择）。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Select Item"))
    void SelectItem(int64 ItemHandle);

    /** 清空选择。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Clear Selection"))
    void ClearSelection();

    // ==================== 跨容器拖（口袋 ↔ 弹挂 ↔ 背包） ====================

    /**
     * 跨容器拖的预览：`SourceGrid` 正拖着东西、光标悬停在本格子上时，本格子画落点高亮 + 半透明幽灵。
     * **不改本格子的拖拽状态**，只影响绘制；传空 / 源格子没在拖 → 等价于清掉预览。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Preview External Drag"))
    void PreviewExternalDrag(UInvGridWidget* SourceGrid, FVector2D ScreenPosition, bool bRotated);

    /** 清掉跨容器拖的落点预览（幽灵 + 高亮）。没在预览时是空操作。 */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Clear External Drag Preview"))
    void ClearExternalDragPreview();

    /**
     * 跨容器落位：把 `SourceGrid` 正在拖的那件物品落到**本**子网格
     * （落点按屏幕坐标与源格子的抓取偏移算，所以源格子把鼠标捕获走了也没关系）。
     *
     * 收到的物品走与 `DropOnThisGrid` 完全一样的落位策略
     * （`MoveItem` → 可旋转时自动换向重试一次 → 与占位物品交换）；成功 / 失败都会收尾源格子的拖拽状态，
     * 成功时两边都刷新。返回 `0` 或错误码（`23` = 状态 / 配置不对）。
     */
    UFUNCTION(BlueprintCallable, Category = "背包界面", meta = (DisplayName = "Drop Drag From Other Grid"))
    int32 DropDragFromOtherGrid(UInvGridWidget* SourceGrid, FVector2D ScreenPosition, bool bRotated);

    // ==================== 查询（UI / 测试用） ====================

    /** 取本子网格里某件物品的视图；不在本子网格返回 `false`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Item View In Grid"))
    bool GetItemViewInGrid(int64 ItemHandle, FInvItemView& OutView) const;

    /** 某个格上的物品句柄；越界或空格返回 `0`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Item At Cell"))
    int64 GetItemAtCell(FIntPoint Cell) const;

    /** 本子网格的标题：`背包 part0 (6x5)`；部件号从 0 起，尺寸读作 `(宽 x 高)`。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Grid Title"))
    FString GetGridTitle() const;

    /** 本子网格的像素尺寸 = `GridSize × CellSize`（`CellSize` 非法时按默认值算）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Grid Pixel Size"))
    FVector2D GetGridPixelSize() const;

    /**
     * 悬停 tooltip 文本：物品名 / 描述 / 重量 / 估值 / 堆叠。
     * 句柄不在本子网格（或组件没绑定）时返回空串。
     */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Build Tooltip Text"))
    FString BuildTooltipText(int64 ItemHandle) const;

    /** 实际生效的单格边长（配置 `<= 0` 时是默认 48）。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Effective Cell Size"))
    float GetEffectiveCellSize() const;

    /** 堆叠角标的字号（像素）：`ScaledFontSize(16, 实际单格边长)`，随 `CellSize` 缩放。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Stack Badge Font Size"))
    int32 GetStackBadgeFontSize() const;

    /** tooltip 的字号（像素）：`ScaledFontSize(16, 实际单格边长)`，随 `CellSize` 缩放。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Tooltip Font Size"))
    int32 GetTooltipFontSize() const;

    /** 界面控件（`OwnerScreen`）；`Create Widget` 单独用时为空。 */
    UFUNCTION(BlueprintPure, Category = "背包界面", meta = (DisplayName = "Get Owner Screen"))
    UInvInventoryScreenWidget* GetOwnerScreen() const;

    // ==================== 绘制 / 交互（Slate 回调） ====================

    /** 释放 Slate 资源时把 `EnsureConstructed` 抓的那手引用也放掉（不然会吊着 Slate 控件不放）。 */
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;

    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
        const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
        const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
    virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
    virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
    virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
    virtual void NativeOnMouseLeave(const FPointerEvent& InMouseEvent) override;
    virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

    /** 本控件的默认单格边长（像素）；`CellSize <= 0` 时用它。 */
    static constexpr float DefaultCellSize = 48.f;

    /** 没有真实光标时的抓取偏移：取物品中心（占位尺寸的 0.5 倍），保证测试可复现。 */
    static constexpr float DefaultGrabOffsetFactor = 0.5f;

private:
    /** 抓取偏移（像素，本地坐标）：`幽灵锚点 = 光标 - DragGrabOffset`。 */
    FVector2D DragGrabOffset = FVector2D::ZeroVector;

    /** 幽灵位置（本地坐标，物品左上角）；画半透明预览用它。 */
    FVector2D DragLocalPos = FVector2D::ZeroVector;

    /** 跨容器预览的幽灵锚点（本地像素，物品左上角）。 */
    FVector2D ExternalDragLocalPos = FVector2D::ZeroVector;

    /** 最近一次鼠标本地坐标（悬停 tooltip 与抓取偏移都读它）。 */
    FVector2D LastCursorLocal = FVector2D::ZeroVector;

    /** 最近一次鼠标是否在本控件上（`NativeOnMouseLeave` 置假）。 */
    bool bHasCursor = false;

    /** 悬停到的物品句柄（tooltip 用）；`0` = 没悬停在物品上。 */
    int64 HoveredItem = 0;

    /** 配置非法的中文警告只打一次（`Refresh` 可能被定时器高频调用）。 */
    bool bLoggedInvalidConfig = false;

    /** 图标缓存：定义 → 已加载的贴图（没有图标 / 加载失败为空）。 */
    UPROPERTY(Transient)
    TMap<TObjectPtr<UInvItemDefinition>, TObjectPtr<UTexture2D>> IconCache;

    /** 挂上的根 `USizeBox`（期望尺寸 = 子网格像素尺寸；构造时建，`Refresh` 改它的覆盖值）。 */
    UPROPERTY(Transient)
    TObjectPtr<USizeBox> RootSizeBox = nullptr;

    /**
     * `EnsureConstructed` 抓的一手 Slate 根控件引用。
     * `TakeWidget()` 的返回值没人持有的话会当场被回收，控件的缓存立刻失效——所以这里自己拿着，
     * `ReleaseSlateResources` 时再放掉。
     */
    TSharedPtr<SWidget> BuiltSlateRoot;

    /** 配置文件（与最近一次日志一起去重）。 */
    FIntPoint LoggedConfig = FIntPoint(-1, -1);

    /** 实际生效的单格边长。 */
    float EffectiveCellSize() const;

    /** 物品占位尺寸（像素）。 */
    FVector2D FootprintPixels(const FInvItemView& View) const;

    /** 物品占位尺寸（格），按任意朝向算（`FootprintCells` 的内部实现）。 */
    static FIntPoint FootprintCellsFor(const UInvItemDefinition* Definition, bool bRotated);

    /** 把格坐标夹到「占位尺寸放得下」的范围内（网格比物品还小时归零）。 */
    static FIntPoint ClampCellToGrid(FIntPoint Cell, FIntPoint Cells, FIntPoint GridSize);

    /** 找本子网格里的物品视图；没有返回 `false`。 */
    bool FindItemView(int64 ItemHandle, FInvItemView& OutView) const;

    /** 某个格坐标对应的占格表下标；越界返回 `INDEX_NONE`。 */
    int32 OccupancyIndex(FIntPoint Cell) const;

    /** 给定落点与占位尺寸，判断能否放下（空格，或全是被拖的这件物品自己）。 */
    bool CanOccupy(FIntPoint Cell, FIntPoint Cells, int64 SelfItem) const;

    /** 落点涉及的格子里，**恰好一件**别的物品时返回它的句柄，否则 `0`（多件 / 没有都不算）。 */
    int64 SingleOccupantAt(FIntPoint Cell, FIntPoint Cells, int64 SelfItem) const;

    /** 配置是否可用（组件就绪 + 容器 / 子网格下标有效）。 */
    bool HasValidTarget() const;

    /** 配置非法时的中文警告（按配置去重，只打一次）。 */
    void LogInvalidConfigOnce(const TCHAR* Reason);

    /** 成功的落位 / 交换 / 拆分之后：收尾拖拽 → 刷新自己 → 通知界面刷全部子格子。 */
    void FinishDropAndRefresh();

    /**
     * 落位核心（`DropOnThisGrid` 与跨容器落位共用）：
     * `MoveItem` → 失败且定义可旋转时自动换向重试一次 → 仍失败时与目标格的**单件**占位物品交换。
     * 返回 `0` 或错误码；不碰拖拽状态、不打「失败」以外的日志。
     */
    int32 PlaceItemIntoThisGrid(int64 ItemHandle, const FInvItemView& ItemView, FIntPoint Cell, bool bRotated);

    /** 跨容器预览 / 落位的本地锚点解算：屏幕坐标 →（本格子本地坐标 - 源格子抓取偏移）。 */
    FVector2D ExternalDragAnchorFromScreen(const UInvGridWidget* SourceGrid, FVector2D ScreenPosition) const;

    /** 清掉拖拽状态（幽灵 / 落点 / 抓取偏移），选中**保留**（落位失败收尾用；`CancelDrag` 才是全清）。 */
    void ClearDragState();

    /**
     * 把一次失败报给界面控件（广播 `OnOperationFailed` + 记镜像字段 + 错误音效）。
     * 没绑定界面时只留下格子自己的中文日志。
     */
    void ReportFailureToScreen(int32 Code, const FString& Message);

    /** 实际生效的图标内缩：夹到「内缩后还剩 1 像素」，配置得再离谱也不会画出负尺寸。 */
    FVector2D ClampedIconPadding(const FVector2D& Size) const;

    /** 把 `OwnerScreen` 记成当前悬停 / 操作的这一格（「整理」按钮靠它找容器）。 */
    void NotifyOwnerScreenFocused();

    /** 把 `RootSizeBox` 的覆盖尺寸对齐当前几何（期望尺寸 = 子网格像素尺寸）。 */
    void ApplyDesiredSizeToRoot() const;

    /** 拖拽幽灵 / 落点解算用的视图：朝向换成 `bDragRotated`（`R` 键翻的就是它）。 */
    FInvItemView MakeDropView(const FInvItemView& Source) const;

    /** 让控件重画（没构造过时是空操作）。 */
    void RequestRepaint() const;

    /** 画一个实心矩形（本地像素坐标）。 */
    void DrawRect(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
        const FVector2D& Min, const FVector2D& Size, const FLinearColor& Color) const;

    /** 画一圈描边（`Thickness` 像素）。 */
    void DrawBorder(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
        const FVector2D& Min, const FVector2D& Size, const FLinearColor& Color, float Thickness) const;

    /** 画一行文字（`Alignment` 是相对 `Position` 的对齐：0 = 左上、0.5 = 居中）。 */
    void DrawTextLine(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
        const FString& Text, const FVector2D& Position, int32 FontSize, const FLinearColor& Color,
        const FVector2D& Alignment = FVector2D(0.f, 0.f)) const;

    /** 画一件物品（图标或占位 + 堆叠角标）；`Alpha` 给拖拽中的原件压暗用。 */
    void DrawItem(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
        const FInvItemView& View, const FVector2D& Min, const FVector2D& Size, bool bRotated, float Alpha) const;

    /** 画 tooltip（鼠标旁的框 + 多行文本）。 */
    void DrawTooltip(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry) const;

    /** 图标缓存里的一次查找：命中且已加载返回贴图，否则返回空（空的走占位画法）。 */
    UTexture2D* FindCachedIcon(const UInvItemDefinition* Definition) const;

    /** 文字宽度的估算（字符数 × 字号系数，中日韩按两倍宽）；不依赖 Slate 渲染器，无头也能用。 */
    static float EstimateTextWidth(const FString& Text, int32 FontSize);
};
