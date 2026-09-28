//! Extra rects from other plugins (e.g. gloview: previews of no_screen_share
//! windows as overlay tiles).
//!
//! API v2 (`include/noshare_cover_api.h`):
//! - each plugin gets its own client: `register` → id, so no other client can
//!   wipe its rects;
//! - `set_rects(client, monitor, rects[])` atomically replaces the client's set on
//!   a monitor: no "cleared but not re-added yet" window, so no flicker;
//! - fill: black or the cover of a specific window (by Hyprland window address,
//!   as in `hyprctl clients`), so the overlay tile shows the same image
//!   as the window itself in the screencast;
//! - `unregister` removes everything the client holds (call it on plugin unload).
//!
//! API v1 (`nsc_api_clear_extra_rects` / `nsc_api_add_extra_rect`)
//! works as before: it's the "legacy" client with black fill.

use std::sync::{Mutex, MutexGuard};

pub const API_VERSION: u32 = 2;

/// How to fill a rect.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Fill {
    Black = 0,
    /// Cover of `window`; black if the window doesn't exist or has no cover.
    WindowCover = 1,
}

impl Fill {
    fn from_raw(v: u32) -> Self {
        if v == Fill::WindowCover as u32 {
            Fill::WindowCover
        } else {
            Fill::Black
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CoverRect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub rounding: f64,
    /// Hyprland window address (as `address` in `hyprctl clients`), 0 = no window.
    pub window: u64,
    pub fill: Fill,
}

impl CoverRect {
    fn sane(&self) -> Option<Self> {
        let finite = [self.x, self.y, self.w, self.h]
            .iter()
            .all(|v| v.is_finite());
        (finite && self.w > 0.0 && self.h > 0.0).then(|| Self {
            rounding: if self.rounding.is_finite() {
                self.rounding.max(0.0)
            } else {
                0.0
            },
            ..*self
        })
    }
}

/// A rect as the renderer sees it (with its monitor).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MonitorRect {
    pub monitor: i64,
    pub rect: CoverRect,
}

#[derive(Debug)]
struct Client {
    id: u64,
    name: String,
    rects: Vec<MonitorRect>,
}

#[derive(Debug)]
struct Clients {
    list: Vec<Client>,
    next_id: u64,
}

/// Client id for the old v1 API.
pub const LEGACY_CLIENT: u64 = 1;
/// Max rects per client: protects against a leak in another plugin.
const MAX_RECTS_PER_CLIENT: usize = 4096;

static CLIENTS: Mutex<Clients> = Mutex::new(Clients {
    list: Vec::new(),
    next_id: LEGACY_CLIENT + 1,
});

fn clients() -> MutexGuard<'static, Clients> {
    // a poisoned mutex is harmless here: it only holds data, take it as is
    CLIENTS.lock().unwrap_or_else(|e| e.into_inner())
}

fn client_mut(c: &mut Clients, id: u64) -> Option<&mut Client> {
    c.list.iter_mut().find(|cl| cl.id == id)
}

fn legacy(c: &mut Clients) -> &mut Client {
    if !c.list.iter().any(|cl| cl.id == LEGACY_CLIENT) {
        c.list.push(Client {
            id: LEGACY_CLIENT,
            name: "legacy".into(),
            rects: Vec::new(),
        });
    }
    client_mut(c, LEGACY_CLIENT).expect("legacy client exists")
}

/// New client. The name is for debugging (who left the rects behind).
pub fn register(name: &str) -> u64 {
    let mut c = clients();
    let id = c.next_id;
    c.next_id += 1;
    c.list.push(Client {
        id,
        name: name.chars().take(64).collect(),
        rects: Vec::new(),
    });
    id
}

pub fn unregister(id: u64) {
    clients().list.retain(|cl| cl.id != id);
    gone_list().retain(|g| g.client != id);
}

/// Replace all of the client's rects on a monitor. An empty list clears the monitor.
/// `false` if the client doesn't exist (never registered or already unregistered).
pub fn set_rects(id: u64, monitor: i64, rects: &[CoverRect]) -> bool {
    let mut c = clients();
    let Some(cl) = client_mut(&mut c, id) else {
        return false;
    };
    cl.rects.retain(|r| r.monitor != monitor);
    let room = MAX_RECTS_PER_CLIENT.saturating_sub(cl.rects.len());
    cl.rects.extend(
        rects
            .iter()
            .filter_map(CoverRect::sane)
            .take(room)
            .map(|rect| MonitorRect { monitor, rect }),
    );
    true
}

/// Clear the client's rects on all monitors, keeping it registered.
pub fn clear_client(id: u64) -> bool {
    let mut c = clients();
    match client_mut(&mut c, id) {
        Some(cl) => {
            cl.rects.clear();
            true
        }
        None => false,
    }
}

/// All rects on a monitor from all clients (a copy, so the lock isn't held while drawing).
pub fn for_monitor(monitor: i64) -> Vec<CoverRect> {
    clients()
        .list
        .iter()
        .flat_map(|cl| cl.rects.iter())
        .filter(|r| r.monitor == monitor)
        .map(|r| r.rect)
        .collect()
}

/// Tests that touch the global client list (directly, or through `nsc_shutdown`,
/// which calls `reset`) take this lock: they run in parallel, and a reset in one of
/// them would pull clients out from under another.
#[cfg(test)]
pub(crate) static TEST_LOCK: Mutex<()> = Mutex::new(());

#[cfg(test)]
pub(crate) fn test_lock() -> MutexGuard<'static, ()> {
    TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

/// Reset on plugin unload.
pub fn reset() {
    gone_list().clear();
    let mut c = clients();
    c.list.clear();
    c.next_id = LEGACY_CLIENT + 1;
}

/// For debugging: how many rects each client holds.
pub fn debug_summary() -> Vec<(String, usize)> {
    clients()
        .list
        .iter()
        .map(|cl| (cl.name.clone(), cl.rects.len()))
        .collect()
}

// ---------------------------------------------------------------- API v1

pub fn legacy_clear() {
    legacy(&mut clients()).rects.clear();
}

pub fn legacy_add(monitor: i64, x: f64, y: f64, w: f64, h: f64, rounding: f64) {
    let rect = CoverRect {
        x,
        y,
        w,
        h,
        rounding,
        window: 0,
        fill: Fill::Black,
    };
    let Some(rect) = rect.sane() else { return };
    let mut c = clients();
    let cl = legacy(&mut c);
    if cl.rects.len() < MAX_RECTS_PER_CLIENT {
        cl.rects.push(MonitorRect { monitor, rect });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_clear_extra_rects() {
    let _ = std::panic::catch_unwind(legacy_clear);
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_add_extra_rect(
    monitor_id: i32,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
    rounding: f64,
) {
    let _ = std::panic::catch_unwind(|| legacy_add(i64::from(monitor_id), x, y, w, h, rounding));
}

// ---------------------------------------------------------------- API v2 (C)

/// `noshare_cover_rect` from the public header.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CRect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub rounding: f64,
    pub window: u64,
    pub fill: u32,
}

impl From<&CRect> for CoverRect {
    fn from(r: &CRect) -> Self {
        Self {
            x: r.x,
            y: r.y,
            w: r.w,
            h: r.h,
            rounding: r.rounding,
            window: r.window,
            fill: Fill::from_raw(r.fill),
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_api_version() -> u32 {
    API_VERSION
}

/// # Safety
/// `name` is a NUL-terminated string or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_api_register_client(name: *const std::ffi::c_char) -> u64 {
    std::panic::catch_unwind(|| {
        let name = if name.is_null() {
            String::from("?")
        } else {
            unsafe { std::ffi::CStr::from_ptr(name) }
                .to_string_lossy()
                .into_owned()
        };
        register(&name)
    })
    .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_unregister_client(client: u64) {
    let _ = std::panic::catch_unwind(|| unregister(client));
}

/// # Safety
/// `rects` points to `count` elements (or NULL when `count == 0`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_api_set_rects(
    client: u64,
    monitor_id: i32,
    rects: *const CRect,
    count: usize,
) -> bool {
    std::panic::catch_unwind(|| {
        let slice = if rects.is_null() || count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(rects, count) }
        };
        let converted: Vec<CoverRect> = slice.iter().map(CoverRect::from).collect();
        set_rects(client, i64::from(monitor_id), &converted)
    })
    .unwrap_or(false)
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_clear_client_rects(client: u64) -> bool {
    std::panic::catch_unwind(|| clear_client(client)).unwrap_or(false)
}

// ---------------------------------------------------------------- "going away"

/// Client callback: noshare-cover is unloading. Called from PLUGIN_EXIT after the
/// renderMonitor hook is removed and before the core stops: the client must drop
/// all pointers to our functions and may take over renderMonitor right away.
pub type GoneCb = unsafe extern "C" fn(user: *mut std::ffi::c_void);

struct Gone {
    client: u64,
    cb: GoneCb,
    user: usize,
}

static GONE: Mutex<Vec<Gone>> = Mutex::new(Vec::new());

fn gone_list() -> std::sync::MutexGuard<'static, Vec<Gone>> {
    GONE.lock().unwrap_or_else(|e| e.into_inner())
}

/// # Safety
/// `cb` is a valid function; `user` lives until it's called or unset (cb = NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_api_set_gone_callback(
    client: u64,
    cb: Option<GoneCb>,
    user: *mut std::ffi::c_void,
) -> bool {
    std::panic::catch_unwind(|| {
        if client_mut(&mut clients(), client).is_none() {
            return false;
        }
        let mut g = gone_list();
        g.retain(|x| x.client != client);
        if let Some(cb) = cb {
            g.push(Gone {
                client,
                cb,
                user: user as usize,
            });
        }
        true
    })
    .unwrap_or(false)
}

/// Send "going away" to all clients. The list is taken out before calling:
/// a callback may call our own functions (unregister) without deadlocking.
#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_notify_gone() {
    let list = std::mem::take(&mut *gone_list());
    for g in list {
        let _ = std::panic::catch_unwind(|| unsafe { (g.cb)(g.user as *mut std::ffi::c_void) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn r(x: f64) -> CoverRect {
        CoverRect {
            x,
            y: 0.0,
            w: 10.0,
            h: 10.0,
            rounding: 0.0,
            window: 0,
            fill: Fill::Black,
        }
    }

    // Global state: the whole scenario is one test so parallel tests don't interfere.
    #[test]
    fn api_scenarios() {
        let _serial = test_lock();
        reset();

        // v1 behaves as before
        nsc_api_add_extra_rect(1, 10.0, 10.0, 100.0, 50.0, -3.0);
        nsc_api_add_extra_rect(1, 0.0, 0.0, 0.0, 50.0, 0.0); // empty, dropped
        nsc_api_add_extra_rect(1, f64::NAN, 0.0, 10.0, 10.0, 0.0); // garbage, dropped
        let m1 = for_monitor(1);
        assert_eq!(m1.len(), 1);
        assert_eq!(m1[0].rounding, 0.0);

        // v2: each client has its own set, another client's clear doesn't touch it
        let a = register("gloview");
        let b = register("other");
        assert!(set_rects(a, 1, &[r(1.0), r(2.0)]));
        assert!(set_rects(b, 1, &[r(3.0)]));
        assert_eq!(for_monitor(1).len(), 4);
        nsc_api_clear_extra_rects(); // v1 clear only clears legacy
        assert_eq!(for_monitor(1).len(), 3);

        // set_rects atomically replaces the set on a monitor, other monitors are untouched
        assert!(set_rects(a, 2, &[r(9.0)]));
        assert!(set_rects(a, 1, &[r(5.0)]));
        let xs: Vec<f64> = for_monitor(1).iter().map(|c| c.x).collect();
        assert!(
            xs.contains(&5.0) && !xs.contains(&1.0) && xs.contains(&3.0),
            "{xs:?}"
        );
        assert_eq!(for_monitor(2).len(), 1);

        // window cover fill is passed through
        let cover = CRect {
            x: 0.0,
            y: 0.0,
            w: 5.0,
            h: 5.0,
            rounding: 2.0,
            window: 0xdead,
            fill: 1,
        };
        assert!(unsafe { nsc_api_set_rects(a, 3, &cover, 1) });
        let got = for_monitor(3);
        assert_eq!((got[0].fill, got[0].window), (Fill::WindowCover, 0xdead));
        // unknown fill = black
        let odd = CRect { fill: 77, ..cover };
        assert!(unsafe { nsc_api_set_rects(a, 3, &odd, 1) });
        assert_eq!(for_monitor(3)[0].fill, Fill::Black);

        // an unregistered client holds nothing and can't write
        unregister(a);
        assert!(!set_rects(a, 1, &[r(1.0)]));
        assert_eq!(for_monitor(1).len(), 1); // only b is left
        assert!(clear_client(b));
        assert!(for_monitor(1).is_empty());

        // per-client limit
        let c = register("spammer");
        let many = vec![r(1.0); MAX_RECTS_PER_CLIENT + 100];
        assert!(set_rects(c, 1, &many));
        assert_eq!(for_monitor(1).len(), MAX_RECTS_PER_CLIENT);

        assert_eq!(nsc_api_api_version(), 2);
        reset();
        assert!(for_monitor(1).is_empty());
    }

    #[test]
    fn gone_callbacks_fire_once_and_allow_reentry() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        static HITS: AtomicUsize = AtomicUsize::new(0);
        let _serial = test_lock();
        unsafe extern "C" fn cb(user: *mut std::ffi::c_void) {
            HITS.fetch_add(1, Ordering::SeqCst);
            // the callback calls our functions: must not deadlock
            nsc_api_unregister_client(user as u64);
        }
        let id = register("gone-test");
        assert!(unsafe { nsc_api_set_gone_callback(id, Some(cb), id as *mut _) });
        assert!(!unsafe { nsc_api_set_gone_callback(999_999, Some(cb), std::ptr::null_mut()) });
        nsc_api_notify_gone();
        nsc_api_notify_gone();
        assert_eq!(HITS.load(Ordering::SeqCst), 1);
    }
}
