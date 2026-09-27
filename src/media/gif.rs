//! GIF with correct frame compositing (disposal Keep / Background / Previous),
//! as in the original plugin. Frames are decoded once on open, and the canvas
//! is built step by step: going forward adds frames, going back (loop) starts over.

use std::path::Path;
use std::sync::Arc;
use std::time::{Duration, Instant};

use gif::{ColorOutput, DecodeOptions, DisposalMethod};

use super::{Kind, MediaError, Source};
use crate::clock::PlaybackClock;
use crate::frame::{CpuFrame, Frame, FrameData};

struct GifFrame {
    delay: Duration,
    dispose: DisposalMethod,
    left: u32,
    top: u32,
    width: u32,
    height: u32,
    /// Frame RGBA; transparent pixels have alpha = 0.
    rgba: Vec<u8>,
}

pub struct GifSource {
    width: u32,
    height: u32,
    /// Background color in BGRA (same as the canvas).
    bg: [u8; 4],
    frames: Vec<GifFrame>,
    total: Duration,
    canvas: Vec<u8>,
    backup: Option<Vec<u8>>,
    /// Index of the last composited frame; `None` means a clean canvas.
    shown: Option<usize>,
    clock: PlaybackClock,
    generation: u64,
    sent: Option<usize>,
}

impl GifSource {
    pub fn open(path: &Path, speed: f64, looped: bool) -> Result<Self, MediaError> {
        let err = |e: &dyn std::fmt::Display| MediaError::Open {
            path: path.display().to_string(),
            reason: e.to_string(),
        };

        let file = std::fs::File::open(path).map_err(|e| err(&e))?;
        let mut opts = DecodeOptions::new();
        opts.set_color_output(ColorOutput::RGBA);
        let mut dec = opts
            .read_info(std::io::BufReader::new(file))
            .map_err(|e| err(&e))?;

        let (width, height) = (u32::from(dec.width()), u32::from(dec.height()));
        if width == 0 || height == 0 {
            return Err(err(&"empty GIF"));
        }

        let bg = match (dec.bg_color(), dec.global_palette()) {
            (Some(i), Some(pal)) if i * 3 + 2 < pal.len() => {
                [pal[i * 3 + 2], pal[i * 3 + 1], pal[i * 3], 255]
            }
            _ => [0, 0, 0, 0],
        };

        let mut frames = Vec::new();
        while let Some(f) = dec.read_next_frame().map_err(|e| err(&e))? {
            // Delay is in centiseconds; browsers and the original plugin treat 0 and 1 as 10.
            let cs = if f.delay <= 1 { 10 } else { u64::from(f.delay) };
            frames.push(GifFrame {
                delay: super::ms(cs * 10),
                dispose: f.dispose,
                left: u32::from(f.left),
                top: u32::from(f.top),
                width: u32::from(f.width),
                height: u32::from(f.height),
                rgba: f.buffer.to_vec(),
            });
        }
        if frames.is_empty() {
            return Err(err(&"GIF has no frames"));
        }
        let total = frames.iter().map(|f| f.delay).sum();

        let mut s = Self {
            width,
            height,
            bg,
            canvas: Vec::new(),
            frames,
            total,
            backup: None,
            shown: None,
            clock: PlaybackClock::new(speed, looped),
            generation: 0,
            sent: None,
        };
        s.clear_canvas();
        Ok(s)
    }

    fn clear_canvas(&mut self) {
        let px = self.width as usize * self.height as usize;
        self.canvas.clear();
        self.canvas.reserve(px * 4);
        for _ in 0..px {
            self.canvas.extend_from_slice(&self.bg);
        }
        self.shown = None;
        self.backup = None;
    }

    /// Which frame is visible at position `pos`.
    fn frame_at(&self, pos: Duration) -> usize {
        let mut acc = Duration::ZERO;
        for (i, f) in self.frames.iter().enumerate() {
            acc += f.delay;
            if pos < acc {
                return i;
            }
        }
        self.frames.len() - 1
    }

    fn fill_rect(&mut self, left: u32, top: u32, w: u32, h: u32, color: [u8; 4]) {
        let x1 = (left + w).min(self.width);
        let y1 = (top + h).min(self.height);
        for y in top.min(self.height)..y1 {
            let row = (y * self.width) as usize * 4;
            for x in left.min(self.width)..x1 {
                let i = row + x as usize * 4;
                self.canvas[i..i + 4].copy_from_slice(&color);
            }
        }
    }

    fn blit(&mut self, index: usize) {
        let f = &self.frames[index];
        let (left, top, fw) = (f.left, f.top, f.width);
        for y in 0..f.height {
            let dy = top + y;
            if dy >= self.height {
                break;
            }
            for x in 0..fw {
                let dx = left + x;
                if dx >= self.width {
                    break;
                }
                let s = ((y * fw + x) * 4) as usize;
                let src = &f.rgba[s..s + 4];
                if src[3] == 0 {
                    continue; // transparent frame pixel: the canvas shows through
                }
                let d = ((dy * self.width + dx) * 4) as usize;
                self.canvas[d..d + 4].copy_from_slice(&[src[2], src[1], src[0], 255]);
            }
        }
    }

    /// Dispose of the previous frame per its disposal method and composite the next one.
    fn step(&mut self) {
        let next = self.shown.map_or(0, |i| i + 1);
        if next >= self.frames.len() {
            return;
        }
        if let Some(prev) = self.shown {
            let f = &self.frames[prev];
            match f.dispose {
                DisposalMethod::Background => {
                    let (l, t, w, h) = (f.left, f.top, f.width, f.height);
                    self.fill_rect(l, t, w, h, self.bg);
                }
                DisposalMethod::Previous => {
                    if let Some(b) = self.backup.take() {
                        self.canvas = b;
                    }
                }
                DisposalMethod::Keep | DisposalMethod::Any => {}
            }
        }
        if self.frames[next].dispose == DisposalMethod::Previous {
            self.backup = Some(self.canvas.clone());
        }
        self.blit(next);
        self.shown = Some(next);
    }

    fn show(&mut self, target: usize) {
        if self.shown == Some(target) {
            return;
        }
        if self.shown.is_some_and(|i| target < i) {
            self.clear_canvas();
        }
        // guard against infinite loops, as in the original
        for _ in 0..=self.frames.len() {
            if self.shown == Some(target) {
                break;
            }
            self.step();
        }
    }

    #[cfg(test)]
    fn pixel(&self, x: u32, y: u32) -> [u8; 4] {
        let i = ((y * self.width + x) * 4) as usize;
        self.canvas[i..i + 4].try_into().unwrap()
    }
}

impl Source for GifSource {
    fn kind(&self) -> Kind {
        Kind::Gif
    }

    fn shown(&mut self) {
        self.clock.restart();
    }

    fn poll(&mut self, now: Instant) -> Result<Option<Frame>, MediaError> {
        let pos = self.clock.position(now, Some(self.total));
        let target = self.frame_at(pos);
        self.show(target);
        if self.sent == Some(target) {
            return Ok(None);
        }
        self.sent = Some(target);
        self.generation += 1;
        let pixels: Arc<[u8]> = self.canvas.as_slice().into();
        Ok(Some(Frame {
            generation: self.generation,
            data: FrameData::Cpu(CpuFrame::from_bgra(self.width, self.height, pixels)),
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gif::{Encoder, Frame as GFrame, Repeat};

    /// 2x1 GIF: frame 0 paints the left pixel red (Keep), frame 1 paints the right one blue.
    fn write_gif(path: &Path, dispose0: DisposalMethod) {
        let palette = [255, 0, 0, 0, 0, 255, 0, 0, 0];
        let mut f = std::fs::File::create(path).unwrap();
        let mut enc = Encoder::new(&mut f, 2, 1, &palette).unwrap();
        enc.set_repeat(Repeat::Infinite).unwrap();

        let a = GFrame {
            width: 1,
            height: 1,
            delay: 10,
            dispose: dispose0,
            buffer: std::borrow::Cow::Borrowed(&[0]),
            ..GFrame::default()
        };
        enc.write_frame(&a).unwrap();

        let b = GFrame {
            left: 1,
            width: 1,
            height: 1,
            delay: 20,
            buffer: std::borrow::Cow::Borrowed(&[1]),
            ..GFrame::default()
        };
        enc.write_frame(&b).unwrap();
    }

    fn tmp(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("nsc-gif-{}-{name}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("t.gif")
    }

    #[test]
    fn timeline_and_keep_disposal() {
        let p = tmp("keep");
        write_gif(&p, DisposalMethod::Keep);
        let mut g = GifSource::open(&p, 1.0, true).unwrap();
        assert_eq!(g.total, Duration::from_millis(300));

        let t0 = Instant::now();
        assert!(g.poll(t0).unwrap().is_some());
        assert_eq!(g.pixel(0, 0), [0, 0, 255, 255]); // red in BGRA

        assert!(
            g.poll(t0 + ms(50)).unwrap().is_none(),
            "same frame -> no upload"
        );
        assert!(g.poll(t0 + ms(150)).unwrap().is_some());
        assert_eq!(
            g.pixel(0, 0),
            [0, 0, 255, 255],
            "Keep leaves previous frame"
        );
        assert_eq!(g.pixel(1, 0), [255, 0, 0, 255]); // blue

        // loop: back to frame 0, the canvas is rebuilt from scratch
        g.poll(t0 + ms(310)).unwrap();
        assert_eq!(g.shown, Some(0));
        std::fs::remove_dir_all(p.parent().unwrap()).unwrap();
    }

    #[test]
    fn background_disposal_clears_rect() {
        let p = tmp("bg");
        write_gif(&p, DisposalMethod::Background);
        let mut g = GifSource::open(&p, 1.0, true).unwrap();
        let t0 = Instant::now();
        g.poll(t0).unwrap();
        g.poll(t0 + ms(150)).unwrap();
        assert_eq!(g.pixel(0, 0), g.bg, "Background disposal restores bg");
        std::fs::remove_dir_all(p.parent().unwrap()).unwrap();
    }

    #[test]
    fn shown_restarts_the_timeline() {
        let p = tmp("shown");
        write_gif(&p, DisposalMethod::Keep);
        let mut g = GifSource::open(&p, 1.0, false).unwrap();
        let t0 = Instant::now();
        assert!(g.poll(t0).unwrap().is_some(), "loader pulls frame 0");
        // shown for the first time long after the loader was done
        g.shown();
        let t1 = t0 + ms(10_000);
        assert!(g.poll(t1).unwrap().is_none(), "still frame 0, nothing new");
        assert_eq!(g.shown, Some(0));
        assert!(g.poll(t1 + ms(150)).unwrap().is_some());
        assert_eq!(g.shown, Some(1));
        std::fs::remove_dir_all(p.parent().unwrap()).unwrap();
    }

    #[test]
    fn no_loop_stops_on_last_frame() {
        let p = tmp("noloop");
        write_gif(&p, DisposalMethod::Keep);
        let mut g = GifSource::open(&p, 1.0, false).unwrap();
        let t0 = Instant::now();
        g.poll(t0).unwrap();
        g.poll(t0 + ms(10_000)).unwrap();
        assert_eq!(g.shown, Some(1));
        std::fs::remove_dir_all(p.parent().unwrap()).unwrap();
    }

    fn ms(v: u64) -> Duration {
        Duration::from_millis(v)
    }
}
