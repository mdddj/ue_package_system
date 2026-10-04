//! 网格几何边界测试：贴边 / 角点 / 越界 / 旋转 / 64 格位图边界 /
//! 位图与逐格扫描等价 / 增删自洽 / 成对互换几何。
//!
//! 全部直接操作 [`GridContainer`]，不经过 Inventory（几何层的独立验证）。

use ue_package_system_dylib::{GridContainer, InventoryError, ItemKey, PlacedMeta};

/// 构造互不相同的测试 key（沿用单元测试的做法，不占用真实 SlotMap）。
fn key(i: u64) -> ItemKey {
    ItemKey::from_u64(i + 1)
}

/// 未旋转的落位元数据。
fn meta(x: u8, y: u8, w: u8, h: u8, rotatable: bool) -> PlacedMeta {
    PlacedMeta {
        x,
        y,
        rotated: false,
        base_w: w,
        base_h: h,
        rotatable,
    }
}

// ==================== 1. 贴边 / 角点 / 越界 ====================

#[test]
fn edge_corner_and_overflow() {
    let mut g = GridContainer::new(10, 10).unwrap();

    // 贴边：整行、右下角单格
    assert!(g.can_place(0, 0, 10, 1));
    assert!(g.can_place(0, 9, 1, 1));
    assert!(g.can_place(9, 9, 1, 1));
    // x + w 恰好等于宽度：合法
    assert!(g.can_place(8, 0, 2, 1));
    assert!(g.can_place(0, 8, 1, 2));
    // 越界一格
    assert!(!g.can_place(9, 0, 2, 1));
    assert!(!g.can_place(0, 9, 1, 2));
    assert!(!g.can_place(10, 0, 1, 1));
    assert!(!g.can_place(0, 10, 1, 1));
    // 0 宽 / 0 高一律不合法
    assert!(!g.can_place(0, 0, 0, 1));
    assert!(!g.can_place(0, 0, 1, 0));

    // 放一个 2x2 后：角点碰撞 + 紧贴边缘仍可放
    g.place(key(0), meta(0, 0, 2, 2, false)).unwrap();
    assert!(!g.can_place(1, 1, 1, 1), "角点重叠应被拒");
    assert!(!g.can_place(0, 1, 4, 1));
    assert!(g.can_place(2, 0, 1, 2), "紧贴右边缘可放");
    // 忽略自己后可“原地重叠”
    assert!(g.can_place_excluding(0, 0, 2, 2, &[key(0)]));
    assert!(g.check().is_empty());
}

#[test]
fn place_with_zero_footprint_is_invalid_def() {
    let mut g = GridContainer::new(4, 4).unwrap();
    // base_w = 0 → footprint 宽为 0 → InvalidDef
    let m = PlacedMeta {
        x: 0,
        y: 0,
        rotated: false,
        base_w: 0,
        base_h: 1,
        rotatable: false,
    };
    assert_eq!(g.place(key(1), m).unwrap_err(), InventoryError::InvalidDef);
    assert!(g.check().is_empty());
}

// ==================== 2. 旋转 ====================

#[test]
fn rotate_changes_footprint_and_detects_blocks() {
    let mut g = GridContainer::new(4, 4).unwrap();
    let a = key(10);
    let b = key(11);
    g.place(a, meta(0, 0, 1, 3, true)).unwrap(); // 竖 1x3：占 (0,0)(0,1)(0,2)
    g.place(b, meta(2, 0, 1, 1, false)).unwrap(); // (2,0)

    // 旋转 a → 3x1，会盖住 (2,0) 上的 b
    assert_eq!(g.rotate(a).unwrap_err(), InventoryError::RotationBlocked);
    assert!(!g.slot_of(a).unwrap().rotated, "失败后朝向不变");
    assert!(g.check().is_empty());

    // 挪开 b 后旋转成功，footprint 互换
    g.remove(b).unwrap();
    g.rotate(a).unwrap();
    assert!(g.slot_of(a).unwrap().rotated);
    assert_eq!(g.slot_of(a).unwrap().footprint(), (3, 1));
    assert!(g.check().is_empty());

    // 再次旋转可回到原朝向
    g.rotate(a).unwrap();
    assert_eq!(g.slot_of(a).unwrap().footprint(), (1, 3));

    // 不可旋转的物品
    g.place(b, meta(3, 3, 1, 1, false)).unwrap();
    assert_eq!(g.rotate(b).unwrap_err(), InventoryError::NotRotatable);

    // 贴右边缘竖直放的 1x2，在 2x2 里转成 2x1 会越界 → RotationBlocked
    let mut g2 = GridContainer::new(2, 2).unwrap();
    let c = key(12);
    g2.place(c, meta(1, 0, 1, 2, true)).unwrap();
    assert_eq!(g2.rotate(c).unwrap_err(), InventoryError::RotationBlocked);
    assert!(!g2.slot_of(c).unwrap().rotated);
    assert!(g2.check().is_empty());
}

// ==================== 3. 64 格位图边界 ====================

#[test]
fn mask_switch_at_64_cells() {
    assert!(GridContainer::new(8, 8).unwrap().is_masked(), "8x8=64 走位图");
    assert!(!GridContainer::new(9, 8).unwrap().is_masked(), "9x8=72 不走位图");
    assert!(!GridContainer::new(8, 9).unwrap().is_masked(), "8x9=72 不走位图");
    assert!(GridContainer::new(64, 1).unwrap().is_masked(), "64x1=64 走位图");
    assert!(GridContainer::new(1, 64).unwrap().is_masked(), "1x64=64 走位图");
    assert!(!GridContainer::new(64, 64).unwrap().is_masked(), "4096 格不走位图");
    // 位图存在性一致
    assert!(GridContainer::new(8, 8).unwrap().occupancy_mask().is_some());
    assert!(GridContainer::new(9, 8).unwrap().occupancy_mask().is_none());
}

#[test]
fn wide_row_boundary_mask() {
    // 64x1：整行恰好铺满一个 u64
    let mut g = GridContainer::new(64, 1).unwrap();
    g.place(key(1), meta(0, 0, 64, 1, false)).unwrap();
    assert_eq!(g.occupancy_mask(), Some(u64::MAX));
    assert!(!g.can_place(0, 0, 1, 1));
    g.remove(key(1)).unwrap();
    assert_eq!(g.occupancy_mask(), Some(0));
    assert!(g.check().is_empty());

    // 1x64：行宽为 1，位下标沿 y 增长
    let mut g2 = GridContainer::new(1, 64).unwrap();
    g2.place(key(2), meta(0, 0, 1, 64, false)).unwrap();
    assert_eq!(g2.occupancy_mask(), Some(u64::MAX));
    g2.remove(key(2)).unwrap();
    assert_eq!(g2.occupancy_mask(), Some(0));
    assert!(g2.check().is_empty());
}

// ==================== 4. 位图 vs 逐格扫描 等价 ====================

/// 简单 xorshift：确定性伪随机，便于复现。
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }
    fn range(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

#[test]
fn can_place_matches_slow_on_random_states() {
    // 覆盖 ≥3 种网格尺寸：位图路径、非位图路径、行宽边界。
    let sizes: [(u8, u8); 5] = [(8, 8), (9, 8), (64, 1), (1, 64), (12, 5)];

    for (i, (gw, gh)) in sizes.iter().enumerate() {
        let mut g = GridContainer::new(*gw, *gh).unwrap();
        let mut rng = Rng(0x9E37_79B9_7F4A_7C15 ^ ((i as u64 + 1) * 0x9E37_79B1));

        // 随机撒入互不重叠的物品
        let mut k = 0u64;
        for _ in 0..80 {
            let w = rng.range(*gw as u64) as u8 + 1;
            let h = rng.range(*gh as u64) as u8 + 1;
            let x = rng.range(*gw as u64) as u8;
            let y = rng.range(*gh as u64) as u8;
            k += 1;
            let _ = g.place(key(k), meta(x, y, w, h, true));
        }

        // 取前两个已落位物品做 ignore
        let placed: Vec<ItemKey> = g.occupants().map(|(kk, _)| kk).collect();
        let ignore: Vec<ItemKey> = placed.iter().take(2).copied().collect();

        let probe: [(u8, u8); 6] = [(1, 1), (2, 3), (3, 2), (4, 4), (1, 5), (5, 2)];
        for x in 0..*gw {
            for y in 0..*gh {
                for (w, h) in probe {
                    assert_eq!(
                        g.can_place(x, y, w, h),
                        g.can_place_slow(x, y, w, h, &[]),
                        "网格 {gw}x{gh} 位置 ({x},{y}) 尺寸 {w}x{h} 空忽略不一致"
                    );
                    assert_eq!(
                        g.can_place_excluding(x, y, w, h, &ignore),
                        g.can_place_slow(x, y, w, h, &ignore),
                        "网格 {gw}x{gh} 位置 ({x},{y}) 尺寸 {w}x{h} 带 ignore 不一致"
                    );
                }
            }
        }
        assert!(g.check().is_empty(), "随机撒放后自检必须为空");
    }
}

// ==================== 5. remove / 重复 place / 自洽 ====================

#[test]
fn place_remove_roundtrip_and_errors() {
    let mut g = GridContainer::new(6, 6).unwrap();
    let k = key(1);
    g.place(k, meta(2, 3, 3, 2, true)).unwrap();
    assert_eq!(g.placed_count(), 1);
    assert!(g.check().is_empty());

    // 重复 place
    assert_eq!(
        g.place(k, meta(0, 0, 1, 1, false)).unwrap_err(),
        InventoryError::AlreadyPlaced
    );
    assert!(g.check().is_empty());

    // 移除不存在的
    assert_eq!(g.remove(key(99)).unwrap_err(), InventoryError::NotPlaced);
    assert!(g.check().is_empty());

    // 移除返回原坐标
    let old = g.remove(k).unwrap();
    assert_eq!((old.x, old.y, old.base_w, old.base_h), (2, 3, 3, 2));
    assert_eq!(g.placed_count(), 0);
    assert_eq!(g.occupancy_mask(), Some(0));
    assert!(g.can_place(2, 3, 3, 2));
    assert!(g.check().is_empty());
}

// ==================== 6. can_place_pair ====================

#[test]
fn pair_swap_adjacent_success() {
    let mut g = GridContainer::new(4, 4).unwrap();
    let a = key(1);
    let b = key(2);
    g.place(a, meta(0, 0, 2, 2, true)).unwrap();
    g.place(b, meta(2, 0, 2, 2, true)).unwrap(); // 相邻贴合

    let new_a = meta(2, 0, 2, 2, true); // a 换到 b 的位置
    let new_b = meta(0, 0, 2, 2, true); // b 换到 a 的位置
    assert!(g.can_place_pair(a, &new_a, b, &new_b));
    assert!(g.check().is_empty());
}

#[test]
fn pair_swap_size_exchange_success() {
    let mut g = GridContainer::new(4, 2).unwrap();
    let a = key(1); // 1x2 竖放
    let b = key(2); // 2x1 横放
    g.place(a, meta(0, 0, 1, 2, true)).unwrap();
    g.place(b, meta(2, 0, 2, 1, true)).unwrap();

    // 互换：a 到 (2,0) 竖放占 (2,0)(2,1)；b 到 (0,0) 横放占 (0,0)(1,0)
    let new_a = meta(2, 0, 1, 2, true);
    let new_b = meta(0, 0, 2, 1, true);
    assert!(g.can_place_pair(a, &new_a, b, &new_b));
    assert!(g.check().is_empty());
}

#[test]
fn pair_swap_off_by_one_overlap_fails() {
    // 非位图网格（100 格 > 64）：逐格扫描路径能正确检测两新矩形互相重叠。
    let mut g = GridContainer::new(10, 10).unwrap();
    let a = key(1);
    let b = key(2);
    g.place(a, meta(0, 0, 2, 2, true)).unwrap();
    g.place(b, meta(2, 0, 2, 2, true)).unwrap();

    // a 挪到 (1,0)：与 b 的新位置 (0,0) 的 2x2 在第 1 列重叠
    let new_a = meta(1, 0, 2, 2, true);
    let new_b = meta(0, 0, 2, 2, true);
    assert!(!g.can_place_pair(a, &new_a, b, &new_b), "只差一格也必须失败");
    assert!(g.check().is_empty());
}

/// 位图路径（≤64 格）下的同一个场景。
///
/// 回归用例：`can_place_pair` 的位图分支曾把 `new_a` 的掩码并进 `bits` 后再与
/// `occ` 按位与，而 `occ` 已剔除 a/b —— 于是 **new_a 与 new_b 互相重叠检测不到**。
/// 现在两个新矩形单独判交，位图与逐格扫描语义一致。
#[test]
fn pair_swap_off_by_one_overlap_masked_grid() {
    let mut g = GridContainer::new(4, 4).unwrap();
    let a = key(1);
    let b = key(2);
    g.place(a, meta(0, 0, 2, 2, true)).unwrap();
    g.place(b, meta(2, 0, 2, 2, true)).unwrap();

    let new_a = meta(1, 0, 2, 2, true);
    let new_b = meta(0, 0, 2, 2, true);
    assert!(!g.can_place_pair(a, &new_a, b, &new_b), "应有重叠，应失败");
}

#[test]
fn pair_swap_third_item_blocks_fails() {
    let mut g = GridContainer::new(4, 4).unwrap();
    let a = key(1);
    let b = key(2);
    let c = key(3);
    g.place(a, meta(0, 0, 1, 1, true)).unwrap();
    g.place(b, meta(2, 0, 1, 1, true)).unwrap();
    g.place(c, meta(2, 1, 1, 1, false)).unwrap(); // 挡在 b 目标区域外，但挡 a 的去路

    // a 想换到 (2,0)，但 c 在 (2,1)... 这里换成 a→(2,0) 仍可；改为把 a 换到 c 所在列
    // 用 c 明确阻挡：让 b 换到 (0,0)，a 换到 (2,1)（c 占着）
    let new_a = meta(2, 1, 1, 1, true);
    let new_b = meta(0, 0, 1, 1, true);
    assert!(!g.can_place_pair(a, &new_a, b, &new_b), "第三方物品应阻挡");
    assert!(g.check().is_empty());
}
