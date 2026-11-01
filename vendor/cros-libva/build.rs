// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// noshare-cover: the bindings are generated once and kept in src/bindings_generated.rs
// (see vendor/README), so a build needs no bindgen, clang or libva headers, only the
// library to link against.

use std::env;
use std::path::Path;

const CROS_LIBVA_LIB_PATH_ENV: &str = "CROS_LIBVA_LIB_PATH";

fn main() {
    // Do not require dependencies when generating docs.
    if env::var("CARGO_DOC").is_ok() || env::var("DOCS_RS").is_ok() {
        return;
    }

    // The bindings come from VA-API 1.24 headers, newer than every version check here.
    for cfg in [
        "libva_1_16_or_higher",
        "libva_1_19_or_higher",
        "libva_1_20_or_higher",
        "libva_1_21_or_higher",
    ] {
        println!("cargo::rustc-check-cfg=cfg({cfg})");
        println!("cargo::rustc-cfg={cfg}");
    }

    let va_lib_path = env::var(CROS_LIBVA_LIB_PATH_ENV).unwrap_or_default();
    if !va_lib_path.is_empty() {
        assert!(Path::new(&va_lib_path).exists(), "{va_lib_path} doesn't exist");
        println!("cargo:rustc-link-arg=-Wl,-rpath={va_lib_path}");
    } else if let Err(e) = pkg_config::probe_library("libva") {
        // the library search path (e.g. a Nix store path)
        panic!("libva is not found in system: {e}");
    }

    // Tell cargo to link va and va-drm objects dynamically.
    println!("cargo:rustc-link-lib=dylib=va");
    println!("cargo:rustc-link-lib=dylib=va-drm"); // for the vaGetDisplayDRM entrypoint
}
