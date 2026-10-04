//! `algo::autosort` 与 `Inventory::autosort` 的集成测试。
//!
//! 关注点：
//! - 整理前后物品集合与堆叠数量守恒；
//! - 整理结果确定（同状态两次整理字节一致）；
//! - 复合容器（弹挂）多 part 重排；
//! - 失败路径零副作用（`SortFailed` 时原布局不动）；
//! - 纯函数 `plan` 的空输入 / 放不下的分支。

use std::collections::BTreeMap;

use ue_package_system_dylib::algo::autosort::{plan, SortItem};
use ue_package_system_dylib::{
    ContainerKey, ContainerSpec, ContainerType, Inventory, ItemDef, ItemKey,
};

/// 当前全库快照字节（确定性比较用）。
fn bytes(inv: &Inventory) -> Vec<u8> {
    inv.snapshot().encode().unwrap()
}

/// `key -> 堆叠数量`（物品身份用 u64 编码，跨 snapshot 稳定）。
fn key_stacks(inv: &Inventory) -> BTreeMap<u64, u32> {
    inv.items().map(|(k, i)| (k.to_u64(), i.stack)).collect()
}

/// 断言每个落位物品在几何上合法（自身矩形可重新放置）。
fn assert_layout_legal(inv: &Inventory) {
    for ck in inv.all_containers() {
        let ct = inv.container(ck).unwrap();
        for (pi, part) in ct.parts().iter().enumerate() {
            for (key, meta) in part.occupants() {
                let (w, h) = meta.footprint();
                assert!(
                    part.can_place_excluding(meta.x, meta.y, w, h, &[key]),
                    "容器 {ck:?} part {pi} 物品 {key:?} 落位非法"
                );
            }
        }
    }
}

/// 搭一个碎片化的 5x5 背包：三种尺寸乱塞。
fn fragmented() -> (Inventory, ContainerKey) {
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("子弹", 1, 1, 60)).unwrap();
    let rifle = inv.define_item(ItemDef::new("步枪", 2, 4, 1).rotatable(true)).unwrap();
    let crate25 = inv.define_item(ItemDef::new("药箱", 2, 2, 1)).unwrap();
    let bp = inv.add_root(ContainerType::Backpack, "背包", &[(5, 5)]).unwrap();

    // 故意制造空洞：2x4 靠右、2x2 靠下、1x1 顶角。
    inv.add_item(ammo, 60, 0, bp, 0, 0, 0, false).unwrap();
    inv.add_item(rifle, 1, 0, bp, 0, 3, 0, false).unwrap();
    inv.add_item(crate25, 1, 0, bp, 0, 0, 3, false).unwrap();
    inv.add_item(ammo, 30, 0, bp, 0, 1, 0, false).unwrap();
    (inv, bp)
}

#[test]
fn fragmented_backpack_conserves_items() {
    let (mut inv, bp) = fragmented();
    let before = key_stacks(&inv);

    inv.autosort(bp).expect("碎片背包应当能整理");

    assert_eq!(key_stacks(&inv), before, "整理后物品集合 / 堆叠数量必须不变");
    assert_layout_legal(&inv);
    assert!(inv.check_invariants().is_empty(), "{:?}", inv.check_invariants());
}

#[test]
fn autosort_is_deterministic() {
    let (mut inv, bp) = fragmented();
    let s0 = inv.snapshot();

    inv.autosort(bp).unwrap();
    let s1 = bytes(&inv);

    // 回到整理前，再整理一次，结果字节必须完全一致。
    inv.restore(&s0).unwrap();
    inv.autosort(bp).unwrap();
    let s2 = bytes(&inv);

    assert_eq!(s1, s2, "同一状态连跑两次 autosort，结果必须完全一致");
    assert!(inv.check_invariants().is_empty());
}

#[test]
fn compound_rig_reorders_across_parts() {
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("子弹", 1, 1, 60)).unwrap();
    let flare = inv.define_item(ItemDef::new("信号弹", 1, 2, 4)).unwrap();
    let rig = inv
        .add_root(ContainerType::ChestRig, "弹挂", &[(2, 2), (2, 2), (3, 1)])
        .unwrap();

    let before_n = inv.item_count();
    inv.add_item(ammo, 60, 0, rig, 0, 0, 0, false).unwrap();
    inv.add_item(ammo, 30, 0, rig, 0, 1, 0, false).unwrap();
    inv.add_item(flare, 4, 0, rig, 1, 0, 0, false).unwrap();
    inv.add_item(flare, 2, 0, rig, 1, 1, 0, false).unwrap();
    let stacks_before = key_stacks(&inv);

    inv.autosort(rig).expect("复合容器应能整理");

    assert_eq!(key_stacks(&inv), stacks_before, "跨 part 重排不得丢物品 / 改数量");
    assert_eq!(inv.item_count(), before_n + 4);
    assert_layout_legal(&inv);
    assert!(inv.check_invariants().is_empty());
}

#[test]
fn empty_and_nested_sort_are_ok() {
    // 空容器。
    let mut inv = Inventory::new();
    let bp = inv.add_root(ContainerType::Backpack, "空背包", &[(4, 4)]).unwrap();
    assert!(inv.autosort(bp).is_ok(), "空容器整理必须 Ok");

    // 嵌套包内部容器。
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("子弹", 1, 1, 60)).unwrap();
    let bag = inv
        .define_item(
            ItemDef::new("小包", 2, 2, 1).as_container(ContainerSpec::rectangular(3, 3)),
        )
        .unwrap();
    let bp = inv.add_root(ContainerType::Backpack, "背包", &[(5, 5)]).unwrap();
    let bag_key = inv.add_item(bag, 1, 0, bp, 0, 0, 0, false).unwrap();
    let inner = inv.item(bag_key).unwrap().container.unwrap();
    inv.add_item(ammo, 60, 0, inner, 0, 0, 0, false).unwrap();
    inv.add_item(ammo, 20, 0, inner, 0, 2, 0, false).unwrap();

    assert!(inv.autosort(inner).is_ok(), "嵌套包内部容器整理必须 Ok");
    assert!(inv.check_invariants().is_empty());
}

#[test]
fn plan_success_and_failure_branches() {
    // 空输入 → Some(vec![])，不是“失败”。
    assert_eq!(plan(&[(3, 3)], &[]), Some(Vec::new()));

    // 放不下（且不可旋转）→ None。
    let too_big = SortItem {
        key: ItemKey::from_u64(1),
        w: 2,
        h: 3,
        rotatable: false,
        def: 0,
    };
    assert_eq!(plan(&[(2, 2)], &[too_big]), None);

    // 可旋转时能救回来 → Some，且落位合法。
    let rotatable = SortItem {
        key: ItemKey::from_u64(1),
        w: 3,
        h: 1,
        rotatable: true,
        def: 0,
    };
    let placed = plan(&[(1, 3)], &[rotatable]).expect("旋转后应放得下");
    assert_eq!(placed.len(), 1);
    assert!(placed[0].rotated, "只有旋转后才能放进 1x3");

    // 全输入放不下 → None。
    let a = SortItem { key: ItemKey::from_u64(1), w: 2, h: 2, rotatable: false, def: 0 };
    let b = SortItem { key: ItemKey::from_u64(2), w: 2, h: 2, rotatable: false, def: 0 };
    assert_eq!(plan(&[(2, 2)], &[a, b]), None);
}

/// `Inventory::autosort` 失败路径：要么 Ok（布局合法），要么 Err(SortFailed) 且字节不变。
#[test]
fn autosort_failure_is_atomic() {
    let mut inv = Inventory::new();
    let big = inv.define_item(ItemDef::new("大件", 2, 3, 1).rotatable(true)).unwrap();
    let small = inv.define_item(ItemDef::new("小件", 1, 1, 1)).unwrap();
    let bp = inv.add_root(ContainerType::Backpack, "背包", &[(3, 3)]).unwrap();
    inv.add_item(big, 1, 0, bp, 0, 0, 0, false).unwrap();
    inv.add_item(small, 1, 0, bp, 0, 2, 0, false).unwrap();
    inv.add_item(small, 1, 0, bp, 0, 2, 2, false).unwrap();

    let before = bytes(&inv);
    match inv.autosort(bp) {
        Ok(()) => {
            assert_layout_legal(&inv);
            assert!(inv.check_invariants().is_empty());
        }
        Err(e) => {
            assert_eq!(e, ue_package_system_dylib::InventoryError::SortFailed);
            assert_eq!(bytes(&inv), before, "SortFailed 时状态必须一个字节都不变");
        }
    }
}

/// 整理只在目标容器内重排；黑名单物品不会被挪去别的根容器。
#[test]
fn blacklisted_item_stays_put() {
    let mut inv = Inventory::new();
    let gold = inv
        .define_item(
            ItemDef::new("大金表", 1, 1, 1)
                .value(5000)
                .forbid(ContainerType::SafeBox.bit()),
        )
        .unwrap();
    let ammo = inv.define_item(ItemDef::new("子弹", 1, 1, 60)).unwrap();
    let bp = inv.add_root(ContainerType::Backpack, "背包", &[(4, 4)]).unwrap();
    let safe = inv.add_root(ContainerType::SafeBox, "安全箱", &[(2, 2)]).unwrap();

    let gold_key = inv.add_item(gold, 1, 0, bp, 0, 0, 0, false).unwrap();
    inv.add_item(ammo, 60, 0, bp, 0, 2, 0, false).unwrap();

    inv.autosort(bp).unwrap();

    let (host, _, _) = inv.container_of_item(gold_key).expect("大金仍在背包里");
    assert_eq!(host, bp, "黑名单物品不得被整理挪出原容器");
    assert_ne!(host, safe);
    assert!(inv.check_invariants().is_empty());
}
