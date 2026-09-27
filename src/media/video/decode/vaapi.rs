//! VA-API (Intel, AMD; NVIDIA via nvidia-vaapi-driver).
//!
//! The actual decoding lives in a separate library, vaapi-helper (cros-codecs on
//! top of libva). It is embedded in the plugin as bytes and loaded from a memfd
//! via dlopen on the first GPU video. That keeps libva/libgbm dependencies of the
//! helper only: without libva, VA-API is unavailable, but the plugin still loads
//! and keeps working on the CPU. The helper is shared by all decoders and is
//! unloaded together with the last one.

use std::ffi::{CString, c_char, c_int, c_void};
use std::path::Path;
use std::sync::{Arc, Mutex, Weak};
use std::time::Duration;

use libloading::Library;

use crate::frame::FrameData;
use crate::media::video::pipeline::{Codec, DecodedFrame, Decoder, Packet, PipeResult};
use crate::media::video::yuv::{self, Chroma, ChromaPlanes, Matrix, Plane, YuvImage};

#[cfg(nsc_vaapi_embedded)]
const HELPER_ABI: u32 = 1;

#[cfg(nsc_vaapi_embedded)]
static HELPER_SO: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/vaapi-helper.so"));

#[repr(C)]
struct NscVaFrame {
    width: u32,
    height: u32,
    y: *const u8,
    y_stride: usize,
    uv: *const u8,
    uv_stride: usize,
    pts_ns: u64,
}

type FrameCb = unsafe extern "C" fn(*mut c_void, *const NscVaFrame);

struct Helper {
    open: unsafe extern "C" fn(*const c_char, u32, *mut c_char, usize) -> *mut c_void,
    decode: unsafe extern "C" fn(*mut c_void, *const u8, usize, u64, FrameCb, *mut c_void) -> c_int,
    flush: unsafe extern "C" fn(*mut c_void, FrameCb, *mut c_void) -> c_int,
    reset: unsafe extern "C" fn(*mut c_void),
    error: unsafe extern "C" fn(*mut c_void, *mut c_char, usize) -> usize,
    close: unsafe extern "C" fn(*mut c_void),
    _lib: Library,
}

/// One shared helper, alive while at least one decoder is.
static HELPER: Mutex<Weak<Helper>> = Mutex::new(Weak::new());

fn helper() -> Result<Arc<Helper>, String> {
    let mut slot = HELPER.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(h) = slot.upgrade() {
        return Ok(h);
    }
    let h = Arc::new(load()?);
    *slot = Arc::downgrade(&h);
    Ok(h)
}

#[cfg(not(nsc_vaapi_embedded))]
fn load() -> Result<Helper, String> {
    Err("plugin built without VA-API (make without vaapi-helper)".into())
}

#[cfg(nsc_vaapi_embedded)]
fn load() -> Result<Helper, String> {
    use std::io::Write;
    use std::os::fd::FromRawFd;

    // SAFETY: memfd_create with a valid C string; the fd is immediately owned by File.
    let fd = unsafe { libc::memfd_create(c"noshare-cover-vaapi".as_ptr(), libc::MFD_CLOEXEC) };
    if fd < 0 {
        return Err(format!("memfd_create: {}", std::io::Error::last_os_error()));
    }
    let mut file = unsafe { std::fs::File::from_raw_fd(fd) };
    file.write_all(HELPER_SO)
        .map_err(|e| format!("memfd: {e}"))?;
    let path = format!("/proc/self/fd/{fd}");
    // SAFETY: our own helper; no constructors with side effects.
    // After dlopen the mapping stays alive on its own, so the fd can be closed.
    let lib = unsafe { Library::new(&path) }.map_err(|e| {
        let e = e.to_string();
        if e.contains("libva") || e.contains("libgbm") {
            format!("libva/libgbm not found for VA-API (install libva): {e}")
        } else {
            format!("failed to load VA-API helper: {e}")
        }
    })?;
    drop(file);

    // SAFETY: signatures come from vaapi-helper/src/lib.rs; the ABI is checked below.
    unsafe {
        let abi: unsafe extern "C" fn() -> u32 =
            *lib.get(b"nsc_vaapi_abi\0").map_err(|e| e.to_string())?;
        if abi() != HELPER_ABI {
            return Err("VA-API helper version mismatch".into());
        }
        Ok(Helper {
            open: *lib.get(b"nsc_vaapi_open\0").map_err(|e| e.to_string())?,
            decode: *lib.get(b"nsc_vaapi_decode\0").map_err(|e| e.to_string())?,
            flush: *lib.get(b"nsc_vaapi_flush\0").map_err(|e| e.to_string())?,
            reset: *lib.get(b"nsc_vaapi_reset\0").map_err(|e| e.to_string())?,
            error: *lib.get(b"nsc_vaapi_error\0").map_err(|e| e.to_string())?,
            close: *lib.get(b"nsc_vaapi_close\0").map_err(|e| e.to_string())?,
            _lib: lib,
        })
    }
}

fn helper_codec(codec: &Codec) -> Option<u32> {
    Some(match codec {
        Codec::H264 => 1,
        Codec::Hevc => 2,
        Codec::Vp8 => 3,
        Codec::Vp9 => 4,
        Codec::Av1 => 5,
        Codec::Other(_) => return None,
    })
}

pub struct VaapiDecoder {
    h: Arc<Helper>,
    handle: *mut c_void,
}

// The VA context is used only from the decoder thread: opened, used and closed
// by a single owner; the pointer never escapes.
unsafe impl Send for VaapiDecoder {}

impl VaapiDecoder {
    pub fn new(codec: &Codec, node: &Path) -> PipeResult<Self> {
        let c = helper_codec(codec).ok_or_else(|| format!("VA-API doesn't support {codec:?}"))?;
        let h = helper()?;
        let node_c = CString::new(node.as_os_str().as_encoded_bytes())
            .map_err(|_| "render node path contains a NUL byte".to_string())?;
        let mut err = [0 as c_char; 512];
        // SAFETY: the C string and error buffer outlive the call.
        let handle = unsafe { (h.open)(node_c.as_ptr(), c, err.as_mut_ptr(), err.len()) };
        if handle.is_null() {
            // SAFETY: the helper writes a C string into err.
            let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            return Err(if msg.is_empty() {
                "VA-API: failed to open".into()
            } else {
                msg
            });
        }
        Ok(Self { h, handle })
    }

    fn last_error(&self) -> String {
        let mut buf = [0 as c_char; 512];
        // SAFETY: handle is alive; the buffer is 512 bytes.
        unsafe { (self.h.error)(self.handle, buf.as_mut_ptr(), buf.len()) };
        unsafe { std::ffi::CStr::from_ptr(buf.as_ptr()) }
            .to_string_lossy()
            .into_owned()
    }
}

/// Helper callback: NV12 from the mapped GBM buffer -> BGRA, copied immediately.
unsafe extern "C" fn on_frame(user: *mut c_void, f: *const NscVaFrame) {
    let _ = std::panic::catch_unwind(|| {
        // SAFETY: user is our Vec; f is a frame valid for the duration of the call.
        let (Some(out), Some(f)) = (
            unsafe { user.cast::<Vec<DecodedFrame>>().as_mut() },
            unsafe { f.as_ref() },
        ) else {
            return;
        };
        let (w, h) = (f.width as usize, f.height as usize);
        if w == 0 || h == 0 || f.y.is_null() || f.uv.is_null() {
            return;
        }
        // SAFETY: the helper guarantees stride*rows bytes per plane.
        let y = unsafe { std::slice::from_raw_parts(f.y, f.y_stride * (h - 1) + w) };
        let ch = h.div_ceil(2);
        let uv =
            unsafe { std::slice::from_raw_parts(f.uv, f.uv_stride * (ch - 1) + w.div_ceil(2) * 2) };
        let img = YuvImage {
            width: f.width,
            height: f.height,
            bit_depth: 8,
            chroma: Chroma::Sub420,
            matrix: Matrix::guess(f.height),
            full_range: false,
            y: Plane {
                data: y,
                stride: f.y_stride,
            },
            uv: ChromaPlanes::Interleaved(Plane {
                data: uv,
                stride: f.uv_stride,
            }),
        };
        if let Some(cpu) = yuv::to_bgra(&img) {
            out.push(DecodedFrame {
                pts: Duration::from_nanos(f.pts_ns),
                data: FrameData::Cpu(cpu),
            });
        }
    });
}

impl Decoder for VaapiDecoder {
    fn name(&self) -> &'static str {
        "VA-API (GPU)"
    }

    fn decode(&mut self, packet: &Packet) -> PipeResult<Vec<DecodedFrame>> {
        let mut out: Vec<DecodedFrame> = Vec::new();
        let pts = u64::try_from(packet.pts.as_nanos()).unwrap_or(u64::MAX);
        // SAFETY: the packet and out outlive the call; the callback is synchronous.
        let r = unsafe {
            (self.h.decode)(
                self.handle,
                packet.data.as_ptr(),
                packet.data.len(),
                pts,
                on_frame,
                (&raw mut out).cast(),
            )
        };
        if r != 0 {
            return Err(self.last_error());
        }
        Ok(out)
    }

    fn flush(&mut self) -> PipeResult<Vec<DecodedFrame>> {
        let mut out: Vec<DecodedFrame> = Vec::new();
        // SAFETY: same as in decode.
        let r = unsafe { (self.h.flush)(self.handle, on_frame, (&raw mut out).cast()) };
        if r != 0 {
            return Err(self.last_error());
        }
        Ok(out)
    }

    fn reset(&mut self) {
        // SAFETY: handle is alive.
        unsafe { (self.h.reset)(self.handle) };
    }
}

impl Drop for VaapiDecoder {
    fn drop(&mut self) {
        // SAFETY: closed exactly once; the helper is unloaded with the last Arc.
        unsafe { (self.h.close)(self.handle) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_match_helper() {
        assert_eq!(helper_codec(&Codec::H264), Some(1));
        assert_eq!(helper_codec(&Codec::Av1), Some(5));
        assert_eq!(helper_codec(&Codec::Other("x".into())), None);
    }

    #[test]
    fn missing_helper_or_device_is_an_error() {
        let e = VaapiDecoder::new(&Codec::H264, Path::new("/nonexistent/renderD999"))
            .err()
            .unwrap();
        eprintln!("VA-API without a device: {e}");
        assert!(!e.is_empty());
        // with the embedded helper the error comes from the helper itself (memfd + dlopen + ABI succeeded)
        #[cfg(nsc_vaapi_embedded)]
        assert!(e.contains("VA-API on") || e.contains("libva"), "{e}");
    }
}
