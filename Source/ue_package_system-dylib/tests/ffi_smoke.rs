//! FFI 冒烟测试：在进程内直接调用 `ffi::uerust_*` C ABI 入口。
//!
//! 覆盖生命周期、返回码语义、两段式数组输出、NULL 指针路径、快照全链路与自检。

use std::ffi::{CStr, CString};
use std::ptr;

use ue_package_system_dylib::ffi::*;
use ue_package_system_dylib::{tags, ContainerType, InventoryError};

/// 建一个最小世界：2 个物品定义（普通 / 套包）、2 个根容器、2 件物品。
unsafe fn setup() -> (*mut ue_package_system_dylib::Inventory, u32, u64, u64) {
    let inv = uerust_inventory_new();
    assert!(!inv.is_null());

    let ammo_name = CString::new("弹药").unwrap();
    let ammo = uerust_inventory_define_item(
        inv,
        ammo_name.as_ptr(),
        1,
        1,
        0,
        60,
        0.012,
        3,
        tags::AMMO,
        0,
        ptr::null(),
        0,
    );
    assert!(ammo >= 0, "普通物品定义失败: {ammo}");

    let bag_name = CString::new("套包").unwrap();
    let dims: [u8; 2] = [4, 4];
    let bag = uerust_inventory_define_item(
        inv,
        bag_name.as_ptr(),
        2,
        2,
        1,
        1,
        1.0,
        200,
        0,
        0,
        dims.as_ptr(),
        1,
    );
    assert!(bag >= 0, "套包定义失败: {bag}");

    let pocket_label = CString::new("口袋").unwrap();
    let pdims: [u8; 2] = [2, 2];
    let pocket = uerust_inventory_add_root(
        inv,
        ContainerType::Pockets as u32,
        pocket_label.as_ptr(),
        pdims.as_ptr(),
        1,
    );
    assert!(pocket > 0, "建口袋失败: {pocket}");

    let bp_label = CString::new("背包").unwrap();
    let bdims: [u8; 2] = [6, 5];
    let backpack = uerust_inventory_add_root(
        inv,
        ContainerType::Backpack as u32,
        bp_label.as_ptr(),
        bdims.as_ptr(),
        1,
    );
    assert!(backpack > 0, "建背包失败: {backpack}");

    let a = uerust_inventory_add_item(inv, ammo as u32, 60, 0, pocket as u64, 0, 0, 0, 0);
    assert!(a > 0, "放弹药失败: {a}");
    let b = uerust_inventory_add_item(inv, bag as u32, 1, 0, backpack as u64, 0, 0, 0, 0);
    assert!(b > 0, "放套包失败: {b}");

    let _ = bag;
    (inv, ammo as u32, pocket as u64, backpack as u64)
}

#[test]
fn lifecycle_and_null_handle() {
    unsafe {
        let inv = uerust_inventory_new();
        assert!(!inv.is_null());
        assert!(!uerust_plugin_version().is_null());

        // NULL 句柄 → InvalidHandle(23)。
        assert_eq!(
            uerust_inventory_try_rotate(ptr::null_mut(), 1),
            InventoryError::InvalidHandle.code()
        );
        assert_eq!(
            uerust_inventory_autosort(ptr::null_mut(), 1),
            InventoryError::InvalidHandle.code()
        );

        uerust_inventory_free(inv);
        uerust_inventory_free(ptr::null_mut()); // 释放 NULL 安全
    }
}

#[test]
fn define_item_rejects_bad_part_count() {
    unsafe {
        let inv = uerust_inventory_new();
        let name = CString::new("坏套包").unwrap();
        let dims: [u8; 2] = [2, 2];
        // container_part_count = 17 > MAX_PARTS(16) → InvalidArgument(22)，返回 -22。
        let bad = uerust_inventory_define_item(
            inv,
            name.as_ptr(),
            2,
            2,
            1,
            1,
            1.0,
            0,
            0,
            0,
            dims.as_ptr(),
            17,
        );
        assert_eq!(bad, -(InventoryError::InvalidArgument.code() as i64));
        // 失败后 last_error 非空。
        let err = CStr::from_ptr(uerust_last_error());
        assert!(!err.to_str().unwrap().is_empty(), "last_error 不应为空");
        uerust_inventory_free(inv);
    }
}

#[test]
fn return_code_semantics() {
    unsafe {
        let (inv, _ammo, _pocket, backpack) = setup();

        // try_* 返回 i32：0 = 成功。
        let bag_item = {
            // 背包里已经放了套包（句柄通过 items 查询拿到）。
            let n = uerust_inventory_items(inv, ptr::null_mut(), 0);
            assert!(n >= 2);
            let mut buf = vec![0u64; n as usize];
            assert_eq!(
                uerust_inventory_items(inv, buf.as_mut_ptr(), buf.len() as u32),
                n
            );
            // 找到套包（非弹药）的句柄：用 item_info 区分 def。
            let mut bag = 0u64;
            for k in &buf {
                let mut info = zero_item_info();
                assert_eq!(uerust_inventory_item_info(inv, *k, &mut info), 0);
                if info.own_container != 0 {
                    bag = *k;
                }
            }
            assert!(bag != 0, "应能找到套包");
            bag
        };

        // 套包不可旋转（定义 rotatable=1，其实可旋转）——测 try_rotate 成功。
        assert_eq!(uerust_inventory_try_rotate(inv, bag_item), 0);

        // try_move 越界 → 正数错误码。
        let code = uerust_inventory_try_move(inv, bag_item, backpack, 0, 99, 0, 0);
        assert!(code > 0, "越界移动应返回正错误码，实际 {code}");

        // try_split：不可堆叠的套包 → 负数 -错误码。
        let split = uerust_inventory_try_split(inv, bag_item, 1, backpack, 0, 0, 0, 0);
        assert!(split < 0, "套包不可拆分，应返回负数");
        assert_eq!(split, -(InventoryError::StackUnmergeable.code() as i64));

        // autosort 成功 → 0。
        assert_eq!(uerust_inventory_autosort(inv, backpack), 0);

        // 失败后 last_error 非空。
        let e = CStr::from_ptr(uerust_last_error());
        assert!(!e.to_str().unwrap().is_empty());

        uerust_inventory_free(inv);
    }
}

/// 全字段置零的 `FfiItemInfo`。
fn zero_item_info() -> FfiItemInfo {
    FfiItemInfo {
        def_id: 0,
        stack: 0,
        durability: 0,
        own_container: 0,
        is_placed: 0,
        host_container: 0,
        host_part: 0,
        x: 0,
        y: 0,
        rotated: 0,
    }
}

fn zero_container_info() -> FfiContainerInfo {
    FfiContainerInfo {
        ctype: 0,
        part_count: 0,
        is_root: 0,
        total_cells: 0,
        used_cells: 0,
    }
}

#[test]
fn queries_and_two_phase_arrays() {
    unsafe {
        let (inv, _ammo, pocket, backpack) = setup();

        // ---- item_info ----
        let n = uerust_inventory_items(inv, ptr::null_mut(), 0);
        assert_eq!(n, 2);
        let mut items = vec![0u64; n as usize];
        assert_eq!(uerust_inventory_items(inv, items.as_mut_ptr(), n as u32), n);
        let mut info = zero_item_info();
        assert_eq!(uerust_inventory_item_info(inv, items[0], &mut info), 0);
        assert_eq!(info.is_placed, 1);
        assert_eq!(info.host_container, pocket);
        assert_eq!(info.def_id, 0);

        // ---- container_info / part_size ----
        let mut ci = zero_container_info();
        assert_eq!(uerust_inventory_container_info(inv, backpack, &mut ci), 0);
        assert_eq!(ci.ctype, ContainerType::Backpack as u32);
        assert_eq!(ci.part_count, 1);
        assert_eq!(ci.is_root, 1);
        assert_eq!(ci.total_cells, 30);

        let mut ow = 0u32;
        let mut oh = 0u32;
        assert_eq!(uerust_inventory_container_part_size(inv, backpack, 0, &mut ow, &mut oh), 0);
        assert_eq!((ow, oh), (6, 5));
        assert_eq!(
            uerust_inventory_container_part_size(inv, backpack, 5, &mut ow, &mut oh),
            InventoryError::PartOutOfRange.code()
        );

        // ---- container_grid 两段式 ----
        let cells = uerust_inventory_container_grid(inv, backpack, 0, ptr::null_mut(), 0);
        assert_eq!(cells, 30, "cap=0 应只返回格数");
        let mut buf = vec![0u64; cells as usize];
        assert_eq!(uerust_inventory_container_grid(inv, backpack, 0, buf.as_mut_ptr(), cells as u32), cells);
        // cap 太小 → -BufferTooSmall(25)
        assert_eq!(
            uerust_inventory_container_grid(inv, backpack, 0, buf.as_mut_ptr(), 2),
            -(InventoryError::BufferTooSmall.code() as i64)
        );

        // ---- item_location ----
        let mut loc_c = 0u64;
        let mut loc_p = 0u32;
        assert_eq!(uerust_inventory_item_location(inv, items[0], &mut loc_c, &mut loc_p), 0);
        assert_eq!(loc_c, pocket);
        assert_eq!(loc_p, 0);

        // ---- total_weight / total_value ----
        let mut weight = 0f64;
        assert_eq!(uerust_inventory_total_weight(inv, &mut weight), 0);
        assert!(weight > 0.0);
        let mut value = 0u64;
        assert_eq!(uerust_inventory_total_value(inv, &mut value), 0);
        assert_eq!(value, 60 * 3 + 200);

        // ---- find_by_tag 两段式 ----
        let hits = uerust_inventory_find_by_tag(inv, tags::AMMO, ptr::null_mut(), 0);
        assert_eq!(hits, 1);
        let mut arr = vec![0u64; hits as usize];
        assert_eq!(uerust_inventory_find_by_tag(inv, tags::AMMO, arr.as_mut_ptr(), hits as u32), hits);

        // ---- containers / items cap 太小 ----
        let cn = uerust_inventory_containers(inv, ptr::null_mut(), 0);
        assert_eq!(cn, 3, "口袋 + 背包 + 套包内部容器");
        let mut cbuf = vec![0u64; cn as usize];
        assert_eq!(uerust_inventory_containers(inv, cbuf.as_mut_ptr(), cn as u32), cn);
        assert_eq!(
            uerust_inventory_items(inv, items.as_mut_ptr(), 1),
            -(InventoryError::BufferTooSmall.code() as i64)
        );

        // ---- label + free_string ----
        let label = uerust_inventory_container_label(inv, backpack);
        assert!(!label.is_null());
        assert_eq!(CStr::from_ptr(label).to_str().unwrap(), "背包");
        uerust_free_string(label);
        uerust_free_string(ptr::null_mut()); // 释放 NULL 安全

        uerust_inventory_free(inv);
    }
}

#[test]
fn snapshot_restore_and_free_bytes() {
    unsafe {
        let (inv, _ammo, _pocket, _backpack) = setup();

        let mut ptr: *mut u8 = ptr::null_mut();
        let mut len: u32 = 0;
        assert_eq!(uerust_inventory_snapshot(inv, &mut ptr, &mut len), 0);
        assert!(!ptr.is_null());
        assert!(len > 0);

        assert_eq!(uerust_inventory_restore(inv, ptr, len), 0);
        assert!(uerust_last_error() as usize != 0);

        // 损坏字节 → 恢复失败（负数或正错误码？restore 返回 i32 错误码）。
        let bad = [0xFFu8; 4];
        assert_eq!(
            uerust_inventory_restore(inv, bad.as_ptr(), bad.len() as u32),
            InventoryError::CorruptSnapshot.code()
        );

        uerust_free_bytes(ptr, len);
        uerust_free_bytes(ptr::null_mut(), 0); // NULL 安全

        uerust_inventory_free(inv);
    }
}

#[test]
fn selftest_report_is_green() {
    unsafe {
        let report = uerust_inventory_selftest();
        assert!(!report.is_null());
        let s = CStr::from_ptr(report).to_str().unwrap();
        assert!(s.contains("通过"), "报告应包含“通过”：\n{s}");
        assert!(!s.contains('✗'), "报告不应含失败标记：\n{s}");
        uerust_free_string(report);
    }
}
