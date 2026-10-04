//! 嵌套分析：子容器枚举、成环检测、子树高度。
//!
//! 全部用显式栈的迭代 DFS（不用递归）：即使数据异常出现环，也不会爆栈或死循环。

use std::collections::BTreeSet;

use crate::core::ids::ContainerKey;
use crate::core::Inventory;

/// 容器 `c` 的直接子容器（放在 `c` 里的套包自带的空间），确定性顺序。
pub fn child_containers(inv: &Inventory, c: ContainerKey) -> Vec<ContainerKey> {
    let mut out = Vec::new();
    for (key, _, _) in inv.items_in(c) {
        if let Ok(inst) = inv.item(key) {
            if let Some(child) = inst.container {
                if !out.contains(&child) {
                    out.push(child);
                }
            }
        }
    }
    out
}

/// `needle` 是否在 `root` 的嵌套子树里（含 `root` 自身）。
pub fn subtree_contains(inv: &Inventory, root: ContainerKey, needle: ContainerKey) -> bool {
    if root == needle {
        return true;
    }
    let mut seen: BTreeSet<ContainerKey> = BTreeSet::new();
    seen.insert(root);
    let mut stack = vec![root];
    while let Some(c) = stack.pop() {
        for ch in child_containers(inv, c) {
            if ch == needle {
                return true;
            }
            if seen.insert(ch) {
                stack.push(ch);
            }
        }
    }
    false
}

/// `root` 的嵌套高度（只有自己 = 1）。
pub fn subtree_height(inv: &Inventory, root: ContainerKey) -> u8 {
    let mut seen: BTreeSet<ContainerKey> = BTreeSet::new();
    seen.insert(root);
    let mut stack = vec![(root, 1u8)];
    let mut max = 1u8;
    while let Some((c, d)) = stack.pop() {
        max = max.max(d);
        for ch in child_containers(inv, c) {
            if seen.insert(ch) {
                stack.push((ch, d.saturating_add(1)));
            }
        }
    }
    max
}

/// 从根容器出发可达的全部容器，确定性顺序（根按固定顺序，子树前序）。
pub fn reachable_containers(inv: &Inventory) -> Vec<ContainerKey> {
    let mut out = Vec::new();
    let mut seen: BTreeSet<ContainerKey> = BTreeSet::new();
    let roots: Vec<ContainerKey> = inv.roots().iter().map(|(_, k)| k).collect();
    let mut stack: Vec<ContainerKey> = roots.into_iter().rev().collect();
    while let Some(c) = stack.pop() {
        if !seen.insert(c) {
            continue;
        }
        out.push(c);
        for ch in child_containers(inv, c).into_iter().rev() {
            stack.push(ch);
        }
    }
    out
}
