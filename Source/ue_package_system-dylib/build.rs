//! cargo build 时自动跑 cbindgen，把 #[no_mangle] extern "C" 函数导出成 C++ 头文件。

use std::path::PathBuf;
use std::{env, fs};

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR 一定存在");
    let include_dir = PathBuf::from(&crate_dir).join("include");

    // cbindgen 的 write_to_file 不会自动建父目录，得先自己建，
    // 否则第一次 clone 下来的人 build 会直接 panic。
    fs::create_dir_all(&include_dir).expect("创建 include/ 失败");

    // C++ 风格的 extern "C" 声明；注释不进头文件，头文件要干净。
    let config = cbindgen::Config {
        language: cbindgen::Language::Cxx,
        include_guard: Some("UE_PACKAGE_SYSTEM_FFI_H".to_string()),
        cpp_compat: true,
        documentation: false,
        ..Default::default()
    };

    cbindgen::generate_with_config(&crate_dir, config)
        .expect("cbindgen 生成头文件失败")
        .write_to_file(include_dir.join("ue_package_system_ffi.h"));

    println!("cargo:rerun-if-changed=src/lib.rs");
}
