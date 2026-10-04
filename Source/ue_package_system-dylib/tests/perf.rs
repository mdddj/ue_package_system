//! 性能门禁：`can_place` 单次中位数。
//!
//! - debug 构建只打印并跳过硬断言；
//! - release 构建（`cargo test --release --test perf -- --nocapture`）要求中位数 ≤ 500 ns。

use std::hint::black_box;
use std::time::Instant;

use ue_package_system_dylib::{GridContainer, ItemKey, PlacedMeta};

/// 在网格里按行优先找第一个能放的位置并落位。
fn try_place(g: &mut GridContainer, next: &mut u64, w: u8, h: u8, gw: u8, gh: u8) -> bool {
    for y in 0..=(gh - h) {
        for x in 0..=(gw - w) {
            if g.can_place(x, y, w, h) {
                let k = ItemKey::from_u64(*next);
                *next += 1;
                g.place(
                    k,
                    PlacedMeta { x, y, rotated: false, base_w: w, base_h: h, rotatable: false },
                )
                .unwrap();
                return true;
            }
        }
    }
    false
}

/// 构造 10x10、约 90% 满载的混合网格，返回 (网格, 已占格数)。
fn build_filled() -> (GridContainer, usize) {
    let (gw, gh) = (10u8, 10u8);
    let mut g = GridContainer::new(gw, gh).unwrap();
    let mut next = 1u64;
    let mut used = 0usize;
    let sizes = [(1u8, 1u8), (1u8, 2u8), (2u8, 2u8)];

    'outer: loop {
        let mut progressed = false;
        for &(w, h) in &sizes {
            let area = w as usize * h as usize;
            if used + area > 96 {
                continue;
            }
            if try_place(&mut g, &mut next, w, h, gw, gh) {
                used += area;
                progressed = true;
            }
        }
        if !progressed || used >= 90 {
            break 'outer;
        }
    }
    (g, used)
}

/// 每批 `batch` 次调用取一次平均耗时，重复 `samples` 次后取中位数（纳秒/次）。
fn median_per_call<F: FnMut()>(mut f: F, batch: usize, samples: usize) -> f64 {
    let mut vals = Vec::with_capacity(samples);
    for _ in 0..samples {
        let t = Instant::now();
        for _ in 0..batch {
            f();
        }
        vals.push(t.elapsed().as_nanos() as f64 / batch as f64);
    }
    vals.sort_by(|a, b| a.partial_cmp(b).unwrap());
    vals[samples / 2]
}

#[test]
fn can_place_median_is_fast() {
    let (g, used) = build_filled();
    println!("10x10 背包：已占 {used}/100 格");

    // 固定伪随机位置序列（xorshift，确定性）。
    let mut seed = 0x9E37_79B9_7F4A_7C15u64;
    let mut rnd = || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };
    let probes: Vec<(u8, u8)> =
        (0..1024).map(|_| ((rnd() % 10) as u8, (rnd() % 10) as u8)).collect();

    let mut idx = 0usize;
    let fast = median_per_call(
        || {
            let (x, y) = probes[idx % probes.len()];
            idx += 1;
            black_box(black_box(&g).can_place(x, y, 1, 1));
        },
        1000,
        200,
    );

    idx = 0;
    let slow = median_per_call(
        || {
            let (x, y) = probes[idx % probes.len()];
            idx += 1;
            black_box(black_box(&g).can_place_slow(x, y, 1, 1, &[]));
        },
        1000,
        200,
    );

    println!("can_place      中位数: {fast:.1} ns/次");
    println!("can_place_slow 中位数: {slow:.1} ns/次（参考路径，仅报告）");

    // 空网格参照：每次探测都要扫完整矩形，用来确认计时确实在测量真实工作。
    let empty = GridContainer::new(10, 10).unwrap();
    let mut idx2 = 0usize;
    let empty_ns = median_per_call(
        || {
            let (x, y) = probes[idx2 % probes.len()];
            idx2 += 1;
            black_box(black_box(&empty).can_place(x, y, 1, 1));
        },
        1000,
        200,
    );
    println!("空网格 can_place 中位数: {empty_ns:.1} ns/次（全扫描参照）");

    #[cfg(not(debug_assertions))]
    {
        assert!(
            fast <= 500.0,
            "can_place 中位数 {fast:.1} ns 超过 500 ns 门禁"
        );
    }
    #[cfg(debug_assertions)]
    {
        println!("skipped in debug（release 下才校验 500ns 门禁）");
    }
}
