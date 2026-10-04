//! 战术网格背包系统（Rust 核心）—— UE5 插件的 dylib 侧。
//!
//! 类似《三角洲行动》/《逃离塔科夫》的网格化背包：
//! 2D 碰撞检测、旋转、多容器（口袋 / 弹挂 / 安全箱 / 背包）、容器嵌套（套包）、
//! 原子性操作（失败零副作用）、一键整理、紧凑序列化。
//!
//! ## 分层
//! - [`core`]：数据模型（物品定义与实例、网格容器、中心化 [`core::Inventory`]）、
//!   原子操作、规则、快照；
//! - [`algo`]：装箱（一键整理）与嵌套（成环 / 深度）分析；
//! - [`ffi`]：C ABI，`cbindgen` 据此生成 `include/ue_package_system_ffi.h` 给 UE C++ 侧。
//!
//! ## 设计红线（实施计划第二节）
//! - 禁止裸指针 / `Rc<RefCell<T>>` / 跨结构体引用构建循环图；
//! - 一切引用走 [`core::ItemKey`] / [`core::ContainerKey`] 间接寻址，
//!   数据由 [`core::Inventory`] 集中持有。

pub mod algo;
pub mod core;
pub mod ffi;

pub use crate::core::*;

#[cfg(test)]
mod tests {
    /// 端到端冒烟：建两件容器 + 套包 + 移动 + 整理 + 快照回放。
    #[test]
    fn end_to_end_smoke() {
        use crate::core::*;

        let mut inv = Inventory::new();
        let ammo = inv
            .define_item(ItemDef::new("弹药", 1, 1, 60).weight(0.01).value(2))
            .unwrap();
        let bag = inv
            .define_item(
                ItemDef::new("背包", 3, 3, 1)
                    .rotatable(true)
                    .as_container(ContainerSpec::rectangular(6, 5)),
            )
            .unwrap();
        let pockets = inv
            .add_root(ContainerType::Pockets, "口袋", &[(1, 1), (1, 1)])
            .unwrap();
        let backpack = inv
            .add_root(ContainerType::Backpack, "背包", &[(6, 5)])
            .unwrap();

        let a = inv.add_item(ammo, 60, 0, pockets, 0, 0, 0, false).unwrap();
        inv.try_move(a, backpack, 0, 0, 0, false).unwrap();
        let b = inv.add_item(bag, 1, 0, backpack, 0, 3, 0, false).unwrap();
        let inner = inv.item(b).unwrap().container.unwrap();
        inv.add_item(ammo, 30, 0, inner, 0, 0, 0, false).unwrap();

        assert_eq!(inv.aggregates().item_count, 3);
        // 重量是 f32 存、f64 累加，容差放宽到 1e-6
        assert!((inv.aggregates().weight - 0.9).abs() < 1e-6);
        assert!(inv.autosort(backpack).is_ok());
        assert!(inv.check_invariants().is_empty());

        let snap = inv.snapshot();
        let bytes = snap.encode().unwrap();
        let decoded = Snapshot::decode(&bytes).unwrap();
        assert_eq!(decoded, snap);
        assert!(inv.restore(&decoded).is_ok());
        assert_eq!(inv.snapshot(), snap);
        assert!(inv.check_invariants().is_empty());
    }
}
