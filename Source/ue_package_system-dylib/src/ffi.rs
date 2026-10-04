//! C ABI：UE5 C++ 侧通过 `dlopen` + `GetDllExport` 调用的全部入口。
//!
//! ## 返回值约定（详见 `docs/API.md`）
//! - `i32`：`0` 成功；`> 0` 为错误码（[`InventoryError::code`]）。
//! - `i64`：`>= 0` 为结果值（DefId / 句柄 / 计数）；`< 0` 为 `-错误码`。
//!   物品 / 容器句柄是 slotmap key 的 u64 编码，活跃句柄永不为 0。
//! - `*mut c_char` / `*mut u8`：Rust 堆分配，必须用 [`uerust_free_string`] /
//!   [`uerust_free_bytes`] 释放，不能在 C++ 侧 `free`。
//!
//! ## 其他约定
//! - 数组输出统一“两段式”：先传 `out = NULL`（或 `cap = 0`）拿所需数量，再按数量传缓冲区；
//!   缓冲区不足返回 `-BufferTooSmall`。
//! - 句柄只在**单线程**内使用；多线程共享请在外面加锁。
//! - 所有入口都把 panic 拦在边界内（`catch_unwind`），不会让异常穿过 FFI。

use std::any::Any;
use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::LazyLock;

use crate::core::catalog::MAX_PARTS;
use crate::core::container::ContainerType;
use crate::core::error::{InvResult, InventoryError};
use crate::core::ids::{ContainerKey, DefId, ItemKey};
use crate::core::item::{tags, ContainerSpec, ItemDef};
use crate::core::snapshot::Snapshot;
use crate::core::Inventory;

// ==================== 跨 FFI 的结构体 ====================

/// 物品实例的完整状态（UI / 网络层取数据用）。
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct FfiItemInfo {
    pub def_id: u32,
    pub stack: u32,
    pub durability: u32,
    /// 自带容器（套包）：0 = 无。
    pub own_container: u64,
    /// 是否已落位（当前版本恒为 1，预留字段）。
    pub is_placed: u32,
    /// 所在容器句柄；未落位为 0。
    pub host_container: u64,
    pub host_part: u32,
    pub x: u32,
    pub y: u32,
    pub rotated: u32,
}

/// 容器状态汇总。
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct FfiContainerInfo {
    /// [`ContainerType`] 的判别值。
    pub ctype: u32,
    pub part_count: u32,
    pub is_root: u32,
    /// 总格数。
    pub total_cells: u32,
    /// 已占格数（按格计，不是物品件数）。
    pub used_cells: u32,
}

// ==================== 错误信息槽 ====================

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").expect("空串合法"));
}

fn set_last_error(msg: &str) {
    let cleaned = msg.replace('\0', " ");
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() =
            CString::new(cleaned).unwrap_or_else(|_| CString::new("错误").expect("字面量合法"))
    });
}

fn panic_message(payload: Box<dyn Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        format!("Rust panic: {s}")
    } else if let Some(s) = payload.downcast_ref::<String>() {
        format!("Rust panic: {s}")
    } else {
        "Rust panic（信息无法读取）".to_string()
    }
}

fn guard_i32(f: impl FnOnce() -> InvResult<()>) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            set_last_error(&e.to_string());
            e.code()
        }
        Err(p) => {
            set_last_error(&panic_message(p));
            InventoryError::Panic.code()
        }
    }
}

fn guard_i64(f: impl FnOnce() -> InvResult<i64>) -> i64 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(v)) => v,
        Ok(Err(e)) => {
            set_last_error(&e.to_string());
            -(e.code() as i64)
        }
        Err(p) => {
            set_last_error(&panic_message(p));
            -(InventoryError::Panic.code() as i64)
        }
    }
}

fn guard_ptr(f: impl FnOnce() -> InvResult<*mut c_char>) -> *mut c_char {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(p)) => p,
        Ok(Err(e)) => {
            set_last_error(&e.to_string());
            std::ptr::null_mut()
        }
        Err(p) => {
            set_last_error(&panic_message(p));
            std::ptr::null_mut()
        }
    }
}

/// # Safety
/// `ptr` 必须来自 [`uerust_inventory_new`] 且尚未释放。
unsafe fn inv_mut<'a>(ptr: *mut Inventory) -> InvResult<&'a mut Inventory> {
    if ptr.is_null() {
        return Err(InventoryError::InvalidHandle);
    }
    Ok(unsafe { &mut *ptr })
}

/// # Safety
/// `ptr` 必须是以 NUL 结尾的合法 C 字符串。
unsafe fn cstr_to_str<'a>(ptr: *const c_char) -> InvResult<&'a str> {
    if ptr.is_null() {
        return Err(InventoryError::InvalidArgument);
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|_| InventoryError::InvalidUtf8)
}

fn into_c_string(s: String) -> *mut c_char {
    let cleaned = s.replace('\0', " ");
    CString::new(cleaned)
        .unwrap_or_else(|_| CString::new("字符串含 NUL").expect("字面量合法"))
        .into_raw()
}

/// 把 u64 数组写进调用方缓冲区；`cap == 0` 或 `out` 为空时只返回所需数量。
///
/// # Safety
/// `out` 非空时必须指向至少 `cap` 个 u64 的可写内存。
unsafe fn write_u64_array(items: &[u64], out: *mut u64, cap: u32) -> i64 {
    if out.is_null() || cap == 0 {
        return items.len() as i64;
    }
    if (cap as usize) < items.len() {
        return -(InventoryError::BufferTooSmall.code() as i64);
    }
    unsafe { std::ptr::copy_nonoverlapping(items.as_ptr(), out, items.len()) };
    items.len() as i64
}

// ==================== 库级入口 ====================

/// 库版本（静态字符串，**不需要**释放）。加载自检用。
#[no_mangle]
pub extern "C" fn uerust_plugin_version() -> *const c_char {
    static VERSION: LazyLock<CString> = LazyLock::new(|| {
        CString::new(format!(
            "战术网格背包系统 (Rust core) v{}",
            env!("CARGO_PKG_VERSION")
        ))
        .expect("版本号里不会有 NUL")
    });
    VERSION.as_ptr()
}

/// 最近一次失败的可读原因（线程本地，**不需要**释放）。
///
/// 指针在下一次失败前有效；不要跨线程使用，不要长期持有。
#[no_mangle]
pub extern "C" fn uerust_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

/// 释放 Rust 侧返回的字符串（[`uerust_inventory_selftest`] / `_container_label` 等）。
///
/// 传 NULL 安全；同一个指针不能释放两次。
///
/// # Safety
/// `ptr` 必须是本库返回的字符串指针。
#[no_mangle]
pub unsafe extern "C" fn uerust_free_string(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    unsafe { drop(CString::from_raw(ptr)) };
}

/// 释放 [`uerust_inventory_snapshot`] 返回的字节缓冲区。`len` 必须与取出时一致。
///
/// # Safety
/// `ptr`/`len` 必须来自同一次 [`uerust_inventory_snapshot`] 调用。
#[no_mangle]
pub unsafe extern "C" fn uerust_free_bytes(ptr: *mut u8, len: u32) {
    if ptr.is_null() {
        return;
    }
    let slice = std::ptr::slice_from_raw_parts_mut(ptr, len as usize);
    unsafe { drop(Box::from_raw(slice)) };
}

// ==================== 生命周期 ====================

/// 创建背包实例。失败返回 NULL（当前实现不会失败）。
#[no_mangle]
pub extern "C" fn uerust_inventory_new() -> *mut Inventory {
    Box::into_raw(Box::new(Inventory::new()))
}

/// 销毁背包实例。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且只能释放一次。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_free(inv: *mut Inventory) {
    if inv.is_null() {
        return;
    }
    unsafe { drop(Box::from_raw(inv)) };
}

// ==================== 目录 / 容器 / 物品 ====================

/// 注册物品定义。返回 `>= 0` 的 DefId；`< 0` 为 `-错误码`。
///
/// `container_dims`：每两个 u8 一组 (宽, 高)，表示这件物品自带的内部网格；
/// `container_part_count = 0` 表示普通物品（不传尺寸）。
///
/// # Safety
/// `name` 为合法 C 字符串；`container_dims` 在 `container_part_count > 0` 时指向
/// `container_part_count * 2` 个合法 u8。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_define_item(
    inv: *mut Inventory,
    name: *const c_char,
    width: u8,
    height: u8,
    rotatable: u32,
    max_stack: u32,
    weight: f32,
    value: u32,
    tag_bits: u32,
    forbidden_container_types: u32,
    container_dims: *const u8,
    container_part_count: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        let name = unsafe { cstr_to_str(name)? };
        if forbidden_container_types > u8::MAX as u32 {
            return Err(InventoryError::InvalidArgument);
        }
        let spec = if container_part_count == 0 {
            None
        } else {
            let n = container_part_count as usize;
            if n > MAX_PARTS || container_dims.is_null() {
                return Err(InventoryError::InvalidArgument);
            }
            let raw = unsafe { std::slice::from_raw_parts(container_dims, n * 2) };
            Some(ContainerSpec::compound(
                raw.as_chunks::<2>().0.iter().map(|c| (c[0], c[1])).collect(),
            ))
        };
        let mut def = ItemDef::new(name, width, height, max_stack)
            .rotatable(rotatable != 0)
            .weight(weight)
            .value(value)
            .tags(tag_bits)
            .forbid(forbidden_container_types as u8);
        if let Some(spec) = spec {
            def = def.as_container(spec);
        }
        Ok(inv.define_item(def)?.0 as i64)
    })
}

/// 创建根容器（0=口袋 1=弹挂 2=安全箱 3=背包）。返回 `> 0` 的容器句柄；`< 0` = `-错误码`。
///
/// `dims` 含义同 [`uerust_inventory_define_item`]。弹挂传多个 1x1 / 1x2 口继承复合容器。
///
/// # Safety
/// 同 [`uerust_inventory_define_item`] 的 `container_dims`。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_add_root(
    inv: *mut Inventory,
    ctype: u32,
    label: *const c_char,
    dims: *const u8,
    part_count: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        let label = unsafe { cstr_to_str(label)? };
        let t = ContainerType::from_u8(u8::try_from(ctype).unwrap_or(u8::MAX))
            .ok_or(InventoryError::InvalidDef)?;
        let dims_vec = read_dims(dims, part_count)?;
        let k = inv.add_root(t, label, &dims_vec)?;
        Ok(k.to_u64() as i64)
    })
}

/// 生成物品并落位。返回 `> 0` 的物品句柄；`< 0` = `-错误码`。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`dst` 必须是本库返回的容器句柄。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_add_item(
    inv: *mut Inventory,
    def: u32,
    stack: u32,
    durability: u32,
    dst: u64,
    part: u32,
    x: u32,
    y: u32,
    rotated: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        if def > u16::MAX as u32 || durability > u16::MAX as u32 {
            return Err(InventoryError::InvalidArgument);
        }
        let key = inv.add_item(
            DefId(def as u16),
            stack,
            durability as u16,
            ContainerKey::from_u64(dst),
            u8::try_from(part).map_err(|_| InventoryError::PartOutOfRange)?,
            u8::try_from(x).map_err(|_| InventoryError::OutOfBounds)?,
            u8::try_from(y).map_err(|_| InventoryError::OutOfBounds)?,
            rotated != 0,
        )?;
        Ok(key.to_u64() as i64)
    })
}

/// 删除物品及其套包内的全部内容。返回被删除的件数（`>= 0`）；`< 0` = `-错误码`。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`item` 必须是本库返回的物品句柄。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_destroy_item(inv: *mut Inventory, item: u64) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        Ok(inv.destroy_item(ItemKey::from_u64(item))? as i64)
    })
}

// ==================== 原子交互 ====================

/// 移动。失败时状态零变化。返回 0 或错误码。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；句柄必须来自本库的返回值。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_try_move(
    inv: *mut Inventory,
    item: u64,
    dst: u64,
    part: u32,
    x: u32,
    y: u32,
    rotated: u32,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        inv.try_move(
            ItemKey::from_u64(item),
            ContainerKey::from_u64(dst),
            u8::try_from(part).map_err(|_| InventoryError::PartOutOfRange)?,
            u8::try_from(x).map_err(|_| InventoryError::OutOfBounds)?,
            u8::try_from(y).map_err(|_| InventoryError::OutOfBounds)?,
            rotated != 0,
        )
    })
}

/// 两个物品原子互换（可跨容器）。返回 0 或错误码。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；句柄必须来自本库的返回值。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_try_swap(inv: *mut Inventory, a: u64, b: u64) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        inv.try_swap(ItemKey::from_u64(a), ItemKey::from_u64(b))
    })
}

/// 原地旋转。返回 0 或错误码。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`item` 必须是本库返回的物品句柄。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_try_rotate(inv: *mut Inventory, item: u64) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        inv.try_rotate(ItemKey::from_u64(item))
    })
}

/// 合并堆叠（`src` 合并进 `dst`）。`out_moved` 收实际转移数量。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`out_moved` 必须可写。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_try_merge(
    inv: *mut Inventory,
    src: u64,
    dst: u64,
    out_moved: *mut u32,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out_moved.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        let moved = inv.try_merge(ItemKey::from_u64(src), ItemKey::from_u64(dst))?;
        unsafe { *out_moved = moved };
        Ok(())
    })
}

/// 拆分堆叠。返回 `> 0` 的新物品句柄；`< 0` = `-错误码`。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；句柄必须来自本库的返回值。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_try_split(
    inv: *mut Inventory,
    item: u64,
    count: u32,
    dst: u64,
    part: u32,
    x: u32,
    y: u32,
    rotated: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        let key = inv.try_split(
            ItemKey::from_u64(item),
            count,
            ContainerKey::from_u64(dst),
            u8::try_from(part).map_err(|_| InventoryError::PartOutOfRange)?,
            u8::try_from(x).map_err(|_| InventoryError::OutOfBounds)?,
            u8::try_from(y).map_err(|_| InventoryError::OutOfBounds)?,
            rotated != 0,
        )?;
        Ok(key.to_u64() as i64)
    })
}

/// 一键整理。返回 0 或错误码（[`InventoryError::SortFailed`] 时布局未改动）。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`container` 必须是本库返回的容器句柄。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_autosort(inv: *mut Inventory, container: u64) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        inv.autosort(ContainerKey::from_u64(container))
    })
}

// ==================== 查询 ====================

/// 物品状态。
///
/// # Safety
/// `out` 指向可写的 [`FfiItemInfo`]。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_item_info(
    inv: *mut Inventory,
    item: u64,
    out: *mut FfiItemInfo,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        let key = ItemKey::from_u64(item);
        let inst = inv.item(key)?;
        let loc = inv.container_of_item(key);
        let info = FfiItemInfo {
            def_id: inst.def.0 as u32,
            stack: inst.stack,
            durability: inst.durability as u32,
            own_container: inst.container.map(|c| c.to_u64()).unwrap_or(0),
            is_placed: u32::from(loc.is_some()),
            host_container: loc.map(|(c, _, _)| c.to_u64()).unwrap_or(0),
            host_part: loc.map(|(_, p, _)| p as u32).unwrap_or(0),
            x: loc.map(|(_, _, m)| m.x as u32).unwrap_or(0),
            y: loc.map(|(_, _, m)| m.y as u32).unwrap_or(0),
            rotated: loc.map(|(_, _, m)| u32::from(m.rotated)).unwrap_or(0),
        };
        unsafe { *out = info };
        Ok(())
    })
}

/// 容器汇总。
///
/// # Safety
/// `out` 指向可写的 [`FfiContainerInfo`]。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_container_info(
    inv: *mut Inventory,
    container: u64,
    out: *mut FfiContainerInfo,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        let ct = inv.container(ContainerKey::from_u64(container))?;
        let (total, used) = ct.cell_stats();
        let info = FfiContainerInfo {
            ctype: ct.ctype() as u32,
            part_count: ct.part_count() as u32,
            is_root: u32::from(ct.ctype().is_root()),
            total_cells: total as u32,
            used_cells: used as u32,
        };
        unsafe { *out = info };
        Ok(())
    })
}

/// 某个子网格的尺寸（宽写 `out_w`，高写 `out_h`）。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`out_w` / `out_h` 必须可写。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_container_part_size(
    inv: *mut Inventory,
    container: u64,
    part: u32,
    out_w: *mut u32,
    out_h: *mut u32,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out_w.is_null() || out_h.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        let ct = inv.container(ContainerKey::from_u64(container))?;
        let g = ct
            .part(part as usize)
            .ok_or(InventoryError::PartOutOfRange)?;
        unsafe {
            *out_w = g.width() as u32;
            *out_h = g.height() as u32;
        }
        Ok(())
    })
}

/// 容器显示名（调用方用 [`uerust_free_string`] 释放）。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`container` 必须是本库返回的容器句柄。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_container_label(
    inv: *mut Inventory,
    container: u64,
) -> *mut c_char {
    guard_ptr(move || {
        let inv = unsafe { inv_mut(inv)? };
        let ct = inv.container(ContainerKey::from_u64(container))?;
        Ok(into_c_string(ct.label().to_string()))
    })
}

/// 导出某块子网格的占格表（行主序，长度为 `w * h`；0 = 空，否则是物品句柄）。
///
/// 两段式：`cap = 0` 或 `out = NULL` 时只返回格数；缓冲区不足返回 `-BufferTooSmall`。
///
/// # Safety
/// `out` 非空时指向至少 `cap` 个 u64。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_container_grid(
    inv: *mut Inventory,
    container: u64,
    part: u32,
    out: *mut u64,
    cap: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        let ct = inv.container(ContainerKey::from_u64(container))?;
        let g = ct
            .part(part as usize)
            .ok_or(InventoryError::PartOutOfRange)?;
        let cells: Vec<u64> = g
            .cells()
            .iter()
            .map(|c| c.map(|k| k.to_u64()).unwrap_or(0))
            .collect();
        Ok(unsafe { write_u64_array(&cells, out, cap) })
    })
}

/// 物品当前落在哪个容器的哪块网格（未落位返回 [`InventoryError::NotPlaced`]）。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`out_container` / `out_part` 必须可写。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_item_location(
    inv: *mut Inventory,
    item: u64,
    out_container: *mut u64,
    out_part: *mut u32,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out_container.is_null() || out_part.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        let (c, p, _) = inv
            .container_of_item(ItemKey::from_u64(item))
            .ok_or(InventoryError::NotPlaced)?;
        unsafe {
            *out_container = c.to_u64();
            *out_part = p as u32;
        }
        Ok(())
    })
}

/// 总负重。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`out` 必须可写。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_total_weight(
    inv: *mut Inventory,
    out: *mut f64,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        unsafe { *out = inv.aggregates().weight };
        Ok(())
    })
}

/// 总估值。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`out` 必须可写。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_total_value(inv: *mut Inventory, out: *mut u64) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        unsafe { *out = inv.aggregates().value };
        Ok(())
    })
}

/// 设置总承重上限（kg）。`kg <= 0`、NaN 或 ±inf 一律视为**取消上限**。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_set_max_capacity(inv: *mut Inventory, kg: f32) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        let rules = inv.rules();
        inv.set_rules(rules.with_max_capacity(kg));
        Ok(())
    })
}

/// 负重比（总重 ÷ 上限）：无上限时写 `0.0`，超重时 > 1.0。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放；`out` 必须可写。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_weight_ratio(inv: *mut Inventory, out: *mut f32) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        unsafe { *out = inv.weight_ratio() };
        Ok(())
    })
}

/// 是否超重：`1` = 超重，`0` = 正常，`< 0` = `-错误码`。无上限时恒为 `0`。
///
/// # Safety
/// `inv` 必须来自 [`uerust_inventory_new`] 且尚未释放。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_is_overloaded(inv: *mut Inventory) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        Ok(i64::from(inv.is_overloaded()))
    })
}

/// 标签查询。两段式（见 [`uerust_inventory_container_grid`]），返回命中数量。
///
/// # Safety
/// `out` 非空时指向至少 `cap` 个 u64。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_find_by_tag(
    inv: *mut Inventory,
    tag_mask: u32,
    out: *mut u64,
    cap: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        let found: Vec<u64> = inv.find_by_tag(tag_mask).iter().map(|k| k.to_u64()).collect();
        Ok(unsafe { write_u64_array(&found, out, cap) })
    })
}

/// 全部容器（根在前，确定性顺序）。两段式，返回数量。
///
/// # Safety
/// `out` 非空时指向至少 `cap` 个 u64。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_containers(
    inv: *mut Inventory,
    out: *mut u64,
    cap: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        let all: Vec<u64> = inv.all_containers().iter().map(|k| k.to_u64()).collect();
        Ok(unsafe { write_u64_array(&all, out, cap) })
    })
}

/// 全部物品。两段式，返回数量。
///
/// # Safety
/// `out` 非空时指向至少 `cap` 个 u64。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_items(
    inv: *mut Inventory,
    out: *mut u64,
    cap: u32,
) -> i64 {
    guard_i64(move || {
        let inv = unsafe { inv_mut(inv)? };
        let all: Vec<u64> = inv.all_items().iter().map(|k| k.to_u64()).collect();
        Ok(unsafe { write_u64_array(&all, out, cap) })
    })
}

// ==================== 序列化 ====================

/// 导出 bincode 快照。写入 `*out_ptr` / `*out_len`，必须用 [`uerust_free_bytes`] 释放。
///
/// # Safety
/// `out_ptr` / `out_len` 可写。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_snapshot(
    inv: *mut Inventory,
    out_ptr: *mut *mut u8,
    out_len: *mut u32,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if out_ptr.is_null() || out_len.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        let bytes = inv.snapshot().encode()?;
        let len = bytes.len();
        let boxed = bytes.into_boxed_slice();
        let ptr = Box::into_raw(boxed).cast::<u8>();
        unsafe {
            *out_ptr = ptr;
            *out_len = len as u32;
        }
        Ok(())
    })
}

/// 从 bincode 快照恢复（目录与规则保持不变；快照损坏时现有数据不受影响）。
///
/// # Safety
/// `bytes` 指向至少 `len` 个可读字节。
#[no_mangle]
pub unsafe extern "C" fn uerust_inventory_restore(
    inv: *mut Inventory,
    bytes: *const u8,
    len: u32,
) -> i32 {
    guard_i32(move || {
        let inv = unsafe { inv_mut(inv)? };
        if bytes.is_null() {
            return Err(InventoryError::InvalidArgument);
        }
        let slice = unsafe { std::slice::from_raw_parts(bytes, len as usize) };
        let snap = crate::core::snapshot::Snapshot::decode(slice)?;
        inv.restore(&snap)
    })
}

// ==================== 自检 ====================

/// 跑一遍内置场景（建容器 / 放物品 / 旋转 / 移动 / 互换 / 堆叠 / 套包 / 成环 / 黑名单 /
/// 整理 / 快照），返回逐项结果的可读报告。UE 蓝图里按一下就能验证整条链路。
///
/// 返回的字符串用 [`uerust_free_string`] 释放。
#[no_mangle]
pub extern "C" fn uerust_inventory_selftest() -> *mut c_char {
    match catch_unwind(AssertUnwindSafe(selftest_report)) {
        Ok(report) => into_c_string(report),
        Err(p) => {
            let msg = panic_message(p);
            set_last_error(&msg);
            into_c_string(format!("Rust 自检发生 panic：{msg}"))
        }
    }
}

fn read_dims(dims: *const u8, part_count: u32) -> InvResult<Vec<(u8, u8)>> {
    let n = part_count as usize;
    if n == 0 || n > MAX_PARTS || dims.is_null() {
        return Err(InventoryError::InvalidArgument);
    }
    let raw = unsafe { std::slice::from_raw_parts(dims, n * 2) };
    Ok(raw.as_chunks::<2>().0.iter().map(|c| (c[0], c[1])).collect())
}

// ==================== 自检场景实现 ====================

struct Report {
    lines: Vec<String>,
    passed: usize,
    failed: usize,
}

impl Report {
    fn new() -> Self {
        Self {
            lines: Vec::new(),
            passed: 0,
            failed: 0,
        }
    }

    fn ok(&mut self, msg: String) {
        self.passed += 1;
        self.lines.push(format!("[✓] {msg}"));
    }

    fn fail(&mut self, msg: String) {
        self.failed += 1;
        self.lines.push(format!("[✗] {msg}"));
    }

    fn check(&mut self, cond: bool, msg: &str) {
        if cond {
            self.ok(msg.to_string());
        } else {
            self.fail(msg.to_string());
        }
    }

    fn expect_err<T: std::fmt::Debug>(
        &mut self,
        r: InvResult<T>,
        want: InventoryError,
        msg: &str,
    ) {
        match r {
            Err(e) if e == want => self.ok(format!("{msg}（预期失败：错误码 {}）", want.code())),
            Err(e) => self.fail(format!(
                "{msg}：预期错误码 {}，实际 {}",
                want.code(),
                e.code()
            )),
            Ok(v) => self.fail(format!("{msg}：预期失败，却成功了（{v:?}）")),
        }
    }
}

fn selftest_report() -> String {
    let mut r = Report::new();
    let mut inv = Inventory::new();

    // ---- 目录 ----
    let ammo = inv
        .define_item(
            ItemDef::new("9x19 弹药", 1, 1, 60)
                .weight(0.012)
                .value(3)
                .tags(tags::AMMO),
        )
        .expect("弹药定义");
    let magazine = inv
        .define_item(
            ItemDef::new("AK 弹匣", 1, 2, 1)
                .rotatable(true)
                .weight(0.2)
                .value(120)
                .tags(tags::MAGAZINE),
        )
        .expect("弹匣定义");
    let medkit = inv
        .define_item(
            ItemDef::new("急救包", 1, 1, 3)
                .weight(0.5)
                .value(300)
                .tags(tags::MEDKIT),
        )
        .expect("急救包定义");
    let grenade = inv
        .define_item(
            ItemDef::new("手雷", 1, 1, 1)
                .weight(0.4)
                .value(200)
                .tags(tags::GRENADE),
        )
        .expect("手雷定义");
    let gold = inv
        .define_item(
            ItemDef::new("大金表", 2, 2, 1)
                .weight(1.0)
                .value(5000)
                .tags(tags::VALUABLE)
                .forbid(ContainerType::SafeBox.bit()),
        )
        .expect("大金定义");
    let bag_big = inv
        .define_item(
            ItemDef::new("突击背包", 3, 3, 1)
                .weight(2.0)
                .value(900)
                .as_container(ContainerSpec::rectangular(6, 5)),
        )
        .expect("大包定义");
    let bag_small = inv
        .define_item(
            ItemDef::new("小挎包", 2, 2, 1)
                .weight(1.0)
                .value(300)
                .as_container(ContainerSpec::rectangular(3, 3)),
        )
        .expect("小包定义");
    let pouch = inv
        .define_item(
            ItemDef::new("小布袋", 1, 1, 1)
                .weight(0.2)
                .value(50)
                .as_container(ContainerSpec::rectangular(2, 2)),
        )
        .expect("布袋定义");
    r.ok(format!(
        "目录：注册 {} 件物品定义（DefId 0..{})",
        inv.catalog().len(),
        inv.catalog().len()
    ));

    // ---- 根容器 ----
    let pockets = inv
        .add_root(
            ContainerType::Pockets,
            "口袋",
            &[(1, 1), (1, 1), (1, 2), (1, 2)],
        )
        .expect("口袋");
    let rig = inv
        .add_root(ContainerType::ChestRig, "战术弹挂", &[(1, 1), (1, 1), (2, 2)])
        .expect("弹挂");
    let safebox = inv
        .add_root(ContainerType::SafeBox, "安全箱", &[(4, 3)])
        .expect("安全箱");
    let backpack = inv
        .add_root(ContainerType::Backpack, "背包", &[(6, 5)])
        .expect("背包");
    r.check(
        inv.roots().iter().count() == 4,
        "根容器：口袋(复合) / 弹挂(复合) / 安全箱 / 背包 全部就绪",
    );

    // ---- 放置与碰撞 ----
    let ammo_key = inv
        .add_item(ammo, 60, 0, pockets, 0, 0, 0, false)
        .expect("放弹药");
    let count_before = inv.item_count();
    r.expect_err(
        inv.add_item(ammo, 30, 0, pockets, 0, 0, 0, false),
        InventoryError::Occupied,
        "同一个口袋格重复放弹药",
    );
    r.check(
        inv.item_count() == count_before,
        "失败零副作用：物品总数没变",
    );
    let medkit_key = inv
        .add_item(medkit, 3, 0, pockets, 1, 0, 0, false)
        .expect("放急救包");
    let grenade_key = inv
        .add_item(grenade, 1, 0, rig, 0, 0, 0, false)
        .expect("放手雷");
    let mag_key = inv
        .add_item(magazine, 1, 0, rig, 2, 0, 0, false)
        .expect("放弹匣");
    r.ok(format!(
        "放置：弹药/急救包进各口袋，手雷/弹匣进弹挂（{} 件物品）",
        inv.item_count()
    ));

    // ---- 旋转 ----
    r.check(inv.try_rotate(mag_key).is_ok(), "弹匣在 2x2 口袋里旋转成功（1x2 → 2x1）");
    r.expect_err(
        inv.try_rotate(grenade_key),
        InventoryError::NotRotatable,
        "手雷不可旋转",
    );

    // ---- 移动 ----
    r.check(
        inv.try_move(mag_key, backpack, 0, 0, 0, true).is_ok(),
        "弹匣从弹挂移到背包（保持旋转态）",
    );
    r.check(
        inv.try_rotate(mag_key).is_ok(),
        "弹匣在背包里再转回 1x2",
    );

    // ---- 原子互换 ----
    r.check(
        inv.try_swap(grenade_key, medkit_key).is_ok(),
        "互换：手雷 ↔ 急救包（两个 1x1 口袋位）",
    );
    r.expect_err(
        inv.try_swap(medkit_key, mag_key),
        InventoryError::OutOfBounds,
        "互换失败：1x2 弹匣塞不进 1x1 的手雷位",
    );
    r.check(
        inv.check_invariants().is_empty(),
        "互换失败后状态自洽（无半成品改动）",
    );

    // ---- 堆叠 ----
    let split_key = inv
        .try_split(ammo_key, 20, backpack, 0, 2, 0, false)
        .expect("拆分 20 发");
    r.check(
        inv.item(ammo_key).expect("弹药还在").stack == 40
            && inv.item(split_key).expect("新堆叠在").stack == 20,
        "拆分：60 发 → 40 + 20",
    );
    r.check(
        inv.try_merge(split_key, ammo_key).expect("合并") == 20
            && inv.item(ammo_key).expect("弹药还在").stack == 60,
        "合并：20 发回填，原堆叠回到 60",
    );

    // ---- 套包 ----
    let bag_big_key = inv
        .add_item(bag_big, 1, 0, backpack, 0, 3, 0, false)
        .expect("放大包进背包");
    let bag_big_inner = inv.item(bag_big_key).expect("大包").container.expect("大包有内部空间");
    let bag_small_key = inv
        .add_item(bag_small, 1, 0, bag_big_inner, 0, 0, 0, false)
        .expect("小包进大包");
    let bag_small_inner = inv.item(bag_small_key).expect("小包").container.expect("小包有内部空间");
    let pouch_key = inv
        .add_item(pouch, 1, 0, bag_small_inner, 0, 0, 0, false)
        .expect("布袋进小包");
    let pouch_inner = inv.item(pouch_key).expect("布袋").container.expect("布袋有内部空间");
    r.ok(format!(
        "套包：背包(深度 {}) > 大包(深度 {}) > 小包(深度 {}) > 布袋(深度 {})",
        inv.depth_of(backpack).expect("深度"),
        inv.depth_of(bag_big_inner).expect("深度"),
        inv.depth_of(bag_small_inner).expect("深度"),
        inv.depth_of(pouch_inner).expect("深度"),
    ));

    // ---- 成环 / 深度 / 黑名单 ----
    r.expect_err(
        inv.add_item(pouch, 1, 0, pouch_inner, 0, 0, 0, false),
        InventoryError::NestingTooDeep,
        "继续往下套包（第 5 层）",
    );
    r.expect_err(
        inv.try_move(bag_big_key, bag_small_inner, 0, 0, 0, false),
        InventoryError::WouldCycle,
        "把大包塞进自己内部的小包（成环）",
    );
    r.expect_err(
        inv.add_item(gold, 1, 0, safebox, 0, 0, 0, false),
        InventoryError::ForbiddenContainer,
        "大金表禁止进安全箱",
    );
    let gold_key = inv.add_item(gold, 1, 0, backpack, 0, 0, 3, false);
    match gold_key {
        Ok(k) => {
            r.ok("同一件大金表可以进背包".to_string());
            // 同网格互换：走 can_place_pair（位图/逐格扫描两条路径语义一致）
            r.check(
                inv.try_swap(mag_key, k).is_ok(),
                "同网格互换：弹匣 ↔ 大金表（两块新落点互不重叠）",
            );
        }
        Err(e) => r.fail(format!("大金表进背包失败：{e}")),
    }

    // ---- 统计 ----
    let agg = inv.aggregates();
    r.ok(format!(
        "统计：物品 {} 件 / 总重 {:.3} kg / 总估值 {}",
        agg.item_count, agg.weight, agg.value
    ));
    let mags = inv.find_by_tag(tags::MAGAZINE);
    r.check(
        mags.len() == 1,
        &format!("标签查询：弹匣 × {}", mags.len()),
    );

    // ---- 负重 ----
    let total_kg = inv.total_weight() as f32;
    let rules = inv.rules();
    inv.set_rules(rules.with_max_capacity(total_kg * 0.8));
    r.check(
        inv.is_overloaded() && (inv.weight_ratio() - 1.25).abs() < 1e-3,
        &format!(
            "负重：总重 {:.3} kg / 上限 {:.3} kg → 比例 {:.2}（判定超重）",
            total_kg,
            total_kg * 0.8,
            inv.weight_ratio()
        ),
    );
    let rules = inv.rules();
    inv.set_rules(rules.with_max_capacity(total_kg * 1.5));
    r.check(
        !inv.is_overloaded(),
        &format!(
            "上限放宽到 {:.2} kg → 不再超重（比例 {:.2}）",
            total_kg * 1.5,
            inv.weight_ratio()
        ),
    );
    let rules = inv.rules();
    inv.set_rules(rules.with_max_capacity(0.0));
    r.check(
        inv.weight_ratio() == 0.0,
        "上限传 0 → 视为无限制（负重比 0.0）",
    );

    // ---- 自动整理 ----
    let before_items = inv.items_in(backpack).len();
    let before_used = inv.container(backpack).expect("背包").cell_stats().1;
    r.check(
        inv.autosort(backpack).is_ok(),
        "背包一键整理成功（沙盒重排 + 整体提交）",
    );
    let after = inv.container(backpack).expect("背包").cell_stats();
    r.check(
        after.1 == before_used && inv.items_in(backpack).len() == before_items,
        &format!("整理后件数不变、占用 {}/{} 格", after.1, after.0),
    );

    // ---- 快照 ----
    let snap = inv.snapshot();
    match snap.encode() {
        Ok(bytes) => {
            r.ok(format!(
                "快照：{} 件物品 / {} 个容器 → {} 字节（bincode）",
                snap.items.len(),
                snap.containers.len(),
                bytes.len()
            ));
            match Snapshot::decode(&bytes) {
                Ok(decoded) => {
                    if decoded != snap {
                        r.fail("快照解码结果与原快照不一致".to_string());
                    } else if let Err(e) = inv.restore(&decoded) {
                        r.fail(format!("快照恢复失败：{e}"));
                    } else {
                        r.check(
                            inv.snapshot() == snap,
                            "快照恢复后再次导出，字节完全一致（确定性回放）",
                        );
                    }
                }
                Err(e) => r.fail(format!("快照解码失败：{e}")),
            }
        }
        Err(e) => r.fail(format!("快照编码失败：{e}")),
    }

    // ---- 总自检 ----
    let errs = inv.check_invariants();
    r.check(
        errs.is_empty(),
        &format!(
            "全库自检：{}",
            if errs.is_empty() {
                "0 个问题".to_string()
            } else {
                errs.join("；")
            }
        ),
    );

    let mut out = String::new();
    out.push_str("===== 战术网格背包系统 · Rust 自检 =====\n");
    for line in &r.lines {
        out.push_str(line);
        out.push('\n');
    }
    out.push_str(&format!(
        "===== 通过 {} 项，失败 {} 项 =====",
        r.passed, r.failed
    ));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selftest_all_green() {
        let report = selftest_report();
        assert!(
            !report.contains("[✗]"),
            "自检有失败项：\n{report}"
        );
        assert!(report.contains("快照"), "{report}");
    }

    #[test]
    fn null_handles_are_rejected() {
        let code = unsafe {
            uerust_inventory_try_rotate(std::ptr::null_mut(), 1)
        };
        assert_eq!(code, InventoryError::InvalidHandle.code());
    }

    #[test]
    fn ffi_roundtrip_basic() {
        let inv = uerust_inventory_new();
        assert!(!inv.is_null());
        let name = CString::new("背包").expect("合法");
        let ck = unsafe {
            uerust_inventory_add_root(
                inv,
                ContainerType::Backpack as u32,
                name.as_ptr(),
                [(4u8, 4u8)].as_ptr().cast(),
                1,
            )
        };
        assert!(ck > 0, "建根容器失败: {ck}");
        let def = unsafe {
            uerust_inventory_define_item(
                inv,
                name.as_ptr(),
                1,
                1,
                0,
                1,
                0.5,
                10,
                0,
                0,
                std::ptr::null(),
                0,
            )
        };
        assert!(def >= 0);
        let item = unsafe { uerust_inventory_add_item(inv, def as u32, 1, 0, ck as u64, 0, 0, 0, 0) };
        assert!(item > 0);
        let mut out = FfiItemInfo {
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
        };
        let code = unsafe { uerust_inventory_item_info(inv, item as u64, &mut out) };
        assert_eq!(code, 0);
        assert_eq!(out.host_container, ck as u64);
        assert_eq!(out.is_placed, 1);
        unsafe { uerust_inventory_free(inv) };
        assert_eq!(uerust_plugin_version().is_null(), false);
    }
}
