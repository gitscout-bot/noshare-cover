//! noshare-cover: Rust core of a Hyprland plugin.
//!
//! Hyprland offers plugins no C API, only its own C++ ABI, which is built for a
//! specific compositor version. So all the logic lives here (media, decoding,
//! playback clock, config, resource lifecycle, extra rects), and the thin C++
//! shim in `shim/` only registers the hook, reads the config and window
//! rules, turns decoded frames into Hyprland textures and draws them.
//!
//! The boundary between them is the C ABI from [`ffi`], declared in `include/noshare_cover.h`.

pub mod clock;
pub mod config;
pub mod extra;
pub mod ffi;
pub mod frame;
pub mod gpu;
pub mod media;
pub mod notify;
pub mod registry;
pub mod worker;

pub use config::{Backend, PlayParams, Settings};
pub use frame::{Frame, FrameData};
pub use registry::Registry;
