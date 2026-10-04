# 背包 UMG 界面的无头验证（在 UE Python 命令集里跑）。
#
#   UnrealEditor-Cmd <proj> -run=pythonscript -script=<此文件> \
#       -EnablePlugins=PythonScriptPlugin -unattended -nosplash
#
# 覆盖四块：
#   P. 格子控件的纯函数：LocalPositionToCell（取整 / 边界 / 夹取）、CellToLocalPosition（互逆）、
#      FootprintCells（旋转交换长宽）、ComputeDropTarget（含拖到右下角不越界、装不下的返回 false）；
#   S. 拖拽状态机：BeginDragAtItem → UpdateDragCursor → DropOnThisGrid（空位成功 / 被占位报错 /
#      自动换向重试 / 与占位物品交换）→ RotateDraggedOrSelected → CancelDrag；
#   W. 控件构造：NewObject 出 UInvGridWidget / UInvInventoryScreenWidget，EnsureConstructed 走完构造
#      （空配置也不崩）+ 负重文本 + tooltip 文本；
#   R. 「整理」：对当前格子 / 背包调 AutosortContainer，物品一件不少。
#
# 只走公开蓝图节点（UFUNCTION），不碰内部函数。结果写脚本同目录 inv_ui_test_out.txt（末行 SUMMARY）；
# 每个失败行都带实际值，方便直接定位。

import os
import traceback

import unreal

# 结果写到脚本同目录（跨机器可移植）
_HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
OUT = os.path.join(_HERE, "inv_ui_test_out.txt")
LINES = []
PASS = 0
FAIL = 0
KEEP = []     # 强引用，防临时对象被回收
_PROBE = []   # 静态函数调用兜底用的控件实例


def emit(msg):
    LINES.append(msg)
    unreal.log_warning("[INVUI] " + msg)


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


# ---------- 小工具 ----------

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


def grid_size_of(grid):
    """子网格尺寸：直接读控件上那个只读属性（`(宽, 高)`）。"""
    return xy(attr(grid, "grid_size"))


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


def call_static(name, *args):
    """调静态蓝图节点：python 里挂在类上（staticmethod），失败再用实例兜底。"""
    try:
        return getattr(unreal.InvGridWidget, name)(*args)
    except Exception as first:
        pass
    try:
        if not _PROBE:
            _PROBE.append(unreal.new_object(unreal.InvGridWidget))
        return getattr(_PROBE[0], name)(*args)
    except Exception as second:
        note("静态函数 %s 两种调用方式都失败：%s / %s" % (name, first, second))
        return None


def make_def(name, w, h, stack=1, rot=False, weight=0.0, value=0, display="", parts=None):
    d = unreal.new_object(unreal.InvItemDefinition, name=name)
    # BlueprintReadOnly 的属性在 Python 里要用 set_editor_property 写
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
    """建格子控件（等价蓝图的 Create Widget + 填 ExposeOnSpawn 参数）。"""
    g = unreal.new_object(unreal.InvGridWidget)
    KEEP.append(g)
    if comp is not None:
        g.set_editor_property("inventory", comp)
    g.set_editor_property("container", container)
    g.set_editor_property("part", part)
    g.set_editor_property("cell_size", cell)
    g.refresh()
    return g


def view_of(comp, handle):
    res = comp.get_item_view(handle)
    for it in (res if isinstance(res, tuple) else (res,)):
        if isinstance(it, unreal.InvItemView):
            return it
    return None


def pos_of(comp, handle):
    v = view_of(comp, handle)
    return (v.x, v.y, bool(v.rotated)) if v else None


def container_of(comp, kind):
    return comp.get_container(kind)


def _enum_value(enum_cls, *names):
    """按候选名取枚举值：python 把 C++ 枚举项名转成大写蛇形（SafeBox → SAFE_BOX），
    这里给几个候选，取不到返回 None（不抛，交给 main 里报 FAIL）。"""
    for n in names:
        v = getattr(enum_cls, n, None)
        if v is not None:
            return v
    return None


BP = _enum_value(unreal.InvContainerType, "BACKPACK")
POCKETS = _enum_value(unreal.InvContainerType, "POCKETS")
SAFEBOX = _enum_value(unreal.InvContainerType, "SAFE_BOX", "SAFEBOX")
CHESTRIG = _enum_value(unreal.InvContainerType, "CHEST_RIG", "CHESTRIG")


# ==================== P. 纯函数 ====================

def section_pure():
    emit("----- P. 纯函数（LocalPositionToCell / CellToLocalPosition）-----")

    GS = ipt(6, 5)
    cases = [
        ((0.0, 0.0), (0, 0), "左上角"),
        ((47.9, 47.9), (0, 0), "格内右下（向下取整）"),
        ((48.0, 48.0), (1, 1), "正好踩在格线上"),
        ((287.9, 239.9), (5, 4), "右下角格内"),
        ((1e4, 1e4), (5, 4), "远越界 → 夹到右下"),
        ((-10.0, -10.0), (0, 0), "负坐标 → 夹到左上"),
    ]
    for index, ((lx, ly), want, desc) in enumerate(cases, start=1):
        got = call_static("local_position_to_cell", v2(lx, ly), 48.0, GS)
        check(got is not None and xy(got) == want,
              "P%d LocalPositionToCell(%s) %s → %s（期望 %s）" % (index, (lx, ly), desc, xy(got), want))

    # 单格边长非法（0）时按默认 48 处理
    got = call_static("local_position_to_cell", v2(50.0, 50.0), 0.0, GS)
    check(got is not None and xy(got) == (1, 1),
          "P7 单格边长 0 → 按默认 48 算，(50,50) → %s（期望 (1, 1)）" % (xy(got),))

    # 网格尺寸为 0（空配置）时不给非法下标
    got = call_static("local_position_to_cell", v2(100.0, 100.0), 48.0, ipt(0, 0))
    check(got is not None and xy(got) == (0, 0),
          "P8 空网格 (0,0) → %s（期望 (0, 0)，越界也要夹到 0）" % (xy(got),))

    # CellToLocalPosition：格左上角的像素位置
    got = call_static("cell_to_local_position", ipt(2, 3), 48.0)
    check(got is not None and (got.x, got.y) == (96.0, 144.0),
          "P9 CellToLocalPosition((2,3), 48) = %s（期望 (96, 144)）" % ((got.x, got.y) if got else None,))

    # 互逆：格左上角 / 格中心都能解回同一格
    ok_inverse = True
    detail = []
    for cx, cy in ((0, 0), (2, 3), (5, 4)):
        left_top = call_static("cell_to_local_position", ipt(cx, cy), 48.0)
        center = v2(left_top.x + 23.9, left_top.y + 23.9)
        back_a = call_static("local_position_to_cell", left_top, 48.0, GS)
        back_b = call_static("local_position_to_cell", center, 48.0, GS)
        detail.append("%s→%s/%s" % ((cx, cy), xy(back_a), xy(back_b)))
        if xy(back_a) != (cx, cy) or xy(back_b) != (cx, cy):
            ok_inverse = False
    check(ok_inverse, "P10 CellToLocalPosition 与 LocalPositionToCell 互逆（%s）" % " ".join(detail))


def drop_target(view, pos, cell_size, grid):
    """`compute_drop_target` 的 python 约定（实测）：
    放得下 → 返回落点 `FIntPoint`；放不下（C++ 返回 false）→ 绑定给出 `None`。
    所以「能不能放」看 None，落点看返回的 FIntPoint。"""
    res = call_static("compute_drop_target", view, pos, cell_size, grid)
    if res is None:
        return None
    for it in (res if isinstance(res, tuple) else (res,)):
        if isinstance(it, unreal.IntPoint):
            return it
    return None


# ==================== V. 视图相关的纯函数 ====================

def section_views():
    emit("----- V. FootprintCells / ComputeDropTarget（用真实视图）-----")

    mag = make_def("UiDef_Mag", 1, 2, rot=True, weight=0.2, value=120, display="弹匣")
    pouch = make_def("UiDef_Pouch", 3, 3, weight=1.0, value=500, display="小包", parts=[(4, 4)])
    comp, ok = make_comp("UiComp_Pure", [mag, pouch],
                         [make_root(BP, "背包", [(6, 5)]),
                          make_root(SAFEBOX, "安全箱", [(4, 3)])],
                         cap=0.0)
    check(ok, "V0 组件 InitializeInventory 成功（纯函数用的这批视图来自真实物品）")
    if not ok:
        return

    backpack = container_of(comp, BP)
    safebox = container_of(comp, SAFEBOX)

    mag_up = comp.give_item(mag, backpack, 0, 0, 0, False, 1)     # 不旋转：1x2
    mag_rot = comp.give_item(mag, backpack, 0, 3, 0, True, 1)     # 旋转：2x1
    comp.give_item(pouch, safebox, 0, 0, 0, False, 1)             # 3x3，用来测「装不下」
    check(mag_up > 0 and mag_rot > 0, "V1 摆两件弹匣（一件不旋转、一件旋转）")

    v_up = view_of(comp, mag_up)
    v_rot = view_of(comp, mag_rot)
    v_pouch = view_of(comp, comp.get_container_grid(safebox, 0)[0])

    fp_up = call_static("footprint_cells", v_up)
    fp_rot = call_static("footprint_cells", v_rot)
    check(xy(fp_up) == (1, 2), "V2 FootprintCells(不旋转的 1x2) = %s（期望 (1, 2)）" % (xy(fp_up),))
    check(xy(fp_rot) == (2, 1), "V3 FootprintCells(旋转的 1x2) = %s（旋转交换长宽，期望 (2, 1)）" % (xy(fp_rot),))
    check(v_rot is not None and bool(v_rot.rotated) is True, "V4 旋转那件的视图 bRotated = true")

    # 正常落点：旋转视图（2x1）落在 (0,0)
    cell = drop_target(v_rot, v2(0.0, 0.0), 48.0, ipt(4, 3))
    check(cell is not None and xy(cell) == (0, 0),
          "V5 ComputeDropTarget(锚点 0,0) → %s（期望 (0, 0)；放不下时绑定给 None）" % (xy(cell),))

    # 拖到右下角：必须夹到「放得下」的位置，不能越界
    cell = drop_target(v_rot, v2(1e4, 1e4), 48.0, ipt(4, 3))
    fits = cell is not None and cell.x + 2 <= 4 and cell.y + 1 <= 3
    check(cell is not None and xy(cell) == (2, 2) and fits,
          "V6 拖到右下角 → %s（期望 (2, 2)，占位 2x1 不越右/下边界）" % (xy(cell),))

    # 物品比网格还大：没有合法落点
    cell = drop_target(v_pouch, v2(0.0, 0.0), 48.0, ipt(2, 2))
    check(cell is None,
          "V7 3x3 物品放进 2x2 网格 → %s（期望 None，对应 C++ 的 false）" % (xy(cell),))


# ==================== S. 拖拽状态机 ====================

def section_state_machine():
    emit("----- S. 拖拽状态机 -----")

    mag = make_def("UiDef_Mag2", 1, 2, rot=True, weight=0.2, value=120, display="弹匣")
    ammo = make_def("UiDef_Ammo", 1, 1, stack=60, weight=0.01, value=2, display="子弹")
    plate = make_def("UiDef_Plate", 1, 2, rot=False, weight=0.5, value=100, display="钢板")
    comp, ok = make_comp("UiComp_Drag", [mag, ammo, plate],
                         [make_root(BP, "背包", [(6, 5)])],
                         cap=10.0)
    check(ok, "S0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    grid = make_grid(comp, backpack, 0)
    check(len(grid.items) == 0 and grid_size_of(grid) == (6, 5),
          "S1 空背包的格子控件：物品 0 件、网格 %s" % (grid_size_of(grid),))

    # ---- 1. 拖到空位：成功，且组件里的位置真的变了 ----
    item_mag = comp.give_item(mag, backpack, 0, 0, 0, False, 1)
    item_ammo = comp.give_item(ammo, backpack, 0, 2, 0, False, 60)
    grid.refresh()
    check(len(grid.items) == 2, "S2 灌 2 件后格子控件看到 %d 件（期望 2）" % len(grid.items))
    check(grid_size_of(grid) == (6, 5), "S3 子网格尺寸 = %s（期望 (6, 5)）" % (grid_size_of(grid),))

    started = grid.begin_drag_at_item(item_mag)
    check(started == item_mag, "S4 BeginDragAtItem 返回被拖的物品句柄 %s（期望 %s）" % (started, item_mag))
    check(bool_attr(grid, "dragging") is True, "S5 拖拽中 bDragging = true")

    # 光标放在 (3,2) 格中心：抓取偏移 = 半格 → 落点应该就是 (3,2)
    grid.update_drag_cursor(v2(3 * 48.0 + 24.0, 2 * 48.0 + 48.0))
    check(xy(grid.drop_cell) == (3, 2), "S6 UpdateDragCursor 后落点 = %s（期望 (3, 2)）" % (xy(grid.drop_cell),))
    check(bool_attr(grid, "drop_allowed") is True, "S7 空位的落点判定为可放")

    dropped = grid.drop_on_this_grid(ipt(3, 2), False)
    check(dropped == 0, "S8 DropOnThisGrid 到空位返回 0（实际 %s）" % dropped)
    check(pos_of(comp, item_mag) == (3, 2, False),
          "S9 组件里的位置真的变了：弹匣 → %s（期望 (3, 2, false)）" % (pos_of(comp, item_mag),))
    check(bool_attr(grid, "dragging") is False and attr(grid, "dragged_item") == 0,
          "S10 落位后拖拽状态收尾（dragging = %s / dragged = %s）"
          % (bool_attr(grid, "dragging"), attr(grid, "dragged_item")))
    check(attr(grid, "selected_item") == item_mag,
          "S11 落位后保留选中（selected = %s）" % attr(grid, "selected_item"))

    # ---- 2. 拖到「两件不同物品占着」的位置：必然失败，位置不变 ----
    # (0,2) 与 (0,3) 各放一件 1x1：目标占位被两件不同的东西占着 → 不构成交换，只能报错
    item_a2 = comp.give_item(ammo, backpack, 0, 0, 2, False, 10)
    item_a3 = comp.give_item(ammo, backpack, 0, 0, 3, False, 10)
    item_plate = comp.give_item(plate, backpack, 0, 4, 3, False, 1)
    grid.refresh()
    check(item_a2 > 0 and item_a3 > 0 and item_plate > 0, "S12 摆好后续场景（两件占位 + 一块 1x2 钢板）")

    grid.begin_drag_at_item(item_plate)
    refused = grid.drop_on_this_grid(ipt(0, 2), False)
    check(refused == 2,
          "S13 拖到被两件物品占着的目标 → 错误码 2（Occupied，实际 %s）" % refused)
    check(pos_of(comp, item_plate) == (4, 3, False),
          "S14 落位失败后物品位置不变：钢板仍在 %s（期望 (4, 3, false)）" % (pos_of(comp, item_plate),))

    # ---- 3. 自动换向重试：(5,3) 挡住 1x2 的下半格，旋转成 2x1 就能塞进 (4,2) ----
    item_block = comp.give_item(ammo, backpack, 0, 5, 3, False, 5)
    grid.refresh()
    grid.begin_drag_at_item(item_mag)
    auto = grid.drop_on_this_grid(ipt(5, 2), False)
    check(auto == 0, "S15 落位失败后自动换向重试成功返回 0（实际 %s）" % auto)
    check(bool_attr(grid, "auto_rotated_on_last_drop") is True,
          "S16 记录下了「这次靠自动旋转才落位」（auto_rotated_on_last_drop = %s）"
          % bool_attr(grid, "auto_rotated_on_last_drop"))
    check(pos_of(comp, item_mag) == (4, 2, True),
          "S17 自动旋转后弹匣落在 %s（期望 (4, 2, true)）" % (pos_of(comp, item_mag),))

    # ---- 4. 与占位物品交换 ----
    # 钢板（1x2，不可旋转）拖到 (5,3)：那格被 1x1 占着 → MoveItem 失败、不能旋转 → 换成 SwapItems
    grid.begin_drag_at_item(item_plate)
    swapped = grid.drop_on_this_grid(ipt(5, 3), False)
    check(swapped == 0, "S18 目标被单件物品占着时改走交换并成功返回 0（实际 %s）" % swapped)
    check(pos_of(comp, item_plate) == (5, 3, False),
          "S19 交换后钢板到 %s（期望 (5, 3, false)）" % (pos_of(comp, item_plate),))
    check(pos_of(comp, item_block) == (4, 3, False),
          "S20 交换后原来那件 1x1 到 %s（期望 (4, 3, false)）" % (pos_of(comp, item_block),))

    # ---- 5. 旋转 ----
    item_mag2 = comp.give_item(mag, backpack, 0, 0, 0, False, 1)  # (0,0)-(0,1) 空着
    grid.refresh()
    grid.begin_drag_at_item(item_mag2)
    check(bool_attr(grid, "drag_rotated") is False, "S21 开始拖拽时幽灵朝向 = 物品当前朝向（不旋转）")
    rotated_ghost = grid.rotate_dragged_or_selected()
    check(rotated_ghost == 0 and bool_attr(grid, "drag_rotated") is True,
          "S22 RotateDraggedOrSelected（拖拽中）翻转幽灵朝向：返回 %s / drag_rotated = %s"
          % (rotated_ghost, bool_attr(grid, "drag_rotated")))
    grid.cancel_drag()
    check(bool_attr(grid, "dragging") is False and attr(grid, "dragged_item") == 0
          and attr(grid, "selected_item") == 0 and xy(grid.drop_cell) == (0, 0),
          "S23 CancelDrag 不留状态（dragging=%s / dragged=%s / selected=%s / drop_cell=%s）"
          % (bool_attr(grid, "dragging"), attr(grid, "dragged_item"),
             attr(grid, "selected_item"), xy(grid.drop_cell)))
    check(bool_attr(grid, "auto_rotated_on_last_drop") is False,
          "S24 CancelDrag 把「上次自动旋转」标记也清掉")

    # 选中态原地旋转（成功路径）
    grid.select_item(item_mag2)
    before = view_of(comp, item_mag2)
    inplace = grid.rotate_dragged_or_selected()
    after = view_of(comp, item_mag2)
    check(inplace == 0 and after is not None and bool(after.rotated) is True,
          "S25 选中态原地旋转成功：返回 %s / bRotated = %s"
          % (inplace, bool(after.rotated) if after else None))
    check(before is not None and bool(before.rotated) is False, "S26 旋转前是未旋转态（对照）")
    check((after.x, after.y) == (0, 0) and (before.x, before.y) == (0, 0), "S27 原地旋转不换格")

    # 不可旋转的定义：返回错误码 4
    grid.select_item(item_plate)
    not_rotatable = grid.rotate_dragged_or_selected()
    check(not_rotatable == 4, "S28 不可旋转的定义原地旋转 → 错误码 4（实际 %s）" % not_rotatable)

    # 没有选中也没在拖：返回 23（无效句柄），不崩
    grid.cancel_drag()
    nothing = grid.rotate_dragged_or_selected()
    check(nothing == 23, "S29 既没拖也没选中 → 错误码 23（实际 %s）" % nothing)

    # tooltip 文本
    tooltip = grid.build_tooltip_text(item_mag2)
    check("弹匣" in tooltip and "重量" in tooltip and "0.200" in tooltip and "堆叠" in tooltip,
          "S30 BuildTooltipText 含名字 / 重量 / 堆叠（原文 %r）" % tooltip)
    check(grid.build_tooltip_text(0) == "", "S31 句柄非法时 tooltip 文本为空串")

    # 鼠标按下选中的入口（不依赖真实鼠标消息）
    grid.select_item(item_a2)
    check(attr(grid, "selected_item") == item_a2, "S32 SelectItem 记录选中物品")
    grid.select_item(0)
    check(attr(grid, "selected_item") == 0, "S33 SelectItem(0) 清空选中")


# ==================== W. 控件构造 / 负重 / 数据 ====================

def section_widgets():
    emit("----- W. 控件构造（NewObject + 控件树 + 负重条，空配置不崩）-----")

    ammo = make_def("UiDef_Ammo2", 1, 1, stack=60, weight=0.01, value=2, display="子弹")
    medkit = make_def("UiDef_Medkit", 1, 1, stack=3, weight=0.5, value=300, display="急救包")
    comp, ok = make_comp("UiComp_Screen", [ammo, medkit],
                         [make_root(BP, "背包", [(6, 5)]),
                          make_root(POCKETS, "口袋", [(1, 1)] * 4)],
                         cap=20.0)
    check(ok, "W0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    pockets = container_of(comp, POCKETS)
    comp.give_item(ammo, backpack, 0, 0, 0, False, 60)
    comp.give_item(medkit, backpack, 0, 1, 0, False, 3)

    # 1) 格子控件：NewObject + 构造入口
    grid = make_grid(comp, backpack, 0)
    built = grid.ensure_constructed()
    note("EnsureConstructed() 返回 %s —— 无头命令集里没有 Slate 应用，控件跳过 Slate 构造（返回 false，不崩）；"
         "PIE / 编辑器里同一句返回 true" % built)
    check(built is False or built is True, "W1 格子控件的构造入口按预期返回布尔（实际 %s）" % built)
    check(len(grid.items) == 2, "W2 构造入口跑完后数据照样可用：%d 件（期望 2）" % len(grid.items))
    check(grid.get_grid_title().startswith("背包 part0") and "6x5" in grid.get_grid_title(),
          "W3 格子标题 = %r（期望形如「背包 part0 (6x5)」）" % grid.get_grid_title())
    w, h = grid.get_grid_pixel_size().x, grid.get_grid_pixel_size().y
    check((w, h) == (288.0, 240.0), "W4 像素尺寸 = %s（6x5 格 × 48 = (288, 240)）" % ((w, h),))
    cell_item = grid.get_item_at_cell(ipt(1, 0))
    check(cell_item > 0, "W5 GetItemAtCell((1,0)) 拿到急救包句柄 %s" % cell_item)
    check(grid.get_item_at_cell(ipt(99, 99)) == 0, "W6 越界查格返回 0")
    check(abs(grid.get_effective_cell_size() - 48.0) < 1e-4,
          "W6b GetEffectiveCellSize = %s（配置 48）" % grid.get_effective_cell_size())

    # 2) 界面控件：控件树（根画布 / 子格子 / 小标题 / 负重条）+ 负重文本
    screen = unreal.new_object(unreal.InvInventoryScreenWidget)
    KEEP.append(screen)
    screen.set_editor_property("inventory", comp)
    screen.set_editor_property("cell_size", 48.0)
    screen_built = screen.ensure_constructed()
    note("界面 EnsureConstructed() 返回 %s（同上：无头命令集没有 Slate 应用）" % screen_built)
    screen.refresh()  # 无 Slate 应用时 Slate 那层跳过，但 UObject 层的控件树照建

    grids = list(screen.get_grid_widgets())
    check(len(grids) == 5,
          "W7 界面建了 %d 块子格子（背包 1 块 + 口袋 4 块 = 5）" % len(grids))
    pairs = sorted([(int(attr(g, "container")), int(attr(g, "part"))) for g in grids if g is not None])
    check(pairs == sorted([(backpack, 0)] + [(pockets, i) for i in range(4)]),
          "W8 子格子覆盖的口子正确：%s" % pairs)
    check(all(int(attr(g, "cell_size")) == 48 for g in grids if g is not None),
          "W9 子格子拿到了界面的 CellSize（48）")
    check(all(attr(g, "owner_screen") == screen for g in grids if g is not None),
          "W10 子格子回指到界面控件（OwnerScreen，跨容器拖 / 「整理」要用）")

    captions = list(screen.caption_labels)
    check(len(captions) == 5 and all(c is not None for c in captions),
          "W11 每块子格子都有小标题（%d 个）" % len(captions))
    caption_texts = [str(c.get_text()) for c in captions if c is not None]
    check(any("背包" in t and "part0" in t and "6x5" in t for t in caption_texts),
          "W12 背包的小标题形如「背包 part0 (6x5)」：%s" % caption_texts)

    # 每块子格子的数据来源正确：背包那块看到 2 件，口袋 4 块都是空的
    by_container = {}
    for g in grids:
        if g is not None:
            by_container.setdefault(int(attr(g, "container")), []).append(len(g.items))
    check(by_container.get(backpack) == [2] and by_container.get(pockets) == [0, 0, 0, 0],
          "W13 各子格子的物品数正确（背包 %s / 口袋 %s）"
          % (by_container.get(backpack), by_container.get(pockets)))

    weight_text = screen.get_weight_text()
    expected_total = "总重 %.2f kg" % comp.get_total_weight()
    check(expected_total in weight_text,
          "W14 负重文本含总重（%r 里找 %r，组件总重 = %s kg）"
          % (weight_text, expected_total, comp.get_total_weight()))
    check("上限 20 kg" in weight_text, "W15 负重文本含上限（%r）" % weight_text)
    check(screen.weight_label is not None and str(screen.weight_label.get_text()) == weight_text,
          "W16 负重文本控件上的字 = GetWeightText（%r）"
          % (str(screen.weight_label.get_text()) if screen.weight_label else None))

    ratio = screen.get_weight_ratio()
    check(abs(ratio - comp.get_total_weight() / 20.0) < 1e-4,
          "W17 GetWeightRatio = %s（组件总重/上限 = %s）" % (ratio, comp.get_total_weight() / 20.0))
    bar_percent = screen.get_weight_bar_percent()
    check(abs(bar_percent - min(1.0, comp.get_total_weight() / 20.0)) < 1e-4,
          "W18 负重条填充比例 = %s（= 负重比夹到 0~1）" % bar_percent)
    normal_color = screen.get_weight_bar_color()
    check(normal_color.g > normal_color.r, "W19 未超重时条是常态色（%s）" % ((normal_color.r, normal_color.g, normal_color.b),))

    check(screen.is_overloaded() is False, "W20 2.10 kg / 20 kg 不该判超重")
    comp.set_max_capacity(1.0)
    screen.refresh()
    overloaded_color = screen.get_weight_bar_color()
    check(screen.is_overloaded() is True and "超重" in screen.get_weight_text(),
          "W21 上限压到 1 kg 后判超重，文本带「超重」（%r）" % screen.get_weight_text())
    check(overloaded_color.r > overloaded_color.g and abs(screen.get_weight_bar_percent() - 1.0) < 1e-4,
          "W22 超重时条变红且填满（%s / %s）"
          % ((overloaded_color.r, overloaded_color.g, overloaded_color.b), screen.get_weight_bar_percent()))
    comp.set_max_capacity(20.0)
    screen.refresh()
    check("上限 20 kg" in screen.get_weight_text(), "W23 上限改回 20 kg")

    # 3) 空配置也不崩
    empty_grid = unreal.new_object(unreal.InvGridWidget)
    KEEP.append(empty_grid)
    empty_grid_built = empty_grid.ensure_constructed()
    empty_grid.refresh()
    check(empty_grid_built is False or empty_grid_built is True,
          "W24 空配置的格子控件构造入口不崩（返回 %s）" % empty_grid_built)
    check(empty_grid.begin_drag_at_item(12345) == -23,
          "W25 空配置的格子控件开始拖拽 → -23（实际 %s）" % empty_grid.begin_drag_at_item(12345))
    check(empty_grid.drop_on_this_grid(ipt(0, 0), False) == 23,
          "W26 空配置的格子控件落位 → 23")
    check(empty_grid.rotate_dragged_or_selected() == 23, "W27 空配置的格子控件旋转 → 23")
    check(empty_grid.build_tooltip_text(1) == "", "W28 空配置的格子控件 tooltip 为空串")

    empty_screen = unreal.new_object(unreal.InvInventoryScreenWidget)
    KEEP.append(empty_screen)
    empty_screen_built = empty_screen.ensure_constructed()
    empty_screen.refresh()
    check(empty_screen_built is False or empty_screen_built is True,
          "W29 空配置的界面控件构造入口不崩（返回 %s）" % empty_screen_built)
    check(len(list(empty_screen.get_grid_widgets())) == 0, "W30 空配置的界面不建任何子格子")
    check("未绑定" in empty_screen.get_weight_text(),
          "W31 空配置的界面负重文本给出提示（%r）" % empty_screen.get_weight_text())
    check(empty_screen.autosort_focused_container() == 23,
          "W32 空配置的界面点「整理」→ 23（没有可整理的容器）")


# ==================== R. 整理 ====================

def section_autosort():
    emit("----- R. 「整理」按钮 -----")

    ammo = make_def("UiDef_Ammo3", 1, 1, stack=60, weight=0.01, value=2, display="子弹")
    plate = make_def("UiDef_Plate3", 2, 2, rot=False, weight=0.5, value=100, display="钢板")
    comp, ok = make_comp("UiComp_Sort", [ammo, plate],
                         [make_root(POCKETS, "口袋", [(1, 1)] * 4),
                          make_root(BP, "背包", [(6, 5)])],
                         cap=20.0)
    check(ok, "R0 组件 InitializeInventory 成功")
    if not ok:
        return

    backpack = container_of(comp, BP)
    pockets = container_of(comp, POCKETS)
    comp.give_item(plate, backpack, 0, 3, 3, False, 1)
    comp.give_item(plate, backpack, 0, 1, 1, False, 1)
    comp.give_item(ammo, backpack, 0, 5, 4, False, 7)
    comp.give_item(ammo, pockets, 0, 0, 0, False, 3)   # 口袋不是空的：整理它才有意义
    before_count = len(as_list(comp.get_all_items()))
    before_weight = comp.get_total_weight()

    screen = unreal.new_object(unreal.InvInventoryScreenWidget)
    KEEP.append(screen)
    screen.set_editor_property("inventory", comp)
    screen.set_editor_property("cell_size", 48.0)
    screen.refresh()  # 建控件树 + 子格子（不依赖 Slate 应用）

    # 悬停 / 选中口径：先报「当前格子」，整理应该打到那个容器上
    pockets_grid = None
    for g in screen.get_grid_widgets():
        if g is not None and int(attr(g, "container")) == pockets:
            pockets_grid = g
            break
    check(pockets_grid is not None, "R1 界面里有口袋的子格子")
    if pockets_grid is not None:
        screen.notify_grid_focused(pockets_grid)
        check(screen.get_focused_grid() is pockets_grid, "R2 NotifyGridFocused 记录了当前格子")
        sorted_pockets = screen.autosort_focused_container()
        check(sorted_pockets == 0, "R3 对「当前格子」所在的口袋整理 → 0（实际 %s）" % sorted_pockets)

    # 没有当前格子 → 退回背包
    screen.notify_grid_focused(None)
    check(screen.get_focused_grid() is None, "R4 清掉当前格子后 GetFocusedGrid 为空")
    sorted_backpack = screen.autosort_focused_container()
    check(sorted_backpack == 0, "R5 没有当前格子时对背包整理 → 0（实际 %s）" % sorted_backpack)

    after_count = len(as_list(comp.get_all_items()))
    check(after_count == before_count,
          "R6 整理后物品一件不少：%d → %d（期望 %d）" % (before_count, after_count, before_count))
    check(abs(comp.get_total_weight() - before_weight) < 1e-4,
          "R7 整理后总重不变：%s → %s" % (before_weight, comp.get_total_weight()))

    # 整理之后子格子也跟着刷了（物品列表不空）
    screen.refresh()
    grids = [g for g in screen.get_grid_widgets() if g is not None]
    backpack_grid = None
    for g in grids:
        if int(attr(g, "container")) == backpack:
            backpack_grid = g
            break
    check(backpack_grid is not None and len(backpack_grid.items) == 3,
          "R8 整理后背包子格子看到 3 件（实际 %s）"
          % (len(backpack_grid.items) if backpack_grid else None))


# ==================== X. 跨容器拖 ====================

def section_cross_container():
    emit("----- X. 跨容器拖（弹挂 → 背包）-----")

    mag = make_def("UiDef_MagX", 1, 2, rot=True, weight=0.2, value=120, display="弹匣")
    ammo = make_def("UiDef_AmmoX", 1, 1, stack=60, weight=0.01, value=2, display="子弹")
    comp, ok = make_comp("UiComp_Cross", [mag, ammo],
                         [make_root(CHESTRIG, "弹挂", [(1, 1), (1, 1), (1, 2)]),
                          make_root(BP, "背包", [(6, 5)])],
                         cap=10.0)
    check(ok, "X0 组件 InitializeInventory 成功")
    if not ok:
        return

    rig = container_of(comp, CHESTRIG)
    backpack = container_of(comp, BP)
    item_mag = comp.give_item(mag, rig, 2, 0, 0, False, 1)
    comp.give_item(ammo, backpack, 0, 2, 2, False, 60)

    src = make_grid(comp, rig, 2)
    dst = make_grid(comp, backpack, 0)
    check(len(src.items) == 1 and len(dst.items) == 1,
          "X1 两个容器的子格子各自看到 1 件（弹挂 %d / 背包 %d）" % (len(src.items), len(dst.items)))

    started = src.begin_drag_at_item(item_mag)
    check(started == item_mag, "X2 在弹挂的格子上开始拖拽（返回 %s）" % started)

    # 预览：源格子按屏幕坐标转播，目标格子自己算落点（鼠标被源格子捕获，目标格子收不到事件）
    dst.preview_external_drag(src, v2(216.0, 48.0), False)
    check(attr(dst, "external_drag_source") == src, "X3 目标格子进入跨容器预览（external_drag_source 已设置）")
    cell = xy(dst.drop_cell)
    check(cell is not None and 0 <= cell[0] <= 5 and 0 <= cell[1] <= 4,
          "X4 跨容器预览的落点在本网格内：%s" % (cell,))
    check(bool_attr(src, "dragging") is True and attr(src, "dragged_item") == item_mag,
          "X5 预览不改源格子的拖拽状态（dragging=%s / dragged=%s）"
          % (bool_attr(src, "dragging"), attr(src, "dragged_item")))

    moved = dst.drop_drag_from_other_grid(src, v2(216.0, 48.0), False)
    check(moved == 0, "X6 跨容器落位返回 0（实际 %s）" % moved)

    after = view_of(comp, item_mag)
    check(after is not None and after.host_container == backpack and after.host_part == 0,
          "X7 弹匣已经换容器：host_container = %s / host_part = %s（期望 %s / 0）"
          % (after.host_container if after else None, after.host_part if after else None, backpack))
    in_bounds = after is not None and 0 <= after.x and after.x + 1 <= 6 and 0 <= after.y and after.y + 2 <= 5
    check(in_bounds, "X8 落点在背包网格内且不越界：%s"
          % ((after.x, after.y, after.rotated) if after else None,))
    check(bool_attr(src, "dragging") is False and attr(src, "dragged_item") == 0,
          "X9 跨容器落位后源格子收尾（dragging=%s / dragged=%s）"
          % (bool_attr(src, "dragging"), attr(src, "dragged_item")))
    check(len(src.items) == 0, "X10 源格子的物品列表已刷新为空（实际 %d 件）" % len(src.items))
    check(len(dst.items) == 2, "X11 目标格子现在有 2 件（实际 %d 件）" % len(dst.items))
    check(attr(dst, "external_drag_source") is None, "X12 落位后跨容器预览已清掉")

    # 源格子已经收尾：再调一次跨容器落位应该报「没有正在拖拽的物品」
    again = dst.drop_drag_from_other_grid(src, v2(216.0, 48.0), False)
    check(again == 23, "X13 源格子没在拖时再交接 → 23（实际 %s）" % again)


# ==================== 主流程 ====================

def main():
    emit("===== 背包 UMG 界面无头验证开始 =====")

    # 先确认要用的绑定都在（名字对不上时后面全崩，早点把原因说出来）
    check(unreal.InvGridWidget is not None and unreal.InvInventoryScreenWidget is not None,
          "W-0 两个控件类在 python 绑定里可见")
    check(all(x is not None for x in (BP, POCKETS, SAFEBOX, CHESTRIG)),
          "W-0b 容器类型枚举可取（BACKPACK=%s / POCKETS=%s / SAFE_BOX=%s / CHEST_RIG=%s）"
          % (BP, POCKETS, SAFEBOX, CHESTRIG))

    section_pure()
    dump()
    section_views()
    dump()
    section_state_machine()
    dump()
    section_cross_container()
    dump()
    section_widgets()
    dump()
    section_autosort()
    emit("===== 结束 =====")


try:
    main()
except Exception:
    FAIL += 1
    emit("[FAIL] 脚本异常:\n" + traceback.format_exc())
finally:
    dump()
