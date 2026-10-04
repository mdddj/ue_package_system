//! 快照（bincode 存档 / 网络包）集成测试。
//!
//! 覆盖：往返保真、编码确定性、典型装载体积门禁（≤ 2 KiB）、
//! 损坏数据拒绝、手工构造非法快照的校验。

use ue_package_system_dylib::{
    tags, ContainerSnap, ContainerSpec, ContainerType, Inventory, ItemDef, ItemSnap, PlaceSnap,
    Snapshot,
};

fn encode(inv: &Inventory) -> Vec<u8> {
    inv.snapshot().encode().unwrap()
}

/// 复杂状态：4 根容器 + 套包 3 层 + 堆叠 + 旋转。
fn complex_state() -> Inventory {
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("子弹", 1, 1, 60).tags(tags::AMMO)).unwrap();
    let rifle = inv
        .define_item(ItemDef::new("步枪", 2, 4, 1).rotatable(true).weight(3.0).value(900))
        .unwrap();
    let med = inv.define_item(ItemDef::new("急救包", 1, 1, 3).tags(tags::MEDKIT)).unwrap();
    let gold = inv
        .define_item(ItemDef::new("大金", 2, 2, 1).forbid(ContainerType::SafeBox.bit()))
        .unwrap();
    let bag_big = inv
        .define_item(ItemDef::new("大包", 3, 3, 1).as_container(ContainerSpec::rectangular(6, 5)))
        .unwrap();
    let bag_mid = inv
        .define_item(ItemDef::new("中包", 2, 2, 1).as_container(ContainerSpec::rectangular(3, 3)))
        .unwrap();
    let bag_small = inv
        .define_item(ItemDef::new("小包", 1, 1, 1).as_container(ContainerSpec::rectangular(2, 2)))
        .unwrap();

    let pockets = inv.add_root(ContainerType::Pockets, "口袋", &[(1, 1), (2, 2)]).unwrap();
    let rig = inv.add_root(ContainerType::ChestRig, "弹挂", &[(1, 1), (1, 1), (2, 2)]).unwrap();
    let safe = inv.add_root(ContainerType::SafeBox, "安全箱", &[(4, 3)]).unwrap();
    let backpack = inv.add_root(ContainerType::Backpack, "背包", &[(6, 5)]).unwrap();

    inv.add_item(ammo, 60, 0, pockets, 1, 0, 0, false).unwrap();
    inv.add_item(med, 3, 0, rig, 2, 0, 0, false).unwrap();
    inv.add_item(med, 2, 0, safe, 0, 0, 0, false).unwrap();

    inv.add_item(rifle, 1, 0, backpack, 0, 0, 0, true).unwrap(); // 旋转态
    let bag_big_k = inv.add_item(bag_big, 1, 0, backpack, 0, 0, 2, false).unwrap();
    inv.add_item(gold, 1, 0, backpack, 0, 4, 0, false).unwrap();
    inv.add_item(ammo, 45, 0, backpack, 0, 4, 2, false).unwrap();

    let big_inner = inv.item(bag_big_k).unwrap().container.unwrap();
    let bag_mid_k = inv.add_item(bag_mid, 1, 0, big_inner, 0, 0, 0, false).unwrap();
    inv.add_item(ammo, 30, 0, big_inner, 0, 0, 3, false).unwrap();
    inv.add_item(med, 1, 0, big_inner, 0, 0, 4, false).unwrap();

    let mid_inner = inv.item(bag_mid_k).unwrap().container.unwrap();
    let bag_small_k = inv.add_item(bag_small, 1, 0, mid_inner, 0, 0, 0, false).unwrap();
    inv.add_item(ammo, 5, 0, mid_inner, 0, 0, 1, false).unwrap();

    let small_inner = inv.item(bag_small_k).unwrap().container.unwrap();
    inv.add_item(ammo, 10, 0, small_inner, 0, 0, 0, false).unwrap();
    inv.add_item(ammo, 7, 0, small_inner, 0, 0, 1, false).unwrap();
    inv
}

#[test]
fn roundtrip_preserves_complex_state() {
    let mut inv = complex_state();
    let snap = inv.snapshot();
    let bytes = snap.encode().unwrap();

    let decoded = Snapshot::decode(&bytes).unwrap();
    assert_eq!(decoded, snap, "编码 → 解码必须逐字段相等");

    inv.restore(&decoded).unwrap();
    assert_eq!(inv.snapshot(), snap, "恢复后再次导出必须与原快照相等");
    assert!(inv.check_invariants().is_empty(), "{:?}", inv.check_invariants());
}

#[test]
fn encoding_is_deterministic() {
    let inv = complex_state();
    assert_eq!(
        inv.snapshot().encode().unwrap(),
        inv.snapshot().encode().unwrap(),
        "同一状态两次编码必须字节一致"
    );
    let snap = inv.snapshot();
    assert_eq!(snap.encode().unwrap(), snap.encode().unwrap());
}

/// 典型装备装载：约 30~40 件物品，验证整包 ≤ 2 KiB。
#[test]
fn typical_loadout_fits_2kib() {
    let mut inv = Inventory::new();
    let ammo = inv
        .define_item(ItemDef::new("5.56 弹药", 1, 1, 60).weight(0.012).value(3).tags(tags::AMMO))
        .unwrap();
    let mag = inv
        .define_item(
            ItemDef::new("弹匣", 1, 2, 1).rotatable(true).weight(0.2).value(120).tags(tags::MAGAZINE),
        )
        .unwrap();
    let med = inv
        .define_item(ItemDef::new("急救包", 1, 1, 5).weight(0.4).value(300).tags(tags::MEDKIT))
        .unwrap();
    let gold = inv
        .define_item(ItemDef::new("大金表", 2, 2, 1).weight(1.0).value(5000).tags(tags::VALUABLE))
        .unwrap();
    let bag_a = inv
        .define_item(
            ItemDef::new("突击背包", 3, 3, 1)
                .weight(2.0)
                .value(900)
                .as_container(ContainerSpec::rectangular(6, 5)),
        )
        .unwrap();
    let bag_b = inv
        .define_item(
            ItemDef::new("小挎包", 2, 2, 1)
                .weight(1.0)
                .value(300)
                .as_container(ContainerSpec::rectangular(4, 4)),
        )
        .unwrap();

    let pockets = inv.add_root(ContainerType::Pockets, "口袋", &[(1, 1), (1, 1)]).unwrap();
    let rig = inv.add_root(ContainerType::ChestRig, "弹挂", &[(1, 1), (1, 1), (2, 2)]).unwrap();
    let safe = inv.add_root(ContainerType::SafeBox, "安全箱", &[(4, 3)]).unwrap();
    let backpack = inv.add_root(ContainerType::Backpack, "背包", &[(10, 8)]).unwrap();

    // 根容器里塞东西。
    inv.add_item(ammo, 60, 0, pockets, 0, 0, 0, false).unwrap();
    inv.add_item(med, 3, 0, pockets, 1, 0, 0, false).unwrap();
    inv.add_item(ammo, 40, 0, rig, 0, 0, 0, false).unwrap();
    inv.add_item(mag, 1, 0, rig, 2, 0, 0, false).unwrap();
    inv.add_item(gold, 1, 0, safe, 0, 0, 0, false).unwrap();

    let bag_a_k = inv.add_item(bag_a, 1, 0, backpack, 0, 0, 0, false).unwrap();
    let bag_b_k = inv.add_item(bag_b, 1, 0, backpack, 0, 3, 0, false).unwrap();
    inv.add_item(gold, 1, 0, backpack, 0, 5, 0, false).unwrap();
    inv.add_item(mag, 1, 0, backpack, 0, 7, 0, false).unwrap();
    inv.add_item(mag, 1, 0, backpack, 0, 8, 0, false).unwrap();
    inv.add_item(med, 1, 0, backpack, 0, 9, 0, false).unwrap();
    for x in 0..10u8 {
        inv.add_item(ammo, 30, 0, backpack, 0, x, 3, false).unwrap();
    }
    inv.add_item(ammo, 30, 0, backpack, 0, 0, 4, false).unwrap();
    inv.add_item(ammo, 30, 0, backpack, 0, 1, 4, false).unwrap();

    // 套包内部。
    let inner_a = inv.item(bag_a_k).unwrap().container.unwrap();
    for x in 0..6u8 {
        inv.add_item(ammo, 30, 0, inner_a, 0, x, 0, false).unwrap();
    }
    inv.add_item(mag, 1, 0, inner_a, 0, 0, 1, false).unwrap();
    inv.add_item(mag, 1, 0, inner_a, 0, 1, 1, false).unwrap();
    inv.add_item(med, 1, 0, inner_a, 0, 2, 1, false).unwrap();

    let bag_b_inner = inv.item(bag_b_k).unwrap().container.unwrap();
    for x in 0..4u8 {
        inv.add_item(ammo, 30, 0, bag_b_inner, 0, x, 0, false).unwrap();
    }
    inv.add_item(med, 1, 0, bag_b_inner, 0, 0, 1, false).unwrap();

    let count = inv.item_count();
    assert!((30..=40).contains(&count), "本用例期望 30~40 件物品，实际 {count}");
    assert!(inv.check_invariants().is_empty(), "{:?}", inv.check_invariants());

    let snap = inv.snapshot();
    let bytes = snap.encode().unwrap();
    println!(
        "典型装载快照：{} 字节 / {} 件物品 / {} 个容器",
        bytes.len(),
        snap.items.len(),
        snap.containers.len()
    );
    assert!(bytes.len() <= 2048, "整包必须 ≤ 2 KiB，实际 {} 字节", bytes.len());
}

#[test]
fn corrupt_bytes_are_rejected() {
    // 用 ASCII label 便于定位字节。
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("弹药", 1, 1, 60)).unwrap();
    let bp = inv.add_root(ContainerType::Backpack, "snapbag", &[(3, 3)]).unwrap();
    inv.add_item(ammo, 60, 0, bp, 0, 0, 0, false).unwrap();
    let bytes = encode(&inv);

    // 空切片 / 截断 → 解码失败。
    assert!(Snapshot::decode(&[]).is_err());
    assert!(Snapshot::decode(&bytes[..bytes.len() / 2]).is_err());

    // 翻转 label 里的一个字节成非法 UTF-8 → 解码失败。
    let pos = bytes
        .windows(7)
        .position(|w| w == b"snapbag")
        .expect("应能定位 label 字节");
    let mut corrupt = bytes.clone();
    corrupt[pos] = 0xFF;
    assert!(Snapshot::decode(&corrupt).is_err(), "非法 UTF-8 必须被拒");
}

// ==================== 手工构造非法快照 ====================

fn base_snap(def: u16, rotated: bool) -> Snapshot {
    Snapshot {
        roots: [None, None, None, Some(0)],
        containers: vec![ContainerSnap {
            label: "袋".into(),
            ctype: ContainerType::Backpack as u8,
            parts: vec![(3, 3)],
        }],
        items: vec![ItemSnap {
            def,
            stack: 1,
            durability: 0,
            container: None,
            place: PlaceSnap { container: 0, part: 0, x: 0, y: 0, rotated },
        }],
    }
}

#[test]
fn hand_built_invalid_snapshots_are_rejected() {
    let mut inv = Inventory::new();
    let ammo = inv.define_item(ItemDef::new("弹药", 1, 1, 60)).unwrap();
    let rifle = inv.define_item(ItemDef::new("步枪", 2, 3, 1)).unwrap(); // 不可旋转

    // 合法基线先确认目录一致。
    assert!(inv.restore(&base_snap(ammo.0, false)).is_ok());

    // def 越界。
    assert_eq!(inv.restore(&base_snap(u16::MAX, false)), Err(ue_package_system_dylib::InventoryError::CorruptSnapshot));
    // rotated 与定义不符（不可旋转却标了旋转）。
    assert_eq!(inv.restore(&base_snap(rifle.0, true)), Err(ue_package_system_dylib::InventoryError::CorruptSnapshot));
    // 容器下标越界。
    let mut s = base_snap(ammo.0, false);
    s.items[0].place.container = 7;
    assert_eq!(inv.restore(&s), Err(ue_package_system_dylib::InventoryError::CorruptSnapshot));
    // part 下标越界。
    let mut s = base_snap(ammo.0, false);
    s.items[0].place.part = 2;
    assert_eq!(inv.restore(&s), Err(ue_package_system_dylib::InventoryError::CorruptSnapshot));
    // 非法容器类型。
    let mut s = base_snap(ammo.0, false);
    s.containers[0].ctype = 9;
    assert_eq!(inv.restore(&s), Err(ue_package_system_dylib::InventoryError::CorruptSnapshot));
    // 堆叠为 0 / 超过上限。
    let mut s = base_snap(ammo.0, false);
    s.items[0].stack = 0;
    assert_eq!(inv.restore(&s), Err(ue_package_system_dylib::InventoryError::CorruptSnapshot));
    let mut s = base_snap(ammo.0, false);
    s.items[0].stack = 61;
    assert_eq!(inv.restore(&s), Err(ue_package_system_dylib::InventoryError::CorruptSnapshot));
}

#[test]
fn failed_restore_keeps_state() {
    let mut inv = complex_state();
    let before = encode(&inv);

    // def 越界的手工快照必定失败。
    let bad = base_snap(u16::MAX, false);
    assert_eq!(
        inv.restore(&bad),
        Err(ue_package_system_dylib::InventoryError::CorruptSnapshot)
    );
    assert_eq!(encode(&inv), before, "restore 失败后状态必须一个字节都不变");
    assert!(inv.check_invariants().is_empty());
}
