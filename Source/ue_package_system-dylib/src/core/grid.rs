//! 单容器网格与 2D 碰撞检测。
//!
//! - 网格用平铺数组 `Vec<Option<ItemKey>>`（行主序，`idx = y * w + x`）；
//! - 格数 ≤ [`MASK_MAX_CELLS`] 的网格额外维护一个 `u64` 占用位图，
//!   放置校验退化为一次按位与（O(1)，无循环）；
//! - 物品的位置与朝向由本结构统一记录（[`PlacedMeta`]），
//!   物品体自己没有坐标字段，杜绝双份状态。

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use super::error::{InvResult, InventoryError};
use super::ids::ItemKey;

/// 网格格数上限。
pub const MAX_CELLS: usize = 4096;
/// 启用 u64 位图快路径的格数上限。
pub const MASK_MAX_CELLS: usize = 64;
/// 单个维度上限（受 x/y 的 u8 表示限制）。
pub const MAX_DIM: u8 = 255;

/// 一次落位的元数据：物品在网格里的完整几何信息。
#[derive(Copy, Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlacedMeta {
    pub x: u8,
    pub y: u8,
    /// 是否旋转 90°。
    pub rotated: bool,
    /// 物品定义里的原始宽（未旋转）。
    pub base_w: u8,
    /// 物品定义里的原始高（未旋转）。
    pub base_h: u8,
    /// 定义是否允许旋转（冗余存一份，让网格可以独立执行 rotate）。
    pub rotatable: bool,
}

impl PlacedMeta {
    /// 当前朝向下的有效占格尺寸。
    #[inline]
    pub fn footprint(&self) -> (u8, u8) {
        if self.rotated {
            (self.base_h, self.base_w)
        } else {
            (self.base_w, self.base_h)
        }
    }

    /// 旋转后的新元数据（不改位置与 base 尺寸）。
    #[inline]
    pub fn toggled(&self) -> Self {
        let mut m = *self;
        m.rotated = !m.rotated;
        m
    }
}

/// 一个矩形网格容器（背包 / 安全箱的一块，或弹挂的一个口袋）。
#[derive(Clone, Debug)]
pub struct GridContainer {
    w: u8,
    h: u8,
    cells: Vec<Option<ItemKey>>,
    placed: BTreeMap<ItemKey, PlacedMeta>,
    /// `Some` 当且仅当 `w * h <= MASK_MAX_CELLS`，与 `cells` 保持同步。
    mask: Option<u64>,
}

impl GridContainer {
    /// 新建网格。尺寸必须 1..=[`MAX_DIM`] 且格数 ≤ [`MAX_CELLS`]。
    pub fn new(w: u8, h: u8) -> InvResult<Self> {
        if w == 0 || h == 0 || w as usize * h as usize > MAX_CELLS {
            return Err(InventoryError::InvalidDef);
        }
        let len = w as usize * h as usize;
        Ok(Self {
            w,
            h,
            cells: vec![None; len],
            placed: BTreeMap::new(),
            mask: if len <= MASK_MAX_CELLS { Some(0) } else { None },
        })
    }

    #[inline]
    pub fn width(&self) -> u8 {
        self.w
    }

    #[inline]
    pub fn height(&self) -> u8 {
        self.h
    }

    #[inline]
    pub fn size(&self) -> (u8, u8) {
        (self.w, self.h)
    }

    #[inline]
    pub fn cell_count(&self) -> usize {
        self.cells.len()
    }

    /// 位图快路径是否启用。
    #[inline]
    pub fn is_masked(&self) -> bool {
        self.mask.is_some()
    }

    /// 已落位物品数。
    #[inline]
    pub fn placed_count(&self) -> usize {
        self.placed.len()
    }

    /// 被占用的格数（按格计，不是物品件数）。
    pub fn occupied_cells(&self) -> usize {
        self.cells.iter().filter(|c| c.is_some()).count()
    }

    /// 平铺单元格（行主序）。空的格是 `None`。
    #[inline]
    pub fn cells(&self) -> &[Option<ItemKey>] {
        &self.cells
    }

    /// 占用位图（格数 > 64 时为 `None`）。
    #[inline]
    pub fn occupancy_mask(&self) -> Option<u64> {
        self.mask
    }

    /// 单元格内容。
    #[inline]
    pub fn cell(&self, x: u8, y: u8) -> Option<ItemKey> {
        if !self.bounds_ok(x, y, 1, 1) {
            return None;
        }
        self.cells[y as usize * self.w as usize + x as usize]
    }

    /// 已落位物品及其元数据，按 [`ItemKey`] 顺序（确定性迭代）。
    pub fn occupants(&self) -> impl Iterator<Item = (ItemKey, PlacedMeta)> + '_ {
        self.placed.iter().map(|(k, m)| (*k, *m))
    }

    /// 某个物品的落位信息。
    #[inline]
    pub fn slot_of(&self, key: ItemKey) -> Option<PlacedMeta> {
        self.placed.get(&key).copied()
    }

    /// 给定矩形是否在网格内（不检查占用）。
    #[inline]
    pub fn bounds_ok(&self, x: u8, y: u8, w: u8, h: u8) -> bool {
        w > 0
            && h > 0
            && x as u32 + w as u32 <= self.w as u32
            && y as u32 + h as u32 <= self.h as u32
    }

    /// 能否在 `(x, y)` 放下一块 `w * h` 的矩形：越界检测 + 重叠碰撞检测。
    #[inline]
    pub fn can_place(&self, x: u8, y: u8, w: u8, h: u8) -> bool {
        self.can_place_excluding(x, y, w, h, &[])
    }

    /// 同 [`Self::can_place`]，但忽略 `ignore` 中物品的占用。
    ///
    /// 用于“物品从 A 挪到 B，起点终点同一网格”的场景：移动前它自己还占着格子。
    pub fn can_place_excluding(&self, x: u8, y: u8, w: u8, h: u8, ignore: &[ItemKey]) -> bool {
        if !self.bounds_ok(x, y, w, h) {
            return false;
        }
        match self.mask {
            Some(base) => {
                if ignore.is_empty() {
                    return base & self.footprint_mask(x, y, w, h) == 0;
                }
                let mut occ = base;
                for k in ignore {
                    if let Some(m) = self.placed.get(k) {
                        let (iw, ih) = m.footprint();
                        occ &= !self.footprint_mask(m.x, m.y, iw, ih);
                    }
                }
                occ & self.footprint_mask(x, y, w, h) == 0
            }
            None => self.scan_place(x, y, w, h, ignore),
        }
    }

    /// 强制走逐格扫描的参考实现（跳过位图）。
    ///
    /// 生产代码在格数 ≤ 64 时走位图；这个方法用于基准对比与 proptest 的等价性校验。
    pub fn can_place_slow(&self, x: u8, y: u8, w: u8, h: u8, ignore: &[ItemKey]) -> bool {
        if !self.bounds_ok(x, y, w, h) {
            return false;
        }
        self.scan_place(x, y, w, h, ignore)
    }

    /// 两个物品互换落点是否可行（原子 Swap 的几何定案）。
    ///
    /// 语义：`a`、`b` 都先离开当前占用，然后 `a` 落到 `new_a`、`b` 落到 `new_b`，
    /// 两者不能重叠，也不能压到其他物品。
    pub fn can_place_pair(
        &self,
        a: ItemKey,
        new_a: &PlacedMeta,
        b: ItemKey,
        new_b: &PlacedMeta,
    ) -> bool {
        let ignore = [a, b];
        // a 的新落点：不能压任何其他物品
        if self.rect_blocked(new_a, &ignore, None) {
            return false;
        }
        // b 的新落点：不能压其他物品，也不能压 a 的新落点
        if self.rect_blocked(new_b, &ignore, Some(new_a)) {
            return false;
        }
        true
    }

    /// 落点 `m` 是否被挡（忽略 `ignore` 的占用；`also_avoid` 的矩形也算障碍）。
    fn rect_blocked(
        &self,
        m: &PlacedMeta,
        ignore: &[ItemKey],
        also_avoid: Option<&PlacedMeta>,
    ) -> bool {
        let (w, h) = m.footprint();
        if !self.bounds_ok(m.x, m.y, w, h) {
            return true;
        }
        match self.mask {
            Some(base) => {
                let mut occ = base;
                for k in ignore {
                    if let Some(im) = self.placed.get(k) {
                        let (iw, ih) = im.footprint();
                        occ &= !self.footprint_mask(im.x, im.y, iw, ih);
                    }
                }
                let own = self.footprint_mask(m.x, m.y, w, h);
                // 与其他物品（剔除 ignore 后）的重叠
                if occ & own != 0 {
                    return true;
                }
                // 与“另一个新落点”的重叠：两者都没进 occ，必须单独判交，
                // 不能把 also_avoid 的掩码并进 bits 再和 occ 按位与（那样恒为 0）。
                if let Some(av) = also_avoid {
                    let (aw, ah) = av.footprint();
                    if own & self.footprint_mask(av.x, av.y, aw, ah) != 0 {
                        return true;
                    }
                }
                false
            }
            None => {
                for j in m.y..m.y + h {
                    for i in m.x..m.x + w {
                        let idx = j as usize * self.w as usize + i as usize;
                        if let Some(k) = self.cells[idx] {
                            if !ignore.contains(&k) {
                                return true;
                            }
                        }
                        if let Some(av) = also_avoid {
                            if rect_covers(av, i, j) {
                                return true;
                            }
                        }
                    }
                }
                false
            }
        }
    }

    fn scan_place(&self, x: u8, y: u8, w: u8, h: u8, ignore: &[ItemKey]) -> bool {
        for j in y..y + h {
            for i in x..x + w {
                if let Some(k) = self.cells[j as usize * self.w as usize + i as usize] {
                    if !ignore.contains(&k) {
                        return false;
                    }
                }
            }
        }
        true
    }

    /// 落位。校验不通过时不产生任何改动。
    pub fn place(&mut self, key: ItemKey, meta: PlacedMeta) -> InvResult<()> {
        if self.placed.contains_key(&key) {
            return Err(InventoryError::AlreadyPlaced);
        }
        let (w, h) = meta.footprint();
        if w == 0 || h == 0 {
            return Err(InventoryError::InvalidDef);
        }
        if !self.can_place(meta.x, meta.y, w, h) {
            return Err(self.classify(meta.x, meta.y, w, h));
        }
        self.place_validated(key, meta);
        Ok(())
    }

    /// 已通过校验的落位（内部两阶段提交用）。调用方保证：
    /// - `key` 当前不在网格里；
    /// - `meta` 的矩形在网格内且不与任何物品重叠。
    ///
    /// debug 构建下会复验这两条；release 下直接写，不可能失败 → 提交阶段天然原子。
    pub(crate) fn place_validated(&mut self, key: ItemKey, meta: PlacedMeta) {
        debug_assert!(!self.placed.contains_key(&key), "place_validated 不允许重复落位");
        let (w, h) = meta.footprint();
        debug_assert!(
            self.can_place(meta.x, meta.y, w, h),
            "place_validated 的调用方必须先通过 can_place 校验"
        );
        self.write_cells(key, &meta, true);
        self.placed.insert(key, meta);
    }

    /// 移除物品，返回它原来的落位信息。物品不在网格里返回 [`InventoryError::NotPlaced`]。
    pub fn remove(&mut self, key: ItemKey) -> InvResult<PlacedMeta> {
        let meta = self.placed.remove(&key).ok_or(InventoryError::NotPlaced)?;
        self.write_cells(key, &meta, false);
        Ok(meta)
    }

    /// 原地旋转：位置不变，宽高互换后重新做碰撞检测。
    pub fn rotate(&mut self, key: ItemKey) -> InvResult<()> {
        let meta = *self.placed.get(&key).ok_or(InventoryError::NotPlaced)?;
        if !meta.rotatable {
            return Err(InventoryError::NotRotatable);
        }
        let new_meta = meta.toggled();
        let (w, h) = new_meta.footprint();
        if !self.can_place_excluding(new_meta.x, new_meta.y, w, h, &[key]) {
            return Err(InventoryError::RotationBlocked);
        }
        // 先清旧格再写新格（方形物品两组格子重合，顺序不能反）
        self.write_cells(key, &meta, false);
        self.write_cells(key, &new_meta, true);
        self.placed.insert(key, new_meta);
        Ok(())
    }

    fn classify(&self, x: u8, y: u8, w: u8, h: u8) -> InventoryError {
        if self.bounds_ok(x, y, w, h) {
            InventoryError::Occupied
        } else {
            InventoryError::OutOfBounds
        }
    }

    /// 计算一块矩形在位图里的位集合。
    ///
    /// 前置条件：`bounds_ok` 通过（调用方保证），此时所有位下标 < 64。
    fn footprint_mask(&self, x: u8, y: u8, w: u8, h: u8) -> u64 {
        debug_assert!(self.mask.is_some());
        debug_assert!(self.bounds_ok(x, y, w, h));
        let gw = self.w as u64;
        let row_bits: u64 = if w >= 64 { u64::MAX } else { (1u64 << w) - 1 };
        let mut m = 0u64;
        for j in 0..h as u64 {
            m |= row_bits << (x as u64 + (y as u64 + j) * gw);
        }
        m
    }

    fn write_cells(&mut self, key: ItemKey, meta: &PlacedMeta, set: bool) {
        let (w, h) = meta.footprint();
        let gw = self.w as usize;
        for j in 0..h as usize {
            for i in 0..w as usize {
                let idx = (meta.y as usize + j) * gw + meta.x as usize + i;
                self.cells[idx] = if set { Some(key) } else { None };
            }
        }
        if self.mask.is_some() {
            let fm = self.footprint_mask(meta.x, meta.y, w, h);
            if let Some(base) = self.mask.as_mut() {
                if set {
                    *base |= fm;
                } else {
                    *base &= !fm;
                }
            }
        }
    }

    /// 自检：单元格 ↔ 落位表 ↔ 位图 三者一致。返回空列表表示一切正常。
    pub fn check(&self) -> Vec<String> {
        let mut errs = Vec::new();
        let gw = self.w as usize;
        let gh = self.h as usize;
        if self.cells.len() != gw * gh {
            errs.push(format!("cells 长度 {} != w*h {}", self.cells.len(), gw * gh));
        }
        // 单元格 → 落位表
        let mut cell_count: BTreeMap<ItemKey, usize> = BTreeMap::new();
        for (idx, c) in self.cells.iter().enumerate() {
            if let Some(k) = c {
                *cell_count.entry(*k).or_default() += 1;
                if !self.placed.contains_key(k) {
                    errs.push(format!("格 {idx} 指向未落位物品 {k:?}"));
                }
            }
        }
        // 落位表 → 单元格
        for (k, meta) in &self.placed {
            let (w, h) = meta.footprint();
            if w == 0 || h == 0 {
                errs.push(format!("{k:?} 的 base 尺寸为 0"));
                continue;
            }
            if !self.bounds_ok(meta.x, meta.y, w, h) {
                errs.push(format!("{k:?} 越界 (x={}, y={}, w={w}, h={h})", meta.x, meta.y));
                continue;
            }
            if cell_count.get(k).copied() != Some(w as usize * h as usize) {
                errs.push(format!(
                    "{k:?} 占格数 {:?} != {}",
                    cell_count.get(k),
                    w as usize * h as usize
                ));
            }
            for j in 0..h as usize {
                for i in 0..w as usize {
                    let idx = (meta.y as usize + j) * gw + meta.x as usize + i;
                    if self.cells[idx] != Some(*k) {
                        errs.push(format!("{k:?} 的格 {idx} 内容不符"));
                    }
                }
            }
        }
        // 位图核对
        if let Some(m) = self.mask {
            let mut recomputed = 0u64;
            for (idx, c) in self.cells.iter().enumerate() {
                if c.is_some() {
                    recomputed |= 1u64 << idx;
                }
            }
            if recomputed != m {
                errs.push(format!("位图不一致: {m:#x} != {recomputed:#x}"));
            }
        }
        errs
    }
}

/// `(x, y)` 是否落在落位矩形内。
fn rect_covers(m: &PlacedMeta, x: u8, y: u8) -> bool {
    let (w, h) = m.footprint();
    x >= m.x && y >= m.y && x < m.x + w && y < m.y + h
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(i: u32) -> ItemKey {
        // 测试里需要一批互不相同的 key：直接按 FFI 编码构造（idx = i + 1, version = 0），
        // 不占用真实 SlotMap，也不需要任何共享状态。
        ItemKey::from_u64(i as u64 + 1)
    }

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

    #[test]
    fn new_rejects_bad_dims() {
        assert_eq!(GridContainer::new(0, 4).unwrap_err(), InventoryError::InvalidDef);
        assert_eq!(GridContainer::new(4, 0).unwrap_err(), InventoryError::InvalidDef);
        // 64x64 = 4096 格，正好在 MAX_CELLS 之内
        assert!(GridContainer::new(64, 64).is_ok());
        // 超过 4096 格
        assert!(GridContainer::new(65, 64).is_err());
        assert!(GridContainer::new(64, 65).is_err());
        assert!(GridContainer::new(64, 1).is_ok());
    }

    #[test]
    fn mask_switch_at_64_cells() {
        assert!(GridContainer::new(8, 8).unwrap().is_masked());
        assert!(!GridContainer::new(9, 8).unwrap().is_masked());
        assert!(GridContainer::new(64, 1).unwrap().is_masked());
        assert!(GridContainer::new(1, 64).unwrap().is_masked());
    }

    #[test]
    fn edge_and_corner_placements() {
        let mut g = GridContainer::new(10, 10).unwrap();
        assert!(g.can_place(0, 0, 10, 1));
        assert!(g.can_place(9, 9, 1, 1));
        assert!(!g.can_place(9, 9, 2, 1));
        assert!(!g.can_place(10, 0, 1, 1));
        assert!(!g.can_place(0, 0, 11, 1));
        assert!(!g.can_place(0, 0, 0, 1));

        g.place(key(0), meta(0, 0, 2, 2, false)).unwrap();
        assert!(!g.can_place(1, 1, 1, 1)); // 角点碰撞
        assert!(!g.can_place(0, 1, 4, 1));
        assert!(g.can_place(2, 0, 1, 2)); // 紧贴右边缘放，不重叠
        // 忽略自己后可以“原地重叠”
        assert!(g.can_place_excluding(0, 0, 2, 2, &[key(0)]));
        assert!(g.check().is_empty());
    }

    #[test]
    fn place_remove_roundtrip() {
        let mut g = GridContainer::new(6, 6).unwrap();
        let k = key(1);
        g.place(k, meta(2, 3, 3, 2, true)).unwrap();
        assert_eq!(g.placed_count(), 1);
        assert_eq!(g.slot_of(k).unwrap().x, 2);
        assert_eq!(g.place(k, meta(0, 0, 1, 1, false)), Err(InventoryError::AlreadyPlaced));
        assert_eq!(g.remove(key(9)), Err(InventoryError::NotPlaced));
        let old = g.remove(k).unwrap();
        assert_eq!((old.x, old.y), (2, 3));
        assert_eq!(g.placed_count(), 0);
        assert_eq!(g.occupancy_mask(), Some(0));
        assert!(g.can_place(2, 3, 3, 2));
        assert!(g.check().is_empty());
    }

    #[test]
    fn rotate_swaps_footprint_and_detects_block() {
        let mut g = GridContainer::new(4, 4).unwrap();
        let a = key(2);
        let b = key(3);
        g.place(a, meta(0, 0, 1, 3, true)).unwrap(); // 1x3：占 (0,0)(0,1)(0,2)
        g.place(b, meta(2, 0, 1, 1, false)).unwrap(); // 占 (2,0)
        // 旋转 a → 3x1，会盖住 (2,0) 上的 b
        assert_eq!(g.rotate(a), Err(InventoryError::RotationBlocked));
        assert!(!g.slot_of(a).unwrap().rotated);
        assert!(g.check().is_empty());

        // 把 b 挪开后再转，成功
        g.remove(b).unwrap();
        g.rotate(a).unwrap();
        assert!(g.slot_of(a).unwrap().rotated);
        assert_eq!(g.slot_of(a).unwrap().footprint(), (3, 1));
        assert!(g.check().is_empty());

        // 不可旋转的物品
        g.place(b, meta(0, 1, 1, 1, false)).unwrap();
        assert_eq!(g.rotate(b), Err(InventoryError::NotRotatable));

        // 换向放不下（越界）：2x2 网格里贴右边缘竖直放的 1x2，转 2x1 会超出右边界
        let mut g2 = GridContainer::new(2, 2).unwrap();
        let c = key(4);
        g2.place(c, meta(1, 0, 1, 2, true)).unwrap();
        assert_eq!(g2.rotate(c), Err(InventoryError::RotationBlocked));
        assert!(g2.check().is_empty());
        assert!(!g2.slot_of(c).unwrap().rotated);
    }

    /// `can_place_pair` 必须与“先移除两个物品、再依次落到新位置”的模拟结果一致。
    #[test]
    fn can_place_pair_matches_simulation() {
        for (gw, gh) in [(4u8, 4u8), (8, 8), (9, 9), (10, 10)] {
            let mut g = GridContainer::new(gw, gh).unwrap();
            let a = key(1);
            let b = key(2);
            let c = key(3);
            g.place(a, meta(0, 0, 2, 2, true)).unwrap();
            g.place(b, meta(2, 1, 1, 3, true)).unwrap();
            g.place(c, meta(gw - 2, 0, 2, 1, false)).unwrap();

            let probes: [(u8, u8, u8, u8); 6] = [
                (1, 0, 2, 2),
                (0, 0, 2, 2),
                (2, 0, 2, 2),
                (1, 1, 1, 3),
                (0, gh - 2, 3, 2),
                (gw - 1, gh - 1, 1, 1),
            ];
            for pa in probes {
                for pb in probes {
                    for (ra, rb) in [(false, false), (true, false), (false, true)] {
                        let new_a = PlacedMeta {
                            x: pa.0,
                            y: pa.1,
                            rotated: ra,
                            base_w: pa.2,
                            base_h: pa.3,
                            rotatable: true,
                        };
                        let new_b = PlacedMeta {
                            x: pb.0,
                            y: pb.1,
                            rotated: rb,
                            base_w: pb.2,
                            base_h: pb.3,
                            rotatable: true,
                        };
                        let fast = g.can_place_pair(a, &new_a, b, &new_b);
                        // 模拟：克隆 → 移除两个物品 → 依次落位（place 自带校验）
                        let mut sim = g.clone();
                        sim.remove(a).unwrap();
                        sim.remove(b).unwrap();
                        let slow = sim.place(a, new_a).is_ok() && sim.place(b, new_b).is_ok();
                        assert_eq!(
                            fast, slow,
                            "网格 {gw}x{gh} new_a={new_a:?} new_b={new_b:?}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn occupied_cells_counts_cells_not_items() {
        let mut g = GridContainer::new(4, 4).unwrap();
        assert_eq!(g.occupied_cells(), 0);
        g.place(key(1), meta(0, 0, 2, 3, false)).unwrap();
        assert_eq!(g.placed_count(), 1);
        assert_eq!(g.occupied_cells(), 6);
        g.remove(key(1)).unwrap();
        assert_eq!(g.occupied_cells(), 0);
    }

    #[test]
    fn mask_and_scan_agree() {
        let mut g = GridContainer::new(8, 8).unwrap();
        g.place(key(5), meta(0, 0, 3, 3, true)).unwrap();
        g.place(key(6), meta(5, 5, 2, 2, false)).unwrap();
        for x in 0..8u8 {
            for y in 0..8u8 {
                for (w, h) in [(1u8, 1u8), (2, 3), (3, 2), (4, 4)] {
                    assert_eq!(
                        g.can_place(x, y, w, h),
                        g.can_place_slow(x, y, w, h, &[]),
                        "位置 ({x},{y}) 尺寸 {w}x{h}"
                    );
                    assert_eq!(
                        g.can_place_excluding(x, y, w, h, &[key(5)]),
                        g.can_place_slow(x, y, w, h, &[key(5)]),
                    );
                }
            }
        }
    }
}
