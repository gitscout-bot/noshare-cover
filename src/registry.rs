//! Cover registry: one source per unique (file, speed, loop), no matter how
//! many windows show it.
//!
//! Lifecycle:
//! - global settings change: everything is reset and the epoch is bumped (the
//!   shim drops its textures on a new epoch);
//! - file missing: retry once per second, no stat() on every frame;
//! - failed to open: one notification, no retries until the config changes;
//! - cover not shown for a while: the source is closed (for video this stops
//!   the decode thread) instead of living until the plugin is unloaded.
//!
//! Opening never happens on the render path: `resolve` is called from the
//! compositor's render hook, and opening a file (decoding a PNG, reading a GIF,
//! setting up a video decoder and waiting for its first frame) can take tens
//! of milliseconds. A new cover is opened on a loader thread; until it is ready
//! `resolve` returns `None` and the shim keeps drawing the black box, and
//! `animating` stays true so the shim keeps asking for frames and picks the
//! cover up as soon as it lands.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::config::{ConfigError, PlayParams, Settings};
use crate::frame::Frame;
use crate::media::{self, MediaError, Source};
use crate::notify::Notifier;

/// Opens a source for the given parameters (replaced in tests).
pub type Opener = fn(&PlayParams, &Settings) -> Result<Box<dyn Source>, MediaError>;

const MISSING_RETRY: Duration = Duration::from_secs(1);
const EVICT_AFTER: Duration = Duration::from_secs(30);
/// How long the loader keeps polling a freshly opened source for its first
/// frame before handing it over anyway (the render path then just polls it).
const FIRST_FRAME_WAIT: Duration = Duration::from_secs(2);
const FIRST_FRAME_STEP: Duration = Duration::from_millis(4);

type Loaded = Result<(Box<dyn Source>, Option<Frame>), MediaError>;

/// Opens a source and prepares its first frame on a separate thread.
struct Loader {
    rx: Receiver<Loaded>,
    cancel: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl Loader {
    fn spawn(opener: Opener, play: PlayParams, settings: Settings) -> Self {
        let (tx, rx) = mpsc::sync_channel(1);
        let cancel = Arc::new(AtomicBool::new(false));
        let stop = Arc::clone(&cancel);
        let job = move || {
            // a panic in a decoder must not take down the compositor
            let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                load(opener, &play, &settings, &stop)
            }))
            .unwrap_or_else(|_| {
                Err(MediaError::Open {
                    path: play.path.display().to_string(),
                    reason: "loader crashed".into(),
                })
            });
            let _ = tx.send(r);
        };
        let worker = std::thread::Builder::new()
            .name("noshare-open".into())
            .spawn(job)
            .ok();
        Self { rx, cancel, worker }
    }
}

impl Loader {
    /// The thread is done (result sent, or it never started): dropping the
    /// loader now doesn't wait.
    fn finished(&self) -> bool {
        self.worker.as_ref().is_none_or(|w| w.is_finished())
    }

    /// Nobody needs the result any more. Stops the first-frame wait; the open
    /// itself (decoding a whole GIF, setting up a decoder) can't be interrupted,
    /// so the registry parks the loader instead of dropping it on the render path.
    fn cancel(&self) {
        self.cancel.store(true, Ordering::Relaxed);
    }
}

impl Drop for Loader {
    fn drop(&mut self) {
        // The thread must be gone before the plugin can be unloaded. The registry
        // only drops loaders that are finished, or on its own drop (unload).
        self.cancel();
        if let Some(w) = self.worker.take() {
            let _ = w.join();
        }
    }
}

fn load(opener: Opener, play: &PlayParams, settings: &Settings, cancel: &AtomicBool) -> Loaded {
    let mut src = opener(play, settings)?;
    let deadline = Instant::now() + FIRST_FRAME_WAIT;
    loop {
        if let Some(frame) = src.poll(Instant::now())? {
            return Ok((src, Some(frame)));
        }
        if cancel.load(Ordering::Relaxed) || Instant::now() >= deadline {
            return Ok((src, None));
        }
        std::thread::sleep(FIRST_FRAME_STEP);
    }
}

enum State {
    /// Being opened on a loader thread. `retry` is set when this is a reopen of
    /// a file that was missing (its absence was already reported).
    Opening {
        loader: Loader,
        retry: bool,
    },
    Ready(Box<dyn Source>),
    Missing {
        retry_at: Instant,
    },
    Failed,
}

struct Cover {
    id: u64,
    state: State,
    current: Option<Frame>,
    polled_in: u64,
    last_used: Instant,
}

/// What the shim draws: a stable cover id (texture cache key) and the frame.
pub struct CoverView<'a> {
    pub id: u64,
    pub frame: &'a Frame,
}

pub struct Registry {
    settings: Option<Settings>,
    epoch: u64,
    covers: HashMap<PlayParams, Cover>,
    next_id: u64,
    frame_no: u64,
    notifier: Notifier,
    opener: Opener,
    /// Loaders of covers dropped while still opening (settings change, eviction).
    /// Joining them right there could stall the render hook for as long as the
    /// open takes, so they are parked here and reaped once finished. What is left
    /// is joined when the registry itself goes away, on unload.
    retired: Vec<Loader>,
}

impl Default for Registry {
    fn default() -> Self {
        Self::with_opener(media::open)
    }
}

impl Registry {
    pub fn with_opener(opener: Opener) -> Self {
        Self {
            settings: None,
            epoch: 0,
            covers: HashMap::new(),
            next_id: 1,
            frame_no: 0,
            notifier: Notifier::default(),
            opener,
            retired: Vec::new(),
        }
    }

    /// Park the loader of a cover that is going away (see `retired`).
    fn retire(&mut self, cover: Cover) {
        // a finished loader is simply dropped with the cover: joining it is instant
        if let State::Opening { loader, .. } = cover.state
            && !loader.finished()
        {
            loader.cancel();
            self.retired.push(loader);
        }
    }

    pub fn epoch(&self) -> u64 {
        self.epoch
    }

    pub fn settings(&self) -> Settings {
        self.settings.clone().unwrap_or_default()
    }

    pub fn notifier(&mut self) -> &mut Notifier {
        &mut self.notifier
    }

    /// New global settings. If they changed, reset everything.
    pub fn set_settings(&mut self, settings: Settings, error: Option<ConfigError>) {
        if self.settings.as_ref() == Some(&settings) {
            return;
        }
        // dropping sources stops the video threads; covers still opening are parked
        for (_, cover) in std::mem::take(&mut self.covers) {
            self.retire(cover);
        }
        self.settings = Some(settings);
        self.epoch += 1;
        self.notifier.reset();
        if let Some(e) = error {
            self.notifier.push(e.to_string());
        }
        self.prewarm(Instant::now());
    }

    /// Open the default cover up front so the first screen capture already has
    /// a frame ready. The video then sleeps (nobody is watching) but keeps its
    /// last frame; eviction never touches it.
    fn prewarm(&mut self, now: Instant) {
        let play = self.settings().default_play();
        if play.path.as_os_str().is_empty() {
            return;
        }
        self.warm(&play, now);
    }

    pub fn begin_frame(&mut self) {
        self.frame_no += 1;
        // joining a finished thread is instant
        self.retired.retain(|l| !l.finished());
    }

    /// Cover for a window. `None` means nothing to draw (no file, error, no frame yet).
    pub fn resolve(&mut self, play: &PlayParams, now: Instant) -> Option<CoverView<'_>> {
        // no default cover and no rule for this window: nothing to draw, not an error
        if play.path.as_os_str().is_empty() {
            return None;
        }
        let settings = self.settings();
        let frame_no = self.frame_no;

        if !self.covers.contains_key(play) {
            self.start(play, &settings, now);
        }

        let cover = self.covers.get_mut(play)?;
        cover.last_used = now;

        if let State::Missing { retry_at } = cover.state {
            if now >= retry_at && play.path.exists() {
                cover.state = State::Opening {
                    loader: Loader::spawn(self.opener, play.clone(), settings.clone()),
                    retry: true,
                };
            } else if now >= retry_at {
                cover.state = State::Missing {
                    retry_at: now + MISSING_RETRY,
                };
            }
        }

        if let State::Opening { loader, retry } = &cover.state {
            let retry = *retry;
            let next = match loader.rx.try_recv() {
                Err(TryRecvError::Empty) => None,
                Ok(Ok((mut src, frame))) => {
                    // first time on screen: restart its timeline and poll it
                    // right below, so the clock starts with this frame
                    src.shown();
                    if let Some(f) = frame {
                        cover.current = Some(f);
                    }
                    Some(State::Ready(src))
                }
                Ok(Err(MediaError::Missing(p))) => {
                    if !retry {
                        self.notifier
                            .push(format!("noshare-cover: file not found: {p}"));
                    }
                    Some(State::Missing {
                        retry_at: now + MISSING_RETRY,
                    })
                }
                Ok(Err(e)) => {
                    self.notifier.push(format!("noshare-cover: {e}"));
                    Some(State::Failed)
                }
                Err(TryRecvError::Disconnected) => {
                    self.notifier.push(String::from(
                        "noshare-cover: failed to start the cover loader",
                    ));
                    Some(State::Failed)
                }
            };
            if let Some(state) = next {
                cover.state = state; // drops the loader, its thread has finished
            }
        }

        if let State::Ready(src) = &mut cover.state {
            // a cover shown in several windows is polled once per frame
            if cover.polled_in != frame_no {
                cover.polled_in = frame_no;
                match src.poll(now) {
                    Ok(Some(f)) => cover.current = Some(f),
                    Ok(None) => {}
                    Err(e) => {
                        self.notifier.push(format!("noshare-cover: {e}"));
                        cover.state = State::Failed;
                        cover.current = None;
                    }
                }
            }
        }

        let cover = self.covers.get(play)?;
        cover.current.as_ref().map(|frame| CoverView {
            id: cover.id,
            frame,
        })
    }

    /// Start opening a cover ahead of time: a window just got a rule with it,
    /// so by the first capture (a single screenshot has no second frame to
    /// catch up in) the frame is already there. No-op if the cover exists.
    pub fn warm(&mut self, play: &PlayParams, now: Instant) {
        if play.path.as_os_str().is_empty() || self.covers.contains_key(play) {
            return;
        }
        let settings = self.settings();
        self.start(play, &settings, now);
    }

    fn start(&mut self, play: &PlayParams, settings: &Settings, now: Instant) {
        let id = self.next_id;
        self.next_id += 1;
        let state = State::Opening {
            loader: Loader::spawn(self.opener, play.clone(), settings.clone()),
            retry: false,
        };
        self.covers.insert(
            play.clone(),
            Cover {
                id,
                state,
                current: None,
                polled_in: 0,
                last_used: now,
            },
        );
    }

    /// End of frame: close covers nobody has shown for a while.
    pub fn end_frame(&mut self, now: Instant) {
        let default = self.settings().default_play();
        let stale: Vec<PlayParams> = self
            .covers
            .iter()
            .filter(|(play, c)| {
                **play != default && now.saturating_duration_since(c.last_used) >= EVICT_AFTER
            })
            .map(|(play, _)| play.clone())
            .collect();
        for play in stale {
            if let Some(cover) = self.covers.remove(&play) {
                self.retire(cover);
            }
        }
    }

    /// Whether any live animated cover (video, GIF) exists. The shim uses this
    /// to decide whether to nudge Hyprland into new capture frames. Covers not
    /// shown for a while get evicted after EVICT_AFTER anyway.
    ///
    /// A cover that is still being opened counts too: its first frame only
    /// shows up on the next rendered frame, and on a static screen there
    /// wouldn't be one.
    pub fn animating(&self, now: Instant) -> bool {
        self.covers.values().any(|c| {
            let live = match &c.state {
                State::Opening { .. } => true,
                State::Ready(src) => src.kind() != crate::media::Kind::Still,
                _ => false,
            };
            live && now.saturating_duration_since(c.last_used) < EVICT_AFTER
        })
    }

    /// Live cover ids (the shim frees textures for ids not listed here).
    pub fn live_ids(&self) -> impl Iterator<Item = u64> + '_ {
        self.covers.values().map(|c| c.id)
    }

    pub fn len(&self) -> usize {
        self.covers.len()
    }

    pub fn is_empty(&self) -> bool {
        self.covers.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame::{CpuFrame, FrameData};
    use crate::media::Kind;
    use std::sync::Mutex;

    // Opens per path: the opener runs on loader threads and tests run in
    // parallel, so each test counts only its own paths.
    static OPENS: Mutex<Vec<String>> = Mutex::new(Vec::new());

    fn opens(path: &str) -> usize {
        OPENS.lock().unwrap().iter().filter(|p| *p == path).count()
    }

    struct Solid(u64);
    impl Source for Solid {
        fn kind(&self) -> Kind {
            Kind::Still
        }
        fn poll(&mut self, _: Instant) -> Result<Option<Frame>, MediaError> {
            self.0 += 1;
            Ok(Some(Frame {
                generation: self.0,
                data: FrameData::Cpu(CpuFrame::from_bgra(1, 1, vec![0; 4].into())),
            }))
        }
    }

    /// Slow to open and to produce its first frame, like a big video.
    struct Slow {
        ready_at: Instant,
    }
    impl Source for Slow {
        fn kind(&self) -> Kind {
            Kind::Video
        }
        fn poll(&mut self, now: Instant) -> Result<Option<Frame>, MediaError> {
            if now < self.ready_at {
                return Ok(None);
            }
            Ok(Some(Frame {
                generation: 1,
                data: FrameData::Cpu(CpuFrame::from_bgra(1, 1, vec![0; 4].into())),
            }))
        }
    }

    fn opener(p: &PlayParams, _: &Settings) -> Result<Box<dyn Source>, MediaError> {
        let path = p.path.to_str().unwrap();
        OPENS.lock().unwrap().push(path.to_string());
        match path {
            "/missing" | "/missing-backoff" => Err(MediaError::Missing(path.into())),
            "/bad" => Err(MediaError::UnknownFormat(".xyz".into())),
            "/slow" => {
                std::thread::sleep(Duration::from_millis(150));
                Ok(Box::new(Slow {
                    ready_at: Instant::now() + Duration::from_millis(150),
                }))
            }
            _ => Ok(Box::new(Solid(0))),
        }
    }

    /// Resolve until the loader thread has delivered (or the state settled).
    fn settle(r: &mut Registry, p: &PlayParams) -> Option<u64> {
        let end = Instant::now() + Duration::from_secs(5);
        loop {
            r.begin_frame();
            let now = Instant::now();
            if let Some(v) = r.resolve(p, now) {
                return Some(v.frame.generation);
            }
            let opening = matches!(
                r.covers.get(p).map(|c| &c.state),
                Some(State::Opening { .. })
            );
            if !opening || Instant::now() > end {
                return None;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
    }

    fn play(path: &str) -> PlayParams {
        PlayParams {
            path: path.into(),
            speed: 1.0,
            looped: true,
        }
    }

    fn defaults() -> Settings {
        Settings {
            path_cover: "/default".into(),
            ..Settings::default()
        }
    }

    /// Registry with settings; the default cover (/default) is already prewarmed.
    fn reg() -> Registry {
        let mut r = Registry::with_opener(opener);
        r.set_settings(defaults(), None);
        r
    }

    #[test]
    fn default_cover_is_prewarmed_and_kept() {
        let r = reg();
        assert_eq!(r.len(), 1, "default cover is opened right away");
        let mut r = r;
        r.end_frame(Instant::now() + EVICT_AFTER * 3);
        assert_eq!(
            r.len(),
            1,
            "and never evicted, even if unused for a long time"
        );
    }

    #[test]
    fn shared_cover_polled_once_per_frame() {
        let mut r = reg();
        let now = Instant::now();
        let g1 = settle(&mut r, &play("/a")).expect("cover loaded");
        let g2 = r.resolve(&play("/a"), now).unwrap().frame.generation;
        assert_eq!(g1, g2, "second window in same frame reuses the frame");
        r.begin_frame();
        assert_eq!(
            r.resolve(&play("/a"), now).unwrap().frame.generation,
            g1 + 1
        );
        assert_eq!(r.len(), 2, "/default + /a");
    }

    #[test]
    fn opening_does_not_block_the_render_path() {
        let mut r = reg();
        let p = play("/slow");
        r.begin_frame();
        let t = Instant::now();
        assert!(
            r.resolve(&p, Instant::now()).is_none(),
            "nothing to draw yet"
        );
        assert!(
            t.elapsed() < Duration::from_millis(50),
            "resolve returned in {:?}, it must not wait for the open",
            t.elapsed()
        );
        assert!(
            r.animating(Instant::now()),
            "a cover being opened keeps the shim asking for frames"
        );
        assert_eq!(settle(&mut r, &p), Some(1), "first frame arrives later");
    }

    #[test]
    fn warmed_cover_is_ready_on_first_resolve() {
        let mut r = reg();
        let p = play("/warm");
        r.warm(&p, Instant::now());
        r.warm(&p, Instant::now());
        assert!(loaded(&r, &p), "loader finished");
        assert_eq!(opens("/warm"), 1, "warming twice opens once");
        r.begin_frame();
        assert!(
            r.resolve(&p, Instant::now()).is_some(),
            "the first capture after warming already has a frame"
        );
    }

    /// Wait until the cover's loader thread is done (its result is ready to be
    /// picked up by the next resolve), with a timeout instead of a fixed sleep.
    fn loaded(r: &Registry, p: &PlayParams) -> bool {
        let end = Instant::now() + Duration::from_secs(5);
        loop {
            let done = match r.covers.get(p).map(|c| &c.state) {
                Some(State::Opening { loader, .. }) => loader.finished(),
                Some(_) => true,
                None => false,
            };
            if done || Instant::now() > end {
                return done;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
    }

    #[test]
    fn dropping_an_opening_cover_does_not_block() {
        let mut r = reg();
        assert!(
            loaded(&r, &play("/default")),
            "default cover out of the way"
        );
        r.begin_frame();
        assert!(r.resolve(&play("/slow"), Instant::now()).is_none());
        // a settings change drops every cover, including the one being opened
        // (the slow opener takes 150 ms): that must not wait for it
        let t = Instant::now();
        r.set_settings(
            Settings {
                speed: 3.0,
                ..defaults()
            },
            None,
        );
        assert!(
            t.elapsed() < Duration::from_millis(50),
            "set_settings took {:?}",
            t.elapsed()
        );
        assert_eq!(r.retired.len(), 1, "the loader is parked");

        // eviction the same way
        r.begin_frame();
        let now = Instant::now();
        assert!(r.resolve(&play("/slow"), now).is_none());
        let t = Instant::now();
        r.end_frame(now + EVICT_AFTER * 2);
        assert!(t.elapsed() < Duration::from_millis(50));
        assert_eq!(r.retired.len(), 2);

        // finished loaders are reaped at the start of a frame
        let end = Instant::now() + Duration::from_secs(5);
        while !r.retired.iter().all(Loader::finished) && Instant::now() < end {
            std::thread::sleep(Duration::from_millis(2));
        }
        r.begin_frame();
        assert!(r.retired.is_empty());
    }

    #[test]
    fn unload_joins_parked_loaders() {
        let mut r = reg();
        assert!(
            loaded(&r, &play("/default")),
            "default cover out of the way"
        );
        r.begin_frame();
        assert!(r.resolve(&play("/slow"), Instant::now()).is_none());
        r.set_settings(
            Settings {
                speed: 4.0,
                ..defaults()
            },
            None,
        );
        assert_eq!(r.retired.len(), 1);
        let t = Instant::now();
        drop(r); // plugin unload: the parked loader is joined, not leaked
        assert!(
            t.elapsed() >= Duration::from_millis(50),
            "drop waited for the loader ({:?})",
            t.elapsed()
        );
    }

    #[test]
    fn empty_path_is_silent() {
        let mut r = reg();
        let now = Instant::now();
        r.begin_frame();
        assert!(r.resolve(&play(""), now).is_none());
        assert!(r.notifier().pop().is_none());
    }

    #[test]
    fn errors_notify_once() {
        let mut r = reg();
        assert!(settle(&mut r, &play("/bad")).is_none());
        let now = Instant::now();
        for _ in 0..3 {
            r.begin_frame();
            assert!(r.resolve(&play("/bad"), now).is_none());
        }
        assert!(r.notifier().pop().is_some());
        assert!(r.notifier().pop().is_none());
    }

    #[test]
    fn settings_change_resets_everything() {
        let mut r = reg();
        let now = Instant::now();
        settle(&mut r, &play("/a")).expect("cover loaded");
        let id = r.resolve(&play("/a"), now).unwrap().id;
        let epoch = r.epoch();
        r.set_settings(
            Settings {
                speed: 2.0,
                ..defaults()
            },
            None,
        );
        assert_eq!(r.epoch(), epoch + 1);
        assert_eq!(
            r.len(),
            1,
            "everything reset, only the default cover is prewarmed again"
        );
        settle(&mut r, &play("/a")).expect("cover loaded");
        assert_ne!(
            r.resolve(&play("/a"), now).unwrap().id,
            id,
            "new cover id after reset"
        );
    }

    #[test]
    fn same_settings_do_not_reset() {
        let mut r = reg();
        let e = r.epoch();
        r.set_settings(defaults(), None);
        assert_eq!(r.epoch(), e);
    }

    #[test]
    fn unused_covers_are_evicted() {
        let mut r = reg();
        let now = Instant::now();
        r.begin_frame();
        r.resolve(&play("/a"), now);
        r.end_frame(now + Duration::from_secs(5));
        assert_eq!(r.len(), 2);
        r.end_frame(now + EVICT_AFTER + Duration::from_secs(1));
        assert_eq!(r.len(), 1, "/a evicted, /default kept");
    }

    #[test]
    fn missing_file_is_retried_with_backoff() {
        let mut r = reg();
        let p = play("/missing-backoff");
        assert!(settle(&mut r, &p).is_none());
        let now = Instant::now();
        for i in 0..10 {
            r.begin_frame();
            r.resolve(&p, now + Duration::from_millis(i * 10));
        }
        // first open only; retries happen after the backoff and only if the file appears
        assert_eq!(opens("/missing-backoff"), 1);
        assert!(r.notifier().pop().is_some(), "missing file reported once");
        assert!(r.notifier().pop().is_none());
    }
}
