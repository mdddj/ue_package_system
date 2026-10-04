//! 背包规则（随玩家走的可调参数）。

/// 嵌套与负重规则。
#[derive(Copy, Clone, Debug, PartialEq)]
pub struct Rules {
    /// 容器嵌套深度上限（根容器 = 1 层）。
    ///
    /// 例：`Backpack(1) > 突击背包(2) > 小挎包(3) > 再一层(4)`，上限 4 时第 5 层会被拒。
    pub max_nesting_depth: u8,
    /// 玩家总承重上限（kg）。`f32::INFINITY` = 无限制（默认，不改变既有行为）。
    pub max_capacity_kg: f32,
}

/// 合理的深度上限：超过这个值几乎肯定是配置错误（多层套包会拖慢查询、也违反玩法设计）。
pub const MAX_ALLOWED_NESTING_DEPTH: u8 = 16;

impl Default for Rules {
    fn default() -> Self {
        Self {
            max_nesting_depth: 4,
            max_capacity_kg: f32::INFINITY,
        }
    }
}

impl Rules {
    /// 校验并设置深度上限。
    pub fn with_max_depth(mut self, depth: u8) -> Self {
        self.max_nesting_depth = depth.clamp(1, MAX_ALLOWED_NESTING_DEPTH);
        self
    }

    /// 设置总承重上限（kg）。
    ///
    /// 非有限值、NaN 或 `<= 0` 一律视为**无限制** —— 传 0 等于“关闭负重限制”，
    /// 而不是“一点都不能带”。
    pub fn with_max_capacity(mut self, kg: f32) -> Self {
        self.max_capacity_kg = if kg.is_finite() && kg > 0.0 {
            kg
        } else {
            f32::INFINITY
        };
        self
    }

    /// 是否设置了承重上限。
    pub fn has_capacity_limit(&self) -> bool {
        self.max_capacity_kg.is_finite()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_and_clamp() {
        assert_eq!(Rules::default().max_nesting_depth, 4);
        assert_eq!(Rules::default().with_max_depth(0).max_nesting_depth, 1);
        assert_eq!(Rules::default().with_max_depth(200).max_nesting_depth, 16);
        assert_eq!(Rules::default().with_max_depth(6).max_nesting_depth, 6);
    }

    #[test]
    fn capacity_default_is_unlimited_and_bad_values_stay_unlimited() {
        let d = Rules::default();
        assert!(!d.has_capacity_limit());
        for bad in [0.0, -1.0, f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            let r = Rules::default().with_max_capacity(bad);
            assert!(!r.has_capacity_limit(), "{bad} 应视为无限制");
        }
        let r = Rules::default().with_max_capacity(12.5);
        assert!(r.has_capacity_limit());
        assert_eq!(r.max_capacity_kg, 12.5);
    }
}
