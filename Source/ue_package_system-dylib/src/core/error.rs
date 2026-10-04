//! 统一错误类型 + 稳定错误码。
//!
//! 错误码会跨 FFI 暴露给 C++ / 蓝图，是公开接口的一部分：**只增不改**。
//! 详细说明见 `docs/API.md`。

use thiserror::Error;

/// 背包系统错误。每个变体的判别值就是跨 FFI 的错误码。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
#[repr(i32)]
pub enum InventoryError {
    /// 目标位置越界（x/w 或 y/h 超出网格）。
    #[error("目标位置越界")]
    OutOfBounds = 1,
    /// 目标位置与已有物品重叠。
    #[error("目标位置已被占用")]
    Occupied = 2,
    /// 原地旋转后与其他物品冲突或越界。
    #[error("旋转后放不下")]
    RotationBlocked = 3,
    /// 物品定义标记为不可旋转。
    #[error("该物品不可旋转")]
    NotRotatable = 4,
    /// 物品当前没有落在任何网格里。
    #[error("物品未放置在容器中")]
    NotPlaced = 5,
    /// 物品实例不存在（ID 失效或从未创建）。
    #[error("物品不存在")]
    ItemNotFound = 6,
    /// 容器实例不存在。
    #[error("容器不存在")]
    ContainerNotFound = 7,
    /// 复合容器的 part 下标越界。
    #[error("容器子网格下标越界")]
    PartOutOfRange = 8,
    /// 物品定义 ID 不在目录中。
    #[error("物品定义不存在")]
    UnknownDef = 9,
    /// 物品定义参数不合法（尺寸/堆叠/容器规格等）。
    #[error("物品定义不合法")]
    InvalidDef = 10,
    /// 同一物品重复落位。
    #[error("物品已在网格中")]
    AlreadyPlaced = 11,
    /// 同名槽位已被占用（例如根容器已存在）。
    #[error("目标槽位已存在")]
    AlreadyExists = 12,
    /// 嵌套会成环（例如把外层的包放进自己的内部）。
    #[error("嵌套会形成环")]
    WouldCycle = 13,
    /// 嵌套深度超过规则上限。
    #[error("嵌套深度超限")]
    NestingTooDeep = 14,
    /// 物品定义禁止放入该类根容器（黑名单）。
    #[error("该物品禁止放入这种容器")]
    ForbiddenContainer = 15,
    /// 两个物品不可合并（定义不同或不可堆叠）。
    #[error("不可堆叠合并")]
    StackUnmergeable = 16,
    /// 目标堆叠已满。
    #[error("堆叠已满")]
    StackFull = 17,
    /// 堆叠数量不合法（0、超过上限或拆分数量越界）。
    #[error("堆叠数量不合法")]
    InvalidStackCount = 18,
    /// 自动整理无法在不丢物品的前提下重排（原布局保持不动）。
    #[error("自动整理失败，布局未改动")]
    SortFailed = 19,
    /// 快照数据损坏或与当前目录不匹配。
    #[error("快照数据损坏")]
    CorruptSnapshot = 20,
    /// 内部状态自检失败（理论上不应出现，出现即 bug）。
    #[error("内部状态不一致")]
    CorruptState = 21,
    /// 调用参数组合非法（例如 swap 同一个物品）。
    #[error("参数不合法")]
    InvalidArgument = 22,

    // ===== 以下为 FFI 层专用（库内逻辑不会产生）=====
    /// FFI 句柄为空或无效。
    #[error("句柄无效")]
    InvalidHandle = 23,
    /// 传入的 C 字符串不是合法 UTF-8。
    #[error("字符串不是合法 UTF-8")]
    InvalidUtf8 = 24,
    /// 输出缓冲区太小；需要的数量通过返回值的语义告知（见 API 文档）。
    #[error("输出缓冲区太小")]
    BufferTooSmall = 25,
    /// Rust 侧 panic 被 FFI 边界捕获。
    #[error("Rust 侧发生 panic（已捕获）")]
    Panic = 26,
}

impl InventoryError {
    /// 跨 FFI 的稳定错误码（正值；0 表示成功）。
    #[inline]
    pub fn code(self) -> i32 {
        self as i32
    }
}

/// 库内统一 Result 别名。
pub type InvResult<T> = Result<T, InventoryError>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_are_stable_and_distinct() {
        let all = [
            InventoryError::OutOfBounds,
            InventoryError::Occupied,
            InventoryError::RotationBlocked,
            InventoryError::NotRotatable,
            InventoryError::NotPlaced,
            InventoryError::ItemNotFound,
            InventoryError::ContainerNotFound,
            InventoryError::PartOutOfRange,
            InventoryError::UnknownDef,
            InventoryError::InvalidDef,
            InventoryError::AlreadyPlaced,
            InventoryError::AlreadyExists,
            InventoryError::WouldCycle,
            InventoryError::NestingTooDeep,
            InventoryError::ForbiddenContainer,
            InventoryError::StackUnmergeable,
            InventoryError::StackFull,
            InventoryError::InvalidStackCount,
            InventoryError::SortFailed,
            InventoryError::CorruptSnapshot,
            InventoryError::CorruptState,
            InventoryError::InvalidArgument,
            InventoryError::InvalidHandle,
            InventoryError::InvalidUtf8,
            InventoryError::BufferTooSmall,
            InventoryError::Panic,
        ];
        for (i, e) in all.iter().enumerate() {
            assert_eq!(e.code(), i as i32 + 1, "{e} 的码必须是 {i} + 1");
        }
    }
}
