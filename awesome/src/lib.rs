// awesome — the single Rust plugin. The `ffi` module is the only place
// unsafe lives (the cabi C ABI boundary); `probe` is the safe Phase 0
// behavior (event logging, a counter, a deferred job, a bus pipe, one config
// keyword). See docs/awesome-rust-plan.md.
#![allow(clippy::missing_safety_doc)]

mod ffi;
mod probe;

// Re-export the two loader entry points at the crate root so they are
// unambiguously part of the public (dylib) surface.
pub use ffi::hyprPluginExitC;
pub use ffi::hyprPluginInitC;
