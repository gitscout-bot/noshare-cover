//! H.264 on CPU: system libopenh264 via dlopen.
//!
//! The library is not bundled (H.264 patents: Cisco pays for its own builds, and
//! distros ship it as a separate package: `openh264` in Arch, `openh264` in
//! nixpkgs). If it's missing, we return a clear error and the plugin keeps running.
//!
//! Timestamps go through openh264 itself (uiInBsTimeStamp → uiOutYuvTimeStamp):
//! with B-frames it outputs pictures in display order, and pts travels with the frame.

use std::ffi::{c_char, c_int, c_long, c_uchar, c_uint, c_ulonglong, c_void};
use std::ptr::null_mut;
use std::time::Duration;

use libloading::Library;

use crate::frame::FrameData;
use crate::media::video::pipeline::{DecodedFrame, Decoder, Packet, PipeResult};
use crate::media::video::yuv::{self, Chroma, ChromaPlanes, Matrix, Plane, YuvImage};

/// Where to look for the library: by soname first (ld.so finds it in /usr/lib,
/// /run/opengl-driver, the NixOS LD_LIBRARY_PATH), then by full paths.
const CANDIDATES: &[&str] = &[
    "libopenh264.so.8",
    "libopenh264.so.7",
    "libopenh264.so",
    "/usr/lib/libopenh264.so",
    "/usr/lib64/libopenh264.so",
    "/usr/lib/x86_64-linux-gnu/libopenh264.so",
];

/// Environment variable for an explicit path (e.g. a downloaded Cisco build).
const ENV_PATH: &str = "NOSHARE_COVER_OPENH264";

// The part of codec_api.h / codec_app_def.h the decoder needs. openh264 keeps this ABI
// across 2.x (so.6–so.8); the layouts are pinned below against the C headers.

#[repr(C)]
struct SVideoProperty {
    size: c_uint,
    bitstream_type: c_int, // VIDEO_BITSTREAM_TYPE
}

#[repr(C)]
struct SDecodingParam {
    file_name_restructed: *mut c_char,
    cpu_load: c_uint,
    target_dq_layer: c_uchar,
    error_concealment: c_int, // ERROR_CON_IDC
    parse_only: bool,
    video_property: SVideoProperty,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct SSysMemBuffer {
    width: c_int,
    height: c_int,
    format: c_int,
    stride: [c_int; 2],
}

#[repr(C)]
struct SBufferInfo {
    /// 1 when a picture is ready
    buffer_status: c_int,
    in_timestamp: c_ulonglong,
    out_timestamp: c_ulonglong,
    /// a one-member union in C
    system_buffer: SSysMemBuffer,
    dst: [*mut c_uchar; 3],
}

impl SBufferInfo {
    fn new(in_timestamp: u64) -> Self {
        Self {
            buffer_status: 0,
            in_timestamp,
            out_timestamp: 0,
            system_buffer: SSysMemBuffer::default(),
            dst: [null_mut(); 3],
        }
    }
}

/// ISVCDecoder* in C: a pointer to the object, whose first field is the vtable pointer.
type Handle = *mut *const Vtable;

#[repr(C)]
struct Vtable {
    initialize: Option<unsafe extern "C" fn(Handle, *const SDecodingParam) -> c_long>,
    uninitialize: *const c_void,
    decode_frame: *const c_void,
    decode_frame_no_delay: Option<
        unsafe extern "C" fn(
            Handle,
            *const c_uchar,
            c_int,
            *mut *mut c_uchar,
            *mut SBufferInfo,
        ) -> c_int,
    >,
    decode_frame2: *const c_void,
    flush_frame: Option<unsafe extern "C" fn(Handle, *mut *mut c_uchar, *mut SBufferInfo) -> c_int>,
    decode_parser: *const c_void,
    decode_frame_ex: *const c_void,
    set_option: Option<unsafe extern "C" fn(Handle, c_int, *mut c_void) -> c_long>,
    get_option: Option<unsafe extern "C" fn(Handle, c_int, *mut c_void) -> c_long>,
}

const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<SDecodingParam>() == 32 && offset_of!(SDecodingParam, video_property) == 24);
    assert!(size_of::<SSysMemBuffer>() == 20);
    assert!(
        size_of::<SBufferInfo>() == 72
            && offset_of!(SBufferInfo, system_buffer) == 24
            && offset_of!(SBufferInfo, dst) == 48
    );
    assert!(
        size_of::<Vtable>() == 80
            && offset_of!(Vtable, flush_frame) == 40
            && offset_of!(Vtable, get_option) == 72
    );
};

const DECODER_OPTION_ERROR_CON_IDC: c_int = 8;
const DECODER_OPTION_TRACE_LEVEL: c_int = 9;
const DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER: c_int = 18;
const DECODER_OPTION_NUM_OF_THREADS: c_int = 19;
const WELS_LOG_QUIET: c_int = 0;
const ERROR_CON_DISABLE: c_int = 0;
const VIDEO_BITSTREAM_AVC: c_int = 0;

type FnCreate = unsafe extern "C" fn(*mut Handle) -> c_long;
type FnDestroy = unsafe extern "C" fn(Handle);

pub struct H264Decoder {
    dec: Handle,
    decode_no_delay: unsafe extern "C" fn(
        Handle,
        *const c_uchar,
        c_int,
        *mut *mut c_uchar,
        *mut SBufferInfo,
    ) -> c_int,
    flush_frame: unsafe extern "C" fn(Handle, *mut *mut c_uchar, *mut SBufferInfo) -> c_int,
    get_option: unsafe extern "C" fn(Handle, c_int, *mut c_void) -> c_long,
    destroy: FnDestroy,
    // keep the library loaded while pointers into it are alive
    _lib: Library,
    /// Packets in a row that openh264 refused without producing a picture.
    refused: u32,
    last_state: i32,
}

// The decoder is only used from the decoder thread.
unsafe impl Send for H264Decoder {}

/// After this many refused packets in a row, report the stream as undecodable
/// instead of silently showing nothing.
const MAX_REFUSED: u32 = 90;
/// DECODING_STATE bits worth naming in the error.
const DS_NO_PARAM_SETS: i32 = 0x10;
const DS_BITSTREAM_ERROR: i32 = 0x04;

fn open_lib() -> Result<Library, String> {
    let mut tried = Vec::new();
    let explicit = std::env::var(ENV_PATH).ok();
    // NSC_LIB_OPENH264: absolute path baked in at build time (Nix: library from the store;
    // on NixOS dlopen by soname won't find it)
    for name in explicit
        .iter()
        .map(String::as_str)
        .chain(option_env!("NSC_LIB_OPENH264"))
        .chain(CANDIDATES.iter().copied())
    {
        // SAFETY: openh264 has no constructors with side effects.
        match unsafe { Library::new(name) } {
            Ok(l) => return Ok(l),
            Err(e) => tried.push(format!("{name}: {e}")),
        }
    }
    Err(format!(
        "libopenh264 not found (install the openh264 package or set {ENV_PATH}); tried: {}",
        tried.join("; ")
    ))
}

impl H264Decoder {
    pub fn new() -> PipeResult<Self> {
        let lib = open_lib()?;
        // SAFETY: signatures and layouts are from codec_api.h (pinned above); the decoder
        // is destroyed exactly once, here on failure or in Drop.
        unsafe {
            let sym = |n: &[u8]| -> Result<*const c_void, String> {
                lib.get::<*const c_void>(n).map(|s| *s).map_err(|e| {
                    format!(
                        "libopenh264: missing {}: {e}",
                        String::from_utf8_lossy(&n[..n.len() - 1])
                    )
                })
            };
            let create: FnCreate = std::mem::transmute(sym(b"WelsCreateDecoder\0")?);
            let destroy: FnDestroy = std::mem::transmute(sym(b"WelsDestroyDecoder\0")?);
            let mut dec: Handle = null_mut();
            if create(&raw mut dec) != 0 || dec.is_null() || (*dec).is_null() {
                return Err("openh264: WelsCreateDecoder failed".into());
            }
            let vt = &**dec;
            let fail = |what: &str| -> String {
                destroy(dec);
                format!("openh264: {what}")
            };
            let (
                Some(initialize),
                Some(set_option),
                Some(decode_no_delay),
                Some(flush_frame),
                Some(get_option),
            ) = (
                vt.initialize,
                vt.set_option,
                vt.decode_frame_no_delay,
                vt.flush_frame,
                vt.get_option,
            )
            else {
                return Err(fail("incomplete decoder vtable"));
            };
            let param = SDecodingParam {
                file_name_restructed: null_mut(),
                cpu_load: 0,
                target_dq_layer: 0,
                error_concealment: ERROR_CON_DISABLE,
                parse_only: false,
                video_property: SVideoProperty {
                    size: 0,
                    bitstream_type: VIDEO_BITSTREAM_AVC,
                },
            };
            if initialize(dec, &raw const param) != 0 {
                return Err(fail("Initialize failed"));
            }
            // quiet log, single thread, no error concealment (a broken frame yields no picture)
            for (option, mut value) in [
                (DECODER_OPTION_TRACE_LEVEL, WELS_LOG_QUIET),
                (DECODER_OPTION_NUM_OF_THREADS, 0),
                (DECODER_OPTION_ERROR_CON_IDC, ERROR_CON_DISABLE),
            ] {
                if set_option(dec, option, (&raw mut value).cast()) != 0 {
                    return Err(fail(&format!("SetOption {option} failed")));
                }
            }
            Ok(Self {
                dec,
                decode_no_delay,
                flush_frame,
                get_option,
                destroy,
                _lib: lib,
                refused: 0,
                last_state: 0,
            })
        }
    }

    fn remaining(&mut self) -> usize {
        let mut n: c_int = 0;
        // SAFETY: GetOption writes one integer through the pointer.
        let r = unsafe {
            (self.get_option)(
                self.dec,
                DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER,
                (&raw mut n).cast(),
            )
        };
        if r == 0 {
            usize::try_from(n).unwrap_or(0)
        } else {
            0
        }
    }
}

impl Drop for H264Decoder {
    fn drop(&mut self) {
        // SAFETY: created in new, destroyed exactly once (WelsDestroyDecoder uninitializes it).
        unsafe { (self.destroy)(self.dec) };
    }
}

/// Frame from the openh264 buffer. The planes live until the next decoder call,
/// so convert to BGRA right away.
fn convert(dst: &[*mut u8; 3], info: &SBufferInfo) -> Option<DecodedFrame> {
    if info.buffer_status != 1 || dst.iter().any(|p| p.is_null()) {
        return None;
    }
    let sb = info.system_buffer;
    let (w, h) = (
        usize::try_from(sb.width).ok()?,
        usize::try_from(sb.height).ok()?,
    );
    let (ys, cs) = (
        usize::try_from(sb.stride[0]).ok()?,
        usize::try_from(sb.stride[1]).ok()?,
    );
    if w == 0 || h == 0 {
        return None;
    }
    let (cw, ch) = (w.div_ceil(2), h.div_ceil(2));
    // SAFETY: openh264 guarantees stride*rows bytes per plane (I420).
    let plane = |p: *mut u8, stride: usize, width: usize, rows: usize| Plane {
        data: unsafe { std::slice::from_raw_parts(p, stride * (rows - 1) + width) },
        stride,
    };
    // openh264 outputs only 8-bit 4:2:0 and doesn't expose the VUI matrix.
    let yuv = YuvImage {
        width: u32::try_from(w).ok()?,
        height: u32::try_from(h).ok()?,
        bit_depth: 8,
        chroma: Chroma::Sub420,
        matrix: Matrix::guess(u32::try_from(h).ok()?),
        full_range: false,
        y: plane(dst[0], ys, w, h),
        uv: ChromaPlanes::Planar {
            u: plane(dst[1], cs, cw, ch),
            v: plane(dst[2], cs, cw, ch),
        },
    };
    Some(DecodedFrame {
        pts: Duration::from_micros(info.out_timestamp),
        data: FrameData::Cpu(yuv::to_bgra(&yuv)?),
    })
}

fn refusal_text(state: i32) -> String {
    let why = if state & DS_NO_PARAM_SETS != 0 {
        "it rejected the SPS/PPS (unsupported profile, e.g. High 10 / 4:2:2)"
    } else if state & DS_BITSTREAM_ERROR != 0 {
        "bitstream error"
    } else {
        "no pictures"
    };
    format!("openh264 can't decode this video: {why}, state {state:#x}; try backend = \"gpu\"")
}

/// Highest level openh264 accepts (5.2). An SPS above it is rejected outright, the
/// decoder then has no parameter sets and returns no frames at all.
const OPENH264_MAX_LEVEL: u8 = 52;

/// Copy of an Annex-B packet with every SPS `level_idc` above 5.2 lowered to 5.2, or
/// `None` if nothing needs changing. Encoders sometimes write level 6.x for small videos;
/// the level only bounds buffer sizes, so clamping is harmless for streams that really
/// fit into 5.2 (anything up to 4K30).
fn clamp_sps_level(data: &[u8]) -> Option<Vec<u8>> {
    let mut out: Option<Vec<u8>> = None;
    let mut i = 0;
    while i + 3 < data.len() {
        // start code 00 00 01 (also the tail of 00 00 00 01)
        if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 {
            let nal = i + 3;
            // SPS: header (type 7), profile_idc, constraint flags, level_idc
            if nal + 3 < data.len() && data[nal] & 0x1f == 7 && data[nal + 3] > OPENH264_MAX_LEVEL {
                out.get_or_insert_with(|| data.to_vec())[nal + 3] = OPENH264_MAX_LEVEL;
            }
            i = nal;
        } else {
            i += 1;
        }
    }
    out
}

impl Decoder for H264Decoder {
    fn name(&self) -> &'static str {
        "openh264 (CPU)"
    }

    fn decode(&mut self, packet: &Packet) -> PipeResult<Vec<DecodedFrame>> {
        // Only keyframes carry SPS; copy just those when their level needs clamping.
        let patched = if packet.keyframe {
            clamp_sps_level(&packet.data)
        } else {
            None
        };
        let data: &[u8] = patched.as_deref().unwrap_or(&packet.data);
        let Ok(len) = i32::try_from(data.len()) else {
            return Ok(Vec::new());
        };
        let mut dst = [null_mut::<u8>(); 3];
        let mut info = SBufferInfo::new(u64::try_from(packet.pts.as_micros()).unwrap_or(u64::MAX));
        // SAFETY: the packet outlives the call; dst/info are our output buffers.
        // The return code is ignored: a broken frame just yields no picture, and
        // the next keyframe fixes everything.
        unsafe {
            self.last_state = (self.decode_no_delay)(
                self.dec,
                data.as_ptr(),
                len,
                dst.as_mut_ptr(),
                &raw mut info,
            );
        }
        let frames: Vec<DecodedFrame> = convert(&dst, &info).into_iter().collect();
        if frames.is_empty() && self.last_state != 0 {
            self.refused += 1;
            if self.refused >= MAX_REFUSED {
                return Err(refusal_text(self.last_state));
            }
        } else {
            self.refused = 0;
        }
        Ok(frames)
    }

    fn flush(&mut self) -> PipeResult<Vec<DecodedFrame>> {
        let mut out = Vec::new();
        for _ in 0..self.remaining() {
            let mut dst = [null_mut::<u8>(); 3];
            let mut info = SBufferInfo::new(0);
            // SAFETY: same as in decode.
            unsafe { (self.flush_frame)(self.dec, dst.as_mut_ptr(), &raw mut info) };
            out.extend(convert(&dst, &info));
        }
        Ok(out)
    }

    fn reset(&mut self) {
        // After a loop the demuxer starts at a keyframe with SPS/PPS, and the decoder
        // reconfigures itself; the tail of the previous pass is simply dropped.
        let _ = self.flush();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sps_level_is_clamped_only_when_needed() {
        // SPS High, level 6.2 (0x3e), then an IDR slice
        let pkt = [
            0, 0, 0, 1, 0x67, 0x64, 0x00, 0x3e, 0xac, 0, 0, 1, 0x65, 0x88,
        ];
        let fixed = clamp_sps_level(&pkt).unwrap();
        assert_eq!(fixed[7], 52);
        assert_eq!(&fixed[..7], &pkt[..7]);
        assert_eq!(&fixed[8..], &pkt[8..]);
        // level 4.0 stays, nothing is copied
        let ok = [0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28, 0xac];
        assert!(clamp_sps_level(&ok).is_none());
        // truncated data doesn't panic
        assert!(clamp_sps_level(&[0, 0, 1, 0x67]).is_none());
    }
}
