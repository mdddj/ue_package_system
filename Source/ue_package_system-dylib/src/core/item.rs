//! 物品：静态定义（[`ItemDef`]，由 `Catalog` 持有）+ 运行时实例（[`ItemInstance`]，由 `Inventory` 持有）。

use serde::{Deserialize, Serialize};

use super::ids::{ContainerKey, DefId};

/// 常用标签位。其余位留给游戏自定义。
///
/// “可直接换弹的弹匣”这类查询用标签位掩码完成，不做字符串匹配。
pub mod tags {
    pub const MAGAZINE: u32 = 1 << 0;
    pub const AMMO: u32 = 1 << 1;
    pub const MEDKIT: u32 = 1 << 2;
    pub const GRENADE: u32 = 1 << 3;
    pub const ARMOR: u32 = 1 << 4;
    pub const HELMET: u32 = 1 << 5;
    pub const VALUABLE: u32 = 1 << 6;
    pub const KEY: u32 = 1 << 7;
    /// 容器类物品（背包 / 弹挂 / 安全箱）。定义容器规格时自动打上。
    pub const CONTAINER: u32 = 1 << 8;
}

/// 容器类物品的内部结构（“套包”里的包）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ContainerSpec {
    /// 每块子网格的 (宽, 高)。
    ///
    /// 恰好一块 = 矩形容器；多块 = 复合容器（弹挂的多个口袋）。
    pub parts: Vec<(u8, u8)>,
}

impl ContainerSpec {
    /// 单块矩形网格（背包 / 安全箱）。
    pub fn rectangular(w: u8, h: u8) -> Self {
        Self { parts: vec![(w, h)] }
    }

    /// 多块独立口袋（弹挂）。
    pub fn compound(parts: Vec<(u8, u8)>) -> Self {
        Self { parts }
    }
}

/// 物品静态定义。一份定义可被任意多个实例引用。
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ItemDef {
    pub name: String,
    /// 未旋转时的占格尺寸（宽，竖 = 沿 y 轴的格数）。
    pub w: u8,
    /// 未旋转时的占格尺寸（高）。
    pub h: u8,
    pub rotatable: bool,
    /// > 1 表示可堆叠（子弹、药品）。可堆叠物品不能同时是容器。
    pub max_stack: u32,
    /// 单件重量；堆叠按数量累乘。
    pub weight: f32,
    /// 单件估值；堆叠按数量累乘。
    pub value: u32,
    /// 标签位掩码，见 [`tags`]。
    pub tags: u32,
    /// `ContainerType` 位掩码：禁止放入的**根容器**类型（大金不进安全箱一类规则）。
    pub forbidden_container_types: u8,
    /// `Some` 表示这件物品是容器（套包 / 弹挂），可继续往里放东西。
    pub container: Option<ContainerSpec>,
}

impl ItemDef {
    /// 最简构造：1x1 不可旋转不可堆叠、无重量无估值。
    pub fn new(name: &str, w: u8, h: u8, max_stack: u32) -> Self {
        Self {
            name: name.to_string(),
            w,
            h,
            rotatable: false,
            max_stack,
            weight: 0.0,
            value: 0,
            tags: 0,
            forbidden_container_types: 0,
            container: None,
        }
    }

    pub fn rotatable(mut self, v: bool) -> Self {
        self.rotatable = v;
        self
    }

    pub fn weight(mut self, v: f32) -> Self {
        self.weight = v;
        self
    }

    pub fn value(mut self, v: u32) -> Self {
        self.value = v;
        self
    }

    pub fn tags(mut self, v: u32) -> Self {
        self.tags = v;
        self
    }

    /// 禁止放入某些根容器（传 `ContainerType::bit()` 的组合）。
    pub fn forbid(mut self, type_mask: u8) -> Self {
        self.forbidden_container_types = type_mask;
        self
    }

    /// 让这件物品成为容器（套包 / 弹挂）。
    pub fn as_container(mut self, spec: ContainerSpec) -> Self {
        self.container = Some(spec);
        self
    }

    /// 有效占格尺寸（考虑旋转）。
    #[inline]
    pub fn footprint(&self, rotated: bool) -> (u8, u8) {
        if rotated {
            (self.h, self.w)
        } else {
            (self.w, self.h)
        }
    }

    #[inline]
    pub fn total_cells(&self) -> u32 {
        self.w as u32 * self.h as u32
    }
}

/// 运行时物品实例。
///
/// 位置信息不存在这里：物品落在哪个网格、占哪些格，统一由 [`super::grid::GridContainer`] 记录，
/// 避免“两处状态互相打架”。
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ItemInstance {
    pub def: DefId,
    /// 堆叠数量；不可堆叠物品恒为 1。
    pub stack: u32,
    /// 耐久（语义由游戏定义，常用 0..=10000 映射 0.00%..100.00%）。
    pub durability: u16,
    /// `Some` 表示这件物品自带一个容器（套包），可继续往里放东西。
    pub container: Option<ContainerKey>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn footprint_rotates() {
        let def = ItemDef::new("枪", 2, 4, 1).rotatable(true);
        assert_eq!(def.footprint(false), (2, 4));
        assert_eq!(def.footprint(true), (4, 2));
        assert_eq!(def.total_cells(), 8);
    }

    #[test]
    fn builders_compose() {
        let def = ItemDef::new("背包", 3, 3, 1)
            .weight(1.5)
            .value(900)
            .tags(tags::CONTAINER)
            .forbid(1 << 2)
            .as_container(ContainerSpec::rectangular(6, 5));
        assert_eq!(def.container.as_ref().unwrap().parts, vec![(6, 5)]);
        assert_eq!(def.forbidden_container_types, 4);
    }
}
