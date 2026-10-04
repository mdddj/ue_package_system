//! 负重系统：总重 / 承重上限 / 负重比 / 超重判定，
//! 以及一条重要性质 —— **几何操作（移动/旋转/互换/拆分/合并/整理）不改变总重**，
//! 只有生成、销毁与恢复快照才会改变。

use std::ffi::CString;

use ue_package_system_dylib::ffi;
use ue_package_system_dylib::{
    tags, ContainerKey, ContainerSpec, ContainerType, DefId, Inventory, InventoryError, ItemDef,
    ItemKey,
};

/// 重量比较容差：f32 存、f64 累加。
const EPS: f64 = 1e-6;

struct World {
    inv: Inventory,
    pockets: ContainerKey,
    backpack: ContainerKey,
    mag_def: DefId,
    ammo_key: ItemKey,
    bag_key: ItemKey,
    bag_inner: ContainerKey,
}

/// 一套固定装载：弹药 60 发（0.72kg）+ 急救包 ×3（1.5kg）+ 突击背包（2.0kg，内含弹药 30 发 0.36kg）
/// = 4.58 kg。
fn world() -> World {
    let mut inv = Inventory::new();
    let ammo_def = inv
        .define_item(
            ItemDef::new("9x19 弹药", 1, 1, 60)
                .weight(0.012)
                .value(3)
                .tags(tags::AMMO),
        )
        .unwrap();
    let medkit_def = inv
        .define_item(ItemDef::new("急救包", 1, 1, 3).weight(0.5).value(300))
        .unwrap();
    let mag_def = inv
        .define_item(
            ItemDef::new("AK 弹匣", 1, 2, 1)
                .rotatable(true)
                .weight(0.2)
                .value(120),
        )
        .unwrap();
    let bag_def = inv
        .define_item(
            ItemDef::new("突击背包", 3, 3, 1)
                .weight(2.0)
                .value(900)
                .as_container(ContainerSpec::rectangular(6, 5)),
        )
        .unwrap();

    let pockets = inv
        .add_root(ContainerType::Pockets, "口袋", &[(1, 1), (1, 1), (1, 2)])
        .unwrap();
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(6, 5)])
        .unwrap();

    let ammo_key = inv.add_item(ammo_def, 60, 0, pockets, 0, 0, 0, false).unwrap();
    inv.add_item(medkit_def, 3, 0, pockets, 1, 0, 0, false).unwrap();
    let bag_key = inv.add_item(bag_def, 1, 0, backpack, 0, 0, 0, false).unwrap();
    let bag_inner = inv.item(bag_key).unwrap().container.unwrap();
    inv.add_item(ammo_def, 30, 0, bag_inner, 0, 0, 0, false).unwrap();

    World {
        inv,
        pockets,
        backpack,
        mag_def,
        ammo_key,
        bag_key,
        bag_inner,
    }
}

#[test]
fn default_is_unlimited() {
    let w = world();
    assert!(!w.inv.rules().has_capacity_limit(), "默认应无承重上限");
    assert_eq!(w.inv.weight_ratio(), 0.0, "无上限时负重比恒为 0");
    assert!(!w.inv.is_overloaded());
    assert!((w.inv.total_weight() - 4.58).abs() < EPS);
}

#[test]
fn ratio_tracks_capacity_and_flags_overload() {
    let mut w = world();
    let total = w.inv.total_weight() as f32;

    let rules = w.inv.rules();
    w.inv.set_rules(rules.with_max_capacity(total * 2.0));
    assert!((w.inv.weight_ratio() - 0.5).abs() < 1e-4);
    assert!(!w.inv.is_overloaded());

    let rules = w.inv.rules();
    w.inv.set_rules(rules.with_max_capacity(total * 0.9));
    assert!(w.inv.weight_ratio() > 1.0, "总重超过上限时比例应 > 1");
    assert!(w.inv.is_overloaded());

    // 上限刚好等于总重：不算超重（阈值是 > 1.0）
    let rules = w.inv.rules();
    w.inv.set_rules(rules.with_max_capacity(total));
    assert!((w.inv.weight_ratio() - 1.0).abs() < 1e-4);
    assert!(!w.inv.is_overloaded());
}

#[test]
fn stacks_and_nested_contents_are_counted() {
    let w = world();
    let ammo = 0.012f32 as f64;
    let medkit = 0.5f32 as f64;
    let bag = 2.0f32 as f64;
    // 弹药按数量乘；套包本体 + 套包内部都计入
    let expected = ammo * 60.0 + medkit * 3.0 + bag + ammo * 30.0;
    assert!((w.inv.total_weight() - expected).abs() < EPS);
    // 套包内部确实有东西（不是空壳）
    assert_eq!(w.inv.items_in(w.bag_inner).len(), 1);
}

#[test]
fn geometry_ops_do_not_change_weight() {
    let mut w = world();
    let inv = &mut w.inv;

    // 布局：背包(0,0) 3x3，弹匣(3,0) 1x2
    let mag = inv.add_item(w.mag_def, 1, 0, w.backpack, 0, 3, 0, false).unwrap();
    let before = inv.total_weight();

    // 1) 同一网格内挪位：套包 (0,0) → (0,2)
    inv.try_move(w.bag_key, w.backpack, 0, 0, 2, false).unwrap();
    // 2) 旋转：弹匣 1x2 → 2x1
    inv.try_rotate(mag).unwrap();
    // 3) 原子互换：弹匣 ↔ 套包（两块新落点互不重叠）
    inv.try_swap(mag, w.bag_key).unwrap();
    // 4) 拆分 + 合并：总量不变
    let split = inv.try_split(w.ammo_key, 10, w.backpack, 0, 0, 4, false).unwrap();
    assert_eq!(inv.try_merge(split, w.ammo_key).unwrap(), 10);
    // 5) 一键整理
    inv.autosort(w.backpack).unwrap();

    assert!(inv.check_invariants().is_empty());
    assert_eq!(inv.items_in(w.pockets).len(), 2, "口袋里的弹药与急救包没动");
    assert!(
        (inv.total_weight() - before).abs() < 1e-9,
        "几何操作不应改变总重：{before} → {}",
        inv.total_weight()
    );
}

#[test]
fn destroy_subtracts_subtree_weight() {
    let mut w = world();
    let before = w.inv.total_weight();
    let removed = w.inv.destroy_item(w.bag_key).unwrap();
    assert_eq!(removed, 2, "背包本体 + 内部弹药");
    // 少掉：背包 2.0 + 内部弹药 30 × 0.012
    let expected = 2.0f32 as f64 + 0.012f32 as f64 * 30.0;
    assert!((before - w.inv.total_weight() - expected).abs() < EPS);
}

#[test]
fn invalid_capacity_means_unlimited() {
    let mut w = world();
    for bad in [0.0f32, -1.0, f32::NAN, f32::INFINITY] {
        let rules = w.inv.rules();
        w.inv.set_rules(rules.with_max_capacity(bad));
        assert!(!w.inv.rules().has_capacity_limit(), "{bad} 应视为无限制");
        assert_eq!(w.inv.weight_ratio(), 0.0);
        assert!(!w.inv.is_overloaded());
    }
    let rules = w.inv.rules();
    w.inv.set_rules(rules.with_max_capacity(5.0));
    assert!(w.inv.rules().has_capacity_limit());
    assert!(w.inv.weight_ratio() > 0.0);
}

#[test]
fn ffi_weight_surface() {
    let inv = ffi::uerust_inventory_new();
    assert!(!inv.is_null());

    // 空背包 + 无上限
    let mut ratio = -1.0f32;
    assert_eq!(unsafe { ffi::uerust_inventory_weight_ratio(inv, &mut ratio) }, 0);
    assert_eq!(ratio, 0.0);
    assert_eq!(unsafe { ffi::uerust_inventory_is_overloaded(inv) }, 0);
    assert_eq!(unsafe { ffi::uerust_inventory_set_max_capacity(inv, 0.0) }, 0);
    assert_eq!(unsafe { ffi::uerust_inventory_is_overloaded(inv) }, 0);

    // 放一个 1.0 kg 的物品，上限设成 0.5 kg → 比例 2.0、判定超重
    let name = CString::new("铁块").unwrap();
    let dims = [(2u8, 2u8)];
    let ck = unsafe {
        ffi::uerust_inventory_add_root(inv, 3, name.as_ptr(), dims.as_ptr().cast(), 1)
    };
    assert!(ck > 0);
    let def = unsafe {
        ffi::uerust_inventory_define_item(
            inv,
            name.as_ptr(),
            1,
            1,
            0,
            1,
            1.0,
            10,
            0,
            0,
            std::ptr::null(),
            0,
        )
    };
    assert!(def >= 0);
    let item = unsafe {
        ffi::uerust_inventory_add_item(inv, def as u32, 1, 0, ck as u64, 0, 0, 0, 0)
    };
    assert!(item > 0);

    assert_eq!(unsafe { ffi::uerust_inventory_set_max_capacity(inv, 0.5) }, 0);
    let mut ratio = 0.0f32;
    assert_eq!(unsafe { ffi::uerust_inventory_weight_ratio(inv, &mut ratio) }, 0);
    assert!((ratio - 2.0).abs() < 1e-4, "1kg / 0.5kg = 2.0，实际 {ratio}");
    assert_eq!(unsafe { ffi::uerust_inventory_is_overloaded(inv) }, 1);

    assert_eq!(unsafe { ffi::uerust_inventory_set_max_capacity(inv, 2.0) }, 0);
    assert_eq!(unsafe { ffi::uerust_inventory_is_overloaded(inv) }, 0);

    // NULL 输出指针 → InvalidArgument 码
    assert_eq!(
        unsafe { ffi::uerust_inventory_weight_ratio(inv, std::ptr::null_mut()) },
        InventoryError::InvalidArgument.code()
    );

    unsafe { ffi::uerust_inventory_free(inv) };

    // NULL 句柄：i32 返回正错误码，i64 返回 -错误码
    assert_eq!(
        unsafe { ffi::uerust_inventory_set_max_capacity(std::ptr::null_mut(), 1.0) },
        InventoryError::InvalidHandle.code()
    );
    assert_eq!(
        unsafe { ffi::uerust_inventory_is_overloaded(std::ptr::null_mut()) },
        -(InventoryError::InvalidHandle.code() as i64)
    );
}
