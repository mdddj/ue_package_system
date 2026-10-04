/* 桥接冒烟：用 dlopen 加载真实产出的 dylib，走一遍 UE 侧运行时实际走的路径
 * （GetDllHandle → GetDllExport → 调用 → 释放）。
 *
 * 不依赖 UE，任何一台机器都能跑：见同目录 run.sh。
 * 期望输出：「通过 N 项，失败 0 项」「冒烟：全部通过」，退出码 0。
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef void* (*NewFn)(void);
typedef void (*FreeInvFn)(void*);
typedef const char* (*VoidStrFn)(void);
typedef char* (*VoidCharFn)(void);
typedef void (*FreeStrFn)(char*);
typedef long long (*DefineItemFn)(void*, const char*, unsigned char, unsigned char,
                                  unsigned int, unsigned int, float, unsigned int,
                                  unsigned int, unsigned int, const unsigned char*, unsigned int);
typedef long long (*AddRootFn)(void*, unsigned int, const char*, const unsigned char*, unsigned int);
typedef long long (*AddItemFn)(void*, unsigned int, unsigned int, unsigned int,
                               unsigned long long, unsigned int, unsigned int, unsigned int, unsigned int);
typedef int (*TryRotateFn)(void*, unsigned long long);
typedef int (*SetCapacityFn)(void*, float);
typedef int (*RatioFn)(void*, float*);
typedef long long (*OverloadedFn)(void*);

int main(int argc, char** argv)
{
    if (argc < 2) { printf("用法: %s <dylib 路径>\n", argv[0]); return 1; }
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen 失败: %s\n", dlerror()); return 1; }

    VoidStrFn ver = (VoidStrFn)dlsym(h, "uerust_plugin_version");
    VoidCharFn selftest = (VoidCharFn)dlsym(h, "uerust_inventory_selftest");
    FreeStrFn free_str = (FreeStrFn)dlsym(h, "uerust_free_string");
    VoidStrFn last_err = (VoidStrFn)dlsym(h, "uerust_last_error");
    NewFn inv_new = (NewFn)dlsym(h, "uerust_inventory_new");
    FreeInvFn inv_free = (FreeInvFn)dlsym(h, "uerust_inventory_free");
    DefineItemFn define_item = (DefineItemFn)dlsym(h, "uerust_inventory_define_item");
    AddRootFn add_root = (AddRootFn)dlsym(h, "uerust_inventory_add_root");
    AddItemFn add_item = (AddItemFn)dlsym(h, "uerust_inventory_add_item");
    TryRotateFn try_rotate = (TryRotateFn)dlsym(h, "uerust_inventory_try_rotate");
    SetCapacityFn set_capacity = (SetCapacityFn)dlsym(h, "uerust_inventory_set_max_capacity");
    RatioFn weight_ratio = (RatioFn)dlsym(h, "uerust_inventory_weight_ratio");
    OverloadedFn overloaded = (OverloadedFn)dlsym(h, "uerust_inventory_is_overloaded");

    if (!ver || !selftest || !free_str || !last_err || !inv_new || !inv_free ||
        !define_item || !add_root || !add_item || !try_rotate ||
        !set_capacity || !weight_ratio || !overloaded) {
        printf("符号缺失: %s\n", dlerror() ? dlerror() : "?");
        return 2;
    }

    printf("版本: %s\n", ver());

    char* report = selftest();
    printf("---- 自检报告 ----\n%s\n------------------\n", report ? report : "(null)");
    int failed = (report == NULL);
    if (report) { failed |= (strstr(report, "[✗]") != NULL); free_str(report); }

    /* 手动驱动：建背包 → 定义不可旋转的手雷 → 落位 → 旋转应失败(4) → 负重接口 */
    void* inv = inv_new();
    const unsigned char dims[2] = {6, 5};
    long long bag = add_root(inv, 3, "背包", dims, 1);
    long long grenade = define_item(inv, "手雷", 1, 1, 0, 1, 0.4f, 200, 0, 0, 0, 0);
    if (bag <= 0 || grenade < 0) { printf("建容器/定义失败 bag=%lld def=%lld\n", bag, grenade); return 3; }
    long long item = add_item(inv, (unsigned int)grenade, 1, 0, (unsigned long long)bag, 0, 0, 0, 0);
    if (item <= 0) { printf("落位失败 item=%lld\n", item); return 3; }
    int code = try_rotate(inv, (unsigned long long)item);
    printf("旋转不可旋转物品 → code=%d（期望 4）; last_error=%s\n", code, last_err());
    failed |= (code != 4);

    /* 负重：0.4 kg 物品、上限 0.2 kg → 超重；0.8 kg → 正常 */
    failed |= (set_capacity(inv, 0.2f) != 0);
    float ratio = 0.0f;
    failed |= (weight_ratio(inv, &ratio) != 0);
    long long over = overloaded(inv);
    printf("负重：比例=%.2f（期望 2.00） 超重=%lld（期望 1）\n", ratio, over);
    failed |= (ratio < 1.99f || ratio > 2.01f);
    failed |= (over != 1);
    failed |= (set_capacity(inv, 0.8f) != 0);
    long long over2 = overloaded(inv);
    printf("上限 0.8 kg → 超重=%lld（期望 0）\n", over2);
    failed |= (over2 != 0);

    inv_free(inv);

    dlclose(h);
    printf(failed ? "冒烟：有问题\n" : "冒烟：全部通过\n");
    return failed ? 4 : 0;
}
