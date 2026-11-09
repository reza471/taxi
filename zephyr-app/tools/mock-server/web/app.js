/* Taxi mock server - admin UI. Plain JavaScript, no external files.
   Everything that comes from the meter (or from the internet) is escaped with esc() before it is shown. */
"use strict";
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = v => String(v ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const fmt = n => (n == null || n === "" ? "-" : Number(n).toLocaleString("en-US"));
const toman = rial => (rial == null ? "-" : fmt(Math.floor(rial / 10)));
const pad = n => String(n).padStart(2, "0");
const hms = ts => { const d = new Date(ts * 1000); return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`; };
const dtime = ts => { const d = new Date(ts * 1000); return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${hms(ts)}`; };
const ago = ts => { if (!ts) return "never"; const s = Math.max(0, Date.now() / 1000 - ts); if (s < 60) return `${Math.floor(s)} s ago`;
  if (s < 3600) return `${Math.floor(s / 60)} min ago`; if (s < 86400) return `${Math.floor(s / 3600)} h ago`; return `${Math.floor(s / 86400)} d ago`; };

async function api(method, path, body, raw) {
  const opt = { method, headers: {} };
  if (body !== undefined) {
    if (raw) opt.body = body; else { opt.body = JSON.stringify(body); opt.headers["Content-Type"] = "application/json"; }
  }
  const r = await fetch(path, opt);
  if (!r.ok) throw new Error((await r.text()) || r.statusText);
  return r.headers.get("content-type")?.includes("json") ? r.json() : r.text();
}
function toast(msg, bad) {
  const t = document.createElement("div"); t.className = "toast" + (bad ? " bad" : ""); t.textContent = msg;
  $("#toasts").appendChild(t); setTimeout(() => t.remove(), bad ? 6000 : 2800);
}
async function act(fn, okMsg) { try { const r = await fn(); if (okMsg) toast(okMsg); return r; } catch (e) { toast(e.message, true); } }

/* ---------------------------------------------------------------- icons */
const I = {
  dash: '<path d="M3 3h7v9H3zM14 3h7v5h-7zM14 12h7v9h-7zM3 16h7v5H3z"/>',
  dev: '<rect x="5" y="5" width="14" height="14" rx="2"/><path d="M9 9h6v6H9zM9 1v3M15 1v3M9 20v3M15 20v3M1 9h3M1 15h3M20 9h3M20 15h3"/>',
  trip: '<path d="M5 17h14M6 17l1.5-6h9L18 17M8 11l1-4h6l1 4"/><circle cx="8" cy="18" r="1.6"/><circle cx="16" cy="18" r="1.6"/>',
  req: '<path d="M4 6h16M4 12h16M4 18h10"/>',
  tag: '<path d="M20.6 13.4l-7.2 7.2a2 2 0 0 1-2.8 0L3 13V3h10l7.6 7.6a2 2 0 0 1 0 2.8z"/><circle cx="7.5" cy="7.5" r="1.5"/>',
  zap: '<path d="M13 2L3 14h9l-1 8 10-12h-9z"/>',
  fw: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  lock: '<rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
  gear: '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/>',
  user: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>', plus: '<path d="M12 5v14M5 12h14"/>',
  dl: '<path d="M12 3v12M7 10l5 5 5-5M5 21h14"/>', trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>',
  refresh: '<path d="M21 12a9 9 0 1 1-2.6-6.4M21 3v6h-6"/>', copy: '<rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/>',
};
const icon = (n, cls = "") => `<svg viewBox="0 0 24 24" class="${cls}">${I[n]}</svg>`;

const PAGES = [
  ["dashboard", "Dashboard", "dash"], ["drivers", "Drivers", "user"], ["devices", "Devices", "dev"], ["trips", "Trips", "trip"], ["requests", "Live log", "req"],
  ["tariff", "Tariff", "tag"], ["scenarios", "Trouble makers", "zap"], ["firmware", "Firmware (OTA)", "fw"],
  ["time", "Time (NTP)", "clock"], ["tls", "TLS", "lock"], ["settings", "Settings", "gear"],
];
let META = null, STATE = null, timers = [];
const every = (ms, fn) => { timers.push(setInterval(fn, ms)); };

/* ---------------------------------------------------------------- tooltip */
const tip = $("#tip");
document.addEventListener("mousemove", e => { if (tip.style.display === "block") { tip.style.left = e.clientX + 14 + "px"; tip.style.top = e.clientY + 12 + "px"; } });
document.addEventListener("mouseover", e => { const t = e.target.closest("[data-tip]"); if (t) { tip.innerHTML = t.dataset.tip; tip.style.display = "block"; } });
document.addEventListener("mouseout", e => { if (e.target.closest("[data-tip]")) tip.style.display = "none"; });

/* ---------------------------------------------------------------- charts (SVG) */
function stackedBars(data, keys, colors, { h = 190, labels = [], unit = "" } = {}) {
  const W = 640, H = h, L = 34, B = 22, T = 8, n = data.length, bw = (W - L - 6) / n;
  const max = Math.max(1, ...data.map(d => keys.reduce((s, k) => s + (d[k] || 0), 0)));
  const nice = Math.ceil(max / 4) * 4 || 4, y = v => H - B - (v / nice) * (H - B - T);
  let s = `<svg class="chart" viewBox="0 0 ${W} ${H}"><g class="grid">`;
  for (let i = 0; i <= 4; i++) { const v = nice * i / 4; s += `<line x1="${L}" x2="${W}" y1="${y(v)}" y2="${y(v)}"/><text x="${L - 6}" y="${y(v) + 3}" text-anchor="end">${fmt(v)}</text>`; }
  s += "</g>";
  data.forEach((d, i) => {
    let acc = 0; const x = L + i * bw + 1, tipTxt = [`<b>${esc(labels[i] ?? "")}</b>`, ...keys.map(k => `${k}: ${fmt(d[k] || 0)}${unit}`)].join("<br>");
    keys.forEach((k, j) => { const v = d[k] || 0; if (!v) return; const y1 = y(acc + v), y0 = y(acc); acc += v;
      s += `<rect x="${x}" y="${y1}" width="${Math.max(1, bw - 2)}" height="${Math.max(0.5, y0 - y1)}" rx="1.5" fill="${colors[j]}"/>`; });
    s += `<rect x="${x}" y="${T}" width="${bw}" height="${H - B - T}" fill="transparent" data-tip="${esc(tipTxt)}"/>`;
    if (labels[i] && (i % Math.ceil(n / 8) === 0 || i === n - 1)) s += `<text x="${x + bw / 2}" y="${H - 6}" text-anchor="middle">${esc(labels[i])}</text>`;
  });
  return s + "</svg>";
}
function comboChart(days) {
  const W = 640, H = 210, L = 34, R = 50, B = 24, T = 10, n = days.length, bw = (W - L - R) / n;
  const maxT = Math.max(1, ...days.map(d => d.trips)), maxM = Math.max(1, ...days.map(d => d.toman));
  const nT = Math.ceil(maxT / 4) * 4 || 4, yT = v => H - B - (v / nT) * (H - B - T), yM = v => H - B - (v / maxM) * (H - B - T);
  let s = `<svg class="chart" viewBox="0 0 ${W} ${H}"><g class="grid">`;
  for (let i = 0; i <= 4; i++) { const v = nT * i / 4; s += `<line x1="${L}" x2="${W - R}" y1="${yT(v)}" y2="${yT(v)}"/><text x="${L - 6}" y="${yT(v) + 3}" text-anchor="end">${v}</text>`;
    s += `<text x="${W - R + 6}" y="${yT(v) + 3}">${fmt(Math.round(maxM * i / 4))}</text>`; }
  s += "</g>"; let line = "";
  days.forEach((d, i) => { const x = L + i * bw;
    s += `<rect x="${x + bw * .18}" y="${yT(d.trips)}" width="${bw * .64}" height="${H - B - yT(d.trips)}" rx="3" fill="var(--c1)" opacity=".85"/>`;
    line += `${i ? "L" : "M"}${x + bw / 2},${yM(d.toman)}`;
    s += `<rect x="${x}" y="${T}" width="${bw}" height="${H - B - T}" fill="transparent" data-tip="${esc(`<b>${d.day}</b><br>trips: ${d.trips}<br>${fmt(d.toman)} Toman`)}"/>`;
    if (i % 2 === 0 || i === n - 1) s += `<text x="${x + bw / 2}" y="${H - 7}" text-anchor="middle">${d.day.slice(5)}</text>`; });
  s += `<path d="${line}" fill="none" stroke="var(--c2)" stroke-width="2.4"/>`;
  days.forEach((d, i) => { s += `<circle cx="${L + i * bw + bw / 2}" cy="${yM(d.toman)}" r="3" fill="var(--c2)"/>`; });
  return s + "</svg>";
}
function donut(items, colors) {
  const total = items.reduce((s, x) => s + x.n, 0); if (!total) return `<div class="empty">no calls in the last 24 h</div>`;
  let a0 = -Math.PI / 2, s = `<svg class="chart" viewBox="0 0 200 200" style="max-width:190px;margin:auto">`;
  items.forEach((it, i) => { const a1 = a0 + 2 * Math.PI * it.n / total, big = a1 - a0 > Math.PI ? 1 : 0, r = 80, ri = 52;
    const p = (a, rr) => `${100 + rr * Math.cos(a)},${100 + rr * Math.sin(a)}`;
    const d = it.n === total ? `M100,20 A80,80 0 1 1 99.99,20 L99.99,48 A52,52 0 1 0 100,48 Z`
      : `M${p(a0, r)} A${r},${r} 0 ${big} 1 ${p(a1, r)} L${p(a1, ri)} A${ri},${ri} 0 ${big} 0 ${p(a0, ri)} Z`;
    s += `<path d="${d}" fill="${colors[i % colors.length]}" data-tip="${esc(`${it.kind}: ${it.n}`)}"/>`; a0 = a1; });
  s += `<text x="100" y="98" text-anchor="middle" style="font-size:26px;font-weight:700;fill:var(--text)">${fmt(total)}</text><text x="100" y="118" text-anchor="middle">calls / 24 h</text></svg>`;
  return s;
}
const legend = (names, colors) => `<div class="legend">${names.map((n, i) => `<span><i style="background:${colors[i]}"></i>${esc(n)}</span>`).join("")}</div>`;

/* ---------------------------------------------------------------- drawer */
function openDrawer(title, html) { $("#dtitle").textContent = title; $("#dbody").innerHTML = html; $("#drawer").classList.add("open"); $("#shade").style.display = "block"; }
function closeDrawer() { $("#drawer").classList.remove("open"); $("#shade").style.display = "none"; }
$("#dclose").onclick = closeDrawer; $("#shade").onclick = closeDrawer; document.addEventListener("keydown", e => { if (e.key === "Escape") closeDrawer(); });

/* ---------------------------------------------------------------- shell */
function kindBadge(k) { return `<span class="badge k-${esc(k)}">${esc(k)}</span>`; }
function statusBadge(s, fault) {
  if (fault) return `<span class="badge bad">${esc(fault)}</span>`;
  if (!s) return `<span class="badge bad">no answer</span>`;
  const c = s < 300 ? "ok" : s < 400 ? "info" : s < 500 ? "warn" : "bad"; return `<span class="badge ${c}">${s}</span>`;
}
function activeTroubles(sc, st) {
  const out = [];
  if (sc.tariff !== "ok") out.push(`tariff: <b>${esc(sc.tariff)}</b>`);
  if (sc.post !== "ok") out.push(`records: <b>${esc(sc.post)}</b>`);
  if (sc.fw !== "ok") out.push(`firmware: <b>${esc(sc.fw)}</b>`);
  if (sc.transport !== "normal") out.push(`transport: <b>${esc(sc.transport)}</b> (${esc(sc.transport_target)})`);
  if (sc.delay_s > 0) out.push(`delay <b>${sc.delay_s} s</b>`);
  if (sc.fail_percent > 0) out.push(`fail <b>${sc.fail_percent} %</b>`);
  if (st && st.time && st.time.offset_s) out.push(`meter time shifted <b>${Math.round(st.time.offset_s / 60)} min</b>`);
  if (st && st.tls && st.tls.profile !== "rsa") out.push(`TLS: <b>${esc(st.tls.profile)}</b>`);
  return out;
}
async function resetTroubles() {
  await api("PUT", "/admin/api/scenario", { tariff: "ok", post: "ok", fw: "ok", transport: "normal", transport_target: "all", delay_s: 0, fail_percent: 0 });
  await api("PUT", "/admin/api/time", { offset_s: 0 }); await api("PUT", "/admin/api/tls", { profile: "rsa" });
  toast("everything back to normal"); STATE = await api("GET", "/admin/api/state"); route();
}
function banner() {
  const t = activeTroubles(STATE.scenario, STATE);
  return t.length ? `<div class="banner">${icon("zap")}<div class="grow"><b>Trouble makers are on:</b> ${t.join(" &middot; ")}</div><button class="btn sm" onclick="resetTroubles()">Reset all</button></div>` : "";
}
async function chips() {
  try {
    const s = await api("GET", "/admin/api/summary");
    const online = s.kpi.devices_online;
    $("#chips").innerHTML = `<span class="chip"><span class="dot ${online ? "ok" : ""}"></span>${online} meter${online === 1 ? "" : "s"} online</span>
      <span class="chip" data-tip="time the meter gets from the NTP server">${icon("clock").replace("<svg", '<svg style="width:14px;height:14px;stroke:currentColor;fill:none;stroke-width:2"')} ${hms(s.meter_time)}</span>
      <span class="chip hm">TLS ${esc(s.tls_profile)}</span>`;
    const n = s.kpi.problems_day; const c = $('#nav a[href="#requests"] .count');
    if (c) c.remove(); if (n) $('#nav a[href="#requests"]').insertAdjacentHTML("beforeend", `<span class="count">${n > 99 ? "99+" : n}</span>`);
  } catch (e) { $("#chips").innerHTML = `<span class="chip"><span class="dot bad"></span>server not reachable</span>`; }
}
function nav() {
  $("#nav").innerHTML = PAGES.map(([id, name, ic]) => `<a href="#${id}">${icon(ic)}<span>${name}</span></a>`).join("");
}
async function route() {
  timers.forEach(clearInterval); timers = []; closeDrawer();
  const id = (location.hash || "#dashboard").slice(1).split("?")[0], page = PAGES.find(p => p[0] === id) || PAGES[0];
  $$("#nav a").forEach(a => a.classList.toggle("on", a.getAttribute("href") === "#" + page[0]));
  $("#title").textContent = page[1]; document.title = `${page[1]} - Taxi Mock`;
  STATE = await api("GET", "/admin/api/state");
  try { await VIEWS[page[0]](); } catch (e) { $("#view").innerHTML = `<div class="card"><b>Error:</b> ${esc(e.message)}</div>`; }
}
$("#theme").onclick = () => { const r = document.documentElement, t = r.dataset.theme === "dark" ? "light" : "dark"; r.dataset.theme = t; try { localStorage.setItem("theme", t); } catch (e) {} route(); };

/* ================================================================ views */
const VIEWS = {};

VIEWS.dashboard = async () => {
  const draw = async () => {
    const s = await api("GET", "/admin/api/summary"), k = s.kpi;
    const labels = s.per_min.map((_, i) => { const d = new Date((s.now - (59 - i) * 60) * 1000); return `${pad(d.getHours())}:${pad(d.getMinutes())}`; });
    const kcolors = ["var(--c1)", "var(--c4)", "var(--c5)", "var(--c2)", "var(--c3)", "var(--c6)"];
    $("#view").innerHTML = banner() + `
      <div class="grid g-kpi">
        ${kpiCard("Meters online", k.devices_online, `${k.devices} known, ${k.unregistered} without driver`, k.devices_online ? "ok" : "")}
        ${kpiCard("Calls / hour", k.requests_hour, `${k.ntp_answers_hour} NTP answers`)}
        ${kpiCard("Trips today", k.trips_today, `${fmt(k.toman_today)} Toman`)}
        ${kpiCard("Problems / 24 h", k.problems_day, "records with an error", k.problems_day ? "bad" : "ok")}
        ${kpiCard("Fare mismatches", k.fare_mismatch, `${k.duplicates} duplicates`, k.fare_mismatch ? "bad" : "")}
        ${kpiCard("TLS failures / 24 h", k.tls_failures_day, `<span data-tip="${esc(s.last_tls || "")}">last: ${esc((s.last_tls || "no TLS yet").split(" ")[0])}</span>`, k.tls_failures_day ? "warn" : "")}
      </div>
      <div class="grid g-2 mt">
        <div class="card"><div class="head"><div><h3>Calls per minute</h3><p class="sub">last 60 minutes</p></div></div>
          ${stackedBars(s.per_min, ["ok", "err", "bad"], ["var(--c4)", "var(--c2)", "var(--c3)"], { labels })}
          ${legend(["answered (2xx/3xx)", "error answer (4xx/5xx)", "no answer / cut / TLS fail"], ["var(--c4)", "var(--c2)", "var(--c3)"])}</div>
        <div class="card"><div class="head"><div><h3>Calls by type</h3><p class="sub">last 24 hours</p></div></div>
          ${donut(s.kinds, kcolors)}${legend(s.kinds.map(x => x.kind), kcolors)}</div>
      </div>
      <div class="grid g-2 mt">
        <div class="card"><div class="head"><div><h3>Trips and income</h3><p class="sub">last 14 days, duplicates not counted</p></div></div>
          ${comboChart(s.days)}${legend(["trips", "income (Toman)"], ["var(--c1)", "var(--c2)"])}</div>
        <div class="card"><div class="head"><div><h3>Latest problems</h3><p class="sub">click to open the live log</p></div></div>
          ${s.recent_problems.length ? `<ul class="list">${s.recent_problems.map(r => `<li>${kindBadge(r.kind)}<div class="t"><div class="trunc">${esc(r.problems[0] || "")}</div>
          <div class="s">${hms(r.ts)} &middot; ${esc(r.serial || "no serial")}${r.problems.length > 1 ? ` &middot; +${r.problems.length - 1} more` : ""}</div></div></li>`).join("")}</ul>`
          : `<div class="empty">no problems ${icon("zap", "").replace("<svg", '<svg style="display:none"')}</div>`}
          <div class="row end mt"><a class="btn sm" href="#requests?problems=1">open problems</a></div></div>
      </div>`;
    $$(".list li", $("#view")).forEach(li => li.style.cursor = "pointer");
    $$(".list li", $("#view")).forEach(li => li.onclick = () => { location.hash = "#requests?problems=1"; });
  };
  await draw(); every(4000, draw);
};
const kpiCard = (label, val, hint, cls = "") => `<div class="card kpi ${cls}"><div class="label">${label}</div><div class="val">${fmt(val)}</div><div class="hint">${hint}</div></div>`;


/* ---------------------------------------------------------------- drivers */
VIEWS.drivers = async () => {
  META = META || await api("GET", "/admin/api/meta");
  const draw = async () => {
    const d = await api("GET", "/admin/api/drivers");
    const lic = r => r.license_until ? (r.days_left < 0 ? `<span class="badge bad">ended ${esc(r.license_until)}</span>`
      : r.days_left <= 7 ? `<span class="badge warn">${esc(r.license_until)} (${r.days_left} d)</span>` : `<span class="badge ok">${esc(r.license_until)}</span>`) : `<span class="muted">no limit</span>`;
    $("#view").innerHTML = banner() + `
      <div class="row"><p class="muted small grow">When a meter connects, the server finds its driver by the serial and sends that driver's prices, services and WiFi.
        A switched-off driver or an ended license gets HTTP 403 (the meter shows E-403).</p>
        <a class="btn sm" href="/admin/api/export/drivers.csv">${icon("dl")} CSV</a><button class="btn pri" id="dadd">${icon("plus")} Add driver</button></div>
      <div class="tablewrap mt"><table><thead><tr><th></th><th>Driver</th><th class="hm">Plate</th><th class="hm">Device</th><th class="hm">Tariff</th><th>License</th>
        <th class="num">Today</th><th class="num hm">All trips</th><th>On</th></tr></thead><tbody>
      ${d.rows.length ? d.rows.map(r => `<tr class="click" data-id="${r.id}">
        <td><span class="dot ${r.online ? "ok" : ""}" data-tip="${r.online ? "meter online" : r.last_seen ? "last seen " + esc(ago(r.last_seen)) : "never seen"}"></span></td>
        <td><b>${esc(r.name)}</b><div class="small muted">${esc(r.phone || "")}${r.car ? " &middot; " + esc(r.car) : ""}</div></td>
        <td class="hm mono">${esc(r.plate || "-")}</td><td class="mono small hm">${esc(r.serial || "-")}</td>
        <td class="hm">${r.profile ? `<span class="badge acc">${esc(r.profile)}</span>` : `<span class="muted">main</span>`}${r.overrides ? ` <span class="badge" data-tip="${esc(r.overrides)}">own prices</span>` : ""}</td>
        <td>${lic(r)}</td><td class="num">${r.trips_today} <div class="small muted">${fmt(r.toman_today)} T</div></td>
        <td class="num hm">${r.trips} <div class="small muted">${fmt(r.toman)} T</div></td>
        <td><label class="switch" onclick="event.stopPropagation()"><input type="checkbox" data-act="${r.id}" ${r.active ? "checked" : ""}><i></i></label></td></tr>`).join("")
      : `<tr><td colspan="9" class="empty">No driver yet. Add one, or press <b>Register</b> next to a meter on the Devices page.</td></tr>`}
      </tbody></table></div>
      <div class="card mt"><h3>Meters without a driver</h3><p class="sub">What a meter gets when no driver is linked to its serial (${d.free_devices.length} such meter(s) seen).</p>
        <div class="seg" id="unreg">${Object.entries(META.unregistered_modes).map(([k, v]) => `<button data-v="${k}" class="${d.unregistered === k ? "on" : ""}" data-tip="${esc(v)}">${esc(k)}</button>`).join("")}</div>
        <p class="small muted mt">${esc(META.unregistered_modes[d.unregistered] || "")}</p></div>`;
    $("#dadd").onclick = () => driverDrawer({ active: 1 }, d.free_devices);
    $$("tr[data-id]").forEach(tr => tr.onclick = () => driverDrawer(d.rows.find(x => x.id == tr.dataset.id), d.free_devices));
    $$("[data-act]").forEach(x => x.onchange = () => act(() => api("PUT", `/admin/api/drivers/${x.dataset.act}`, { active: x.checked }), x.checked ? "driver on" : "driver off (gets 403)"));
    $$("#unreg button").forEach(b => b.onclick = async () => { if (await act(() => api("PUT", "/admin/api/tariff", { unregistered: b.dataset.v }), "saved")) draw(); });
  };
  await draw(); every(8000, () => { if (!$("#drawer").classList.contains("open")) draw(); });
};
async function driverDrawer(r, freeDevices) {
  const prof = await api("GET", "/admin/api/profiles");
  if (!freeDevices) freeDevices = (await api("GET", "/admin/api/drivers")).free_devices;
  const isNew = !r.id, j = v => { try { return v ? JSON.stringify(JSON.parse(v), null, 2) : ""; } catch (e) { return v; } };
  const svc = (() => { try { return r.services ? JSON.parse(r.services).join(", ") : ""; } catch (e) { return ""; } })();
  const wifi = (() => { try { return r.wifi ? JSON.parse(r.wifi) : {}; } catch (e) { return {}; } })();
  openDrawer(isNew ? "New driver" : r.name, `
    <div class="fields" style="grid-template-columns:1fr 1fr">
      <label class="f"><span>Name *</span><input type="text" id="dn" value="${esc(r.name || "")}"></label>
      <label class="f"><span>Phone</span><input type="text" id="dp" value="${esc(r.phone || "")}"></label>
      <label class="f"><span>Plate</span><input type="text" id="dpl" value="${esc(r.plate || "")}"></label>
      <label class="f"><span>Car</span><input type="text" id="dc" value="${esc(r.car || "")}"></label>
      <label class="f"><span>Device serial (12 hex)</span><input type="text" id="ds" list="dslist" value="${esc(r.serial || "")}" placeholder="from INFO page of the meter">
        <datalist id="dslist">${freeDevices.map(x => `<option value="${esc(x)}">`).join("")}</datalist></label>
      <label class="f"><span>License until</span><input type="date" id="dl" value="${esc(r.license_until || "")}" style="width:100%"></label>
      <label class="f"><span>Tariff profile</span><select id="dprof"><option value="">main tariff</option>${prof.rows.map(p => `<option value="${p.id}" ${p.id == r.profile_id ? "selected" : ""}>${esc(p.name)}</option>`).join("")}</select></label>
      <label class="f"><span>&nbsp;</span><label class="switch"><input type="checkbox" id="da" ${r.active ? "checked" : ""}><i></i><span>driver can work</span></label></label>
    </div>
    <div class="section-title">Only for this driver (optional)</div>
    <label class="f"><span>Own prices: only the keys you write, e.g. {"pri_cost": 30000}</span><textarea id="dov" rows="4">${esc(j(r.overrides))}</textarea></label>
    <label class="f mt"><span>Service numbers (empty = the main list)</span><input type="text" id="dsv" value="${esc(svc)}" style="width:100%"></label>
    <div class="fields mt" style="grid-template-columns:1fr 1fr"><label class="f"><span>WiFi name (empty = main)</span><input type="text" id="dw" value="${esc(wifi.wifi || "")}"></label>
      <label class="f"><span>WiFi password</span><input type="text" id="dwp" value="${esc(wifi.wifiPass || "")}"></label></div>
    <label class="f mt"><span>Note</span><input type="text" id="dno" value="${esc(r.note || "")}" style="width:100%"></label>
    ${isNew ? "" : `<div class="section-title">What this meter gets when it connects</div><div id="dprev" class="muted small">loading ...</div>
      <div class="row mt"><a class="btn sm" href="#trips?driver=${r.id}">trips of this driver</a>${r.serial ? `<a class="btn sm" href="#requests?serial=${encodeURIComponent(r.serial)}">calls of the meter</a>` : ""}</div>`}
    <div class="row mt2">${isNew ? "" : `<button class="btn danger" id="ddel">${icon("trash")} Delete</button>`}<span class="grow"></span>
      <button class="btn" onclick="closeDrawer()">Cancel</button><button class="btn pri" id="dsave">${isNew ? "Add driver" : "Save"}</button></div>`);
  if (!isNew) api("GET", `/admin/api/drivers/${r.id}/preview`).then(p => {
    $("#dprev").innerHTML = p.block ? `<span class="badge bad">HTTP ${p.block[0]}</span> ${esc(p.block[1])}<br><span class="small">the meter shows E-403</span>`
      : p.tariff ? `<div class="small">from: ${esc(p.source.join(" + "))}</div><pre class="json mt">${esc(JSON.stringify(p.tariff, null, 2))}</pre>` : esc(p.note || "");
  }).catch(e => $("#dprev").textContent = e.message);
  $("#dsave").onclick = async () => {
    const body = { name: $("#dn").value, phone: $("#dp").value, plate: $("#dpl").value, car: $("#dc").value, serial: $("#ds").value,
      license_until: $("#dl").value, profile_id: $("#dprof").value || null, active: $("#da").checked, overrides: $("#dov").value,
      services: $("#dsv").value, wifi: $("#dw").value ? { wifi: $("#dw").value, wifiPass: $("#dwp").value } : "", note: $("#dno").value };
    const res = await act(() => isNew ? api("POST", "/admin/api/drivers", body) : api("PUT", `/admin/api/drivers/${r.id}`, body), isNew ? "driver added" : "saved");
    if (res) { closeDrawer(); route(); }
  };
  if (!isNew) $("#ddel").onclick = async () => { if (confirm(`Delete ${r.name}? Trips stay in the log.`) && await act(() => api("DELETE", `/admin/api/drivers/${r.id}`), "deleted")) { closeDrawer(); route(); } };
}

/* ---------------------------------------------------------------- devices */
VIEWS.devices = async () => {
  const draw = async () => {
    const d = await api("GET", "/admin/api/devices");
    $("#view").innerHTML = banner() + `
      <div class="row"><p class="muted small grow">A meter appears here after its first call. Mark one as <b>expired</b> to test E-403 on that meter only, or give it its own prices.</p>
      <a class="btn sm" href="/admin/api/export/devices.csv">${icon("dl")} CSV</a></div>
      <div class="tablewrap mt"><table><thead><tr><th></th><th>Serial</th><th>Driver</th><th>Last seen</th><th class="hm">IP</th><th class="hm">TLS</th><th class="num hm">Calls</th><th class="num">Trips</th>
      <th class="num">Problems</th><th>Own prices</th><th>Expired</th><th></th></tr></thead><tbody>
      ${d.rows.length ? d.rows.map(r => `<tr>
        <td><span class="dot ${r.online ? "ok" : ""}" data-tip="${r.online ? "seen in the last 10 min" : "offline"}"></span></td>
        <td class="mono"><a href="#requests?serial=${encodeURIComponent(r.serial)}">${esc(r.serial)}</a>${r.note ? `<div class="small muted">${esc(r.note)}</div>` : ""}</td>
        <td>${r.driver ? `<a href="#drivers">${esc(r.driver.name)}</a>` : `<button class="btn sm pri" data-reg="${esc(r.serial)}">${icon("plus")} Register</button>`}</td>
        <td data-tip="${esc(r.last_seen ? dtime(r.last_seen) : "")}">${ago(r.last_seen)}</td><td class="mono hm">${esc(r.last_ip)}</td><td class="small hm">${esc(r.last_tls || "-")}</td>
        <td class="num hm">${fmt(r.requests)}</td><td class="num"><a href="#trips?serial=${encodeURIComponent(r.serial)}">${fmt(r.trips)}</a></td>
        <td class="num">${r.problems ? `<span class="badge bad">${r.problems}</span>` : "0"}</td>
        <td>${r.tariff_override ? `<span class="badge acc" data-tip="${esc(r.tariff_override)}">yes</span>` : `<span class="muted">-</span>`}
          <button class="btn sm" data-ov="${esc(r.serial)}">edit</button></td>
        <td><label class="switch red"><input type="checkbox" data-exp="${esc(r.serial)}" ${r.expired ? "checked" : ""}><i></i></label></td>
        <td><button class="btn sm danger" data-del="${esc(r.serial)}" title="forget this device">${icon("trash")}</button></td></tr>`).join("")
      : `<tr><td colspan="12" class="empty">No meter yet. Build the meter with this server's address (see Settings) and switch it on.</td></tr>`}
      </tbody></table></div>`;
    $$("[data-exp]").forEach(x => x.onchange = () => act(() => api("PUT", `/admin/api/devices/${encodeURIComponent(x.dataset.exp)}`, { expired: x.checked }), x.checked ? "marked as expired" : "license active again"));
    $$("[data-del]").forEach(x => x.onclick = async () => { if (confirm("Forget this device? (its calls and trips stay in the log)")) { await act(() => api("DELETE", `/admin/api/devices/${encodeURIComponent(x.dataset.del)}`)); draw(); } });
    $$("[data-ov]").forEach(x => x.onclick = () => overrideDrawer(d.rows.find(r => r.serial === x.dataset.ov)));
    $$("[data-reg]").forEach(x => x.onclick = () => driverDrawer({ serial: x.dataset.reg, active: 1 }));
  };
  await draw(); every(8000, () => { if (!$("#drawer").classList.contains("open")) draw(); });
};
function overrideDrawer(dev) {
  openDrawer(`Own prices for ${dev.serial}`, `
    <p class="muted small">Only the keys you write here replace the main tariff for this meter, for example <code>{"pri_cost": 30000, "korsi": 0}</code>. Empty = main tariff.</p>
    <textarea id="ovtxt" rows="10">${esc(dev.tariff_override ? JSON.stringify(JSON.parse(dev.tariff_override), null, 2) : "")}</textarea>
    <label class="f mt"><span>Note</span><input type="text" id="ovnote" value="${esc(dev.note || "")}"></label>
    <div class="row end mt"><button class="btn" onclick="closeDrawer()">Cancel</button><button class="btn pri" id="ovsave">Save</button></div>`);
  $("#ovsave").onclick = async () => {
    const t = $("#ovtxt").value.trim(); let ov = "";
    if (t) { try { ov = JSON.parse(t); if (typeof ov !== "object" || Array.isArray(ov)) throw 0; } catch (e) { return toast("not a JSON object", true); } }
    if (await act(() => api("PUT", `/admin/api/devices/${encodeURIComponent(dev.serial)}`, { tariff_override: ov, note: $("#ovnote").value }), "saved")) { closeDrawer(); route(); }
  };
}

/* ---------------------------------------------------------------- trips */
VIEWS.trips = async () => {
  const qs = new URLSearchParams(location.hash.split("?")[1] || "");
  const f = { serial: qs.get("serial") || "", check: qs.get("check") || "", dup: qs.get("dup") || "", driver: qs.get("driver") || "" };
  $("#view").innerHTML = banner() + `
    <div class="row">
      <input type="search" id="tserial" placeholder="serial" value="${esc(f.serial)}" style="width:170px">
      <div class="seg" id="tcheck">${[["", "All"], ["ok", "Fare OK"], ["mismatch", "Mismatch"]].map(([v, n]) => `<button data-v="${v}" class="${f.check === v ? "on" : ""}">${n}</button>`).join("")}</div>
      <label class="switch"><input type="checkbox" id="tdup" ${f.dup ? "checked" : ""}><i></i><span class="small">duplicates only</span></label>
      <span class="grow"></span><a class="btn sm" href="/admin/api/export/trips.csv">${icon("dl")} CSV</a></div>
    <div class="tablewrap mt"><table><thead><tr><th>Time</th><th class="hm">Serial</th><th>Driver</th><th>Meter</th><th class="hm">Start - stop</th><th class="num">km</th><th class="num hm">min</th>
      <th class="num hm">Wait</th><th class="num">Toman</th><th class="num hm">Expected</th><th>Check</th></tr></thead><tbody id="tbody"></tbody></table></div>`;
  const load = async () => {
    const p = new URLSearchParams(); if (f.serial) p.set("serial", f.serial); if (f.check) p.set("check", f.check); if (f.dup) p.set("dup", "1"); if (f.driver) p.set("driver", f.driver); p.set("limit", 300);
    const d = await api("GET", "/admin/api/trips?" + p);
    $("#tbody").innerHTML = d.rows.length ? d.rows.map(r => { const ok = r.fare_check.startsWith("ok"), mm = r.fare_check.startsWith("mismatch");
      return `<tr class="click ${r.problems.length ? "prob" : ""}" data-id="${r.id}"><td><span class="hm">${dtime(r.ts).slice(5, 10)} </span>${hms(r.ts).slice(0, 5)}</td><td class="mono hm">${esc(r.serial)}</td>
      <td>${r.driver ? esc(r.driver) : `<span class="muted">-</span>`}</td><td>${r.bt === 5 ? "single" : "ch " + r.bt}</td><td class="mono hm">${esc(r.start_time)} - ${esc(r.stop_time)}</td>
      <td class="num">${(r.distance / 1000).toFixed(1)}</td><td class="num hm">${Math.round(r.duration / 60)}</td><td class="num hm">${Math.round(r.wait / 60)} min</td>
      <td class="num"><b>${toman(r.total)}</b>${r.night_price ? ` <span class="badge acc" data-tip="night part ${toman(r.night_price)} Toman">night</span>` : ""}</td>
      <td class="num small muted hm">${toman(r.expected_low)}-${toman(r.expected_high)}</td>
      <td>${ok ? `<span class="badge ok">OK</span>` : mm ? `<span class="badge bad">wrong</span>` : `<span class="badge">?</span>`}
        ${r.duplicate_of ? `<span class="badge warn" data-tip="same trip as #${r.duplicate_of}">dup #${r.duplicate_of}</span>` : ""}</td></tr>`; }).join("")
      : `<tr><td colspan="11" class="empty">No trips yet. A trip appears when the meter sends its trip end.</td></tr>`;
    $$("#tbody tr[data-id]").forEach(tr => tr.onclick = () => { const r = d.rows.find(x => x.id == tr.dataset.id); tripDrawer(r); });
  };
  const upd = () => { const p = new URLSearchParams(); Object.entries(f).forEach(([k, v]) => v && p.set(k, v)); history.replaceState(null, "", "#trips" + (p.toString() ? "?" + p : "")); load(); };
  $("#tserial").oninput = e => { f.serial = e.target.value.trim(); upd(); };
  $$("#tcheck button").forEach(b => b.onclick = () => { f.check = b.dataset.v; $$("#tcheck button").forEach(x => x.classList.toggle("on", x === b)); upd(); });
  $("#tdup").onchange = e => { f.dup = e.target.checked ? "1" : ""; upd(); };
  await load(); every(6000, () => { if (!$("#drawer").classList.contains("open")) load(); });
};
function tripDrawer(r) {
  openDrawer(`Trip #${r.id}`, `
    <div class="kv"><div>Driver</div><div>${esc(r.driver || "not registered")}</div><div>Serial</div><div class="mono">${esc(r.serial)}</div><div>Meter</div><div>${r.bt === 5 ? "single meter" : "channel " + r.bt}</div>
    <div>Start - stop</div><div>${esc(r.start_time)} - ${esc(r.stop_time)}</div><div>Distance</div><div>${fmt(r.distance)} m</div>
    <div>Duration</div><div>${fmt(r.duration)} s</div><div>Waiting</div><div>${fmt(r.wait)} s</div><div>Speed avg / max</div><div>${r.avg_speed} / ${r.max_speed} km/h</div>
    <div>Trip price</div><div>${fmt(r.trip_price)} Rial</div><div>Waiting price</div><div>${fmt(r.traffic_price)} Rial</div><div>Night part</div><div>${fmt(r.night_price)} Rial</div>
    <div>Total</div><div><b>${fmt(r.total)} Rial</b> (${toman(r.total)} Toman)</div><div>Expected</div><div>${fmt(r.expected_low)} .. ${fmt(r.expected_high)} Rial</div></div>
    <div class="section-title">Fare check</div><p>${esc(r.fare_check)}</p>
    ${r.problems.length ? `<div class="section-title">Problems</div><ul>${r.problems.map(p => `<li class="small">${esc(p)}</li>`).join("")}</ul>` : `<p class="badge ok">no problems</p>`}
    <p class="muted small mt">The check recomputes the fare from the tariff this meter downloaded. It catches most errors above about 5 %.</p>`);
}

/* ---------------------------------------------------------------- live log */
VIEWS.requests = async () => {
  const qs = new URLSearchParams(location.hash.split("?")[1] || "");
  const f = { kind: qs.get("kind") || "", serial: qs.get("serial") || "", problems: qs.get("problems") || "", q: "" };
  let live = true, rows = [];
  const kinds = ["", "TARIFF", "START", "END", "SERVICE", "FIRMWARE", "TLS"];
  $("#view").innerHTML = banner() + `
    <div class="row">
      <div class="seg" id="rkind">${kinds.map(k => `<button data-v="${k}" class="${f.kind === k ? "on" : ""}">${k || "All"}</button>`).join("")}</div>
      <input type="search" id="rserial" placeholder="serial" value="${esc(f.serial)}" style="width:150px">
      <input type="search" id="rq" placeholder="search in data" style="width:170px">
      <label class="switch"><input type="checkbox" id="rprob" ${f.problems ? "checked" : ""}><i></i><span class="small">problems only</span></label>
      <span class="grow"></span>
      <label class="switch"><input type="checkbox" id="rlive" checked><i></i><span class="small">live</span></label>
      <a class="btn sm" href="/admin/api/export/requests.csv">${icon("dl")} CSV</a></div>
    <p class="muted small" id="rtotal"></p>
    <div class="tablewrap"><table><thead><tr><th>Time</th><th>Type</th><th>Serial</th><th>Answer</th><th class="num hm">ms</th><th class="hm">TLS</th><th>Data / problems</th></tr></thead><tbody id="rbody"></tbody></table></div>
    <div class="row mt" style="justify-content:center"><button class="btn" id="rmore">Load older</button></div>`;
  const rowHtml = r => {
    const summary = r.problems.length ? `<span style="color:var(--bad)">${esc(r.problems.join("; "))}</span>`
      : r.notes.length ? `<span style="color:var(--warn)">${esc(r.notes.join("; "))}</span>` + ` <span class="muted">${esc(paramsShort(r))}</span>` : `<span class="muted">${esc(paramsShort(r))}</span>`;
    return `<tr class="click ${r.problems.length ? "prob" : r.notes.length ? "note" : ""}" data-id="${r.id}"><td class="mono" data-tip="${esc(dtime(r.ts))}">${hms(r.ts)}</td>
      <td>${kindBadge(r.kind)}</td><td class="mono">${esc(r.serial || "-")}</td><td>${statusBadge(r.status, r.fault)}</td><td class="num hm">${r.ms ?? ""}</td>
      <td class="small muted hm">${esc((r.tls || "").replace("_WITH_", " "))}</td><td><div class="trunc">${summary}</div></td></tr>`;
  };
  const load = async (more) => {
    const p = new URLSearchParams(); Object.entries(f).forEach(([k, v]) => v && p.set(k, v)); p.set("limit", 150);
    if (more && rows.length) p.set("before", rows[rows.length - 1].id);
    const d = await api("GET", "/admin/api/requests?" + p);
    rows = more ? rows.concat(d.rows) : d.rows;
    $("#rbody").innerHTML = rows.length ? rows.map(rowHtml).join("") : `<tr><td colspan="7" class="empty">Nothing yet. Every call of the meter shows up here at once.</td></tr>`;
    $("#rtotal").textContent = `${fmt(d.total)} call(s) match. Red = the record has an error, yellow = note (scenario, transport fault, order).`;
    $("#rmore").style.display = rows.length < d.total ? "" : "none";
    $$("#rbody tr[data-id]").forEach(tr => tr.onclick = () => reqDrawer(rows.find(x => x.id == tr.dataset.id)));
  };
  const upd = () => { const p = new URLSearchParams(); ["kind", "serial", "problems"].forEach(k => f[k] && p.set(k, f[k])); history.replaceState(null, "", "#requests" + (p.toString() ? "?" + p : "")); load(); };
  $$("#rkind button").forEach(b => b.onclick = () => { f.kind = b.dataset.v; $$("#rkind button").forEach(x => x.classList.toggle("on", x === b)); upd(); });
  $("#rserial").oninput = e => { f.serial = e.target.value.trim(); upd(); };
  $("#rq").oninput = e => { f.q = e.target.value.trim(); load(); };
  $("#rprob").onchange = e => { f.problems = e.target.checked ? "1" : ""; upd(); };
  $("#rlive").onchange = e => { live = e.target.checked; };
  $("#rmore").onclick = () => load(true);
  await load(); every(2500, () => { if (live && !$("#drawer").classList.contains("open") && rows.length <= 150) load(); });
};
function paramsShort(r) {
  const p = r.params || {};
  if (r.kind === "END") return `total ${p.total_price ?? "?"}, ${p.trip_distance ?? "?"} m, ${p.start_time ?? "?"}-${p.stop_time ?? "?"}, ch ${p.bt_number ?? "?"}`;
  if (r.kind === "START") return `channel ${p.bt_number ?? "?"}`;
  if (r.kind === "SERVICE") return `service ${p.serviceNum ?? "?"}, ${p.start === "1" ? "begin" : p.start === "2" ? "end" : p.start}`;
  if (r.kind === "FIRMWARE") return `${p.file ?? ""} ${p.bytes ? fmt(p.bytes) + " bytes" : ""}`;
  return Object.entries(p).filter(([k]) => k !== "serial").map(([k, v]) => `${k}=${v}`).join(" ");
}
function reqDrawer(r) {
  openDrawer(`${r.kind} #${r.id}`, `
    <div class="kv"><div>Time</div><div>${dtime(r.ts)}</div><div>Call</div><div class="mono">${esc(r.method)} ${esc(r.path)}</div>
    <div>Serial</div><div class="mono">${esc(r.serial || "-")}</div><div>From</div><div class="mono">${esc(r.remote)}</div>
    <div>Answer</div><div>${statusBadge(r.status, r.fault)}</div><div>Time taken</div><div>${r.ms ?? "-"} ms</div><div>TLS</div><div class="small">${esc(r.tls || "-")}</div></div>
    ${r.problems.length ? `<div class="section-title">Problems</div><ul>${r.problems.map(p => `<li style="color:var(--bad)">${esc(p)}</li>`).join("")}</ul>` : ""}
    ${r.notes.length ? `<div class="section-title">Notes</div><ul>${r.notes.map(p => `<li>${esc(p)}</li>`).join("")}</ul>` : ""}
    <div class="section-title">Data</div><pre class="json">${esc(JSON.stringify(r.params, null, 2))}</pre>`);
}

/* ---------------------------------------------------------------- tariff */
VIEWS.tariff = async () => {
  META = META || await api("GET", "/admin/api/meta");
  const t = STATE.tariff, groups = {};
  META.tariff_fields.forEach(f => (groups[f.group] = groups[f.group] || []).push(f));
  $("#view").innerHTML = banner() + `
    <div class="row"><p class="muted small grow">These are the prices every meter downloads (a meter can have its own prices on the Devices page). After saving, press <b>TARF</b> in the meter menu, or wait up to 10 minutes.</p>
      <div class="seg" id="tmode"><button class="on" data-v="form">Form</button><button data-v="json">JSON</button></div></div>
    <div id="tform" class="grid g-2e mt">${Object.entries(groups).map(([g, fs]) => `<div class="card"><h3>${esc(g)}</h3><div class="fields mt">
      ${fs.map(f => `<label class="f"><span>${esc(f.label)}</span><input type="number" data-k="${esc(f.key)}" value="${esc(t[f.key] ?? 0)}"></label>`).join("")}</div></div>`).join("")}
      <div class="card"><h3>Services and WiFi</h3><div class="fields mt" style="grid-template-columns:1fr">
        <label class="f"><span>Service numbers (SAMR page), comma separated</span><input type="text" id="tsvc" value="${esc(STATE.services.join(", "))}"></label>
        <label class="f"><span>WiFi name sent to the meter (empty = do not send)</span><input type="text" id="twifi" value="${esc(STATE.wifi.wifi)}"></label>
        <label class="f"><span>WiFi password</span><input type="text" id="twpass" value="${esc(STATE.wifi.wifiPass)}"></label></div></div>
    </div>
    <div id="tjson" style="display:none" class="mt"><textarea id="tjtxt" rows="22">${esc(JSON.stringify(t, null, 2))}</textarea></div>
    <div class="row end mt"><button class="btn" id="treset">Default prices</button><button class="btn pri" id="tsave">Save tariff</button></div>
    <div class="section-title">Tariff profiles</div>
    <div class="row"><p class="muted small grow">A profile changes some prices of the main tariff (for example a city or an airport tariff). Give it to drivers on the Drivers page.</p>
      <button class="btn" id="padd">${icon("plus")} New profile</button></div>
    <div class="tiles mt" id="plist"></div>`;
  let mode = "form";
  $$("#tmode button").forEach(b => b.onclick = () => { mode = b.dataset.v; $$("#tmode button").forEach(x => x.classList.toggle("on", x === b));
    $("#tform").style.display = mode === "form" ? "" : "none"; $("#tjson").style.display = mode === "json" ? "" : "none";
    if (mode === "json") $("#tjtxt").value = JSON.stringify(collect(), null, 2); });
  const collect = () => { const o = { ...STATE.tariff }; $$("#tform [data-k]").forEach(i => o[i.dataset.k] = i.value === "" ? 0 : Number(i.value)); return o; };
  $("#tsave").onclick = async () => {
    let tariff; if (mode === "json") { try { tariff = JSON.parse($("#tjtxt").value); } catch (e) { return toast("JSON error: " + e.message, true); } } else tariff = collect();
    const services = $("#tsvc").value.split(",").map(x => x.trim()).filter(Boolean).map(Number);
    if (services.some(isNaN)) return toast("service numbers must be numbers", true);
    if (await act(() => api("PUT", "/admin/api/tariff", { tariff, services, wifi: { wifi: $("#twifi").value, wifiPass: $("#twpass").value } }), "tariff saved")) route();
  };
  const profs = await api("GET", "/admin/api/profiles");
  $("#plist").innerHTML = profs.rows.length ? profs.rows.map(p => `<div class="tile" data-pid="${p.id}"><b>${esc(p.name)}</b>
    <span>${Object.keys(p.tariff).length ? esc(Object.entries(p.tariff).map(([k, v]) => k + " " + v).join(", ")) : "no change"}</span>
    <div class="meta"><span class="badge">${p.drivers} driver(s)</span></div></div>`).join("") : `<div class="muted small">no profile yet</div>`;
  const profEdit = p => { openDrawer(p ? p.name : "New tariff profile", `
      <label class="f"><span>Name</span><input type="text" id="pn" value="${esc(p ? p.name : "")}" style="width:100%"></label>
      <label class="f mt"><span>Prices that differ from the main tariff (JSON). Keys: ${esc(META.tariff_fields.map(f => f.key).join(", "))}</span>
      <textarea id="pt" rows="10">${esc(p ? JSON.stringify(p.tariff, null, 2) : '{\n  "pri_cost": 30000\n}')}</textarea></label>
      <div class="row mt2">${p ? `<button class="btn danger" id="pdel">${icon("trash")} Delete</button>` : ""}<span class="grow"></span><button class="btn" onclick="closeDrawer()">Cancel</button><button class="btn pri" id="psave">Save</button></div>`);
    $("#psave").onclick = async () => { let t; try { t = JSON.parse($("#pt").value || "{}"); } catch (e) { return toast("JSON error: " + e.message, true); }
      const body = { name: $("#pn").value, tariff: t };
      if (await act(() => p ? api("PUT", `/admin/api/profiles/${p.id}`, body) : api("POST", "/admin/api/profiles", body), "profile saved")) { closeDrawer(); route(); } };
    if (p) $("#pdel").onclick = async () => { if (confirm(`Delete profile ${p.name}? Its drivers go back to the main tariff.`) && await act(() => api("DELETE", `/admin/api/profiles/${p.id}`))) { closeDrawer(); route(); } }; };
  $("#padd").onclick = () => profEdit(null);
  $$("#plist [data-pid]").forEach(t => t.onclick = () => profEdit(profs.rows.find(p => p.id == t.dataset.pid)));
  $("#treset").onclick = async () => { if (!confirm("Put the default prices back?")) return;
    await act(() => api("PUT", "/admin/api/tariff", { tariff: { PM: 46, pri_cost: 26200, Start_night: 22, Stop_night: 6, Cost_200: 1318, Cost_stop: 8000, Max_stop_km: 20,
      free_trip: 0, free_time: 0, Percent_night: 10, fraction: 50, taxi: 1, pri2_cost: 5000, Cost2_200: 384, Cost2_stop: 334, samar: 1, darbast: 1, korsi: 1, nobat: 1,
      trip3: 0, Cost3_200: 0, Cost3_stop: 0 } }), "default prices"); route(); };
};

/* ---------------------------------------------------------------- scenarios */
VIEWS.scenarios = async () => {
  META = META || await api("GET", "/admin/api/meta");
  const sc = STATE.scenario;
  const badKeys = new Set(["expired", "error500", "html502", "bad_json", "out_of_range", "huge_number", "pm_zero", "empty", "bad_request", "truncated", "corrupt", "wrong_magic", "not_found", "partial", "drop", "reset", "stall", "redirect302"]);
  const tiles = (name, map, cur) => `<div class="tiles" data-group="${name}">${Object.entries(map).map(([k, d]) =>
    `<div class="tile ${k === cur ? "on" : ""} ${k !== cur ? "" : badKeys.has(k) ? "bad" : (k === "ok" || k === "normal") ? "" : "warn"}" data-k="${esc(k)}"><b>${esc(k)}</b><span>${esc(d)}</span></div>`).join("")}</div>`;
  $("#view").innerHTML = banner() + `
    <p class="muted small">Pick what the server does wrong on purpose. The meter must survive all of it: keep its old tariff, keep trips in its queue and send them later, refuse bad firmware.</p>
    <div class="section-title">Tariff answer (GET TaxiFare)</div>${tiles("tariff", META.tariff_scenarios, sc.tariff)}
    <div class="section-title">Answer to trip / service records</div>${tiles("post", META.post_scenarios, sc.post)}
    <div class="section-title">Transport: how the bytes arrive</div>
    <div class="row" style="margin-bottom:10px"><span class="small muted">Apply to</span><div class="seg" id="ttarget">${Object.entries(META.transport_targets).map(([k, d]) =>
      `<button data-v="${k}" class="${sc.transport_target === k ? "on" : ""}" data-tip="${esc(d)}">${k}</button>`).join("")}</div></div>
    ${tiles("transport", META.transport_modes, sc.transport)}
    <div class="section-title">Firmware download</div>${tiles("fw", META.fw_scenarios, sc.fw)}
    <div class="section-title">Slow and unreliable server</div>
    <div class="grid g-2e"><div class="card"><h3>Delay every answer <span class="badge" id="vdelay">${sc.delay_s} s</span></h3><p class="sub">the meter gives up after 10 s</p>
      <input type="range" id="sdelay" min="0" max="30" step="1" value="${sc.delay_s}"></div>
      <div class="card"><h3>Fail calls with HTTP 503 <span class="badge" id="vfail">${sc.fail_percent} %</span></h3><p class="sub">random, for every call of the meter</p>
      <input type="range" id="sfail" min="0" max="100" step="5" value="${sc.fail_percent}"></div></div>
    <div class="row end mt"><button class="btn" onclick="resetTroubles()">Reset all to normal</button></div>`;
  $$(".tiles").forEach(g => $$(".tile", g).forEach(t => t.onclick = async () => {
    if (await act(() => api("PUT", "/admin/api/scenario", { [g.dataset.group]: t.dataset.k }), `${g.dataset.group}: ${t.dataset.k}`)) route(); }));
  $$("#ttarget button").forEach(b => b.onclick = async () => { if (await act(() => api("PUT", "/admin/api/scenario", { transport_target: b.dataset.v }))) route(); });
  const slider = (id, out, key, unit) => { $(id).oninput = e => $(out).textContent = e.target.value + unit;
    $(id).onchange = async e => { if (await act(() => api("PUT", "/admin/api/scenario", { [key]: Number(e.target.value) }), "applied")) route(); }; };
  slider("#sdelay", "#vdelay", "delay_s", " s"); slider("#sfail", "#vfail", "fail_percent", " %");
};

/* ---------------------------------------------------------------- firmware */
VIEWS.firmware = async () => {
  const d = await api("GET", "/admin/api/fw"), fw = d.fw;
  $("#view").innerHTML = banner() + `
    <div class="grid g-2">
      <div class="card"><h3>Upload a firmware image</h3><p class="sub">Build with <code>west build --sysbuild ...</code>, then drop <code>build/zephyr-app/zephyr/zephyr.signed.bin</code> here. The version is read from the image.</p>
        <div class="drop" id="drop">${icon("fw").replace("<svg", '<svg style="width:28px;height:28px;stroke:currentColor;fill:none;stroke-width:1.6"')}<div class="mt">Drop the file here or click</div><input type="file" id="fwfile" hidden></div>
        <div class="tablewrap mt"><table><thead><tr><th>File</th><th class="num">Size</th><th>Image</th><th></th></tr></thead><tbody>
        ${d.files.length ? d.files.map(f => `<tr><td class="mono">${esc(f.name)}</td><td class="num">${fmt(f.size)}</td>
          <td>${f.is_image ? `<span class="badge ok">MCUboot ${esc(f.version)}</span>` : `<span class="badge bad">not an image</span>`}</td>
          <td class="row" style="justify-content:flex-end">${f.is_image ? `<button class="btn sm" data-use="${esc(f.name)}">offer this</button>` : ""}<button class="btn sm danger" data-del="${esc(f.name)}">${icon("trash")}</button></td></tr>`).join("")
          : `<tr><td colspan="4" class="empty">no file yet</td></tr>`}</tbody></table></div></div>
      <div class="card"><h3>Offer in the tariff</h3><p class="sub">The meter downloads only a NEWER version, only after 10 min of uptime and 60 s without key presses. MCUboot checks the signature.</p>
        <label class="switch"><input type="checkbox" id="offer" ${fw.offer ? "checked" : ""}><i></i><span>offer <b class="mono">${esc(fw.file || "(no file)")}</b></span></label>
        <div class="kv mt"><div>File</div><div class="mono">${esc(fw.file || "-")}</div><div>Version sent</div><div class="mono">${esc(fw.version || "-")}</div></div>
        <label class="f mt"><span>Change the version text (only to test "wrong / older version")</span><input type="text" id="fwver" value="${esc(fw.version)}" placeholder="0.2.0"></label>
        <div class="row end mt"><button class="btn" id="fwvsave">Set version</button></div>
        <p class="small muted mt">Download problems (cut file, changed bytes ...) are on the <a href="#scenarios">Trouble makers</a> page.</p></div>
    </div>`;
  const drop = $("#drop"), file = $("#fwfile");
  drop.onclick = () => file.click();
  ["dragenter", "dragover"].forEach(ev => drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.add("over"); }));
  ["dragleave", "drop"].forEach(ev => drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.remove("over"); }));
  const up = async f => { const r = await act(() => api("PUT", "/admin/api/fw/upload?name=" + encodeURIComponent(f.name), f, true));
    if (r) { toast(r.warning || `uploaded, version ${r.version}`, !!r.warning); route(); } };
  drop.addEventListener("drop", e => e.dataTransfer.files[0] && up(e.dataTransfer.files[0]));
  file.onchange = () => file.files[0] && up(file.files[0]);
  $("#offer").onchange = async e => { if (await act(() => api("PUT", "/admin/api/fw/offer", { offer: e.target.checked }), e.target.checked ? "offered" : "not offered")) route(); };
  $("#fwvsave").onclick = async () => { if (await act(() => api("PUT", "/admin/api/fw/offer", { version: $("#fwver").value.trim() }), "version set")) route(); };
  $$("[data-use]").forEach(b => b.onclick = async () => { if (await act(() => api("PUT", "/admin/api/fw/offer", { file: b.dataset.use }), "selected")) route(); });
  $$("[data-del]").forEach(b => b.onclick = async () => { if (confirm("Delete " + b.dataset.del + "?") && await act(() => api("DELETE", "/admin/api/fw/" + encodeURIComponent(b.dataset.del)))) route(); });
};

/* ---------------------------------------------------------------- time */
VIEWS.time = async () => {
  const d = await api("GET", "/admin/api/time"); let base = { now: d.now, meter: d.meter_time, at: Date.now() };
  const presets = [["Real time", null], ["21:58 (night soon)", "21:58"], ["05:58 (day soon)", "05:58"], ["12:00", "12:00"], ["23:59 (date change)", "23:59"]];
  $("#view").innerHTML = banner() + `
    <div class="grid g-2e">
      <div class="card"><h3>This PC</h3><div class="clock" id="cpc"></div></div>
      <div class="card"><h3>The meter gets (NTP) ${d.offset_s ? `<span class="badge warn">shifted ${Math.round(d.offset_s / 60)} min</span>` : `<span class="badge ok">real time</span>`}</h3><div class="clock" id="cmeter"></div></div>
    </div>
    <div class="card mt"><h3>Set the meter's time</h3><p class="sub">Test night / day prices without waiting. Night = ${esc(STATE.tariff.Start_night)}:00 to ${esc(STATE.tariff.Stop_night)}:00 in the tariff.
      The meter asks for the time when WiFi connects and then every 10 minutes: switch the meter off and on to use the new time at once.</p>
      <div class="row">${presets.map(([n, v]) => `<button class="btn" data-t="${v ?? ""}">${n}</button>`).join("")}</div>
      <div class="row mt"><input type="text" id="tcustom" placeholder="HH:MM" style="width:100px"><button class="btn pri" id="tset">Set</button>
        <span class="muted small">or shift by</span><button class="btn sm" data-sh="-3600">-1 h</button><button class="btn sm" data-sh="-600">-10 min</button><button class="btn sm" data-sh="600">+10 min</button><button class="btn sm" data-sh="3600">+1 h</button></div></div>
    <div class="card mt"><h3>NTP server ${d.ntp.running ? `<span class="badge ok">running on UDP ${d.ntp.port}</span>` : `<span class="badge bad">off</span>`}</h3>
      ${d.ntp.running ? "" : `<p class="sub">${esc(d.ntp.error || "Start the server with -Ntp (PowerShell as administrator).")} Build the meter with <code>-DCONFIG_TAXI_NTP_HOST=\\"&lt;this PC's IP&gt;\\"</code>. NTP is UDP: it needs the meter on the same network or an open UDP 123.</p>`}
      <div class="tablewrap mt"><table><thead><tr><th>Asked at</th><th>From</th><th>Time given</th></tr></thead><tbody>
      ${d.last_requests.length ? d.last_requests.map(r => `<tr><td>${dtime(r.ts)}</td><td class="mono">${esc(r.remote)}</td><td>${dtime(r.served)}</td></tr>`).join("") : `<tr><td colspan="3" class="empty">no NTP request yet</td></tr>`}</tbody></table></div></div>`;
  const tick = () => { const e = (Date.now() - base.at) / 1000;
    $("#cpc").innerHTML = `${hms(base.now + e)}<small>${dtime(base.now + e).slice(0, 10)}</small>`;
    $("#cmeter").innerHTML = `${hms(base.meter + e)}<small>${dtime(base.meter + e).slice(0, 10)}</small>`; };
  tick(); every(500, tick);
  const set = async body => { if (await act(() => api("PUT", "/admin/api/time", body), "meter time set")) route(); };
  $$("[data-t]").forEach(b => b.onclick = () => set(b.dataset.t ? { target: b.dataset.t } : { offset_s: 0 }));
  $$("[data-sh]").forEach(b => b.onclick = () => set({ offset_s: (STATE.time.offset_s || 0) + Number(b.dataset.sh) }));
  $("#tset").onclick = () => set({ target: $("#tcustom").value.trim() });
};

/* ---------------------------------------------------------------- TLS */
VIEWS.tls = async () => {
  const d = await api("GET", "/admin/api/tls");
  const works = { tls13only: ["bad", "meter can not connect"], bigchain: ["ok", "works (16 KB buffer)"], expired: ["ok", "works (no check)"] };
  $("#view").innerHTML = banner() + `
    ${d.enabled ? "" : `<div class="banner"><b>TLS is off</b> (server started with --no-tls). The meter needs HTTPS.</div>`}
    <p class="muted small">Choose how this server speaks TLS. New connections use it at once. This shows whether the meter can talk to servers set up in different ways.</p>
    <div class="tiles mt">${Object.entries(d.profiles).map(([k, desc]) => { const i = d.info[k] || {}, w = works[k] || ["ok", "meter works"];
      return `<div class="tile ${k === d.profile ? "on" : ""}" data-p="${esc(k)}"><b>${esc(k)}</b><span>${esc(desc)}</span>
        <div class="meta"><span class="badge">${esc(i.key || "")}</span><span class="badge">${i.certificates || "?"} cert, ${fmt(i.chain_bytes)} B</span><span class="badge ${w[0]}">${w[1]}</span></div></div>`; }).join("")}</div>
    <div class="grid g-2e mt">
      <div class="card"><h3>What the meter agreed on</h3><p class="sub">TLS version and cipher of successful calls</p>
        ${d.negotiated.length ? `<ul class="list">${d.negotiated.map(r => `<li><div class="t mono small">${esc(r.tls)}</div><span class="badge">${fmt(r.n)}</span></li>`).join("")}</ul>` : `<div class="empty">no TLS call yet</div>`}</div>
      <div class="card"><h3>Failed handshakes</h3><p class="sub">the meter (or anybody) could not agree on TLS</p>
        ${d.failures.length ? `<ul class="list">${d.failures.map(r => `<li><div class="t"><div class="small">${esc(r.problems[0] || "")}</div><div class="s">${dtime(r.ts)} &middot; ${esc(r.remote)}</div></div></li>`).join("")}</ul>` : `<div class="empty">none</div>`}</div>
    </div>`;
  $$("[data-p]").forEach(t => t.onclick = async () => { if (await act(() => api("PUT", "/admin/api/tls", { profile: t.dataset.p }), "TLS: " + t.dataset.p)) route(); });
};

/* ---------------------------------------------------------------- settings */
VIEWS.settings = async () => {
  META = META || await api("GET", "/admin/api/meta");
  const host = META.host, port = META.port, flags = `-DCONFIG_TAXI_SERVER_HOST=\\"${host}\\" -DCONFIG_TAXI_SERVER_PORT=${port}`;
  const outside = `https://${host}:${port}/admin?token=${STATE.admin_token}`;
  $("#view").innerHTML = banner() + `
    <div class="grid g-2e">
      <div class="card"><h3>Build the meter for this server</h3><p class="sub">add to <code>west build -b esp32_devkitc/esp32/procpu zephyr-app --</code></p>
        <div class="copy"><code>${esc(flags)}</code><button class="btn sm" data-copy="${esc(flags)}">${icon("copy")}</button></div>
        <p class="small muted mt">With NTP also <code>-DCONFIG_TAXI_NTP_HOST=\\"${esc(host)}\\"</code>. The normal build still talks to <code>node.raxitaxi.ir:443</code>.</p></div>
      <div class="card"><h3>Admin page from another computer</h3><p class="sub">On this PC no token is needed. From outside, open this link once (it sets a cookie):</p>
        <div class="copy"><code>${esc(outside)}</code><button class="btn sm" data-copy="${esc(outside)}">${icon("copy")}</button></div>
        <p class="small muted mt">Keep the token secret: with it anybody can change prices and firmware offers.</p></div>
      <div class="card"><h3>Only answer these meters</h3><p class="sub">serials, comma separated. Empty = answer every meter.</p>
        <input type="text" id="sserials" style="width:100%" value="${esc(STATE.allowed_serials.join(", "))}">
        <div class="row end mt"><button class="btn pri" id="ssave">Save</button></div></div>
      <div class="card"><h3>Data</h3><p class="sub">Everything is in <code>data/taxi.db</code> (SQLite). Settings are kept when you clear the log.</p>
        <div class="row"><a class="btn" href="/admin/api/export/requests.csv">${icon("dl")} calls.csv</a><a class="btn" href="/admin/api/export/trips.csv">${icon("dl")} trips.csv</a>
        <a class="btn" href="/admin/api/export/devices.csv">${icon("dl")} devices.csv</a><a class="btn" href="/admin/docs" target="_blank">API docs</a></div>
        <div class="row mt"><button class="btn danger fill" id="sclear">${icon("trash")} Clear log, trips and devices</button></div></div>
    </div>`;
  $$("[data-copy]").forEach(b => b.onclick = () => navigator.clipboard.writeText(b.dataset.copy).then(() => toast("copied"), () => toast("copy failed", true)));
  $("#ssave").onclick = () => act(() => api("PUT", "/admin/api/tariff", { allowed_serials: $("#sserials").value.split(",").map(x => x.trim()).filter(Boolean) }), "saved");
  $("#sclear").onclick = async () => { if (confirm("Delete all calls, trips and devices? Settings stay.")) { await act(() => api("DELETE", "/admin/api/data"), "cleared"); route(); } };
};

/* ---------------------------------------------------------------- start */
nav(); window.addEventListener("hashchange", route); route(); chips(); setInterval(chips, 5000);
api("GET", "/admin/api/meta").then(m => { META = m; $("#foot").innerHTML = `${esc(m.host)}:${m.port}<br>${m.tls_enabled ? "HTTPS" : "HTTP"} &middot; NTP ${m.ntp.running ? "on" : "off"}`; }).catch(() => {});
