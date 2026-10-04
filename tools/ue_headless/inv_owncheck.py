# 复核脚本：验证友好层新增的 FInvItemView.OwnContainer 字段（套包展开用）。
# 用法：UnrealEditor-Cmd <proj> -run=pythonscript -script=<本文件> -EnablePlugins=PythonScriptPlugin -unattended -nosplash
import os

import unreal

_HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
OUT = os.path.join(_HERE, "inv_owncheck_out.txt")
logs = []


def check(name, cond):
    logs.append(("[PASS] " if cond else "[FAIL] ") + name)
    return cond


def find_struct(res):
    """取 bool + 出参结构体的返回值（不同版本可能返回 tuple 或裸结构体）。"""
    if isinstance(res, tuple):
        for x in res:
            if isinstance(x, unreal.InvItemView):
                return x
        return None
    return res if isinstance(res, unreal.InvItemView) else None


comp = unreal.new_object(unreal.InvInventoryComponent)

# 一件 3x3 的套包（内部 6x5）
bag = unreal.new_object(unreal.InvItemDefinition)
bag.set_editor_property("width", 3)
bag.set_editor_property("height", 3)
bag.set_editor_property("max_stack", 1)
bag.set_editor_property("container_parts", [unreal.IntPoint(6, 5)])

setup = unreal.InvRootContainerSetup()
setup.set_editor_property("type", unreal.InvContainerType.BACKPACK)
setup.set_editor_property("label", "背包")
setup.set_editor_property("parts", [unreal.IntPoint(6, 5)])

comp.set_editor_property("root_containers", [setup])
comp.set_editor_property("item_definitions", [bag])
check("组件初始化", comp.initialize_inventory() is True)

backpack = comp.get_container(unreal.InvContainerType.BACKPACK)
check("背包句柄有效", backpack != 0)

item = comp.give_item(bag, backpack, 0, 0, 0, False, 1)
check("套包落位", item > 0)

view = find_struct(comp.get_item_view(item))
check("拿到视图", view is not None)

own = view.get_editor_property("own_container") if view is not None else 0
check("视图 OwnContainer 非 0（实际 %d）" % own, own != 0)

# 用这个句柄能直接展开内部：6x5 = 30 格
grid = comp.get_container_grid(own, 0)
check("用 OwnContainer 能取到内部占格表（30 格，实际 %d）" % len(grid), len(grid) == 30)

# 往里放一件普通物品，确认嵌套真的可用
plain = unreal.new_object(unreal.InvItemDefinition)
plain.set_editor_property("width", 1)
plain.set_editor_property("height", 1)
comp.register_definition(plain)
inner_item = comp.give_item(plain, own, 0, 0, 0, False, 1)
check("能把东西放进套包内部", inner_item > 0)
check("内部顶层 1 件", len(list(comp.get_container_items_view(own))) == 1)
check("背包顶层仍是 1 件（套包本体）", len(list(comp.get_container_items_view(backpack))) == 1)

passed = sum(1 for l in logs if l.startswith("[PASS]"))
failed = sum(1 for l in logs if l.startswith("[FAIL]"))
with open(OUT, "w") as f:
    f.write("\n".join(logs) + "\nSUMMARY pass=%d fail=%d\n" % (passed, failed))
unreal.log("OWNCHECK pass=%d fail=%d" % (passed, failed))
