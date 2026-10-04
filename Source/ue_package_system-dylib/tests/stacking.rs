//! 堆叠测试：合并（部分 / 恰好填满 / 目标满 / 定义不同 / 不可堆叠 /
//! 源清空销毁）与拆分（数量校验 / 耐久继承 / 同网格落位 / 禁止容器）。
//! 所有失败路径断言状态字节不变。

use ue_package_system_dylib::*;

fn bytes(inv: &Inventory) -> Vec<u8> {
    inv.snapshot().encode().unwrap()
}

struct World {
    inv: Inventory,
    ammo: DefId,
    field: ContainerKey,
    safe: ContainerKey,
}

/// 背包 4x4 + 安全箱 2x2；弹药可堆叠到 60。
fn world() -> World {
    let mut inv = Inventory::new();
    let ammo = inv
        .define_item(ItemDef::new("弹药", 1, 1, 60).weight(0.01).value(2).tags(tags::AMMO))
        .unwrap();
    let field = inv
        .add_root(ContainerType::Backpack, "背包", &[(4, 4)])
        .unwrap();
    let safe = inv
        .add_root(ContainerType::SafeBox, "安全箱", &[(2, 2)])
        .unwrap();
    World {
        inv,
        ammo,
        field,
        safe,
    }
}

// ==================== merge ====================

#[test]
fn merge_partial_transfer_returns_actual() {
    let mut w = world();
    let src = w.inv.add_item(w.ammo, 20, 0, w.field, 0, 0, 0, false).unwrap();
    let dst = w.inv.add_item(w.ammo, 50, 0, w.field, 0, 1, 0, false).unwrap();

    let moved = w.inv.try_merge(src, dst).unwrap();
    assert_eq!(moved, 10, "只转移了填满目标的 10 发");
    assert_eq!(w.inv.item(dst).unwrap().stack, 60);
    assert_eq!(w.inv.item(src).unwrap().stack, 10, "源还剩余量");
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn merge_exactly_fills_and_destroys_src() {
    let mut w = world();
    let src = w.inv.add_item(w.ammo, 10, 0, w.field, 0, 0, 0, false).unwrap();
    let dst = w.inv.add_item(w.ammo, 50, 0, w.field, 0, 1, 0, false).unwrap();
    assert_eq!(w.inv.item_count(), 2);

    let moved = w.inv.try_merge(src, dst).unwrap();
    assert_eq!(moved, 10);
    assert_eq!(w.inv.item(dst).unwrap().stack, 60);
    assert!(w.inv.item(src).is_err(), "源被清空后应销毁");
    assert_eq!(w.inv.item_count(), 1);
    // 源原来的格子空出
    assert!(w.inv.container(w.field).unwrap().part(0).unwrap().cell(0, 0).is_none());
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn merge_target_full_is_stack_full_and_unchanged() {
    let mut w = world();
    let src = w.inv.add_item(w.ammo, 5, 0, w.field, 0, 0, 0, false).unwrap();
    let dst = w.inv.add_item(w.ammo, 60, 0, w.field, 0, 1, 0, false).unwrap();
    let before = bytes(&w.inv);

    assert_eq!(w.inv.try_merge(src, dst).unwrap_err(), InventoryError::StackFull);
    assert_eq!(bytes(&w.inv), before);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn merge_different_defs_unmergeable() {
    let mut w = world();
    let ammo2 = w.inv.define_item(ItemDef::new("弹药2", 1, 1, 60)).unwrap();
    let src = w.inv.add_item(w.ammo, 5, 0, w.field, 0, 0, 0, false).unwrap();
    let dst = w.inv.add_item(ammo2, 5, 0, w.field, 0, 1, 0, false).unwrap();
    let before = bytes(&w.inv);

    assert_eq!(
        w.inv.try_merge(src, dst).unwrap_err(),
        InventoryError::StackUnmergeable
    );
    assert_eq!(bytes(&w.inv), before);
}

#[test]
fn merge_non_stackable_unmergeable() {
    let mut w = world();
    let gun = w
        .inv
        .define_item(ItemDef::new("枪", 2, 1, 1))
        .unwrap();
    let src = w.inv.add_item(gun, 1, 0, w.field, 0, 0, 0, false).unwrap();
    let dst = w.inv.add_item(gun, 1, 0, w.field, 0, 0, 1, false).unwrap();
    let before = bytes(&w.inv);

    assert_eq!(
        w.inv.try_merge(src, dst).unwrap_err(),
        InventoryError::StackUnmergeable
    );
    assert_eq!(bytes(&w.inv), before);
}

#[test]
fn merge_same_item_is_invalid_argument() {
    let mut w = world();
    let a = w.inv.add_item(w.ammo, 5, 0, w.field, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);
    assert_eq!(
        w.inv.try_merge(a, a).unwrap_err(),
        InventoryError::InvalidArgument
    );
    assert_eq!(bytes(&w.inv), before);
}

// ==================== split ====================

#[test]
fn split_success_inherits_durability() {
    let mut w = world();
    let src = w
        .inv
        .add_item(w.ammo, 60, 42, w.field, 0, 0, 0, false)
        .unwrap();
    // 拆到同一网格的非重叠位置
    let new_item = w.inv.try_split(src, 20, w.field, 0, 1, 0, false).unwrap();

    assert_eq!(w.inv.item(new_item).unwrap().stack, 20);
    assert_eq!(w.inv.item(new_item).unwrap().durability, 42, "耐久继承");
    assert_eq!(w.inv.item(src).unwrap().stack, 40, "原堆叠减少");
    assert_eq!(w.inv.item_count(), 2);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn split_count_zero_and_full_are_invalid() {
    let mut w = world();
    let src = w.inv.add_item(w.ammo, 30, 0, w.field, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);

    assert_eq!(
        w.inv.try_split(src, 0, w.field, 0, 1, 0, false).unwrap_err(),
        InventoryError::InvalidStackCount
    );
    // count == 全部数量：应改用 try_move
    assert_eq!(
        w.inv.try_split(src, 30, w.field, 0, 2, 0, false).unwrap_err(),
        InventoryError::InvalidStackCount
    );
    assert_eq!(bytes(&w.inv), before);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn split_into_forbidden_container_fails() {
    let mut w = world();
    // 大金：禁止进安全箱，但可堆叠
    let gold = w
        .inv
        .define_item(
            ItemDef::new("金币", 1, 1, 60)
                .forbid(ContainerType::SafeBox.bit()),
        )
        .unwrap();
    let src = w.inv.add_item(gold, 30, 0, w.field, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);

    assert_eq!(
        w.inv.try_split(src, 5, w.safe, 0, 0, 0, false).unwrap_err(),
        InventoryError::ForbiddenContainer
    );
    assert_eq!(bytes(&w.inv), before);
    assert!(w.inv.check_invariants().is_empty());
}

#[test]
fn split_non_stackable_unmergeable() {
    let mut w = world();
    let gun = w.inv.define_item(ItemDef::new("枪", 1, 1, 1)).unwrap();
    let src = w.inv.add_item(gun, 1, 0, w.field, 0, 0, 0, false).unwrap();
    let before = bytes(&w.inv);

    assert_eq!(
        w.inv.try_split(src, 1, w.field, 0, 1, 0, false).unwrap_err(),
        InventoryError::StackUnmergeable
    );
    assert_eq!(bytes(&w.inv), before);
}
