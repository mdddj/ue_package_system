//! 一键自动整理：二维装箱。
//!
//! 步骤（对应实施计划阶段四）：
//! 1. 提取容器内全部顶层物品，按**面积降序**（或长边降序）排序；
//! 2. Best-Fit Decreasing 贪心：逐个物品枚举所有子网格的候选位置与两种朝向，
//!    选“最贴合”的那个（接触边最多：靠墙、靠已有物品）；
//! 3. 多种排序策略依次尝试，全部排不下返回 `None`（调用方保留原布局）。
//!
//! 算法本身是纯函数（不碰 [`crate::core::Inventory`]），
//! 结果在调用方的沙盒网格里二次校验后才提交。

use crate::core::ids::ItemKey;

/// 待排布的物品（几何信息已从网格里取好）。
#[derive(Copy, Clone, Debug)]
pub struct SortItem {
    pub key: ItemKey,
    pub w: u8,
    pub h: u8,
    pub rotatable: bool,
    /// 定义 ID：只用于排序的最终决胜键，保证结果确定性。
    pub def: u16,
}

/// 排布结果。
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Placement {
    pub key: ItemKey,
    pub part: u8,
    pub x: u8,
    pub y: u8,
    pub rotated: bool,
}

/// 排序策略。依次尝试，先成功者胜出。
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Strategy {
    /// 面积降序 + Best-Fit。
    BestFitByArea,
    /// 长边降序 + Best-Fit（面积降序放不下时的兜底）。
    BestFitByLongEdge,
    /// 面积降序 + First-Fit（更保守，贴近玩家的直觉摆法）。
    FirstFitByArea,
}

/// 计算排布方案。`None` = 该容器装不下全部物品。
pub fn plan(parts: &[(u8, u8)], items: &[SortItem]) -> Option<Vec<Placement>> {
    if items.is_empty() {
        return Some(Vec::new());
    }
    for strategy in [
        Strategy::BestFitByArea,
        Strategy::BestFitByLongEdge,
        Strategy::FirstFitByArea,
    ] {
        if let Some(p) = pack(parts, items, strategy) {
            return Some(p);
        }
    }
    None
}

fn pack(parts: &[(u8, u8)], items: &[SortItem], strategy: Strategy) -> Option<Vec<Placement>> {
    let mut grids: Vec<Scratch> = parts
        .iter()
        .map(|&(w, h)| Scratch::new(w, h))
        .collect();
    let mut sorted: Vec<SortItem> = items.to_vec();
    sorted.sort_by(|a, b| cmp_items(a, b, strategy));

    let mut out = Vec::with_capacity(sorted.len());
    for it in &sorted {
        let pick = best_position(&grids, it, strategy == Strategy::FirstFitByArea)?;
        grids[pick.part as usize].mark(pick.x, pick.y, it.w, it.h, pick.rotated);
        out.push(Placement {
            key: it.key,
            part: pick.part,
            x: pick.x,
            y: pick.y,
            rotated: pick.rotated,
        });
    }
    Some(out)
}

fn area(i: &SortItem) -> u32 {
    i.w as u32 * i.h as u32
}

fn long_edge(i: &SortItem) -> u8 {
    i.w.max(i.h)
}

fn short_edge(i: &SortItem) -> u8 {
    i.w.min(i.h)
}

/// 全序比较（最后一级决胜键是 key/def，确保任意输入下结果确定）。
fn cmp_items(a: &SortItem, b: &SortItem, s: Strategy) -> std::cmp::Ordering {
    use std::cmp::Ordering;
    let tie = |x: &SortItem, y: &SortItem| (x.key.to_u64(), x.def).cmp(&(y.key.to_u64(), y.def));
    let desc_u32 = |x: u32, y: u32| y.cmp(&x);
    let desc_u8 = |x: u8, y: u8| y.cmp(&x);
    match s {
        Strategy::BestFitByArea | Strategy::FirstFitByArea => desc_u32(area(a), area(b))
            .then_with(|| desc_u8(long_edge(a), long_edge(b)))
            .then_with(|| desc_u8(short_edge(a), short_edge(b)))
            .then_with(|| tie(a, b)),
        Strategy::BestFitByLongEdge => desc_u8(long_edge(a), long_edge(b))
            .then_with(|| desc_u32(area(a), area(b)))
            .then_with(|| tie(a, b)),
    }
    .then(Ordering::Equal)
}

#[derive(Copy, Clone, Debug)]
struct Candidate {
    part: u8,
    x: u8,
    y: u8,
    rotated: bool,
    score: i32,
}

/// 为一件物品挑最贴合的落点。`first_fit` 时不评分（行优先第一个能放的位置）。
fn best_position(grids: &[Scratch], it: &SortItem, first_fit: bool) -> Option<Candidate> {
    let mut best: Option<Candidate> = None;
    for (pi, g) in grids.iter().enumerate() {
        for rot in [false, true] {
            if rot && (!it.rotatable || it.w == it.h) {
                continue;
            }
            let (w, h) = if rot { (it.h, it.w) } else { (it.w, it.h) };
            if w > g.w || h > g.h {
                continue;
            }
            for y in 0..=(g.h - h) {
                for x in 0..=(g.w - w) {
                    if !g.free(x, y, w, h) {
                        continue;
                    }
                    if first_fit {
                        return Some(Candidate {
                            part: pi as u8,
                            x,
                            y,
                            rotated: rot,
                            score: 0,
                        });
                    }
                    let score = g.score(x, y, w, h);
                    let better = match &best {
                        None => true,
                        Some(b) => {
                            score > b.score
                                || (score == b.score
                                    && (y, x, pi as u8, rot) < (b.y, b.x, b.part, b.rotated))
                        }
                    };
                    if better {
                        best = Some(Candidate {
                            part: pi as u8,
                            x,
                            y,
                            rotated: rot,
                            score,
                        });
                    }
                }
            }
        }
    }
    best
}

/// 沙盒占用图。
struct Scratch {
    w: u8,
    h: u8,
    cells: Vec<bool>,
}

impl Scratch {
    fn new(w: u8, h: u8) -> Self {
        Self {
            w,
            h,
            cells: vec![false; w as usize * h as usize],
        }
    }

    fn free(&self, x: u8, y: u8, w: u8, h: u8) -> bool {
        for j in y..y + h {
            for i in x..x + w {
                if self.cells[j as usize * self.w as usize + i as usize] {
                    return false;
                }
            }
        }
        true
    }

    fn mark(&mut self, x: u8, y: u8, w: u8, h: u8, rotated: bool) {
        let (w, h) = if rotated { (h, w) } else { (w, h) };
        for j in y..y + h {
            for i in x..x + w {
                self.cells[j as usize * self.w as usize + i as usize] = true;
            }
        }
    }

    /// 贴合度：矩形每条边界上“接触墙或已占格”的单位边数。越大越紧凑。
    fn score(&self, x: u8, y: u8, w: u8, h: u8) -> i32 {
        let mut s = 0;
        for j in 0..h as i32 {
            for i in 0..w as i32 {
                let cx = x as i32 + i;
                let cy = y as i32 + j;
                for (dx, dy) in [(-1, 0), (1, 0), (0, -1), (0, 1)] {
                    let nx = cx + dx;
                    let ny = cy + dy;
                    let out_of_grid = nx < 0 || ny < 0 || nx >= self.w as i32 || ny >= self.h as i32;
                    // 贴着墙或贴着已占格，都算“贴合”
                    if out_of_grid || self.cells[ny as usize * self.w as usize + nx as usize] {
                        s += 1;
                    }
                }
            }
        }
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::ids::ItemKey;

    fn item(i: u32, w: u8, h: u8, rotatable: bool) -> SortItem {
        SortItem {
            key: ItemKey::from_u64(i as u64 + 1),
            w,
            h,
            rotatable,
            def: i as u16,
        }
    }

    #[test]
    fn empty_input_is_ok() {
        assert_eq!(plan(&[(4, 4)], &[]), Some(Vec::new()));
    }

    #[test]
    fn packs_simple_set() {
        // 4x4 里放 2x2 + 2x2 + 1x1
        let items = vec![item(0, 2, 2, false), item(1, 2, 2, false), item(2, 1, 1, false)];
        let out = plan(&[(4, 4)], &items).unwrap();
        assert_eq!(out.len(), 3);
        // 全部落在网格内且互不重叠 —— 用沙盒复算
        let mut scratch = Scratch::new(4, 4);
        for p in &out {
            let it = items.iter().find(|i| i.key == p.key).unwrap();
            let (w, h) = if p.rotated { (it.h, it.w) } else { (it.w, it.h) };
            assert!(scratch.free(p.x, p.y, w, h), "重叠: {p:?}");
            scratch.mark(p.x, p.y, it.w, it.h, p.rotated);
        }
    }

    #[test]
    fn rotation_is_used_when_needed() {
        // 2x1 竖条 + 网格 1x2：横放放不下，必须旋转
        let items = vec![item(0, 2, 1, true)];
        let out = plan(&[(1, 2)], &items).unwrap();
        assert!(out[0].rotated);
    }

    #[test]
    fn non_rotatable_fails_when_it_does_not_fit() {
        let items = vec![item(0, 2, 1, false)];
        assert!(plan(&[(1, 2)], &items).is_none());
    }

    #[test]
    fn too_many_items_fails() {
        let items = vec![item(0, 2, 2, false), item(1, 2, 2, false)];
        assert!(plan(&[(2, 2)], &items).is_none());
    }

    #[test]
    fn packs_across_parts() {
        let items = vec![item(0, 1, 2, false), item(1, 2, 1, true)];
        let out = plan(&[(1, 1), (2, 2)], &items).unwrap();
        assert_eq!(out.len(), 2);
    }

    #[test]
    fn deterministic_for_shuffled_input() {
        let a = vec![item(0, 2, 2, true), item(1, 1, 3, true), item(2, 2, 2, false)];
        let mut b = a.clone();
        b.reverse();
        assert_eq!(plan(&[(4, 4)], &a), plan(&[(4, 4)], &b));
    }

    #[test]
    fn tight_packing_of_many_small_items() {
        // 10x10 网格放 25 个 2x2：必然成功且无重叠
        let items: Vec<SortItem> = (0..25).map(|i| item(i, 2, 2, false)).collect();
        let out = plan(&[(10, 10)], &items).unwrap();
        assert_eq!(out.len(), 25);
        let mut scratch = Scratch::new(10, 10);
        for p in &out {
            assert_eq!(p.rotated, false);
            assert!(scratch.free(p.x, p.y, 2, 2));
            scratch.mark(p.x, p.y, 2, 2, false);
        }
    }
}
