use std::cell::RefCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::OnceLock;
use std::time::Duration;

use tauri::{App, AppHandle, Emitter, Manager};
use windows::core::w;
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    CreateBitmap, CreateDIBSection, DeleteObject, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, DIB_RGB_COLORS, HGDIOBJ,
};
use windows::Win32::System::Com::{CoCreateInstance, CLSCTX_INPROC_SERVER};
use windows::Win32::UI::Shell::{
    DefSubclassProc, ITaskbarList3, SetWindowSubclass, TaskbarList, THBF_ENABLED, THBN_CLICKED, THB_FLAGS, THB_ICON,
    THB_TOOLTIP, THUMBBUTTON,
};
use windows::Win32::UI::WindowsAndMessaging::{CreateIconIndirect, RegisterWindowMessageW, HICON, ICONINFO, WM_COMMAND};

use crate::{request, Backend, MessageType, Value};

const ID_PREVIOUS: u32 = 1;
const ID_PLAY: u32 = 2;
const ID_NEXT: u32 = 3;
const SUBCLASS_ID: usize = 0x5741_5645;
const ICON_SIZE: i32 = 32;

#[derive(Clone, Copy)]
enum Glyph {
    Previous,
    Play,
    Pause,
    Next,
}

static APP: OnceLock<AppHandle> = OnceLock::new();
static PLAYING: AtomicBool = AtomicBool::new(false);
static ADDED: AtomicBool = AtomicBool::new(false);
static BUTTON_CREATED_MSG: AtomicU32 = AtomicU32::new(0);

thread_local! {
    static TASKBAR: RefCell<Option<ITaskbarList3>> = const { RefCell::new(None) };
    static ICONS: RefCell<Option<[HICON; 4]>> = const { RefCell::new(None) };
    static WINDOW: RefCell<Option<HWND>> = const { RefCell::new(None) };
}

pub fn setup(app: &App) -> Result<(), Box<dyn std::error::Error>> {
    let window = app.get_webview_window("main").ok_or("main window not found")?;
    let hwnd = window.hwnd()?;
    let _ = APP.set(app.handle().clone());
    WINDOW.with(|w| *w.borrow_mut() = Some(hwnd));
    unsafe {
        BUTTON_CREATED_MSG.store(RegisterWindowMessageW(w!("TaskbarButtonCreated")), Ordering::Relaxed);
        let _ = SetWindowSubclass(hwnd, Some(subclass_proc), SUBCLASS_ID, 0);
    }
    add_buttons(hwnd);
    Ok(())
}

pub fn set_playing(playing: bool) {
    if PLAYING.swap(playing, Ordering::Relaxed) == playing {
        return;
    }
    if let Some(app) = APP.get() {
        let _ = app.run_on_main_thread(update_play_button);
    }
}

fn inside_triangle(px: f32, py: f32, a: (f32, f32), b: (f32, f32), c: (f32, f32)) -> bool {
    let side = |p: (f32, f32), q: (f32, f32)| (px - q.0) * (p.1 - q.1) - (p.0 - q.0) * (py - q.1);
    let (d1, d2, d3) = (side(a, b), side(b, c), side(c, a));
    let negative = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
    let positive = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
    !(negative && positive)
}

fn inside_rect(px: f32, py: f32, x0: f32, y0: f32, x1: f32, y1: f32) -> bool {
    px >= x0 && px <= x1 && py >= y0 && py <= y1
}

fn covered(glyph: Glyph, x: f32, y: f32) -> bool {
    match glyph {
        Glyph::Play => inside_triangle(x, y, (11.0, 8.0), (11.0, 24.0), (24.0, 16.0)),
        Glyph::Pause => inside_rect(x, y, 9.0, 8.0, 14.0, 24.0) || inside_rect(x, y, 18.0, 8.0, 23.0, 24.0),
        Glyph::Previous => inside_rect(x, y, 8.0, 9.0, 11.0, 23.0) || inside_triangle(x, y, (24.0, 9.0), (24.0, 23.0), (12.0, 16.0)),
        Glyph::Next => inside_rect(x, y, 21.0, 9.0, 24.0, 23.0) || inside_triangle(x, y, (8.0, 9.0), (8.0, 23.0), (20.0, 16.0)),
    }
}

fn make_icon(glyph: Glyph) -> HICON {
    unsafe {
        let mut info = BITMAPINFO::default();
        info.bmiHeader = BITMAPINFOHEADER {
            biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: ICON_SIZE,
            biHeight: -ICON_SIZE,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB.0,
            ..Default::default()
        };
        let mut bits: *mut c_void = std::ptr::null_mut();
        let Ok(color) = CreateDIBSection(None, &info, DIB_RGB_COLORS, &mut bits, None, 0) else {
            return HICON::default();
        };
        let pixels = std::slice::from_raw_parts_mut(bits as *mut u32, (ICON_SIZE * ICON_SIZE) as usize);
        const SAMPLES: i32 = 4;
        for y in 0..ICON_SIZE {
            for x in 0..ICON_SIZE {
                let mut hits = 0;
                for sy in 0..SAMPLES {
                    for sx in 0..SAMPLES {
                        let px = x as f32 + (sx as f32 + 0.5) / SAMPLES as f32;
                        let py = y as f32 + (sy as f32 + 0.5) / SAMPLES as f32;
                        if covered(glyph, px, py) {
                            hits += 1;
                        }
                    }
                }
                let alpha = (hits * 255 / (SAMPLES * SAMPLES)) as u32;
                pixels[(y * ICON_SIZE + x) as usize] = (alpha << 24) | 0x00FF_FFFF;
            }
        }
        let mask = CreateBitmap(ICON_SIZE, ICON_SIZE, 1, 1, None);
        let icon_info = ICONINFO { fIcon: true.into(), xHotspot: 0, yHotspot: 0, hbmMask: mask, hbmColor: color };
        let icon = CreateIconIndirect(&icon_info).unwrap_or_default();
        let _ = DeleteObject(HGDIOBJ(color.0));
        let _ = DeleteObject(HGDIOBJ(mask.0));
        icon
    }
}

fn icon(glyph: Glyph) -> HICON {
    ICONS.with(|icons| {
        let mut icons = icons.borrow_mut();
        let set = icons.get_or_insert_with(|| {
            [make_icon(Glyph::Previous), make_icon(Glyph::Play), make_icon(Glyph::Pause), make_icon(Glyph::Next)]
        });
        set[glyph as usize]
    })
}

fn button(id: u32, glyph: Glyph, tip: &str) -> THUMBBUTTON {
    let mut b = THUMBBUTTON {
        dwMask: THB_ICON | THB_TOOLTIP | THB_FLAGS,
        iId: id,
        hIcon: icon(glyph),
        dwFlags: THBF_ENABLED,
        ..Default::default()
    };
    for (slot, unit) in b.szTip.iter_mut().zip(tip.encode_utf16().take(259)) {
        *slot = unit;
    }
    b
}

fn play_button() -> THUMBBUTTON {
    if PLAYING.load(Ordering::Relaxed) {
        button(ID_PLAY, Glyph::Pause, "Pause")
    } else {
        button(ID_PLAY, Glyph::Play, "Play")
    }
}

fn taskbar() -> Option<ITaskbarList3> {
    TASKBAR.with(|t| {
        let mut t = t.borrow_mut();
        if t.is_none() {
            let created: windows::core::Result<ITaskbarList3> = unsafe { CoCreateInstance(&TaskbarList, None, CLSCTX_INPROC_SERVER) };
            if let Ok(list) = created {
                if unsafe { list.HrInit() }.is_ok() {
                    *t = Some(list);
                }
            }
        }
        t.clone()
    })
}

fn add_buttons(hwnd: HWND) {
    let Some(list) = taskbar() else { return };
    let buttons = [
        button(ID_PREVIOUS, Glyph::Previous, "Previous"),
        play_button(),
        button(ID_NEXT, Glyph::Next, "Next"),
    ];
    if unsafe { list.ThumbBarAddButtons(hwnd, &buttons) }.is_ok() {
        ADDED.store(true, Ordering::Relaxed);
    }
}

fn update_play_button() {
    if !ADDED.load(Ordering::Relaxed) {
        return;
    }
    let Some(hwnd) = WINDOW.with(|w| *w.borrow()) else { return };
    if let Some(list) = taskbar() {
        let _ = unsafe { list.ThumbBarUpdateButtons(hwnd, &[play_button()]) };
    }
}

fn on_click(id: u32) {
    let Some(app) = APP.get().cloned() else { return };
    std::thread::spawn(move || {
        let backend = app.state::<Backend>();
        let timeout = Duration::from_secs(10);
        match id {
            ID_PREVIOUS => {
                let _ = request(&backend, MessageType::QueuePrevious, &[], timeout);
            }
            ID_NEXT => {
                let _ = request(&backend, MessageType::QueueNext, &[], timeout);
            }
            ID_PLAY => {
                let kind = if PLAYING.load(Ordering::Relaxed) { MessageType::Pause } else { MessageType::Resume };
                let resumed = matches!(request(&backend, kind, &[], timeout).as_deref(), Ok([Value::Bool(true), ..]));
                if kind == MessageType::Resume && !resumed {
                    let _ = app.emit("wave-taskbar-play", ());
                }
            }
            _ => {}
        }
    });
}

unsafe extern "system" fn subclass_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    _id: usize,
    _data: usize,
) -> LRESULT {
    let created = BUTTON_CREATED_MSG.load(Ordering::Relaxed);
    if created != 0 && msg == created {
        ADDED.store(false, Ordering::Relaxed);
        add_buttons(hwnd);
    } else if msg == WM_COMMAND && ((wparam.0 >> 16) & 0xFFFF) as u32 == THBN_CLICKED {
        on_click((wparam.0 & 0xFFFF) as u32);
        return LRESULT(0);
    }
    unsafe { DefSubclassProc(hwnd, msg, wparam, lparam) }
}
