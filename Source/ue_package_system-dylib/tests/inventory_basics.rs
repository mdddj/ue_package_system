//! 目录 / 容器 / 统计 基础测试：根容器登记、add_item 全错误路径、
//! 聚合统计（含嵌套）、标签查询、确定性顺序、destroy_item 子树清理、
//! root_type_of / depth_of 四层套包链路。

use ue_package_system_dylib::*;

/// 建一个含口袋 + 背包装备的库存，返回 (背包, 口袋)。
fn setup_roots(inv: &mut Inventory) -> (ContainerKey, ContainerKey) {
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let pockets = inv
        .add_root(ContainerType::Pockets, "口袋", &[(2, 1)])
        .unwrap();
    (backpack, pockets)
}

// ==================== 1. 根容器 ====================

#[test]
fn add_root_four_types_and_errors() {
    let mut inv = Inventory::new();
    assert!(inv.add_root(ContainerType::Pockets, "口袋", &[(1, 1)]).is_ok());
    assert!(inv.add_root(ContainerType::ChestRig, "弹挂", &[(1, 1), (2, 2)]).is_ok());
    assert!(inv.add_root(ContainerType::SafeBox, "安全箱", &[(3, 3)]).is_ok());
    assert!(inv.add_root(ContainerType::Backpack, "背包", &[(5, 4)]).is_ok());

    // 重复登记
    assert_eq!(
        inv.add_root(ContainerType::Pockets, "口袋2", &[(1, 1)]).unwrap_err(),
        InventoryError::AlreadyExists
    );

    // 非法 dims：0 尺寸 / 空列表
    let mut inv2 = Inventory::new();
    assert_eq!(
        inv2.add_root(ContainerType::Backpack, "坏", &[(0, 1)]).unwrap_err(),
        InventoryError::InvalidDef
    );
    assert_eq!(
        inv2.add_root(ContainerType::Backpack, "空", &[]).unwrap_err(),
        InventoryError::InvalidDef
    );
    // 超过单维上限
    assert_eq!(
        inv2.add_root(ContainerType::Backpack, "超", &[(33, 1)]).unwrap_err(),
        InventoryError::InvalidDef
    );

    // Nested 不能当根
    let mut inv3 = Inventory::new();
    assert_eq!(
        inv3.add_root(ContainerType::Nested, "嵌套", &[(1, 1)]).unwrap_err(),
        InventoryError::InvalidDef
    );
    assert!(inv3.check_invariants().is_empty());
}

// ==================== 2. add_item 错误路径 ====================

#[test]
fn add_item_error_paths() {
    let mut inv = Inventory::new();
    inv.define_item(ItemDef::new("弹药", 1, 1, 60)).unwrap();
    let rig = inv
        .add_root(ContainerType::ChestRig, "弹挂", &[(1, 1), (2, 2)])
        .unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let ammo = DefId(0);

    let before = inv.snapshot().encode().unwrap();

    // 未知 DefId
    assert_eq!(
        inv.add_item(DefId(999), 1, 0, backpack, 0, 0, 0, false).unwrap_err(),
        InventoryError::UnknownDef
    );
    // stack = 0
    assert_eq!(
        inv.add_item(ammo, 0, 0, backpack, 0, 0, 0, false).unwrap_err(),
        InventoryError::InvalidStackCount
    );
    // stack > max_stack
    assert_eq!(
        inv.add_item(ammo, 61, 0, backpack, 0, 0, 0, false).unwrap_err(),
        InventoryError::InvalidStackCount
    );
    // rotated 但不可旋转
    assert_eq!(
        inv.add_item(ammo, 1, 0, backpack, 0, 0, 0, true).unwrap_err(),
        InventoryError::NotRotatable
    );
    // part 越界（弹挂只有 2 块）
    assert_eq!(
        inv.add_item(ammo, 1, 0, rig, 5, 0, 0, false).unwrap_err(),
        InventoryError::PartOutOfRange
    );
    // 越界
    assert_eq!(
        inv.add_item(ammo, 1, 0, backpack, 0, 4, 0, false).unwrap_err(),
        InventoryError::OutOfBounds
    );
    // 占位冲突
    inv.add_item(ammo, 1, 0, backpack, 0, 0, 0, false).unwrap();
    assert_eq!(
        inv.add_item(ammo, 1, 0, backpack, 0, 0, 0, false).unwrap_err(),
        InventoryError::Occupied
    );

    // 全部失败路径都不应改动状态（最后成功的 add 才是唯一变化）
    let after = inv.snapshot().encode().unwrap();
    assert_ne!(before, after, "成功放置应改变状态");
    assert_eq!(inv.item_count(), 1);
    assert!(inv.check_invariants().is_empty());
}

#[test]
fn add_root_and_item_keep_state_clean() {
    let mut inv = Inventory::new();
    inv.define_item(ItemDef::new("弹药", 1, 1, 60)).unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    inv.add_item(DefId(0), 30, 7, backpack, 0, 1, 2, false).unwrap();
    assert!(inv.check_invariants().is_empty());
    let (c, p, m) = inv.container_of_item(inv.items().next().unwrap().0).unwrap();
    assert_eq!((c, p, m.x, m.y), (backpack, 0, 1, 2));
    let inst = inv.items().next().unwrap().1;
    assert_eq!((inst.stack, inst.durability), (30, 7));
}

// ==================== 3. 聚合统计（含嵌套） ====================

#[test]
fn aggregates_include_nested() {
    let mut inv = Inventory::new();
    let ammo = inv
        .define_item(ItemDef::new("弹药", 1, 1, 60).weight(0.01).value(2).tags(tags::AMMO))
        .unwrap();
    let bag = inv
        .define_item(
            ItemDef::new("背包", 2, 2, 1)
                .weight(2.0)
                .value(100)
                .as_container(ContainerSpec::rectangular(4, 4)),
        )
        .unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let pockets = inv
        .add_root(ContainerType::Pockets, "口袋", &[(2, 1)])
        .unwrap();

    inv.add_item(ammo, 60, 0, pockets, 0, 0, 0, false).unwrap();
    let bag_item = inv.add_item(bag, 1, 0, backpack, 0, 0, 0, false).unwrap();
    let inner = inv.item(bag_item).unwrap().container.unwrap();
    inv.add_item(ammo, 10, 0, inner, 0, 0, 0, false).unwrap();

    let agg = inv.aggregates();
    // 0.01*60 + 2.0 + 0.01*10 = 2.7（f32 存 / f64 累加，容差 1e-6）
    assert!(
        (agg.weight - 2.7).abs() < 1e-6,
        "重量应含套包内部，实际 {}",
        agg.weight
    );
    // 估值整数精确：2*60 + 100 + 2*10 = 240
    assert_eq!(agg.value, 240);
    assert_eq!(agg.item_count, 3);
    assert!(inv.check_invariants().is_empty());
}

// ==================== 4. 标签查询 & 确定性顺序 ====================

#[test]
fn tag_query_recurses_and_order_is_deterministic() {
    let mut inv = Inventory::new();
    let ammo = inv
        .define_item(ItemDef::new("弹药", 1, 1, 60).tags(tags::AMMO))
        .unwrap();
    let med = inv
        .define_item(ItemDef::new("药", 1, 1, 5).tags(tags::MEDKIT))
        .unwrap();
    let bag = inv
        .define_item(
            ItemDef::new("背包", 2, 2, 1)
                .as_container(ContainerSpec::rectangular(4, 4)),
        )
        .unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();

    let bag_item = inv.add_item(bag, 1, 0, backpack, 0, 0, 0, false).unwrap();
    let inner = inv.item(bag_item).unwrap().container.unwrap();
    // 顶层一件弹药，套包内部一件弹药 + 一件药
    inv.add_item(ammo, 5, 0, backpack, 0, 3, 3, false).unwrap();
    inv.add_item(ammo, 5, 0, inner, 0, 0, 0, false).unwrap();
    inv.add_item(med, 5, 0, inner, 0, 1, 0, false).unwrap();

    let ammo_hits = inv.find_by_tag(tags::AMMO);
    assert_eq!(ammo_hits.len(), 2, "标签查询必须递归进套包内部");
    let med_hits = inv.find_by_tag(tags::MEDKIT);
    assert_eq!(med_hits.len(), 1);
    assert_eq!(inv.find_by_tag(tags::GRENADE).len(), 0);

    // 顺序确定性：连跑两次一致
    assert_eq!(inv.items_in(backpack), inv.items_in(backpack));
    assert_eq!(inv.all_items(), inv.all_items());
    assert_eq!(inv.all_containers(), inv.all_containers());

    // all_items 覆盖全部 4 件
    assert_eq!(inv.all_items().len(), 4);
    assert_eq!(inv.all_containers().len(), 2); // 背包根 + 套包内部
}

// ==================== 5. destroy_item 子树清理 ====================

#[test]
fn destroy_item_removes_subtree() {
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("弹药", 1, 1, 60)).unwrap();
    let bag = inv
        .define_item(
            ItemDef::new("背包", 2, 2, 1)
                .as_container(ContainerSpec::rectangular(4, 4)),
        )
        .unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();

    let bag_item = inv.add_item(bag, 1, 0, backpack, 0, 0, 0, false).unwrap();
    let inner = inv.item(bag_item).unwrap().container.unwrap();
    inv.add_item(ammo, 5, 0, inner, 0, 0, 0, false).unwrap();
    inv.add_item(ammo, 5, 0, inner, 0, 1, 0, false).unwrap();
    // 再放一件留在背包里（不应被销毁）
    inv.add_item(ammo, 5, 0, backpack, 0, 3, 3, false).unwrap();

    assert_eq!(inv.item_count(), 4);
    assert_eq!(inv.container_count(), 2);

    let destroyed = inv.destroy_item(bag_item).unwrap();
    assert_eq!(destroyed, 3, "套包 + 内部 2 件 = 3");
    assert_eq!(inv.item_count(), 1);
    assert_eq!(inv.container_count(), 1, "套包内部容器应一并清掉");
    assert!(inv.container(inner).is_err(), "容器记录已删除");
    assert!(inv.item(bag_item).is_err(), "物品记录已删除");
    assert!(inv.check_invariants().is_empty());
}

// ==================== 6. root_type_of / depth_of 四层链路 ====================

#[test]
fn root_type_and_depth_four_level_chain() {
    let mut inv = Inventory::new();
    let bag = inv
        .define_item(
            ItemDef::new("包", 1, 1, 1).as_container(ContainerSpec::rectangular(4, 4)),
        )
        .unwrap();
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();

    let b2 = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let c2 = inv.item(b2).unwrap().container.unwrap();
    let b3 = inv.add_item(bag, 1, 0, c2, 0, 0, 0, false).unwrap();
    let c3 = inv.item(b3).unwrap().container.unwrap();
    let b4 = inv.add_item(bag, 1, 0, c3, 0, 0, 0, false).unwrap();
    let c4 = inv.item(b4).unwrap().container.unwrap();

    assert_eq!(inv.root_type_of(root).unwrap(), ContainerType::Backpack);
    assert_eq!(inv.root_type_of(c2).unwrap(), ContainerType::Backpack);
    assert_eq!(inv.root_type_of(c4).unwrap(), ContainerType::Backpack);

    assert_eq!(inv.depth_of(root).unwrap(), 1);
    assert_eq!(inv.depth_of(c2).unwrap(), 2);
    assert_eq!(inv.depth_of(c3).unwrap(), 3);
    assert_eq!(inv.depth_of(c4).unwrap(), 4);

    // 第 5 层被拒
    assert_eq!(
        inv.add_item(bag, 1, 0, c4, 0, 0, 0, false).unwrap_err(),
        InventoryError::NestingTooDeep
    );
    assert!(inv.check_invariants().is_empty());
}

// ==================== 7. 容器只读访问 ====================

#[test]
fn container_accessors_and_stats() {
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("弹药", 1, 1, 60)).unwrap();
    let (backpack, _pockets) = setup_roots(&mut inv);

    inv.add_item(ammo, 10, 0, backpack, 0, 0, 0, false).unwrap();

    let ct = inv.container(backpack).unwrap();
    assert_eq!(ct.label(), "背包");
    assert_eq!(ct.ctype(), ContainerType::Backpack);
    assert_eq!(ct.kind(), ContainerKind::Rectangular);
    assert_eq!(ct.part_count(), 1);
    assert_eq!(ct.dims(), vec![(4, 4)]);
    assert_eq!(ct.cell_stats(), (16, 1));

    let bogus = ContainerKey::from_u64(0);
    assert!(matches!(inv.container(bogus), Err(InventoryError::ContainerNotFound)));
    assert!(inv.check_invariants().is_empty());
}
