//! 原子移动 / 互换（核心验收）。
//!
//! 约定：所有失败路径都必须保证状态一字节不变（用快照编码后的字节比较）。
//! 每成功一步后校验 `check_invariants()` 为空。

use ue_package_system_dylib::*;

/// 状态字节指纹：失败路径断言“快照字节完全一致”。
fn bytes(inv: &Inventory) -> Vec<u8> {
    inv.snapshot().encode().unwrap()
}

struct World {
    inv: Inventory,
    ammo: DefId,
    bag: DefId,
    backpack: ContainerKey,
    rig: ContainerKey,
    safe: ContainerKey,
    pockets: ContainerKey,
}

/// 标准沙盒：背包 4x4、弹挂 (1x1)+(2x2)、安全箱 2x2、口袋 2x1。
fn world() -> World {
    let mut inv = Inventory::new();
    let ammo = inv
        .define_item(ItemDef::new("弹药", 1, 1, 60).weight(0.01).value(2).tags(tags::AMMO))
        .unwrap();
    let bag = inv
        .define_item(
            ItemDef::new("背包", 2, 2, 1)
                .weight(2.0)
                .as_container(ContainerSpec::rectangular(4, 4)),
        )
        .unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let rig = inv
        .add_root(ContainerType::ChestRig, "弹挂", &[(1, 1), (2, 2)])
        .unwrap();
    let safe = inv
        .add_root(ContainerType::SafeBox, "安全箱", &[(2, 2)])
        .unwrap();
    let pockets = inv
        .add_root(ContainerType::Pockets, "口袋", &[(2, 1)])
        .unwrap();
    World {
        inv,
        ammo,
        bag,
        backpack,
        rig,
        safe,
        pockets,
    }
}

// ==================== try_move 成功 ====================

#[test]
fn move_cross_container() {
    let mut w = world();
    let item = w.inv.add_item(w.ammo, 20, 0, w.pockets, 0, 0, 0, false).unwrap();
    w.inv.try_move(item, w.backpack, 0, 0, 0, false).unwrap();
    let (c, p, m) = w.inv.container_of_item(item).unwrap();
    assert_eq!((c, p, m.x, m.y), (w.backpack, 0, 0, 0));
    assert!(w.inv.container(w.pockets).unwrap().cell_stats().1 == 0);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn move_same_grid_overlapping_self() {
    let mut w = world();
    let bag = w.inv.add_item(w.bag, 1, 0, w.backpack, 0, 0, 0, false).unwrap(); // 2x2 在 (0,0)
    // 挪到 (1,1)：新旧占位在 (1,1) 重叠，但自己不算障碍
    w.inv.try_move(bag, w.backpack, 0, 1, 1, false).unwrap();
    let (_, _, m) = w.inv.container_of_item(bag).unwrap();
    assert_eq!((m.x, m.y), (1, 1));
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn move_with_rotation() {
    let mut w = world();
    let ammo = w
        .inv
        .define_item(ItemDef::new("弹匣", 1, 2, 1).rotatable(true))
        .unwrap();
    let item = w.inv.add_item(ammo, 1, 0, w.backpack, 0, 0, 0, false).unwrap();
    // 旋转着挪：1x2 → 2x1，占 (2,0)(3,0)
    w.inv.try_move(item, w.backpack, 0, 2, 0, true).unwrap();
    let (_, _, m) = w.inv.container_of_item(item).unwrap();
    assert!(m.rotated);
    assert_eq!(m.footprint(), (2, 1));
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn move_into_chest_rig_part() {
    let mut w = world();
    let item = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 0, 0, false).unwrap();
    // 挪进弹挂的第 2 块子网格（2x2）
    w.inv.try_move(item, w.rig, 1, 1, 1, false).unwrap();
    let (c, p, m) = w.inv.container_of_item(item).unwrap();
    assert_eq!((c, p, m.x, m.y), (w.rig, 1, 1, 1));
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn move_noop_returns_ok_and_unchanged() {
    let mut w = world();
    let item = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 2, 2, false).unwrap();
    let before = bytes(&w.inv);
    // 挪到原地：成功且状态不变
    w.inv.try_move(item, w.backpack, 0, 2, 2, false).unwrap();
    assert_eq!(bytes(&w.inv), before);
    assert!(w.inv.check_invariants().is_empty());
}

// ==================== try_move 失败（状态必须零变化） ====================

#[test]
fn move_fail_out_of_bounds() {
    let mut w = world();
    let item = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);
    assert_eq!(
        w.inv.try_move(item, w.backpack, 0, 4, 0, false).unwrap_err(),
        InventoryError::OutOfBounds
    );
    assert_eq!(bytes(&w.inv), before);
}

#[test]
fn move_fail_occupied() {
    let mut w = world();
    let a = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 0, 0, false).unwrap();
    let b = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 1, 0, false).unwrap();
    let before = bytes(&w.inv);
    assert_eq!(
        w.inv.try_move(b, w.backpack, 0, 0, 0, false).unwrap_err(),
        InventoryError::Occupied
    );
    assert_eq!(bytes(&w.inv), before);
    // a 仍在原地
    let (_, _, m) = w.inv.container_of_item(a).unwrap();
    assert_eq!((m.x, m.y), (0, 0));
}

#[test]
fn move_fail_cycle() {
    let mut w = world();
    let bag = w.inv.add_item(w.bag, 1, 0, w.backpack, 0, 0, 0, false).unwrap();
    let inner = w.inv.item(bag).unwrap().container.unwrap();
    w.inv.add_item(w.ammo, 5, 0, inner, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);
    // 把套包塞进它自己的内部
    assert_eq!(
        w.inv.try_move(bag, inner, 0, 0, 0, false).unwrap_err(),
        InventoryError::WouldCycle
    );
    assert_eq!(bytes(&w.inv), before);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn move_fail_depth() {
    let mut w = world();
    w.inv.set_rules(Rules::default().with_max_depth(2));
    let bag2 = w.inv.add_item(w.bag, 1, 0, w.backpack, 0, 0, 0, false).unwrap();
    let c2 = w.inv.item(bag2).unwrap().container.unwrap();
    let bag3 = w.inv.add_item(w.bag, 1, 0, w.backpack, 0, 2, 0, false).unwrap();
    assert_eq!(w.inv.depth_of(c2).unwrap(), 2);
    let before = bytes(&w.inv);
    // 放进已处于第 2 层的容器 → 第 3 层超限
    assert_eq!(
        w.inv.try_move(bag3, c2, 0, 0, 0, false).unwrap_err(),
        InventoryError::NestingTooDeep
    );
    assert_eq!(bytes(&w.inv), before);
}

#[test]
fn move_fail_forbidden_container() {
    let mut w = world();
    // 大金：禁止进安全箱
    let gold = w
        .inv
        .define_item(
            ItemDef::new("大金", 1, 1, 1)
                .value(9999)
                .forbid(ContainerType::SafeBox.bit()),
        )
        .unwrap();
    let item = w.inv.add_item(gold, 1, 0, w.backpack, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);
    assert_eq!(
        w.inv.try_move(item, w.safe, 0, 0, 0, false).unwrap_err(),
        InventoryError::ForbiddenContainer
    );
    assert_eq!(bytes(&w.inv), before);
    // 进背包没问题
    w.inv.try_move(item, w.backpack, 0, 3, 3, false).unwrap();
    assert!(w.inv.check_invariants().is_empty());
}

// ==================== try_swap 成功 ====================

#[test]
fn swap_same_grid_different_sizes() {
    let mut w = world();
    let a = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 0, 0, false).unwrap();
    let b = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 2, 0, false).unwrap();
    w.inv.try_swap(a, b).unwrap();
    assert_eq!(w.inv.container_of_item(a).unwrap().2.x, 2);
    assert_eq!(w.inv.container_of_item(b).unwrap().2.x, 0);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn swap_cross_container() {
    let mut w = world();
    let a = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 1, 1, false).unwrap();
    let b = w.inv.add_item(w.ammo, 5, 0, w.pockets, 0, 0, 0, false).unwrap();
    w.inv.try_swap(a, b).unwrap();
    let (ca, _, ma) = w.inv.container_of_item(a).unwrap();
    let (cb, _, mb) = w.inv.container_of_item(b).unwrap();
    assert_eq!((ca, ma.x, ma.y), (w.pockets, 0, 0));
    assert_eq!((cb, mb.x, mb.y), (w.backpack, 1, 1));
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn swap_needs_flip() {
    // 3x3：a 横放 2x1 在 (0,0)，b 竖放 1x2 在 (2,0)。
    // 互换后 a 必须翻回竖放才放得下 b 的位置。
    let mut inv = Inventory::new();
    let d = inv
        .define_item(ItemDef::new("板", 1, 2, 1).rotatable(true))
        .unwrap();
    let g = inv
        .add_root(ContainerType::Backpack, "背包", &[(3, 3)])
        .unwrap();
    let a = inv.add_item(d, 1, 0, g, 0, 0, 0, true).unwrap(); // 旋转 → 2x1 占 (0,0)(1,0)
    let b = inv.add_item(d, 1, 0, g, 0, 2, 0, false).unwrap(); // 竖 1x2 占 (2,0)(2,1)

    inv.try_swap(a, b).unwrap();
    let (_, _, ma) = inv.container_of_item(a).unwrap();
    let (_, _, mb) = inv.container_of_item(b).unwrap();
    // a 到 (2,0)：2x1 会越界，翻转成 1x2 才放得下
    assert_eq!((ma.x, ma.y, ma.rotated), (2, 0, false));
    // b 到 (0,0)：竖 1x2 原朝向即可
    assert_eq!((mb.x, mb.y, mb.rotated), (0, 0, false));
    assert!(inv.check_invariants().is_empty());
}

// ==================== try_swap 失败 ====================

#[test]
fn swap_fail_target_does_not_fit() {
    let mut w = world();
    let a = w.inv.add_item(w.bag, 1, 0, w.backpack, 0, 0, 0, false).unwrap(); // 2x2
    // 口袋只有 2x1，放不下 a
    let b = w.inv.add_item(w.ammo, 5, 0, w.pockets, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);
    assert!(w.inv.try_swap(a, b).is_err());
    assert_eq!(bytes(&w.inv), before);
}

#[test]
fn swap_fail_same_item() {
    let mut w = world();
    let a = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);
    assert_eq!(w.inv.try_swap(a, a).unwrap_err(), InventoryError::InvalidArgument);
    assert_eq!(bytes(&w.inv), before);
}

#[test]
fn swap_fail_would_cycle() {
    let mut w = world();
    let bag = w.inv.add_item(w.bag, 1, 0, w.backpack, 0, 0, 0, false).unwrap();
    let inner = w.inv.item(bag).unwrap().container.unwrap();
    let child = w.inv.add_item(w.ammo, 5, 0, inner, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);
    // 互换外层套包与其内部物品 → 成环
    assert_eq!(w.inv.try_swap(bag, child).unwrap_err(), InventoryError::WouldCycle);
    assert_eq!(bytes(&w.inv), before);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn swap_same_grid_occupied_fails_unchanged() {
    let mut w = world();
    // a=2x2 在 (0,0)，b=1x1 在 (2,0)，c 挡在 a 的目标区域 (3,1)
    let d = w
        .inv
        .define_item(ItemDef::new("大包", 2, 2, 1).as_container(ContainerSpec::rectangular(2, 2)))
        .unwrap();
    let a = w.inv.add_item(d, 1, 0, w.backpack, 0, 0, 0, false).unwrap();
    let b = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 2, 0, false).unwrap();
    let c = w.inv.add_item(w.ammo, 5, 0, w.backpack, 0, 3, 1, false).unwrap();
    let _ = c;
    let before = bytes(&w.inv);
    // a 换到 b 的位置 (2,0) 需要 (3,1)，被 c 挡住 → 失败且状态不变
    assert_eq!(
        w.inv.try_swap(a, b).unwrap_err(),
        InventoryError::Occupied
    );
    assert_eq!(bytes(&w.inv), before);
    assert!(w.inv.check_invariants().is_empty());
}
