# 背包演示 Actor 的无头验证（在 UE Python 命令集里跑）。
#
#   UnrealEditor-Cmd <proj> -run=pythonscript -script=<此文件> \
#       -EnablePlugins=PythonScriptPlugin -unattended -nosplash
#
# 只走公开入口：AInvInventoryDemoActor::InitializeDemoInventory 返回的摘要文本，
# 外加 UInvInventoryComponent 的公开蓝图节点（Get All Items / Get Total Weight ...）。
# 结果写脚本同目录的 inv_demo_test_out.txt（末行 SUMMARY pass/fail）；
# 最新一次的摘要原文另存 inv_demo_test_summary.txt，方便直接贴进文档 / 报告。
#
# 为什么用 spawn_actor_from_class 而不是 new_object：
# Python 里 `unreal.new_object(某个Actor类)` 造出来的是**世界外**的 Actor，调它的 native UFUNCTION
# 会被引擎静默吸收（ProcessEvent 直接返回，返回值是零值）——连引擎自带的 AActor 也一样：
# new_object 出来的普通 Actor 上调 set_actor_hidden 不生效、get_actor_label 返回空串。
# 所以这里把演示 Actor 生成到编辑器世界（临时 Untitled 地图，脚本结束就销毁、不落盘），
# 再显式调 initialize_demo_inventory()（编辑器世界不跑 BeginPlay）。
# C++ 侧直接调用（或 BeginPlay 里自己调）不受这个限制：演示 Actor 的初始化本来就不需要世界。

import os
import re
import traceback

import unreal

# 结果写到脚本同目录（跨机器可移植）
_HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
OUT = os.path.join(_HERE, "inv_demo_test_out.txt")
SUMMARY_OUT = os.path.join(_HERE, "inv_demo_test_summary.txt")
LINES = []
PASS = 0
FAIL = 0
KEEP = []     # 强引用，防临时对象被回收
SPAWNED = []  # 生成到编辑器世界的演示 Actor，跑完销毁

# 预期 7 件（逐项算的依据）：
#   口袋 弹药×60 = 1 件；口袋 急救包×3 = 1 件；弹挂 手雷×1 = 1 件；弹挂 弹匣×1 = 1 件；
#   背包 突击背包×1 = 1 件；套包内 弹药×30 = 1 件；套包内 弹匣×1（旋转）= 1 件。
EXPECTED_ITEMS = 7
# 重量 = 单件重量 × 堆叠：0.012×90 + 0.2×2 + 0.5×3 + 0.4×1 + 2.0×1 = 5.38 kg
EXPECTED_WEIGHT = 0.012 * 90 + 0.2 * 2 + 0.5 * 3 + 0.4 + 2.0
# 估值 = 单件估值 × 堆叠：3×90 + 120×2 + 300×3 + 200 + 900 = 2510
EXPECTED_VALUE = 3 * 90 + 120 * 2 + 300 * 3 + 200 + 900
EXPECTED_RATIO = EXPECTED_WEIGHT / 30.0

# 预期的物品清单：字母按“首次出现顺序”分配，摆位固定 => 这份表逐字可预期。
#   A/B 在口袋、C/D 在弹挂、E 在背包、F/G 在套包内部。
EXPECTED_LEGEND = {
    "A": ("弹药", 60, "1x1"),
    "B": ("急救包", 3, "1x1"),
    "C": ("手雷", 1, "1x1"),
    "D": ("弹匣", 1, "1x2"),
    "E": ("突击背包", 1, "3x3"),
    "F": ("弹药", 30, "1x1"),
    "G": ("弹匣", 1, "1x2, 旋转"),
}


def emit(msg):
    LINES.append(msg)
    unreal.log_warning("[INVDEMO] " + msg)


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


def get_prop(obj, name):
    """优先属性访问（BlueprintReadOnly），退回 get_editor_property（EditAnywhere / VisibleAnywhere）。"""
    if obj is None:
        return None
    try:
        value = getattr(obj, name)
        if value is not None:
            return value
    except Exception:
        pass
    try:
        return obj.get_editor_property(name)
    except Exception:
        return None


def spawn_demo_actor(label):
    """生成一个演示 Actor 到编辑器世界（见文件头说明），并记进 SPAWNED 供收尾销毁。"""
    cls = unreal.InvInventoryDemoActor
    actor = None
    try:
        subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        actor = subsystem.spawn_actor_from_class(cls, unreal.Vector(0, 0, 0))
    except Exception as err:
        note("EditorActorSubsystem 生成失败（%r），改用 EditorLevelLibrary" % err)
        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, unreal.Vector(0, 0, 0))
    if actor:
        try:
            actor.set_actor_label(label)
        except Exception:
            pass
        SPAWNED.append(actor)
    return actor


def summary_sections(text):
    """把摘要拆成 {段名: [行]}：段名取方括号里的字，只收网格行（part 标题 / 缩进 4 格的网格本体）。"""
    sections = {}
    order = []
    current = None
    for line in text.splitlines():
        m = re.match(r"^\[([^\]]+)\]", line)
        if m:
            current = m.group(1)
            if current not in sections:
                sections[current] = []
                order.append(current)
            continue
        if current is None:
            continue
        if line.startswith("  part") or line.startswith("    "):
            sections[current].append(line)
    return sections, order


def part_map(section_lines):
    """段里的 part 标题行 -> {下标: (宽, 高, 同行内容)}；多行子网格的同行内容为空串。"""
    parts = {}
    for line in section_lines:
        m = re.match(r"^  part(\d+) \((\d+)x(\d+)\):(.*)$", line)
        if m:
            parts[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), m.group(4).strip())
    return parts


def grid_rows(section_lines):
    """段里的网格本体行（缩进 4 格）。"""
    return [line[4:] for line in section_lines if line.startswith("    ")]


def legend_entries(text):
    """物品清单：{'A': {'name': '弹药', 'stack': 60, 'size': '1x1'}}"""
    entries = {}
    for line in text.splitlines():
        for m in re.finditer(r"([A-Z]+) = (\S+) ×(\d+) \(([^)]*)\)", line):
            entries[m.group(1)] = {"name": m.group(2), "stack": int(m.group(3)), "size": m.group(4)}
    return entries


def main():
    emit("===== 背包演示 Actor 无头验证开始 =====")

    # ---------- A. 初始化与摘要 ----------
    actor = spawn_demo_actor("DemoActor_Test")
    check(actor is not None, "A0 演示 Actor 生成到编辑器世界")
    if actor is None:
        return

    comp = get_prop(actor, "inventory_component")
    KEEP.append(comp)
    check(comp is not None, "A1 演示 Actor 上挂着背包组件（构造函数里 CreateDefaultSubobject）")
    check(get_prop(actor, "billboard") is not None, "A2 演示 Actor 挂着编辑器广告牌（关卡里好找）")

    summary = actor.initialize_demo_inventory()
    check(isinstance(summary, str) and len(summary) > 0,
          "A3 InitializeDemoInventory 返回文本摘要（%s 字）" % (len(summary) if isinstance(summary, str) else "?"))
    if isinstance(summary, str):
        with open(SUMMARY_OUT, "w") as f:
            f.write(summary)
        note("摘要原文已写 %s" % SUMMARY_OUT)
        for line in summary.splitlines():
            note("| " + line)

    check(comp.ready is True, "A4 组件就绪（bReady = true）")
    check(comp.inventory is not None and comp.inventory.is_valid_handle() is True,
          "A5 组件底下的背包实例有效")

    roots = list(comp.root_containers)
    check([(r.get_editor_property("label"), len(list(r.get_editor_property("parts")))) for r in roots]
          == [("口袋", 4), ("弹挂", 3), ("安全箱", 1), ("背包", 1)],
          "A6 组件被填了 4 个默认根容器（口袋 4 块 / 弹挂 3 块 / 安全箱 1 块 / 背包 1 块）")

    sections, order = summary_sections(summary)
    check(order[:4] == ["口袋", "弹挂", "安全箱", "背包"] and order[4:] == ["套包内部"],
          "A7 摘要按固定顺序给出 4 个根容器段 + 套包内部（实际 %s）" % order)
    check("套包内部" in sections, "A8 摘要展开了套包内部（突击背包）")

    pockets = part_map(sections.get("口袋", []))
    chest = part_map(sections.get("弹挂", []))
    safe = part_map(sections.get("安全箱", []))
    backpack = part_map(sections.get("背包", []))
    check(len(pockets) == 4 and all(pockets[i][:2] == (1, 1) for i in pockets),
          "A9 口袋 4 块 1x1 子网格（实际 %s）" % [pockets[i][:2] for i in sorted(pockets)])
    check(len(chest) == 3 and chest[0][:2] == (1, 1) and chest[1][:2] == (1, 1) and chest[2][:2] == (1, 2),
          "A10 弹挂 3 块子网格：1x1 / 1x1 / 1x2（实际 %s）" % [chest[i][:2] for i in sorted(chest)])
    check(len(safe) == 1 and safe[0][:2] == (4, 3),
          "A11 安全箱 1 块 4x3（实际 %s）" % [safe[i][:2] for i in sorted(safe)])
    check(len(backpack) == 1 and backpack[0][:2] == (6, 5),
          "A12 背包 1 块 6x5（实际 %s）" % [backpack[i][:2] for i in sorted(backpack)])

    # ---------- B. 件数 / 负重 / 估值 ----------
    items = list(comp.get_all_items())
    check(len(items) == EXPECTED_ITEMS,
          "B1 物品 %d 件（含套包内部；实际 %d 件）" % (EXPECTED_ITEMS, len(items)))
    weight = comp.get_total_weight()
    check(abs(weight - EXPECTED_WEIGHT) < 1e-3,
          "B2 总重 = %.3f kg（预期 %.3f = 0.012×90 + 0.2×2 + 0.5×3 + 0.4 + 2.0）"
          % (weight, EXPECTED_WEIGHT))
    ratio = comp.get_weight_ratio()
    check(abs(ratio - EXPECTED_RATIO) < 1e-4,
          "B3 负重比 = %.4f（预期 %.4f = 总重 / 30 kg）" % (ratio, EXPECTED_RATIO))
    check(abs(comp.max_capacity_kg - 30.0) < 1e-6,
          "B4 承重上限被设成 30 kg（实际 %s）" % comp.max_capacity_kg)
    value = comp.get_total_value()
    check(value == EXPECTED_VALUE, "B5 总估值 = %d（预期 %d）" % (value, EXPECTED_VALUE))

    check("物品 7 件" in summary and "5.380 kg" in summary and "2510" in summary,
          "B6 摘要汇总行含件数 / 总重 / 总估值")
    check("负重比 0.179" in summary and "上限 30 kg" in summary,
          "B7 摘要汇总行含负重比（≈ 5.38/30）与上限 30 kg")

    # ---------- C. ASCII 网格 ----------
    rows = grid_rows(sections.get("背包", []))
    check(len(rows) == 5 and all(len(r) == 6 for r in rows),
          "C1 背包网格 = 5 行 × 6 列（实际 %s）" % rows)

    entries = legend_entries(summary)
    actual_legend = {letter: (e["name"], e["stack"], e["size"]) for letter, e in entries.items()}
    check(actual_legend == EXPECTED_LEGEND,
          "C2 物品清单逐条对得上（字母按首次出现顺序 A…G）（实际 %s）" % actual_legend)

    # 突击背包 = E，3x3 落在背包 (0,0)：前三行左三格是 E，其余是点
    check(rows == ["EEE..."] * 3 + ["......"] * 2,
          "C3 背包网格画出 3x3 的突击背包 + 两行空格（实际 %s）" % rows)

    safe_rows = grid_rows(sections.get("安全箱", []))
    check(len(safe_rows) == 3 and all(r == "." * 4 for r in safe_rows),
          "C4 空的安全箱也画出 3 行 × 4 列的点（实际 %s）" % safe_rows)

    inline = {name: {i: part_map(sec)[i][2] for i in sorted(part_map(sec))}
              for name, sec in sections.items()}
    check(inline.get("口袋") == {0: "A", 1: "B", 2: ".", 3: "."},
          "C5 口袋四个 1x1：part0 = 弹药 / part1 = 急救包 / part2、part3 空（实际 %s）" % inline.get("口袋"))
    check(inline.get("弹挂") == {0: "C", 1: ".", 2: ""},
          "C6 弹挂：part0 = 手雷、part1 空、part2 是 1x2 另起行画（实际 %s）" % inline.get("弹挂"))

    chest_rows = grid_rows(sections.get("弹挂", []))
    check(chest_rows == ["D", "D"], "C7 弹挂 part2 的 1x2 竖直占两行（实际 %s）" % chest_rows)

    # 套包内部：弹药 ×30（F）在 (0,0)，旋转的弹匣（G）占 (1,0)-(2,0)
    # —— 1x2 旋转后横向占 2 格，行内一眼能看出宽高互换
    nested = sections.get("套包内部", [])
    nested_rows = grid_rows(nested)
    nested_parts = part_map(nested)
    check(len(nested_parts) == 1 and nested_parts.get(0, ("", "", ""))[:2] == (6, 5),
          "C8 套包内部是一块 6x5 子网格（实际 %s）" % [nested_parts[i][:2] for i in sorted(nested_parts)])
    check(nested_rows[:1] == ["FGG..."] and len(nested_rows) == 5 and all(len(r) == 6 for r in nested_rows),
          "C9 套包内部第一行 = 弹药 + 旋转弹匣横向占两格 + 空格（实际 %s）" % nested_rows)

    # ---------- D. 幂等 ----------
    second = actor.initialize_demo_inventory()
    check(second == summary, "D1 再调一次 InitializeDemoInventory 返回同一份文本（没有重复装载）")
    check(len(list(comp.get_all_items())) == EXPECTED_ITEMS,
          "D2 幂等调用后物品仍是 %d 件（实际 %d）" % (EXPECTED_ITEMS, len(list(comp.get_all_items()))))
    check(abs(comp.get_total_weight() - EXPECTED_WEIGHT) < 1e-3,
          "D3 幂等调用后总重没翻倍（%.3f kg）" % comp.get_total_weight())

    # ---------- E. 内置定义 ----------
    defs = list(get_prop(actor, "builtin_definitions") or [])
    check(len(defs) == 5, "E1 内置定义 5 件（实际 %d）" % len(defs))
    ids = [comp.get_def_id(d) for d in defs]
    check(all(i >= 0 for i in ids), "E2 内置定义都注册进目录了（DefId = %s）" % ids)
    names = sorted(d.get_name() for d in defs)
    check(names == ["DemoDef_Ammo", "DemoDef_Backpack", "DemoDef_Grenade", "DemoDef_Mag", "DemoDef_Medkit"],
          "E3 内置定义名字固定（实际 %s）" % names)
    comp_defs = list(comp.item_definitions)
    check(len(comp_defs) == 5 and all(d in defs for d in comp_defs),
          "E4 组件的内容表被填成了这 5 件（实际 %d 件）" % len(comp_defs))
    sizes = {d.get_name(): (d.width, d.height, d.max_stack) for d in defs}
    check(sizes.get("DemoDef_Ammo") == (1, 1, 60) and sizes.get("DemoDef_Mag") == (1, 2, 1)
          and sizes.get("DemoDef_Medkit") == (1, 1, 3) and sizes.get("DemoDef_Grenade") == (1, 1, 1)
          and sizes.get("DemoDef_Backpack") == (3, 3, 1),
          "E5 内置定义的宽高 / 堆叠与规格一致（实际 %s）" % sizes)
    mag_def = [d for d in defs if d.get_name() == "DemoDef_Mag"][0]
    pack_def = [d for d in defs if d.get_name() == "DemoDef_Backpack"][0]
    check(mag_def.rotatable is True and mag_def.has_container() is False, "E6 弹匣可旋转、不是容器")
    parts = list(pack_def.container_parts)
    check(pack_def.has_container() is True and len(parts) == 1 and (parts[0].x, parts[0].y) == (6, 5),
          "E7 突击背包自带一块 6x5 内部网格")

    # ---------- F. 未就绪保护 ----------
    cold = unreal.new_object(unreal.InvInventoryComponent, name="DemoColdComp")
    KEEP.append(cold)
    cold_container = cold.get_container(unreal.InvContainerType.BACKPACK)
    check(cold_container in (0, -23),
          "F1 没配任何东西的组件 GetContainer 返回 0 / -23，不崩（实际 %s）" % cold_container)
    check(cold.ready is False and cold.get_total_weight() == 0.0 and cold.is_overloaded() is False,
          "F2 未就绪组件给安全默认值（总重 0 / 不超重）")
    check(list(cold.get_all_items()) == [], "F3 未就绪组件没有物品")

    # ---------- G. 两个演示 Actor 互不干扰 ----------
    other = spawn_demo_actor("DemoActor_Second")
    check(other is not None, "G0 第二个演示 Actor 也生成成功（内置定义挂在各自 Actor 下，不撞名）")
    if other:
        other_summary = other.initialize_demo_inventory()
        other_comp = get_prop(other, "inventory_component")
        KEEP.append(other_comp)
        check(other_summary == summary, "G1 第二个演示 Actor 的摘要与第一个逐字一致（输出确定）")
        check(other_comp is not None and len(list(other_comp.get_all_items())) == EXPECTED_ITEMS,
              "G2 第二个演示 Actor 独立灌了 %d 件" % EXPECTED_ITEMS)
        check(len(list(comp.get_all_items())) == EXPECTED_ITEMS,
              "G3 第一个演示 Actor 不受影响，仍是 %d 件" % EXPECTED_ITEMS)

    emit("===== 结束 =====")


try:
    main()
except Exception:
    FAIL += 1
    emit("[FAIL] 脚本异常:\n" + traceback.format_exc())
finally:
    # 收尾：把生成到编辑器世界的演示 Actor 销毁（地图不落盘，纯粹别留垃圾）
    for actor in SPAWNED:
        try:
            subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
            subsystem.destroy_actor(actor)
        except Exception:
            try:
                unreal.EditorLevelLibrary.destroy_actor(actor)
            except Exception:
                pass
    dump()
