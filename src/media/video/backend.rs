//! Picks the demuxer and decoder for a file and settings.
//!
//! Order: `backend = gpu`: VA-API only, error if unavailable;
//! `cpu`: software only; `auto`: VA-API if a GPU is present and supports the codec,
//! otherwise software. Every rejection comes with a reason that is shown
//! to the user instead of being silently dropped.
//!
//! Demuxers live in demux.rs (mp4/mov, webm/mkv), decoders in decode/: VA-API
//! (vaapi-helper), NVDEC, and the CPU fallbacks rav1d (AV1), openh264 (H.264)
//! and libvpx (VP8/VP9). The clock, decode thread and lifecycle are in
//! pipeline.rs and mod.rs.

use std::path::Path;

use super::pipeline::{Codec, Decoder, Demuxer, Pipeline};
use crate::config::{Backend, Settings};
use crate::gpu;

pub fn open_pipeline(path: &Path, looped: bool, settings: &Settings) -> Result<Pipeline, String> {
    let demux = open_demuxer(path)?;
    let decoder = open_decoder(&demux.info().codec, settings)?;
    Ok(Pipeline::new(demux, decoder, looped))
}

fn open_demuxer(path: &Path) -> Result<Box<dyn Demuxer>, String> {
    super::demux::open(path)
}

fn open_decoder(codec: &Codec, settings: &Settings) -> Result<Box<dyn Decoder>, String> {
    let mut reasons = Vec::new();

    if matches!(settings.backend, Backend::Auto | Backend::Gpu) {
        match gpu::pick_render_node(settings.gpu_device.as_deref()) {
            Ok(node) => match open_hw(codec, &node) {
                Ok(d) => return Ok(d),
                Err(e) => reasons.push(format!("GPU {}: {e}", node.display())),
            },
            Err(e) => reasons.push(format!("GPU: {e}")),
        }
        if settings.backend == Backend::Gpu {
            return Err(reasons.join("; "));
        }
    }

    match open_sw(codec) {
        Ok(d) => Ok(d),
        Err(e) => {
            reasons.push(format!("CPU: {e}"));
            Err(reasons.join("; "))
        }
    }
}

/// GPU decoder for a render node. NVIDIA: NVDEC directly first (the driver
/// parses the stream itself), then VA-API (nvidia-vaapi-driver); Intel/AMD: VA-API.
fn open_hw(codec: &Codec, node: &Path) -> Result<Box<dyn Decoder>, String> {
    let mut reasons: Vec<String> = Vec::new();

    #[cfg(feature = "nvdec")]
    if super::decode::nvdec::node_is_nvidia(node) {
        match super::decode::nvdec::NvdecDecoder::new(codec, Some(node)) {
            Ok(d) => return Ok(Box::new(d)),
            Err(e) => reasons.push(format!("NVDEC: {e}")),
        }
    }

    #[cfg(all(feature = "vaapi", target_os = "linux"))]
    match super::decode::vaapi::VaapiDecoder::new(codec, node) {
        Ok(d) => return Ok(Box::new(d)),
        Err(e) => reasons.push(format!("VA-API: {e}")),
    }
    #[cfg(not(all(feature = "vaapi", target_os = "linux")))]
    reasons.push(format!(
        "VA-API is not available in this build ({codec:?}, {})",
        node.display()
    ));

    Err(reasons.join("; "))
}

fn open_sw(codec: &Codec) -> Result<Box<dyn Decoder>, String> {
    // YUV frames -> premultiplied BGRA in a single pass (yuv.rs).
    match codec {
        #[cfg(feature = "cpu-av1")]
        Codec::Av1 => Ok(Box::new(super::decode::av1::Av1Decoder::new()?)),
        #[cfg(feature = "cpu-h264")]
        Codec::H264 => Ok(Box::new(super::decode::h264::H264Decoder::new()?)),
        #[cfg(feature = "cpu-vpx")]
        Codec::Vp8 | Codec::Vp9 => Ok(Box::new(super::decode::vpx::VpxDecoder::new(codec)?)),
        _ => Err(format!(
            "software decoder for {codec:?} is not wired up yet"
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gpu_only_reports_why() {
        let s = Settings {
            backend: Backend::Gpu,
            gpu_device: Some("/nonexistent/renderD999".into()),
            ..Settings::default()
        };
        let err = open_decoder(&Codec::Av1, &s).err().unwrap();
        assert!(err.contains("GPU"), "{err}");
        assert!(!err.contains("CPU"), "gpu-only must not fall back: {err}");
    }

    #[test]
    fn auto_tries_both() {
        let s = Settings {
            backend: Backend::Auto,
            gpu_device: Some("/nonexistent/renderD999".into()),
            ..Settings::default()
        };
        let err = open_decoder(&Codec::Other("V_TEST".into()), &s)
            .err()
            .unwrap();
        assert!(err.contains("GPU") && err.contains("CPU"), "{err}");
    }
}

#[cfg(test)]
mod real {
    use super::*;
    use crate::frame::FrameData;
    use crate::media::video::pipeline::Next;

    fn run_cpu(file: &str, decoder: &str) {
        let Some(dir) = std::env::var_os("NSC_TEST_MEDIA").map(std::path::PathBuf::from) else {
            return;
        };
        let path = dir.join(file);
        if !path.exists() {
            return;
        }
        let s = Settings {
            backend: Backend::Cpu,
            ..Settings::default()
        };
        let mut p = match open_pipeline(&path, true, &s) {
            Ok(p) => p,
            Err(e) if e.contains("not found") => {
                eprintln!("{file}: {e} — skipping");
                return;
            }
            Err(e) => panic!("{file}: {e}"),
        };
        assert_eq!(p.decoder_name(), decoder);
        let mut last = None;
        for n in 0..400 {
            let Next::Frame(f) = p.next_frame().unwrap() else {
                panic!("loop must not end")
            };
            #[allow(irrefutable_let_patterns)]
            let FrameData::Cpu(c) = &f.data else {
                panic!("expected a CPU frame")
            };
            assert_eq!((c.width, c.height), (640, 360));
            // NSC_DUMP=dir: save frame 60 as PNG to check the colors by eye
            if let (60, Some(d)) = (n, std::env::var_os("NSC_DUMP")) {
                let rgba: Vec<u8> = c
                    .pixels
                    .chunks(4)
                    .flat_map(|p| [p[2], p[1], p[0], p[3]])
                    .collect();
                image::save_buffer(
                    std::path::Path::new(&d).join(format!("{file}.png")),
                    &rgba,
                    c.width,
                    c.height,
                    image::ExtendedColorType::Rgba8,
                )
                .unwrap();
            }
            if let Some(prev) = last {
                assert!(
                    f.pts > prev,
                    "{file}: pts keep increasing across the loop: {prev:?} -> {:?}",
                    f.pts
                );
            }
            last = Some(f.pts);
        }
        assert!(last.unwrap() > std::time::Duration::from_secs(10));
    }

    #[cfg(feature = "cpu-h264")]
    #[test]
    fn h264_mp4_decodes_on_cpu() {
        run_cpu("h264.mp4", "openh264 (CPU)");
    }

    #[cfg(feature = "cpu-vpx")]
    #[test]
    fn vp9_webm_decodes_on_cpu() {
        run_cpu("vp9.webm", "libvpx VP9 (CPU)");
    }

    #[cfg(feature = "cpu-av1")]
    #[test]
    fn av1_mp4_decodes_on_cpu() {
        run_cpu("av1.mp4", "rav1d (CPU)");
    }
}
