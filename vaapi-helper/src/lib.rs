//! VA-API decoder for noshare-cover: a thin C wrapper over the cros-codecs
//! stateless decoders. The main plugin loads it via dlopen (from a memfd), so
//! the whole interface is a few `extern "C"` functions and one frame struct.
//!
//! Frames are decoded into NV12 GBM buffers, mapped into memory after decode
//! (gbm_bo_map handles detiling) and passed to the callback without copying:
//! the pointers are valid only for the duration of the callback.
//!
//! cros-codecs 0.0.6 limitation: the VA-API path is 8-bit only (NV12).

#![cfg_attr(not(target_os = "linux"), allow(unused))]

use std::ffi::{c_char, c_void};

/// ABI version between the helper and the plugin.
pub const ABI: u32 = 1;

#[repr(C)]
pub struct NscVaFrame {
    pub width: u32,
    pub height: u32,
    pub y: *const u8,
    pub y_stride: usize,
    pub uv: *const u8,
    pub uv_stride: usize,
    pub pts_ns: u64,
}

pub type FrameCb = unsafe extern "C" fn(user: *mut c_void, frame: *const NscVaFrame);

// Codec IDs; must match decode/vaapi.rs in the main plugin.
pub const CODEC_H264: u32 = 1;
pub const CODEC_HEVC: u32 = 2;
pub const CODEC_VP8: u32 = 3;
pub const CODEC_VP9: u32 = 4;
pub const CODEC_AV1: u32 = 5;

#[unsafe(no_mangle)]
pub extern "C" fn nsc_vaapi_abi() -> u32 {
    ABI
}

fn write_err(buf: *mut c_char, len: usize, msg: &str) {
    if buf.is_null() || len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(len - 1);
    // SAFETY: the caller provided a buffer of len bytes.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf.cast::<u8>(), n);
        *buf.add(n) = 0;
    }
}

/// A panic must not cross the C boundary.
fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or(fallback)
}

#[cfg(target_os = "linux")]
mod imp {
    use super::*;
    use std::ffi::CStr;
    use std::rc::Rc;
    use std::sync::Arc;

    use cros_codecs::decoder::stateless::av1::Av1;
    use cros_codecs::decoder::stateless::h264::H264;
    use cros_codecs::decoder::stateless::h265::H265;
    use cros_codecs::decoder::stateless::vp8::Vp8;
    use cros_codecs::decoder::stateless::vp9::Vp9;
    use cros_codecs::decoder::stateless::{
        DecodeError, DynStatelessVideoDecoder, StatelessDecoder, StatelessVideoDecoder,
    };
    use cros_codecs::decoder::{DecodedHandle, DecoderEvent};
    use cros_codecs::libva::Display;
    use cros_codecs::video_frame::VideoFrame;
    use cros_codecs::video_frame::frame_pool::{FramePool, PooledVideoFrame};
    use cros_codecs::video_frame::gbm_video_frame::{GbmDevice, GbmUsage, GbmVideoFrame};
    use cros_codecs::{BlockingMode, Fourcc};

    type Frame = PooledVideoFrame<GbmVideoFrame>;

    pub struct Dec {
        // field order = drop order: the decoder releases frames and context before the pool and display
        dec: DynStatelessVideoDecoder<Frame>,
        pool: FramePool<GbmVideoFrame>,
        _display: Rc<Display>,
        pub err: String,
    }

    /// Extra frames to keep in the pool above the minimum, so the decoder doesn't
    /// hit NotEnoughOutputBuffers while we copy a frame out.
    const POOL_HEADROOM: usize = 2;

    pub fn open(node: &str, codec: u32) -> Result<Dec, String> {
        let display =
            Display::open_drm_display(node).map_err(|e| format!("VA-API on {node}: {e}"))?;
        let gbm = GbmDevice::open(node).map_err(|e| format!("GBM on {node}: {e}"))?;
        let pool = FramePool::new(move |si| {
            Arc::clone(&gbm)
                .new_frame(
                    Fourcc::from(b"NV12"),
                    si.display_resolution,
                    si.coded_resolution,
                    GbmUsage::Decode,
                )
                .expect("GBM: failed to allocate an NV12 frame")
        });
        let bm = BlockingMode::Blocking;
        let d = Rc::clone(&display);
        let dec: DynStatelessVideoDecoder<Frame> = match codec {
            CODEC_H264 => {
                StatelessDecoder::<H264, _>::new_vaapi(d, bm).map(|x| x.into_trait_object())
            }
            CODEC_HEVC => {
                StatelessDecoder::<H265, _>::new_vaapi(d, bm).map(|x| x.into_trait_object())
            }
            CODEC_VP8 => {
                StatelessDecoder::<Vp8, _>::new_vaapi(d, bm).map(|x| x.into_trait_object())
            }
            CODEC_VP9 => {
                StatelessDecoder::<Vp9, _>::new_vaapi(d, bm).map(|x| x.into_trait_object())
            }
            CODEC_AV1 => {
                StatelessDecoder::<Av1, _>::new_vaapi(d, bm).map(|x| x.into_trait_object())
            }
            other => return Err(format!("unknown codec {other}")),
        }
        .map_err(|e| format!("VA-API can't decode this codec on {node}: {e}"))?;
        Ok(Dec {
            dec,
            pool,
            _display: display,
            err: String::new(),
        })
    }

    impl Dec {
        /// Drain decoder events. Returns `true` if anything made progress.
        fn drain(&mut self, cb: FrameCb, user: *mut c_void) -> Result<bool, String> {
            let mut progress = false;
            while let Some(ev) = self.dec.next_event() {
                progress = true;
                match ev {
                    DecoderEvent::FormatChanged => {
                        let mut si = self
                            .dec
                            .stream_info()
                            .ok_or("format change without stream_info")?
                            .clone();
                        si.min_num_frames += POOL_HEADROOM;
                        self.pool.resize(&si);
                    }
                    DecoderEvent::FrameReady(h) => {
                        h.sync().map_err(|e| format!("frame sync: {e}"))?;
                        let pts_ns = h.timestamp();
                        let vf = h.video_frame();
                        let res = vf.resolution();
                        let pitch = vf.get_plane_pitch();
                        let map = vf.map().map_err(|e| format!("frame mapping: {e}"))?;
                        let planes = map.get();
                        if planes.len() < 2 || pitch.len() < 2 {
                            return Err("frame lacks the two NV12 planes".into());
                        }
                        let f = NscVaFrame {
                            width: res.width,
                            height: res.height,
                            y: planes[0].as_ptr(),
                            y_stride: pitch[0],
                            uv: planes[1].as_ptr(),
                            uv_stride: pitch[1],
                            pts_ns,
                        };
                        // SAFETY: the plugin callback copies the data within the call.
                        unsafe { cb(user, &f) };
                    }
                }
            }
            Ok(progress)
        }

        pub fn decode(
            &mut self,
            data: &[u8],
            pts_ns: u64,
            cb: FrameCb,
            user: *mut c_void,
        ) -> Result<(), String> {
            let mut off = 0;
            let mut stalls = 0;
            while off < data.len() {
                let pool = &mut self.pool;
                match self.dec.decode(pts_ns, &data[off..], &mut || pool.alloc()) {
                    Ok(0) => break, // decoder consumed nothing; don't spin forever
                    Ok(n) => {
                        off += n;
                        stalls = 0;
                    }
                    Err(DecodeError::CheckEvents | DecodeError::NotEnoughOutputBuffers(_)) => {
                        if !self.drain(cb, user)? {
                            stalls += 1;
                            if stalls > 3 {
                                return Err("VA-API: decoder is out of output frames".into());
                            }
                        }
                    }
                    Err(e) => return Err(format!("VA-API: {e}")),
                }
            }
            self.drain(cb, user).map(|_| ())
        }

        pub fn flush(&mut self, cb: FrameCb, user: *mut c_void) -> Result<(), String> {
            self.dec.flush().map_err(|e| format!("VA-API flush: {e}"))?;
            self.drain(cb, user).map(|_| ())
        }

        pub fn reset(&mut self) {
            // after flush the decoder waits for a keyframe, which is exactly what the next loop starts with
            let _ = self.dec.flush();
            while self.dec.next_event().is_some() {}
        }
    }

    pub fn cstr(p: *const c_char) -> Option<String> {
        if p.is_null() {
            return None;
        }
        // SAFETY: the caller passes a C string.
        Some(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
    }

    pub fn with_dec(h: *mut c_void, f: impl FnOnce(&mut Dec) -> Result<(), String>) -> i32 {
        guard(-2, || {
            // SAFETY: h comes from nsc_vaapi_open and hasn't been closed yet.
            let Some(d) = (unsafe { h.cast::<Dec>().as_mut() }) else {
                return -1;
            };
            match f(d) {
                Ok(()) => 0,
                Err(e) => {
                    d.err = e;
                    -1
                }
            }
        })
    }
}

/// Open a decoder. Returns NULL on error, with the message in `err`.
///
/// # Safety
/// `node` is a C string; `err` is a buffer of `err_len` bytes or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_open(
    node: *const c_char,
    codec: u32,
    err: *mut c_char,
    err_len: usize,
) -> *mut c_void {
    #[cfg(target_os = "linux")]
    {
        guard(std::ptr::null_mut(), || {
            let Some(node) = imp::cstr(node) else {
                write_err(err, err_len, "no render node");
                return std::ptr::null_mut();
            };
            match imp::open(&node, codec) {
                Ok(d) => Box::into_raw(Box::new(d)).cast(),
                Err(e) => {
                    write_err(err, err_len, &e);
                    std::ptr::null_mut()
                }
            }
        })
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (node, codec);
        write_err(err, err_len, "VA-API is Linux-only");
        std::ptr::null_mut()
    }
}

/// # Safety
/// `h` comes from `nsc_vaapi_open`; `data` is `len` bytes; `cb` is called synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_decode(
    h: *mut c_void,
    data: *const u8,
    len: usize,
    pts_ns: u64,
    cb: FrameCb,
    user: *mut c_void,
) -> i32 {
    #[cfg(target_os = "linux")]
    {
        if data.is_null() {
            return 0;
        }
        // SAFETY: see the function contract.
        let bytes = unsafe { std::slice::from_raw_parts(data, len) };
        imp::with_dec(h, |d| d.decode(bytes, pts_ns, cb, user))
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (h, data, len, pts_ns, cb, user);
        -1
    }
}

/// # Safety
/// Same as `nsc_vaapi_decode`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_flush(h: *mut c_void, cb: FrameCb, user: *mut c_void) -> i32 {
    #[cfg(target_os = "linux")]
    {
        imp::with_dec(h, |d| d.flush(cb, user))
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (h, cb, user);
        -1
    }
}

/// # Safety
/// `h` comes from `nsc_vaapi_open`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_reset(h: *mut c_void) {
    #[cfg(target_os = "linux")]
    imp::with_dec(h, |d| {
        d.reset();
        Ok(())
    });
    #[cfg(not(target_os = "linux"))]
    let _ = h;
}

/// Text of the last error. Returns its length without the NUL terminator.
///
/// # Safety
/// `h` comes from `nsc_vaapi_open`; `buf` is `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_error(h: *mut c_void, buf: *mut c_char, len: usize) -> usize {
    guard(0, || {
        // SAFETY: see the contract.
        #[cfg(target_os = "linux")]
        if let Some(d) = unsafe { h.cast::<imp::Dec>().as_ref() } {
            write_err(buf, len, &d.err);
            return d.err.len();
        }
        let _ = (h, buf, len);
        0
    })
}

/// # Safety
/// `h` comes from `nsc_vaapi_open` and is closed exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_close(h: *mut c_void) {
    guard((), || {
        #[cfg(target_os = "linux")]
        if !h.is_null() {
            // SAFETY: the Box from nsc_vaapi_open.
            drop(unsafe { Box::from_raw(h.cast::<imp::Dec>()) });
        }
        let _ = h;
    });
}
