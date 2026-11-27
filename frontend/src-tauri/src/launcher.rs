use std::fs::OpenOptions;
use std::io::{self, Read};
use std::net::{Ipv4Addr, SocketAddr, TcpStream};
use std::path::PathBuf;
use std::process::Command;
use std::thread::{self, sleep};
use std::time::{Duration, Instant};

use tauri::{AppHandle, Emitter, Manager};

use crate::Backend;

const PIPE_NAME: &str = r"\\.\pipe\waveport";
const BACKEND_EXE: &str = "WaveBackend.exe";

const ERROR_FILE_NOT_FOUND: i32 = 2;
const ERROR_PIPE_BUSY: i32 = 231;
const ERROR_BROKEN_PIPE: i32 = 109;
const ERROR_PIPE_NOT_CONNECTED: i32 = 233;

pub const STARTING: &str = "starting";
pub const CONNECTED: &str = "connected";
pub const OFFLINE: &str = "offline";

fn read_port(timeout: Duration) -> io::Result<u16> {
    let start = Instant::now();
    let mut pipe = loop {
        match OpenOptions::new().read(true).open(PIPE_NAME) {
            Ok(pipe) => break pipe,
            Err(e)
                if matches!(e.raw_os_error(), Some(ERROR_FILE_NOT_FOUND | ERROR_PIPE_BUSY))
                    && start.elapsed() < timeout =>
            {
                sleep(Duration::from_millis(50));
            }
            Err(e) => return Err(e),
        }
    };

    let mut bytes = Vec::new();
    let mut buf = [0u8; 256];
    loop {
        match pipe.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => bytes.extend_from_slice(&buf[..n]),
            Err(e) if matches!(e.raw_os_error(), Some(ERROR_PIPE_NOT_CONNECTED | ERROR_BROKEN_PIPE)) => break,
            Err(e) => return Err(e),
        }
    }

    let message = String::from_utf8_lossy(&bytes);
    message
        .trim()
        .parse::<u16>()
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, format!("bad port {message:?}: {e}")))
}

fn connect(port: u16) -> io::Result<TcpStream> {
    let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, port));
    let stream = TcpStream::connect_timeout(&addr, Duration::from_secs(3))?;
    stream.set_nodelay(true)?;
    Ok(stream)
}

fn backend_path() -> io::Result<PathBuf> {
    let exe = std::env::current_exe()?;
    let dir = exe.parent().ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "no exe folder"))?;
    let path = dir.join(BACKEND_EXE);
    if path.is_file() {
        Ok(path)
    } else {
        Err(io::Error::new(io::ErrorKind::NotFound, format!("{} is missing", path.display())))
    }
}

fn launch() -> io::Result<()> {
    let path = backend_path()?;
    let mut command = Command::new(&path);
    if let Some(dir) = path.parent() {
        command.current_dir(dir);
    }
    command.spawn().map(|_| ())
}

fn open() -> io::Result<TcpStream> {
    if let Ok(port) = read_port(Duration::from_millis(150)) {
        return connect(port);
    }
    launch()?;
    connect(read_port(Duration::from_secs(15))?)
}

fn set_status(app: &AppHandle, status: &'static str) {
    app.state::<Backend>().set_status(status);
    let _ = app.emit("wave-backend-status", status);
}

pub fn start(app: AppHandle) {
    thread::spawn(move || {
        let mut failures = 0u32;
        loop {
            set_status(&app, STARTING);
            let stream = match open() {
                Ok(stream) => stream,
                Err(e) => {
                    eprintln!("couldn't start the backend: {e}");
                    set_status(&app, OFFLINE);
                    sleep(Duration::from_secs(5));
                    continue;
                }
            };

            let backend = app.state::<Backend>();
            let Some(reader) = backend.attach(stream) else {
                set_status(&app, OFFLINE);
                sleep(Duration::from_secs(5));
                continue;
            };
            set_status(&app, CONNECTED);
            let connected_at = Instant::now();
            crate::read_loop(&app, reader);
            backend.detach();
            let _ = app.emit("wave-backend-lost", ());

            failures = if connected_at.elapsed() < Duration::from_secs(10) { failures + 1 } else { 0 };
            if failures >= 3 {
                set_status(&app, OFFLINE);
                sleep(Duration::from_secs(10));
                failures = 0;
            } else {
                sleep(Duration::from_millis(500));
            }
        }
    });
}
