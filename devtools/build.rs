// The virtual-keyboard protocol was dropped from upstream wlr-protocols; the
// fork still ships (and implements) it, so the fork's XML is the source of
// truth — same as the C fixture's KXML.
fn main() {
    let home = std::env::var("HOME").expect("HOME");
    let hl = std::env::var("HYPR_LAND_DIR").unwrap_or(format!("{home}/repo/Hyprland"));
    let src = format!("{hl}/protocols/virtual-keyboard-unstable-v1.xml");
    let dst = "protocols/vkbd.xml";
    std::fs::create_dir_all("protocols").expect("protocols dir");
    let bytes = std::fs::read(&src).unwrap_or_else(|err| panic!("read {src}: {err}"));
    if std::fs::read(dst).ok().as_deref() != Some(bytes.as_slice()) {
        std::fs::write(dst, &bytes).expect("write protocols/vkbd.xml");
    }
    println!("cargo:rerun-if-changed={src}");
    println!("cargo:rerun-if-changed=build.rs");
}
