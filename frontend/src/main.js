// Wave — frontend.
// The library (imported files and folders) is stored by the C++ backend in %APPDATA%\Wave.
// Playlists are stored by the backend too. Settings still live in memory only.

const $ = (sel, root = document) => root.querySelector(sel);

const AUDIO_EXTENSIONS = ["mp3", "wav", "ogg", "oga", "flac", "m4a", "mp4", "mov", "aac", "opus", "webm", "weba", "wma"];

const tauri = window.__TAURI__;
const invoke = (cmd, args) =>
  tauri?.core?.invoke ? tauri.core.invoke(cmd, args) : Promise.reject("Not running inside the Wave app");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const state = {
  tracks: new Map(), // id -> { id, name, title, artist, album, cover, duration, path, addedAt }
  tree: [], // backend folders: [{ name, folders, files }]
  order: [], // library order (track ids)
  playlists: [], // [{ id, name, trackIds, createdAt }]
  view: { type: "library", path: [] },
  selected: null, // { kind: "folder", key }
  picked: new Set(), // selected track ids
  anchor: null, // track id a Shift range starts from
  query: "",

  queue: { order: [], index: -1, shuffle: false, repeat: "off", source: "" },
  currentId: null,

  volume: 0.8,
  muted: false,
  playlistCovers: true,
};

let seeking = false;
const queuePlaying = () => !player.paused && !player.isPreview;
const PREVIEW_PREFIX = "preview:";

class BackendPlayer extends EventTarget {
  #status = "stopped";
  #position = 0;
  #duration = 0;
  #volume = 1;
  #muted = false;
  #fileId = null;
  #updatedAt = 0;

  constructor() {
    super();
    tauri?.event?.listen("wave-player", (e) => this.#update(e.payload));
    tauri?.event?.listen("wave-backend-lost", () => toast("Lost the connection to the Wave backend"));
  }

  get paused() {
    return this.#status !== "playing" && this.#status !== "loading";
  }

  get status() {
    return this.#status;
  }

  get currentTime() {
    return this.#position;
  }

  get fileId() {
    return this.#fileId;
  }

  get isPreview() {
    return !!this.#fileId?.startsWith(PREVIEW_PREFIX);
  }

  get livePosition() {
    if (this.#status !== "playing") return this.#position;
    const advanced = this.#position + (performance.now() - this.#updatedAt) / 1000;
    return this.#duration ? Math.min(advanced, this.#duration) : advanced;
  }

  set currentTime(seconds) {
    this.#position = seconds;
    this.#updatedAt = performance.now();
    invoke("player_seek", { seconds }).catch(() => {});
    this.#emit("timeupdate");
  }

  get duration() {
    return this.#duration;
  }

  set volume(value) {
    this.#volume = value;
    this.#sendVolume();
  }

  set muted(value) {
    this.#muted = value;
    this.#sendVolume();
  }

  play() {
    return invoke("player_resume", {});
  }

  pause() {
    invoke("player_pause", {}).catch(() => {});
  }

  stop() {
    this.#fileId = null;
    this.#status = "stopped";
    this.#position = 0;
    this.#duration = 0;
    invoke("player_stop", {}).catch(() => {});
    this.#emit("pause");
  }

  #sendVolume(persist = false) {
    invoke("player_set_volume", { volume: this.#volume, muted: this.#muted, persist }).catch(() => {});
  }

  saveVolume() {
    this.#sendVolume(true);
  }

  applyState(s) {
    if (s) this.#update(s);
  }

  #emit(type) {
    this.dispatchEvent(new Event(type));
  }

  #update(s) {
    const beforeFile = this.#fileId;
    this.#fileId = s.fileId || null;
    if (this.#fileId !== beforeFile) this.#emit("filechange");
    const before = this.#status;
    const beforeDuration = this.#duration;
    this.#status = s.status;
    this.#position = s.position;
    this.#duration = s.duration;
    this.#updatedAt = performance.now();

    if (this.#duration !== beforeDuration) this.#emit("durationchange");
    this.#emit("timeupdate");
    if (before === this.#status) return;
    if (this.#status === "playing") this.#emit("play");
    else if (this.#status === "paused") this.#emit("pause");
    else if (this.#status === "ended") {
      this.#emit("pause");
      this.#emit("ended");
    } else if (this.#status === "error") this.#emit("error");
  }
}

const player = new BackendPlayer();

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
const esc = (s) =>
  String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

// Hues stay in the violet -> pink band so every cover fits the purple theme
function hue(str) {
  let h = 0;
  for (const c of String(str)) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return 245 + (h % 80);
}

function fmtTime(sec) {
  if (!Number.isFinite(sec) || sec < 0) sec = 0;
  sec = Math.floor(sec);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = String(sec % 60).padStart(2, "0");
  return h ? `${h}:${String(m).padStart(2, "0")}:${s}` : `${m}:${s}`;
}

function fmtLong(sec) {
  const m = Math.round(sec / 60);
  if (m < 1) return `${Math.round(sec)} sec`;
  if (m < 60) return `${m} min`;
  return `${Math.floor(m / 60)} hr ${m % 60} min`;
}

function fmtDate(ts) {
  const d = new Date(ts);
  const sameYear = d.getFullYear() === new Date().getFullYear();
  return d.toLocaleDateString(undefined, { month: "short", day: "numeric", ...(sameYear ? {} : { year: "numeric" }) });
}

function shuffled(arr) {
  const a = [...arr];
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}

const icon = (name) => `<svg class="i"><use href="#i-${name}"/></svg>`;

function setIcon(btn, name) {
  btn.querySelector("use").setAttribute("href", `#i-${name}`);
}

function setRangeFill(input) {
  const min = +input.min || 0;
  const max = +input.max || 1;
  input.style.setProperty("--p", `${((+input.value - min) / (max - min)) * 100}%`);
}


let toastTimer;
function toast(msg) {
  const el = $("#toast");
  el.textContent = msg;
  el.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove("show"), 2600);
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------
const viewKey = (v = state.view) => (v.type === "library" ? `lib:${(v.path || []).join("/")}` : v.type === "downloader" ? "dl" : `pl:${v.id}`);

const LOOSE_FOLDER = "__loose";
const libPath = () => (state.view.type === "library" ? state.view.path || [] : []);

function folderAt(path) {
  let folder = {
    name: "",
    folders: state.tree.filter((f) => f.name !== LOOSE_FOLDER),
    files: state.tree.find((f) => f.name === LOOSE_FOLDER)?.files || [],
  };
  for (const name of path) {
    const next = folder.folders.find((f) => f.name === name);
    if (!next) return null;
    folder = next;
  }
  return folder;
}

function folderFileIds(folder, out = []) {
  for (const f of folder.files || []) out.push(f.id);
  for (const sub of folder.folders || []) folderFileIds(sub, out);
  return out;
}

function currentFolder() {
  return state.view.type === "library" ? folderAt(libPath()) : null;
}

function visibleFolders() {
  if (state.query.trim()) return [];
  return currentFolder()?.folders || [];
}

const playableIds = () => (state.query.trim() ? visibleIds() : viewSource());

function currentPlaylist() {
  return state.view.type === "playlist" ? state.playlists.find((p) => p.id === state.view.id) : null;
}

function viewSource() {
  const pl = currentPlaylist();
  if (pl) return pl.trackIds.filter((id) => state.tracks.has(id));
  const folder = currentFolder();
  return (folder ? folderFileIds(folder) : []).filter((id) => state.tracks.has(id));
}

function visibleIds() {
  const q = state.query.trim().toLowerCase();
  if (!q) {
    if (currentPlaylist()) return viewSource();
    return (currentFolder()?.files || []).map((f) => f.id).filter((id) => state.tracks.has(id));
  }
  return viewSource().filter((id) => state.tracks.get(id).search.includes(q));
}

const viewHistory = { back: [], forward: [] };
const sameView = (a, b) => JSON.stringify(a) === JSON.stringify(b);

function setView(view, record = true) {
  if (record && !sameView(view, state.view)) {
    viewHistory.back.push(state.view);
    if (viewHistory.back.length > 100) viewHistory.back.shift();
    viewHistory.forward = [];
  }
  state.view = view;
  state.selected = null;
  state.picked = new Set();
  state.anchor = null;
  state.query = "";
  $("#search").value = view.type === "downloader" ? downloader.query : "";
  render();
  $("#main").scrollTop = 0;
}

function goBack() {
  const prev = viewHistory.back.pop();
  if (prev) {
    viewHistory.forward.push(state.view);
    setView(prev, false);
  } else if (libPath().length) {
    setView({ type: "library", path: libPath().slice(0, -1) });
  }
}

function goForward() {
  const next = viewHistory.forward.pop();
  if (!next) return;
  viewHistory.back.push(state.view);
  setView(next, false);
}

function openFolder(name) {
  setView({ type: "library", path: [...libPath(), name] });
}

function paintSelection() {
  for (const el of $("#trackList").children) {
    if (el.classList.contains("folder-row")) el.classList.toggle("selected", state.selected?.key === el.dataset.name);
    else el.classList.toggle("selected", state.picked.has(el.dataset.id));
  }
}

function select(kind, key) {
  state.selected = kind === "folder" ? { kind, key } : null;
  state.picked = new Set(kind === "track" ? [key] : []);
  state.anchor = kind === "track" ? key : null;
  paintSelection();
}

function toggleTrack(id) {
  state.selected = null;
  if (state.picked.has(id)) state.picked.delete(id);
  else state.picked.add(id);
  state.anchor = id;
  paintSelection();
}

function selectRange(toId, additive = false) {
  const ids = visibleIds();
  const to = ids.indexOf(toId);
  if (to < 0) return;
  let from = ids.indexOf(state.anchor);
  if (from < 0) {
    from = to;
    state.anchor = toId;
  }
  const range = ids.slice(Math.min(from, to), Math.max(from, to) + 1);
  state.selected = null;
  state.picked = additive ? new Set([...state.picked, ...range]) : new Set(range);
  paintSelection();
}

function selectAllTracks() {
  const ids = visibleIds();
  if (!ids.length) return;
  state.selected = null;
  state.picked = new Set(ids);
  state.anchor = ids[0];
  paintSelection();
}

const pickedIds = () => visibleIds().filter((id) => state.picked.has(id));

const typeAhead = { text: "", at: 0 };
const sortKey = (text) => (text || "").toLowerCase().replace(/^[^\p{L}\p{N}]+/u, "");

function jumpToTyped(char) {
  const now = performance.now();
  const typed = now - typeAhead.at < 800 ? typeAhead.text + char : char;
  typeAhead.text = typed;
  typeAhead.at = now;

  const rows = [...$("#trackList").children].filter((el) => el.dataset.id || el.classList.contains("folder-row"));
  const label = (el) => sortKey(el.dataset.id ? state.tracks.get(el.dataset.id)?.title : el.dataset.name);
  const isSel = (el) => (el.dataset.id ? state.picked.has(el.dataset.id) : state.selected?.key === el.dataset.name);
  const repeating = [...typed].every((c) => c === typed[0]);
  const current = rows.findIndex(isSel);

  let match;
  if (repeating) {
    const hits = rows.map((el, i) => (label(el).startsWith(typed[0]) ? i : -1)).filter((i) => i >= 0);
    match = hits.find((i) => i > current) ?? hits[0] ?? -1;
  } else {
    match = rows.findIndex((el) => label(el).startsWith(typed));
  }
  if (match < 0) return;
  const row = rows[match];
  if (row.dataset.id) select("track", row.dataset.id);
  else select("folder", row.dataset.name);
  row.scrollIntoView({ block: "center" });
}

const isSelected = (kind, key) => (kind === "folder" ? state.selected?.key === key : state.picked.has(key));

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
function render() {
  if (state.view.type === "playlist" && !currentPlaylist()) state.view = { type: "library", path: [] };
  if (state.view.type === "library" && !currentFolder()) state.view = { type: "library", path: [] };
  $("#main").classList.toggle("downloader-view", state.view.type === "downloader");
  renderSidebar();
  renderHeader();
  renderList();
  downloader.render();
  renderPlayState();
}

function renderSidebar() {
  $("#libCount").textContent = state.order.length || "";
  $("#libraryBtn").classList.toggle("active", state.view.type === "library");
  $("#downloaderBtn").classList.toggle("active", state.view.type === "downloader");
  $("#dlCount").textContent = downloader.activeCount || "";

  const list = $("#playlistList");
  if (!state.playlists.length) {
    list.innerHTML = `<li class="pl-empty">No playlists yet.<br>Hit + to make one.</li>`;
    return;
  }
  list.innerHTML = state.playlists
    .map((p) => {
      const active = state.view.type === "playlist" && state.view.id === p.id;
      return `<li>
        <button class="pl-item${active ? " active" : ""}" data-id="${p.id}" data-key="pl:${p.id}" data-name="${esc(p.name)}">
          <span class="pl-cover" style="--h:${hue(p.id)}">${icon("music")}${mosaicHTML(playlistCoverUrls(p))}<span class="pl-play" role="button" aria-label="Play ${esc(p.name)}">${icon("play")}</span></span>
          <span class="pl-name">${esc(p.name)}</span>
          <span class="pl-count">${p.trackIds.length || ""}</span>
        </button>
      </li>`;
    })
    .join("");
}

function renderHeader() {
  if (state.view.type === "downloader") {
    $("#main").style.setProperty("--h", 290);
    $("#viewEyebrow").textContent = "SoundCloud";
    $("#viewTitle").textContent = "Downloader";
    $("#viewSub").textContent = downloader.summary();
    setViewCover("download");
    $("#renameBtn").hidden = true;
    $("#deleteBtn").hidden = true;
    $("#search").placeholder = "Search SoundCloud";
    return;
  }
  $("#search").placeholder = "Search";
  const pl = currentPlaylist();
  const path = libPath();
  const src = viewSource();
  const total = src.reduce((sum, id) => sum + (state.tracks.get(id).duration || 0), 0);
  const folders = pl ? 0 : visibleFolders().length;
  const title = pl ? pl.name : path.length ? path[path.length - 1] : "All tracks";

  $("#main").style.setProperty("--h", pl ? hue(pl.id) : path.length ? hue(path.join("/")) : 265);
  const eyebrow = $("#viewEyebrow");
  if (pl) {
    eyebrow.textContent = "Playlist";
  } else {
    const crumbs = ["Library", ...path.slice(0, -1)];
    eyebrow.innerHTML = path.length
      ? crumbs.map((name, i) => `<button class="crumb" data-depth="${i}">${esc(name)}</button>`).join(`<span class="crumb-sep">/</span>`)
      : "Library";
  }
  $("#viewTitle").textContent = title;
  const parts = [];
  if (folders) parts.push(`${folders} ${folders === 1 ? "folder" : "folders"}`);
  parts.push(`${src.length} ${src.length === 1 ? "track" : "tracks"}`);
  if (total) parts.push(fmtLong(total));
  $("#viewSub").textContent = parts.join(" · ");
  setViewCover(pl ? "music" : path.length ? "folder" : "library", pl ? playlistCoverUrls(pl) : []);

  $("#playAllBtn").disabled = !src.length;
  $("#shuffleAllBtn").disabled = !src.length;
  $("#renameBtn").hidden = !pl;
  $("#deleteBtn").hidden = !pl;
}

const coverCache = new Map();
const coverWanted = new Set();
let coverFlush = null;

const coverImg = (url) => `<img src="${url}" alt="" draggable="false">`;

function coverHTML(t) {
  const key = t?.coverKey;
  const url = key ? coverCache.get(key) : null;
  if (url) return coverImg(url);
  if (key && !coverCache.has(key)) requestCover(key);
  return icon("music");
}

function requestCover(id) {
  if (coverCache.has(id) || coverWanted.has(id)) return;
  coverWanted.add(id);
  if (!coverFlush) coverFlush = setTimeout(flushCovers, 16);
}

async function flushCovers() {
  const ids = [...coverWanted].slice(0, 64);
  let loaded = false;
  for (const id of ids) {
    coverWanted.delete(id);
    coverCache.set(id, null);
  }
  try {
    const found = await invoke("get_covers", { ids });
    for (const id of ids) {
      if (!found[id]) continue;
      const blob = await (await fetch(found[id])).blob();
      const url = URL.createObjectURL(blob);
      coverCache.set(id, url);
      loaded = true;
      for (const el of document.querySelectorAll(`[data-cover="${id}"]`)) el.innerHTML = coverImg(url);
    }
  } catch (err) {
    console.warn("Wave: couldn't load covers", err);
  }
  if (loaded && state.playlists.length) {
    renderSidebar();
    const pl = currentPlaylist();
    if (pl) setViewCover("music", playlistCoverUrls(pl));
  }
  coverFlush = coverWanted.size ? setTimeout(flushCovers, 0) : null;
}

function playlistCoverUrls(p) {
  if (!state.playlistCovers) return [];
  const seen = new Set();
  const urls = [];
  for (const id of p.trackIds) {
    const key = state.tracks.get(id)?.coverKey;
    if (!key || seen.has(key)) continue;
    seen.add(key);
    if (!coverCache.has(key)) requestCover(key);
    const url = coverCache.get(key);
    if (url) urls.push(url);
    if (urls.length === 4 || seen.size >= 24) break;
  }
  return urls.length >= 4 ? urls : urls.slice(0, 1);
}

const mosaicHTML = (urls) =>
  urls.length ? `<span class="cover-mosaic${urls.length === 4 ? " grid" : ""}">${urls.map(coverImg).join("")}</span>` : "";

function setViewCover(name, urls = []) {
  const el = $("#viewCover");
  const key = `${name}|${urls.join("|")}`;
  if (el.dataset.key === key) return;
  el.dataset.key = key;
  el.innerHTML = icon(name) + mosaicHTML(urls);
}

function prefetchCovers() {
  for (const t of state.tracks.values()) {
    if (t.coverKey) requestCover(t.coverKey);
  }
}

function resetCoverCache() {
  for (const url of coverCache.values()) {
    if (url) URL.revokeObjectURL(url);
  }
  coverCache.clear();
  coverWanted.clear();
}

function folderRowHTML(folder) {
  const ids = folderFileIds(folder);
  const total = ids.reduce((sum, id) => sum + (state.tracks.get(id)?.duration || 0), 0);
  const subs = folder.folders?.length || 0;
  const detail = [`${ids.length} ${ids.length === 1 ? "track" : "tracks"}`];
  if (subs) detail.push(`${subs} ${subs === 1 ? "folder" : "folders"}`);
  const selected = isSelected("folder", folder.name) ? " selected" : "";
  return `<li class="folder-row${selected}" data-name="${esc(folder.name)}" draggable="true">
    <span class="col-num">${icon("folder")}</span>
    <span class="col-title">
      <span class="art folder-art" style="--h:${hue(folder.name)}">${icon("folder")}</span>
      <span class="tt">
        <span class="t-title">${esc(folder.name)}</span>
        <span class="t-artist">${detail.join(" · ")}</span>
      </span>
    </span>
    <span class="col-album"></span>
    <span class="col-added"></span>
    <span class="col-dur">${total ? fmtTime(total) : ""}</span>
    <span></span>
  </li>`;
}

function rowHTML(t, i) {
  const current = t.id === state.currentId;
  const playing = current && queuePlaying();
  const classes = `row${isSelected("track", t.id) ? " selected" : ""}${current ? " current" : ""}${playing ? " is-playing" : ""}`;
  return `<li class="${classes}" data-id="${t.id}" draggable="true">
    <span class="col-num">
      <span class="num">${i + 1}</span>
      <span class="eq"><i></i><i></i><i></i></span>
      <button class="row-play" aria-label="Play ${esc(t.title)}">${icon(playing ? "pause" : "play")}</button>
    </span>
    <span class="col-title">
      <span class="art" data-cover="${t.coverKey}" style="--h:${hue(t.artist + t.title)}">${coverHTML(t)}</span>
      <span class="tt">
        <span class="t-title">${esc(t.title)}</span>
        <span class="t-artist">${esc(t.artist)}</span>
      </span>
    </span>
    <span class="col-album">${esc(t.album)}</span>
    <span class="col-added">${fmtDate(t.addedAt)}</span>
    <span class="col-dur">${t.duration ? fmtTime(t.duration) : "–"}</span>
    <button class="icon-btn row-more" aria-label="More options">${icon("more")}</button>
  </li>`;
}

function emptyHTML() {
  const pl = currentPlaylist();
  if (state.query) {
    return `<div class="empty-icon">${icon("search")}</div>
      <h2>No matches</h2>
      <p>Nothing here matches “${esc(state.query)}”.</p>`;
  }
  if (pl) {
    return `<div class="empty-icon">${icon("music")}</div>
      <h2>This playlist is empty</h2>
      <p>Drag tracks from your Library onto it in the sidebar, use a track's ⋯ menu, or import files straight into it.</p>
      <div class="empty-actions">
        <button class="btn" data-action="go-library">${icon("library")}<span>Browse Library</span></button>
        <button class="btn primary" data-action="import-files">${icon("upload")}<span>Import files</span></button>
      </div>`;
  }
  return `<div class="empty-icon">${icon("music")}</div>
    <h2>Your library is empty</h2>
    <p>Import some audio files from your computer to get started.</p>
    <div class="empty-actions">
      <button class="btn primary" data-action="import-files">${icon("upload")}<span>Import files</span></button>
      <button class="btn" data-action="import-folder">${icon("folder")}<span>Import folder</span></button>
    </div>`;
}

function renderList() {
  if (state.view.type === "downloader") {
    ++listRenderToken;
    $("#trackList").innerHTML = "";
    return;
  }
  const ids = visibleIds();
  const folders = visibleFolders();
  const list = $("#trackList");
  const empty = $("#emptyState");

  if (!ids.length && !folders.length) {
    list.innerHTML = "";
    $("#trackHead").hidden = true;
    empty.hidden = false;
    empty.innerHTML = emptyHTML();
    return;
  }
  $("#trackHead").hidden = false;
  empty.hidden = true;
  const token = ++listRenderToken;
  const rows = (from, to) => {
    let html = "";
    for (let i = from; i < to; i++) html += rowHTML(state.tracks.get(ids[i]), i);
    return html;
  };
  let next = Math.min(ids.length, 120);
  list.innerHTML = folders.map(folderRowHTML).join("") + rows(0, next);
  const step = () => {
    if (token !== listRenderToken || next >= ids.length) return;
    const end = Math.min(ids.length, next + 400);
    list.insertAdjacentHTML("beforeend", rows(next, end));
    next = end;
    requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}

let listRenderToken = 0;

function renderRowsState() {
  const playing = queuePlaying();
  for (const row of $("#trackList").querySelectorAll(".row")) {
    const cur = row.dataset.id === state.currentId;
    row.classList.toggle("current", cur);
    row.classList.toggle("is-playing", cur && playing);
    setIcon(row.querySelector(".row-play"), cur && playing ? "pause" : "play");
  }
}

function renderPlayState() {
  const playing = !player.paused;
  const playBtn = $("#playBtn");
  setIcon(playBtn, playing ? "pause" : "play");
  playBtn.ariaLabel = playing ? "Pause" : "Play";

  const thisViewPlaying = queuePlaying() && state.queue.source === viewKey();
  setIcon($("#playAllBtn"), thisViewPlaying ? "pause" : "play");

  for (const el of document.querySelectorAll(".pl-item")) {
    const live = queuePlaying() && state.queue.source === el.dataset.key;
    el.classList.toggle("live", live);
    const play = el.querySelector(".pl-play");
    if (play) {
      setIcon(play, live ? "pause" : "play");
      play.ariaLabel = `${live ? "Pause" : "Play"} ${el.dataset.name}`;
    }
  }
  renderRowsState();
  downloader.paintPreview();
  const currentPlay = $("#queueBody")?.querySelector(".q-item.current .q-play");
  if (currentPlay) setIcon(currentPlay, queuePlaying() ? "pause" : "play");
}

function renderNow() {
  const art = $("#nowArt");
  const preview = downloader.previewInfo(player.fileId);
  if (preview) {
    $("#nowTitle").textContent = preview.title;
    $("#nowArtist").textContent = `${preview.artist} · Preview`;
    art.classList.remove("idle");
    art.style.setProperty("--h", hue(preview.artist + preview.title));
    delete art.dataset.cover;
    art.innerHTML = preview.thumbnailUrl ? coverImg(esc(preview.thumbnailUrl)) : icon("music");
    document.title = `${preview.title} · ${preview.artist}`;
    return;
  }
  const t = state.tracks.get(state.currentId);
  if (!t) {
    $("#nowTitle").textContent = "Nothing playing";
    $("#nowArtist").textContent = "Pick a track to start";
    art.classList.add("idle");
    delete art.dataset.cover;
    art.innerHTML = icon("music");
    document.title = "Wave";
    $("#curTime").textContent = $("#durTime").textContent = "0:00";
    $("#seek").value = 0;
    setRangeFill($("#seek"));
    return;
  }
  $("#nowTitle").textContent = t.title;
  $("#nowArtist").textContent = t.album ? `${t.artist} · ${t.album}` : t.artist;
  art.classList.remove("idle");
  art.style.setProperty("--h", hue(t.artist + t.title));
  art.dataset.cover = t.coverKey || "";
  art.innerHTML = coverHTML(t);
  document.title = `${t.title} · ${t.artist}`;
  nowPlaying.sync();
}

function renderModes() {
  const sh = $("#shuffleBtn");
  const { shuffle, repeat } = state.queue;
  sh.classList.toggle("on", shuffle);
  sh.setAttribute("aria-pressed", shuffle);

  const rp = $("#repeatBtn");
  rp.classList.toggle("on", repeat !== "off");
  setIcon(rp, repeat === "one" ? "repeat-one" : "repeat");
  rp.ariaLabel = `Repeat: ${repeat}`;
}

function renderVolume() {
  const vol = $("#volume");
  vol.value = state.muted ? 0 : state.volume;
  setRangeFill(vol);
  player.volume = state.volume;
  player.muted = state.muted;
  const m = $("#muteBtn");
  setIcon(m, state.muted || state.volume === 0 ? "mute" : "volume");
  m.ariaLabel = state.muted ? "Unmute" : "Mute";
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------
async function queueCall(cmd, args = {}) {
  try {
    return await invoke(cmd, args);
  } catch (err) {
    toast(`Playback error: ${String(err).split("\n")[0]}`);
    return false;
  }
}

function applyQueue(q) {
  const before = state.currentId;
  state.queue = {
    order: Array.isArray(q?.order) ? q.order : [],
    index: Number.isInteger(q?.index) ? q.index : -1,
    queued: Number.isInteger(q?.queued) ? q.queued : 0,
    shuffle: !!q?.shuffle,
    repeat: q?.repeat || "off",
    source: q?.source || "",
  };
  state.currentId = state.queue.order[state.queue.index] ?? null;
  if (state.currentId !== before) {
    renderNow();
    const t = state.tracks.get(state.currentId);
    if (t) updateMediaSession(t);
  }
  renderModes();
  renderPlayState();
  renderQueuePanel();
}

async function playFrom(ids, start = 0, source = viewKey()) {
  if (!ids.length) return;
  if (!(await queueCall("queue_set", { ids, start: Math.max(0, start), source }))) {
    toast(`Can't play “${state.tracks.get(ids[start])?.title ?? "that track"}”`);
  }
}

async function sendDebug() {
  const invoke = window.__TAURI__?.core?.invoke;
  if (!invoke) return;
  const title = state.tracks.get(state.currentId)?.title ?? "no track";
  try {
    const result = await invoke("send_message", { messageType: "Debug", data: title });
    console.log(result);
    toast(result);
  } catch (err) {
    console.error("send_message failed:", err);
    toast(`send_message failed: ${String(err).split("\n")[0]}`);
  }
}

function togglePlay() {
  if (!state.currentId && !player.fileId) {
    const ids = playableIds();
    if (ids.length) playFrom(ids, state.queue.shuffle ? Math.floor(Math.random() * ids.length) : 0);
    return;
  }
  if (player.paused) player.play().catch(() => {});
  else player.pause();
}

function next() {
  queueCall("queue_next");
}

function prev() {
  queueCall("queue_previous");
}

async function playNext(id) {
  if (await queueCall("queue_play_next", { id })) toast(`“${state.tracks.get(id)?.title}” plays next`);
}

async function addToQueue(ids) {
  if (!ids.length) return;
  if (await queueCall("queue_add", { ids })) {
    toast(ids.length === 1 ? `Added “${state.tracks.get(ids[0])?.title}” to the queue` : `Added ${ids.length} tracks to the queue`);
  }
}

function toggleShuffle() {
  return queueCall("set_shuffle", { on: !state.queue.shuffle });
}

function cycleRepeat() {
  queueCall("set_repeat", { mode: { off: "all", all: "one", one: "off" }[state.queue.repeat] || "off" });
}

function stopPlayback() {
  player.stop();
}

function queueSourceLabel(source) {
  if (source.startsWith("pl:")) return state.playlists.find((p) => p.id === source.slice(3))?.name || "Playlist";
  if (source.startsWith("lib:")) {
    const path = source.slice(4);
    return path ? path.split("/").join(" / ") : "Library";
  }
  return "";
}

function queueItemHTML(id, index, current) {
  const t = state.tracks.get(id);
  if (!t) return "";
  const playing = current && queuePlaying();
  return `<li class="q-item${current ? " current" : ""}" data-index="${index}">
    <span class="q-art">
      <span class="art" data-cover="${t.coverKey}" style="--h:${hue(t.artist + t.title)}">${coverHTML(t)}</span>
      <span class="q-play" role="button" aria-label="${playing ? "Pause" : "Play"} ${esc(t.title)}">${icon(playing ? "pause" : "play")}</span>
    </span>
    <span class="tt">
      <span class="t-title">${esc(t.title)}</span>
      <span class="t-artist">${esc(t.artist)}</span>
    </span>
    <span class="q-dur">${t.duration ? fmtTime(t.duration) : ""}</span>
    ${current ? "<span></span>" : `<button class="icon-btn q-remove" aria-label="Remove from queue">${icon("close")}</button>`}
  </li>`;
}

const isQueueOpen = () => $(".app").classList.contains("queue-open");

function renderQueuePanel() {
  if (!isQueueOpen()) return;
  const { order, index, source } = state.queue;
  const body = $("#queueBody");
  if (!order.length || index < 0) {
    body.innerHTML = `<p class="q-empty">Nothing in the queue.<br>Play something to fill it up.</p>`;
    return;
  }
  const label = queueSourceLabel(source);
  const QUEUE_SHOWN = 300;
  const queued = Math.max(0, Math.min(state.queue.queued || 0, order.length - index - 1));
  const queuedItems = [];
  for (let i = index + 1; i <= index + queued; i++) queuedItems.push([order[i], i]);
  const remaining = order.length - index - 1 - queued;
  const upcoming = [];
  for (let i = index + 1 + queued; i < order.length && upcoming.length < QUEUE_SHOWN; i++) upcoming.push([order[i], i]);
  body.innerHTML = `
    <h3 class="q-heading">Now playing</h3>
    <ol class="q-list">${queueItemHTML(order[index], index, true)}</ol>
    ${queuedItems.length ? `<h3 class="q-heading">Next in queue</h3><ol class="q-list">${queuedItems.map(([id, i]) => queueItemHTML(id, i, false)).join("")}</ol>` : ""}
    <h3 class="q-heading">Next up${label ? ` <span class="q-source">from ${esc(label)}</span>` : ""}</h3>
    ${upcoming.length ? `<ol class="q-list">${upcoming.map(([id, i]) => queueItemHTML(id, i, false)).join("")}</ol>` : `<p class="q-empty small">Nothing after this.</p>`}
    ${remaining > upcoming.length ? `<p class="q-empty small">…and ${remaining - upcoming.length} more</p>` : ""}`;
}

function queueItemMenu(index, x, y, el) {
  const { order, source } = state.queue;
  const id = order[index];
  const t = state.tracks.get(id);
  if (!t) return;
  const current = index === state.queue.index;
  const playing = current && queuePlaying();
  const sourcePl = source.startsWith("pl:") ? state.playlists.find((p) => p.id === source.slice(3)) : null;
  const items = [
    {
      label: current ? (playing ? "Pause" : "Play") : "Play now",
      icon: playing ? "pause" : "play",
      action: () => (current ? togglePlay() : queueCall("queue_jump", { index })),
    },
  ];
  if (!current) {
    items.push({ label: "Play next", icon: "queue", action: () => playNext(id) });
    items.push({ label: "Remove from queue", icon: "minus", action: () => queueCall("queue_remove", { index }) });
  }
  items.push("sep", { heading: "Add to playlist" });
  for (const p of state.playlists) {
    const has = p.trackIds.includes(id);
    items.push({ label: p.name, icon: has ? "check" : "list-plus", disabled: has, action: () => addToPlaylist(p, [id]) });
  }
  items.push({ label: "New playlist…", icon: "plus", action: () => createPlaylist([id]) });
  if (sourcePl && sourcePl.trackIds.includes(id)) {
    items.push("sep", { label: `Remove from ${sourcePl.name}`, icon: "trash", danger: true, action: () => removeFromPlaylist(sourcePl, [id]) });
  }
  openMenu(x, y, items, el);
}

function toggleQueuePanel(show = !isQueueOpen(), save = true) {
  $(".app").classList.toggle("queue-open", show);
  $("#queuePanel").setAttribute("aria-hidden", String(!show));
  $("#queueBtn").classList.toggle("on", show);
  $("#queueBtn").setAttribute("aria-pressed", show);
  $("#showQueueSetting").checked = show;
  renderQueuePanel();
  if (save) invoke("set_interface", { settings: { showQueue: show } }).catch(() => {});
}

function updateMediaSession(t) {
  if (!("mediaSession" in navigator)) return;
  try {
    navigator.mediaSession.metadata = new MediaMetadata({ title: t.title, artist: t.artist, album: "Wave" });
  } catch {}
}

function bindMediaSession() {
  if (!("mediaSession" in navigator)) return;
  const handlers = {
    play: () => player.play().catch(() => {}),
    pause: () => player.pause(),
    previoustrack: prev,
    nexttrack: () => next(),
    seekto: (d) => (player.currentTime = d.seekTime),
  };
  for (const [action, fn] of Object.entries(handlers)) {
    try {
      navigator.mediaSession.setActionHandler(action, fn);
    } catch {}
  }
}

// ---------------------------------------------------------------------------
// Library & playlists
// ---------------------------------------------------------------------------
const baseName = (path) => path.split(/[\\/]/).pop();

function walkTree(folder, out) {
  for (const f of folder.files || []) out.push(f);
  for (const sub of folder.folders || []) walkTree(sub, out);
}

function trackFromFile(f, addedAt) {
  const name = baseName(f.path);
  const title = f.displayTitle || f.title || name;
  const artist = f.displayArtist || f.artist || "Unknown artist";
  return {
    id: f.id,
    name,
    title,
    artist,
    album: f.album || "",
    coverKey: f.coverKey || "",
    duration: f.duration || 0,
    path: f.path,
    addedAt,
    search: [title, artist, f.album || "", name].join(" ").toLowerCase(),
  };
}

/** Pulls the file tree from the backend and adds any files the library doesn't have yet. */
async function loadLibrary() {
  let tree;
  try {
    tree = await invoke("get_file_tree");
  } catch (err) {
    console.warn("Wave: couldn't load the library from the backend", err);
    toast(`Couldn't load your library: ${String(err).split("\n")[0]}`);
    return [];
  }

  state.tree = tree.folders || [];
  const files = [];
  for (const folder of state.tree) walkTree(folder, files);

  const base = Date.now();
  const present = new Set();
  for (const [n, f] of files.entries()) {
    const existing = state.tracks.get(f.id);
    const track = trackFromFile(f, existing?.addedAt ?? base + n);
    state.tracks.set(track.id, track);
    present.add(track.id);
    if (!existing) state.order.push(track.id);
  }
  for (const id of [...state.tracks.keys()]) {
    if (!present.has(id)) state.tracks.delete(id);
  }
  state.order = state.order.filter((id) => present.has(id));
  render();
  renderNow();
  return files;
}

const errText = (err) => String(err).split("\n")[0];

const playlistFromBackend = (p) => ({
  id: p.id,
  name: p.name,
  trackIds: Array.isArray(p.tracks) ? p.tracks : [],
  createdAt: p.createdAt || 0,
});

async function loadPlaylists() {
  try {
    const list = await invoke("get_playlists");
    state.playlists = (list || []).map(playlistFromBackend);
  } catch (err) {
    console.warn("Wave: couldn't load playlists from the backend", err);
    toast(`Couldn't load your playlists: ${errText(err)}`);
  }
  render();
}

async function addImportedToPlaylist(pl, ids) {
  const fresh = [...new Set(ids)].filter((id) => !pl.trackIds.includes(id));
  if (!fresh.length) return 0;
  try {
    await invoke("add_to_playlist", { id: pl.id, trackIds: fresh });
  } catch (err) {
    toast(`Couldn't add to ${pl.name}: ${errText(err)}`);
    return 0;
  }
  pl.trackIds.push(...fresh);
  render();
  return fresh.length;
}

async function pickAndImportFiles() {
  let paths;
  try {
    paths = await tauri.dialog.open({
      multiple: true,
      title: "Import audio files",
      filters: [{ name: "Audio", extensions: AUDIO_EXTENSIONS }],
    });
  } catch (err) {
    toast(`Couldn't open the file picker: ${String(err).split("\n")[0]}`);
    return;
  }
  if (!paths || !paths.length) return;
  if (!Array.isArray(paths)) paths = [paths];
  await importFilePaths(paths);
}

async function importFilePaths(paths) {
  const pl = currentPlaylist();
  const before = new Set(state.tracks.keys());
  toast(`Importing ${paths.length} ${paths.length === 1 ? "file" : "files"}…`);

  let results;
  try {
    results = await invoke("import_files", { paths });
  } catch (err) {
    toast(`Import failed: ${String(err).split("\n")[0]}`);
    return;
  }
  await loadLibrary();

  const ids = results.filter((r) => r.id).map((r) => r.id);
  const failed = results.length - ids.length;
  const fresh = ids.filter((id) => !before.has(id)).length;
  const addedToPl = pl ? await addImportedToPlaylist(pl, ids) : 0;

  let msg = fresh ? `Imported ${fresh} ${fresh === 1 ? "track" : "tracks"}` : ids.length ? "Already in your library" : "Nothing imported";
  if (fresh && ids.length > fresh) msg += ` (${ids.length - fresh} already there)`;
  if (failed) msg += ` · ${failed} couldn't be imported`;
  if (pl && addedToPl) msg += ` · added to ${pl.name}`;
  toast(msg);
}

async function pickAndImportFolder() {
  let path;
  try {
    path = await tauri.dialog.open({ directory: true, title: "Import a folder" });
  } catch (err) {
    toast(`Couldn't open the folder picker: ${String(err).split("\n")[0]}`);
    return;
  }
  if (!path) return;
  await importFolderPath(path);
}

async function importFolderPath(path) {
  const pl = currentPlaylist();
  const before = new Set(state.tracks.keys());
  toast(`Scanning ${baseName(path) || path}…`);

  let count;
  try {
    count = await invoke("import_folder", { path });
  } catch (err) {
    toast(`Import failed: ${String(err).split("\n")[0]}`);
    return;
  }
  const files = await loadLibrary();

  const prefix = path.replace(/[\\/]+$/, "").toLowerCase() + "\\";
  const ids = files.filter((f) => f.path.toLowerCase().startsWith(prefix)).map((f) => f.id);
  const fresh = ids.filter((id) => !before.has(id)).length;
  const addedToPl = pl ? await addImportedToPlaylist(pl, ids) : 0;

  let msg = !count
    ? "No audio files in that folder"
    : fresh
      ? `Imported ${fresh} ${fresh === 1 ? "track" : "tracks"}`
      : "Already in your library";
  if (fresh && count > fresh) msg += ` (${count - fresh} already there)`;
  if (pl && addedToPl) msg += ` · added to ${pl.name}`;
  toast(msg);
}

const isAudioPath = (path) => AUDIO_EXTENSIONS.includes(path.split(".").pop().toLowerCase());

async function importDropped(entries) {
  const folders = entries.filter((e) => e.isDir).map((e) => e.path);
  const files = entries.filter((e) => !e.isDir && isAudioPath(e.path)).map((e) => e.path);
  if (!folders.length && !files.length) {
    toast("No audio files in what you dropped");
    return;
  }
  if (files.length) await importFilePaths(files);
  for (const folder of folders) await importFolderPath(folder);
}

async function removeFromLibrary(ids) {
  const unique = [...new Set(ids)].filter((id) => state.tracks.has(id));
  if (!unique.length) return;
  const one = unique.length === 1 ? state.tracks.get(unique[0]) : null;
  const ok = await ask({
    title: one ? "Remove from library?" : `Remove ${unique.length} tracks from library?`,
    text: `${one ? `“${one.title}” will` : "They'll"} be removed from your library, every playlist and the queue. The files on your computer aren't touched.`,
    ok: "Remove",
    danger: true,
  });
  if (!ok) return;

  let removed;
  try {
    removed = await invoke("remove_from_library", { ids: unique });
  } catch (err) {
    toast(`Couldn't remove from library: ${errText(err)}`);
    return;
  }
  const gone = new Set(unique);
  for (const p of state.playlists) p.trackIds = p.trackIds.filter((x) => !gone.has(x));
  state.picked = new Set([...state.picked].filter((id) => !gone.has(id)));
  await loadLibrary();
  const count = typeof removed === "number" ? removed : unique.length;
  toast(count === 1 ? "Removed from library" : `Removed ${count} tracks from library`);
}

async function createPlaylist(withTrackIds = [], suggestedName = "") {
  const name = await ask({
    title: "New playlist",
    value: suggestedName || `My playlist #${state.playlists.length + 1}`,
    ok: "Create",
  });
  if (!name) return null;
  let p;
  try {
    p = playlistFromBackend(await invoke("create_playlist", { name, trackIds: [...new Set(withTrackIds)] }));
  } catch (err) {
    toast(`Couldn't create the playlist: ${errText(err)}`);
    return null;
  }
  state.playlists.push(p);
  if (withTrackIds.length) {
    renderSidebar();
    toast(p.trackIds.length === 1 ? `Added to ${p.name}` : `Added ${p.trackIds.length} tracks to ${p.name}`);
  } else {
    setView({ type: "playlist", id: p.id });
  }
  return p;
}

async function renamePlaylist(p) {
  const name = await ask({ title: "Rename playlist", value: p.name, ok: "Save" });
  if (!name || name === p.name) return;
  try {
    if (!(await invoke("rename_playlist", { id: p.id, name }))) throw "the backend refused";
  } catch (err) {
    toast(`Couldn't rename the playlist: ${errText(err)}`);
    return;
  }
  p.name = name.trim();
  render();
}

async function deletePlaylist(p) {
  const ok = await ask({
    title: "Delete playlist?",
    text: `“${p.name}” will be deleted. Its tracks stay in your library.`,
    ok: "Delete",
    danger: true,
  });
  if (!ok) return;
  try {
    if (!(await invoke("delete_playlist", { id: p.id }))) throw "the backend refused";
  } catch (err) {
    toast(`Couldn't delete the playlist: ${errText(err)}`);
    return;
  }
  state.playlists = state.playlists.filter((x) => x.id !== p.id);
  if (state.view.type === "playlist" && state.view.id === p.id) setView({ type: "library", path: [] });
  else render();
  toast("Playlist deleted");
}

async function addToPlaylist(p, ids) {
  const unique = [...new Set(ids)];
  const fresh = unique.filter((id) => !p.trackIds.includes(id));
  if (!fresh.length) {
    toast(unique.length === 1 ? `Already in ${p.name}` : `All ${unique.length} tracks are already in ${p.name}`);
    return;
  }
  try {
    await invoke("add_to_playlist", { id: p.id, trackIds: fresh });
  } catch (err) {
    toast(`Couldn't add to ${p.name}: ${errText(err)}`);
    return;
  }
  p.trackIds.push(...fresh);
  renderSidebar();
  renderPlayState();
  if (state.view.type === "playlist" && state.view.id === p.id) {
    renderHeader();
    renderList();
  }
  const skipped = unique.length - fresh.length;
  if (unique.length === 1) toast(`Added to ${p.name}`);
  else toast(`Added ${fresh.length} ${fresh.length === 1 ? "track" : "tracks"} to ${p.name}${skipped ? ` (${skipped} already there)` : ""}`);
}

async function removeFromPlaylist(p, ids) {
  const remove = new Set(ids);
  if (!remove.size) return;
  let removed;
  try {
    removed = await invoke("remove_from_playlist", { id: p.id, trackIds: [...remove] });
  } catch (err) {
    toast(`Couldn't remove from ${p.name}: ${errText(err)}`);
    return;
  }
  p.trackIds = p.trackIds.filter((x) => !remove.has(x));
  state.picked = new Set([...state.picked].filter((id) => !remove.has(id)));
  render();
  const count = typeof removed === "number" ? removed : remove.size;
  toast(count === 1 ? `Removed from ${p.name}` : `Removed ${count} tracks from ${p.name}`);
}

function selectionMenu(x, y, row) {
  const ids = pickedIds();
  const pl = currentPlaylist();
  const n = ids.length;
  const items = [
    { label: `Play ${n} tracks`, icon: "play", action: () => playFrom(ids, 0) },
    { label: "Add to queue", icon: "list-plus", action: () => addToQueue(ids) },
    "sep",
    { heading: `Add ${n} tracks to playlist` },
  ];
  for (const p of state.playlists) {
    if (pl && p.id === pl.id) continue;
    const hasAll = ids.every((id) => p.trackIds.includes(id));
    items.push({ label: p.name, icon: hasAll ? "check" : "list-plus", disabled: hasAll, action: () => addToPlaylist(p, ids) });
  }
  items.push({ label: "New playlist…", icon: "plus", action: () => createPlaylist(ids) });
  items.push("sep");
  if (pl) items.push({ label: `Remove ${n} from this playlist`, icon: "minus", action: () => removeFromPlaylist(pl, ids) });
  items.push({ label: `Remove ${n} from library`, icon: "trash", danger: true, action: () => removeFromLibrary(ids) });
  openMenu(x, y, items, row);
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------
const menu = $("#menu");
let menuRow = null;

function openMenu(x, y, items, row = null) {
  closeMenu();
  menu.innerHTML = "";
  for (const it of items) {
    if (it === "sep") {
      menu.insertAdjacentHTML("beforeend", `<div class="menu-sep"></div>`);
    } else if (it.heading) {
      menu.insertAdjacentHTML("beforeend", `<div class="menu-heading">${esc(it.heading)}</div>`);
    } else {
      const b = document.createElement("button");
      b.className = `menu-item${it.danger ? " danger" : ""}`;
      b.setAttribute("role", "menuitem");
      b.disabled = !!it.disabled;
      b.innerHTML = `${icon(it.icon || "music")}<span>${esc(it.label)}</span>`;
      b.onclick = () => {
        closeMenu();
        it.action();
      };
      menu.append(b);
    }
  }
  menu.hidden = false;
  const r = menu.getBoundingClientRect();
  menu.style.left = `${Math.max(8, Math.min(x, innerWidth - r.width - 8))}px`;
  menu.style.top = `${Math.max(8, y + r.height > innerHeight - 8 ? y - r.height : y)}px`;
  menuRow = row;
  menuRow?.classList.add("menu-open");
  menu.querySelector(".menu-item:not(:disabled)")?.focus({ preventScroll: true });
}

function closeMenu() {
  if (menu.hidden) return;
  menu.hidden = true;
  menuRow?.classList.remove("menu-open");
  menuRow = null;
}

function trackMenu(id, x, y, row) {
  const t = state.tracks.get(id);
  const pl = currentPlaylist();
  const items = [
    {
      label: state.currentId === id && queuePlaying() ? "Pause" : "Play",
      icon: state.currentId === id && queuePlaying() ? "pause" : "play",
      action: () => (state.currentId === id && !player.isPreview ? togglePlay() : playFrom(visibleIds(), visibleIds().indexOf(id))),
    },
  ];
  if (state.currentId && state.currentId !== id) {
    items.push({ label: "Play next", icon: "queue", action: () => playNext(id) });
  }
  items.push({ label: "Add to queue", icon: "list-plus", action: () => addToQueue([id]) });
  items.push("sep", { heading: "Add to playlist" });
  for (const p of state.playlists) {
    if (pl && p.id === pl.id) continue;
    const has = p.trackIds.includes(id);
    items.push({ label: p.name, icon: has ? "check" : "list-plus", disabled: has, action: () => addToPlaylist(p, [id]) });
  }
  items.push({ label: "New playlist…", icon: "plus", action: () => createPlaylist([id]) });
  items.push("sep");
  if (pl) items.push({ label: "Remove from this playlist", icon: "minus", action: () => removeFromPlaylist(pl, [id]) });
  items.push({ label: "Remove from library", icon: "trash", danger: true, action: () => removeFromLibrary([t.id]) });
  openMenu(x, y, items, row);
}

function folderByName(name) {
  return currentFolder()?.folders?.find((f) => f.name === name) || null;
}

function folderMenu(name, x, y, row) {
  const folder = folderByName(name);
  if (!folder) return;
  const ids = folderFileIds(folder).filter((id) => state.tracks.has(id));
  const source = `lib:${[...libPath(), name].join("/")}`;
  const items = [
    { label: "Open", icon: "folder", action: () => openFolder(name) },
    { label: "Play", icon: "play", disabled: !ids.length, action: () => playFrom(ids, 0, source) },
    { label: "Add to queue", icon: "list-plus", disabled: !ids.length, action: () => addToQueue(ids) },
    "sep",
    { heading: `Add ${ids.length} ${ids.length === 1 ? "track" : "tracks"} to playlist` },
  ];
  for (const p of state.playlists) {
    const hasAll = ids.length > 0 && ids.every((id) => p.trackIds.includes(id));
    items.push({ label: p.name, icon: hasAll ? "check" : "list-plus", disabled: !ids.length || hasAll, action: () => addToPlaylist(p, ids) });
  }
  items.push({ label: "New playlist…", icon: "plus", disabled: !ids.length, action: () => createPlaylist(ids, name) });
  openMenu(x, y, items, row);
}

function playPlaylist(p) {
  const source = `pl:${p.id}`;
  if (state.queue.source === source && state.currentId) {
    togglePlay();
    return;
  }
  const ids = p.trackIds.filter((id) => state.tracks.has(id));
  if (!ids.length) {
    toast(`${p.name} is empty`);
    return;
  }
  playFrom(ids, state.queue.shuffle ? Math.floor(Math.random() * ids.length) : 0, source);
}

function playlistMenu(p, x, y) {
  openMenu(x, y, [
    {
      label: "Play",
      icon: "play",
      disabled: !p.trackIds.length,
      action: () => {
        setView({ type: "playlist", id: p.id });
        playFrom(viewSource(), 0);
      },
    },
    { label: "Rename", icon: "edit", action: () => renamePlaylist(p) },
    "sep",
    { label: "Delete playlist", icon: "trash", danger: true, action: () => deletePlaylist(p) },
  ]);
}

// ---------------------------------------------------------------------------
// Modal (window.prompt/confirm don't work inside Tauri's webview)
// ---------------------------------------------------------------------------
let modalClose = null;

function ask({ title, text = "", value = null, ok = "OK", danger = false }) {
  const modal = $("#modal");
  const input = $("#modalInput");
  const okBtn = $("#modalOk");
  $("#modalTitle").textContent = title;
  $("#modalText").textContent = text;
  okBtn.textContent = ok;
  okBtn.classList.toggle("danger", danger);
  input.hidden = value === null;
  input.value = value ?? "";
  modal.hidden = false;
  closeMenu();
  requestAnimationFrame(() => (value === null ? okBtn.focus() : (input.focus(), input.select())));

  return new Promise((resolve) => {
    const done = (result) => {
      modal.hidden = true;
      modalClose = null;
      $("#modalForm").onsubmit = null;
      resolve(result);
    };
    modalClose = () => done(null);
    $("#modalForm").onsubmit = (e) => {
      e.preventDefault();
      if (value === null) return done(true);
      const v = input.value.trim();
      if (!v) return input.focus();
      done(v);
    };
  });
}

// ---------------------------------------------------------------------------
// Drag & drop
// ---------------------------------------------------------------------------
const TRACK_MIME = "application/x-wave-track";

function bindDragDrop() {
  const overlay = $("#dropOverlay");
  const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes("Files");

  window.addEventListener("dragover", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "copy";
    overlay.classList.add("show");
  });
  overlay.addEventListener("dragleave", (e) => {
    if (!e.relatedTarget || !overlay.contains(e.relatedTarget)) overlay.classList.remove("show");
  });
  window.addEventListener("drop", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    overlay.classList.remove("show");
    const forward = window.chrome?.webview?.postMessageWithAdditionalObjects;
    if (!forward || !e.dataTransfer.files.length) {
      toast("Drag and drop isn't available here. Use Import files or Import folder.");
      return;
    }
    window.chrome.webview.postMessageWithAdditionalObjects("wave-drop", e.dataTransfer.files);
  });
  tauri?.event?.listen("wave-dropped-paths", (e) => importDropped(e.payload));

  // Dragging rows onto sidebar playlists
  const list = $("#trackList");
  list.addEventListener("dragstart", (e) => {
    const folderRow = e.target.closest(".folder-row");
    const row = folderRow || e.target.closest(".row");
    if (!row) return;
    let ids;
    let label;
    if (folderRow) {
      const folder = folderByName(folderRow.dataset.name);
      ids = folder ? folderFileIds(folder).filter((id) => state.tracks.has(id)) : [];
      label = `${folderRow.dataset.name} (${ids.length} ${ids.length === 1 ? "track" : "tracks"})`;
      if (!ids.length) {
        e.preventDefault();
        return;
      }
    } else if (state.picked.has(row.dataset.id) && state.picked.size > 1) {
      ids = pickedIds();
      label = `${ids.length} tracks`;
    } else {
      ids = [row.dataset.id];
      label = state.tracks.get(row.dataset.id)?.title || "";
    }
    e.dataTransfer.setData(TRACK_MIME, JSON.stringify(ids));
    e.dataTransfer.setData("text/plain", label);
    e.dataTransfer.effectAllowed = "copy";
    row.classList.add("dragging");
  });
  list.addEventListener("dragend", (e) => e.target.closest?.(".row, .folder-row")?.classList.remove("dragging"));

  const pls = $("#playlistList");
  const clearTargets = () => pls.querySelectorAll(".drop-target").forEach((el) => el.classList.remove("drop-target"));
  pls.addEventListener("dragover", (e) => {
    if (![...e.dataTransfer.types].includes(TRACK_MIME)) return;
    const item = e.target.closest(".pl-item");
    if (!item) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "copy";
    if (!item.classList.contains("drop-target")) {
      clearTargets();
      item.classList.add("drop-target");
    }
  });
  pls.addEventListener("dragleave", (e) => {
    if (!pls.contains(e.relatedTarget)) clearTargets();
  });
  pls.addEventListener("drop", (e) => {
    const raw = e.dataTransfer.getData(TRACK_MIME);
    const item = e.target.closest(".pl-item");
    clearTargets();
    if (!raw || !item) return;
    e.preventDefault();
    let ids;
    try {
      ids = JSON.parse(raw);
    } catch {
      ids = [raw];
    }
    if (!Array.isArray(ids) || !ids.length) return;
    const p = state.playlists.find((x) => x.id === item.dataset.id);
    if (p) addToPlaylist(p, ids);
  });
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
function bindUI() {
  // data-action buttons (toolbar + empty states)
  document.addEventListener("click", (e) => {
    const el = e.target.closest("[data-action]");
    if (!el) return;
    const action = el.dataset.action;
    if (action === "import-files") pickAndImportFiles();
    else if (action === "import-folder") pickAndImportFolder();
    else if (action === "go-library") setView({ type: "library", path: [] });
  });

  // Sidebar
  $("#libraryBtn").onclick = () => setView({ type: "library", path: [] });
  $("#downloaderBtn").onclick = () => setView({ type: "downloader" });
  $("#viewEyebrow").addEventListener("click", (e) => {
    const crumb = e.target.closest(".crumb");
    if (crumb) setView({ type: "library", path: libPath().slice(0, +crumb.dataset.depth) });
  });
  $("#newPlaylistBtn").onclick = () => createPlaylist();
  $("#playlistList").addEventListener("click", (e) => {
    const item = e.target.closest(".pl-item");
    if (!item) return;
    if (e.target.closest(".pl-play")) {
      const p = state.playlists.find((x) => x.id === item.dataset.id);
      if (p) playPlaylist(p);
      return;
    }
    setView({ type: "playlist", id: item.dataset.id });
  });
  $("#playlistList").addEventListener("contextmenu", (e) => {
    const item = e.target.closest(".pl-item");
    if (!item) return;
    e.preventDefault();
    const p = state.playlists.find((x) => x.id === item.dataset.id);
    if (p) playlistMenu(p, e.clientX, e.clientY);
  });

  // Toolbar
  $("#playAllBtn").onclick = () => {
    if (state.queue.source === viewKey() && state.currentId && !player.isPreview) togglePlay();
    else playFrom(playableIds(), 0);
  };
  $("#shuffleAllBtn").onclick = async () => {
    const ids = playableIds();
    if (!ids.length) return;
    if (!state.queue.shuffle) await toggleShuffle();
    playFrom(ids, Math.floor(Math.random() * ids.length));
  };
  $("#renameBtn").onclick = () => currentPlaylist() && renamePlaylist(currentPlaylist());
  $("#deleteBtn").onclick = () => currentPlaylist() && deletePlaylist(currentPlaylist());
  let searchTimer = null;
  $("#search").addEventListener("input", (e) => {
    clearTimeout(searchTimer);
    if (state.view.type === "downloader") {
      searchTimer = setTimeout(() => downloader.search(e.target.value), 500);
      return;
    }
    searchTimer = setTimeout(() => {
      state.query = e.target.value;
      renderList();
      $("#main").scrollTop = Math.min($("#main").scrollTop, $(".view-head").offsetHeight);
    }, 120);
  });

  $("#search").addEventListener("keydown", (e) => {
    if (e.key !== "Enter" || state.view.type !== "downloader") return;
    e.preventDefault();
    clearTimeout(searchTimer);
    downloader.search(e.target.value, true);
  });
  $("#dlList").addEventListener("click", (e) => {
    const play = e.target.closest(".dl-play");
    if (play) {
      downloader.togglePreview(play.closest(".dl-row").dataset.id);
      return;
    }
    const btn = e.target.closest(".dl-btn");
    if (btn) downloader.press(btn.closest(".dl-row").dataset.id);
  });
  $("#dlState").addEventListener("click", (e) => {
    if (e.target.closest("[data-dl-retry]")) downloader.search($("#search").value, true);
  });

  // Track list
  const list = $("#trackList");
  const rowIndex = (row) => [...list.querySelectorAll(".row")].indexOf(row);
  list.addEventListener("click", (e) => {
    const folderRow = e.target.closest(".folder-row");
    if (folderRow) {
      select("folder", folderRow.dataset.name);
      return;
    }
    const row = e.target.closest(".row");
    if (!row) {
      select(null);
      return;
    }
    if (e.target.closest(".t-title") && row.dataset.id === state.currentId && !e.shiftKey && !e.ctrlKey && !e.metaKey) {
      nowPlaying.open(row);
      return;
    }
    if (!e.target.closest("button")) {
      const id = row.dataset.id;
      if (e.shiftKey) selectRange(id, e.ctrlKey || e.metaKey);
      else if (e.ctrlKey || e.metaKey) toggleTrack(id);
      else select("track", id);
    }
    if (e.target.closest(".row-play")) {
      if (row.dataset.id === state.currentId && !player.isPreview) togglePlay();
      else playFrom(visibleIds(), rowIndex(row));
    } else if (e.target.closest(".row-more")) {
      const r = e.target.closest(".row-more").getBoundingClientRect();
      trackMenu(row.dataset.id, r.right - 210, r.bottom + 4, row);
    }
  });
  list.addEventListener("dblclick", (e) => {
    const folderRow = e.target.closest(".folder-row");
    if (folderRow) {
      openFolder(folderRow.dataset.name);
      return;
    }
    const row = e.target.closest(".row");
    if (!row || e.target.closest("button")) return;
    if (nowPlaying.isOpen || (e.target.closest(".t-title") && row.dataset.id === state.currentId)) return;
    playFrom(visibleIds(), rowIndex(row));
  });
  let sweeping = false;
  list.addEventListener("mousedown", (e) => {
    if (e.button !== 0 || !e.shiftKey) return;
    const row = e.target.closest(".row");
    if (!row || e.target.closest("button")) return;
    e.preventDefault();
    sweeping = true;
    if (!state.anchor || !state.picked.size) state.anchor = row.dataset.id;
    selectRange(row.dataset.id, e.ctrlKey || e.metaKey);
  });
  list.addEventListener("mouseover", (e) => {
    if (!sweeping) return;
    const row = e.target.closest(".row");
    if (row) selectRange(row.dataset.id, e.ctrlKey || e.metaKey);
  });
  window.addEventListener("mouseup", () => {
    sweeping = false;
  });

  list.addEventListener("contextmenu", (e) => {
    const folderRow = e.target.closest(".folder-row");
    if (folderRow) {
      e.preventDefault();
      select("folder", folderRow.dataset.name);
      folderMenu(folderRow.dataset.name, e.clientX, e.clientY, folderRow);
      return;
    }
    const row = e.target.closest(".row");
    if (!row) return;
    e.preventDefault();
    if (state.picked.has(row.dataset.id) && state.picked.size > 1) {
      selectionMenu(e.clientX, e.clientY, row);
      return;
    }
    select("track", row.dataset.id);
    trackMenu(row.dataset.id, e.clientX, e.clientY, row);
  });

  // Player
  $("#playBtn").onclick = () => {
    togglePlay();
    //sendDebug();
  };
  $("#nextBtn").onclick = () => next();
  $("#prevBtn").onclick = prev;
  $("#shuffleBtn").onclick = toggleShuffle;
  $("#repeatBtn").onclick = cycleRepeat;

  const seek = $("#seek");
  seek.addEventListener("input", () => {
    seeking = true;
    setRangeFill(seek);
    if (player.duration) $("#curTime").textContent = fmtTime((seek.value / 1000) * player.duration);
  });
  seek.addEventListener("change", () => {
    if (player.duration) player.currentTime = (seek.value / 1000) * player.duration;
    seeking = false;
  });

  const vol = $("#volume");
  vol.addEventListener("input", () => {
    state.volume = +vol.value;
    state.muted = state.volume === 0;
    renderVolume();
  });
  vol.addEventListener("change", () => player.saveVolume());
  $("#muteBtn").onclick = () => {
    if (state.muted || state.volume === 0) {
      state.muted = false;
      if (state.volume === 0) state.volume = 0.5;
    } else {
      state.muted = true;
    }
    renderVolume();
    player.saveVolume();
  };

  player.addEventListener("play", renderPlayState);
  player.addEventListener("pause", renderPlayState);
  player.addEventListener("filechange", () => {
    renderNow();
    renderPlayState();
  });
  player.addEventListener("timeupdate", () => {
    if (seeking || !player.duration) return;
    seek.value = (player.currentTime / player.duration) * 1000;
    setRangeFill(seek);
    $("#curTime").textContent = fmtTime(player.currentTime);
  });
  player.addEventListener("durationchange", () => {
    $("#durTime").textContent = fmtTime(player.duration);
    const t = state.tracks.get(state.currentId);
    if (t && !t.duration && Number.isFinite(player.duration)) {
      t.duration = player.duration;
      renderHeader();
      renderList();
    }
  });
  player.addEventListener("error", () => {
    if (!state.currentId) return;
    toast(`Can't play “${state.tracks.get(state.currentId)?.title ?? "this file"}”`);
    renderPlayState();
  });

  // Global dismissal
  document.addEventListener("mousedown", (e) => {
    if (!menu.hidden && !menu.contains(e.target)) closeMenu();
  });
  $("#modal").addEventListener("mousedown", (e) => {
    if (e.target.id === "modal") modalClose?.();
  });
  $("#modalCancel").onclick = () => modalClose?.();
  $("#main").addEventListener("scroll", closeMenu, { passive: true });
  const toolbar = $(".toolbar");
  $("#main").addEventListener(
    "scroll",
    () => toolbar.classList.toggle("stuck", toolbar.offsetTop - $("#main").scrollTop <= 0),
    { passive: true }
  );
  window.addEventListener("resize", closeMenu);
  window.addEventListener("blur", closeMenu);

  // Keyboard
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      if (!$("#modal").hidden) return modalClose?.();
      if (!$("#settingsPanel").hidden) return closeSettings();
      if (!menu.hidden) return closeMenu();
      if (nowPlaying.isOpen) return nowPlaying.close();
      if (isQueueOpen()) return toggleQueuePanel(false);
      if (document.activeElement === $("#search")) $("#search").blur();
      else if (state.picked.size || state.selected) select(null);
      return;
    }
    if (!$("#modal").hidden || !menu.hidden || !$("#settingsPanel").hidden) return;
    const t = e.target;
    const typing = t.matches?.("input:not([type=range]), textarea, [contenteditable]");
    if (typing) return;
    if (nowPlaying.isOpen && e.code !== "Space" && e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;

    if (e.altKey && (e.key === "ArrowLeft" || e.key === "ArrowRight")) {
      e.preventDefault();
      if (e.key === "ArrowLeft") goBack();
      else goForward();
      return;
    }
    if (e.key === "Backspace" && libPath().length) {
      e.preventDefault();
      setView({ type: "library", path: libPath().slice(0, -1) });
      return;
    }
    if (e.key === "Enter" && (state.selected || state.picked.size)) {
      e.preventDefault();
      if (state.selected) openFolder(state.selected.key);
      else {
        const ids = visibleIds();
        const index = ids.indexOf(pickedIds()[0]);
        if (index >= 0) playFrom(ids, index);
      }
      return;
    }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "a") {
      e.preventDefault();
      selectAllTracks();
      return;
    }
    if (e.key === "Delete" && state.picked.size) {
      e.preventDefault();
      const pl = currentPlaylist();
      if (pl) removeFromPlaylist(pl, pickedIds());
      else removeFromLibrary(pickedIds());
      return;
    }

    if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey && /[\p{L}\p{N}]/u.test(e.key) && !t.matches?.("input[type=range], select")) {
      e.preventDefault();
      jumpToTyped(e.key.toLowerCase());
      return;
    }

    if (e.code === "Space") {
      e.preventDefault();
      if (t instanceof HTMLButtonElement) t.blur(); // don't also "click" the focused button
      togglePlay();
    } else if (t.matches?.("input[type=range]")) {
      return; // let sliders use arrow keys
    } else if (e.key === "ArrowRight" && player.duration) {
      e.preventDefault();
      if (e.ctrlKey || e.metaKey) next();
      else player.currentTime = Math.min(player.duration, player.currentTime + 5);
    } else if (e.key === "ArrowLeft" && player.duration) {
      e.preventDefault();
      if (e.ctrlKey || e.metaKey) prev();
      else player.currentTime = Math.max(0, player.currentTime - 5);
    } else if (e.key === "/" || ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "f")) {
      e.preventDefault();
      $("#search").focus();
    }
  });

  const isNavButton = (e) => e.button === 3 || e.button === 4;
  for (const type of ["mousedown", "auxclick"]) {
    window.addEventListener(type, (e) => {
      if (isNavButton(e)) e.preventDefault();
    });
  }
  window.addEventListener("mouseup", (e) => {
    if (!isNavButton(e)) return;
    e.preventDefault();
    if (e.button === 3) goBack();
    else goForward();
  });

  tauri?.event?.listen("wave-queue", (e) => applyQueue(e.payload));
  tauri?.event?.listen("wave-taskbar-play", () => togglePlay());
  bindSettings();
  $("#queueBtn").onclick = () => toggleQueuePanel();
  $("#queueCloseBtn").onclick = () => toggleQueuePanel(false);
  $("#queueClearBtn").onclick = () => queueCall("queue_clear");
  $("#queueBody").addEventListener("click", (e) => {
    const remove = e.target.closest(".q-remove");
    if (remove) {
      queueCall("queue_remove", { index: +remove.closest(".q-item").dataset.index });
      return;
    }
    const play = e.target.closest(".q-play");
    if (play) {
      const index = +play.closest(".q-item").dataset.index;
      if (index === state.queue.index && !player.isPreview) togglePlay();
      else queueCall("queue_jump", { index });
    }
  });
  $("#queueBody").addEventListener("contextmenu", (e) => {
    const item = e.target.closest(".q-item");
    if (!item) return;
    e.preventDefault();
    queueItemMenu(+item.dataset.index, e.clientX, e.clientY, item);
  });
  $("#queueBody").addEventListener("dblclick", (e) => {
    const item = e.target.closest(".q-item");
    if (item && !e.target.closest("button")) queueCall("queue_jump", { index: +item.dataset.index });
  });

  // Hide the default webview context menu outside our own menus
  document.addEventListener("contextmenu", (e) => {
    if (!e.target.closest("input")) e.preventDefault();
  });
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
const EQ_PRESETS = {
  Flat: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  "Bass Boost": [6, 5, 4, 2, 0, 0, 0, 0, 0, 0],
  "Bass Reducer": [-6, -5, -4, -2, 0, 0, 0, 0, 0, 0],
  "Treble Boost": [0, 0, 0, 0, 0, 1, 2, 4, 5, 6],
  Vocal: [-2, -2, -1, 1, 3, 4, 3, 1, 0, -1],
  Rock: [4, 3, 2, 0, -1, -1, 1, 2, 3, 4],
  Pop: [-1, 1, 2, 3, 2, 0, -1, -1, 1, 1],
  Electronic: [5, 4, 1, 0, -2, 1, 0, 2, 4, 5],
  Acoustic: [3, 3, 2, 1, 1, 1, 2, 2, 2, 1],
};

const eq = { enabled: false, preamp: 0, gains: Array(10).fill(0), frequencies: [], maxGain: 12 };
let eqSendTimer = null;

const fmtHz = (f) => (f >= 1000 ? `${f / 1000}k` : `${f}`);
const fmtDb = (v) => `${v > 0 ? "+" : ""}${Number(v).toFixed(v % 1 ? 1 : 0)} dB`;

function fmtBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

function matchingPreset() {
  return Object.keys(EQ_PRESETS).find((name) => EQ_PRESETS[name].every((g, i) => g === eq.gains[i])) || "Custom";
}

function renderEqualizer() {
  $("#eqEnabled").checked = eq.enabled;
  $("#eqPanel").classList.toggle("off", !eq.enabled);
  const preset = matchingPreset();
  $("#eqPreset").value = preset;
  $("#eqPreset").querySelector('option[value="Custom"]').hidden = preset !== "Custom";

  const pre = $("#eqPreamp");
  pre.value = eq.preamp;
  setRangeFill(pre);
  $("#eqPreampValue").textContent = fmtDb(eq.preamp);

  const bands = $("#eqBands");
  if (bands.children.length !== eq.gains.length) {
    bands.innerHTML = eq.gains
      .map(
        (_, i) => `<div class="eq-band">
          <span class="eq-value" data-value="${i}"></span>
          <div class="eq-track"><input type="range" class="eq-slider" data-band="${i}" min="${-eq.maxGain}" max="${eq.maxGain}" step="0.5" aria-label="${fmtHz(eq.frequencies[i] || 0)} Hz" /></div>
          <span class="eq-freq">${fmtHz(eq.frequencies[i] || 0)}</span>
        </div>`
      )
      .join("");
  }
  for (const input of bands.querySelectorAll(".eq-slider")) {
    const i = +input.dataset.band;
    input.value = eq.gains[i];
    setRangeFill(input);
    bands.querySelector(`[data-value="${i}"]`).textContent = `${eq.gains[i] > 0 ? "+" : ""}${eq.gains[i]}`;
  }
}

function sendEqualizer() {
  clearTimeout(eqSendTimer);
  eqSendTimer = setTimeout(() => {
    invoke("set_equalizer", { settings: { enabled: eq.enabled, preamp: eq.preamp, bands: eq.gains } }).catch((err) =>
      toast(`Couldn't update the equalizer: ${errText(err)}`)
    );
  }, 60);
}

const deess = { enabled: false, auto: false, gain: 0, frequency: 6500, minGain: -24, minFrequency: 2000, maxFrequency: 12000 };
let deessSendTimer = null;

const deessSlider = (f) => Math.round((1000 * Math.log(f / deess.minFrequency)) / Math.log(deess.maxFrequency / deess.minFrequency));
const deessFrequency = (v) => Math.round((deess.minFrequency * Math.pow(deess.maxFrequency / deess.minFrequency, v / 1000)) / 50) * 50;
const fmtKhz = (f) => `${(f / 1000).toFixed(f % 1000 ? 1 : 0)} kHz`;

function renderDeEsser() {
  $("#deessEnabled").checked = deess.enabled;
  $("#deessBody").classList.toggle("off", !deess.enabled);
  const auto = $("#deessAuto");
  auto.setAttribute("aria-pressed", String(deess.auto));
  $("#deessPanel").classList.toggle("off", deess.auto);

  const gain = $("#deessGain");
  gain.min = deess.minGain;
  gain.value = deess.gain;
  gain.disabled = deess.auto;
  setRangeFill(gain);
  $("#deessGainValue").textContent = deess.gain === 0 ? "0 dB (off)" : fmtDb(deess.gain);

  const freq = $("#deessFreq");
  freq.value = deessSlider(deess.frequency);
  freq.disabled = deess.auto;
  setRangeFill(freq);
  $("#deessFreqValue").textContent = fmtKhz(deess.frequency);
}

function sendDeEsser() {
  clearTimeout(deessSendTimer);
  deessSendTimer = setTimeout(() => {
    invoke("set_deesser", { settings: { enabled: deess.enabled, auto: deess.auto, gain: deess.gain, frequency: deess.frequency } }).catch((err) =>
      toast(`Couldn't update the de-esser: ${errText(err)}`)
    );
  }, 60);
}

let normalizeTimer = null;

function showNormalize(state) {
  const info = $("#normalizeInfo");
  $("#normalizeEnabled").checked = state?.enabled === true;
  clearTimeout(normalizeTimer);
  if (!state?.enabled || !state.total) {
    info.hidden = true;
    return;
  }
  info.hidden = false;
  const done = state.measured >= state.total;
  info.textContent = done
    ? `All ${state.total} songs measured · target ${state.target} LUFS`
    : `Measuring loudness… ${state.measured} of ${state.total} songs`;
  if (!done && !$("#settingsPanel").hidden) normalizeTimer = setTimeout(refreshNormalize, 2000);
}

async function refreshNormalize() {
  try {
    showNormalize(await invoke("get_normalize"));
  } catch {}
}

async function openSettings() {
  $("#settingsPanel").hidden = false;
  $("#cacheInfo").textContent = "Loading…";
  try {
    const settings = await invoke("get_settings");
    const e = settings.equalizer || {};
    eq.enabled = !!e.enabled;
    eq.preamp = e.preamp || 0;
    eq.maxGain = e.maxGain || 12;
    eq.frequencies = (e.bands || []).map((b) => b.frequency);
    eq.gains = (e.bands || []).map((b) => b.gain);
    renderEqualizer();
    Object.assign(deess, settings.deesser || {});
    renderDeEsser();
    showNormalize(settings.normalize);
    $("#skipToVocals").checked = settings.playback?.skipToVocals === true;
    $("#crossfadeSeconds").value = settings.playback?.crossfade || 0;
    showCrossfade();
    const cache = settings.cache || { bytes: 0, files: 0 };
    $("#cacheInfo").textContent = `${cache.files} cover ${cache.files === 1 ? "image" : "images"} · ${fmtBytes(cache.bytes)}`;
    showLogsInfo(settings.logs);
    showLyricsInfo(settings.lyrics);
    $("#dataFolder").textContent = settings.dataFolder || "";
    $("#discordEnabled").checked = settings.discord?.enabled !== false;
    $("#runOnStartup").checked = settings.startup?.enabled === true;
    $("#micRelay").checked = settings.microphone?.relay === true && settings.microphone?.installed === true;
  } catch (err) {
    $("#cacheInfo").textContent = "Unavailable";
    toast(`Couldn't load settings: ${errText(err)}`);
  }
}

function showCrossfade() {
  const seconds = +$("#crossfadeSeconds").value;
  $("#crossfadeValue").textContent = seconds > 0 ? `${seconds} s` : "Off";
}

function closeSettings() {
  $("#settingsPanel").hidden = true;
}

async function clearCache() {
  const ok = await ask({
    title: "Clear cache?",
    text: "Cover images are deleted and rebuilt from your music files. Your library and playlists aren't touched.",
    ok: "Clear cache",
    danger: true,
  });
  if (!ok) return;
  const btn = $("#clearCacheBtn");
  btn.disabled = true;
  $("#cacheInfo").textContent = "Clearing and rebuilding covers…";
  try {
    const result = await invoke("clear_cache");
    resetCoverCache();
    await loadLibrary();
    prefetchCovers();
    const cache = result.cache || { bytes: 0, files: 0 };
    $("#cacheInfo").textContent = `${cache.files} cover ${cache.files === 1 ? "image" : "images"} · ${fmtBytes(cache.bytes)}`;
    toast(`Cache cleared (${fmtBytes(result.freed || 0)} freed) and covers rebuilt`);
  } catch (err) {
    toast(`Couldn't clear the cache: ${errText(err)}`);
  } finally {
    btn.disabled = false;
  }
}

function showLyricsInfo(lyrics) {
  const info = lyrics || { cached: 0, total: 0 };
  $("#lyricsInfo").textContent = info.total
    ? `${info.cached.toLocaleString()} of ${info.total.toLocaleString()} ${info.total === 1 ? "song has" : "songs have"} cached lyrics`
    : "No songs in your library yet";
}

async function clearLyrics() {
  const ok = await ask({
    title: "Clear cached lyrics?",
    text: "Every song's lyrics are deleted and made again in the background, starting with what you're playing. This can take a few hours for a big library.",
    ok: "Clear lyrics",
    danger: true,
  });
  if (!ok) return;
  const btn = $("#clearLyricsBtn");
  btn.disabled = true;
  $("#lyricsInfo").textContent = "Clearing lyrics…";
  try {
    const result = await invoke("clear_lyrics");
    showLyricsInfo(result.lyrics);
    toast(`Cleared lyrics for ${result.cleared.toLocaleString()} ${result.cleared === 1 ? "song" : "songs"}`);
  } catch (err) {
    toast(`Couldn't clear the lyrics: ${errText(err)}`);
  } finally {
    btn.disabled = false;
  }
}

function showLogsInfo(logs) {
  const info = logs || { bytes: 0, files: 0 };
  $("#logsInfo").textContent = `${info.files} log ${info.files === 1 ? "file" : "files"} · ${fmtBytes(info.bytes)}`;
}

async function clearLogs() {
  const ok = await ask({
    title: "Clear logs?",
    text: "Old log files are deleted and the current log starts fresh.",
    ok: "Clear logs",
    danger: true,
  });
  if (!ok) return;
  const btn = $("#clearLogsBtn");
  btn.disabled = true;
  try {
    const result = await invoke("clear_logs");
    showLogsInfo(result.logs);
    toast(`Logs cleared (${fmtBytes(result.freed || 0)} freed)`);
  } catch (err) {
    toast(`Couldn't clear the logs: ${errText(err)}`);
  } finally {
    btn.disabled = false;
  }
}

function bindSettings() {
  $("#settingsBtn").onclick = openSettings;
  $("#settingsCloseBtn").onclick = closeSettings;
  $("#settingsPanel").addEventListener("mousedown", (e) => {
    if (e.target.id === "settingsPanel") closeSettings();
  });
  $("#clearCacheBtn").onclick = clearCache;
  $("#clearLogsBtn").onclick = clearLogs;
  $("#clearLyricsBtn").onclick = clearLyrics;
  $("#showQueueSetting").addEventListener("change", (e) => toggleQueuePanel(e.target.checked));
  $("#playlistCoversSetting").addEventListener("change", (e) => {
    state.playlistCovers = e.target.checked;
    render();
    invoke("set_interface", { settings: { playlistCovers: state.playlistCovers } }).catch(() => {});
  });
  $("#micRelay").addEventListener("change", (e) => {
    const enabled = e.target.checked;
    e.target.disabled = true;
    if (enabled) toast("Setting up your microphone — approve the Windows prompt if it appears");
    invoke("set_mic_relay", { enabled })
      .then(() => toast(enabled ? "Music now plays through your microphone" : "Stopped playing music through your microphone"))
      .catch((err) => {
        e.target.checked = !enabled;
        toast(`Couldn't change Relay To Microphone: ${errText(err)}`);
      })
      .finally(() => {
        e.target.disabled = false;
      });
  });
  $("#runOnStartup").addEventListener("change", (e) => {
    const enabled = e.target.checked;
    invoke("set_run_on_startup", { enabled })
      .then((ok) => {
        if (!ok) throw "the backend couldn't update the startup entry";
        toast(enabled ? "Wave will run on startup" : "Wave won't run on startup");
      })
      .catch((err) => {
        e.target.checked = !enabled;
        toast(`Couldn't change run on startup: ${errText(err)}`);
      });
  });
  $("#discordEnabled").addEventListener("change", (e) => {
    const enabled = e.target.checked;
    invoke("set_discord_presence", { enabled })
      .then(() => toast(enabled ? "Discord rich presence on" : "Discord rich presence off"))
      .catch((err) => {
        e.target.checked = !enabled;
        toast(`Couldn't change Discord presence: ${errText(err)}`);
      });
  });

  const presets = $("#eqPreset");
  presets.innerHTML =
    Object.keys(EQ_PRESETS).map((name) => `<option value="${esc(name)}">${esc(name)}</option>`).join("") +
    `<option value="Custom">Custom</option>`;
  presets.addEventListener("change", () => {
    const gains = EQ_PRESETS[presets.value];
    if (!gains) return;
    eq.gains = [...gains];
    if (!eq.enabled) eq.enabled = true;
    renderEqualizer();
    sendEqualizer();
  });

  $("#eqEnabled").addEventListener("change", (e) => {
    eq.enabled = e.target.checked;
    renderEqualizer();
    sendEqualizer();
  });
  $("#eqPreamp").addEventListener("input", (e) => {
    eq.preamp = +e.target.value;
    renderEqualizer();
    sendEqualizer();
  });
  $("#eqBands").addEventListener("input", (e) => {
    const input = e.target.closest(".eq-slider");
    if (!input) return;
    eq.gains[+input.dataset.band] = +input.value;
    renderEqualizer();
    sendEqualizer();
  });
  $("#eqBands").addEventListener("dblclick", (e) => {
    const input = e.target.closest(".eq-slider");
    if (!input) return;
    eq.gains[+input.dataset.band] = 0;
    renderEqualizer();
    sendEqualizer();
  });
  $("#normalizeEnabled").addEventListener("change", (e) => {
    const enabled = e.target.checked;
    invoke("set_normalize", { enabled })
      .then(() => {
        toast(enabled ? "Volume normalization on" : "Volume normalization off");
        setTimeout(refreshNormalize, 300);
      })
      .catch((err) => {
        e.target.checked = !enabled;
        toast(`Couldn't change volume normalization: ${errText(err)}`);
      });
  });
  $("#crossfadeSeconds").addEventListener("input", showCrossfade);
  $("#crossfadeSeconds").addEventListener("change", (e) => {
    const seconds = +e.target.value;
    invoke("set_crossfade", { seconds })
      .then(() => toast(seconds > 0 ? `Crossfade set to ${seconds} s` : "Crossfade off"))
      .catch((err) => toast(`Couldn't change crossfade: ${errText(err)}`));
  });
  $("#skipToVocals").addEventListener("change", (e) => {
    const enabled = e.target.checked;
    invoke("set_skip_to_vocals", { enabled })
      .then(() => toast(enabled ? "Skip to vocals on" : "Skip to vocals off"))
      .catch((err) => {
        e.target.checked = !enabled;
        toast(`Couldn't change skip to vocals: ${errText(err)}`);
      });
  });
  $("#deessEnabled").addEventListener("change", (e) => {
    deess.enabled = e.target.checked;
    renderDeEsser();
    sendDeEsser();
  });
  $("#deessAuto").onclick = () => {
    deess.auto = !deess.auto;
    if (!deess.enabled) deess.enabled = true;
    renderDeEsser();
    sendDeEsser();
  };
  $("#deessGain").addEventListener("input", (e) => {
    deess.gain = +e.target.value;
    if (!deess.enabled) deess.enabled = true;
    renderDeEsser();
    sendDeEsser();
  });
  $("#deessGain").addEventListener("dblclick", () => {
    if (deess.auto) return;
    deess.gain = 0;
    renderDeEsser();
    sendDeEsser();
  });
  $("#deessFreq").addEventListener("input", (e) => {
    deess.frequency = deessFrequency(+e.target.value);
    if (!deess.enabled) deess.enabled = true;
    renderDeEsser();
    sendDeEsser();
  });
  $("#eqReset").onclick = () => {
    eq.gains = Array(eq.gains.length).fill(0);
    eq.preamp = 0;
    renderEqualizer();
    sendEqualizer();
  };
}

// ---------------------------------------------------------------------------
// Now Playing
// ---------------------------------------------------------------------------
const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)");

class NowPlaying {
  static BANDS = 64;
  static LYRIC_LEAD = 0.15;
  static USER_SCROLL_PAUSE = 3500;
  static LYRICS_POLL = 4000;
  static ART_CACHE = 8;
  static MIN_ART_CHROMA = 0.06;

  #el = $("#nowPlaying");
  #art = $("#npArt");
  #bg = $("#npBg");
  #lyrics = $("#npLyrics");
  #canvas = $("#npViz");
  #ctx = this.#canvas.getContext("2d");
  #open = false;
  #trackId = null;
  #lines = [];
  #lineEls = [];
  #active = -2;
  #hue = 265;
  #palette = this.#vizPalette(null);
  #returnFocus = null;
  #userScrollUntil = 0;
  #lyricsTimer = null;
  #frame = 0;
  #levels = new Float32Array(NowPlaying.BANDS);
  #targets = new Float32Array(NowPlaying.BANDS);
  #spectrumAt = 0;
  #artCache = new Map();
  #closeTimer = null;

  constructor() {
    $("#npClose").onclick = () => this.close();
    const opener = $("#nowOpen");
    opener.addEventListener("click", () => this.open(opener));
    opener.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        this.open(opener);
      }
    });
    this.#lyrics.addEventListener("click", (e) => {
      const line = e.target.closest(".np-line");
      if (!line || !player.duration) return;
      player.currentTime = +line.dataset.start;
      this.#userScrollUntil = 0;
      this.#active = -2;
    });
    for (const type of ["wheel", "touchmove", "pointerdown"]) {
      this.#lyrics.addEventListener(type, () => (this.#userScrollUntil = performance.now() + NowPlaying.USER_SCROLL_PAUSE), { passive: true });
    }
    this.#lyrics.addEventListener("keydown", (e) => {
      if (["ArrowUp", "ArrowDown", "PageUp", "PageDown", "Home", "End"].includes(e.key))
        this.#userScrollUntil = performance.now() + NowPlaying.USER_SCROLL_PAUSE;
    });
    tauri?.event?.listen("wave-spectrum", (e) => this.#onSpectrum(e.payload));
    document.addEventListener("visibilitychange", () => this.#setVisualizer(this.#open && !document.hidden));
    const bar = $(".player");
    const measure = () => document.documentElement.style.setProperty("--player-h", `${Math.max(0, window.innerHeight - bar.getBoundingClientRect().top)}px`);
    new ResizeObserver(measure).observe(bar);
    window.addEventListener("resize", measure);
    measure();
  }

  get isOpen() {
    return this.#open;
  }

  open(trigger) {
    if (this.#open || !state.currentId || !state.tracks.has(state.currentId)) return;
    clearTimeout(this.#closeTimer);
    this.#returnFocus = document.activeElement;
    const source = trigger?.matches?.(".art, .now-art") ? trigger : trigger?.querySelector?.(".art, .now-art");
    this.#open = true;
    const show = () => {
      this.#el.hidden = false;
      this.#el.classList.remove("leaving");
      this.#el.classList.add("entering");
      this.#load(true);
      requestAnimationFrame(() => $("#npClose").focus({ preventScroll: true }));
    };
    if (this.#canAnimate(source)) {
      source.style.viewTransitionName = "np-art";
      const transition = document.startViewTransition(() => {
        source.style.viewTransitionName = "";
        show();
        this.#art.style.viewTransitionName = "np-art";
      });
      transition.finished.finally(() => (this.#art.style.viewTransitionName = ""));
    } else {
      this.#el.classList.add("no-vt");
      show();
    }
    setTimeout(() => this.#el.classList.remove("entering", "no-vt"), 900);
    this.#setVisualizer(true);
    this.#loop();
  }

  close() {
    if (!this.#open) return;
    const target = this.#closeTarget();
    this.#open = false;
    const finish = () => {
      this.#el.hidden = true;
      this.#el.classList.remove("leaving", "entering");
      clearTimeout(this.#lyricsTimer);
      cancelAnimationFrame(this.#frame);
      this.#setVisualizer(false);
    };
    if (this.#canAnimate(target)) {
      this.#art.style.viewTransitionName = "np-art";
      const transition = document.startViewTransition(() => {
        this.#art.style.viewTransitionName = "";
        finish();
        target.style.viewTransitionName = "np-art";
      });
      transition.finished.finally(() => (target.style.viewTransitionName = ""));
    } else if (reduceMotion.matches) {
      finish();
    } else {
      this.#el.classList.add("leaving");
      this.#closeTimer = setTimeout(finish, 260);
    }
    const remembered = this.#returnFocus;
    const back = remembered?.isConnected && remembered !== document.body && remembered.getClientRects().length ? remembered : $("#nowOpen");
    back?.focus({ preventScroll: true });
  }

  sync() {
    if (!this.#open) return;
    if (!state.currentId || !state.tracks.has(state.currentId)) return this.close();
    this.#load(false);
  }

  #canAnimate(el) {
    return !reduceMotion.matches && typeof document.startViewTransition === "function" && !!el?.isConnected && el.getClientRects().length > 0;
  }

  #closeTarget() {
    const art = $(`#trackList .row[data-id="${CSS.escape(state.currentId || "")}"] .art`);
    if (art) {
      const r = art.getBoundingClientRect();
      const view = $("#main").getBoundingClientRect();
      if (r.height && r.top >= view.top + 60 && r.bottom <= view.bottom) return art;
    }
    return $("#nowArt");
  }

  #load(force) {
    const id = state.currentId;
    if (!force && id === this.#trackId) return;
    this.#trackId = id;
    const t = state.tracks.get(id);
    this.#hue = hue(t.artist + t.title);
    this.#palette = this.#vizPalette(this.#artCache.get(id)?.color);
    this.#el.style.setProperty("--h", this.#hue);
    this.#art.style.setProperty("--h", this.#hue);
    $("#npTitle").textContent = t.title;
    $("#npArtist").textContent = t.album ? `${t.artist} · ${t.album}` : t.artist;
    $("#npSource").textContent = queueSourceLabel(state.queue.source || "");
    this.#art.innerHTML = `<span class="np-lo" data-cover="${esc(t.coverKey || "")}">${coverHTML(t)}</span>`;
    this.#setBackground(t.coverKey ? coverCache.get(t.coverKey) : null);
    this.#loadArtwork(id);
    this.#lines = [];
    this.#lineEls = [];
    this.#active = -2;
    this.#lyrics.classList.remove("synced");
    this.#lyrics.innerHTML = "";
    this.#loadLyrics(id);
  }

  #setBackground(url) {
    this.#bg.classList.toggle("has-art", !!url);
    this.#bg.style.backgroundImage = url ? `url("${url}")` : "";
  }

  #vizPalette(color) {
    const fallback = { hue: this.#hue + 30, sat: 95, light: 78, baseHue: this.#hue, baseSat: 85, baseLight: 58, glowHue: this.#hue + 10 };
    if (typeof color !== "number" || color < 0) return fallback;
    const r = ((color >> 16) & 255) / 255;
    const g = ((color >> 8) & 255) / 255;
    const b = (color & 255) / 255;
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const chroma = max - min;
    if (chroma < NowPlaying.MIN_ART_CHROMA)
      return { hue: 0, sat: 0, light: 92, baseHue: 0, baseSat: 0, baseLight: 64, glowHue: 0, glowSat: 0 };
    let h;
    if (max === r) h = ((g - b) / chroma) % 6;
    else if (max === g) h = (b - r) / chroma + 2;
    else h = (r - g) / chroma + 4;
    const hueDeg = Math.round((h * 60 + 360) % 360);
    return { hue: hueDeg + 12, sat: 92, light: 72, baseHue: hueDeg, baseSat: 82, baseLight: 56, glowHue: hueDeg + 6 };
  }

  async #loadArtwork(id) {
    let art = this.#artCache.get(id);
    if (art === undefined) {
      try {
        const data = await invoke("get_artwork", { id });
        art = data?.url ? { url: URL.createObjectURL(await (await fetch(data.url)).blob()), color: data.color } : null;
      } catch {
        art = null;
      }
      this.#artCache.set(id, art);
      if (this.#artCache.size > NowPlaying.ART_CACHE) {
        const [oldId, oldArt] = this.#artCache.entries().next().value;
        if (oldId !== this.#trackId && oldArt?.url) URL.revokeObjectURL(oldArt.url);
        this.#artCache.delete(oldId);
      }
    }
    if (id !== this.#trackId || !this.#open) return;
    this.#palette = this.#vizPalette(art?.color);
    const url = art?.url;
    if (!url) return;
    const img = new Image();
    img.className = "hi";
    img.alt = "";
    img.draggable = false;
    img.src = url;
    await img.decode().catch(() => {});
    if (id !== this.#trackId) return;
    this.#art.appendChild(img);
    requestAnimationFrame(() => img.classList.add("ready"));
    this.#setBackground(url);
  }

  async #loadLyrics(id) {
    clearTimeout(this.#lyricsTimer);
    let data;
    try {
      data = await invoke("get_lyrics", { id });
    } catch {
      data = { status: "unavailable", lines: [] };
    }
    if (id !== this.#trackId || !this.#open) return;
    const lines = (data.lines || []).filter((l) => l.text);
    if (lines.length) {
      this.#lines = lines;
      this.#lyrics.innerHTML = lines
        .map((l, i) => `<button class="np-line" tabindex="-1" data-index="${i}" data-start="${l.start}">${esc(l.text)}</button>`)
        .join("");
      this.#lineEls = [...this.#lyrics.children];
      this.#lyrics.classList.add("synced");
      this.#active = -2;
      this.#syncLyrics();
      return;
    }
    this.#lyrics.classList.remove("synced");
    if (data.status === "pending") {
      this.#lyrics.innerHTML = this.#note("pending", "music", "Lyrics are on the way", "Wave is listening to this song and writing down its lyrics. They'll appear here in a few seconds.");
      this.#lyricsTimer = setTimeout(() => this.#loadLyrics(id), NowPlaying.LYRICS_POLL);
    } else if (data.status === "none") {
      this.#lyrics.innerHTML = this.#note("", "music", "No lyrics", "This one sounds instrumental. Enjoy the music.");
    } else {
      this.#lyrics.innerHTML = this.#note("", "music", "Lyrics aren't available", "Wave couldn't find lyrics for this song.");
    }
  }

  #note(kind, iconName, title, text) {
    return `<div class="np-note ${kind}">${icon(iconName)}<h3>${esc(title)}</h3><p>${esc(text)}</p></div>`;
  }

  #syncLyrics() {
    if (!this.#lines.length) return;
    const position = player.livePosition + NowPlaying.LYRIC_LEAD;
    let lo = 0;
    let hi = this.#lines.length - 1;
    let index = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (this.#lines[mid].start <= position) {
        index = mid;
        lo = mid + 1;
      } else hi = mid - 1;
    }
    if (index === this.#active) return;
    this.#active = index;
    this.#lineEls.forEach((el, i) => {
      el.classList.toggle("active", i === index);
      el.classList.toggle("past", i < index);
      if (i === index) el.setAttribute("aria-current", "true");
      else el.removeAttribute("aria-current");
    });
    if (performance.now() < this.#userScrollUntil) return;
    const target = this.#lineEls[Math.max(index, 0)];
    const top = target.offsetTop - this.#lyrics.clientHeight * 0.42 + target.offsetHeight / 2;
    this.#lyrics.scrollTo({ top, behavior: reduceMotion.matches ? "auto" : "smooth" });
  }

  #setVisualizer(enabled) {
    invoke("set_visualizer", { enabled }).catch(() => {});
    if (!enabled) this.#targets.fill(0);
  }

  #onSpectrum(bands) {
    if (!this.#open || !Array.isArray(bands)) return;
    for (let i = 0; i < NowPlaying.BANDS; i++) this.#targets[i] = (bands[i] || 0) / 255;
    this.#spectrumAt = performance.now();
  }

  #loop() {
    cancelAnimationFrame(this.#frame);
    const tick = () => {
      if (!this.#open) return;
      this.#syncLyrics();
      this.#drawVisualizer();
      this.#frame = requestAnimationFrame(tick);
    };
    this.#frame = requestAnimationFrame(tick);
  }

  #drawVisualizer() {
    const canvas = this.#canvas;
    const ctx = this.#ctx;
    const dpr = window.devicePixelRatio || 1;
    const width = Math.round(canvas.clientWidth * dpr);
    const height = Math.round(canvas.clientHeight * dpr);
    if (!width || !height) return;
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const live = !player.paused && performance.now() - this.#spectrumAt < 400;
    const calm = reduceMotion.matches;
    const attack = calm ? 0.2 : 0.55;
    const release = calm ? 0.05 : 0.13;
    for (let i = 0; i < NowPlaying.BANDS; i++) {
      const target = live ? this.#targets[i] : 0;
      const level = this.#levels[i];
      this.#levels[i] = level + (target - level) * (target > level ? attack : release);
    }

    ctx.clearRect(0, 0, width, height);
    const count = NowPlaying.BANDS;
    const gap = Math.max(2, Math.round(width / count / 4));
    const barWidth = (width - gap * (count - 1)) / count;
    const baseline = height * 0.74;
    const minBar = 2 * dpr;
    const gradient = ctx.createLinearGradient(0, 0, 0, baseline);
    const p = this.#palette;
    gradient.addColorStop(0, `hsl(${p.hue} ${p.sat}% ${p.light}%)`);
    gradient.addColorStop(1, `hsl(${p.baseHue} ${p.baseSat}% ${p.baseLight}%)`);
    ctx.fillStyle = gradient;
    ctx.shadowColor = calm ? "transparent" : `hsl(${p.glowHue} ${p.glowSat ?? 95}% 70% / .55)`;
    ctx.shadowBlur = calm ? 0 : 14 * dpr;
    const radius = Math.min(barWidth / 2, 3 * dpr);
    for (let i = 0; i < count; i++) {
      const h = Math.max(minBar, Math.pow(this.#levels[i], 1.25) * baseline * 0.96);
      const x = i * (barWidth + gap);
      ctx.beginPath();
      ctx.roundRect(x, baseline - h, barWidth, h, [radius, radius, 0, 0]);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    ctx.globalAlpha = 0.22;
    for (let i = 0; i < count; i++) {
      const h = Math.max(minBar * 0.5, Math.pow(this.#levels[i], 1.25) * (height - baseline) * 0.9);
      ctx.fillRect(i * (barWidth + gap), baseline + dpr, barWidth, h);
    }
    ctx.globalAlpha = 1;
  }
}

const nowPlaying = new NowPlaying();

// ---------------------------------------------------------------------------
// Downloader
// ---------------------------------------------------------------------------
function fmtCount(n) {
  n = Number(n) || 0;
  for (const [size, unit] of [[1e9, "B"], [1e6, "M"], [1e3, "K"]]) {
    if (n >= size) {
      const v = n / size;
      return `${v >= 100 ? Math.round(v) : v.toFixed(1).replace(/\.0$/, "")}${unit}`;
    }
  }
  return String(n);
}

const DOWNLOADS_FOLDER = "Downloads";
const isBusy = (job) => job?.status === "queued" || job?.status === "downloading";

class Downloader {
  #query = "";
  #status = "idle";
  #error = "";
  #results = [];
  #rendered = null;
  #jobs = new Map();
  #token = 0;
  #previews = new Map();
  #loadingPreview = null;
  #previewToken = 0;

  constructor() {
    tauri?.event?.listen("wave-download", (e) => this.#onProgress(e.payload));
  }

  get query() {
    return this.#query;
  }

  get isOpen() {
    return state.view.type === "downloader";
  }

  get activeCount() {
    let n = 0;
    for (const job of this.#jobs.values()) if (isBusy(job)) n++;
    return n;
  }

  summary() {
    const parts = [];
    if (this.#status === "done" && this.#query) {
      const n = this.#results.length;
      parts.push(`${n} ${n === 1 ? "result" : "results"} for “${this.#query}”`);
    } else {
      parts.push("Search SoundCloud and save tracks to your Library");
    }
    const active = this.activeCount;
    if (active) parts.push(`${active} downloading`);
    return parts.join(" · ");
  }

  async search(text, force = false) {
    const query = String(text || "").trim();
    if (!force && query === this.#query && (this.#status === "loading" || this.#status === "done")) return;
    this.#query = query;
    const token = ++this.#token;
    if (!query) {
      this.#status = "idle";
      this.#results = [];
      this.render();
      return;
    }
    this.#status = "loading";
    this.render();
    try {
      const reply = await invoke("downloader_search", { query });
      if (token !== this.#token) return;
      this.#results = Array.isArray(reply?.results) ? reply.results : [];
      this.#status = "done";
    } catch (err) {
      if (token !== this.#token) return;
      this.#results = [];
      this.#error = errText(err);
      this.#status = "error";
    }
    this.render();
    if (this.isOpen) $("#main").scrollTop = 0;
  }

  previewInfo(fileId) {
    if (!fileId?.startsWith(PREVIEW_PREFIX)) return null;
    return this.#previews.get(fileId.slice(PREVIEW_PREFIX.length)) || null;
  }

  async togglePreview(id) {
    const live = player.fileId === PREVIEW_PREFIX + id;
    if (live && player.paused === false) {
      player.pause();
      return;
    }
    if (live && player.status !== "ended") {
      player.play().catch(() => {});
      return;
    }
    const result = this.#results.find((r) => r.id === id);
    if (result) this.#previews.set(id, { title: result.title, artist: result.artist, thumbnailUrl: result.thumbnailUrl });
    const token = ++this.#previewToken;
    this.#loadingPreview = id;
    this.paintPreview();
    try {
      await invoke("preview_track", { id });
    } catch (err) {
      if (token === this.#previewToken && errText(err) !== "superseded") toast(`Couldn't preview “${result?.title ?? "this track"}”: ${errText(err)}`);
    } finally {
      if (token === this.#previewToken) {
        this.#loadingPreview = null;
        this.paintPreview();
      }
    }
  }

  paintPreview() {
    const playing = !player.paused;
    for (const row of $("#dlList").children) {
      const btn = row.querySelector(".dl-play");
      if (!btn) continue;
      const loading = this.#loadingPreview === row.dataset.id;
      const live = !loading && player.fileId === PREVIEW_PREFIX + row.dataset.id;
      row.classList.toggle("previewing", live || loading);
      btn.classList.toggle("loading", loading);
      btn.classList.toggle("live", live);
      const want = loading ? "spinner" : live && playing ? "pause" : "play";
      if (btn.dataset.state === want) continue;
      btn.dataset.state = want;
      btn.innerHTML = want === "spinner" ? `<span class="spinner"></span>` : icon(want);
      btn.ariaLabel = `${want === "pause" ? "Pause" : "Preview"} ${row.querySelector(".t-title").textContent}`;
    }
  }

  press(id) {
    const job = this.#jobs.get(id);
    if (isBusy(job)) return;
    if (job?.status === "done") {
      setView({ type: "library", path: [DOWNLOADS_FOLDER] });
      return;
    }
    this.download(id);
  }

  async download(id) {
    const result = this.#results.find((r) => r.id === id);
    this.#setJob(id, { status: "queued", progress: 0, error: "", title: result?.title || "this track" });
    try {
      if (!(await invoke("downloader_download", { id }))) throw "search for this track again, then retry";
    } catch (err) {
      this.#setJob(id, { status: "error", error: errText(err) });
      toast(`Couldn't download “${this.#jobs.get(id).title}”: ${errText(err)}`);
    }
  }

  async #onProgress(e) {
    if (!e?.id) return;
    const prev = this.#jobs.get(e.id);
    if (prev?.status === "done" && e.status !== "done") return;
    this.#setJob(e.id, { status: e.status, progress: e.progress || 0, error: e.error || "", fresh: e.status === "done" });
    const title = this.#jobs.get(e.id).title || "track";
    if (e.status === "done") {
      toast(`Downloaded “${title}” to your Library`);
      await loadLibrary();
    } else if (e.status === "error") {
      toast(`Couldn't download “${title}”: ${e.error || "unknown error"}`);
    }
  }

  #setJob(id, patch) {
    this.#jobs.set(id, { ...(this.#jobs.get(id) || {}), ...patch });
    this.#paintRow(id);
    $("#dlCount").textContent = this.activeCount || "";
    if (this.isOpen) $("#viewSub").textContent = this.summary();
  }

  #paintRow(id) {
    const row = [...$("#dlList").children].find((el) => el.dataset.id === id);
    const result = this.#results.find((r) => r.id === id);
    if (!row || !result) return;
    const job = this.#jobs.get(id);
    const btn = row.querySelector(".dl-btn");
    if (job?.status === "downloading" && btn?.classList.contains("downloading")) {
      const pct = Math.round((job.progress || 0) * 100);
      btn.style.setProperty("--p", `${pct}%`);
      btn.querySelector(".dl-label").textContent = pct ? `${pct}%` : "Starting…";
      return;
    }
    row.classList.toggle("is-done", job?.status === "done");
    row.querySelector(".col-dl").innerHTML = this.#buttonHTML(result, job);
    row.querySelector(".t-artist").outerHTML = this.#metaHTML(result, job);
    if (job?.fresh) job.fresh = false;
  }

  #buttonHTML(r, job) {
    const name = esc(r.title);
    switch (job?.status) {
      case "queued":
        return `<button class="btn dl-btn busy" aria-busy="true" aria-label="Waiting to download ${name}"><span class="spinner"></span><span class="dl-label">Waiting…</span></button>`;
      case "downloading": {
        const pct = Math.round((job.progress || 0) * 100);
        return `<button class="btn dl-btn busy downloading" style="--p:${pct}%" aria-busy="true" aria-label="Downloading ${name}"><span class="spinner"></span><span class="dl-label">${pct ? `${pct}%` : "Starting…"}</span></button>`;
      }
      case "done":
        return `<button class="btn dl-btn done${job.fresh ? " fresh" : ""}" title="Open in Library" aria-label="${name} is in your Library">${icon("check")}<span>In Library</span></button>`;
      case "error":
        return `<button class="btn dl-btn failed" title="${esc(job.error || "The download failed")} — click to retry" aria-label="Retry downloading ${name}">${icon("retry")}<span>Retry</span></button>`;
      default:
        return `<button class="btn dl-btn" aria-label="Download ${name}">${icon("download")}<span>Download</span></button>`;
    }
  }

  #metaHTML(r, job) {
    if (job?.status === "error") {
      const msg = esc(job.error || "The download failed");
      return `<span class="t-artist dl-err" title="${msg}">${msg}</span>`;
    }
    return `<span class="t-artist">${esc(r.artist)}</span>`;
  }

  #rowHTML(r, i) {
    const job = this.#jobs.get(r.id);
    const art = r.thumbnailUrl ? `<img src="${esc(r.thumbnailUrl)}" alt="" draggable="false" loading="lazy" onerror="this.remove()">` : "";
    return `<li class="dl-row${job?.status === "done" ? " is-done" : ""}" data-id="${esc(r.id)}">
      <span class="col-num"><span class="num">${i + 1}</span></span>
      <span class="col-title">
        <span class="art" style="--h:${hue(r.artist + r.title)}">${icon("music")}${art}<button class="dl-play" aria-label="Preview ${esc(r.title)}">${icon("play")}</button></span>
        <span class="tt">
          <span class="t-title" title="${esc(r.title)}">${esc(r.title)}</span>
          ${this.#metaHTML(r, job)}
        </span>
      </span>
      <span class="col-stat" title="${(r.likeCount || 0).toLocaleString()} likes">${icon("heart")}${fmtCount(r.likeCount)}</span>
      <span class="col-stat" title="${(r.repostCount || 0).toLocaleString()} reposts">${icon("repost")}${fmtCount(r.repostCount)}</span>
      <span class="col-dur">${r.durationMs ? fmtTime(r.durationMs / 1000) : "–"}</span>
      <span class="col-dl">${this.#buttonHTML(r, job)}</span>
    </li>`;
  }

  #stateHTML() {
    const q = esc(this.#query);
    switch (this.#status) {
      case "loading":
        return `<div class="empty-icon"><span class="spinner"></span></div>
          <h2>Searching SoundCloud…</h2>
          <p>Looking for “${q}”.</p>`;
      case "error":
        return `<div class="empty-icon">${icon("alert")}</div>
          <h2>Search failed</h2>
          <p>${esc(this.#error)}</p>
          <div class="empty-actions"><button class="btn" data-dl-retry>${icon("retry")}<span>Try again</span></button></div>`;
      case "done":
        return `<div class="empty-icon">${icon("search")}</div>
          <h2>No results</h2>
          <p>Nothing on SoundCloud that can be downloaded matches “${q}”.</p>`;
      default:
        return `<div class="empty-icon">${icon("download")}</div>
          <h2>Find music to download</h2>
          <p>Search SoundCloud for a song or artist. Downloads are saved to Wave's data folder and show up in your Library.</p>
          <p class="hint">Type above and press Enter</p>`;
    }
  }

  render() {
    if (!this.isOpen) return;
    const box = $("#dlState");
    const showList = this.#status === "done" && this.#results.length > 0;
    $("#dlHead").hidden = !showList;
    box.hidden = showList;
    box.classList.toggle("error", this.#status === "error");
    if (showList) {
      if (this.#rendered !== this.#results) {
        $("#dlList").innerHTML = this.#results.map((r, i) => this.#rowHTML(r, i)).join("");
        this.#rendered = this.#results;
        this.paintPreview();
      }
    } else {
      $("#dlList").innerHTML = "";
      this.#rendered = null;
      box.innerHTML = this.#stateHTML();
    }
    $("#viewSub").textContent = this.summary();
  }
}

const downloader = new Downloader();

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
const BACKEND_LABELS = { starting: "Backend starting", connected: "Backend connected", offline: "Backend offline" };
let backendState = "";

function setBackendStatus(next) {
  if (!BACKEND_LABELS[next] || next === backendState) return;
  backendState = next;
  const el = $("#backendStatus");
  el.dataset.state = next;
  el.setAttribute("aria-label", BACKEND_LABELS[next]);
  if (next === "connected") loadFromBackend();
}

async function bindBackend() {
  await tauri?.event?.listen("wave-backend-status", (e) => setBackendStatus(e.payload));
  try {
    setBackendStatus(await invoke("get_backend_status"));
  } catch {
    setBackendStatus("offline");
  }
}

async function init() {
  bindUI();
  bindDragDrop();
  bindMediaSession();
  renderModes();
  renderVolume();
  renderNow();
  render();
  bindBackend();
}

async function loadFromBackend() {
  try {
    const settings = await invoke("get_settings");
    const playback = settings.playback || {};
    if (typeof playback.volume === "number") state.volume = Math.min(1, Math.max(0, playback.volume));
    if (typeof playback.muted === "boolean") state.muted = playback.muted;
    renderVolume();
    if (typeof settings.interface?.playlistCovers === "boolean") state.playlistCovers = settings.interface.playlistCovers;
    $("#playlistCoversSetting").checked = state.playlistCovers;
    if (settings.interface?.showQueue) {
      const app = $(".app");
      app.classList.add("no-anim");
      toggleQueuePanel(true, false);
      requestAnimationFrame(() => requestAnimationFrame(() => app.classList.remove("no-anim")));
    }
  } catch {}
  await loadLibrary();
  await loadPlaylists();
  try {
    applyQueue(await invoke("get_queue"));
    player.applyState(await invoke("get_player_state"));
    renderPlayState();
  } catch {}
  prefetchCovers();
}

init();
