//! Video: decoding runs on its own thread; only the latest ready frame is exposed.
//!
//! The thread sleeps while nobody requests frames (no active screen capture),
//! same as the original plugin (400 ms threshold). After waking, the clock
//! restarts from the current frame instead of catching up on missed time.
//! Dropping the source stops and joins the thread, so nothing is left
//! running after the plugin is unloaded.

pub mod backend;
pub mod bitstream;
pub mod decode;
pub mod demux;
pub mod pipeline;
pub mod yuv;

use std::path::Path;
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use super::{Kind, MediaError, Source};
use crate::config::Settings;
use crate::frame::Frame;
use pipeline::{Next, Pipeline};

/// How long frames can go unrequested before the thread goes to sleep.
const IDLE_AFTER: Duration = Duration::from_millis(400);
/// A frame later than this is dropped instead of shown.
const DROP_LATE: Duration = Duration::from_millis(80);

#[derive(Default)]
struct Shared {
    latest: Option<Frame>,
    generation: u64,
    watched: Option<Instant>,
    quit: bool,
    /// Fatal thread error (shown to the user).
    error: Option<String>,
    ended: bool,
}

struct Sync {
    state: Mutex<Shared>,
    cv: Condvar,
}

impl Sync {
    fn lock(&self) -> MutexGuard<'_, Shared> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

pub struct VideoSource {
    sync: Arc<Sync>,
    worker: Option<JoinHandle<()>>,
    sent: u64,
    path: String,
}

impl VideoSource {
    pub fn open(
        path: &Path,
        speed: f64,
        looped: bool,
        settings: &Settings,
    ) -> Result<Self, MediaError> {
        let shown = path.display().to_string();
        let pipeline =
            backend::open_pipeline(path, looped, settings).map_err(|reason| MediaError::Video {
                path: shown.clone(),
                reason,
            })?;
        Ok(Self::spawn(pipeline, speed, shown))
    }

    fn spawn(pipeline: Pipeline, speed: f64, path: String) -> Self {
        let sync = Arc::new(Sync {
            state: Mutex::new(Shared::default()),
            cv: Condvar::new(),
        });
        let s2 = Arc::clone(&sync);
        let speed = crate::config::sanitize_speed(speed);
        let worker = std::thread::Builder::new()
            .name("noshare-video".into())
            .spawn(move || {
                // a panic in the decoder must not take down the compositor
                let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run(pipeline, speed, &s2)
                }));
                if r.is_err() {
                    let mut st = s2.lock();
                    st.error.get_or_insert_with(|| "decoder crashed".into());
                    s2.cv.notify_all();
                }
            })
            .expect("spawn video thread");
        Self {
            sync,
            worker: Some(worker),
            sent: 0,
            path,
        }
    }
}

impl Source for VideoSource {
    fn kind(&self) -> Kind {
        Kind::Video
    }

    fn poll(&mut self, now: Instant) -> Result<Option<Frame>, MediaError> {
        let mut st = self.sync.lock();
        st.watched = Some(now);
        self.sync.cv.notify_all();
        if let Some(e) = &st.error
            && st.generation == 0
        {
            return Err(MediaError::Video {
                path: self.path.clone(),
                reason: e.clone(),
            });
        }
        if st.generation == self.sent {
            return Ok(None);
        }
        self.sent = st.generation;
        Ok(st.latest.take())
    }
}

impl Drop for VideoSource {
    fn drop(&mut self) {
        {
            let mut st = self.sync.lock();
            st.quit = true;
        }
        self.sync.cv.notify_all();
        if let Some(w) = self.worker.take() {
            let _ = w.join();
        }
    }
}

fn is_watched(st: &Shared) -> bool {
    st.watched.is_some_and(|t| t.elapsed() < IDLE_AFTER)
}

fn run(mut pipe: Pipeline, speed: f64, sync: &Sync) {
    // (wall time, pts): the clock's reference point
    let mut origin: Option<(Instant, Duration)> = None;

    loop {
        // sleep while nobody is watching
        {
            let mut st = sync.lock();
            let mut slept = false;
            while !st.quit && !is_watched(&st) {
                slept = true;
                st = sync
                    .cv
                    .wait_timeout(st, IDLE_AFTER)
                    .map(|(g, _)| g)
                    .unwrap_or_else(|e| e.into_inner().0);
            }
            if st.quit {
                return;
            }
            if slept {
                origin = None; // after sleeping, resume from the current position instead of catching up
            }
        }

        let frame = match pipe.next_frame() {
            Ok(Next::Frame(f)) => f,
            Ok(Next::End) => {
                let mut st = sync.lock();
                st.ended = true;
                // video ended without looping: the last frame stays on screen
                while !st.quit {
                    st = sync.cv.wait(st).unwrap_or_else(|e| e.into_inner());
                }
                return;
            }
            Err(e) => {
                let mut st = sync.lock();
                st.error = Some(e);
                sync.cv.notify_all();
                return;
            }
        };

        let (t0, pts0) = *origin.get_or_insert((Instant::now(), frame.pts));
        let due = t0 + frame.pts.saturating_sub(pts0).div_f64(speed);
        let now = Instant::now();

        if due > now {
            // wait until display time, but wake up on quit
            let mut st = sync.lock();
            let deadline = due;
            while !st.quit {
                let left = deadline.saturating_duration_since(Instant::now());
                if left.is_zero() {
                    break;
                }
                st = sync
                    .cv
                    .wait_timeout(st, left.min(Duration::from_secs(1)))
                    .map(|(g, _)| g)
                    .unwrap_or_else(|e| e.into_inner().0);
            }
            if st.quit {
                return;
            }
        } else if now - due > DROP_LATE {
            continue; // late: drop it so we don't fall further behind
        }

        let mut st = sync.lock();
        st.generation += 1;
        let generation = st.generation;
        st.latest = Some(Frame {
            generation,
            data: frame.data,
        });
        drop(st);
        sync.cv.notify_all(); // the first poll may be waiting for this frame
    }
}

#[cfg(test)]
mod tests {
    use super::pipeline::testing::{FakeDecoder, FakeDemux};
    use super::*;

    fn source(frames: u32, step_ms: u64, looped: bool) -> VideoSource {
        let pipe = Pipeline::new(
            Box::new(FakeDemux::new(frames, Duration::from_millis(step_ms))),
            Box::new(FakeDecoder::default()),
            looped,
        );
        VideoSource::spawn(pipe, 1.0, "fake".into())
    }

    fn wait_frame(s: &mut VideoSource, timeout: Duration) -> Option<Frame> {
        let end = Instant::now() + timeout;
        while Instant::now() < end {
            if let Some(f) = s.poll(Instant::now()).unwrap() {
                return Some(f);
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        None
    }

    #[test]
    fn produces_frames_while_watched() {
        let mut s = source(50, 10, true);
        let a = wait_frame(&mut s, Duration::from_secs(2)).expect("first frame");
        let b = wait_frame(&mut s, Duration::from_secs(2)).expect("next frame");
        assert!(b.generation > a.generation);
    }

    #[test]
    fn sleeps_when_not_watched() {
        let mut s = source(1000, 5, true);
        wait_frame(&mut s, Duration::from_secs(2)).expect("frame");
        std::thread::sleep(IDLE_AFTER + Duration::from_millis(200));
        let before = s.sync.lock().generation;
        std::thread::sleep(Duration::from_millis(300));
        let after = s.sync.lock().generation;
        assert_eq!(before, after, "no decoding without viewers");
    }

    #[test]
    fn drop_joins_thread_quickly() {
        let mut s = source(1000, 1000, true); // long gaps between frames
        wait_frame(&mut s, Duration::from_secs(2));
        let t = Instant::now();
        drop(s);
        assert!(
            t.elapsed() < Duration::from_millis(500),
            "drop must not wait for next frame"
        );
    }

    #[test]
    fn no_loop_keeps_last_frame_and_stops() {
        let mut s = source(3, 5, false);
        let mut last = None;
        let end = Instant::now() + Duration::from_secs(2);
        while Instant::now() < end && !s.sync.lock().ended {
            if let Some(f) = s.poll(Instant::now()).unwrap() {
                last = Some(f.generation);
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(s.sync.lock().ended);
        assert!(last.is_some());
    }
}
