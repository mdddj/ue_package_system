# 背包**内容管线**（DataTable → 物品定义）的无头验证。
#
#   UnrealEditor-Cmd <proj> -run=pythonscript -script=<此文件> \
#       -EnablePlugins=PythonScriptPlugin -unattended -nosplash
#
# 覆盖七件事（对应交付清单 1~7）：
#   1. 导入端到端：用 unreal.AssetImportTask 把 tools/samples/ItemTable.sample.json 导成真 DataTable，
#      断言行数 ≥ 5、行名对得上、抽样字段与样例文件逐字段一致（把表再导出成 JSON 对账）；
#   2. 表单源注册：组件只配表 → GetAllDefinitions() 按 RowName 升序对应 DefId 0..N-1，
#      行字段（宽高 / 堆叠 / 重量 / 标签 / 容器规格 / 名字）透传到定义对象；
#   3. 图标软引用透传：程序化行（资产不存在）的 Icon 路径字符串从行 → 定义；
#   4. 向后兼容：只用 ItemDefinitions 数组时 DefId 仍是"按资产名升序"；
#   5. 混用：资产 + 表都在 → 资产（按名）在前、表行（按 RowName）在后；
#   6. 错误路径：表 RowStruct 不符 → 不崩、打日志、其余来源照常注册；
#   7. 按名查找：FindDefinitionByName(行名 / 资产名) 命中同一个定义对象，不存在 → 空。
#
# 另外检查一张真资产：演示表（插件 Content 里的 DT_InventoryItems）存在、行数 ≥ 5、行结构正确。
#
# 结果写脚本同目录的 inv_pipeline_test_out.txt（末行 SUMMARY pass/fail）。
# 依赖：项目 C++ 已编译、Rust dylib 已放到 Binaries/<平台>/。
#
# 为什么导入用 ReimportDataTableFactory 而不是 DataTableFactory：
# .json 只有 UReimportDataTableFactory 声明了 "json" 格式且 FactoryCanImport 恒真，
# AssetTools 在 automated 模式下才会拿它当指定工厂（否则没有工厂认领 .json，导入直接失败）；
# 行结构靠 AutomatedImportSettings.ImportRowStruct 传进去（automated 模式不会弹选择框）。

import json
import os
import re
import traceback

import unreal

_HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
OUT = os.path.join(_HERE, "inv_pipeline_test_out.txt")
SAMPLE_JSON = os.path.normpath(os.path.join(_HERE, os.pardir, "samples", "ItemTable.sample.json"))

TEMP_DIR = "/Game/BackpackDemo/PipelineTemp"
TEMP_TABLE = TEMP_DIR + "/DT_PipelineSample"
TEMP_WRONGSTRUCT_TABLE = TEMP_DIR + "/DT_PipelineWrongStruct"
DEMO_TABLE = "/ue_package_system/Demo/DT_InventoryItems"

LINES = []
PASS = 0
FAIL = 0
KEEP = []  # 强引用，防临时对象被回收


def emit(msg):
    LINES.append(msg)
    unreal.log_warning("[INVPIPE] " + msg)


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        emit("[PASS] " + msg)
    else:
        FAIL += 1
        emit("[FAIL] " + msg)
    return bool(cond)


def note(msg):
    emit("[INFO] " + msg)


def arr(res):
    """TArray 返回值：转到 python list。"""
    return list(res) if res is not None else []


# ==================== 通用工具 ====================


def row_struct():
    """行结构对象（FInvItemDefinitionRow 的 UScriptStruct）。"""
    return unreal.InvItemDefinitionRow.static_struct()


def import_json_table(source_json, dest_path, dest_name, struct):
    """用 AssetImportTask + AssetTools 把 JSON 导成 DataTable 资产，返回资产（失败返回 None）。"""
    factory = unreal.ReimportDataTableFactory()
    settings = factory.get_editor_property("automated_import_settings")
    settings.set_editor_property("import_row_struct", struct)
    settings.set_editor_property("import_type", unreal.CSVImportType.ECSV_DATA_TABLE)
    factory.set_editor_property("automated_import_settings", settings)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source_json)
    task.set_editor_property("destination_path", dest_path)
    task.set_editor_property("destination_name", dest_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    task.set_editor_property("factory", factory)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return unreal.load_asset("%s/%s" % (dest_path, dest_name))


def export_rows(table):
    """把表导出成 JSON 再解析回 python：{行名: {字段: 值}}。

    用导出对账是因为 Python 读不到 RowMap（DataTable 没暴露行数据），
    导出走的是引擎自己的属性导出器，足以证明"表里真的存着这些值"。
    """
    res = table.export_to_json_string()
    text = None
    if isinstance(res, tuple):
        for item in res:
            if isinstance(item, str):
                text = item
    elif isinstance(res, str):
        text = res
    if not text:
        return {}
    parsed = json.loads(text)
    if isinstance(parsed, dict):  # 单行表导出成一个对象而不是数组
        parsed = [parsed]
    return {str(row.get("Name", "")): row for row in parsed}


def field(row, key):
    """从导出的行里取字段：属性名 / 导出名（bool 会去掉 b 前缀）两种写法都认。"""
    candidates = [key]
    if key.startswith("b") and len(key) > 1 and key[1].isupper():
        candidates.append(key[1:])
    else:
        candidates.append("b" + key[:1].upper() + key[1:])
    for name in candidates:
        if name in row:
            return row[name]
    return None


def text_of(value):
    """把导出的 FText 归一成源字符串。

    表里的 FText 有稳定本地化键，导出时写的是 `NSLOCTEXT("表 [GUID]", "行名_字段名", "原文")`；
    导入时写的是裸字符串。两种都要能对账，所以这里只取最后一段引号里的原文。
    """
    text = "" if value is None else str(value)
    if text.startswith("NSLOCTEXT(") or text.startswith("LOCTEXT(") or text.startswith("INVTEXT("):
        quoted = re.findall(r'"((?:[^"\\]|\\.)*)"', text)
        if quoted:
            text = quoted[-1].replace('\\"', '"').replace("\\\\", "\\")
    elif text.startswith('"') and text.endswith('"') and len(text) >= 2:
        text = text[1:-1].replace('\\"', '"')
    return text


def same_point_xy(value, expected):
    """把 FIntPoint 的几种浮法（{'X':6,'Y':5} / '(X=6,Y=5)' / [6,5]）归一成 (x, y)。"""
    if isinstance(value, dict):
        return (value.get("X"), value.get("Y")) == expected
    if isinstance(value, str):
        digits = [int(x) for x in value.replace("(", " ").replace(")", " ").replace("=", " ")
                  .replace(",", " ").split() if x.lstrip("-").isdigit()]
        return len(digits) == 2 and tuple(digits) == expected
    if isinstance(value, (list, tuple)):
        return len(value) == 2 and (value[0], value[1]) == expected
    return False


def points_of(res):
    """组件/定义返回的 TArray<FIntPoint> → [(x, y)]。"""
    return [(p.x, p.y) for p in arr(res)]


def asset_name_of(config_path):
    """'X/Y/T_Foo.T_Foo' → 'T_Foo'（只用来宽松比软引用）。"""
    return os.path.basename(config_path).split(".")[0]


def make_def(name, **props):
    d = unreal.new_object(unreal.InvItemDefinition, name=name)
    for key, value in props.items():
        d.set_editor_property(key, value)
    KEEP.append(d)
    return d


def make_comp(name, defs=None, table=None, roots=None):
    c = unreal.new_object(unreal.InvInventoryComponent, name=name)
    if defs is not None:
        c.set_editor_property("item_definitions", defs)
    if table is not None:
        c.set_editor_property("item_definition_table", table)
    if roots is not None:
        c.set_editor_property("root_containers", roots)
    KEEP.append(c)
    return c


def names_of(defs):
    return [d.get_name() for d in arr(defs)]


def fname_sort_key(name):
    """与 C++ 侧 FNameLexicalLess 对齐：FName::Compare 是 IgnoreCase 的字母序。"""
    return str(name).lower()


def with_blob_run(fn):
    """跑一个段落：异常不许打断整套（否则后面的检查全丢）。"""
    try:
        fn()
    except Exception:  # noqa: BLE001
        check(False, "段落 %s 抛异常：\n%s" % (fn.__name__, traceback.format_exc()))


# ==================== 1. 导入端到端 ====================

SAMPLE_ROWS = []
IMPORTED_TABLE = None


def section_import():
    global IMPORTED_TABLE

    with open(SAMPLE_JSON, encoding="utf-8") as handle:
        SAMPLE_ROWS.extend(json.load(handle))
    note("样例文件 %s：%d 行" % (os.path.basename(SAMPLE_JSON), len(SAMPLE_ROWS)))

    IMPORTED_TABLE = import_json_table(SAMPLE_JSON, TEMP_DIR, "DT_PipelineSample", row_struct())
    KEEP.append(IMPORTED_TABLE)
    if not check(IMPORTED_TABLE is not None, "样例 JSON 导入成了 DataTable 资产（%s）" % TEMP_TABLE):
        return

    check(IMPORTED_TABLE.get_row_struct() == row_struct(),
          "导入后的行结构是 FInvItemDefinitionRow")

    row_names = [str(n) for n in arr(IMPORTED_TABLE.get_row_names())]
    expected_names = [str(row["Name"]) for row in SAMPLE_ROWS]
    check(len(row_names) >= 5, "行数 ≥ 5（实际 %d）" % len(row_names))
    check(sorted(row_names) == sorted(expected_names),
          "行名与样例一致（%s）" % ", ".join(row_names))

    rows = export_rows(IMPORTED_TABLE)
    check(len(rows) == len(expected_names), "导出对账拿到全部 %d 行（实际 %d）" % (len(expected_names), len(rows)))

    # 抽样字段：逐行、逐字段与样例文件对账（数值严格比，文本 / 软引用 / FIntPoint 归一化后比）。
    mismatches = []
    for expected in SAMPLE_ROWS:
        name = str(expected["Name"])
        row = rows.get(name)
        if row is None:
            mismatches.append("%s: 行不存在" % name)
            continue

        for key in ("DisplayName", "Description"):
            got = text_of(field(row, key))
            if got != str(expected[key]):
                mismatches.append("%s.%s: %r != %r" % (name, key, got, expected[key]))

        icon = str(field(row, "Icon") or "").strip('"')
        if asset_name_of(icon) != asset_name_of(str(expected["Icon"])):
            mismatches.append("%s.Icon: %r != %r" % (name, icon, expected["Icon"]))

        for key in ("Width", "Height", "MaxStack", "Value", "TagBits", "ForbiddenContainerTypes"):
            got = field(row, key)
            if int(got) != int(expected[key]):
                mismatches.append("%s.%s: %r != %r" % (name, key, got, expected[key]))

        got_rot = bool(field(row, "bRotatable"))
        if got_rot != bool(expected["bRotatable"]):
            mismatches.append("%s.bRotatable: %r != %r" % (name, got_rot, expected["bRotatable"]))

        if abs(float(field(row, "Weight")) - float(expected["Weight"])) > 1e-6:
            mismatches.append("%s.Weight: %r != %r" % (name, field(row, "Weight"), expected["Weight"]))

        expected_parts = [(int(p["X"]), int(p["Y"])) for p in expected["ContainerParts"]]
        got_parts = field(row, "ContainerParts")
        if isinstance(got_parts, list):
            ok_parts = len(got_parts) == len(expected_parts) and all(
                same_point_xy(got, exp) for got, exp in zip(got_parts, expected_parts))
        else:
            ok_parts = not expected_parts and not got_parts
        if not ok_parts:
            mismatches.append("%s.ContainerParts: %r != %r" % (name, got_parts, expected_parts))

    check(not mismatches, "抽样字段与样例文件逐字一致%s" % ("" if not mismatches else "（对不上：%s）" % "; ".join(mismatches)))


# ==================== 2. 表单源注册 ====================

TABLE_ORDER = []


def section_table_source():
    if IMPORTED_TABLE is None:
        check(False, "表来源：上一步没导入成功，跳过")
        return

    comp = make_comp("PipeCompTableOnly", table=IMPORTED_TABLE)
    check(comp.initialize_inventory() is True, "只配表的组件初始化成功")

    defs = arr(comp.get_all_definitions())
    TABLE_ORDER.extend(defs)
    expected_names = sorted([str(n) for n in arr(IMPORTED_TABLE.get_row_names())], key=fname_sort_key)
    got_names = names_of(defs)
    check(got_names == expected_names,
          "GetAllDefinitions() 按 RowName 升序（%s）" % ", ".join(got_names))

    # DefId 必须是 0..N-1 且与顺序一致
    ids = [comp.get_def_id(d) for d in defs]
    check(ids == list(range(len(defs))), "DefId 从 0 连续编号（%s）" % ids)
    check(comp.get_definition(0) == defs[0], "GetDefinition(0) 与第一个定义是同一个对象")

    # 字段透传：拿导入的行当依据，逐个比到定义对象上
    by_name = {str(row["Name"]): row for row in SAMPLE_ROWS}
    problems = []
    for definition in defs:
        row = by_name.get(definition.get_name())
        if row is None:
            problems.append("%s: 样例里没有这行" % definition.get_name())
            continue
        checks = {
            "width": row["Width"],
            "height": row["Height"],
            "rotatable": row["bRotatable"],
            "max_stack": row["MaxStack"],
            "weight": row["Weight"],
            "value": row["Value"],
            "tag_bits": row["TagBits"],
            "forbidden_container_types": row["ForbiddenContainerTypes"],
        }
        for prop, expected in checks.items():
            got = definition.get_editor_property(prop)
            if isinstance(expected, bool):
                same = bool(got) == expected
            elif isinstance(expected, float):
                same = abs(float(got) - expected) < 1e-6
            else:
                same = int(got) == int(expected)
            if not same:
                problems.append("%s.%s: %r != %r" % (definition.get_name(), prop, got, expected))

        if str(definition.get_editor_property("display_name")) != str(row["DisplayName"]):
            problems.append("%s.display_name: %r != %r" % (
                definition.get_name(), definition.get_editor_property("display_name"), row["DisplayName"]))

        expected_parts = [(int(p["X"]), int(p["Y"])) for p in row["ContainerParts"]]
        if points_of(definition.get_editor_property("container_parts")) != expected_parts:
            problems.append("%s.container_parts: %r != %r" % (
                definition.get_name(), points_of(definition.get_editor_property("container_parts")), expected_parts))

    check(not problems, "行字段透传到定义对象%s" % ("" if not problems else "（对不上：%s）" % "; ".join(problems)))

    # 套包：6x5 一行的容器规格要能当容器用（有内部网格）
    bag = comp.find_definition_by_name("Backpack_Assault")
    check(bag is not None and bag.has_container(), "带 ContainerParts 的行成了容器（HasContainer() 为真）")

    # 表来源也能直接拿去玩：造一件物品证明定义真的能用
    roots = [setup_for(unreal.InvContainerType.BACKPACK, "背包", [(6, 5)])]
    play = make_comp("PipeCompTablePlay", table=IMPORTED_TABLE, roots=roots)
    ok = play.initialize_inventory() is True
    backpack = play.get_container(unreal.InvContainerType.BACKPACK) if ok else 0
    ammo = play.find_definition_by_name("Ammo_762")
    handle = play.give_item(ammo, backpack, 0, 0, 0, False, 60) if ammo is not None else -1
    check(handle > 0, "表来源的定义能直接拿来生成物品（弹药 ×60）")


def setup_for(kind, label, parts):
    setup = unreal.InvRootContainerSetup()
    setup.set_editor_property("type", kind)
    setup.set_editor_property("label", label)
    setup.set_editor_property("parts", [unreal.IntPoint(p[0], p[1]) for p in parts])
    return setup


# ==================== 3. 图标软引用透传 ====================

PROBE_ICON_PATH = "/Game/NotARealProject/Icons/T_Icon_Probe.T_Icon_Probe"
# 引擎自带的贴图：程序里一定存在，用来验证"软引用能解析出来"。
# 为什么不用上面那条假路径做定义级断言：Python 把 TSoftObjectPtr<UTexture2D> 当**对象引用**暴露
# （NativizeObject 只认 Texture2D），读出来的是"解析后的对象"，解析不到就是 None，
# 看不出是"没抄过去"还是"抄了但资产不存在"。所以分两段验：
#   表级：假路径，导出对账证明行里存的就是那条路径字符串；
#   定义级：真路径，证明行 → 定义的软引用拷贝真的发生了（能解析成同一个贴图对象）。
ENGINE_ICON_PATH = "/Engine/EngineResources/DefaultTexture.DefaultTexture"


def section_icon_passthrough():
    json_text = json.dumps([
        {
            "Name": "Icon_Probe",
            "DisplayName": "图标探针",
            "Description": "没有真实图标资产，只有一条路径",
            "Icon": PROBE_ICON_PATH,
            "Width": 1,
            "Height": 1,
            "bRotatable": False,
            "MaxStack": 1,
            "Weight": 0.1,
            "Value": 7,
            "TagBits": 0,
            "ForbiddenContainerTypes": 0,
            "ContainerParts": [],
        },
        {
            "Name": "Icon_Engine",
            "DisplayName": "引擎贴图探针",
            "Description": "图标指向引擎自带贴图",
            "Icon": ENGINE_ICON_PATH,
            "Width": 1,
            "Height": 1,
            "bRotatable": False,
            "MaxStack": 1,
            "Weight": 0.1,
            "Value": 8,
            "TagBits": 0,
            "ForbiddenContainerTypes": 0,
            "ContainerParts": [],
        },
    ])

    table = unreal.new_object(unreal.DataTable, name="PipeProgrammaticTable")
    KEEP.append(table)
    filled = unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json_text, row_struct())
    if not check(filled is True, "程序化 JSON 填表成功（内存表，不落盘）"):
        return
    names = [str(n) for n in arr(table.get_row_names())]
    check(sorted(names) == ["Icon_Engine", "Icon_Probe"], "程序化表里有 Icon_Probe / Icon_Engine 两行")

    # 表级：假路径原样进表
    rows = export_rows(table)
    probe_icon = str(field(rows.get("Icon_Probe", {}), "Icon") or "").strip('"')
    check(probe_icon == PROBE_ICON_PATH,
          "不存在资产的图标路径原样进表（%s）" % probe_icon)

    comp = make_comp("PipeCompIcon", table=table)
    check(comp.initialize_inventory() is True, "程序化表来源的组件初始化成功")

    definition = comp.find_definition_by_name("Icon_Probe")
    if not check(definition is not None, "FindDefinitionByName('Icon_Probe') 命中"):
        return
    check(str(definition.get_editor_property("display_name")) == "图标探针", "FText 字段透传（中文不乱码）")
    check(definition.get_editor_property("value") == 7, "数值字段透传（Value = 7）")

    # 定义级：真路径 → 解析成同一个贴图对象
    engine_def = comp.find_definition_by_name("Icon_Engine")
    if not check(engine_def is not None, "FindDefinitionByName('Icon_Engine') 命中"):
        return
    icon = engine_def.get_editor_property("icon")
    expected = unreal.load_asset(ENGINE_ICON_PATH)
    check(expected is not None, "引擎贴图 %s 存在（拿来当图标基准）" % ENGINE_ICON_PATH)
    got_path = icon.get_path_name() if icon is not None else "<None>"
    check(icon is not None and expected is not None and got_path == expected.get_path_name(),
          "Icon 软引用从行透传到定义并能解析成同一张贴图（%s）" % got_path)


# ==================== 4. 向后兼容（只用资产数组）====================


def section_backward_compat():
    # 老项目只有 ItemDefinitions，DefId 必须还是"按资产名升序"。
    # 故意乱序填数组，验证排序不看数组顺序。
    zeta = make_def("PipeDef_Zeta", width=1, height=1, max_stack=1, weight=1.0, value=10)
    alpha = make_def("PipeDef_Alpha", width=1, height=1, max_stack=1, weight=2.0, value=20)
    mike = make_def("PipeDef_Mike", width=2, height=1, max_stack=1, weight=3.0, value=30)

    comp = make_comp("PipeCompAssetsOnly", defs=[zeta, mike, alpha])
    check(comp.initialize_inventory() is True, "老式（只有资产）组件初始化成功")

    defs = arr(comp.get_all_definitions())
    check(names_of(defs) == ["PipeDef_Alpha", "PipeDef_Mike", "PipeDef_Zeta"],
          "只用资产时仍按资产名升序（%s）" % ", ".join(names_of(defs)))
    check([comp.get_def_id(d) for d in defs] == [0, 1, 2], "只用资产时 DefId 仍是 0/1/2")
    check(comp.get_def_id(alpha) == 0 and comp.get_def_id(mike) == 1 and comp.get_def_id(zeta) == 2,
          "每个资产的 DefId 与升序位置一致")

    # 没配表的老组件：按名查找只能命中资产名，且不会因为没表而报警/崩
    check(comp.find_definition_by_name("PipeDef_Alpha") == alpha, "按资产名能查到定义")
    check(comp.find_definition_by_name("不存在的名字") is None, "查不存在的名字返回空")


# ==================== 5. 混用（资产 + 表）====================


def section_mixed():
    if IMPORTED_TABLE is None:
        check(False, "混用：样例表没导入成功，跳过")
        return

    zeta = make_def("PipeMix_Zeta", width=1, height=1, max_stack=1, weight=1.0, value=10)
    alpha = make_def("PipeMix_Alpha", width=1, height=1, max_stack=1, weight=1.0, value=10)

    comp = make_comp("PipeCompMixed", defs=[zeta, alpha], table=IMPORTED_TABLE)
    check(comp.initialize_inventory() is True, "资产 + 表混用的组件初始化成功")

    defs = arr(comp.get_all_definitions())
    got = names_of(defs)
    expected_assets = ["PipeMix_Alpha", "PipeMix_Zeta"]
    expected_rows = sorted([str(n) for n in arr(IMPORTED_TABLE.get_row_names())], key=fname_sort_key)
    check(got == expected_assets + expected_rows,
          "混用顺序 = 资产（按名）在前、表行（按 RowName）在后（%s）" % ", ".join(got))
    check([comp.get_def_id(d) for d in defs] == list(range(len(defs))), "混用时 DefId 从 0 连续")

    # 资产那一份的号不受表影响：加表前 PipeMix_Alpha 是 0，加表后还是 0
    check(comp.get_def_id(alpha) == 0 and comp.get_def_id(zeta) == 1,
          "资产的 DefId 不受表来源影响（老存档不会错位）")

    # 表行的号接在资产后面
    row_name = expected_rows[0]
    row_def = comp.find_definition_by_name(row_name)
    check(row_def is not None and comp.get_def_id(row_def) == len(expected_assets),
          "第一条表行的 DefId 紧跟资产之后（%s -> %d）" % (row_name, comp.get_def_id(row_def) if row_def else -1))


# ==================== 6. 错误路径（RowStruct 不符）====================


def section_wrong_row_struct():
    # 故意用 FTableRowBase 当行结构导入同一份 JSON：字段全对不上，但导入本身能过。
    wrong = import_json_table(SAMPLE_JSON, TEMP_DIR, "DT_PipelineWrongStruct", unreal.TableRowBase.static_struct())
    KEEP.append(wrong)
    if not check(wrong is not None, "能造出一张行结构不符的表（RowStruct = TableRowBase）"):
        return
    check(wrong.get_row_struct() != row_struct(), "这张表的行结构与 FInvItemDefinitionRow 不同")

    good = make_def("PipeWrong_Good", width=1, height=1, max_stack=1, weight=1.0, value=10)
    comp = make_comp("PipeCompWrongStruct", defs=[good], table=wrong)
    check(comp.initialize_inventory() is True, "行结构不符时初始化仍然成功（不崩）")

    defs = arr(comp.get_all_definitions())
    check(names_of(defs) == ["PipeWrong_Good"], "行结构不符时资产来源照常注册（%s）" % ", ".join(names_of(defs)))
    check(comp.get_def_id(good) == 0, "资产来源的 DefId 不受坏表影响")
    check(comp.find_definition_by_name("AK_Mag") is None, "坏表的行名没有进按名索引")

    # 表行数为 0（坏表）与表未配置要能区分开：这里只确认调用第二次幂等且不重复注册
    check(comp.initialize_inventory() is True and len(arr(comp.get_all_definitions())) == 1,
          "重复初始化是幂等的，不会重复注册")


# ==================== 7. 按名查找 ====================


def section_find_by_name():
    if IMPORTED_TABLE is None:
        check(False, "按名查找：样例表没导入成功，跳过")
        return

    comp = make_comp("PipeCompNames", table=IMPORTED_TABLE, defs=[make_def("PipeNames_Asset", width=1, height=1,
                                                                        max_stack=1, weight=1.0, value=5)])
    check(comp.initialize_inventory() is True, "按名查找用的组件初始化成功")

    row_def = comp.get_all_definitions()[1] if len(arr(comp.get_all_definitions())) > 1 else None
    expected_first_row = sorted([str(n) for n in arr(IMPORTED_TABLE.get_row_names())], key=fname_sort_key)[0]
    check(row_def is not None and row_def.get_name() == expected_first_row,
          "定义对象名 = 表行名（%s）" % (row_def.get_name() if row_def else "-"))

    found_row = comp.find_definition_by_name(expected_first_row)
    check(found_row is not None and found_row == row_def, "按表行名查到的就是同一个定义对象")

    found_asset = comp.find_definition_by_name("PipeNames_Asset")
    check(found_asset is not None and found_asset.get_name() == "PipeNames_Asset", "按资产对象名也能查到")
    check(found_asset is not None and comp.get_def_id(found_asset) == 0, "查到的资产定义 DefId 正确")

    check(comp.find_definition_by_name("NoSuchItem") is None, "查不存在的行名返回空")

    # FName 的比较天生忽略大小写（TMap / RowName / DataTable 都一样），按名查找跟着这个语义走。
    lower_hit = comp.find_definition_by_name("ak_mag")
    upper_hit = comp.find_definition_by_name("AK_Mag")
    check(lower_hit is not None and lower_hit == upper_hit,
          "名字大小写不敏感（FName 语义，和 RowName 一致）：ak_mag 命中 AK_Mag")


    # 名字冲突：资产名和表行名撞了 → 先注册的（资产）赢，表行仍然注册但不覆盖名字。
    clash = make_def("AK_Mag", width=1, height=1, max_stack=1, weight=1.0, value=1)
    comp2 = make_comp("PipeCompClash", defs=[clash], table=IMPORTED_TABLE)
    check(comp2.initialize_inventory() is True, "资产名与表行名冲突时初始化成功（不崩）")
    hit = comp2.find_definition_by_name("AK_Mag")
    check(hit == clash, "冲突时按名查到的是先注册的资产（对象名 AK_Mag）")
    check(hit is not None and comp2.get_def_id(hit) == 0, "冲突时按名拿到的是资产那条（DefId 0）")
    row_twin = comp2.get_definition(1)
    check(row_twin is not None and row_twin != clash, "同名表行照样注册（只是按名查不到它）")
    check(row_twin is not None and row_twin.get_name() == "AK_Mag", "同名表行的对象名仍是 RowName")


# ==================== 演示资产（插件 Content 里的示例表）====================


def section_demo_asset():
    demo = unreal.load_asset(DEMO_TABLE)
    if not check(demo is not None, "演示表资产存在（%s）" % DEMO_TABLE):
        return
    check(demo.get_row_struct() == row_struct(), "演示表的行结构是 FInvItemDefinitionRow")
    row_names = [str(n) for n in arr(demo.get_row_names())]
    check(len(row_names) >= 5, "演示表行数 ≥ 5（实际 %d）" % len(row_names))

    # 直接拿这张真资产当组件配置跑一遍：用户打开插件看到的就是它，必须能开箱即用。
    comp = make_comp("PipeCompDemoAsset", table=demo)
    check(comp.initialize_inventory() is True, "演示表能直接配到组件上并初始化")
    defs = arr(comp.get_all_definitions())
    check(len(defs) == len(row_names), "演示表注册出全部 %d 个定义（实际 %d）" % (len(row_names), len(defs)))
    bag = comp.find_definition_by_name("Backpack_Assault")
    check(bag is not None and bag.has_container() and bag.get_editor_property("max_stack") == 1,
          "演示表里能按名查到套包定义（容器 + MaxStack = 1）")


# ==================== 主流程 ====================


def main():
    emit("===== 背包内容管线验证开始 =====")
    with_blob_run(section_import)
    with_blob_run(section_table_source)
    with_blob_run(section_icon_passthrough)
    with_blob_run(section_backward_compat)
    with_blob_run(section_mixed)
    with_blob_run(section_wrong_row_struct)
    with_blob_run(section_find_by_name)
    with_blob_run(section_demo_asset)
    emit("===== 结束 =====")


main()

with open(OUT, "w", encoding="utf-8") as handle:
    handle.write("\n".join(LINES) + "\nSUMMARY pass=%d fail=%d\n" % (PASS, FAIL))
unreal.log("INVPIPE pass=%d fail=%d" % (PASS, FAIL))
