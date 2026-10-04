//! 背包系统核心：静态数据、网格几何、容器与中心化管理器。
//!
//! 设计红线（见实施计划）：
//! - 禁止裸指针 / `Rc<RefCell<T>>` / 跨结构体借用；
//! - 一切引用走 [`ItemKey`] / [`ContainerKey`] 间接寻址，数据由 [`Inventory`] 集中持有。

pub mod catalog;
pub mod container;
pub mod error;
pub mod grid;
pub mod ids;
pub mod inventory;
pub mod item;
pub mod rules;
pub mod snapshot;

pub use catalog::Catalog;
pub use container::{Container, ContainerKind, ContainerType};
pub use error::{InvResult, InventoryError};
pub use grid::{GridContainer, PlacedMeta};
pub use ids::{ContainerKey, DefId, ItemKey};
pub use inventory::{Aggregates, Inventory, Roots};
pub use item::{tags, ContainerSpec, ItemDef, ItemInstance};
pub use rules::Rules;
pub use snapshot::{ContainerSnap, ItemSnap, PlaceSnap, Snapshot};
