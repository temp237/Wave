use std::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};
use std::sync::atomic::{fence, Ordering};
use std::thread::{self, sleep};
use std::time::{Duration, Instant};

use windows::core::{w, Result};
use windows::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0};
use windows::Win32::Media::Audio::{
    eConsole, eRender, IAudioClient, IAudioRenderClient, IMMDeviceEnumerator, MMDeviceEnumerator,
    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, WAVEFORMATEX,
};
use windows::Win32::System::Com::{CoCreateInstance, CoInitializeEx, CoTaskMemFree, CLSCTX_ALL, COINIT_MULTITHREADED};
use windows::Win32::System::Memory::{
    MapViewOfFile, OpenFileMappingW, UnmapViewOfFile, FILE_MAP_ALL_ACCESS, MEMORY_MAPPED_VIEW_ADDRESS,
};
use windows::Win32::System::Performance::{QueryPerformanceCounter, QueryPerformanceFrequency};
use windows::Win32::System::SystemInformation::GetTickCount64;
use windows::Win32::System::Threading::{
    AvSetMmThreadCharacteristicsW, CreateEventW, GetCurrentProcess, ProcessPowerThrottling, SetProcessInformation,
    WaitForSingleObject, PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
    PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, PROCESS_POWER_THROTTLING_STATE,
};

const MAGIC: u32 = 0x5241_5657;
const VERSION: u32 = 4;
const CAPACITY: u32 = 1 << 21;
const MASK: i64 = CAPACITY as i64 - 1;
const WAVE_FORMAT_IEEE_FLOAT: u16 = 3;
const BUFFER_DURATION: i64 = 1_000_000;
const STALE_MS: i64 = 1500;

#[repr(C)]
struct Header {
    magic: u32,
    version: u32,
    capacity: u32,
    sample_rate: i32,
    write_position: i64,
    last_write_tick: i64,
    anchor_sequence: i32,
    clock_sequence: i32,
    anchor_frame: i64,
    anchor_time: i64,
    clock_time: i64,
    clock_qpc: i64,
    playing: i32,
    volume: f32,
    reader_tick: i64,
    start_frame: i64,
    start_count: i32,
    reserved: i32,
    heard_frame: i64,
}

const _: () = assert!(std::mem::size_of::<Header>() == 112);
const START_WAIT: Duration = Duration::from_millis(400);
const MAP_SIZE: usize = std::mem::size_of::<Header>() + CAPACITY as usize * 2 * 4;

struct Shared {
    mapping: HANDLE,
    view: *mut Header,
}

impl Shared {
    fn open() -> Option<Shared> {
        unsafe {
            let mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS.0, false, w!("Local\\WaveAudioRelay4")).ok()?;
            let view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, MAP_SIZE);
            if view.Value.is_null() {
                let _ = CloseHandle(mapping);
                return None;
            }
            let shared = Shared { mapping, view: view.Value as *mut Header };
            if shared.valid() { Some(shared) } else { None }
        }
    }

    fn valid(&self) -> bool {
        unsafe {
            read_volatile(addr_of!((*self.view).magic)) == MAGIC
                && read_volatile(addr_of!((*self.view).version)) == VERSION
                && read_volatile(addr_of!((*self.view).capacity)) == CAPACITY
        }
    }

    fn get<T: Copy>(&self, field: *const T) -> T {
        unsafe { read_volatile(field) }
    }

    fn header(&self) -> *const Header {
        self.view
    }

    fn samples(&self) -> *const f32 {
        unsafe { self.view.add(1) as *const f32 }
    }

    fn touch(&self) {
        unsafe { write_volatile(addr_of_mut!((*self.view).reader_tick), GetTickCount64() as i64) }
    }

    fn set_heard(&self, frame: i64) {
        unsafe { write_volatile(addr_of_mut!((*self.view).heard_frame), frame) }
    }

    fn release(&self) {
        unsafe { write_volatile(addr_of_mut!((*self.view).reader_tick), 0) }
    }

    fn anchor(&self) -> Option<(i64, i64)> {
        let h = self.header();
        for _ in 0..4 {
            let before = self.get(unsafe { addr_of!((*h).anchor_sequence) });
            if before & 1 != 0 {
                continue;
            }
            fence(Ordering::Acquire);
            let frame = self.get(unsafe { addr_of!((*h).anchor_frame) });
            let time = self.get(unsafe { addr_of!((*h).anchor_time) });
            fence(Ordering::Acquire);
            if self.get(unsafe { addr_of!((*h).anchor_sequence) }) == before {
                return (before != 0).then_some((frame, time));
            }
        }
        None
    }

    fn clock(&self) -> Option<(i64, i64, bool)> {
        let h = self.header();
        for _ in 0..4 {
            let before = self.get(unsafe { addr_of!((*h).clock_sequence) });
            if before & 1 != 0 {
                continue;
            }
            fence(Ordering::Acquire);
            let time = self.get(unsafe { addr_of!((*h).clock_time) });
            let qpc = self.get(unsafe { addr_of!((*h).clock_qpc) });
            let playing = self.get(unsafe { addr_of!((*h).playing) }) != 0;
            fence(Ordering::Acquire);
            if self.get(unsafe { addr_of!((*h).clock_sequence) }) == before {
                return (before != 0).then_some((time, qpc, playing));
            }
        }
        None
    }
}

impl Drop for Shared {
    fn drop(&mut self) {
        self.release();
        unsafe {
            let _ = UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: self.view as *mut _ });
            let _ = CloseHandle(self.mapping);
        }
    }
}

struct Output {
    client: IAudioClient,
    render: IAudioRenderClient,
    event: HANDLE,
    buffer_frames: u32,
    rate: u32,
    device_id: String,
}

impl Output {
    fn open(enumerator: &IMMDeviceEnumerator, rate: u32) -> Result<Output> {
        unsafe {
            let device = enumerator.GetDefaultAudioEndpoint(eRender, eConsole)?;
            let device_id = device_id(enumerator).unwrap_or_default();
            let client: IAudioClient = device.Activate(CLSCTX_ALL, None)?;
            let format = WAVEFORMATEX {
                wFormatTag: WAVE_FORMAT_IEEE_FLOAT,
                nChannels: 2,
                nSamplesPerSec: rate,
                nAvgBytesPerSec: rate * 8,
                nBlockAlign: 8,
                wBitsPerSample: 32,
                cbSize: 0,
            };
            client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                BUFFER_DURATION,
                0,
                &format,
                None,
            )?;
            let event = CreateEventW(None, false, false, None)?;
            client.SetEventHandle(event)?;
            let buffer_frames = client.GetBufferSize()?;
            let render: IAudioRenderClient = client.GetService()?;
            client.Start()?;
            Ok(Output { client, render, event, buffer_frames, rate, device_id })
        }
    }
}

impl Drop for Output {
    fn drop(&mut self) {
        unsafe {
            let _ = self.client.Stop();
            let _ = CloseHandle(self.event);
        }
    }
}

fn device_id(enumerator: &IMMDeviceEnumerator) -> Option<String> {
    unsafe {
        let device = enumerator.GetDefaultAudioEndpoint(eRender, eConsole).ok()?;
        let id = device.GetId().ok()?;
        let text = id.to_string().ok();
        CoTaskMemFree(Some(id.0 as *const _));
        text
    }
}

struct Player {
    cursor: Option<i64>,
    error: f64,
    volume: f32,
    qpc_frequency: f64,
    offset: i64,
    start_count: i32,
    pending_since: Option<Instant>,
}

impl Player {
    fn now_qpc() -> i64 {
        let mut now = 0i64;
        unsafe {
            let _ = QueryPerformanceCounter(&mut now);
        }
        now
    }

    fn fill(&mut self, shared: &Shared, out: &mut [f32], frames: usize, padding: u32, rate: u32) {
        let h = shared.header();
        let target_volume = shared.get(unsafe { addr_of!((*h).volume) }).clamp(0.0, 1.0);
        let start_volume = self.volume;
        self.volume = target_volume;

        let rate_f = rate as f64;
        let write_position = shared.get(unsafe { addr_of!((*h).write_position) });
        let last_write = shared.get(unsafe { addr_of!((*h).last_write_tick) });
        let stale = unsafe { GetTickCount64() } as i64 - last_write > STALE_MS;

        let desired = match (shared.clock(), shared.anchor()) {
            (Some((clock_time, clock_qpc, true)), Some((anchor_frame, anchor_time))) if !stale => {
                let elapsed = ((Self::now_qpc() - clock_qpc) as f64 / self.qpc_frequency).clamp(0.0, 1.0);
                let seconds = (clock_time - anchor_time) as f64 / 1e7 + elapsed;
                Some((anchor_frame as f64 + seconds * rate_f + padding as f64).round() as i64)
            }
            _ => None,
        };

        let Some(desired) = desired else {
            self.cursor = None;
            self.error = 0.0;
            shared.set_heard(-1);
            out.fill(0.0);
            return;
        };

        let count = shared.get(unsafe { addr_of!((*h).start_count) });
        if count & 1 == 1 {
            let since = *self.pending_since.get_or_insert_with(Instant::now);
            if since.elapsed() < START_WAIT {
                self.cursor = None;
                shared.set_heard(-1);
                out.fill(0.0);
                return;
            }
        } else {
            self.pending_since = None;
        }

        let mut cursor = if count & 1 == 0 && count != self.start_count {
            self.start_count = count;
            self.error = 0.0;
            let frame = shared.get(unsafe { addr_of!((*h).start_frame) });
            if frame >= 0 && frame < write_position && write_position - frame < CAPACITY as i64 {
                self.offset = frame - desired;
                frame
            } else {
                self.offset = 0;
                desired
            }
        } else {
            let target = desired + self.offset;
            match self.cursor {
                Some(c) if ((c - target) as f64).abs() <= 0.25 * rate_f => c,
                _ => {
                    self.error = 0.0;
                    target
                }
            }
        };
        let desired = desired + self.offset;
        self.error = self.error * 0.95 + (cursor - desired) as f64 * 0.05;
        let threshold = 0.01 * rate_f;
        if self.error > threshold {
            cursor -= 1;
            self.error -= 1.0;
        } else if self.error < -threshold {
            cursor += 1;
            self.error += 1.0;
        }

        shared.set_heard(cursor - padding as i64);
        let samples = shared.samples();
        let oldest = write_position - CAPACITY as i64 + 1;
        let step = if frames > 0 { (target_volume - start_volume) / frames as f32 } else { 0.0 };
        for i in 0..frames {
            let position = cursor + i as i64;
            let gain = start_volume + step * i as f32;
            let (left, right) = if position >= oldest.max(0) && position < write_position {
                let index = (position & MASK) as usize;
                unsafe { (read_volatile(samples.add(index * 2)), read_volatile(samples.add(index * 2 + 1))) }
            } else {
                (0.0, 0.0)
            };
            out[i * 2] = left * gain;
            out[i * 2 + 1] = right * gain;
        }
        self.cursor = Some(cursor + frames as i64);
    }
}

fn run(enumerator: &IMMDeviceEnumerator, shared: &Shared, player: &mut Player) {
    let rate = shared.get(unsafe { addr_of!((*shared.header()).sample_rate) });
    if rate <= 0 {
        shared.touch();
        sleep(Duration::from_millis(50));
        return;
    }
    let output = match Output::open(enumerator, rate as u32) {
        Ok(output) => output,
        Err(e) => {
            eprintln!("audio relay: couldn't open the speakers: {e}");
            sleep(Duration::from_secs(1));
            return;
        }
    };
    player.cursor = None;
    let mut last_device_check = Instant::now();
    loop {
        if !shared.valid() {
            return;
        }
        if shared.get(unsafe { addr_of!((*shared.header()).sample_rate) }) as u32 != output.rate {
            return;
        }
        if last_device_check.elapsed() > Duration::from_secs(1) {
            last_device_check = Instant::now();
            if device_id(enumerator).is_some_and(|id| id != output.device_id) {
                return;
            }
        }
        unsafe {
            if WaitForSingleObject(output.event, 100) != WAIT_OBJECT_0 {
                if output.client.GetCurrentPadding().is_err() {
                    return;
                }
            }
            let Ok(padding) = output.client.GetCurrentPadding() else { return };
            let available = output.buffer_frames.saturating_sub(padding);
            shared.touch();
            if available == 0 {
                continue;
            }
            let Ok(data) = output.render.GetBuffer(available) else { return };
            let out = std::slice::from_raw_parts_mut(data as *mut f32, available as usize * 2);
            player.fill(shared, out, available as usize, padding, output.rate);
            if output.render.ReleaseBuffer(available, 0).is_err() {
                return;
            }
        }
    }
}

pub fn start() {
    thread::spawn(|| {
        unsafe {
            let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
            let mut task = 0u32;
            let _ = AvSetMmThreadCharacteristicsW(w!("Pro Audio"), &mut task);
            let throttling = PROCESS_POWER_THROTTLING_STATE {
                Version: PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                ControlMask: PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
                StateMask: 0,
            };
            let _ = SetProcessInformation(
                GetCurrentProcess(),
                ProcessPowerThrottling,
                &throttling as *const _ as *const _,
                std::mem::size_of::<PROCESS_POWER_THROTTLING_STATE>() as u32,
            );
        }
        let enumerator: IMMDeviceEnumerator = match unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL) } {
            Ok(e) => e,
            Err(e) => {
                eprintln!("audio relay: no audio devices: {e}");
                return;
            }
        };
        let mut frequency = 0i64;
        unsafe {
            let _ = QueryPerformanceFrequency(&mut frequency);
        }
        let mut player = Player {
            cursor: None,
            error: 0.0,
            volume: 1.0,
            qpc_frequency: frequency.max(1) as f64,
            offset: 0,
            start_count: 0,
            pending_since: None,
        };
        loop {
            let Some(shared) = Shared::open() else {
                sleep(Duration::from_millis(500));
                continue;
            };
            let count = shared.get(unsafe { addr_of!((*shared.header()).start_count) });
            player.start_count = count & !1;
            player.offset = 0;
            while shared.valid() {
                run(&enumerator, &shared, &mut player);
            }
        }
    });
}
