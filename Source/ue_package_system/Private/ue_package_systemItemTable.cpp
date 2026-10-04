#include "ue_package_systemItemTable.h"

#include "ue_package_systemItemDefinition.h"

void FInvItemDefinitionRow::ApplyToDefinition(UInvItemDefinition& Target) const
{
    // 逐字段照抄，字段名与顺序与 UInvItemDefinition 保持一致，方便对着看有没有漏。
    Target.DisplayName = DisplayName;
    Target.Description = Description;
    Target.Icon = Icon;

    Target.Width = Width;
    Target.Height = Height;
    Target.bRotatable = bRotatable;
    Target.MaxStack = MaxStack;

    Target.Weight = Weight;
    Target.Value = Value;
    Target.TagBits = TagBits;
    Target.ForbiddenContainerTypes = ForbiddenContainerTypes;

    Target.ContainerParts = ContainerParts;
}
