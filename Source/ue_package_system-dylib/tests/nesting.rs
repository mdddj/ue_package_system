//! 套包规则测试：四层链路、成环（直接 / A→B→A / 外层进深处）、
//! 深度上限与子树高度、黑名单按根类型判定、嵌套分析函数直接断言。
//! 全部失败路径断言状态字节不变。

use ue_package_system_dylib::algo::nesting;
use ue_package_system_dylib::*;

fn bytes(inv: &Inventory) -> Vec<u8> {
    inv.snapshot().encode().unwrap()
}

/// 1x1 的“包”，内部 4x4。
fn bag_def(inv: &mut Inventory) -> DefId {
    inv.define_item(ItemDef::new("包", 1, 1, 1).as_container(ContainerSpec::rectangular(4, 4)))
        .unwrap()
}

// ==================== 1. 四层链路 ====================

#[test]
fn backpack_in_backpack_four_levels_allowed() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();

    let b2 = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let c2 = inv.item(b2).unwrap().container.unwrap();
    let b3 = inv.add_item(bag, 1, 0, c2, 0, 0, 0, false).unwrap();
    let c3 = inv.item(b3).unwrap().container.unwrap();
    let b4 = inv.add_item(bag, 1, 0, c3, 0, 0, 0, false).unwrap();
    let c4 = inv.item(b4).unwrap().container.unwrap();

    assert_eq!(inv.depth_of(c2).unwrap(), 2);
    assert_eq!(inv.depth_of(c3).unwrap(), 3);
    assert_eq!(inv.depth_of(c4).unwrap(), 4, "四层链路允许");
    assert!(inv.check_invariants().is_empty());

    // 第 5 层被拒
    assert_eq!(
        inv.add_item(bag, 1, 0, c4, 0, 0, 0, false).unwrap_err(),
        InventoryError::NestingTooDeep
    );
    assert!(inv.check_invariants().is_empty());
}

// ==================== 2. 成环 ====================

#[test]
fn direct_self_nesting_is_cycle() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let b = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let inner = inv.item(b).unwrap().container.unwrap();
    let before = bytes(&inv);

    assert_eq!(
        inv.try_move(b, inner, 0, 0, 0, false).unwrap_err(),
        InventoryError::WouldCycle
    );
    assert_eq!(bytes(&inv), before);
}

#[test]
fn a_to_b_to_a_is_cycle() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let a = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let ca = inv.item(a).unwrap().container.unwrap();
    let b = inv.add_item(bag, 1, 0, ca, 0, 0, 0, false).unwrap();
    let cb = inv.item(b).unwrap().container.unwrap();
    let before = bytes(&inv);

    // A → B，再想 B → A：把 A 放进 B 的内部会成环
    assert_eq!(
        inv.try_move(a, cb, 0, 0, 0, false).unwrap_err(),
        InventoryError::WouldCycle
    );
    assert_eq!(bytes(&inv), before);
    assert!(inv.check_invariants().is_empty());
}

#[test]
fn outer_into_deep_interior_is_cycle() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let a = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let ca = inv.item(a).unwrap().container.unwrap();
    let b = inv.add_item(bag, 1, 0, ca, 0, 0, 0, false).unwrap();
    let cb = inv.item(b).unwrap().container.unwrap();
    let c = inv.add_item(bag, 1, 0, cb, 0, 0, 0, false).unwrap();
    let cc = inv.item(c).unwrap().container.unwrap();
    let before = bytes(&inv);

    // 把最外层包 A 塞进它自己的深处（第 3 层容器 cc）
    assert_eq!(
        inv.try_move(a, cc, 0, 0, 0, false).unwrap_err(),
        InventoryError::WouldCycle
    );
    assert_eq!(bytes(&inv), before);
    assert!(inv.check_invariants().is_empty());
}

// ==================== 3. 深度上限 ====================

#[test]
fn max_depth_two_rejects_third_level_then_allows() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    inv.set_rules(Rules::default().with_max_depth(2));
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let b2 = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let c2 = inv.item(b2).unwrap().container.unwrap();
    assert_eq!(inv.depth_of(c2).unwrap(), 2);

    let before = bytes(&inv);
    assert_eq!(
        inv.add_item(bag, 1, 0, c2, 0, 0, 0, false).unwrap_err(),
        InventoryError::NestingTooDeep
    );
    assert_eq!(bytes(&inv), before);

    // 改回 4 后可放入
    inv.set_rules(Rules::default().with_max_depth(4));
    assert!(inv.add_item(bag, 1, 0, c2, 0, 0, 0, false).is_ok());
    assert!(inv.check_invariants().is_empty());
}

// ==================== 4. 子树高度计入 ====================

#[test]
fn subtree_height_counts_not_just_depth_plus_one() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(6, 6)])
        .unwrap();

    // P：目标容器，深度 2（空）
    let p = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let cp = inv.item(p).unwrap().container.unwrap();

    // M：已含 2 层子树（M>N>O），M 的容器子树高度 = 3
    let m = inv.add_item(bag, 1, 0, root, 0, 1, 0, false).unwrap();
    let cm = inv.item(m).unwrap().container.unwrap();
    let n = inv.add_item(bag, 1, 0, cm, 0, 0, 0, false).unwrap();
    let cn = inv.item(n).unwrap().container.unwrap();
    let o = inv.add_item(bag, 1, 0, cn, 0, 0, 0, false).unwrap();
    let _co = inv.item(o).unwrap().container.unwrap();

    assert_eq!(nesting::subtree_height(&inv, cm), 3);
    assert_eq!(inv.depth_of(cp).unwrap(), 2);

    // 放进深度 2 的容器：2 + 3 = 5 > 4 → 超限（不是简单的 2+1=3）
    let before = bytes(&inv);
    assert_eq!(
        inv.try_move(m, cp, 0, 0, 0, false).unwrap_err(),
        InventoryError::NestingTooDeep
    );
    assert_eq!(bytes(&inv), before);
    assert!(inv.check_invariants().is_empty());
}

// ==================== 5. 黑名单按根类型判定 ====================

#[test]
fn blacklist_uses_root_type() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    // 大金：禁止进安全箱
    let gold = inv
        .define_item(
            ItemDef::new("大金", 1, 1, 1)
                .value(9999)
                .forbid(ContainerType::SafeBox.bit()),
        )
        .unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let safe = inv
        .add_root(ContainerType::SafeBox, "安全箱", &[(4, 4)])
        .unwrap();

    // 进背包 OK
    let g1 = inv.add_item(gold, 1, 0, backpack, 0, 0, 0, false).unwrap();
    // 进“背包里的包” OK（根类型仍是背包）
    let b_in_bag = inv.add_item(bag, 1, 0, backpack, 0, 0, 1, false).unwrap();
    let inner_backpack = inv.item(b_in_bag).unwrap().container.unwrap();
    assert!(inv.add_item(gold, 1, 0, inner_backpack, 0, 0, 0, false).is_ok());

    // 进安全箱 → 拒绝
    let before = bytes(&inv);
    assert_eq!(
        inv.try_move(g1, safe, 0, 0, 0, false).unwrap_err(),
        InventoryError::ForbiddenContainer
    );
    assert_eq!(bytes(&inv), before);

    // 进“安全箱里的包” → 根类型是安全箱，同样拒绝
    let b_in_safe = inv.add_item(bag, 1, 0, safe, 0, 0, 0, false).unwrap();
    let inner_safe = inv.item(b_in_safe).unwrap().container.unwrap();
    assert_eq!(
        inv.add_item(gold, 1, 0, inner_safe, 0, 0, 0, false).unwrap_err(),
        InventoryError::ForbiddenContainer
    );
    assert!(inv.check_invariants().is_empty());
}

// ==================== 6. 嵌套分析函数直接断言 ====================

#[test]
fn nesting_analysis_functions() {
    let mut inv = Inventory::new();
    let bag = bag_def(&mut inv);
    let root = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();

    let a = inv.add_item(bag, 1, 0, root, 0, 0, 0, false).unwrap();
    let ca = inv.item(a).unwrap().container.unwrap();
    let b = inv.add_item(bag, 1, 0, ca, 0, 0, 0, false).unwrap();
    let cb = inv.item(b).unwrap().container.unwrap();

    // child_containers
    assert_eq!(nesting::child_containers(&inv, root), vec![ca]);
    assert_eq!(nesting::child_containers(&inv, ca), vec![cb]);
    assert_eq!(nesting::child_containers(&inv, cb), Vec::new());
    // Inventory 上的方法应一致
    assert_eq!(inv.child_containers(root), vec![ca]);

    // subtree_contains（含自身）
    assert!(nesting::subtree_contains(&inv, root, cb));
    assert!(nesting::subtree_contains(&inv, ca, ca));
    assert!(!nesting::subtree_contains(&inv, ca, root));
    assert!(!nesting::subtree_contains(&inv, cb, ca));

    // subtree_height：root 子树高度 3，逐层递减
    assert_eq!(nesting::subtree_height(&inv, root), 3);
    assert_eq!(nesting::subtree_height(&inv, ca), 2);
    assert_eq!(nesting::subtree_height(&inv, cb), 1);

    // reachable_containers 覆盖全部
    assert_eq!(nesting::reachable_containers(&inv).len(), 3);
    assert_eq!(inv.all_containers().len(), 3);
    assert!(inv.check_invariants().is_empty());
}
