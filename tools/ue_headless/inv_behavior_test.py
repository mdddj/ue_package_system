# 背包组件层行为验证（在 UE Python 命令集里跑）。
# 只走新层的公开蓝图入口（UInvInventoryComponent 的 API），不碰内部函数。
# 结果同时写日志（warning 级，命令集里 Display 会被吞）和脚本同目录的 inv_behavior_test_out.txt。

import os
import traceback

import unreal

# 结果写到脚本同目录（跨机器可移植）
_HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
OUT = os.path.join(_HERE, "inv_behavior_test_out.txt")
LINES = []
PASS = 0
FAIL = 0
KEEP = []  # 强引用，防临时对象被回收


def emit(msg):
    LINES.append(msg)
    unreal.log_warning("[INVTEST] " + msg)


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


def items_of(res):
    return list(res) if isinstance(res, tuple) else [res]


def find_type(res, t):
    for it in items_of(res):
        if isinstance(it, t):
            return it
    return None


def ipt(x, y):
    # FIntPoint 在蓝图里当 (宽, 高) 用
    try:
        return unreal.IntPoint(x, y)
    except Exception:
        p = unreal.IntPoint()
        p.set_editor_property("x", x)
        p.set_editor_property("y", y)
        return p


def make_def(name, w, h, stack=1, rot=False, weight=0.0, value=0, parts=None, display=""):
    d = unreal.new_object(unreal.InvItemDefinition, name=name)
    # BlueprintReadOnly 的属性在 Python 里要用 set_editor_property 写
    d.set_editor_property("display_name", display)
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
    return c


def arr(res):
    """TArray 返回值：转到 python list。"""
    return list(res) if res is not None else []


def part_xy(p):
    return (p.x, p.y)


def main():
    emit("===== 背包组件层行为验证开始 =====")

    mag = make_def("TestDef_Mag", 1, 2, stack=1, rot=True, weight=0.25, value=120, display="弹匣")
    ammo = make_def("TestDef_Ammo", 1, 1, stack=60, rot=False, weight=0.01, value=2, display="子弹")
    pouch = make_def("TestDef_Pouch", 3, 3, stack=1, rot=False, weight=1.0, value=500,
                     parts=[(4, 4)], display="小包")

    comp = make_comp("TestComp_A", [mag, ammo, pouch],
                     [make_root(unreal.InvContainerType.BACKPACK, "背包", [(6, 5)]),
                      make_root(unreal.InvContainerType.POCKETS, "口袋", [(1, 1)] * 4)],
                     cap=2.0)

    # ---------- A. 初始化与 DefId ----------
    initialized = comp.initialize_inventory()
    check(initialized is True, "A1 InitializeInventory 返回 true（实际 %s）" % initialized)
    check(comp.ready is True, "A2 bReady 为真")
    check(comp.inventory is not None and comp.inventory.is_valid_handle() is True, "A3 Inventory 句柄有效")

    note("组件上的配置数组长度 = %s" % len(arr(comp.item_definitions)))
    note("reg(mag) = %s" % comp.register_definition(mag))
    note("id(mag) = %s / id(ammo) = %s / id(pouch) = %s"
         % (comp.get_def_id(mag), comp.get_def_id(ammo), comp.get_def_id(pouch)))
    note("definition(0) = %s definition(1) = %s" % (comp.get_definition(0), comp.get_definition(1)))

    order = sorted([mag, ammo, pouch], key=lambda d: d.get_name())
    ids = [comp.get_def_id(d) for d in order]
    note("按名字排序 = %s" % [(d.get_name(), i) for d, i in zip(order, ids)])
    check(ids == [0, 1, 2], "A4 注册顺序按资产名升序，与数组顺序无关（得到 %s）" % ids)
    check(comp.get_definition(0) == order[0], "A5 GetDefinition(0) 返回排第一的定义")

    comp2 = make_comp("TestComp_B", [pouch, mag, ammo],
                      [make_root(unreal.InvContainerType.BACKPACK, "背包", [(6, 5)])], cap=0.0)
    check(comp2.initialize_inventory() is True, "A6 第二个组件初始化成功")
    ids2 = [comp2.get_def_id(d) for d in order]
    check(ids2 == ids, "A7 数组顺序反过来，DefId 仍然一致（%s）" % ids2)
    check(comp.initialize_inventory() is True, "A8 重复初始化幂等")
    check(comp.register_definition(mag) == comp.get_def_id(mag), "A9 重复注册返回同一个 DefId")
    check(comp.get_def_id(None) == -22, "A10 GetDefId(nil) 返回 -22")
    check(comp.get_definition(-1) is None and comp.get_definition(999) is None, "A11 越界 DefId 返回空指针")

    # ---------- B. 容器 ----------
    backpack = comp.get_container(unreal.InvContainerType.BACKPACK)
    pockets = comp.get_container(unreal.InvContainerType.POCKETS)
    check(backpack > 0, "B1 拿到背包根容器句柄 %s" % backpack)
    check(pockets > 0, "B2 拿到口袋根容器句柄 %s" % pockets)
    check(comp.get_container(unreal.InvContainerType.SAFE_BOX) == 0, "B3 没配安全箱 => 返回 0（无）")
    check(comp.get_container_label(backpack) == "背包", "B4 容器显示名 = 背包（实际 '%s'）"
          % comp.get_container_label(backpack))
    parts = arr(comp.get_container_parts(backpack))
    check(len(parts) == 1 and part_xy(parts[0]) == (6, 5),
          "B5 背包子网格 = (6,5)（实际 %s）" % ([part_xy(p) for p in parts],))
    pocket_parts = arr(comp.get_container_parts(pockets))
    check(len(pocket_parts) == 4 and all(part_xy(p) == (1, 1) for p in pocket_parts),
          "B6 口袋 4 块 1x1（实际 %s）" % [part_xy(p) for p in pocket_parts])

    cinfo = find_type(comp.get_container_info(backpack), unreal.InvContainerInfo)
    check(cinfo is not None and cinfo.type == unreal.InvContainerType.BACKPACK and cinfo.root is True,
          "B7 容器汇总：类型/根容器标记正确")
    check(cinfo is not None and cinfo.part_count == 1 and cinfo.total_cells == 30 and cinfo.used_cells == 0,
          "B8 容器汇总：1 块子网格 / 30 格 / 已占 0 格")

    # ---------- C. 物品、视图、操作、负重 ----------
    mag_item = comp.give_item(mag, backpack, 0, 0, 0, True, 1)
    check(mag_item > 0, "C1 GiveItem 落一件旋转的 1x2 弹匣，句柄 %s" % mag_item)

    grid = arr(comp.get_container_grid(backpack, 0))
    check(len(grid) == 30, "C2 占格表长度 = 宽*高 = 30（实际 %s）" % len(grid))
    check(len(grid) == 30 and grid[0] == mag_item and grid[1] == mag_item and grid[2] == 0 and grid[6] == 0,
          "C3 旋转后占 (0,0)(1,0)，第二行第一个格子空")

    v = find_type(comp.get_item_view(mag_item), unreal.InvItemView)
    check(v is not None and v.handle == mag_item and v.definition == mag, "C4 GetItemView 的句柄与定义正确")
    c5 = (v.x, v.y, v.rotated, v.stack) if v else None
    check(c5 == (0, 0, True, 1), "C5 视图位置/朝向/堆叠正确（实际 %s）" % (c5,))
    check(v is not None and v.host_container == backpack and v.host_part == 0, "C6 视图的所在容器/子网格正确")
    check(mag.has_container() is False and pouch.has_container() is True, "C7 HasContainer 区分普通物品与套包")
    check(part_xy(mag.footprint(True)) == (2, 1) and part_xy(mag.footprint(False)) == (1, 2),
          "C8 Footprint 旋转交换长宽")

    allv = arr(comp.get_container_items_view(backpack))
    check(len(allv) == 1 and allv[0].handle == mag_item, "C9 容器内视图列表 = 1 件（实际 %s）" % len(allv))
    check(allv and allv[0].definition == mag, "C9b 视图里带着定义对象")

    check(abs(comp.get_total_weight() - 0.25) < 1e-4, "C10 总重 = 0.25kg（实际 %s）" % comp.get_total_weight())
    check(comp.get_total_value() == 120, "C11 总估值 = 120（实际 %s）" % comp.get_total_value())
    check(abs(comp.get_weight_ratio() - 0.125) < 1e-4, "C12 负重比 = 0.125（实际 %s）" % comp.get_weight_ratio())
    check(comp.is_overloaded() is False, "C13 2kg 上限下不超重")

    comp.set_max_capacity(0.05)
    check(abs(comp.max_capacity_kg - 0.05) < 1e-6,
          "C14 SetMaxCapacity 同步到 MaxCapacityKg（实际 %s）" % comp.max_capacity_kg)
    check(comp.is_overloaded() is True, "C15 0.05kg 上限下超重")
    check(abs(comp.get_weight_ratio() - 5.0) < 1e-3, "C16 负重比 = 5.0（实际 %s）" % comp.get_weight_ratio())
    comp.set_max_capacity(5.0)

    check(comp.move_item(mag_item, backpack, 0, 3, 2, False) == 0, "C17 移动到 (3,2) 成功")
    moved = find_type(comp.get_item_view(mag_item), unreal.InvItemView)
    c18 = (moved.x, moved.y, moved.rotated) if moved else None
    check(c18 == (3, 2, False), "C18 移动后位置/朝向正确（实际 %s）" % (c18,))
    check(comp.rotate_item(mag_item) == 0, "C19 原地旋转成功")
    rotated = find_type(comp.get_item_view(mag_item), unreal.InvItemView)
    check(rotated is not None and rotated.rotated is True, "C20 旋转后 bRotated = true")

    bad_move = comp.move_item(mag_item, backpack, 0, 5, 4, True)
    note("越界移动返回错误码 = %s" % bad_move)
    check(bad_move > 0, "C21 越界移动返回错误码（不是 0）")
    still = find_type(comp.get_item_view(mag_item), unreal.InvItemView)
    c22 = (still.x, still.y) if still else None
    check(c22 == (3, 2), "C22 失败操作零副作用（实际 %s）" % (c22,))

    occupied = comp.give_item(mag, backpack, 0, 3, 2, False, 1)
    note("占位冲突的 GiveItem 返回 = %s" % occupied)
    check(occupied == -2, "C23 生成到已占格返回错误码 -2（Occupied）")

    found = arr(comp.find_items_by_definition(mag))
    check(len(found) == 1 and found[0].handle == mag_item, "C25 FindItemsByDefinition 找到 1 件（实际 %s）" % len(found))
    check(len(arr(comp.find_items_by_definition(ammo))) == 0, "C26 没实例的定义返回空列表")

    # ---------- D. 套包（嵌套 + 顶层过滤） ----------
    pouch_item = comp.give_item(pouch, backpack, 0, 0, 0, False, 1)
    check(pouch_item > 0, "D1 落一件 3x3 套包，句柄 %s" % pouch_item)
    check(len(arr(comp.get_container_items_view(backpack))) == 2, "D2 背包顶层 = 2 件")

    # 套包内部的容器句柄：GetAllContainers 里 root == False 的那个（根容器在前，子树随后）
    inner = 0
    root_count = 0
    for c in arr(comp.get_all_containers()):
        ci = find_type(comp.get_container_info(c), unreal.InvContainerInfo)
        if ci is None:
            continue
        if ci.root:
            root_count += 1
        else:
            inner = c
    check(inner > 0, "D3 GetContainerInfo 报出套包内部容器（非根容器）句柄 %s" % inner)
    check(root_count == 2, "D3b 根容器 2 个（背包 + 口袋）实际 %s" % root_count)

    check(comp.move_item(mag_item, inner, 0, 0, 0, True) == 0, "D4 把弹匣移进套包内部")
    top = arr(comp.get_container_items_view(backpack))
    check(len(top) == 1 and top[0].handle == pouch_item, "D5 顶层视图只剩套包（实际 %s 件）" % len(top))
    inside = arr(comp.get_container_items_view(inner))
    check(len(inside) == 1 and inside[0].handle == mag_item, "D6 套包内部的视图能看到弹匣（实际 %s 件）" % len(inside))
    check(len(arr(comp.find_items_by_definition(mag))) == 1, "D7 FindItemsByDefinition 能穿透套包找到它")

    # ---------- E. 存档 ----------
    res_bytes = comp.save_to_bytes()
    note("SaveToBytes 返回类型 = %s" % [type(x).__name__ for x in items_of(res_bytes)])
    raw = None
    for it in items_of(res_bytes):
        if not isinstance(it, bool):
            raw = it
    if raw is not None and not isinstance(raw, (bytes, bytearray)):
        raw = bytes(bytearray(int(b) & 0xFF for b in raw))
    data = raw
    check(isinstance(data, (bytes, bytearray)) and len(data) > 0,
          "E1 SaveToBytes 输出 %s 字节" % (len(data) if data else 0))

    check(len(arr(comp.get_all_items())) == 2, "E2 存档前共 2 件物品")
    destroyed = comp.destroy_item(pouch_item)
    check(destroyed == 2, "E3 DestroyItem(套包) 返回被删件数 2（实际 %s）" % destroyed)
    check(len(arr(comp.get_all_items())) == 0, "E4 删干净了")

    check(comp.load_from_bytes(data) is True, "E5 LoadFromBytes 成功")
    check(len(arr(comp.get_all_items())) == 2, "E6 读档后物品回来了（实际 %s 件）" % len(arr(comp.get_all_items())))
    new_backpack = comp.get_container(unreal.InvContainerType.BACKPACK)
    check(new_backpack > 0, "E6b 读档后重新取到背包句柄 %s（旧句柄 %s 已作废）" % (new_backpack, backpack))
    backpack = new_backpack

    narrow = make_comp("TestComp_Narrow", [mag],
                       [make_root(unreal.InvContainerType.BACKPACK, "背包", [(6, 5)]),
                        make_root(unreal.InvContainerType.POCKETS, "口袋", [(1, 1)] * 4)], cap=0.0)
    narrow.initialize_inventory()
    note("目录不匹配的读档结果（文档说应当被拒）= %s" % narrow.load_from_bytes(data))

    check(comp.load_from_bytes(bytes()) is False, "E7 空字节读档返回 false")
    check(len(arr(comp.get_all_items())) == 2, "E8 读档失败不影响现有数据")

    # ---------- F. 未就绪 / 无效句柄 ----------
    cold = make_comp("TestComp_Cold", [mag], [], cap=0.0)
    check(cold.ready is False, "F1 没初始化 => bReady false")
    cold_code = cold.give_item(mag, 1, 0, 0, 0, False, 1)
    check(cold_code == -23, "F2 未就绪 => GiveItem 返回 -23（实际 %s）" % cold_code)
    check(cold.get_container(unreal.InvContainerType.BACKPACK) == -23, "F3 未就绪 => GetContainer 返回 -23")
    check(arr(cold.get_all_items()) == [], "F4 未就绪 => 空物品列表")
    f5 = cold.get_item_view(-1)
    check(f5 is None or f5.handle == 0, "F5 未就绪 => GetItemView 给空视图（句柄 %s）"
          % (f5.handle if f5 else None))
    check(cold.is_overloaded() is False and cold.get_total_weight() == 0.0, "F6 未就绪 => 负重安全默认值")
    f7 = cold.save_to_bytes()
    check(f7 is None or len(f7) == 0, "F7 未就绪 => SaveToBytes 不崩且给空结果")

    f8 = comp.get_item_view(-1)
    check(f8 is None or f8.handle == 0, "F8 无效物品句柄 => 空视图（句柄 %s）"
          % (f8.handle if f8 else None))
    check(comp.move_item(-1, backpack, 0, 0, 0, False) > 0, "F9 无效物品句柄 => 错误码")
    check(comp.swap_items(-1, -2) > 0, "F10 两个无效句柄互换 => 错误码")
    check(comp.rotate_item(-1) > 0, "F11 无效句柄旋转 => 错误码")
    check(comp.destroy_item(-1) < 0, "F12 无效句柄删除 => 负错误码")
    check(arr(comp.get_container_parts(999999)) == [], "F13 野容器句柄 => 空子网格")
    check(comp.get_container_label(999999) == "", "F14 野容器句柄 => 空名字")
    check(arr(comp.get_container_grid(backpack, 99)) == [], "F15 子网格下标越界 => 空占格表")
    check(comp.get_item_definition(-1) is None, "F16 无效句柄取定义 => 空")
    check(comp.autosort_container(backpack) == 0, "F17 Autosort 正常返回 0")

    # ---------- G. 字段夹取与运行时补注册 ----------
    bad = make_def("TestDef_BadSize", 0, 0, stack=100000, weight=-1.0)
    bad_id = comp.register_definition(bad)
    check(bad_id >= 0, "G1 越界字段被夹住，定义照样注册成功（DefId %s）" % bad_id)
    check(part_xy(bad.footprint(False)) == (1, 1), "G2 尺寸 0 时 Footprint 兜底为 1x1")

    extra = make_def("TestDef_Extra", 1, 1, stack=1, weight=0.1, value=5)
    taken = [i for i, v in enumerate(arr(comp.get_container_grid(backpack, 0))) if v != 0]
    note("G3 前背包已占格下标 = %s" % taken)
    extra_item = comp.give_item(extra, backpack, 0, 5, 0, False, 1)
    check(extra_item > 0, "G3 没注册的定义由 GiveItem 就地补注册并落位")
    check(comp.get_def_id(extra) >= 0, "G4 补注册拿到有效 DefId（实际 %s）" % comp.get_def_id(extra))
    check(comp.get_item_definition(extra_item) == extra, "G5 句柄 -> 定义 反查正确")

    emit("===== 结束 =====")


try:
    main()
except Exception:
    FAIL += 1
    emit("[FAIL] 脚本异常:\n" + traceback.format_exc())
finally:
    dump()
