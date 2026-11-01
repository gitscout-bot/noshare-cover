//! Threads started from Hyprland's own threads.
//!
//! `std::thread::spawn` keeps state on the calling thread (its spawn hooks) whose destructor
//! glibc registers for that thread, and a library with a thread-local destructor pending
//! stays mapped until that thread ends: dlclose doesn't unmap it. A cover is opened on a
//! thread started from Hyprland's main thread, which lives as long as the session, so every
//! unload or update of the plugin left the old copy in memory. These threads are started
//! with `pthread_create`, which leaves nothing behind on the caller; inside them std works as
//! usual, and what they register goes when they end.

use std::ffi::{CStr, c_void};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

/// A thread joined on drop: it runs code of this library, so it must not outlive it.
pub struct Worker {
    id: libc::pthread_t,
    done: Arc<AtomicBool>,
}

struct Job {
    run: Box<dyn FnOnce() + Send>,
    done: Arc<AtomicBool>,
}

extern "C" fn start(arg: *mut c_void) -> *mut c_void {
    // SAFETY: the box from Worker::spawn, taken exactly once.
    let job = unsafe { Box::from_raw(arg.cast::<Job>()) };
    // a panic must not unwind into the C thread start
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(job.run));
    job.done.store(true, Ordering::Release);
    std::ptr::null_mut()
}

impl Worker {
    /// `name` is at most 15 bytes (the kernel's limit).
    pub fn spawn(name: &CStr, run: impl FnOnce() + Send + 'static) -> std::io::Result<Self> {
        let done = Arc::new(AtomicBool::new(false));
        let job = Box::into_raw(Box::new(Job {
            run: Box::new(run),
            done: Arc::clone(&done),
        }));
        let mut id: libc::pthread_t = 0;
        // SAFETY: start takes the job; on failure it was never started and is freed here.
        let rc = unsafe { libc::pthread_create(&mut id, std::ptr::null(), start, job.cast()) };
        if rc != 0 {
            drop(unsafe { Box::from_raw(job) });
            return Err(std::io::Error::from_raw_os_error(rc));
        }
        // SAFETY: a live thread and a NUL-terminated name; only a label, failure doesn't matter.
        unsafe { libc::pthread_setname_np(id, name.as_ptr()) };
        Ok(Self { id, done })
    }

    /// The thread has run to its end (joining now doesn't wait).
    pub fn is_finished(&self) -> bool {
        self.done.load(Ordering::Acquire)
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        // SAFETY: a thread started by spawn and joined only here.
        unsafe { libc::pthread_join(self.id, std::ptr::null_mut()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    #[test]
    fn runs_and_joins_on_drop() {
        let hits = Arc::new(AtomicUsize::new(0));
        let h = Arc::clone(&hits);
        let w = Worker::spawn(c"nsc-test", move || {
            std::thread::sleep(std::time::Duration::from_millis(20));
            h.fetch_add(1, Ordering::SeqCst);
        })
        .unwrap();
        drop(w);
        assert_eq!(hits.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn a_panic_stays_inside() {
        let w = Worker::spawn(c"nsc-test", || panic!("boom")).unwrap();
        while !w.is_finished() {
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
    }
}
