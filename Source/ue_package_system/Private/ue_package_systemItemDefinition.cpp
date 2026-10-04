#include "ue_package_systemItemDefinition.h"

bool UInvItemDefinition::HasContainer() const
{
    // 约定与 C ABI 一致：ContainerParts 非空 = 套包 / 弹挂。
    return ContainerParts.Num() > 0;
}

FIntPoint UInvItemDefinition::Footprint(bool bRotated) const
{
    // 旋转只换长宽，不改面积；具体落位校验由 Rust 核心做，这里只回答“占多大”。
    // 下限夹到 1：注册时组件也会这么夹，保证这里给出的就是实际占位（尺寸填 0 的物品不会算出 0x0）。
    const int32 ClampedWidth = FMath::Max(Width, 1);
    const int32 ClampedHeight = FMath::Max(Height, 1);
    return bRotated ? FIntPoint(ClampedHeight, ClampedWidth) : FIntPoint(ClampedWidth, ClampedHeight);
}
