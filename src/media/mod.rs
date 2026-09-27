//! Media sources: image, GIF, video. The registry sees them all the same way:
//! "give me the frame for this position if it changed".

pub mod gif;
pub mod still;
pub mod video;

use std::path::Path;
use std::time::{Duration, Instant};

use crate::config::{PlayParams, Settings};
use crate::frame::Frame;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Still,
    Gif,
    Video,
}

#[derive(Debug, thiserror::Error)]
pub enum MediaError {
    #[error("file not found: {0}")]
    Missing(String),
    #[error("unsupported format {0}")]
    UnknownFormat(String),
    #[error("failed to open {path}: {reason}")]
    Open { path: String, reason: String },
    #[error("video {path}: {reason}")]
    Video { path: String, reason: String },
}

/// Common source interface.
pub trait Source: Send {
    fn kind(&self) -> Kind;

    /// The frame current at `now`. `None` means the image hasn't changed since
    /// the last call (the shim keeps drawing the old texture). For video, `now`
    /// also signals "someone is watching": without calls the decoder goes to sleep.
    fn poll(&mut self, now: Instant) -> Result<Option<Frame>, MediaError>;

    /// The cover reaches the screen for the first time. The loader thread has
    /// already pulled the first frame, possibly long before (covers are opened
    /// ahead of the first capture), so a timeline that started there restarts
    /// here: a GIF that doesn't loop must not have played out unseen.
    fn shown(&mut self) {}
}

/// Supported extensions, case-insensitive.
pub fn kind_for(path: &Path) -> Option<Kind> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    match ext.as_str() {
        "png" | "jpg" | "jpeg" => Some(Kind::Still),
        "gif" => Some(Kind::Gif),
        "mp4" | "m4v" | "mov" | "webm" | "mkv" => Some(Kind::Video),
        _ => None,
    }
}

/// Open a source by path. The error is ready to show to the user as is.
pub fn open(play: &PlayParams, settings: &Settings) -> Result<Box<dyn Source>, MediaError> {
    let path = &play.path;
    let shown = path.display().to_string();
    if !path.exists() {
        return Err(MediaError::Missing(shown));
    }
    let kind = kind_for(path).ok_or_else(|| {
        MediaError::UnknownFormat(
            path.extension()
                .map(|e| format!(".{}", e.to_string_lossy()))
                .unwrap_or_else(|| shown.clone()),
        )
    })?;
    Ok(match kind {
        Kind::Still => Box::new(still::StillSource::open(path)?),
        Kind::Gif => Box::new(gif::GifSource::open(path, play.speed, play.looped)?),
        Kind::Video => Box::new(video::VideoSource::open(
            path,
            play.speed,
            play.looped,
            settings,
        )?),
    })
}

/// Small helper for sources: milliseconds to Duration without overflow.
pub(crate) fn ms(v: u64) -> Duration {
    Duration::from_millis(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kinds_by_extension() {
        assert_eq!(kind_for(Path::new("a.GIF")), Some(Kind::Gif));
        assert_eq!(kind_for(Path::new("/x/y.jpeg")), Some(Kind::Still));
        assert_eq!(kind_for(Path::new("clip.MKV")), Some(Kind::Video));
        assert_eq!(kind_for(Path::new("clip.avi")), None);
        assert_eq!(kind_for(Path::new("noext")), None);
    }
}
