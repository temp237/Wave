mod audio_relay;
mod bits;
mod launcher;
#[cfg(windows)]
mod file_drop;
#[cfg(windows)]
mod taskbar;

use bits::{BitReader, BitWriter};
use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::collections::{HashMap, VecDeque};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager};

#[tauri::command]
fn greet(name: &str) -> String {
    format!("Hello, {}! You've been greeted from Rust!", name)
}

#[derive(Debug, Clone, Copy, PartialEq, serde::Deserialize)]
#[repr(u8)]
pub(crate) enum MessageType {
    Debug = 0,
    ImportFile = 1,
    ImportFolder = 2,
    GetFileTree = 3,
    Play = 4,
    Pause = 5,
    Resume = 6,
    Seek = 7,
    SetVolume = 8,
    Stop = 9,
    PlayerState = 10,
    GetCover = 11,
    GetPlaylists = 12,
    CreatePlaylist = 13,
    DeletePlaylist = 14,
    RenamePlaylist = 15,
    AddToPlaylist = 16,
    RemoveFromPlaylist = 17,
    GetCovers = 18,
    QueueSet = 19,
    QueueNext = 20,
    QueuePrevious = 21,
    QueueJump = 22,
    QueueRemove = 23,
    QueuePlayNext = 24,
    QueueAdd = 25,
    QueueClear = 26,
    SetShuffle = 27,
    SetRepeat = 28,
    QueueState = 29,
    GetQueue = 30,
    GetSettings = 31,
    SetEqualizer = 32,
    ClearCache = 33,
    SetDiscordPresence = 34,
    RemoveFromLibrary = 35,
    SetInterface = 36,
    GetPlayerState = 37,
    SetRunOnStartup = 38,
    SetMicRelay = 39,
    ClearLogs = 40,
    SetDeEsser = 41,
    SetNormalize = 42,
    GetNormalize = 43,
    GetLyrics = 44,
    SetVisualizer = 45,
    Spectrum = 46,
    GetArtwork = 47,
    ClearLyrics = 48,
}

// Tag order must match the std::variant in backend Messages.hpp
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum Value {
    Bool(bool),
    U8(u8),
    I8(i8),
    U16(u16),
    I16(i16),
    U32(u32),
    I32(i32),
    U64(u64),
    I64(i64),
    F32(f32),
    F64(f64),
    Str(String),
}

const TAG_BITS: u32 = 4;

impl Value {
    fn write(&self, w: &mut BitWriter) {
        match self {
            Value::Bool(v) => { w.write_bits(0, TAG_BITS); w.write_bool(*v) }
            Value::U8(v) => { w.write_bits(1, TAG_BITS); w.write_u8(*v) }
            Value::I8(v) => { w.write_bits(2, TAG_BITS); w.write_i8(*v) }
            Value::U16(v) => { w.write_bits(3, TAG_BITS); w.write_u16(*v) }
            Value::I16(v) => { w.write_bits(4, TAG_BITS); w.write_i16(*v) }
            Value::U32(v) => { w.write_bits(5, TAG_BITS); w.write_u32(*v) }
            Value::I32(v) => { w.write_bits(6, TAG_BITS); w.write_i32(*v) }
            Value::U64(v) => { w.write_bits(7, TAG_BITS); w.write_u64(*v) }
            Value::I64(v) => { w.write_bits(8, TAG_BITS); w.write_i64(*v) }
            Value::F32(v) => { w.write_bits(9, TAG_BITS); w.write_f32(*v) }
            Value::F64(v) => { w.write_bits(10, TAG_BITS); w.write_f64(*v) }
            Value::Str(v) => { w.write_bits(11, TAG_BITS); w.write_string(v) }
        }
    }

    fn read(r: &mut BitReader) -> Option<Value> {
        Some(match r.read_bits(TAG_BITS)? {
            0 => Value::Bool(r.read_bool()?),
            1 => Value::U8(r.read_u8()?),
            2 => Value::I8(r.read_i8()?),
            3 => Value::U16(r.read_u16()?),
            4 => Value::I16(r.read_i16()?),
            5 => Value::U32(r.read_u32()?),
            6 => Value::I32(r.read_i32()?),
            7 => Value::U64(r.read_u64()?),
            8 => Value::I64(r.read_i64()?),
            9 => Value::F32(r.read_f32()?),
            10 => Value::F64(r.read_f64()?),
            11 => Value::Str(r.read_string()?),
            _ => return None,
        })
    }
}

fn debug_values(data: String) -> Vec<Value> {
    vec![
        Value::Bool(true),
        Value::Bool(false),
        Value::U8(0),
        Value::U8(u8::MAX),
        Value::I8(i8::MIN),
        Value::I8(-1),
        Value::U16(u16::MAX),
        Value::I16(-12345),
        Value::U32(4_000_000_000),
        Value::I32(i32::MIN),
        Value::I32(i32::MAX),
        Value::U64(u64::MAX),
        Value::U64(0x0123_4567_89AB_CDEF),
        Value::I64(i64::MIN),
        Value::I64(-9_007_199_254_740_993),
        Value::F32(3.14159),
        Value::F32(-0.0),
        Value::F32(f32::MAX),
        Value::F64(-2.718281828459045),
        Value::F64(1e-300),
        Value::F64(f64::INFINITY),
        Value::Str(String::new()),
        Value::Str("Wave ♪ ünïcödé".into()),
        Value::Str(data),
        Value::Bool(true),
    ]
}

fn encode(message_type: MessageType, values: &[Value]) -> Vec<u8> {
    let mut w = BitWriter::new();
    w.write_u8(message_type as u8);
    w.write_u16(values.len() as u16);
    for v in values {
        v.write(&mut w);
    }
    w.into_bytes()
}

fn decode(bytes: &[u8]) -> Option<(u8, Vec<Value>)> {
    let mut r = BitReader::new(bytes);
    let message_type = r.read_u8()?;
    let count = r.read_u16()?;
    let values = (0..count).map(|_| Value::read(&mut r)).collect::<Option<Vec<_>>>()?;
    Some((message_type, values))
}

// Frame: u32 little-endian length, then the payload
fn write_frame(stream: &mut TcpStream, payload: &[u8]) -> io::Result<()> {
    stream.write_all(&(payload.len() as u32).to_le_bytes())?;
    stream.write_all(payload)
}

fn read_frame(stream: &mut TcpStream) -> io::Result<Vec<u8>> {
    let mut len = [0u8; 4];
    stream.read_exact(&mut len)?;
    let len = u32::from_le_bytes(len) as usize;
    if len > 16 * 1024 * 1024 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "frame too large"));
    }
    let mut payload = vec![0u8; len];
    stream.read_exact(&mut payload)?;
    Ok(payload)
}

type Waiters = HashMap<u8, VecDeque<(u64, Sender<Vec<Value>>)>>;

struct Connection {
    writer: Mutex<TcpStream>,
    waiters: Mutex<Waiters>,
    next_id: AtomicU64,
}

pub struct Backend {
    connection: RwLock<Option<Arc<Connection>>>,
    status: Mutex<&'static str>,
}

impl Backend {
    fn new() -> Self {
        Self { connection: RwLock::new(None), status: Mutex::new(launcher::STARTING) }
    }

    fn current(&self) -> Option<Arc<Connection>> {
        self.connection.read().ok()?.clone()
    }

    fn attach(&self, stream: TcpStream) -> Option<TcpStream> {
        let reader = stream.try_clone().ok()?;
        let conn = Arc::new(Connection {
            writer: Mutex::new(stream),
            waiters: Mutex::new(HashMap::new()),
            next_id: AtomicU64::new(0),
        });
        *self.connection.write().ok()? = Some(conn);
        Some(reader)
    }

    fn detach(&self) {
        let old = self.connection.write().ok().and_then(|mut c| c.take());
        if let Some(conn) = old {
            if let Ok(mut waiters) = conn.waiters.lock() {
                waiters.clear();
            }
        }
    }

    fn set_status(&self, status: &'static str) {
        if let Ok(mut s) = self.status.lock() {
            *s = status;
        }
    }

    fn status(&self) -> &'static str {
        self.status.lock().map(|s| *s).unwrap_or(launcher::OFFLINE)
    }
}

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct PlayerEvent {
    status: String,
    file_id: String,
    position: f64,
    duration: f64,
}

fn read_loop(app: &AppHandle, mut reader: TcpStream) {
    let backend = app.state::<Backend>();
    loop {
        let frame = match read_frame(&mut reader) {
            Ok(frame) => frame,
            Err(e) => {
                eprintln!("backend connection closed: {e}");
                break;
            }
        };
        let Some((message_type, values)) = decode(&frame) else {
            eprintln!("malformed frame from the backend");
            continue;
        };
        if message_type == MessageType::PlayerState as u8 {
            if let [Value::Str(status), Value::Str(file_id), Value::F64(position), Value::F64(duration), ..] = values.as_slice() {
                #[cfg(windows)]
                taskbar::set_playing(status == "playing" || status == "loading");
                let _ = app.emit("wave-player", PlayerEvent {
                    status: status.clone(),
                    file_id: file_id.clone(),
                    position: *position,
                    duration: *duration,
                });
            }
            continue;
        }
        if message_type == MessageType::Spectrum as u8 {
            let bands: Vec<u8> = values.iter().filter_map(|v| if let Value::U8(b) = v { Some(*b) } else { None }).collect();
            let _ = app.emit("wave-spectrum", bands);
            continue;
        }
        if message_type == MessageType::QueueState as u8 {
            if let [Value::Str(json), ..] = values.as_slice() {
                if let Ok(state) = serde_json::from_str::<serde_json::Value>(json) {
                    let _ = app.emit("wave-queue", state);
                }
            }
            continue;
        }
        let Some(conn) = backend.current() else { break };
        let waiter = conn.waiters.lock().ok().and_then(|mut w| w.get_mut(&message_type)?.pop_front());
        if let Some((_, reply)) = waiter {
            let _ = reply.send(values);
        }
    }
}

pub(crate) fn request(backend: &Backend, message_type: MessageType, values: &[Value], timeout: Duration) -> Result<Vec<Value>, String> {
    let conn = backend.current().ok_or("the backend isn't connected yet")?;
    let id = conn.next_id.fetch_add(1, Ordering::Relaxed);
    let (tx, rx) = mpsc::channel();
    let forget = || {
        if let Ok(mut waiters) = conn.waiters.lock() {
            if let Some(queue) = waiters.get_mut(&(message_type as u8)) {
                queue.retain(|(waiter, _)| *waiter != id);
            }
        }
    };

    {
        let mut writer = conn.writer.lock().map_err(|e| e.to_string())?;
        conn.waiters
            .lock()
            .map_err(|e| e.to_string())?
            .entry(message_type as u8)
            .or_default()
            .push_back((id, tx));
        if let Err(e) = write_frame(&mut writer, &encode(message_type, values)) {
            forget();
            return Err(format!("send failed: {e}"));
        }
    }

    match rx.recv_timeout(timeout) {
        Ok(received) => Ok(received),
        Err(RecvTimeoutError::Timeout) => {
            forget();
            Err(format!("no reply to {message_type:?}"))
        }
        Err(RecvTimeoutError::Disconnected) => Err("lost the connection to the backend".into()),
    }
}

fn expect_ok(reply: Vec<Value>) -> Result<bool, String> {
    match reply.as_slice() {
        [Value::Bool(ok), ..] => Ok(*ok),
        _ => Err("unexpected reply from the backend".into()),
    }
}

#[tauri::command]
async fn send_message(
    backend: tauri::State<'_, Backend>,
    message_type: MessageType,
    data: String,
) -> Result<String, String> {
    let sent = match message_type {
        MessageType::Debug => debug_values(data),
        _ => return Err("send_message only supports Debug".into()),
    };
    let received = request(&backend, message_type, &sent, Duration::from_secs(2))?;
    if received != sent {
        return Err(format!("mismatch\nsent:     {sent:?}\nreceived: {received:?}"));
    }
    Ok(format!("Debug round trip OK: {} values", sent.len()))
}

#[derive(serde::Serialize)]
struct ImportedFile {
    path: String,
    id: Option<String>,
}

#[tauri::command]
async fn import_files(
    backend: tauri::State<'_, Backend>,
    paths: Vec<String>,
    folder: Option<String>,
) -> Result<Vec<ImportedFile>, String> {
    let mut results = Vec::with_capacity(paths.len());
    for path in paths {
        let mut values = vec![Value::Str(path.clone())];
        if let Some(folder) = &folder {
            values.push(Value::Str(folder.clone()));
        }
        let reply = request(&backend, MessageType::ImportFile, &values, Duration::from_secs(30))?;
        let id = match reply.as_slice() {
            [Value::Bool(true), Value::Str(id), ..] => Some(id.clone()),
            _ => None,
        };
        results.push(ImportedFile { path, id });
    }
    Ok(results)
}

#[tauri::command]
async fn import_folder(backend: tauri::State<'_, Backend>, path: String) -> Result<u32, String> {
    let reply = request(&backend, MessageType::ImportFolder, &[Value::Str(path)], Duration::from_secs(600))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::U32(count), ..] => Ok(*count),
        [Value::Bool(false), ..] => Err("the backend couldn't import that folder".into()),
        _ => Err("unexpected reply from the backend".into()),
    }
}

#[tauri::command]
async fn get_file_tree(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    let reply = request(&backend, MessageType::GetFileTree, &[], Duration::from_secs(600))?;
    match reply.as_slice() {
        [Value::Str(json), ..] => serde_json::from_str(json).map_err(|e| format!("bad file tree JSON: {e}")),
        _ => Err("unexpected reply from the backend".into()),
    }
}

#[tauri::command]
async fn get_cover(backend: tauri::State<'_, Backend>, id: String) -> Result<Option<String>, String> {
    let reply = request(&backend, MessageType::GetCover, &[Value::Str(id)], Duration::from_secs(10))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::Str(mime), Value::Str(data), ..] => Ok(Some(format!("data:{mime};base64,{data}"))),
        _ => Ok(None),
    }
}

fn strings(first: String, rest: Vec<String>) -> Vec<Value> {
    std::iter::once(first).chain(rest).map(Value::Str).collect()
}

#[tauri::command]
async fn get_playlists(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    let reply = request(&backend, MessageType::GetPlaylists, &[], Duration::from_secs(10))?;
    match reply.as_slice() {
        [Value::Str(json), ..] => serde_json::from_str(json).map_err(|e| format!("bad playlists JSON: {e}")),
        _ => Err("unexpected reply from the backend".into()),
    }
}

#[tauri::command]
async fn create_playlist(
    backend: tauri::State<'_, Backend>,
    name: String,
    track_ids: Vec<String>,
) -> Result<serde_json::Value, String> {
    let reply = request(&backend, MessageType::CreatePlaylist, &strings(name, track_ids), Duration::from_secs(10))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::Str(json), ..] => serde_json::from_str(json).map_err(|e| format!("bad playlist JSON: {e}")),
        _ => Err("the backend couldn't create the playlist".into()),
    }
}

#[tauri::command]
async fn rename_playlist(backend: tauri::State<'_, Backend>, id: String, name: String) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::RenamePlaylist, &strings(id, vec![name]), Duration::from_secs(10))?)
}

#[tauri::command]
async fn delete_playlist(backend: tauri::State<'_, Backend>, id: String) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::DeletePlaylist, &strings(id, vec![]), Duration::from_secs(10))?)
}

#[tauri::command]
async fn add_to_playlist(backend: tauri::State<'_, Backend>, id: String, track_ids: Vec<String>) -> Result<u32, String> {
    let reply = request(&backend, MessageType::AddToPlaylist, &strings(id, track_ids), Duration::from_secs(10))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::U32(added), ..] => Ok(*added),
        _ => Err("the backend couldn't update the playlist".into()),
    }
}

#[tauri::command]
async fn remove_from_playlist(backend: tauri::State<'_, Backend>, id: String, track_ids: Vec<String>) -> Result<u32, String> {
    let reply = request(&backend, MessageType::RemoveFromPlaylist, &strings(id, track_ids), Duration::from_secs(10))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::U32(removed), ..] => Ok(*removed),
        _ => Err("the backend couldn't update the playlist".into()),
    }
}

#[tauri::command]
async fn get_covers(
    backend: tauri::State<'_, Backend>,
    ids: Vec<String>,
) -> Result<std::collections::HashMap<String, String>, String> {
    if ids.is_empty() {
        return Ok(Default::default());
    }
    let values: Vec<Value> = ids.into_iter().map(Value::Str).collect();
    let reply = request(&backend, MessageType::GetCovers, &values, Duration::from_secs(60))?;
    let mut covers = std::collections::HashMap::new();
    for chunk in reply.chunks(3) {
        if let [Value::Str(id), Value::Str(mime), Value::Str(data)] = chunk {
            covers.insert(id.clone(), format!("data:{mime};base64,{data}"));
        }
    }
    Ok(covers)
}

fn queue_state(reply: Vec<Value>) -> Result<serde_json::Value, String> {
    match reply.as_slice() {
        [Value::Str(json), ..] => serde_json::from_str(json).map_err(|e| format!("bad queue JSON: {e}")),
        _ => Err("unexpected reply from the backend".into()),
    }
}

#[tauri::command]
async fn get_settings(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    queue_state(request(&backend, MessageType::GetSettings, &[], Duration::from_secs(10))?)
}

#[tauri::command]
async fn set_equalizer(backend: tauri::State<'_, Backend>, settings: serde_json::Value) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetEqualizer, &[Value::Str(settings.to_string())], Duration::from_secs(5))?)
}

#[tauri::command]
async fn set_deesser(backend: tauri::State<'_, Backend>, settings: serde_json::Value) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetDeEsser, &[Value::Str(settings.to_string())], Duration::from_secs(5))?)
}

#[tauri::command]
async fn remove_from_library(backend: tauri::State<'_, Backend>, ids: Vec<String>) -> Result<u32, String> {
    let values: Vec<Value> = ids.into_iter().map(Value::Str).collect();
    let reply = request(&backend, MessageType::RemoveFromLibrary, &values, Duration::from_secs(120))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::U32(removed), ..] => Ok(*removed),
        _ => Err("the backend couldn't update the library".into()),
    }
}

#[tauri::command]
async fn get_player_state(backend: tauri::State<'_, Backend>) -> Result<PlayerEvent, String> {
    let reply = request(&backend, MessageType::GetPlayerState, &[], Duration::from_secs(5))?;
    match reply.as_slice() {
        [Value::Str(status), Value::Str(file_id), Value::F64(position), Value::F64(duration), ..] => Ok(PlayerEvent {
            status: status.clone(),
            file_id: file_id.clone(),
            position: *position,
            duration: *duration,
        }),
        _ => Err("unexpected reply from the backend".into()),
    }
}

#[tauri::command]
fn get_backend_status(backend: tauri::State<'_, Backend>) -> &'static str {
    backend.status()
}

#[tauri::command]
async fn set_interface(backend: tauri::State<'_, Backend>, settings: serde_json::Value) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetInterface, &[Value::Str(settings.to_string())], Duration::from_secs(5))?)
}

#[tauri::command]
async fn set_discord_presence(backend: tauri::State<'_, Backend>, enabled: bool) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetDiscordPresence, &[Value::Bool(enabled)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn set_normalize(backend: tauri::State<'_, Backend>, enabled: bool) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetNormalize, &[Value::Bool(enabled)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn get_normalize(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    queue_state(request(&backend, MessageType::GetNormalize, &[], Duration::from_secs(10))?)
}

#[tauri::command]
async fn get_lyrics(backend: tauri::State<'_, Backend>, id: String) -> Result<serde_json::Value, String> {
    queue_state(request(&backend, MessageType::GetLyrics, &[Value::Str(id)], Duration::from_secs(10))?)
}

#[tauri::command]
async fn set_visualizer(backend: tauri::State<'_, Backend>, enabled: bool) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetVisualizer, &[Value::Bool(enabled)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn get_artwork(backend: tauri::State<'_, Backend>, id: String) -> Result<Option<serde_json::Value>, String> {
    let reply = request(&backend, MessageType::GetArtwork, &[Value::Str(id)], Duration::from_secs(20))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::Str(mime), Value::Str(data), rest @ ..] => {
            let color = match rest.first() {
                Some(Value::U32(c)) => Some(*c),
                _ => None,
            };
            Ok(Some(serde_json::json!({ "url": format!("data:{mime};base64,{data}"), "color": color })))
        }
        _ => Ok(None),
    }
}

#[tauri::command]
async fn set_run_on_startup(backend: tauri::State<'_, Backend>, enabled: bool) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetRunOnStartup, &[Value::Bool(enabled)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn set_mic_relay(backend: tauri::State<'_, Backend>, enabled: bool) -> Result<bool, String> {
    let reply = request(&backend, MessageType::SetMicRelay, &[Value::Bool(enabled)], Duration::from_secs(180))?;
    match reply.as_slice() {
        [Value::Bool(true), ..] => Ok(true),
        [Value::Bool(false), Value::Str(error), ..] => Err(error.clone()),
        _ => Err("unexpected reply from the backend".into()),
    }
}

#[tauri::command]
async fn clear_cache(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    let reply = request(&backend, MessageType::ClearCache, &[], Duration::from_secs(600))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::Str(json), ..] => serde_json::from_str(json).map_err(|e| format!("bad reply JSON: {e}")),
        _ => Err("the backend couldn't clear the cache".into()),
    }
}

#[tauri::command]
async fn clear_lyrics(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    let reply = request(&backend, MessageType::ClearLyrics, &[], Duration::from_secs(60))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::Str(json), ..] => serde_json::from_str(json).map_err(|e| format!("bad reply JSON: {e}")),
        _ => Err("the backend couldn't clear the lyrics".into()),
    }
}

#[tauri::command]
async fn clear_logs(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    let reply = request(&backend, MessageType::ClearLogs, &[], Duration::from_secs(30))?;
    match reply.as_slice() {
        [Value::Bool(true), Value::Str(json), ..] => serde_json::from_str(json).map_err(|e| format!("bad reply JSON: {e}")),
        _ => Err("the backend couldn't clear the logs".into()),
    }
}

#[tauri::command]
async fn get_queue(backend: tauri::State<'_, Backend>) -> Result<serde_json::Value, String> {
    queue_state(request(&backend, MessageType::GetQueue, &[], Duration::from_secs(5))?)
}

#[tauri::command]
async fn queue_set(backend: tauri::State<'_, Backend>, ids: Vec<String>, start: u32, source: String) -> Result<bool, String> {
    let mut values = vec![Value::Str(source), Value::U32(start)];
    values.extend(ids.into_iter().map(Value::Str));
    expect_ok(request(&backend, MessageType::QueueSet, &values, Duration::from_secs(10))?)
}

#[tauri::command]
async fn queue_next(backend: tauri::State<'_, Backend>) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::QueueNext, &[], Duration::from_secs(10))?)
}

#[tauri::command]
async fn queue_previous(backend: tauri::State<'_, Backend>) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::QueuePrevious, &[], Duration::from_secs(10))?)
}

#[tauri::command]
async fn queue_jump(backend: tauri::State<'_, Backend>, index: u32) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::QueueJump, &[Value::U32(index)], Duration::from_secs(10))?)
}

#[tauri::command]
async fn queue_remove(backend: tauri::State<'_, Backend>, index: u32) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::QueueRemove, &[Value::U32(index)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn queue_play_next(backend: tauri::State<'_, Backend>, id: String) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::QueuePlayNext, &[Value::Str(id)], Duration::from_secs(10))?)
}

#[tauri::command]
async fn queue_add(backend: tauri::State<'_, Backend>, ids: Vec<String>) -> Result<bool, String> {
    let values: Vec<Value> = ids.into_iter().map(Value::Str).collect();
    expect_ok(request(&backend, MessageType::QueueAdd, &values, Duration::from_secs(10))?)
}

#[tauri::command]
async fn queue_clear(backend: tauri::State<'_, Backend>) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::QueueClear, &[], Duration::from_secs(5))?)
}

#[tauri::command]
async fn set_shuffle(backend: tauri::State<'_, Backend>, on: bool) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetShuffle, &[Value::Bool(on)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn set_repeat(backend: tauri::State<'_, Backend>, mode: String) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::SetRepeat, &[Value::Str(mode)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn player_play(backend: tauri::State<'_, Backend>, id: String) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::Play, &[Value::Str(id)], Duration::from_secs(10))?)
}

#[tauri::command]
async fn player_pause(backend: tauri::State<'_, Backend>) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::Pause, &[], Duration::from_secs(5))?)
}

#[tauri::command]
async fn player_resume(backend: tauri::State<'_, Backend>) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::Resume, &[], Duration::from_secs(5))?)
}

#[tauri::command]
async fn player_stop(backend: tauri::State<'_, Backend>) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::Stop, &[], Duration::from_secs(5))?)
}

#[tauri::command]
async fn player_seek(backend: tauri::State<'_, Backend>, seconds: f64) -> Result<bool, String> {
    expect_ok(request(&backend, MessageType::Seek, &[Value::F64(seconds)], Duration::from_secs(5))?)
}

#[tauri::command]
async fn player_set_volume(
    backend: tauri::State<'_, Backend>,
    volume: f64,
    muted: Option<bool>,
    persist: Option<bool>,
) -> Result<bool, String> {
    let values = [Value::F64(volume), Value::Bool(muted.unwrap_or(false)), Value::Bool(persist.unwrap_or(false))];
    expect_ok(request(&backend, MessageType::SetVolume, &values, Duration::from_secs(5))?)
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(Backend::new())
        .setup(move |app| {
            launcher::start(app.handle().clone());
            audio_relay::start();
            #[cfg(windows)]
            if let Err(e) = file_drop::setup(app) {
                eprintln!("file drop unavailable: {e}");
            }
            #[cfg(windows)]
            if let Err(e) = taskbar::setup(app) {
                eprintln!("taskbar buttons unavailable: {e}");
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            greet,
            send_message,
            import_files,
            import_folder,
            get_file_tree,
            get_cover,
            get_covers,
            get_queue,
            get_settings,
            set_equalizer,
            set_deesser,
            set_normalize,
            get_normalize,
            get_lyrics,
            set_visualizer,
            get_artwork,
            clear_cache,
            clear_logs,
            clear_lyrics,
            set_discord_presence,
            set_run_on_startup,
            set_mic_relay,
            set_interface,
            get_player_state,
            get_backend_status,
            remove_from_library,
            queue_set,
            queue_next,
            queue_previous,
            queue_jump,
            queue_remove,
            queue_play_next,
            queue_add,
            queue_clear,
            set_shuffle,
            set_repeat,
            get_playlists,
            create_playlist,
            rename_playlist,
            delete_playlist,
            add_to_playlist,
            remove_from_playlist,
            player_play,
            player_pause,
            player_resume,
            player_stop,
            player_seek,
            player_set_volume
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
