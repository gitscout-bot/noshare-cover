//! PNG and JPEG: decoded once, the frame never changes after that.

use std::path::Path;
use std::time::Instant;

use super::{Kind, MediaError, Source};
use crate::frame::{CpuFrame, Frame, FrameData};

pub struct StillSource {
    frame: Option<Frame>,
}

impl StillSource {
    pub fn open(path: &Path) -> Result<Self, MediaError> {
        let bytes = std::fs::read(path).map_err(|e| open_err(path, e))?;
        let (w, h, rgba) = decode(&bytes).map_err(|e| open_err(path, e))?;
        if w == 0 || h == 0 {
            return Err(open_err(path, "empty image"));
        }
        let frame = Frame {
            generation: 1,
            data: FrameData::Cpu(CpuFrame::from_rgba(w, h, &rgba)),
        };
        Ok(Self { frame: Some(frame) })
    }
}

/// PNG or JPEG, told apart by the signature (not the extension) -> (width, height, RGBA8).
fn decode(bytes: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        decode_png(bytes)
    } else if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        decode_jpeg(bytes)
    } else {
        Err("not a PNG or JPEG image".into())
    }
}

fn decode_png(bytes: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    // the png crate allows 64 MiB by default, too little for an 8K RGBA cover
    let mut dec = png::Decoder::new_with_limits(
        std::io::Cursor::new(bytes),
        png::Limits { bytes: 512 << 20 },
    );
    // palette and tRNS to RGB(A), 16-bit samples to 8-bit
    dec.set_transformations(png::Transformations::normalize_to_color8());
    let mut reader = dec.read_info().map_err(|e| e.to_string())?;
    let mut buf = vec![0; reader.output_buffer_size().ok_or("PNG too large")?];
    // an animated PNG shows its first frame
    let info = reader.next_frame(&mut buf).map_err(|e| e.to_string())?;
    buf.truncate(info.buffer_size());
    let rgba = match info.color_type {
        png::ColorType::Rgba => buf,
        png::ColorType::Rgb => buf
            .as_chunks::<3>()
            .0
            .iter()
            .flat_map(|&[r, g, b]| [r, g, b, 255])
            .collect(),
        png::ColorType::GrayscaleAlpha => buf
            .as_chunks::<2>()
            .0
            .iter()
            .flat_map(|&[l, a]| [l, l, l, a])
            .collect(),
        png::ColorType::Grayscale => buf.iter().flat_map(|&g| [g, g, g, 255]).collect(),
        png::ColorType::Indexed => return Err("indexed PNG was not expanded".into()),
    };
    Ok((info.width, info.height, rgba))
}

fn decode_jpeg(bytes: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    use zune_jpeg::zune_core::{
        bytestream::ZCursor, colorspace::ColorSpace, options::DecoderOptions,
    };
    // RGB, not RGBA: zune-jpeg converts CMYK/YCCK to RGBA wrongly (to RGB it's fine, and
    // that is what the image crate asked it for)
    let options = DecoderOptions::default()
        .jpeg_set_out_colorspace(ColorSpace::RGB)
        .set_strict_mode(false);
    let mut dec = zune_jpeg::JpegDecoder::new_with_options(ZCursor::new(bytes), options);
    let rgb = dec.decode().map_err(|e| e.to_string())?;
    let (w, h) = dec.dimensions().ok_or("JPEG without dimensions")?;
    let dim = |v: usize| u32::try_from(v).map_err(|_| "JPEG too large".to_string());
    let rgba = rgb
        .as_chunks::<3>()
        .0
        .iter()
        .flat_map(|&[r, g, b]| [r, g, b, 255])
        .collect();
    Ok((dim(w)?, dim(h)?, rgba))
}

/// Writes RGBA8 pixels as a PNG (test fixtures, frame dumps).
#[cfg(test)]
pub(crate) fn write_png(path: &Path, width: u32, height: u32, rgba: &[u8]) {
    let file = std::io::BufWriter::new(std::fs::File::create(path).unwrap());
    let mut enc = png::Encoder::new(file, width, height);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().unwrap().write_image_data(rgba).unwrap();
}

impl Source for StillSource {
    fn kind(&self) -> Kind {
        Kind::Still
    }

    fn poll(&mut self, _now: Instant) -> Result<Option<Frame>, MediaError> {
        // hand out the frame once: the shim keeps the texture
        Ok(self.frame.take())
    }
}

fn open_err(path: &Path, e: impl std::fmt::Display) -> MediaError {
    MediaError::Open {
        path: path.display().to_string(),
        reason: e.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn png_roundtrip_once() {
        let dir = std::env::temp_dir().join(format!("nsc-still-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("t.png");
        write_png(&p, 3, 2, &[255, 0, 0, 255].repeat(6));

        let mut s = StillSource::open(&p).unwrap();
        let f = s
            .poll(Instant::now())
            .unwrap()
            .expect("first poll gives frame");
        assert_eq!(f.data.size(), (3, 2));
        assert!(
            s.poll(Instant::now()).unwrap().is_none(),
            "still image is sent once"
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }

    fn encode_png(
        width: u32,
        height: u32,
        color: png::ColorType,
        bit_depth: png::BitDepth,
        data: &[u8],
    ) -> Vec<u8> {
        let mut out = Vec::new();
        let mut enc = png::Encoder::new(&mut out, width, height);
        enc.set_color(color);
        enc.set_depth(bit_depth);
        enc.write_header().unwrap().write_image_data(data).unwrap();
        out
    }

    #[test]
    fn png_color_types_become_rgba() {
        use png::{BitDepth, ColorType};
        let cases: [(ColorType, BitDepth, &[u8], [u8; 4]); 4] = [
            (
                ColorType::Rgb,
                BitDepth::Eight,
                &[10, 20, 30],
                [10, 20, 30, 255],
            ),
            (
                ColorType::Grayscale,
                BitDepth::Eight,
                &[77],
                [77, 77, 77, 255],
            ),
            (
                ColorType::GrayscaleAlpha,
                BitDepth::Eight,
                &[77, 128],
                [77, 77, 77, 128],
            ),
            // 16-bit samples keep their high byte
            (
                ColorType::Rgba,
                BitDepth::Sixteen,
                &[0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xff, 0xff],
                [0x12, 0x56, 0x9a, 0xff],
            ),
        ];
        for (color, depth, pixel, want) in cases {
            let (w, h, rgba) = decode(&encode_png(1, 1, color, depth, pixel)).unwrap();
            assert_eq!(
                (w, h, rgba.as_slice()),
                (1, 1, want.as_slice()),
                "{color:?} {depth:?}"
            );
        }
    }

    #[test]
    fn jpeg_decodes_to_rgba() {
        // 16x16 #ff8000, YCbCr 4:2:0
        let (w, h, rgba) = decode(include_bytes!("../../tests/data/orange.jpg")).unwrap();
        assert_eq!((w, h, rgba.len()), (16, 16, 16 * 16 * 4));
        let [r, g, b, a] = rgba[..4] else {
            unreachable!()
        };
        assert!(
            r > 240 && (110..150).contains(&g) && b < 15 && a == 255,
            "{:?}",
            &rgba[..4]
        );
    }

    #[test]
    fn cmyk_jpeg_matches_rgb() {
        // the same magenta-to-cyan gradient saved as RGB and as CMYK
        let (_, _, rgb) = decode(include_bytes!("../../tests/data/gradient.jpg")).unwrap();
        let (w, h, cmyk) = decode(include_bytes!("../../tests/data/gradient-cmyk.jpg")).unwrap();
        assert_eq!((w, h, cmyk.len()), (16, 64, rgb.len()));
        for c in 0..4 {
            let mean = |px: &[u8]| {
                px.iter()
                    .skip(c)
                    .step_by(4)
                    .map(|&v| f64::from(v))
                    .sum::<f64>()
                    / f64::from(w * h)
            };
            let (a, b) = (mean(&rgb), mean(&cmyk));
            assert!((a - b).abs() < 8.0, "channel {c}: RGB {a:.0}, CMYK {b:.0}");
        }
    }

    #[test]
    fn garbage_is_an_error_not_a_panic() {
        let dir = std::env::temp_dir().join(format!("nsc-still-bad-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("bad.png");
        // not an image at all, and a right signature with a broken body
        for bytes in [
            &b"not a png"[..],
            b"\x89PNG\r\n\x1a\nbroken",
            b"\xff\xd8\xff\xe0broken",
        ] {
            std::fs::write(&p, bytes).unwrap();
            assert!(matches!(
                StillSource::open(&p),
                Err(MediaError::Open { .. })
            ));
        }
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
