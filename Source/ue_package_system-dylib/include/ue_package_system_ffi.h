#ifndef UE_PACKAGE_SYSTEM_FFI_H
#define UE_PACKAGE_SYSTEM_FFI_H

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

constexpr static const uint8_t MAX_DEF_DIM = 32;

constexpr static const uintptr_t MAX_PART_CELLS = 1024;

constexpr static const uintptr_t MAX_PARTS = 16;

constexpr static const uintptr_t MAX_NAME_BYTES = 128;

constexpr static const uintptr_t ROOT_TYPE_COUNT = 4;

constexpr static const uintptr_t MAX_CELLS = 4096;

constexpr static const uintptr_t MASK_MAX_CELLS = 64;

constexpr static const uint8_t MAX_DIM = 255;

constexpr static const uint32_t MAGAZINE = (1 << 0);

constexpr static const uint32_t AMMO = (1 << 1);

constexpr static const uint32_t MEDKIT = (1 << 2);

constexpr static const uint32_t GRENADE = (1 << 3);

constexpr static const uint32_t ARMOR = (1 << 4);

constexpr static const uint32_t HELMET = (1 << 5);

constexpr static const uint32_t VALUABLE = (1 << 6);

constexpr static const uint32_t KEY = (1 << 7);

constexpr static const uint32_t CONTAINER = (1 << 8);

constexpr static const uint8_t MAX_ALLOWED_NESTING_DEPTH = 16;

struct Inventory;

struct FfiItemInfo {
  uint32_t def_id;
  uint32_t stack;
  uint32_t durability;
  uint64_t own_container;
  uint32_t is_placed;
  uint64_t host_container;
  uint32_t host_part;
  uint32_t x;
  uint32_t y;
  uint32_t rotated;
};

struct FfiContainerInfo {
  uint32_t ctype;
  uint32_t part_count;
  uint32_t is_root;
  uint32_t total_cells;
  uint32_t used_cells;
};

extern "C" {

const char *uerust_plugin_version();

const char *uerust_last_error();

void uerust_free_string(char *ptr);

void uerust_free_bytes(uint8_t *ptr, uint32_t len);

Inventory *uerust_inventory_new();

void uerust_inventory_free(Inventory *inv);

int64_t uerust_inventory_define_item(Inventory *inv,
                                     const char *name,
                                     uint8_t width,
                                     uint8_t height,
                                     uint32_t rotatable,
                                     uint32_t max_stack,
                                     float weight,
                                     uint32_t value,
                                     uint32_t tag_bits,
                                     uint32_t forbidden_container_types,
                                     const uint8_t *container_dims,
                                     uint32_t container_part_count);

int64_t uerust_inventory_add_root(Inventory *inv,
                                  uint32_t ctype,
                                  const char *label,
                                  const uint8_t *dims,
                                  uint32_t part_count);

int64_t uerust_inventory_add_item(Inventory *inv,
                                  uint32_t def,
                                  uint32_t stack,
                                  uint32_t durability,
                                  uint64_t dst,
                                  uint32_t part,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t rotated);

int64_t uerust_inventory_destroy_item(Inventory *inv, uint64_t item);

int32_t uerust_inventory_try_move(Inventory *inv,
                                  uint64_t item,
                                  uint64_t dst,
                                  uint32_t part,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t rotated);

int32_t uerust_inventory_try_swap(Inventory *inv, uint64_t a, uint64_t b);

int32_t uerust_inventory_try_rotate(Inventory *inv, uint64_t item);

int32_t uerust_inventory_try_merge(Inventory *inv, uint64_t src, uint64_t dst, uint32_t *out_moved);

int64_t uerust_inventory_try_split(Inventory *inv,
                                   uint64_t item,
                                   uint32_t count,
                                   uint64_t dst,
                                   uint32_t part,
                                   uint32_t x,
                                   uint32_t y,
                                   uint32_t rotated);

int32_t uerust_inventory_autosort(Inventory *inv, uint64_t container);

int32_t uerust_inventory_item_info(Inventory *inv, uint64_t item, FfiItemInfo *out);

int32_t uerust_inventory_container_info(Inventory *inv, uint64_t container, FfiContainerInfo *out);

int32_t uerust_inventory_container_part_size(Inventory *inv,
                                             uint64_t container,
                                             uint32_t part,
                                             uint32_t *out_w,
                                             uint32_t *out_h);

char *uerust_inventory_container_label(Inventory *inv, uint64_t container);

int64_t uerust_inventory_container_grid(Inventory *inv,
                                        uint64_t container,
                                        uint32_t part,
                                        uint64_t *out,
                                        uint32_t cap);

int32_t uerust_inventory_item_location(Inventory *inv,
                                       uint64_t item,
                                       uint64_t *out_container,
                                       uint32_t *out_part);

int32_t uerust_inventory_total_weight(Inventory *inv, double *out);

int32_t uerust_inventory_total_value(Inventory *inv, uint64_t *out);

int32_t uerust_inventory_set_max_capacity(Inventory *inv, float kg);

int32_t uerust_inventory_weight_ratio(Inventory *inv, float *out);

int64_t uerust_inventory_is_overloaded(Inventory *inv);

int64_t uerust_inventory_find_by_tag(Inventory *inv,
                                     uint32_t tag_mask,
                                     uint64_t *out,
                                     uint32_t cap);

int64_t uerust_inventory_containers(Inventory *inv, uint64_t *out, uint32_t cap);

int64_t uerust_inventory_items(Inventory *inv, uint64_t *out, uint32_t cap);

int32_t uerust_inventory_snapshot(Inventory *inv, uint8_t **out_ptr, uint32_t *out_len);

int32_t uerust_inventory_restore(Inventory *inv, const uint8_t *bytes, uint32_t len);

char *uerust_inventory_selftest();

}  // extern "C"

#endif  // UE_PACKAGE_SYSTEM_FFI_H
