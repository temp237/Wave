use tauri::{App, Emitter, Manager};
use webview2_com::Microsoft::Web::WebView2::Win32::{
    ICoreWebView2File, ICoreWebView2WebMessageReceivedEventArgs2,
};
use webview2_com::{take_pwstr, WebMessageReceivedEventHandler};
use windows::core::{Interface, PWSTR};

const DROP_MESSAGE: &str = "wave-drop";
pub const DROPPED_EVENT: &str = "wave-dropped-paths";

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct DroppedPath {
    path: String,
    is_dir: bool,
}

pub fn setup(app: &App) -> Result<(), Box<dyn std::error::Error>> {
    let window = app.get_webview_window("main").ok_or("main window not found")?;
    let handle = app.handle().clone();

    window.with_webview(move |webview| unsafe {
        let core = match webview.controller().CoreWebView2() {
            Ok(core) => core,
            Err(e) => {
                eprintln!("file drop: no CoreWebView2: {e}");
                return;
            }
        };

        let handler = WebMessageReceivedEventHandler::create(Box::new(move |_, args| {
            let Some(args) = args else { return Ok(()) };

            let mut message = PWSTR::null();
            if args.TryGetWebMessageAsString(&mut message).is_err() || take_pwstr(message) != DROP_MESSAGE {
                return Ok(());
            }

            let Ok(args) = args.cast::<ICoreWebView2WebMessageReceivedEventArgs2>() else {
                return Ok(());
            };
            let objects = args.AdditionalObjects()?;
            let mut count = 0u32;
            objects.Count(&mut count)?;

            let mut dropped = Vec::new();
            for i in 0..count {
                let Ok(file) = objects.GetValueAtIndex(i).and_then(|o| o.cast::<ICoreWebView2File>()) else {
                    continue;
                };
                let mut path = PWSTR::null();
                if file.Path(&mut path).is_ok() {
                    let path = take_pwstr(path);
                    let is_dir = std::path::Path::new(&path).is_dir();
                    dropped.push(DroppedPath { path, is_dir });
                }
            }

            if !dropped.is_empty() {
                let _ = handle.emit(DROPPED_EVENT, dropped);
            }
            Ok(())
        }));

        let mut token = 0i64;
        if let Err(e) = core.add_WebMessageReceived(&handler, &mut token) {
            eprintln!("file drop: couldn't register the message handler: {e}");
        }
    })?;
    Ok(())
}
