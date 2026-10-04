//! proptest 属性测试。
//!
//! 覆盖三类性质：
//! 1. 随机操作序列的原子性：失败零副作用（快照字节不变）、成功则自洽；
//! 2. 网格快路径（位图）与参考实现（逐格扫描）等价；
//! 3. place/remove 往返后位图与单元格一致。

use proptest::prelude::*;

use ue_package_system_dylib::{
    tags, ContainerKey, ContainerSpec, ContainerType, DefId, GridContainer, Inventory, InvResult,
    ItemDef, ItemKey, PlacedMeta,
};

// ==================== 随机操作序列 ====================

/// 一条原始操作（参数用取模映射到“当前状态里实际存在的对象”）。
#[derive(Clone, Debug)]
struct RawOp {
    kind: u8,
    a: u32,
    b: u32,
    part: u8,
    x: u8,
    y: u8,
    rot: bool,
}

fn op_strategy() -> impl Strategy<Value = RawOp> {
    (0u8..8, any::<u32>(), any::<u32>(), 0u8..5, 0u8..8, 0u8..8, any::<bool>())
        .prop_map(|(kind, a, b, part, x, y, rot)| RawOp { kind, a, b, part, x, y, rot })
}

/// 搭一个固定但内容丰富的世界：混合定义 + 四类根容器 + 初始物品。
fn make_world() -> (Inventory, Vec<DefId>) {
    let mut inv = Inventory::new();
    let ammo = inv
        .define_item(ItemDef::new("子弹", 1, 1, 60).weight(0.01).value(2).tags(tags::AMMO))
        .unwrap();
    let rifle = inv
        .define_item(ItemDef::new("步枪", 2, 3, 1).rotatable(true).weight(3.5).value(900))
        .unwrap();
    let crate25 = inv
        .define_item(ItemDef::new("药箱", 2, 2, 1).weight(1.0).value(300).tags(tags::MEDKIT))
        .unwrap();
    let bag = inv
        .define_item(
            ItemDef::new("套包", 2, 2, 1)
                .weight(0.5)
                .value(200)
                .as_container(ContainerSpec::rectangular(4, 4)),
        )
        .unwrap();
    let gold = inv
        .define_item(
            ItemDef::new("大金", 1, 1, 1)
                .weight(1.0)
                .value(5000)
                .tags(tags::VALUABLE)
                .forbid(ContainerType::SafeBox.bit()),
        )
        .unwrap();
    let defs = vec![ammo, rifle, crate25, bag, gold];

    let pockets = inv.add_root(ContainerType::Pockets, "口袋", &[(1, 1), (1, 1)]).unwrap();
    let rig = inv.add_root(ContainerType::ChestRig, "弹挂", &[(1, 1), (2, 2)]).unwrap();
    let safe = inv.add_root(ContainerType::SafeBox, "安全箱", &[(3, 3)]).unwrap();
    let backpack = inv.add_root(ContainerType::Backpack, "背包", &[(5, 5)]).unwrap();
    let _ = (pockets, rig, safe);

    // 初始物品：保证 ops 有目标可挑。
    inv.add_item(ammo, 60, 0, pockets, 0, 0, 0, false).unwrap();
    inv.add_item(rifle, 1, 0, backpack, 0, 0, 0, false).unwrap();
    let bag_key = inv.add_item(bag, 1, 0, backpack, 0, 2, 0, false).unwrap();
    let inner = inv.item(bag_key).unwrap().container.unwrap();
    inv.add_item(ammo, 20, 0, inner, 0, 0, 0, false).unwrap();

    (inv, defs)
}

/// 施加一条操作。全部参数都按当前状态取模，因此随机数不会越界 panic，只会产生合法或错误返回。
fn apply(inv: &mut Inventory, defs: &[DefId], op: &RawOp) -> InvResult<()> {
    let conts: Vec<ContainerKey> = inv.all_containers();
    let items: Vec<ItemKey> = inv.items().map(|(k, _)| k).collect();
    if conts.is_empty() {
        return Ok(());
    }
    let c = |idx: u32| conts[(idx as usize) % conts.len()];
    let it = |idx: u32| items[(idx as usize) % items.len()];

    match op.kind {
        0 => {
            let def = defs[(op.a as usize) % defs.len()];
            let max = inv.catalog().get(def).unwrap().max_stack;
            let stack = 1 + (op.b % max);
            inv.add_item(def, stack, 0, c(op.b), op.part, op.x, op.y, op.rot)?;
        }
        1 if !items.is_empty() => {
            inv.try_move(it(op.a), c(op.b), op.part, op.x, op.y, op.rot)?
        }
        2 if items.len() >= 2 => inv.try_swap(it(op.a), it(op.b))?,
        3 if !items.is_empty() => inv.try_rotate(it(op.a))?,
        4 if items.len() >= 2 => {
            let _ = inv.try_merge(it(op.a), it(op.b))?;
        }
        5 if !items.is_empty() => {
            let _ = inv.try_split(it(op.a), 1 + (op.b % 5), c(op.b), op.part, op.x, op.y, op.rot)?;
        }
        6 => inv.autosort(c(op.a))?,
        7 if !items.is_empty() => {
            inv.destroy_item(it(op.a))?;
        }
        _ => {}
    }
    Ok(())
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(64))]

    /// 随机操作序列：失败 → 快照字节零变化；成功 → 全库自洽。
    #[test]
    fn random_ops_are_atomic_and_consistent(ops in prop::collection::vec(op_strategy(), 1..48)) {
        let (mut inv, defs) = make_world();
        for op in &ops {
            let before = inv.snapshot().encode().unwrap();
            match apply(&mut inv, &defs, op) {
                Ok(()) => {
                    let errs = inv.check_invariants();
                    prop_assert!(errs.is_empty(), "操作 {:?} 成功但自检失败: {:?}", op, errs);
                }
                Err(_) => {
                    let after = inv.snapshot().encode().unwrap();
                    prop_assert_eq!(before, after, "操作 {:?} 返回错误却有副作用", op);
                }
            }
        }
    }
}

// ==================== 网格：快慢路径等价 ====================

proptest! {
    #![proptest_config(ProptestConfig::with_cases(64))]

    /// 任意随机占位状态下，位图快路径与逐格扫描参考实现在全位置扫描上等价。
    #[test]
    fn can_place_matches_reference(
        w in 1u8..=10,
        h in 1u8..=10,
        placed in prop::collection::vec((0u8..10, 0u8..10, 1u8..=3, 1u8..=3), 0..10),
        probes in prop::collection::vec((0u8..10, 0u8..10, 1u8..=3, 1u8..=3), 1..30),
    ) {
        let mut g = GridContainer::new(w, h).unwrap();
        let mut next: u64 = 1;
        let mut keys: Vec<ItemKey> = Vec::new();
        for (x, y, pw, ph) in &placed {
            let k = ItemKey::from_u64(next);
            next += 1;
            let meta = PlacedMeta {
                x: *x, y: *y, rotated: false, base_w: *pw, base_h: *ph, rotatable: false,
            };
            if g.place(k, meta).is_ok() {
                keys.push(k);
            }
        }
        for (x, y, pw, ph) in &probes {
            prop_assert_eq!(
                g.can_place(*x, *y, *pw, *ph),
                g.can_place_slow(*x, *y, *pw, *ph, &[]),
                "can_place 与参考实现不一致 @({},{}) {}x{}", x, y, pw, ph
            );
            if let Some(k) = keys.first() {
                prop_assert_eq!(
                    g.can_place_excluding(*x, *y, *pw, *ph, &[*k]),
                    g.can_place_slow(*x, *y, *pw, *ph, &[*k]),
                    "排除某个物品时两条路径不一致 @({},{}) {}x{}", x, y, pw, ph
                );
            }
        }
    }
}

/// 位图与单元格互推是否一致（≤64 格才有位图）。
fn mask_ok(g: &GridContainer) -> bool {
    match g.occupancy_mask() {
        None => true,
        Some(m) => {
            let expect = g
                .cells()
                .iter()
                .enumerate()
                .fold(0u64, |acc, (i, c)| if c.is_some() { acc | (1u64 << i) } else { acc });
            m == expect
        }
    }
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(64))]

    /// place/remove 往返：位图与单元格始终一致，全部移除后网格回到空。
    #[test]
    fn place_remove_roundtrip(
        w in 1u8..=10,
        h in 1u8..=10,
        placed in prop::collection::vec((0u8..10, 0u8..10, 1u8..=3, 1u8..=3), 0..10),
    ) {
        let mut g = GridContainer::new(w, h).unwrap();
        let mut next: u64 = 1;
        let mut keys: Vec<ItemKey> = Vec::new();
        for (x, y, pw, ph) in &placed {
            let k = ItemKey::from_u64(next);
            next += 1;
            let meta = PlacedMeta {
                x: *x, y: *y, rotated: false, base_w: *pw, base_h: *ph, rotatable: false,
            };
            if g.place(k, meta).is_ok() {
                keys.push(k);
                prop_assert!(mask_ok(&g), "落位后位图与单元格不一致");
            }
        }
        for k in &keys {
            prop_assert!(g.remove(*k).is_ok());
            prop_assert!(mask_ok(&g), "移除后位图与单元格不一致");
        }
        prop_assert_eq!(g.placed_count(), 0);
        prop_assert!(g.cells().iter().all(|c| c.is_none()));
        if let Some(m) = g.occupancy_mask() {
            prop_assert_eq!(m, 0);
        }
    }
}
