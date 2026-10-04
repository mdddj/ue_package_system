//! 物品定义目录。
//!
//! 目录属于**单个** [`super::inventory::Inventory`]：定义按注册顺序编号，
//! 存在 `DefId` 里，不自建全局可变状态（多玩家 / 多测试互不干扰）。

use super::container::validate_dims;
use super::error::{InvResult, InventoryError};
use super::ids::DefId;
use super::item::{tags, ItemDef};

/// 单个维度上限（格）。
pub const MAX_DEF_DIM: u8 = 32;
/// 单块子网格的格数上限。
pub const MAX_PART_CELLS: usize = 1024;
/// 单个物品定义最多挂多少块子网格。
pub const MAX_PARTS: usize = 16;
/// 名称长度上限（字节）。
pub const MAX_NAME_BYTES: usize = 128;

/// 物品定义表。`DefId` 就是这里的下标。
#[derive(Clone, Debug, Default)]
pub struct Catalog {
    defs: Vec<ItemDef>,
}

impl Catalog {
    pub fn new() -> Self {
        Self { defs: Vec::new() }
    }

    pub fn len(&self) -> usize {
        self.defs.len()
    }

    pub fn is_empty(&self) -> bool {
        self.defs.is_empty()
    }

    /// 按 ID 取定义。
    pub fn get(&self, id: DefId) -> InvResult<&ItemDef> {
        self.defs.get(id.0 as usize).ok_or(InventoryError::UnknownDef)
    }

    /// 全部定义，按注册顺序。
    pub fn iter(&self) -> impl Iterator<Item = (DefId, &ItemDef)> {
        self.defs
            .iter()
            .enumerate()
            .map(|(i, d)| (DefId(i as u16), d))
    }

    /// 注册一份定义，返回其 ID。校验不过返回 [`InventoryError::InvalidDef`]。
    pub fn define(&mut self, mut def: ItemDef) -> InvResult<DefId> {
        if def.name.is_empty() || def.name.len() > MAX_NAME_BYTES {
            return Err(InventoryError::InvalidDef);
        }
        if def.w == 0 || def.h == 0 || def.w > MAX_DEF_DIM || def.h > MAX_DEF_DIM {
            return Err(InventoryError::InvalidDef);
        }
        if !def.weight.is_finite() || def.weight < 0.0 {
            return Err(InventoryError::InvalidDef);
        }
        if def.max_stack == 0 || def.max_stack > u16::MAX as u32 {
            return Err(InventoryError::InvalidDef);
        }
        if self.defs.len() >= u16::MAX as usize {
            return Err(InventoryError::InvalidDef);
        }

        if let Some(spec) = &def.container {
            // 可堆叠物品不能同时是容器：套包没有“数量”概念，二者混在一起状态会失控。
            if def.max_stack != 1 {
                return Err(InventoryError::InvalidDef);
            }
            validate_dims(&spec.parts)?;
            def.tags |= tags::CONTAINER;
        }

        self.defs.push(def);
        Ok(DefId((self.defs.len() - 1) as u16))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::item::ContainerSpec;

    #[test]
    fn define_and_get() {
        let mut cat = Catalog::new();
        let id = cat.define(ItemDef::new("子弹", 1, 1, 60)).unwrap();
        assert_eq!(id, DefId(0));
        assert_eq!(cat.get(id).unwrap().name, "子弹");
        assert_eq!(cat.get(DefId(7)), Err(InventoryError::UnknownDef));
        assert_eq!(cat.len(), 1);
    }

    #[test]
    fn define_rejects_bad_defs() {
        let mut cat = Catalog::new();
        assert_eq!(cat.define(ItemDef::new("", 1, 1, 1)), Err(InventoryError::InvalidDef));
        assert_eq!(cat.define(ItemDef::new("x", 0, 1, 1)), Err(InventoryError::InvalidDef));
        assert_eq!(
            cat.define(ItemDef::new("x", 33, 1, 1)),
            Err(InventoryError::InvalidDef)
        );
        assert_eq!(
            cat.define(ItemDef::new("x", 1, 1, 0)),
            Err(InventoryError::InvalidDef)
        );
        // 可堆叠 + 容器 = 非法
        assert_eq!(
            cat.define(
                ItemDef::new("袋", 1, 1, 5).as_container(ContainerSpec::rectangular(2, 2))
            ),
            Err(InventoryError::InvalidDef)
        );
        // 空 part 列表 = 非法
        assert_eq!(
            cat.define(ItemDef::new("袋", 1, 1, 1).as_container(ContainerSpec::compound(vec![]))),
            Err(InventoryError::InvalidDef)
        );
    }

    #[test]
    fn container_tag_is_auto_added() {
        let mut cat = Catalog::new();
        let id = cat
            .define(ItemDef::new("背包", 2, 2, 1).as_container(ContainerSpec::rectangular(4, 4)))
            .unwrap();
        assert_ne!(cat.get(id).unwrap().tags & tags::CONTAINER, 0);
    }
}
