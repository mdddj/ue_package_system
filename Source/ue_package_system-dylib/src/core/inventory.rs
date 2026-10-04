//! 中心化管理器：集中持有全部物品与容器，对外提供原子操作。
//!
//! ## 原子性怎么保证
//!
//! 所有修改操作都走**验证先行**的两阶段提交：
//! 1. 校验阶段：只读，把越界 / 重叠 / 成环 / 深度 / 黑名单 / 堆叠上限全部查清；
//! 2. 提交阶段：调 `place_validated` 之类的“不可能失败”的写操作，纯内存移动。
//!
//! 校验不过直接返回错误，一个字节都不改 —— 天然回滚，不存在“改到一半失败”的中间态。

use std::collections::{BTreeMap, BTreeSet};

use slotmap::{SecondaryMap, SlotMap};

use crate::algo::{autosort, nesting};

use super::catalog::Catalog;
use super::container::{validate_dims, Container, ContainerType, ROOT_TYPE_COUNT};
use super::error::{InvResult, InventoryError};
use super::grid::{GridContainer, PlacedMeta};
use super::ids::{ContainerKey, DefId, ItemKey};
use super::item::{ItemDef, ItemInstance};
use super::rules::Rules;
use super::snapshot::{ContainerSnap, ItemSnap, PlaceSnap, Snapshot};

/// 玩家四类根容器的槽位。
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Roots {
    pockets: Option<ContainerKey>,
    chest_rig: Option<ContainerKey>,
    safe_box: Option<ContainerKey>,
    backpack: Option<ContainerKey>,
}

impl Roots {
    /// 取某类根容器。
    pub fn get(&self, t: ContainerType) -> Option<ContainerKey> {
        match t {
            ContainerType::Pockets => self.pockets,
            ContainerType::ChestRig => self.chest_rig,
            ContainerType::SafeBox => self.safe_box,
            ContainerType::Backpack => self.backpack,
            ContainerType::Nested => None,
        }
    }

    /// 登记根容器。`Nested` 不是根类型，返回 `false`。
    pub(crate) fn set(&mut self, t: ContainerType, k: ContainerKey) -> bool {
        match t {
            ContainerType::Pockets => self.pockets = Some(k),
            ContainerType::ChestRig => self.chest_rig = Some(k),
            ContainerType::SafeBox => self.safe_box = Some(k),
            ContainerType::Backpack => self.backpack = Some(k),
            ContainerType::Nested => return false,
        }
        true
    }

    /// 按固定顺序（口袋 → 弹挂 → 安全箱 → 背包）遍历已存在的根容器。
    pub fn iter(&self) -> impl Iterator<Item = (ContainerType, ContainerKey)> + '_ {
        ContainerType::ROOTS
            .iter()
            .filter_map(move |t| self.get(*t).map(|k| (*t, k)))
    }
}

/// 聚合统计：总负重 / 总估值 / 物品件数（含嵌套内部的东西）。
#[derive(Copy, Clone, Debug, Default, PartialEq)]
pub struct Aggregates {
    /// 总负重：Σ 单件重量 × 堆叠数量（含套包内部）。
    pub weight: f64,
    /// 总估值：Σ 单件估值 × 堆叠数量。
    pub value: u64,
    /// 物品实例数量（堆叠算一件）。
    pub item_count: u32,
}

/// 背包数据总入口。一个玩家（或一个测试沙盒）一个实例。
#[derive(Debug)]
pub struct Inventory {
    catalog: Catalog,
    items: SlotMap<ItemKey, ItemInstance>,
    containers: SlotMap<ContainerKey, Container>,
    /// 嵌套容器的归属物品（根容器没有归属）。
    container_owner: SecondaryMap<ContainerKey, ItemKey>,
    roots: Roots,
    rules: Rules,
}

impl Default for Inventory {
    fn default() -> Self {
        Self::new()
    }
}

impl Inventory {
    pub fn new() -> Self {
        Self {
            catalog: Catalog::new(),
            items: SlotMap::with_key(),
            containers: SlotMap::with_key(),
            container_owner: SecondaryMap::new(),
            roots: Roots::default(),
            rules: Rules::default(),
        }
    }

    // ==================== 目录 ====================

    pub fn catalog(&self) -> &Catalog {
        &self.catalog
    }

    /// 注册物品定义。
    pub fn define_item(&mut self, def: ItemDef) -> InvResult<DefId> {
        self.catalog.define(def)
    }

    // ==================== 规则 ====================

    pub fn rules(&self) -> Rules {
        self.rules
    }

    pub fn set_rules(&mut self, rules: Rules) {
        self.rules = rules;
    }

    /// 根容器槽位表。
    pub fn roots(&self) -> &Roots {
        &self.roots
    }

    /// 取某类根容器。
    pub fn root(&self, t: ContainerType) -> Option<ContainerKey> {
        self.roots.get(t)
    }

    // ==================== 只读访问 ====================

    pub fn container(&self, k: ContainerKey) -> InvResult<&Container> {
        self.containers
            .get(k)
            .ok_or(InventoryError::ContainerNotFound)
    }

    pub fn item(&self, k: ItemKey) -> InvResult<&ItemInstance> {
        self.items.get(k).ok_or(InventoryError::ItemNotFound)
    }

    pub fn items(&self) -> impl Iterator<Item = (ItemKey, &ItemInstance)> {
        self.items.iter()
    }

    pub fn containers(&self) -> impl Iterator<Item = (ContainerKey, &Container)> {
        self.containers.iter()
    }

    pub fn item_count(&self) -> usize {
        self.items.len()
    }

    pub fn container_count(&self) -> usize {
        self.containers.len()
    }

    /// 物品定义。
    pub fn def_of(&self, item: ItemKey) -> InvResult<&ItemDef> {
        let inst = self.item(item)?;
        self.catalog.get(inst.def)
    }

    fn grid(&self, c: ContainerKey, part: u8) -> InvResult<&GridContainer> {
        self.container(c)?
            .part(part as usize)
            .ok_or(InventoryError::PartOutOfRange)
    }

    fn grid_mut(&mut self, c: ContainerKey, part: u8) -> InvResult<&mut GridContainer> {
        self.containers
            .get_mut(c)
            .ok_or(InventoryError::ContainerNotFound)?
            .part_mut(part as usize)
            .ok_or(InventoryError::PartOutOfRange)
    }

    /// 物品当前在哪个容器的哪块子网格、落位信息。
    ///
    /// 扫描全部容器（容器数是个位数量级，代价可忽略）；热路径是网格级 `can_place`，不经过这里。
    pub fn container_of_item(&self, item: ItemKey) -> Option<(ContainerKey, u8, PlacedMeta)> {
        for (ck, ct) in self.containers.iter() {
            for (pi, part) in ct.parts().iter().enumerate() {
                if let Some(meta) = part.slot_of(item) {
                    return Some((ck, pi as u8, meta));
                }
            }
        }
        None
    }

    /// 物品所在容器的**根类型**（沿归属链上溯到口袋/弹挂/安全箱/背包）。
    pub fn root_type_of(&self, c: ContainerKey) -> InvResult<ContainerType> {
        let mut cur = c;
        for _ in 0..=self.containers.len() {
            let ct = self.container(cur)?;
            if ct.ctype() != ContainerType::Nested {
                return Ok(ct.ctype());
            }
            cur = self.owner_host(cur)?;
        }
        Err(InventoryError::CorruptState)
    }

    /// 容器在嵌套树里的深度（根容器 = 1）。
    pub fn depth_of(&self, c: ContainerKey) -> InvResult<u8> {
        let mut cur = c;
        let mut depth: u8 = 1;
        for _ in 0..=self.containers.len() {
            let ct = self.container(cur)?;
            if ct.ctype() != ContainerType::Nested {
                return Ok(depth);
            }
            cur = self.owner_host(cur)?;
            depth = depth.saturating_add(1);
        }
        Err(InventoryError::CorruptState)
    }

    /// 归属物品（`c` 是这件物品的内部空间）以及这件物品所在的宿主容器。
    fn owner_host(&self, c: ContainerKey) -> InvResult<ContainerKey> {
        let owner = self
            .container_owner
            .get(c)
            .copied()
            .ok_or(InventoryError::CorruptState)?;
        let (host, _, _) = self
            .container_of_item(owner)
            .ok_or(InventoryError::CorruptState)?;
        Ok(host)
    }

    /// `c` 的直接子容器（放在 `c` 里的套包自带的空间）。
    pub fn child_containers(&self, c: ContainerKey) -> Vec<ContainerKey> {
        nesting::child_containers(self, c)
    }

    /// 容器里的顶层物品（不递归），确定性顺序。
    pub fn items_in(&self, c: ContainerKey) -> Vec<(ItemKey, u8, PlacedMeta)> {
        let mut out = Vec::new();
        if let Ok(ct) = self.container(c) {
            for (pi, part) in ct.parts().iter().enumerate() {
                for (key, meta) in part.occupants() {
                    out.push((key, pi as u8, meta));
                }
            }
        }
        out
    }

    /// 全部物品 ID（按落位可达顺序，确定性）。
    pub fn all_items(&self) -> Vec<ItemKey> {
        let mut out = Vec::new();
        for c in nesting::reachable_containers(self) {
            for (key, _, _) in self.items_in(c) {
                out.push(key);
            }
        }
        out
    }

    /// 全部容器 ID（根在前，确定性顺序）。
    pub fn all_containers(&self) -> Vec<ContainerKey> {
        nesting::reachable_containers(self)
    }

    /// 标签查询（可递归进套包内部）。
    ///
    /// “当前能直接换弹的弹匣” = `find_by_tag(tags::MAGAZINE)` 再按容器的根类型过滤。
    pub fn find_by_tag(&self, tag_mask: u32) -> Vec<ItemKey> {
        let mut out = Vec::new();
        for c in nesting::reachable_containers(self) {
            for (key, _, _) in self.items_in(c) {
                if let Ok(def) = self.def_of(key) {
                    if def.tags & tag_mask != 0 {
                        out.push(key);
                    }
                }
            }
        }
        out
    }

    /// 聚合统计（重量 / 估值 / 件数）。O(物品数)，每次实时计算。
    pub fn aggregates(&self) -> Aggregates {
        let mut agg = Aggregates {
            weight: 0.0,
            value: 0,
            item_count: self.items.len() as u32,
        };
        for c in nesting::reachable_containers(self) {
            for (key, _, _) in self.items_in(c) {
                let Ok(inst) = self.item(key) else { continue };
                let Ok(def) = self.catalog.get(inst.def) else {
                    continue;
                };
                let n = inst.stack as f64;
                agg.weight += def.weight as f64 * n;
                agg.value += def.value as u64 * inst.stack as u64;
            }
        }
        agg
    }

    /// 总负重（kg，含套包内部）。
    pub fn total_weight(&self) -> f64 {
        self.aggregates().weight
    }

    /// 负重比 = 总重 ÷ 承重上限。
    ///
    /// - 未设上限（[`Rules::has_capacity_limit`] == false）→ 恒为 `0.0`；
    /// - 超重时大于 `1.0`（例：`1.35` = 超重 35%），游戏侧可直接拿它套速度/体力曲线；
    /// - 注意：这里是**比例**而不是“剩余可携带重量”，调用方要判空负重自己换算。
    pub fn weight_ratio(&self) -> f32 {
        let max = self.rules.max_capacity_kg;
        if !max.is_finite() || max <= 0.0 {
            return 0.0;
        }
        (self.total_weight() / max as f64) as f32
    }

    /// 是否超重（负重比 > 1.0）。未设上限时恒为 `false`。
    pub fn is_overloaded(&self) -> bool {
        self.weight_ratio() > 1.0
    }

    // ==================== 容器创建 ====================

    /// 创建并登记一个根容器（口袋 / 弹挂 / 安全箱 / 背包）。
    ///
    /// 每类根容器只允许一个；重复创建返回 [`InventoryError::AlreadyExists`]。
    pub fn add_root(
        &mut self,
        ctype: ContainerType,
        label: &str,
        dims: &[(u8, u8)],
    ) -> InvResult<ContainerKey> {
        if !ctype.is_root() {
            return Err(InventoryError::InvalidDef);
        }
        if self.roots.get(ctype).is_some() {
            return Err(InventoryError::AlreadyExists);
        }
        validate_dims(dims)?;
        let dims_owned = dims.to_vec();
        let label_owned = label.to_string();
        let key = self.containers.insert_with_key(|k| {
            Container::new(k, &label_owned, ctype, &dims_owned).expect("dims 已校验过")
        });
        self.roots.set(ctype, key);
        Ok(key)
    }

    // ==================== 放置规则 ====================

    /// 把一件物品放进 `dst` 之前的规则校验（只读）。
    ///
    /// `own_container`：物品自带的容器（套包）；新物品传 `None`。
    fn check_placement_rules(
        &self,
        def: &ItemDef,
        dst: ContainerKey,
        own_container: Option<ContainerKey>,
    ) -> InvResult<()> {
        let root = self.root_type_of(dst)?;
        if def.forbidden_container_types & root.bit() != 0 {
            return Err(InventoryError::ForbiddenContainer);
        }
        match own_container {
            Some(cx) => {
                // 套包放进自己的子树 = 成环
                if nesting::subtree_contains(self, cx, dst) {
                    return Err(InventoryError::WouldCycle);
                }
                let total = self.depth_of(dst)? as u16 + nesting::subtree_height(self, cx) as u16;
                if total > self.rules.max_nesting_depth as u16 {
                    return Err(InventoryError::NestingTooDeep);
                }
            }
            None => {
                if def.container.is_some() {
                    let total = self.depth_of(dst)? as u16 + 1;
                    if total > self.rules.max_nesting_depth as u16 {
                        return Err(InventoryError::NestingTooDeep);
                    }
                }
            }
        }
        Ok(())
    }

    fn classify_place(&self, c: ContainerKey, part: u8, x: u8, y: u8, w: u8, h: u8) -> InventoryError {
        match self.grid(c, part) {
            Ok(g) => {
                if g.bounds_ok(x, y, w, h) {
                    InventoryError::Occupied
                } else {
                    InventoryError::OutOfBounds
                }
            }
            Err(e) => e,
        }
    }

    // ==================== 增删 ====================

    /// 生成一件物品并放进 `(dst, part)` 的 `(x, y)`。
    ///
    /// 物品定义带容器规格时（套包），同时为它创建内部容器。
    // 落位天然需要（定义、数量、耐久、容器、part、x、y、朝向）这些参数，拆结构体反而更绕。
    #[allow(clippy::too_many_arguments)]
    pub fn add_item(
        &mut self,
        def_id: DefId,
        stack: u32,
        durability: u16,
        dst: ContainerKey,
        part: u8,
        x: u8,
        y: u8,
        rotated: bool,
    ) -> InvResult<ItemKey> {
        let def = self.catalog.get(def_id)?.clone();
        if stack == 0 || stack > def.max_stack {
            return Err(InventoryError::InvalidStackCount);
        }
        self.grid(dst, part)?;
        if rotated && !def.rotatable {
            return Err(InventoryError::NotRotatable);
        }
        let (w, h) = def.footprint(rotated);
        self.check_placement_rules(&def, dst, None)?;
        if !self.grid(dst, part)?.can_place(x, y, w, h) {
            return Err(self.classify_place(dst, part, x, y, w, h));
        }

        // —— 提交阶段：以下不可能失败 ——
        let key = self.items.insert(ItemInstance {
            def: def_id,
            stack,
            durability,
            container: None,
        });
        self.grid_mut(dst, part)?.place_validated(
            key,
            PlacedMeta {
                x,
                y,
                rotated,
                base_w: def.w,
                base_h: def.h,
                rotatable: def.rotatable,
            },
        );
        if let Some(spec) = &def.container {
            let label = def.name.clone();
            let dims = spec.parts.clone();
            let ck = self.containers.insert_with_key(|k| {
                Container::new(k, &label, ContainerType::Nested, &dims).expect("spec 已在定义时校验")
            });
            self.container_owner.insert(ck, key);
            self.items
                .get_mut(key)
                .expect("刚插入的物品必然存在")
                .container = Some(ck);
        }
        Ok(key)
    }

    /// 彻底删除一件物品及其套包里的全部内容。返回被删除的物品件数（含子树）。
    pub fn destroy_item(&mut self, root: ItemKey) -> InvResult<u32> {
        if !self.items.contains_key(root) {
            return Err(InventoryError::ItemNotFound);
        }
        // 收集子树（显式栈，不用递归）
        let mut doomed_items: Vec<ItemKey> = Vec::new();
        let mut doomed_containers: Vec<ContainerKey> = Vec::new();
        let mut stack = vec![root];
        let mut seen: BTreeSet<ItemKey> = BTreeSet::new();
        while let Some(k) = stack.pop() {
            if !seen.insert(k) {
                continue;
            }
            doomed_items.push(k);
            if let Some(c) = self.items.get(k).and_then(|i| i.container) {
                doomed_containers.push(c);
                for (child, _, _) in self.items_in(c) {
                    stack.push(child);
                }
            }
        }
        // 先摘出网格，再删容器，最后删物品
        for &k in &doomed_items {
            if let Some((c, p, _)) = self.container_of_item(k) {
                if let Ok(g) = self.grid_mut(c, p) {
                    let _ = g.remove(k);
                }
            }
        }
        for &c in &doomed_containers {
            self.container_owner.remove(c);
            self.containers.remove(c);
        }
        let mut destroyed = 0u32;
        for &k in &doomed_items {
            if self.items.remove(k).is_some() {
                destroyed += 1;
            }
        }
        Ok(destroyed)
    }

    // ==================== 原子交互 ====================

    /// 跨容器 / 容器内移动。失败时不做任何改动。
    pub fn try_move(
        &mut self,
        item: ItemKey,
        dst: ContainerKey,
        part: u8,
        x: u8,
        y: u8,
        rotated: bool,
    ) -> InvResult<()> {
        let def = self.def_of(item)?.clone();
        let (src_c, src_p, _src_meta) = self
            .container_of_item(item)
            .ok_or(InventoryError::NotPlaced)?;
        self.grid(dst, part)?;
        if rotated && !def.rotatable {
            return Err(InventoryError::NotRotatable);
        }
        let (w, h) = def.footprint(rotated);
        let own_container = self.item(item)?.container;
        self.check_placement_rules(&def, dst, own_container)?;

        let new_meta = PlacedMeta {
            x,
            y,
            rotated,
            base_w: def.w,
            base_h: def.h,
            rotatable: def.rotatable,
        };
        let same_grid = src_c == dst && src_p == part;

        // 校验阶段
        if same_grid {
            if !self.grid(dst, part)?.can_place_excluding(x, y, w, h, &[item]) {
                return Err(self.classify_place(dst, part, x, y, w, h));
            }
        } else if !self.grid(dst, part)?.can_place(x, y, w, h) {
            return Err(self.classify_place(dst, part, x, y, w, h));
        }

        // 提交阶段
        if same_grid {
            let g = self.grid_mut(dst, part)?;
            g.remove(item)?;
            g.place_validated(item, new_meta);
        } else {
            self.grid_mut(src_c, src_p)?.remove(item)?;
            self.grid_mut(dst, part)?.place_validated(item, new_meta);
        }
        Ok(())
    }

    /// 两个物品原子互换位置（可以跨容器）。
    ///
    /// 朝向策略：各自优先保持当前朝向，放不下再尝试翻转（可旋转的才行）；
    /// 两边都放得下才提交，否则原样返回错误。
    pub fn try_swap(&mut self, a: ItemKey, b: ItemKey) -> InvResult<()> {
        if a == b {
            return Err(InventoryError::InvalidArgument);
        }
        let (ac, ap, am) = self.container_of_item(a).ok_or(InventoryError::NotPlaced)?;
        let (bc, bp, bm) = self.container_of_item(b).ok_or(InventoryError::NotPlaced)?;
        let da = self.def_of(a)?.clone();
        let db = self.def_of(b)?.clone();

        // 规则校验（成环 / 深度 / 黑名单），两个方向都要过
        self.check_placement_rules(&da, bc, self.item(a)?.container)?;
        self.check_placement_rules(&db, ac, self.item(b)?.container)?;

        let rot_a = self
            .choose_rotation(&da, bc, bp, bm.x, bm.y, &[a, b], am.rotated)
            .ok_or_else(|| self.classify_swap(&da, bc, bp, bm.x, bm.y))?;
        let rot_b = self
            .choose_rotation(&db, ac, ap, am.x, am.y, &[a, b], bm.rotated)
            .ok_or_else(|| self.classify_swap(&db, ac, ap, am.x, am.y))?;

        let new_a = PlacedMeta {
            x: bm.x,
            y: bm.y,
            rotated: rot_a,
            base_w: da.w,
            base_h: da.h,
            rotatable: da.rotatable,
        };
        let new_b = PlacedMeta {
            x: am.x,
            y: am.y,
            rotated: rot_b,
            base_w: db.w,
            base_h: db.h,
            rotatable: db.rotatable,
        };

        if ac == bc && ap == bp {
            // 同一块网格：两侧都要避开对方的新占位，用成对检查定案
            if !self.grid(ac, ap)?.can_place_pair(a, &new_a, b, &new_b) {
                return Err(InventoryError::Occupied);
            }
            let g = self.grid_mut(ac, ap)?;
            g.remove(a)?;
            g.remove(b)?;
            g.place_validated(a, new_a);
            g.place_validated(b, new_b);
        } else {
            // 不同网格：各自先让位，再放进对方的位置
            self.grid_mut(ac, ap)?.remove(a)?;
            self.grid_mut(bc, bp)?.remove(b)?;
            self.grid_mut(ac, ap)?.place_validated(b, new_b);
            self.grid_mut(bc, bp)?.place_validated(a, new_a);
        }
        Ok(())
    }

    /// 原地旋转。
    pub fn try_rotate(&mut self, item: ItemKey) -> InvResult<()> {
        let (c, p, _) = self.container_of_item(item).ok_or(InventoryError::NotPlaced)?;
        self.grid_mut(c, p)?.rotate(item)
    }

    /// 把 `src` 的堆叠合并进 `dst`（同一定义、可堆叠）。返回实际转移的数量。
    ///
    /// `src` 被合并空后自动销毁；耐久取 `dst` 的（合并规则由游戏层决定，这里不参与校验）。
    pub fn try_merge(&mut self, src: ItemKey, dst: ItemKey) -> InvResult<u32> {
        if src == dst {
            return Err(InventoryError::InvalidArgument);
        }
        let src_inst = self.item(src)?.clone();
        let dst_inst = self.item(dst)?.clone();
        if src_inst.def != dst_inst.def {
            return Err(InventoryError::StackUnmergeable);
        }
        let def = self.catalog.get(src_inst.def)?.clone();
        if def.max_stack <= 1 {
            return Err(InventoryError::StackUnmergeable);
        }
        let space = def.max_stack - dst_inst.stack;
        if space == 0 {
            return Err(InventoryError::StackFull);
        }
        let moved = src_inst.stack.min(space);
        let (sc, sp, _) = self
            .container_of_item(src)
            .ok_or(InventoryError::NotPlaced)?;

        // 提交
        self.items
            .get_mut(dst)
            .expect("刚刚还在")
            .stack = dst_inst.stack + moved;
        let left = src_inst.stack - moved;
        self.items.get_mut(src).expect("刚刚还在").stack = left;
        if left == 0 {
            self.grid_mut(sc, sp)?.remove(src)?;
            self.items.remove(src);
        }
        Ok(moved)
    }

    /// 从 `item` 拆出 `count` 个，放进 `(dst, part)` 的 `(x, y)`。返回新物品 ID。
    ///
    /// `count` 必须 `1..item.stack`（等于全部数量请直接用 `try_move`）。
    #[allow(clippy::too_many_arguments)]
    pub fn try_split(
        &mut self,
        item: ItemKey,
        count: u32,
        dst: ContainerKey,
        part: u8,
        x: u8,
        y: u8,
        rotated: bool,
    ) -> InvResult<ItemKey> {
        let def = self.def_of(item)?.clone();
        if def.max_stack <= 1 {
            return Err(InventoryError::StackUnmergeable);
        }
        let cur = self.item(item)?.stack;
        if count == 0 || count >= cur {
            return Err(InventoryError::InvalidStackCount);
        }
        if rotated && !def.rotatable {
            return Err(InventoryError::NotRotatable);
        }
        let (w, h) = def.footprint(rotated);
        self.grid(dst, part)?;
        self.check_placement_rules(&def, dst, None)?;
        if !self.grid(dst, part)?.can_place(x, y, w, h) {
            return Err(self.classify_place(dst, part, x, y, w, h));
        }
        let durability = self.item(item)?.durability;

        // 提交
        self.items.get_mut(item).expect("刚刚还在").stack = cur - count;
        let new_key = self.items.insert(ItemInstance {
            def: self.item(item)?.def,
            stack: count,
            durability,
            container: None,
        });
        self.grid_mut(dst, part)?.place_validated(
            new_key,
            PlacedMeta {
                x,
                y,
                rotated,
                base_w: def.w,
                base_h: def.h,
                rotatable: def.rotatable,
            },
        );
        Ok(new_key)
    }

    /// 一键整理：把 `c` 里的顶层物品重排成更紧凑的布局。
    ///
    /// 计算在沙盒网格里完成，任何一步不满足（放不下 / 数量不守恒 / 自检失败）
    /// 都直接返回 [`InventoryError::SortFailed`]，原布局一个字节不改。
    pub fn autosort(&mut self, c: ContainerKey) -> InvResult<()> {
        let (dims, sort_items, metas) = {
            let ct = self.container(c)?;
            let dims = ct.dims();
            let mut sort_items = Vec::new();
            let mut metas: BTreeMap<ItemKey, PlacedMeta> = BTreeMap::new();
            for part in ct.parts() {
                for (key, meta) in part.occupants() {
                    let inst = self.item(key)?;
                    sort_items.push(autosort::SortItem {
                        key,
                        w: meta.base_w,
                        h: meta.base_h,
                        rotatable: meta.rotatable,
                        def: inst.def.0,
                    });
                    metas.insert(key, meta);
                }
            }
            (dims, sort_items, metas)
        };
        if sort_items.is_empty() {
            return Ok(());
        }

        let planned = autosort::plan(&dims, &sort_items).ok_or(InventoryError::SortFailed)?;

        // 沙盒组装：先在全新网格里完整排一遍（place 自带校验，算法 bug 会被这里挡下）
        let mut new_parts: Vec<GridContainer> = dims
            .iter()
            .map(|&(w, h)| GridContainer::new(w, h))
            .collect::<InvResult<Vec<_>>>()?;
        for p in &planned {
            let Some(base) = metas.get(&p.key) else {
                return Err(InventoryError::SortFailed);
            };
            let meta = PlacedMeta {
                x: p.x,
                y: p.y,
                rotated: p.rotated,
                base_w: base.base_w,
                base_h: base.base_h,
                rotatable: base.rotatable,
            };
            let Some(grid) = new_parts.get_mut(p.part as usize) else {
                return Err(InventoryError::SortFailed);
            };
            if grid.place(p.key, meta).is_err() {
                return Err(InventoryError::SortFailed);
            }
        }

        // 二次校验：数量守恒 + 网格自检
        let placed: usize = new_parts.iter().map(|p| p.placed_count()).sum();
        if placed != metas.len() {
            return Err(InventoryError::SortFailed);
        }
        for np in &new_parts {
            if !np.check().is_empty() {
                return Err(InventoryError::SortFailed);
            }
        }

        // 提交：整体替换，原子
        self.containers
            .get_mut(c)
            .ok_or(InventoryError::ContainerNotFound)?
            .replace_parts(new_parts);
        Ok(())
    }

    // ==================== 内部工具 ====================

    /// 选择朝向：优先 `prefer`，不行再翻，都不行返回 `None`。
    #[allow(clippy::too_many_arguments)]
    fn choose_rotation(
        &self,
        def: &ItemDef,
        c: ContainerKey,
        part: u8,
        x: u8,
        y: u8,
        ignore: &[ItemKey],
        prefer: bool,
    ) -> Option<bool> {
        let g = self.grid(c, part).ok()?;
        for cand in [prefer, !prefer] {
            if cand && !def.rotatable {
                continue;
            }
            let (w, h) = def.footprint(cand);
            if g.can_place_excluding(x, y, w, h, ignore) {
                return Some(cand);
            }
        }
        None
    }

    fn classify_swap(&self, def: &ItemDef, c: ContainerKey, part: u8, x: u8, y: u8) -> InventoryError {
        let Ok(g) = self.grid(c, part) else {
            return InventoryError::ContainerNotFound;
        };
        let (uw, uh) = def.footprint(false);
        let (rw, rh) = def.footprint(true);
        let unrot_ok = g.bounds_ok(x, y, uw, uh);
        let rot_ok = def.rotatable && g.bounds_ok(x, y, rw, rh);
        if !unrot_ok && !rot_ok {
            InventoryError::OutOfBounds
        } else {
            InventoryError::Occupied
        }
    }

    // ==================== 快照 ====================

    /// 导出紧凑快照（容器与物品按确定性顺序排列，bincode 编码后可直接进网络包 / 存档）。
    ///
    /// 注意：快照不含目录（[`Catalog`]）—— 定义属于内容数据，两边必须一致。
    pub fn snapshot(&self) -> Snapshot {
        // 1. 容器顺序：根固定顺序前序 DFS
        let mut c_order: Vec<ContainerKey> = Vec::new();
        let mut seen: BTreeSet<ContainerKey> = BTreeSet::new();
        let mut stack: Vec<ContainerKey> = self.roots.iter().map(|(_, k)| k).collect();
        stack.reverse();
        while let Some(c) = stack.pop() {
            if !seen.insert(c) {
                continue;
            }
            c_order.push(c);
            for ch in self.child_containers(c).into_iter().rev() {
                stack.push(ch);
            }
        }
        let c_index: BTreeMap<ContainerKey, u16> = c_order
            .iter()
            .enumerate()
            .map(|(i, k)| (*k, i as u16))
            .collect();

        // 2. 物品顺序：按容器顺序、part 顺序、网格内 key 顺序（确定性）
        let mut items: Vec<ItemSnap> = Vec::new();
        for c in &c_order {
            let Ok(ct) = self.container(*c) else { continue };
            for (pi, part) in ct.parts().iter().enumerate() {
                for (key, meta) in part.occupants() {
                    let Ok(inst) = self.item(key) else { continue };
                    items.push(ItemSnap {
                        def: inst.def.0,
                        stack: inst.stack,
                        durability: inst.durability,
                        container: inst
                            .container
                            .and_then(|ck| c_index.get(&ck).copied()),
                        place: PlaceSnap {
                            container: c_index[c],
                            part: pi as u8,
                            x: meta.x,
                            y: meta.y,
                            rotated: meta.rotated,
                        },
                    });
                }
            }
        }

        // 3. 容器快照与根表
        let containers: Vec<ContainerSnap> = c_order
            .iter()
            .filter_map(|c| {
                let ct = self.container(*c).ok()?;
                Some(ContainerSnap {
                    label: ct.label().to_string(),
                    ctype: ct.ctype() as u8,
                    parts: ct.dims(),
                })
            })
            .collect();
        let mut roots: [Option<u16>; ROOT_TYPE_COUNT] = [None; ROOT_TYPE_COUNT];
        for (t, k) in self.roots.iter() {
            if let Some(i) = t.root_index() {
                roots[i] = c_index.get(&k).copied();
            }
        }

        Snapshot {
            roots,
            containers,
            items,
        }
    }

    /// 从快照恢复（覆盖当前全部物品 / 容器 / 根表；目录与规则保持不变）。
    ///
    /// 先在临时状态里完整重建并校验，全部通过才整体替换 —— 损坏的快照不会破坏现有数据。
    pub fn restore(&mut self, snap: &Snapshot) -> InvResult<()> {
        let mut items: SlotMap<ItemKey, ItemInstance> = SlotMap::with_key();
        let mut containers: SlotMap<ContainerKey, Container> = SlotMap::with_key();
        let mut owner: SecondaryMap<ContainerKey, ItemKey> = SecondaryMap::new();

        // 1. 容器
        let mut cks: Vec<ContainerKey> = Vec::with_capacity(snap.containers.len());
        for cs in &snap.containers {
            let ctype = ContainerType::from_u8(cs.ctype).ok_or(InventoryError::CorruptSnapshot)?;
            if validate_dims(&cs.parts).is_err() {
                return Err(InventoryError::CorruptSnapshot);
            }
            let label = cs.label.clone();
            let dims = cs.parts.clone();
            let ck = containers.insert_with_key(|k| {
                Container::new(k, &label, ctype, &dims).expect("dims 已校验过")
            });
            cks.push(ck);
        }

        // 2. 物品实例
        let mut iks: Vec<ItemKey> = Vec::with_capacity(snap.items.len());
        for is in &snap.items {
            let (max_stack, has_container) = {
                let def = self
                    .catalog
                    .get(DefId(is.def))
                    .map_err(|_| InventoryError::CorruptSnapshot)?;
                (def.max_stack, def.container.is_some())
            };
            if is.stack == 0 || is.stack > max_stack || is.container.is_some() != has_container {
                return Err(InventoryError::CorruptSnapshot);
            }
            let ik = items.insert(ItemInstance {
                def: DefId(is.def),
                stack: is.stack,
                durability: is.durability,
                container: None,
            });
            iks.push(ik);
        }

        // 3. 落位 + 建立套包链接
        for (idx, is) in snap.items.iter().enumerate() {
            let ik = iks[idx];
            let (dw, dh, rotatable) = {
                let def = self
                    .catalog
                    .get(DefId(is.def))
                    .map_err(|_| InventoryError::CorruptSnapshot)?;
                (def.w, def.h, def.rotatable)
            };
            if is.place.rotated && !rotatable {
                return Err(InventoryError::CorruptSnapshot);
            }
            let ck = *cks
                .get(is.place.container as usize)
                .ok_or(InventoryError::CorruptSnapshot)?;
            let meta = PlacedMeta {
                x: is.place.x,
                y: is.place.y,
                rotated: is.place.rotated,
                base_w: dw,
                base_h: dh,
                rotatable,
            };
            let ct = containers
                .get_mut(ck)
                .ok_or(InventoryError::CorruptSnapshot)?;
            let part = ct
                .part_mut(is.place.part as usize)
                .ok_or(InventoryError::CorruptSnapshot)?;
            part.place(ik, meta)
                .map_err(|_| InventoryError::CorruptSnapshot)?;
            if let Some(ci) = is.container {
                let child = *cks
                    .get(ci as usize)
                    .ok_or(InventoryError::CorruptSnapshot)?;
                if owner.insert(child, ik).is_some() {
                    return Err(InventoryError::CorruptSnapshot);
                }
                items
                    .get_mut(ik)
                    .ok_or(InventoryError::CorruptSnapshot)?
                    .container = Some(child);
            }
        }

        // 4. 根表
        let mut roots = Roots::default();
        for (i, r) in snap.roots.iter().enumerate() {
            let Some(ci) = r else { continue };
            let ck = *cks.get(*ci as usize).ok_or(InventoryError::CorruptSnapshot)?;
            let t = ContainerType::ROOTS[i];
            let ct = containers
                .get(ck)
                .ok_or(InventoryError::CorruptSnapshot)?;
            if ct.ctype() != t || !roots.set(t, ck) {
                return Err(InventoryError::CorruptSnapshot);
            }
        }

        // 5. 用完整自检把关：归属链自洽、全容器可达（无环）、嵌套深度不超上限。
        //
        // 组装在临时实例里做，检查通过才把字段搬进 self —— 损坏的快照永远不会污染现有数据。
        {
            let mut probe = Inventory::new();
            probe.catalog = self.catalog.clone();
            probe.rules = self.rules;
            probe.items = items;
            probe.containers = containers;
            probe.container_owner = owner;
            probe.roots = roots;
            if !probe.check_invariants().is_empty() {
                return Err(InventoryError::CorruptSnapshot);
            }
            self.items = probe.items;
            self.containers = probe.containers;
            self.container_owner = probe.container_owner;
            self.roots = probe.roots;
        }
        Ok(())
    }

    // ==================== 自检 ====================

    /// 全库一致性自检。返回空列表表示一切正常（测试与调试用）。
    pub fn check_invariants(&self) -> Vec<String> {
        let mut errs = Vec::new();

        // 每个物品恰好落位一次；网格里没有幽灵物品
        let mut placed_count: BTreeMap<ItemKey, usize> = BTreeMap::new();
        for (ck, ct) in self.containers.iter() {
            for (pi, part) in ct.parts().iter().enumerate() {
                for e in part.check() {
                    errs.push(format!("容器 {ck:?} part {pi}: {e}"));
                }
                for (k, _) in part.occupants() {
                    *placed_count.entry(k).or_default() += 1;
                }
            }
        }
        for (k, n) in &placed_count {
            if *n != 1 {
                errs.push(format!("物品 {k:?} 落位次数 {n} != 1"));
            }
            if !self.items.contains_key(*k) {
                errs.push(format!("网格里出现幽灵物品 {k:?}"));
            }
        }
        for (k, inst) in self.items.iter() {
            if !placed_count.contains_key(&k) {
                errs.push(format!("物品 {k:?} 未落位"));
            }
            match self.catalog.get(inst.def) {
                Ok(def) => {
                    if inst.stack == 0 || inst.stack > def.max_stack {
                        errs.push(format!("物品 {k:?} 堆叠数量非法: {}", inst.stack));
                    }
                    if def.container.is_some() != inst.container.is_some() {
                        errs.push(format!("物品 {k:?} 与定义的容器规格不一致"));
                    }
                }
                Err(_) => errs.push(format!("物品 {k:?} 引用了未知定义")),
            }
            if let Some(c) = inst.container {
                if !self.containers.contains_key(c) {
                    errs.push(format!("物品 {k:?} 引用的容器 {c:?} 不存在"));
                } else if self.container_owner.get(c).copied() != Some(k) {
                    errs.push(format!("容器 {c:?} 的归属记录与物品 {k:?} 不一致"));
                }
            }
        }

        // 容器归属：根 / 被拥有 / 无孤儿
        for (ck, ct) in self.containers.iter() {
            let owner = self.container_owner.get(ck).copied();
            if ct.ctype().is_root() {
                if owner.is_some() {
                    errs.push(format!("根容器 {ck:?} 不应有归属物品"));
                }
                if self.roots.get(ct.ctype()) != Some(ck) {
                    errs.push(format!("根容器 {ck:?} 未登记到 roots"));
                }
            } else if owner.is_none() {
                errs.push(format!("嵌套容器 {ck:?} 没有归属物品"));
            }
            if let Some(o) = owner {
                match self.items.get(o) {
                    Some(inst) if inst.container == Some(ck) => {}
                    _ => errs.push(format!("容器 {ck:?} 的归属物品 {o:?} 反向引用不一致")),
                }
            }
        }
        for t in ContainerType::ROOTS {
            if let Some(k) = self.roots.get(t) {
                match self.containers.get(k) {
                    Some(c) if c.ctype() == t => {}
                    _ => errs.push(format!("roots[{t:?}] 指向的容器不存在或类型不符")),
                }
            }
        }

        // 可达性与嵌套深度
        let reachable = nesting::reachable_containers(self);
        if reachable.len() != self.containers.len() {
            errs.push(format!(
                "存在不可达容器：可达 {} 个，实际 {} 个",
                reachable.len(),
                self.containers.len()
            ));
        }
        for c in &reachable {
            match self.depth_of(*c) {
                Ok(d) if d <= self.rules.max_nesting_depth => {}
                Ok(d) => errs.push(format!("容器 {c:?} 深度 {d} 超过上限")),
                Err(_) => errs.push(format!("容器 {c:?} 深度计算失败")),
            }
        }

        errs
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::item::ContainerSpec;

    /// 一棵三层深的套包链：背包(1) > 布袋(2) > 布袋(3)。
    fn depth_chain_inventory() -> Inventory {
        let mut inv = Inventory::new();
        let pouch = inv
            .define_item(ItemDef::new("布袋", 1, 1, 1).as_container(ContainerSpec::rectangular(2, 2)))
            .unwrap();
        let backpack = inv
            .add_root(ContainerType::Backpack, "背包", &[(6, 5)])
            .unwrap();
        let a = inv.add_item(pouch, 1, 0, backpack, 0, 0, 0, false).unwrap();
        let ca = inv.item(a).unwrap().container.unwrap();
        inv.add_item(pouch, 1, 0, ca, 0, 0, 0, false).unwrap();
        inv
    }

    #[test]
    fn restore_rejects_snapshot_deeper_than_rules() {
        let mut inv = depth_chain_inventory();
        let snap = inv.snapshot();
        assert!(inv.restore(&snap).is_ok());

        // 规则收紧到 2 层：快照本身合法，但恢复到当前规则下会被拒
        inv.set_rules(Rules::default().with_max_depth(2));
        assert_eq!(inv.restore(&snap), Err(InventoryError::CorruptSnapshot));

        // 被拒后现有数据不受影响
        inv.set_rules(Rules::default().with_max_depth(4));
        assert!(inv.restore(&snap).is_ok());
        assert!(inv.check_invariants().is_empty());
    }

    #[test]
    fn move_and_swap_keep_state_attached() {
        let mut inv = depth_chain_inventory();
        let backpack = inv.root(ContainerType::Backpack).unwrap();
        let items = inv.items_in(backpack);
        assert_eq!(items.len(), 1);
        let top = items[0].0;
        let inner = inv.item(top).unwrap().container.unwrap();
        // 顶层布袋挪到自己的内部会被成环挡住
        assert_eq!(
            inv.try_move(top, inner, 0, 0, 0, false),
            Err(InventoryError::WouldCycle)
        );
        assert!(inv.check_invariants().is_empty());
    }
}
