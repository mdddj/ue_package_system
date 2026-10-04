//! 容器实例：一块或多块网格。
//!
//! 复合容器（弹挂）在这里统一建模为 `parts: Vec<GridContainer>`：
//! 恰好一块 = 矩形容器（背包 / 安全箱），多块 = 弹挂的多个独立口袋。
//! 子网格各自维护自己的坐标空间，互不干扰。

use serde::{Deserialize, Serialize};

use super::error::{InvResult, InventoryError};
use super::grid::GridContainer;
use super::ids::ContainerKey;

/// 根容器分类。`Nested` 表示“套在别的容器里的容器”（背包放进背包后，里层包自身）。
#[derive(Copy, Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u8)]
pub enum ContainerType {
    /// 口袋（通常也是复合容器：几个 1x1 小格）。
    Pockets = 0,
    /// 弹挂（复合容器）。
    ChestRig = 1,
    /// 安全箱。
    SafeBox = 2,
    /// 背包。
    Backpack = 3,
    /// 非根容器（套包内部、弹挂内部）。
    Nested = 4,
}

/// 根容器种类数量（[`ContainerType::Pockets`]..=[`ContainerType::Backpack`]）。
pub const ROOT_TYPE_COUNT: usize = 4;

impl ContainerType {
    /// 全部根类型，固定顺序（快照 / FFI 遍历都靠它保证确定性）。
    pub const ROOTS: [ContainerType; ROOT_TYPE_COUNT] = [
        ContainerType::Pockets,
        ContainerType::ChestRig,
        ContainerType::SafeBox,
        ContainerType::Backpack,
    ];

    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(ContainerType::Pockets),
            1 => Some(ContainerType::ChestRig),
            2 => Some(ContainerType::SafeBox),
            3 => Some(ContainerType::Backpack),
            4 => Some(ContainerType::Nested),
            _ => None,
        }
    }

    /// 位掩码（用于 [`super::item::ItemDef::forbidden_container_types`]）。
    #[inline]
    pub fn bit(self) -> u8 {
        1 << (self as u8)
    }

    /// 是否为根容器。
    #[inline]
    pub fn is_root(self) -> bool {
        !matches!(self, ContainerType::Nested)
    }

    /// 根类型在 [`Self::ROOTS`] 里的下标。
    pub fn root_index(self) -> Option<usize> {
        Self::ROOTS.iter().position(|t| *t == self)
    }
}

/// 容器形状。
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ContainerKind {
    /// 单块矩形网格。
    Rectangular,
    /// 多块独立口袋。
    Compound,
}

/// 校验一组子网格尺寸是否合法（< 1 或超过上限即 [`InventoryError::InvalidDef`]）。
pub fn validate_dims(dims: &[(u8, u8)]) -> InvResult<()> {
    if dims.is_empty() || dims.len() > super::catalog::MAX_PARTS {
        return Err(InventoryError::InvalidDef);
    }
    for &(w, h) in dims {
        if w == 0
            || h == 0
            || w > super::catalog::MAX_DEF_DIM
            || h > super::catalog::MAX_DEF_DIM
            || w as usize * h as usize > super::catalog::MAX_PART_CELLS
        {
            return Err(InventoryError::InvalidDef);
        }
    }
    Ok(())
}

/// 容器实例：玩家根容器，或套包 / 弹挂的内部空间。
#[derive(Clone, Debug)]
pub struct Container {
    id: ContainerKey,
    label: String,
    ctype: ContainerType,
    parts: Vec<GridContainer>,
}

impl Container {
    /// 构造。`dims` 必须已通过 [`validate_dims`]（否则 panic —— 由调用方先校验）。
    pub fn new(
        id: ContainerKey,
        label: &str,
        ctype: ContainerType,
        dims: &[(u8, u8)],
    ) -> InvResult<Self> {
        validate_dims(dims)?;
        let parts = dims
            .iter()
            .map(|&(w, h)| GridContainer::new(w, h))
            .collect::<InvResult<Vec<_>>>()?;
        Ok(Self {
            id,
            label: label.to_string(),
            ctype,
            parts,
        })
    }

    #[inline]
    pub fn id(&self) -> ContainerKey {
        self.id
    }

    #[inline]
    pub fn label(&self) -> &str {
        &self.label
    }

    #[inline]
    pub fn ctype(&self) -> ContainerType {
        self.ctype
    }

    /// 形状：一块 = 矩形；多块 = 复合（弹挂）。
    #[inline]
    pub fn kind(&self) -> ContainerKind {
        if self.parts.len() == 1 {
            ContainerKind::Rectangular
        } else {
            ContainerKind::Compound
        }
    }

    #[inline]
    pub fn part_count(&self) -> usize {
        self.parts.len()
    }

    #[inline]
    pub fn parts(&self) -> &[GridContainer] {
        &self.parts
    }

    /// 第 `index` 块子网格。
    #[inline]
    pub fn part(&self, index: usize) -> Option<&GridContainer> {
        self.parts.get(index)
    }

    /// 全部子网格尺寸。
    pub fn dims(&self) -> Vec<(u8, u8)> {
        self.parts.iter().map(|p| p.size()).collect()
    }

    /// 总格数与已占格数（按格计，不是物品件数）。
    pub fn cell_stats(&self) -> (usize, usize) {
        let total = self.parts.iter().map(|p| p.cell_count()).sum();
        let used = self.parts.iter().map(|p| p.occupied_cells()).sum();
        (total, used)
    }

    /// 子网格可变访问（库内使用）。
    pub(crate) fn part_mut(&mut self, index: usize) -> Option<&mut GridContainer> {
        self.parts.get_mut(index)
    }

    /// 整体替换子网格（自动整理的原子提交用）。
    pub(crate) fn replace_parts(&mut self, parts: Vec<GridContainer>) {
        self.parts = parts;
    }

    /// 自检。
    pub fn check(&self) -> Vec<String> {
        let mut errs = Vec::new();
        if self.parts.is_empty() {
            errs.push("容器没有任何子网格".to_string());
        }
        for (i, p) in self.parts.iter().enumerate() {
            for e in p.check() {
                errs.push(format!("part {i}: {e}"));
            }
        }
        errs
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::grid::PlacedMeta;
    use crate::core::ids::{ContainerKey, ItemKey};

    fn ckey(i: u32) -> ContainerKey {
        ContainerKey::from_u64(i as u64 + 1)
    }

    #[test]
    fn kinds_and_dims() {
        let rect = Container::new(ckey(0), "背包", ContainerType::Backpack, &[(6, 5)]).unwrap();
        assert_eq!(rect.kind(), ContainerKind::Rectangular);
        assert_eq!(rect.dims(), vec![(6, 5)]);
        assert_eq!(rect.cell_stats(), (30, 0));

        let rig = Container::new(
            ckey(1),
            "弹挂",
            ContainerType::ChestRig,
            &[(1, 1), (1, 1), (1, 2), (2, 2)],
        )
        .unwrap();
        assert_eq!(rig.kind(), ContainerKind::Compound);
        assert_eq!(rig.part_count(), 4);
        assert_eq!(rig.cell_stats(), (1 + 1 + 2 + 4, 0));
        assert!(rig.check().is_empty());
    }

    #[test]
    fn validate_dims_rejects_bad() {
        assert!(validate_dims(&[]).is_err());
        assert!(validate_dims(&[(0, 1)]).is_err());
        assert!(validate_dims(&[(33, 1)]).is_err());
        assert!(validate_dims(&[(33, 33)]).is_err());
        assert!(validate_dims(&vec![(1, 1); 17]).is_err());
        assert!(validate_dims(&[(5, 5), (1, 1)]).is_ok());
    }

    #[test]
    fn parts_are_independent_coordinate_spaces() {
        let mut rig = Container::new(
            ckey(2),
            "弹挂",
            ContainerType::ChestRig,
            &[(1, 1), (1, 2)],
        )
        .unwrap();
        let k = ItemKey::from_u64(1);
        let meta = PlacedMeta {
            x: 0,
            y: 0,
            rotated: false,
            base_w: 1,
            base_h: 2,
            rotatable: false,
        };
        // part 0 是 1x1，放不下 1x2
        assert!(rig.part_mut(0).unwrap().place(k, meta).is_err());
        // part 1 是 1x2，放得下；两个 part 的坐标互不影响
        rig.part_mut(1).unwrap().place(k, meta).unwrap();
        assert!(rig.part(0).unwrap().cells().iter().all(|c| c.is_none()));
        assert!(rig.check().is_empty());
    }

    #[test]
    fn type_codes_and_bits() {
        assert_eq!(ContainerType::Pockets.bit(), 1);
        assert_eq!(ContainerType::ChestRig.bit(), 2);
        assert_eq!(ContainerType::SafeBox.bit(), 4);
        assert_eq!(ContainerType::Backpack.bit(), 8);
        assert_eq!(ContainerType::Nested.bit(), 16);
        assert!(ContainerType::Backpack.is_root());
        assert!(!ContainerType::Nested.is_root());
        assert_eq!(ContainerType::from_u8(3), Some(ContainerType::Backpack));
        assert_eq!(ContainerType::from_u8(9), None);
        assert_eq!(ContainerType::SafeBox.root_index(), Some(2));
        assert_eq!(ContainerType::Nested.root_index(), None);
    }
}
