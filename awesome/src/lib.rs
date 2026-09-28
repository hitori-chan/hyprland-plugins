// awesome — the single Rust plugin. The `ffi` module is the only place
// unsafe lives (the cabi C ABI boundary); `probe` is the safe Phase 0
// behavior (event logging, a counter, a deferred job, a bus pipe, one config
// keyword); `bar` is the Phase 1 mini-bar (render + textures). See
// docs/awesome-rust-plan.md.
#![allow(clippy::missing_safety_doc)]
// The mechanical pedantic families this crate trips on by construction:
// the render math is round-then-truncate px casts (the C++ did the same
// `int()` casts), geometry locals are (x, y)/(a, b) pairs, the state
// structs are flag bundles, the notify submodule group shares its module
// scope, and the zbus macro shapes its own (a)sync surface.
#![allow(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    clippy::cast_precision_loss,
    clippy::cast_lossless,
    clippy::cast_possible_wrap,
    clippy::many_single_char_names,
    clippy::doc_markdown,
    clippy::wildcard_imports,
    clippy::struct_excessive_bools,
    clippy::if_not_else,
    clippy::unused_async,
    clippy::unused_async_trait_impl
)]
// Texture handles are raw refs valid on the event-loop thread only; the
// Arc is a refcount, not a thread handoff (the one-thread rule).
#![allow(clippy::arc_with_non_send_sync)]

mod bar;
mod bus;
mod click;
mod ffi;
mod max;
mod nicons;
mod ninput;
mod notify;
mod nparse;
mod nrender;
mod osd;
mod pad;
mod place;
mod probe;
mod snap;

// Re-export the two loader entry points at the crate root so they are
// unambiguously part of the public (dylib) surface.
pub use ffi::hyprPluginExitC;
pub use ffi::hyprPluginInitC;
