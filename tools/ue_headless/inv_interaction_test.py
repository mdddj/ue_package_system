# 背包「自动落位 / 右键菜单 / 拆分 / 删除」的无头验证（在 UE Python 命令集里跑）。
#
#   UnrealEditor-Cmd <proj> -run=pythonscript -script=<此文件> \
#       -EnablePlugins=PythonScriptPlugin -unattended -nosplash
#
# 覆盖的七块（与上一批交付的界面层解耦：全部走公开蓝图节点，不需要鼠标 / 键盘 / Slate 应用）：
#   F. 组件 FindFreeCell：空网格 / 第一个空位 / 行主序 / 碎片化 / 旋转尺寸 / 全满 / 越界 Size / 无效 part；
#   A. 组件 TryAutoPlace：同容器与跨容器的自动落位、换向重试、全满 → 2 且位置零变化、参数非法；
#   M. 右键菜单：可见性 / 句柄 / 位置、四种组合下的可用性过滤与按钮收起、五种动作的执行与失败路径；
#   C. 拆分：HandleModifiedClick（Ctrl）/ SplitHalfSelected / SplitHalfItem 的落点回退（本 part → 其它 part）；
#   D. Delete 键：有选中 → 删除成功；无选中 → 22；空配置 → 23；
#   I. IconPadding：只影响画图标，不影响 GetGridPixelSize 等纯函数；
#   E. 事件镜像 + 三个音效留空（零资产）时全流程不崩、失败路径确实记到 OnOperationFailed 的镜像字段。
#
# 结果写脚本同目录 inv_interaction_test_out.txt（末行 SUMMARY）；每个失败行都带实际值，方便直接定位。

import os
import traceback

import unreal

_HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
OUT = os.path.join(_HERE, "inv_interaction_test_out.txt")
LINES = []
PASS = 0
FAIL = 0
KEEP = []          # 强引用，防临时对象被回收
_SHAPE_NOTE = []   # 只记一次绑定返回形态


def emit(msg):
    LINES.append(msg)
    unreal.log_warning("[INVIR] " + msg)


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        emit("[PASS] " + msg)
    else:
        FAIL += 1
        emit("[FAIL] " + msg)


def note(msg):
    emit("[INFO] " + msg)


def dump():
    with open(OUT, "w") as f:
        f.write("\n".join(LINES) + "\n")
        f.write("SUMMARY pass=%d fail=%d\n" % (PASS, FAIL))


# ---------- 小工具（与 inv_ui_test.py 同款约定） ----------

def ipt(x, y):
    """FIntPoint（在背包系统里读作 (宽, 高)）。"""
    try:
        return unreal.IntPoint(x, y)
    except Exception:
        p = unreal.IntPoint()
        p.set_editor_property("x", x)
        p.set_editor_property("y", y)
        return p


def v2(x, y):
    try:
        return unreal.Vector2D(x, y)
    except Exception:
        v = unreal.Vector2D()
        v.set_editor_property("x", x)
        v.set_editor_property("y", y)
        return v


def xy(p):
    return (p.x, p.y) if p is not None else None


def as_list(res):
    return list(res) if res is not None else []


def attr(obj, *names):
    """按名字取属性；布尔属性在 python 里不带 `b` 前缀，所以多给几个候选名。"""
    for n in names:
        try:
            if hasattr(obj, n):
                return getattr(obj, n)
        except Exception:
            pass
    return None


def bool_attr(obj, *names):
    v = attr(obj, *names)
    return None if v is None else bool(v)


def _enum_value(enum_cls, *names):
    """按候选名取枚举值：python 把 C++ 枚举项名转成大写蛇形（SplitHalf → SPLIT_HALF）。"""
    if enum_cls is None:
        return None
    for n in names:
        v = getattr(enum_cls, n, None)
        if v is not None:
            return v
    return None


def make_def(name, w, h, stack=1, rot=False, weight=0.0, value=0, display="", parts=None):
    d = unreal.new_object(unreal.InvItemDefinition, name=name)
    d.set_editor_property("display_name", display)
    d.set_editor_property("description", display + "的说明文本")
    d.set_editor_property("width", w)
    d.set_editor_property("height", h)
    d.set_editor_property("rotatable", rot)
    d.set_editor_property("max_stack", stack)
    d.set_editor_property("weight", weight)
    d.set_editor_property("value", value)
    if parts:
        d.set_editor_property("container_parts", [ipt(p[0], p[1]) for p in parts])
    KEEP.append(d)
    return d


def make_root(kind, label, parts):
    s = unreal.InvRootContainerSetup()
    s.set_editor_property("type", kind)
    s.set_editor_property("label", label)
    s.set_editor_property("parts", [ipt(p[0], p[1]) for p in parts])
    return s


def make_comp(name, defs, roots, cap=0.0):
    c = unreal.new_object(unreal.InvInventoryComponent, name=name)
    c.set_editor_property("item_definitions", defs)
    c.set_editor_property("root_containers", roots)
    c.set_editor_property("max_capacity_kg", cap)
    KEEP.append(c)
    return c, c.initialize_inventory()


def make_grid(comp, container, part, cell=48.0):
    g = unreal.new_object(unreal.InvGridWidget)
    KEEP.append(g)
    if comp is not None:
        g.set_editor_property("inventory", comp)
    g.set_editor_property("container", container)
    g.set_editor_property("part", part)
    g.set_editor_property("cell_size", cell)
    g.refresh()
    return g


def make_screen(comp, cell=48.0):
    s = unreal.new_object(unreal.InvInventoryScreenWidget)
    KEEP.append(s)
    if comp is not None:
        s.set_editor_property("inventory", comp)
    s.set_editor_property("cell_size", cell)
    s.refresh()   # 无 Slate 应用时跳过 Slate 那层，但 UObject 层的控件树照建
    return s


def view_of(comp, handle):
    res = comp.get_item_view(handle)
    for it in (res if isinstance(res, tuple) else (res,)):
        if isinstance(it, unreal.InvItemView):
            return it
    return None


def pos_of(comp, handle):
    v = view_of(comp, handle)
    return (v.x, v.y, bool(v.rotated)) if v else None


def host_of(comp, handle):
    v = view_of(comp, handle)
    return (v.host_container, v.host_part) if v else None


def stack_of(comp, handle):
    v = view_of(comp, handle)
    return v.stack if v else None


def free_cell(comp, container, part, size, rotated=False):
    """`find_free_cell` 的 python 约定：找得到 → 落点 `(x, y)`；找不到 → None。

    绑定形态可能是 `FIntPoint` / `(bool, FIntPoint)` / `(FIntPoint,)`，这里都兜住
    （只在第一次调用时把原始形态记一行，方便以后确认绑定真的没变）。"""
    res = comp.find_free_cell(container, part, size, rotated)
    if not _SHAPE_NOTE:
        _SHAPE_NOTE.append(True)
        note("FindFreeCell 绑定返回形态：%r" % (res,))
    if res is None:
        return None
    parts = res if isinstance(res, tuple) else (res,)
    if len(parts) == 2 and isinstance(parts[0], (bool, int)) and not isinstance(parts[0], unreal.IntPoint):
        ok, cell = parts[0], parts[1]
        return xy(cell) if (ok and isinstance(cell, unreal.IntPoint)) else None
    for it in parts:
        if isinstance(it, unreal.IntPoint):
            return xy(it)
    return None


def container_of(comp, kind):
    return comp.get_container(kind)


def grids_of(screen):
    return [g for g in screen.get_grid_widgets() if g is not None]


def grid_for(screen, container, part=0):
    for g in grids_of(screen):
        if int(attr(g, "container")) == container and int(attr(g, "part")) == part:
            return g
    return None


def items_of(comp, container):
    return list(comp.get_container_items_view(container))


def item_count(comp, container):
    return len(items_of(comp, container))


def vis_text(widget):
    v = attr(widget, "visibility")
    return str(v) if v is not None else None


def menu_button_shows(screen, index):
    """第 `index` 个菜单按钮是不是「可用」（不是 Collapsed）。"""
    buttons = as_list(attr(screen, "context_menu_buttons"))
    if index >= len(buttons) or buttons[index] is None:
        return None
    t = vis_text(buttons[index])
    return None if t is None else ("COLLAPSED" not in t)


BP = _enum_value(unreal.InvContainerType, "BACKPACK")
POCKETS = _enum_value(unreal.InvContainerType, "POCKETS")
SAFEBOX = _enum_value(unreal.InvContainerType, "SAFE_BOX", "SAFEBOX")
CHESTRIG = _enum_value(unreal.InvContainerType, "CHEST_RIG", "CHESTRIG")

# 菜单动作：名字 + 枚举候选名 + 在菜单里的下标（= 枚举顺序）
_ACTION_SPECS = [
    ("Rotate", ("ROTATE",)),
    ("SplitHalf", ("SPLIT_HALF", "SPLITHALF")),
    ("Use", ("USE",)),
    ("Destroy", ("DESTROY",)),
    ("AutosortContainer", ("AUTOSORT_CONTAINER", "AUTOSORT")),
]
try:
    _CTX_ENUM = unreal.InvContextAction
except Exception:
    _CTX_ENUM = None
ACTIONS = [(name, i, _enum_value(_CTX_ENUM, *cands)) for i, (name, cands) in enumerate(_ACTION_SPECS)]
ACTION_NAME_BY_INDEX = {i: name for name, i, _ in ACTIONS}


def action_names(screen, item):
    """可用动作 → 名字列表（顺序就是枚举顺序）；认不出的原样给字符串。"""
    names = []
    for value in as_list(screen.get_available_context_actions(item)):
        matched = None
        for name, _, enum_value in ACTIONS:
            if enum_value is not None and value == enum_value:
                matched = name
                break
        names.append(matched if matched else str(value))
    return names


def invoke(screen, index):
    value = None
    for _, i, enum_value in ACTIONS:
        if i == index:
            value = enum_value
            break
    if value is None:
        note("拿不到右键菜单动作枚举值（下标 %d），invoke 跳过" % index)
        return None
    return screen.invoke_context_action(value)


# ==================== F. FindFreeCell ====================

def section_find_free_cell():
    emit("----- F. 组件 FindFreeCell（行主序 / 旋转 / 碎片化 / 越界）-----")

    one = make_def("IrDef_One", 1, 1, stack=1, rot=False, weight=0.1, display="小件")
    wide3 = make_def("IrDef_Wide3", 3, 1, stack=1, rot=False, weight=1.0, display="长条")
    plate = make_def("IrDef_Plate", 1, 2, stack=1, rot=True, weight=0.5, display="弹匣")
    comp, ok = make_comp("IrComp_Free", [one, wide3, plate],
                         [make_root(BP, "背包", [(6, 5)]),
                          make_root(SAFEBOX, "安全箱", [(2, 2)]),
                          make_root(POCKETS, "口袋", [(1, 1)])],
                         cap=0.0)
    check(ok, "F0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    safebox = container_of(comp, SAFEBOX)
    pockets = container_of(comp, POCKETS)

    # ---- 1. 空网格：第一个空位就是左上角 ----
    cell = free_cell(comp, backpack, 0, ipt(1, 1))
    check(cell == (0, 0), "F1 空网格找 1x1 → %s（期望 (0, 0)）" % (cell,))
    cell = free_cell(comp, backpack, 0, ipt(6, 5))
    check(cell == (0, 0), "F2 空网格找整块 6x5 → %s（期望 (0, 0)）" % (cell,))

    # ---- 2. 行主序：角上被占 → 让到右边一格 ----
    placed_ok = comp.give_item(one, backpack, 0, 0, 0, False, 1)
    check(placed_ok > 0, "F3 摆一件 1x1 到 (0,0)：%s" % (pos_of(comp, placed_ok),))
    cell = free_cell(comp, backpack, 0, ipt(1, 1))
    check(cell == (1, 0), "F3b (0,0) 被 1x1 占了 → 1x1 的落点 %s（期望 (1, 0)）" % (cell,))

    # ---- 3. 整行占满 → 换到下一行行首（行主序，先左右后上下）----
    row_fill = [
        comp.give_item(wide3, backpack, 0, 1, 0, False, 1),   # (1,0)-(3,0)
        comp.give_item(one, backpack, 0, 4, 0, False, 1),     # (4,0)
        comp.give_item(one, backpack, 0, 5, 0, False, 1),     # (5,0)
    ]
    check(all(h > 0 for h in row_fill), "F4 把第 0 行摆满（3x1 + 两件 1x1）：%s"
          % ([h > 0 for h in row_fill],))
    cell = free_cell(comp, backpack, 0, ipt(1, 1))
    check(cell == (0, 1), "F5 第 0 行占满 → 1x1 落到 %s（期望 (0, 1)，行主序）" % (cell,))

    # ---- 4. 旋转尺寸：同一个尺寸换朝向后「放得下」变成「放不下」----
    # 安全箱 2x2 里 (0,0)+(0,1) 被一件 1x2 占着：
    #   占位 (1,2)（不旋转）→ 右边一列 (1,0)(1,1) 空着 → (1,0)
    #   占位 (2,1)（旋转）  → 两行都得占 (0,·) → 冲突 → 放不下
    comp.give_item(plate, safebox, 0, 0, 0, False, 1)
    unrotated = free_cell(comp, safebox, 0, ipt(1, 2), False)
    rotated = free_cell(comp, safebox, 0, ipt(1, 2), True)
    check(unrotated == (1, 0), "F6 占位 (1,2)（不旋转）→ %s（期望 (1, 0)）" % (unrotated,))
    check(rotated is None, "F7 同一尺寸旋转成 (2,1) → %s（期望 None：两行都被挡）" % (rotated,))

    # ---- 5. 全满 ----
    comp.give_item(one, pockets, 0, 0, 0, False, 1)
    cell = free_cell(comp, pockets, 0, ipt(1, 1))
    check(cell is None, "F8 1x1 的口袋被占满 → %s（期望 None，放不下）" % (cell,))

    # ---- 6. 越界 / 非法尺寸：不将就，直接 false ----
    check(free_cell(comp, backpack, 0, ipt(7, 1)) is None, "F9 尺寸比子网格宽（7x1 对 6x5）→ None")
    check(free_cell(comp, backpack, 0, ipt(1, 6)) is None, "F10 尺寸比子网格高（1x6 对 6x5）→ None")
    check(free_cell(comp, backpack, 0, ipt(0, 1)) is None, "F11 宽 0 → None（每边至少 1 格）")
    check(free_cell(comp, backpack, 0, ipt(1, 0)) is None, "F12 高 0 → None")
    check(free_cell(comp, backpack, 0, ipt(-1, 1)) is None, "F13 负宽 → None")

    # ---- 7. 无效 part / 无效容器 ----
    check(free_cell(comp, backpack, 9, ipt(1, 1)) is None, "F14 子网格下标 9（越界）→ None")
    check(free_cell(comp, backpack, -1, ipt(1, 1)) is None, "F15 子网格下标 -1 → None")
    check(free_cell(comp, 999999, 0, ipt(1, 1)) is None, "F16 容器句柄 999999（无效）→ None")

    # ---- 8. 碎片化：单格有空，但连不成 2x1 ----
    frag, ok_frag = make_comp("IrComp_Frag", [one],
                              [make_root(BP, "背包", [(2, 2)])],
                              cap=0.0)
    check(ok_frag, "F17 碎片化用的组件 InitializeInventory 成功")
    if not ok_frag:
        return
    frag_bp = container_of(frag, BP)
    comp_ok = True
    comp_ok = comp_ok and frag.give_item(one, frag_bp, 0, 0, 0, False, 1) > 0
    comp_ok = comp_ok and frag.give_item(one, frag_bp, 0, 1, 1, False, 1) > 0
    check(comp_ok, "F18 对角摆两件 1x1（(0,0) 与 (1,1)），中间格子空着但连不成整块")
    single = free_cell(frag, frag_bp, 0, ipt(1, 1))
    pair = free_cell(frag, frag_bp, 0, ipt(2, 1))
    check(single == (1, 0), "F19 1x1 仍有位置 → %s（期望 (1, 0)）" % (single,))
    check(pair is None, "F20 2x1 排不下（碎片化）→ %s（期望 None）" % (pair,))


# ==================== A. TryAutoPlace ====================

def section_auto_place():
    emit("----- A. 组件 TryAutoPlace（自动落位 / 换向 / 全满）-----")

    ammo = make_def("IrDef_Ammo", 1, 1, stack=60, rot=False, weight=0.01, display="子弹")
    plate = make_def("IrDef_Plate2", 1, 2, stack=1, rot=True, weight=0.5, display="弹匣")
    comp, ok = make_comp("IrComp_Auto", [ammo, plate],
                         [make_root(BP, "背包", [(6, 5)]),
                          make_root(POCKETS, "口袋", [(1, 1)]),
                          make_root(CHESTRIG, "弹挂", [(3, 2)])],
                         cap=0.0)
    check(ok, "A0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    pockets = container_of(comp, POCKETS)
    rig = container_of(comp, CHESTRIG)

    # ---- 1. 同一容器内自动落位：位置 = 行主序第一个空位 ----
    item_a = comp.give_item(ammo, backpack, 0, 4, 3, False, 1)
    check(item_a > 0 and pos_of(comp, item_a) == (4, 3, False),
          "A1 摆一件 1x1 到 (4,3)：%s" % (pos_of(comp, item_a),))
    placed = comp.try_auto_place(item_a, backpack)
    check(placed == 0, "A2 TryAutoPlace（同容器）→ 0（实际 %s）" % placed)
    check(pos_of(comp, item_a) == (0, 0, False),
          "A3 自动落位到第一个空位：%s（期望 (0, 0, false)）" % (pos_of(comp, item_a),))

    # ---- 2. 1x2 的物品：行主序找得到 (1,0)（(0,0) 已被占）----
    item_b = comp.give_item(plate, backpack, 0, 5, 3, False, 1)   # (5,3)-(5,4)：6x5 里放得下
    check(item_b > 0, "A4 摆一件 1x2 到 (5,3)：%s" % (pos_of(comp, item_b),))
    placed = comp.try_auto_place(item_b, backpack)
    check(placed == 0, "A5 TryAutoPlace（1x2 物品）→ 0（实际 %s）" % placed)
    check(pos_of(comp, item_b) == (1, 0, False),
          "A6 1x2 落到 %s（期望 (1, 0, false)：行主序第一个放得下 (1,0)-(1,1) 的位置）"
          % (pos_of(comp, item_b),))

    # ---- 3. 跨容器：从背包挪到弹挂的空位 ----
    item_c = comp.give_item(ammo, backpack, 0, 5, 0, False, 1)
    placed = comp.try_auto_place(item_c, rig)
    check(placed == 0, "A7 TryAutoPlace（跨容器：背包 → 弹挂）→ 0（实际 %s）" % placed)
    check(host_of(comp, item_c) == (rig, 0) and pos_of(comp, item_c) == (0, 0, False),
          "A8 物品换容器了：宿主 %s / 位置 %s（期望 (%s, 0) / (0, 0, false)）"
          % (host_of(comp, item_c), pos_of(comp, item_c), rig))

    # ---- 4. 全满：返回 2（Occupied）且位置零变化 ----
    item_d = comp.give_item(ammo, pockets, 0, 0, 0, False, 1)
    check(item_d > 0 and pos_of(comp, item_d) == (0, 0, False), "A9 1x1 的口袋被一件 1x1 占满")
    before = pos_of(comp, item_d)
    placed = comp.try_auto_place(item_d, pockets)
    check(placed == 2, "A10 TryAutoPlace（容器全满）→ 2（Occupied，实际 %s）" % placed)
    check(pos_of(comp, item_d) == before,
          "A11 失败时位置零变化：%s → %s（期望 %s）" % (before, pos_of(comp, item_d), before))

    # ---- 5. 换向重试：竖直 1x2 放不下，转成 2x1 就有位置 ----
    # 弹挂 3x2：(0,0)(1,0)(2,1) 各占一件 1x1 → 每列的竖直位都被挡，但 (0,1)-(1,1) 横着两格空着。
    rig_c, ok_rigc = make_comp("IrComp_AutoRig", [ammo, plate],
                               [make_root(BP, "背包", [(6, 5)]),
                                make_root(CHESTRIG, "弹挂", [(3, 2)])],
                               cap=0.0)
    check(ok_rigc, "A12 换向重试用的组件（背包 + 弹挂）InitializeInventory 成功")
    if not ok_rigc:
        return
    bp_c = container_of(rig_c, BP)
    rig_d = container_of(rig_c, CHESTRIG)
    ok_fill = True
    for (cx, cy) in ((0, 0), (1, 0), (2, 1)):
        ok_fill = ok_fill and rig_c.give_item(ammo, rig_d, 0, cx, cy, False, 1) > 0
    check(ok_fill, "A13 三件 1x1 挡住所有竖直 1x2 位（(0,0)/(1,0)/(2,1)）")
    check(free_cell(rig_c, rig_d, 0, ipt(1, 2), False) is None,
          "A14 竖直 1x2 在弹挂里确实没有位置（对照：%s）"
          % (free_cell(rig_c, rig_d, 0, ipt(1, 2), False),))
    check(free_cell(rig_c, rig_d, 0, ipt(1, 2), True) == (0, 1),
          "A15 横过来 (2,1) 有位置 → %s（期望 (0, 1)）"
          % (free_cell(rig_c, rig_d, 0, ipt(1, 2), True),))

    mover = rig_c.give_item(plate, bp_c, 0, 0, 0, False, 1)   # 弹匣当前朝向 = 不旋转
    check(mover > 0 and pos_of(rig_c, mover) == (0, 0, False),
          "A16 弹匣先放在背包（当前朝向不旋转）：%s" % (pos_of(rig_c, mover),))
    placed = rig_c.try_auto_place(mover, rig_d)
    check(placed == 0, "A17 TryAutoPlace 靠换向成功 → 0（实际 %s）" % placed)
    check(pos_of(rig_c, mover) == (0, 1, True),
          "A18 换向后落到 %s（期望 (0, 1, true)：竖直放不下、横过来才行）" % (pos_of(rig_c, mover),))

    # ---- 6. 参数非法 ----
    check(comp.try_auto_place(0, backpack) == 22, "A19 TryAutoPlace(0, …) → 22（参数非法）")
    check(comp.try_auto_place(123456789, backpack) == 23, "A20 TryAutoPlace(野句柄, …) → 23（无效句柄）")


# ==================== M. 右键菜单 ====================

def section_context_menu():
    emit("----- M. 右键菜单（可用性过滤 / 五种动作）-----")

    both = make_def("IrDef_Both", 2, 1, stack=10, rot=True, weight=0.4, display="可转可叠")
    no_stack = make_def("IrDef_NoStack", 1, 1, stack=1, rot=False, weight=0.3, display="单件")
    rot_only = make_def("IrDef_RotOnly", 1, 1, stack=1, rot=True, weight=0.3, display="可转单件")
    stack_only = make_def("IrDef_StackOnly", 1, 1, stack=10, rot=False, weight=0.3, display="可叠不可转")
    comp, ok = make_comp("IrComp_Menu", [both, no_stack, rot_only, stack_only],
                         [make_root(BP, "背包", [(6, 5)]),
                          make_root(POCKETS, "口袋", [(1, 1)])],
                         cap=0.0)
    check(ok, "M0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    pockets = container_of(comp, POCKETS)

    item_both = comp.give_item(both, backpack, 0, 0, 0, False, 8)        # 可转 + 可叠（8 件）
    item_single = comp.give_item(no_stack, backpack, 0, 3, 0, False, 1)  # 不可转 + 不可叠
    item_rot = comp.give_item(rot_only, backpack, 0, 4, 0, False, 1)     # 可转 + 不可叠
    item_stack = comp.give_item(stack_only, backpack, 0, 5, 0, False, 3)  # 不可转 + 可叠（3 件）
    item_one = comp.give_item(stack_only, backpack, 0, 5, 2, False, 1)   # 不可转 + 可叠但只有 1 件
    check(min(item_both, item_single, item_rot, item_stack, item_one) > 0,
          "M1 摆好四件对照物品（句柄 %s）"
          % ([item_both, item_single, item_rot, item_stack, item_one],))

    screen = make_screen(comp)
    check(len(grids_of(screen)) == 2, "M2 界面建了 2 块子格子（背包 + 口袋），实际 %d" % len(grids_of(screen)))
    bp_grid = grid_for(screen, backpack)
    pockets_grid = grid_for(screen, pockets)
    check(bp_grid is not None and pockets_grid is not None, "M3 背包 / 口袋的子格子都拿到了")

    # ---- 1. 可用性过滤：四种组合 + 「可叠但只有 1 件」 ----
    names_both = action_names(screen, item_both)
    check(names_both == ["Rotate", "SplitHalf", "Use", "Destroy", "AutosortContainer"],
          "M4 可转 + 可叠（8 件）→ %s（期望五个动作齐全，顺序固定）" % (names_both,))

    names_single = action_names(screen, item_single)
    check(names_single == ["Use", "Destroy", "AutosortContainer"],
          "M5 不可转 + 不可叠 → %s（期望 Use/Destroy/AutosortContainer）" % (names_single,))

    names_rot = action_names(screen, item_rot)
    check(names_rot == ["Rotate", "Use", "Destroy", "AutosortContainer"],
          "M6 可转 + 不可叠 → %s（期望 Rotate/Use/Destroy/AutosortContainer）" % (names_rot,))

    names_stack = action_names(screen, item_stack)
    check(names_stack == ["SplitHalf", "Use", "Destroy", "AutosortContainer"],
          "M7 不可转 + 可叠（3 件）→ %s（期望 SplitHalf/Use/Destroy/AutosortContainer）" % (names_stack,))

    names_one = action_names(screen, item_one)
    check(names_one == ["Use", "Destroy", "AutosortContainer"],
          "M8 可叠但只有 1 件 → %s（期望没有 SplitHalf：拆不出「一半」）" % (names_one,))

    check(action_names(screen, 0) == [], "M9 句柄 0 → 可用动作为空数组（%s）" % (action_names(screen, 0),))
    check(screen.get_available_context_actions(123456789) is not None, "M9b 野句柄不崩（返回空数组）")

    # ---- 2. 开菜单：可见性 / 句柄 / 位置 ----
    check(screen.is_context_menu_visible() is False, "M10 初始不显示菜单")
    screen.request_context_menu_for_item(item_both, v2(300.0, 200.0))
    check(screen.is_context_menu_visible() is True, "M11 RequestContextMenuForItem 之后菜单可见")
    check(screen.get_context_menu_item_handle() == item_both,
          "M12 菜单目标句柄 = %s（期望 %s）" % (screen.get_context_menu_item_handle(), item_both))
    pos = attr(screen, "context_menu_position")
    check(pos is not None and abs(pos.x - 300.0) < 1e-3 and abs(pos.y - 200.0) < 1e-3,
          "M13 菜单落在请求的屏幕坐标 (%s, %s)（期望 (300, 200)）"
          % (pos.x if pos else None, pos.y if pos else None))

    # ---- 3. 按钮收起：下标 = 枚举顺序，不可用的按钮必须是 Collapsed ----
    shows = [menu_button_shows(screen, i) for i in range(5)]
    check(len(as_list(attr(screen, "context_menu_buttons"))) == 5,
          "M14 菜单正好 5 个按钮（实际 %d）" % len(as_list(attr(screen, "context_menu_buttons"))))
    check(shows == [True, True, True, True, True],
          "M15 五种动作都可用时按钮全部放开：%s" % (shows,))

    screen.request_context_menu_for_item(item_single, v2(10.0, 10.0))
    shows = [menu_button_shows(screen, i) for i in range(5)]
    check(shows == [False, False, True, True, True],
          "M16 不可转 + 不可叠：旋转 / 拆分一半收起，其余放开 → %s（下标 0=Rotate,1=SplitHalf）" % (shows,))

    # ---- 4. 没有目标时执行动作 → 22 + 失败事件 ----
    failed_before = int(attr(screen, "failed_event_count") or 0)
    screen.close_context_menu()
    check(screen.is_context_menu_visible() is False, "M17 CloseContextMenu 之后菜单不可见")
    check(screen.get_context_menu_item_handle() == 0, "M18 关掉菜单后目标句柄清 0")
    no_target = invoke(screen, 0)
    check(no_target == 22, "M19 没有目标时 InvokeContextAction → 22（实际 %s）" % no_target)
    check(int(attr(screen, "failed_event_count") or 0) == failed_before + 1,
          "M20 失败事件计数 +1（%s → %s）"
          % (failed_before, attr(screen, "failed_event_count")))
    check(int(attr(screen, "last_failed_code") or 0) == 22,
          "M21 镜像字段 LastFailedCode = %s（期望 22）" % attr(screen, "last_failed_code"))
    check(len(str(attr(screen, "last_failed_message") or "")) > 0,
          "M22 镜像字段 LastFailedMessage 非空：%r" % attr(screen, "last_failed_message"))

    # ---- 5. 拆分一半（菜单动作）----
    screen.request_context_menu_for_item(item_both, v2(50.0, 50.0))
    check(screen.is_context_menu_visible() is True, "M23 重新开菜单（同一件物品）")
    before_items = item_count(comp, backpack)
    split_result = invoke(screen, 1)
    check(split_result == 0, "M24 菜单「拆分一半」→ 0（实际 %s）" % split_result)
    check(stack_of(comp, item_both) == 4,
          "M25 原堆叠对半：8 → %s（期望 4）" % stack_of(comp, item_both))
    views = items_of(comp, backpack)
    check(len(views) == before_items + 1,
          "M26 容器里多了一件：%d → %d（期望 %d）" % (before_items, len(views), before_items + 1))
    halves = [v for v in views if v.handle != item_both and v.definition == both]
    check(len(halves) == 1 and halves[0].stack == 4,
          "M27 新物品存在且是另一半：事件 %d 件 / 堆叠 %s"
          % (len(halves), halves[0].stack if halves else None))
    check(screen.is_context_menu_visible() is False, "M28 动作成功后菜单自动收起")
    if halves:
        # 第 0 行被 (0,0)+(1,0) 的 2x1、(3,0)、(4,0)、(5,0) 占着 → 2x1 只能落到第 1 行行首。
        check((halves[0].x, halves[0].y) == (0, 1),
              "M29 新的一半落到行主序第一个放得下的空位 (0,1)：实际 %s" % ((halves[0].x, halves[0].y),))

    # ---- 6. 旋转（菜单动作）----
    screen.request_context_menu_for_item(item_rot, v2(60.0, 60.0))
    rotated_before = bool(view_of(comp, item_rot).rotated)
    rotate_result = invoke(screen, 0)
    rotated_after = bool(view_of(comp, item_rot).rotated)
    check(rotate_result == 0 and rotated_after != rotated_before,
          "M30 菜单「旋转」→ %s，bRotated %s → %s"
          % (rotate_result, rotated_before, rotated_after))

    # 不可旋转的定义：直接执行 → 4（NotRotatable），菜单里本来也不会出现这个按钮
    screen.request_context_menu_for_item(item_single, v2(60.0, 60.0))
    rotate_bad = invoke(screen, 0)
    check(rotate_bad == 4, "M31 对不可旋转的物品执行「旋转」→ 4（实际 %s）" % rotate_bad)

    # ---- 7. 使用（只广播 + 记句柄）----
    screen.request_context_menu_for_item(item_stack, v2(60.0, 60.0))
    used_before = int(attr(screen, "used_event_count") or 0)
    use_result = invoke(screen, 2)
    check(use_result == 0, "M32 菜单「使用」→ 0（实际 %s）" % use_result)
    check(int(attr(screen, "used_event_count") or 0) == used_before + 1,
          "M33 OnItemUsed 计数 +1（%s → %s）" % (used_before, attr(screen, "used_event_count")))
    check(int(attr(screen, "last_used_item_handle") or 0) == item_stack,
          "M34 LastUsedItemHandle = %s（期望 %s）" % (attr(screen, "last_used_item_handle"), item_stack))
    check(stack_of(comp, item_stack) == 3, "M35 「使用」不改堆叠（仍是 %s）" % stack_of(comp, item_stack))

    # ---- 8. 整理这件物品所在容器 ----
    screen.request_context_menu_for_item(item_one, v2(60.0, 60.0))
    sort_result = invoke(screen, 4)
    check(sort_result == 0, "M36 菜单「整理容器」→ 0（实际 %s）" % sort_result)
    check(item_count(comp, backpack) == before_items + 1,
          "M37 整理后物品一件不少：%d 件" % item_count(comp, backpack))

    # ---- 9. 销毁 ----
    destroyed_target = item_one
    destroy_before = int(attr(screen, "destroyed_event_count") or 0)
    screen.request_context_menu_for_item(destroyed_target, v2(70.0, 70.0))
    destroy_result = invoke(screen, 3)
    check(destroy_result == 0, "M38 菜单「销毁」→ 0（实际 %s）" % destroy_result)
    check(view_of(comp, destroyed_target) is None,
          "M39 物品真的没了（GetItemView 失败）")
    check(int(attr(screen, "last_destroyed_item_handle") or 0) == destroyed_target,
          "M40 LastDestroyedItemHandle = %s（期望 %s）"
          % (attr(screen, "last_destroyed_item_handle"), destroyed_target))
    check(int(attr(screen, "destroyed_event_count") or 0) == destroy_before + 1,
          "M41 OnItemDestroyed 计数 +1（%s → %s）"
          % (destroy_before, attr(screen, "destroyed_event_count")))
    check(all(v.handle != destroyed_target for v in items_of(comp, backpack)),
          "M42 容器列表里也没有它了")

    # ---- 10. 无效句柄开菜单：不开，也不崩 ----
    screen.close_context_menu()
    screen.request_context_menu_for_item(987654321, v2(10.0, 10.0))
    check(screen.is_context_menu_visible() is False, "M43 野句柄请求菜单 → 菜单仍不可见")

    # ---- 11. 子格子侧：右键路由（同一物品再次右键 = 关）----
    toggled = bp_grid.notify_context_menu_requested(item_stack, v2(120.0, 120.0))
    check(toggled is True and screen.is_context_menu_visible() is True,
          "M44 子格子右键 → 菜单打开（返回 %s / 可见 %s）" % (toggled, screen.is_context_menu_visible()))
    check(screen.get_context_menu_item_handle() == item_stack,
          "M45 菜单目标 = 右键的那件 %s（实际 %s）"
          % (item_stack, screen.get_context_menu_item_handle()))
    check(bool_attr(bp_grid, "is_context_menu_open") is True, "M46 本格子的 IsContextMenuOpen = true")
    check(pockets_grid is not None and pockets_grid.is_context_menu_open() is False,
          "M47 别的格子（口袋）的 IsContextMenuOpen = false")
    toggled_again = bp_grid.notify_context_menu_requested(item_stack, v2(120.0, 120.0))
    check(toggled_again is False and screen.is_context_menu_visible() is False,
          "M48 对同一件物品再次右键 → 收起菜单（返回 %s / 可见 %s）"
          % (toggled_again, screen.is_context_menu_visible()))

    # 没有绑定界面控件的格子：右键只打日志、不崩
    lonely = make_grid(comp, backpack, 0)
    lonely_result = lonely.notify_context_menu_requested(item_stack, v2(0.0, 0.0))
    check(lonely_result is False, "M49 没绑定界面的格子右键 → false（不崩，实际 %s）" % lonely_result)


# ==================== C. 拆分 ====================

def section_split():
    emit("----- C. 拆分（Ctrl 点击 / 拆分选中 / 落点回退）-----")

    stack1 = make_def("IrDef_S1", 1, 1, stack=10, rot=False, weight=0.1, display="子弹")
    solid = make_def("IrDef_Solid", 1, 1, stack=1, rot=False, weight=0.2, display="单件")
    comp, ok = make_comp("IrComp_Split", [stack1, solid],
                         [make_root(BP, "背包", [(6, 5)]),
                          make_root(CHESTRIG, "弹挂", [(1, 1), (1, 1)])],
                         cap=0.0)
    check(ok, "C0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    rig = container_of(comp, CHESTRIG)
    grid = make_grid(comp, backpack, 0)

    # ---- 1. Ctrl + 左键 = 拆一半 ----
    item_a = comp.give_item(stack1, backpack, 0, 3, 3, False, 6)
    grid.refresh()
    selected_before = int(attr(grid, "selected_item") or 0)
    before = item_count(comp, backpack)
    modified = grid.handle_modified_click(item_a, True)
    check(modified == 0, "C1 HandleModifiedClick(item, Ctrl) → 0（实际 %s）" % modified)
    check(stack_of(comp, item_a) == 3, "C2 原来那件剩一半：6 → %s（期望 3）" % stack_of(comp, item_a))
    views = items_of(comp, backpack)
    check(len(views) == before + 1, "C3 多了一件：%d → %d" % (before, len(views)))
    halves = [v for v in views if v.handle != item_a and v.definition == stack1]
    check(len(halves) == 1 and halves[0].stack == 3,
          "C4 拆出来的一半是 3 件：%s" % (halves[0].stack if halves else None))
    check(int(attr(grid, "selected_item") or 0) == selected_before,
          "C5 拆分不动选中状态：%s → %s（期望 %s）"
          % (selected_before, attr(grid, "selected_item"), selected_before))

    # ---- 2. 不可堆叠：非 0 错误码 + 状态零变化 ----
    item_solid = comp.give_item(solid, backpack, 0, 5, 4, False, 1)
    grid.refresh()
    before_items = item_count(comp, backpack)
    before_pos = pos_of(comp, item_solid)
    before_stack = stack_of(comp, item_solid)
    failed = grid.handle_modified_click(item_solid, True)
    check(failed != 0 and failed == 18,
          "C6 不可堆叠物品 Ctrl 点击 → 18（InvalidStackCount，实际 %s）" % failed)
    check(item_count(comp, backpack) == before_items and stack_of(comp, item_solid) == before_stack
          and pos_of(comp, item_solid) == before_pos,
          "C7 失败后状态零变化（件数 %d / 堆叠 %s / 位置 %s）"
          % (item_count(comp, backpack), stack_of(comp, item_solid), pos_of(comp, item_solid)))

    # ---- 3. 非修饰点击：只选中，不拆 ----
    before_items = item_count(comp, backpack)
    plain = grid.handle_modified_click(item_a, False)
    check(plain == 0 and int(attr(grid, "selected_item")) == item_a and item_count(comp, backpack) == before_items,
          "C8 不带 Ctrl 的点击 → 0 且只做选中（选中 %s / 件数 %d → %d）"
          % (attr(grid, "selected_item"), before_items, item_count(comp, backpack)))

    # ---- 4. SplitHalfSelected ----
    grid.cancel_drag()
    check(attr(grid, "selected_item") == 0, "C9 CancelDrag 清掉选中（%s）" % attr(grid, "selected_item"))
    no_selection = grid.split_half_selected()
    check(no_selection == 22, "C10 没选中时 SplitHalfSelected → 22（实际 %s）" % no_selection)
    grid.select_item(item_a)
    selected_split = grid.split_half_selected()
    check(selected_split == 0 and stack_of(comp, item_a) == 2,
          "C11 选中后 SplitHalfSelected → %s，堆叠 3 → %s（3/2 = 1，剩 2）"
          % (selected_split, stack_of(comp, item_a)))

    # ---- 5. 拆分无处可放 → 2，状态零变化 ----
    tight, ok_tight = make_comp("IrComp_SplitTight", [stack1, solid],
                                [make_root(POCKETS, "口袋", [(1, 1)])],
                                cap=0.0)
    check(ok_tight, "C12 1x1 口袋的组件 InitializeInventory 成功")
    if not ok_tight:
        return
    tight_pockets = container_of(tight, POCKETS)
    tight_item = tight.give_item(stack1, tight_pockets, 0, 0, 0, False, 5)
    tight_grid = make_grid(tight, tight_pockets, 0)
    no_room = tight_grid.handle_modified_click(tight_item, True)
    check(no_room == 2, "C13 1x1 口袋里拆一半（没地方放）→ 2（Occupied，实际 %s）" % no_room)
    check(stack_of(tight, tight_item) == 5 and item_count(tight, tight_pockets) == 1,
          "C14 放不下时零变化：堆叠 %s / 件数 %d（期望 5 / 1）"
          % (stack_of(tight, tight_item), item_count(tight, tight_pockets)))

    # ---- 6. 落点回退：本 part 满 → 同容器下一块子网格 ----
    rig_item = comp.give_item(stack1, rig, 0, 0, 0, False, 4)
    rig_grid = make_grid(comp, rig, 0)
    check(rig_item > 0 and rig_grid is not None, "C15 弹挂 0 号口里放一件 4 件的堆叠")
    fallback = rig_grid.split_half_item(rig_item)
    check(fallback == 0, "C16 本口放不下时回退到同容器其它口 → 0（实际 %s）" % fallback)
    check(stack_of(comp, rig_item) == 2, "C17 原来那件剩 2 件（实际 %s）" % stack_of(comp, rig_item))
    rig_views = items_of(comp, rig)
    new_halves = [v for v in rig_views if v.handle != rig_item]
    check(len(new_halves) == 1 and new_halves[0].host_part == 1 and new_halves[0].stack == 2,
          "C18 拆出来的一半落在 1 号口：part=%s / 堆叠 %s"
          % (new_halves[0].host_part if new_halves else None, new_halves[0].stack if new_halves else None))

    # ---- 7. 句柄不在本子网格 / 非法 → 22 ----
    elsewhere = comp.give_item(stack1, backpack, 0, 1, 4, False, 4)
    not_here = rig_grid.split_half_item(elsewhere)
    check(not_here == 22, "C19 拆一件不在本子网格的物品 → 22（实际 %s）" % not_here)
    check(rig_grid.split_half_item(0) == 22, "C20 空句柄拆分 → 22")


# ==================== D. Delete 键 ====================

def section_delete_key():
    emit("----- D. Delete 键（有选中 → 删；无选中 → 22）-----")

    solid = make_def("IrDef_Del", 1, 1, stack=1, rot=False, weight=0.2, display="废料")
    comp, ok = make_comp("IrComp_Del", [solid],
                         [make_root(BP, "背包", [(6, 5)])],
                         cap=0.0)
    check(ok, "D0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    screen = make_screen(comp)
    grid = grid_for(screen, backpack)
    check(grid is not None, "D1 界面里有背包的子格子")

    item_a = comp.give_item(solid, backpack, 0, 0, 0, False, 1)
    item_b = comp.give_item(solid, backpack, 0, 2, 0, False, 1)
    grid.refresh()
    check(item_a > 0 and item_b > 0 and item_count(comp, backpack) == 2,
          "D2 摆两件物品（%d 件）" % item_count(comp, backpack))

    # 没选中 → 22 + 失败事件
    grid.clear_selection()
    failed_before = int(attr(screen, "failed_event_count") or 0)
    none_selected = grid.handle_delete_key()
    check(none_selected == 22, "D3 没有选中时 HandleDeleteKey → 22（实际 %s）" % none_selected)
    check(int(attr(screen, "failed_event_count") or 0) == failed_before + 1,
          "D4 没选中也算一次失败事件（%s → %s）"
          % (failed_before, attr(screen, "failed_event_count")))

    # 有选中 → 删除成功
    grid.select_item(item_a)
    destroyed_before = int(attr(screen, "destroyed_event_count") or 0)
    deleted = grid.handle_delete_key()
    check(deleted == 0, "D5 选中后 HandleDeleteKey → 0（实际 %s）" % deleted)
    check(view_of(comp, item_a) is None, "D6 物品真的被删了")
    check(item_count(comp, backpack) == 1, "D7 容器里剩 %d 件（期望 1）" % item_count(comp, backpack))
    check(attr(grid, "selected_item") == 0, "D8 删完清掉选中（%s）" % attr(grid, "selected_item"))
    check(int(attr(screen, "last_destroyed_item_handle") or 0) == item_a,
          "D9 LastDestroyedItemHandle = %s（期望 %s）"
          % (attr(screen, "last_destroyed_item_handle"), item_a))
    check(int(attr(screen, "destroyed_event_count") or 0) == destroyed_before + 1,
          "D10 销毁事件 +1（%s → %s）" % (destroyed_before, attr(screen, "destroyed_event_count")))

    # 选中的东西已经不在了：等同没选中 → 22
    grid.select_item(item_b)
    comp.destroy_item(item_b)
    grid.refresh()
    stale = grid.handle_delete_key()
    check(stale == 22, "D11 「选中的东西已经被别处删了」再按删除 → 22（实际 %s）" % stale)

    # 没有绑定界面的格子：删除一样能跑（不崩）
    lone_item = comp.give_item(solid, backpack, 0, 4, 4, False, 1)
    lonely = make_grid(comp, backpack, 0)
    lonely.select_item(lone_item)
    lonely_delete = lonely.handle_delete_key()
    check(lonely_delete == 0 and view_of(comp, lone_item) is None,
          "D12 没挂界面的格子删除 → %s，物品已删除" % lonely_delete)

    # 空配置：23
    empty = make_grid(None, 0, 0)
    check(empty.handle_delete_key() == 23, "D13 空配置格子的 Delete → 23（实际 %s）" % empty.handle_delete_key())


# ==================== I. IconPadding ====================

def section_icon_padding():
    emit("----- I. IconPadding（只影响画图标，不动几何）-----")

    solid = make_def("IrDef_Pad", 1, 1, stack=1, rot=False, weight=0.2, display="方块")
    comp, ok = make_comp("IrComp_Pad", [solid],
                         [make_root(BP, "背包", [(6, 5)])],
                         cap=0.0)
    check(ok, "I0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    grid = make_grid(comp, backpack, 0)
    item = comp.give_item(solid, backpack, 0, 2, 1, False, 1)
    grid.refresh()

    default_pad = attr(grid, "icon_padding")
    check(default_pad is not None and abs(default_pad.x - 2.0) < 1e-4 and abs(default_pad.y - 2.0) < 1e-4,
          "I1 IconPadding 默认 (2,2)（实际 %s）"
          % ((default_pad.x, default_pad.y) if default_pad else None,))

    pixel_before = (grid.get_grid_pixel_size().x, grid.get_grid_pixel_size().y)
    cell_before = grid.get_effective_cell_size()
    title_before = grid.get_grid_title()
    view = view_of(comp, item)
    footprint_before = call_cell_text(grid, view)

    for pad in ((0.0, 0.0), (8.0, 8.0), (1000.0, 1000.0)):
        grid.set_editor_property("icon_padding", v2(pad[0], pad[1]))
        grid.refresh()
        pixel_now = (grid.get_grid_pixel_size().x, grid.get_grid_pixel_size().y)
        check(pixel_now == pixel_before and abs(grid.get_effective_cell_size() - cell_before) < 1e-4
              and grid.get_grid_title() == title_before,
              "I2 IconPadding %s 不影响像素尺寸 / 单格边长 / 标题（%s / %s / %r）"
              % (pad, pixel_now, grid.get_effective_cell_size(), grid.get_grid_title()))
        check(call_cell_text(grid, view) == footprint_before,
              "I3 IconPadding %s 不影响 FootprintCells（%s）" % (pad, call_cell_text(grid, view)))
        check(len(grid.items) == 1 and len(grid.occupancy) == 30,
              "I4 IconPadding %s 之后数据照常（物品 %d 件 / 占格表 %d 格）"
              % (pad, len(grid.items), len(grid.occupancy)))

    # 夹取：配得再离谱也只是「内缩到剩 1 像素」，绘制不会出负尺寸
    grid.set_editor_property("icon_padding", v2(1000.0, 1000.0))
    grid.refresh()
    check(abs(grid.get_grid_pixel_size().x - 288.0) < 1e-4 and abs(grid.get_grid_pixel_size().y - 240.0) < 1e-4,
          "I5 极端 IconPadding 下像素尺寸仍是 (288, 240)（实际 %s）"
          % ((grid.get_grid_pixel_size().x, grid.get_grid_pixel_size().y),))
    grid.set_editor_property("icon_padding", v2(2.0, 2.0))
    grid.refresh()


def call_cell_text(grid, view):
    """FootprintCells 是静态节点：python 里挂在类上，失败再用实例兜底。"""
    try:
        return xy(unreal.InvGridWidget.footprint_cells(view))
    except Exception:
        try:
            return xy(grid.footprint_cells(view))
        except Exception as exc:
            note("FootprintCells 调用失败：%s" % exc)
            return None


# ==================== E. 事件镜像 / 音效留空 ====================

def section_events_and_sounds():
    emit("----- E. 事件镜像 + 三个音效留空（零资产）-----")

    ammo = make_def("IrDef_EvAmmo", 1, 1, stack=60, rot=False, weight=0.01, display="子弹")
    solid = make_def("IrDef_EvSolid", 1, 1, stack=1, rot=False, weight=0.2, display="单件")
    comp, ok = make_comp("IrComp_Event", [ammo, solid],
                         [make_root(BP, "背包", [(6, 5)])],
                         cap=0.0)
    check(ok, "E0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    screen = make_screen(comp)
    grid = grid_for(screen, backpack)
    check(grid is not None, "E1 界面里有背包的子格子")

    # 三个音效默认留空 + 总开关默认开
    check(attr(screen, "pick_up_sound") is None and attr(screen, "drop_sound") is None
          and attr(screen, "error_sound") is None,
          "E2 三个音效默认留空（%s / %s / %s）"
          % (attr(screen, "pick_up_sound"), attr(screen, "drop_sound"), attr(screen, "error_sound")))
    check(bool_attr(screen, "play_sounds") is True, "E3 bPlaySounds 默认开（但三个音效为空 = 静音）")

    check(int(attr(screen, "picked_up_event_count") or 0) == 0
          and int(attr(screen, "dropped_event_count") or 0) == 0
          and int(attr(screen, "used_event_count") or 0) == 0
          and int(attr(screen, "destroyed_event_count") or 0) == 0
          and int(attr(screen, "failed_event_count") or 0) == 0,
          "E4 事件计数初始全 0（%s / %s / %s / %s / %s）"
          % (attr(screen, "picked_up_event_count"), attr(screen, "dropped_event_count"),
             attr(screen, "used_event_count"), attr(screen, "destroyed_event_count"),
             attr(screen, "failed_event_count")))

    item_a = comp.give_item(ammo, backpack, 0, 0, 0, False, 5)
    item_b = comp.give_item(solid, backpack, 0, 3, 3, False, 1)
    grid.refresh()

    # ---- 1. 拿起 → 放下（成功）----
    started = grid.begin_drag_at_item(item_a)
    check(started == item_a, "E5 BeginDragAtItem 成功（%s）" % started)
    check(int(attr(screen, "picked_up_event_count") or 0) == 1,
          "E6 拿起事件计数 = %s（期望 1，音效留空也没崩）" % attr(screen, "picked_up_event_count"))
    dropped = grid.drop_on_this_grid(ipt(0, 4), False)
    check(dropped == 0, "E7 落位成功 → 0（实际 %s）" % dropped)
    check(int(attr(screen, "dropped_event_count") or 0) == 1,
          "E8 放下事件计数 = %s（期望 1）" % attr(screen, "dropped_event_count"))

    # ---- 2. 失败路径：不可堆叠的 Ctrl 点击 → 18 + OnOperationFailed 镜像 ----
    failed_before = int(attr(screen, "failed_event_count") or 0)
    modified = grid.handle_modified_click(item_b, True)
    check(modified == 18, "E9 不可堆叠物品 Ctrl 点击 → 18（实际 %s）" % modified)
    check(int(attr(screen, "failed_event_count") or 0) == failed_before + 1,
          "E10 失败路径确实触发了 OnOperationFailed（计数 %s → %s）"
          % (failed_before, attr(screen, "failed_event_count")))
    check(int(attr(screen, "last_failed_code") or 0) == 18,
          "E11 LastFailedCode = %s（期望 18）" % attr(screen, "last_failed_code"))
    check("一半" in str(attr(screen, "last_failed_message") or ""),
          "E12 LastFailedMessage 是中文字说明：%r" % attr(screen, "last_failed_message"))

    # ---- 3. 失败路径：1x2 的物品拖到「两件不同物品占着」的位置 → 2 + 失败事件 ----
    plate_ev = make_def("IrDef_EvPlate", 1, 2, stack=1, rot=False, weight=0.5, display="钢板")
    plate_item = comp.give_item(plate_ev, backpack, 0, 2, 0, False, 1)   # 占 (2,0)+(2,1)
    comp.give_item(solid, backpack, 0, 5, 1, False, 1)
    comp.give_item(solid, backpack, 0, 5, 2, False, 1)
    grid.refresh()
    check(plate_item > 0, "E13 摆一块 1x2 钢板（%s）与两件挡路的 1x1" % (pos_of(comp, plate_item),))
    grid.begin_drag_at_item(plate_item)
    grid.update_drag_cursor(v2(5 * 48.0 + 24.0, 1 * 48.0 + 48.0))
    failed_before = int(attr(screen, "failed_event_count") or 0)
    dropped_before = int(attr(screen, "dropped_event_count") or 0)
    refused = grid.drop_on_this_grid(ipt(5, 1), False)
    check(refused == 2, "E14 拖到被两件不同物品占着的格子（1x2 也换不了向）→ 2（实际 %s）" % refused)
    check(int(attr(screen, "dropped_event_count") or 0) == dropped_before + 1,
          "E15 失败的落位也会广播 OnItemDropped（计数 %s → %s）"
          % (dropped_before, attr(screen, "dropped_event_count")))
    check(int(attr(screen, "failed_event_count") or 0) == failed_before + 1,
          "E16 失败的落位记了一次 OnOperationFailed（%s → %s）"
          % (failed_before, attr(screen, "failed_event_count")))

    # ---- 4. 关掉总开关：整条链路照常跑（音效确实只是「可选」）----
    screen.set_editor_property("play_sounds", False)
    check(bool_attr(screen, "play_sounds") is False, "E17 bPlaySounds 可以关掉")
    item_d = comp.give_item(ammo, backpack, 0, 1, 0, False, 3)
    screen.request_context_menu_for_item(item_d, v2(20.0, 20.0))
    used_before = int(attr(screen, "used_event_count") or 0)
    use_result = invoke(screen, 2)
    check(use_result == 0 and int(attr(screen, "used_event_count") or 0) == used_before + 1,
          "E18 关掉音效后「使用」照常：返回 %s / 计数 %s → %s"
          % (use_result, used_before, attr(screen, "used_event_count")))
    screen.set_editor_property("play_sounds", True)

    # ---- 5. 空配置界面：事件节点照样能调（不崩）----
    empty_screen = make_screen(None)
    empty_screen.notify_operation_failed(7, "测试用失败说明")
    check(int(attr(empty_screen, "failed_event_count") or 0) == 1
          and int(attr(empty_screen, "last_failed_code") or 0) == 7,
          "E19 空配置界面直接报失败也不崩（计数 %s / 码 %s）"
          % (attr(empty_screen, "failed_event_count"), attr(empty_screen, "last_failed_code")))
    check(empty_screen.get_available_context_actions(1) is not None,
          "E20 空配置界面查可用动作返回空数组（不崩）")
    check(empty_screen.is_context_menu_visible() is False, "E21 空配置界面没有菜单")


# ==================== 主流程 ====================

def main():
    emit("===== 背包交互（自动落位 / 右键菜单 / 拆分 / 删除）无头验证开始 =====")

    check(unreal.InvGridWidget is not None and unreal.InvInventoryScreenWidget is not None,
          "Z0 两个控件类在 python 绑定里可见")
    check(all(x is not None for x in (BP, POCKETS, SAFEBOX, CHESTRIG)),
          "Z0b 容器类型枚举可取（BACKPACK=%s / POCKETS=%s / SAFE_BOX=%s / CHEST_RIG=%s）"
          % (BP, POCKETS, SAFEBOX, CHESTRIG))
    check(all(v is not None for _, _, v in ACTIONS),
          "Z0c 右键菜单动作枚举可取（%s）"
          % ([(name, v is not None) for name, _, v in ACTIONS],))

    section_find_free_cell()
    dump()
    section_auto_place()
    dump()
    section_context_menu()
    dump()
    section_split()
    dump()
    section_delete_key()
    dump()
    section_icon_padding()
    dump()
    section_events_and_sounds()
    emit("===== 结束 =====")


try:
    main()
except Exception:
    FAIL += 1
    emit("[FAIL] 脚本异常:\n" + traceback.format_exc())
finally:
    dump()
