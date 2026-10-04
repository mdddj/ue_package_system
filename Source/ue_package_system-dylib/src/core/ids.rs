//! 物品与容器的实例 ID。
//!
//! 全部走 slotmap 的代际索引（generational index）：
//! - 槽位释放后复用，但 generation 递增 → 旧 key 永远不会误指向新对象；
//! - 没有裸指针、没有 `Rc<RefCell<T>>`、没有跨结构体借用，模板化序列化友好；
//! - 跨 FFI 边界统一编码成 `u64`（`0` 保留给“无”）。

use serde::{Deserialize, Serialize};
// `data()` 来自 slotmap 的 `Key` trait，必须显式引入作用域。
use slotmap::Key as _;

slotmap::new_key_type! {
    /// 物品实例 ID。
    pub struct ItemKey;
    /// 容器实例 ID（玩家根容器 / 套包内部 / 弹挂子网格组）。
    pub struct ContainerKey;
}

/// 物品静态定义 ID（[`super::catalog::Catalog`] 下标，从 0 开始）。
#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct DefId(pub u16);

macro_rules! impl_key_u64 {
    ($t:ty) => {
        impl $t {
            /// 编码成 FFI 用的 u64。活跃 key 永不为 0（generation ≥ 1），0 可当哨兵。
            #[inline]
            pub fn to_u64(self) -> u64 {
                self.data().as_ffi()
            }

            /// 从 FFI u64 还原。调用方必须保证值来自 [`Self::to_u64`]。
            #[inline]
            pub fn from_u64(v: u64) -> Self {
                Self::from(slotmap::KeyData::from_ffi(v))
            }
        }
    };
}

impl_key_u64!(ItemKey);
impl_key_u64!(ContainerKey);

#[cfg(test)]
mod tests {
    use super::*;
    use slotmap::Key;

    #[test]
    fn u64_roundtrip() {
        let mut map = slotmap::SlotMap::<ItemKey, u32>::with_key();
        let k = map.insert(1);
        assert_eq!(ItemKey::from_u64(k.to_u64()), k);
        // 活跃 key 的编码 ≥ 2^32，永不为 0 → 0 可以安全地当“无”的哨兵
        assert!(k.to_u64() >= 1 << 32);
        assert_ne!(ItemKey::null(), k);
        assert_ne!(ContainerKey::null(), ContainerKey::from_u64(1));
    }
}
