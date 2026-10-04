//! 紧凑快照：bincode 编码的背包存档 / 网络包。
//!
//! 设计目标（实施计划阶段五）：单玩家整包压缩在 1~2 KiB 以内。
//! 做法：
//! - **键不入包**：容器按确定性顺序编号（u16 下标），物品用下标互相引用；
//! - 尺寸 / 可旋转性从目录推导，只存 `rotated` 标志；
//! - 数量压到 u16 / u32，没有 JSON 的字段名开销。
//!
//! 注意：快照**不含**物品目录（[`crate::core::Catalog`]）。
//! 目录属于内容数据，发送方和接收方必须使用同一份定义表；
//! [`crate::core::Inventory::restore`] 会用接收方的目录校验快照。

use serde::{Deserialize, Serialize};

use super::container::ROOT_TYPE_COUNT;
use super::error::{InvResult, InventoryError};

/// 一份背包快照。字段顺序即 bincode 的编码顺序。
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Snapshot {
    /// 四类根容器指向 [`Self::containers`] 的下标（`None` = 没有该类容器）。
    pub roots: [Option<u16>; ROOT_TYPE_COUNT],
    pub containers: Vec<ContainerSnap>,
    pub items: Vec<ItemSnap>,
}

/// 容器快照。
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ContainerSnap {
    pub label: String,
    /// [`super::container::ContainerType`] 的判别值。
    pub ctype: u8,
    /// 每块子网格的 (宽, 高)。
    pub parts: Vec<(u8, u8)>,
}

/// 物品快照。
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ItemSnap {
    pub def: u16,
    pub stack: u32,
    pub durability: u16,
    /// 自带容器（套包）在 [`Snapshot::containers`] 里的下标。
    pub container: Option<u16>,
    /// 落位信息（当前版本所有物品都落位）。
    pub place: PlaceSnap,
}

/// 落位快照：容器下标 + 子网格 + 坐标 + 朝向。
#[derive(Copy, Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlaceSnap {
    pub container: u16,
    pub part: u8,
    pub x: u8,
    pub y: u8,
    pub rotated: bool,
}

impl Snapshot {
    /// bincode 编码。
    pub fn encode(&self) -> InvResult<Vec<u8>> {
        bincode::serialize(self).map_err(|_| InventoryError::CorruptState)
    }

    /// 解码。数据损坏返回 [`InventoryError::CorruptSnapshot`]。
    pub fn decode(bytes: &[u8]) -> InvResult<Self> {
        bincode::deserialize(bytes).map_err(|_| InventoryError::CorruptSnapshot)
    }

    /// 编码后的字节数（不实际编码）。
    pub fn encoded_len(&self) -> InvResult<usize> {
        bincode::serialized_size(self)
            .map(|n| n as usize)
            .map_err(|_| InventoryError::CorruptState)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Snapshot {
        Snapshot {
            roots: [Some(0), None, Some(1), None],
            containers: vec![
                ContainerSnap {
                    label: "口袋".to_string(),
                    ctype: 0,
                    parts: vec![(1, 1), (1, 2)],
                },
                ContainerSnap {
                    label: "安全箱".to_string(),
                    ctype: 2,
                    parts: vec![(4, 3)],
                },
            ],
            items: vec![ItemSnap {
                def: 3,
                stack: 60,
                durability: 0,
                container: None,
                place: PlaceSnap {
                    container: 0,
                    part: 1,
                    x: 0,
                    y: 0,
                    rotated: false,
                },
            }],
        }
    }

    #[test]
    fn encode_decode_roundtrip() {
        let s = sample();
        let bytes = s.encode().unwrap();
        assert_eq!(Snapshot::decode(&bytes).unwrap(), s);
        assert_eq!(s.encoded_len().unwrap(), bytes.len());
    }

    #[test]
    fn decode_rejects_garbage() {
        assert_eq!(
            Snapshot::decode(&[1, 2, 3]),
            Err(InventoryError::CorruptSnapshot)
        );
        assert_eq!(Snapshot::decode(&[]), Err(InventoryError::CorruptSnapshot));
    }
}
