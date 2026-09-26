// Generate the FFI bindings from the fork's cabi C ABI header. The header is
// pure C99 (includes only <stdint.h>), so bindgen parses it standalone.
//
// The header path comes from HYPR_CABI_HEADER (the gate/stage points it at the
// staged header that matches the running binary); the default is the fork
// source. The runtime cabiAbiVersion() handshake is the real guard, so a
// slightly stale header degrades to a clean eject, never UB.
fn main() {
    let home = std::env::var("HOME").expect("HOME");
    let hl = std::env::var("HYPR_LAND_DIR").unwrap_or(format!("{home}/repo/Hyprland"));
    let header =
        std::env::var("HYPR_CABI_HEADER").unwrap_or(format!("{hl}/src/plugins/cabi/cabi.h"));

    let bindings = bindgen::Builder::default()
        .header(&header)
        .allowlist_type("hl_.*")
        .allowlist_function("hl_.*|cabiAbiVersion")
        .allowlist_var("HL_EV_.*|HL_CFG_.*")
        .ctypes_prefix("::libc")
        .generate()
        .unwrap_or_else(|e| panic!("bindgen({header}): {e}"));

    let out_dir = std::env::var("OUT_DIR").expect("OUT_DIR");
    bindings
        .write_to_file(format!("{out_dir}/cabi.rs"))
        .expect("write cabi.rs");

    println!("cargo:rerun-if-changed={header}");
    println!("cargo:rerun-if-changed=build.rs");
}
