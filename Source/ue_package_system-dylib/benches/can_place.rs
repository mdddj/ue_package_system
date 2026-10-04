//! `GridContainer::can_place` 等热路径的 criterion 基准。
//!
//! 运行：`cargo bench --bench can_place`
//!
//! 验收基准（实施计划阶段三）：100 格背包、约 90% 满载时，单次 `can_place` ≤ 500ns。
//!
//! ## 关于「位图路径」的重要事实（与基准名对照）
//! 源码里 `MASK_MAX_CELLS = 64`，即**只有格数 ≤ 64 的网格**才启用 `u64` 占用位图。
//! 因此：
//! - `10x10 = 100 格 > 64` → `can_place` 实际走的是**逐格扫描**回退（`mask == None`）；
//!   `can_place/backpack_10x10_90pct` 与 `can_place/scan_10x10_90pct` 是同一条代码路径，
//!   两组数字应当一致 —— 这两组保留以便与计划里的命名对齐，并验证这一点。
//! - 真正的位图快路径只能落在 ≤64 格的网格上，故额外提供 `can_place/mask_8x8_90pct`
//!   （8x8 = 64 格，恰好命中位图）作为真实位图性能基准。
//! - `can_place/scan_12x12_90pct`（144 格）同样走扫描路径，用于对比网格规模的影响。
//!
//! 注意：`MASK_MAX_CELLS`、`GridContainer` 语义都来自冻结的 `src/`，基准不修改任何生产代码。

use criterion::{black_box, criterion_group, criterion_main, Criterion};
use ue_package_system_dylib::core::grid::MASK_MAX_CELLS;
use ue_package_system_dylib::core::{
    ContainerKey, ContainerSpec, ContainerType, DefId, GridContainer, Inventory, ItemDef, ItemKey,
    PlacedMeta, Snapshot,
};

/// 生成 90% 占用用的物品形状（混合 1x1 / 2x2 / 1x2）。
const SHAPES: [(u8, u8); 3] = [(1, 1), (2, 2), (1, 2)];

/// 固定种子的线性同余发生器：保证基准布局与查询序列可复现。
struct Lcg(u64);

impl Lcg {
    fn new(seed: u64) -> Self {
        Self(seed)
    }

    /// MMIX 常数版 LCG。
    fn next_u32(&mut self) -> u32 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (self.0 >> 33) as u32
    }

    /// `[0, n)` 内的伪随机数（要求 `n >= 1`）。
    fn below(&mut self, n: u32) -> u32 {
        self.next_u32() % n
    }
}

/// 在空网格上用固定 LCG 贪心摆放 1x1/2x2/1x2 物品直到占用格数达到 `target`。
///
/// 形状按 `attempts % 3` 轮转，所以 1x1 每三轮必出现一次；只要还有空格，
/// 1x1 的随机落点迟早命中，循环必然终止。放不下的候选位置直接跳过。
/// 返回实际占用格数（可能略微超过 `target`）。
fn fill_grid_90pct(grid: &mut GridContainer, target: usize) -> usize {
    let (gw, gh) = grid.size();
    let mut rng = Lcg::new(0xC0FF_EE12_3456_789A);
    let mut occupied = 0usize;
    let mut placed_seq = 0u64;
    let mut attempts = 0u32;
    while occupied < target && attempts < 500_000 {
        let shape = SHAPES[(attempts as usize) % SHAPES.len()];
        attempts += 1;
        if shape.0 > gw || shape.1 > gh {
            continue;
        }
        let x = rng.below((gw - shape.0 + 1) as u32) as u8;
        let y = rng.below((gh - shape.1 + 1) as u32) as u8;
        if !grid.can_place(x, y, shape.0, shape.1) {
            continue;
        }
        let key = ItemKey::from_u64(placed_seq + 1);
        grid.place(
            key,
            PlacedMeta {
                x,
                y,
                rotated: false,
                base_w: shape.0,
                base_h: shape.1,
                rotatable: true,
            },
        )
        .expect("刚落位前已通过 can_place 校验");
        occupied += shape.0 as usize * shape.1 as usize;
        placed_seq += 1;
    }
    occupied
}

/// 构造约 90% 占用的网格，并断言前置条件（防止基准在空网格上失真）。
fn build_raw_grid(w: u8, h: u8, target: usize) -> GridContainer {
    let mut grid = GridContainer::new(w, h).expect("尺寸合法");
    let occupied = fill_grid_90pct(&mut grid, target);
    let total = w as usize * h as usize;
    assert!(
        occupied as f64 >= total as f64 * 0.85,
        "{w}x{h} 前置条件：占用 {occupied}/{total} 格，应 ≥85%"
    );
    assert!(
        grid.is_masked() == (total <= MASK_MAX_CELLS),
        "is_masked 应与格数阈值一致"
    );
    grid
}

/// 固定伪随机查询序列：位置落在界内，尺寸 1x1 .. 3x2。
fn build_queries(n: usize, w: u8, h: u8) -> Vec<(u8, u8, u8, u8)> {
    let mut rng = Lcg::new(0x1357_9BDF_2468_ACE0);
    let mut out = Vec::with_capacity(n);
    for _ in 0..n {
        let qw = 1 + rng.below(3) as u8; // 1..=3
        let qh = 1 + rng.below(2) as u8; // 1..=2
        let x = rng.below((w - qw + 1) as u32) as u8;
        let y = rng.below((h - qh + 1) as u32) as u8;
        out.push((x, y, qw, qh));
    }
    out
}

/// 位图快路径：10x10 名字保留，但 100 格 > 64 实际走 scan 回退（见文件头说明）。
fn bench_mask_10x10(c: &mut Criterion) {
    let grid = build_raw_grid(10, 10, 90);
    eprintln!(
        "[bench] can_place/backpack_10x10_90pct: 100 格 / is_masked={}（阈值 {}）→ 实际路径={}",
        grid.is_masked(),
        MASK_MAX_CELLS,
        if grid.is_masked() { "位图" } else { "逐格扫描" }
    );
    let queries = build_queries(256, 10, 10);
    let mut i = 0usize;
    c.bench_function("can_place/backpack_10x10_90pct", |b| {
        b.iter(|| {
            let q = queries[i % queries.len()];
            i = i.wrapping_add(1);
            black_box(grid.can_place(q.0, q.1, q.2, q.3))
        })
    });
}

/// 真正的 u64 位图路径：8x8 = 64 格，恰好命中 `MASK_MAX_CELLS`。
fn bench_mask_8x8(c: &mut Criterion) {
    let grid = build_raw_grid(8, 8, 58); // 90.6%
    assert!(grid.is_masked(), "8x8 应启用位图");
    eprintln!(
        "[bench] can_place/mask_8x8_90pct: {} 格 / is_masked={}",
        grid.cell_count(),
        grid.is_masked()
    );
    let queries = build_queries(256, 8, 8);
    let mut i = 0usize;
    c.bench_function("can_place/mask_8x8_90pct", |b| {
        b.iter(|| {
            let q = queries[i % queries.len()];
            i = i.wrapping_add(1);
            black_box(grid.can_place(q.0, q.1, q.2, q.3))
        })
    });
}

/// 同一 10x10 状态走参考实现 `can_place_slow`（跳过位图，逐格扫描）。
fn bench_scan_10x10(c: &mut Criterion) {
    let grid = build_raw_grid(10, 10, 90);
    let queries = build_queries(256, 10, 10);
    let mut i = 0usize;
    c.bench_function("can_place/scan_10x10_90pct", |b| {
        b.iter(|| {
            let q = queries[i % queries.len()];
            i = i.wrapping_add(1);
            black_box(grid.can_place_slow(q.0, q.1, q.2, q.3, &[]))
        })
    });
}

/// 144 格（>64）走逐格扫描路径，对比网格规模影响。
fn bench_scan_12x12(c: &mut Criterion) {
    let grid = build_raw_grid(12, 12, 130); // 90.3%
    assert!(!grid.is_masked(), "12x12 应走逐格扫描");
    let queries = build_queries(256, 12, 12);
    let mut i = 0usize;
    c.bench_function("can_place/scan_12x12_90pct", |b| {
        b.iter(|| {
            let q = queries[i % queries.len()];
            i = i.wrapping_add(1);
            black_box(grid.can_place(q.0, q.1, q.2, q.3))
        })
    });
}

/// 一次 place + remove 往返（落位/移除的位图与平铺数组更新成本）。
fn bench_insert_remove(c: &mut Criterion) {
    let mut grid = build_raw_grid(10, 10, 90);
    // 90% 占用后必然还有空格，找一个可用的 1x1。
    let (ex, ey) = (0..10u8)
        .flat_map(|y| (0..10u8).map(move |x| (x, y)))
        .find(|&(x, y)| grid.can_place(x, y, 1, 1))
        .expect("90% 占用后仍有空格");
    let key = ItemKey::from_u64(9_999);
    let meta = PlacedMeta {
        x: ex,
        y: ey,
        rotated: false,
        base_w: 1,
        base_h: 1,
        rotatable: true,
    };
    c.bench_function("insert_remove/10x10", |b| {
        b.iter(|| {
            grid.place(key, meta).expect("预选空格");
            black_box(grid.remove(key).expect("刚放进去"));
        })
    });
}

/// 跨容器移动一件物品：口袋 → 套包内部 → 背包空格 → 回口袋（三条路径轮转）。
fn bench_try_move_cross_container(c: &mut Criterion) {
    let mut inv = Inventory::new();
    let unit = inv
        .define_item(ItemDef::new("小件", 1, 1, 1).weight(0.1))
        .expect("定义");
    let bag = inv
        .define_item(
            ItemDef::new("套包", 3, 3, 1)
                .rotatable(true)
                .as_container(ContainerSpec::rectangular(4, 4)),
        )
        .expect("定义");
    let pockets = inv
        .add_root(ContainerType::Pockets, "口袋", &[(1, 1)])
        .expect("建口袋");
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(6, 5)])
        .expect("建背包");
    let bag_item = inv
        .add_item(bag, 1, 0, backpack, 0, 0, 0, false)
        .expect("放置套包");
    let nested: ContainerKey = inv.item(bag_item).expect("套包物品存在").container.expect("套包有内部容器");
    let item = inv
        .add_item(unit, 1, 0, pockets, 0, 0, 0, false)
        .expect("放置小件");
    assert!(inv.check_invariants().is_empty(), "前置状态应自洽");

    let mut state = 0u8;
    c.bench_function("manager/try_move_cross_container", |b| {
        b.iter(|| {
            match state {
                0 => inv.try_move(item, nested, 0, 0, 0, false).expect("口袋→套包"),
                1 => inv.try_move(item, backpack, 0, 3, 0, false).expect("套包→背包空格"),
                _ => inv.try_move(item, pockets, 0, 0, 0, false).expect("背包→口袋"),
            }
            state = (state + 1) % 3;
        })
    });
    assert!(inv.check_invariants().is_empty(), "基准后状态应自洽");
}

/// 碎片化 10x10 容器整理一次（每次迭代都从碎片态开始）。
fn bench_autosort(c: &mut Criterion) {
    let mut inv = Inventory::new();
    let small = inv.define_item(ItemDef::new("小件", 1, 1, 1)).expect("定义");
    let wide = inv
        .define_item(ItemDef::new("长件", 2, 1, 1).rotatable(true))
        .expect("定义");
    let big = inv.define_item(ItemDef::new("方块", 2, 2, 1)).expect("定义");
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(10, 10)])
        .expect("建背包");

    // 手工散布，刻意留下空洞，模拟碎片化布局。
    let layout: [(DefId, u8, u8); 10] = [
        (small, 0, 0),
        (small, 9, 0),
        (small, 0, 9),
        (small, 9, 9),
        (big, 4, 4),
        (big, 7, 7),
        (wide, 2, 6),
        (wide, 6, 2),
        (small, 5, 0),
        (small, 0, 5),
    ];
    for (d, x, y) in layout {
        inv.add_item(d, 1, 0, backpack, 0, x, y, false)
            .expect("手工布局不重叠");
    }
    assert!(inv.check_invariants().is_empty(), "前置状态应自洽");

    // 先确认整理可行，再把布局还原成碎片态，保证每次迭代的输入一致。
    let fragmented = inv.snapshot().encode().expect("编码");
    inv.autosort(backpack).expect("可整理");
    assert!(inv.check_invariants().is_empty(), "整理后状态应自洽");
    let restored = Snapshot::decode(&fragmented).expect("解码");
    inv.restore(&restored).expect("还原碎片布局");

    c.bench_function("manager/autosort_10x10", |b| {
        b.iter(|| inv.autosort(backpack).expect("可整理"))
    });
}

/// 编码一次 10x10 约 90% 占用的快照（同时打印字节数）。
fn bench_snapshot_encode(c: &mut Criterion) {
    let mut inv = Inventory::new();
    let unit = inv
        .define_item(ItemDef::new("小件", 1, 1, 1).weight(0.05).value(10))
        .expect("定义");
    let big = inv.define_item(ItemDef::new("方块", 2, 2, 1)).expect("定义");
    let wide = inv
        .define_item(ItemDef::new("长件", 1, 2, 1).rotatable(true))
        .expect("定义");
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(10, 10)])
        .expect("建背包");

    let (gw, gh) = inv.container(backpack).expect("容器存在").part(0).expect("有 part").size();
    let mut rng = Lcg::new(0xDEAD_BEEF_0BAD_F00D);
    let defs = [unit, big, wide];
    let mut occupied = 0usize;
    let mut attempts = 0u32;
    while occupied < 90 && attempts < 500_000 {
        let shape = SHAPES[(attempts as usize) % SHAPES.len()];
        attempts += 1;
        if shape.0 > gw || shape.1 > gh {
            continue;
        }
        let x = rng.below((gw - shape.0 + 1) as u32) as u8;
        let y = rng.below((gh - shape.1 + 1) as u32) as u8;
        if !inv
            .container(backpack)
            .expect("容器存在")
            .part(0)
            .expect("有 part")
            .can_place(x, y, shape.0, shape.1)
        {
            continue;
        }
        let def = match shape {
            (1, 1) => defs[0],
            (2, 2) => defs[1],
            _ => defs[2],
        };
        inv.add_item(def, 1, 0, backpack, 0, x, y, false)
            .expect("已通过 can_place");
        occupied += shape.0 as usize * shape.1 as usize;
    }
    assert!(occupied >= 85, "10x10 前置条件：占用 {occupied}/100 格，应 ≥85");
    assert!(inv.check_invariants().is_empty(), "前置状态应自洽");

    let bytes = inv.snapshot().encode().expect("编码");
    eprintln!(
        "[bench] snapshot/encode_10x10_90pct: {} 字节（{} 件物品，占用 {} 格）",
        bytes.len(),
        inv.item_count(),
        occupied
    );

    c.bench_function("snapshot/encode_10x10_90pct", |b| {
        b.iter(|| black_box(inv.snapshot().encode().expect("编码")))
    });
}

criterion_group!(
    benches,
    bench_mask_10x10,
    bench_mask_8x8,
    bench_scan_10x10,
    bench_scan_12x12,
    bench_insert_remove,
    bench_try_move_cross_container,
    bench_autosort,
    bench_snapshot_encode,
);
criterion_main!(benches);
