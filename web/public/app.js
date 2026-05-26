// moshon-timetable web — vanilla JS, no build step.
//
// Routing: pathname-based via History API.
//   /              landing (favourites + search)
//   /aalter        live board for Aalter
//   /brussel-zuid  live board for Brussels-South
//
// State (favourites + last-viewed) is kept in localStorage.

const IRAIL_BASE = "https://api.irail.be";
const REFRESH_MS = 45_000;
const LS_FAVS    = "moshon.favs";

// ---------------------------------------------------------------------
// Tiny helpers
// ---------------------------------------------------------------------

const $ = (sel, root = document) => root.querySelector(sel);
const h = (tag, attrs = {}, ...children) => {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class")        el.className = v;
    else if (k === "html")    el.innerHTML = v;
    else if (k.startsWith("on") && typeof v === "function")
                              el.addEventListener(k.slice(2), v);
    else if (v != null)       el.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c == null) continue;
    el.append(c.nodeType ? c : document.createTextNode(c));
  }
  return el;
};

const slug = (s) => s.toLowerCase()
  .normalize("NFD").replace(/[̀-ͯ]/g, "")
  .replace(/[^a-z0-9]+/g, "-")
  .replace(/^-+|-+$/g, "");

// iRail returns names in NL by default. Map slug → canonical name via the
// list it gives us. We cache that list in localStorage forever (renames
// are very rare; the user can clear cache if they ever happen).
let _stations = null;
async function loadStations() {
  if (_stations) return _stations;
  const cached = localStorage.getItem("moshon.stations.v1");
  if (cached) {
    try { _stations = JSON.parse(cached); return _stations; } catch {}
  }
  const r = await fetch(`${IRAIL_BASE}/v1/stations/?format=json&lang=nl`);
  const j = await r.json();
  _stations = j.station.map(s => ({ name: s.standardname || s.name, id: s.id }));
  localStorage.setItem("moshon.stations.v1", JSON.stringify(_stations));
  return _stations;
}

function getFavs() {
  try { return JSON.parse(localStorage.getItem(LS_FAVS)) || []; } catch { return []; }
}
function setFavs(list) {
  localStorage.setItem(LS_FAVS, JSON.stringify(list));
}
function isFav(name)  { return getFavs().includes(name); }
function toggleFav(name) {
  const favs = getFavs();
  const i = favs.indexOf(name);
  if (i >= 0) favs.splice(i, 1); else favs.push(name);
  setFavs(favs);
}

function hhmm(unix) {
  const d = new Date(unix * 1000);
  return d.toLocaleTimeString("nl-BE", { hour: "2-digit", minute: "2-digit", hour12: false });
}

function toast(msg, ms = 2200) {
  const t = h("div", { class: "toast" }, msg);
  document.body.append(t);
  setTimeout(() => t.remove(), ms);
}

// ---------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------

window.addEventListener("popstate", () => render());
document.addEventListener("click", (e) => {
  // Soft-navigate <a href="/..."> internal links without a full reload.
  const a = e.target.closest("a[href^='/']");
  if (a && !a.target && !e.ctrlKey && !e.metaKey && !e.shiftKey) {
    e.preventDefault();
    history.pushState({}, "", a.getAttribute("href"));
    render();
  }
});

function go(path) { history.pushState({}, "", path); render(); }

// ---------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------

let _refreshTimer = null;

function render() {
  clearInterval(_refreshTimer);
  _refreshTimer = null;

  const path = location.pathname;
  const root = $("#app");
  root.innerHTML = "";
  if (path === "/" || path === "") {
    renderLanding(root);
  } else {
    renderBoard(root, decodeURIComponent(path.slice(1)));
  }
}

async function renderLanding(root) {
  const stations = await loadStations().catch(() => []);
  const favs = getFavs();

  const search = h("input", {
    type: "search", placeholder: "Search a station…",
    autocomplete: "off", autocapitalize: "off", spellcheck: "false",
  });
  const results = h("ul", { class: "results empty" });

  const updateResults = () => {
    const q = search.value.trim().toLowerCase();
    if (!q || q.length < 2) {
      results.className = "results empty";
      results.innerHTML = "";
      return;
    }
    const matches = stations
      .filter(s => s.name.toLowerCase().includes(q))
      .slice(0, 30);
    results.className = "results";
    results.innerHTML = "";
    for (const s of matches) {
      results.append(h("li", { onclick: () => go(`/${slug(s.name)}`) }, s.name));
    }
    if (!matches.length) results.append(h("li", {}, "No match."));
  };
  search.addEventListener("input", updateResults);

  const favList = h("ul", { class: "fav-list" });
  if (favs.length === 0) {
    favList.append(h("li", { class: "empty" }, "No favourites yet — search and tap a station to add it."));
  } else {
    for (const f of favs) {
      favList.append(h("li", {},
        h("a", { href: `/${slug(f)}` },
          h("span", { class: "pin" }, "★"),
          h("span", {}, f)
        )));
    }
  }

  root.append(
    h("div", { class: "landing" },
      h("h1", {}, "moshon-timetable"),
      h("p", { class: "lead" },
        "Live NMBS/SNCB train departures and arrivals. Pick a station to view its board, then tap the star to save it."),
      h("div", { class: "search" }, search),
      results,
      h("div", { class: "fav-section" },
        h("h2", {}, "Saved stations"),
        favList
      )
    )
  );
}

// ---------- station board ----------

async function renderBoard(root, requestedSlug) {
  const stations = await loadStations().catch(() => []);
  const match = stations.find(s => slug(s.name) === requestedSlug);
  const stationName = match ? match.name : decodeURIComponent(requestedSlug);

  // Persist last-viewed
  localStorage.setItem("moshon.last", stationName);

  let mode = "departure";        // "departure" | "arrival"
  let lastSuccess = 0;

  const clockEl   = h("span", { class: "clock" }, "--:--");
  const dotEl     = h("span", { class: "dot", "data-state": "failed", title: "Data freshness" });
  const starEl    = h("span", {
    class: isFav(stationName) ? "star on" : "star",
    title: "Toggle favourite",
    onclick: () => {
      toggleFav(stationName);
      starEl.classList.toggle("on");
      toast(isFav(stationName) ? "Saved" : "Removed");
    }
  }, "★");

  const rowsEl    = h("ul", { class: "rows" });
  const emptyEl   = h("div", { class: "empty-board" },
    h("div", { class: "spinner" }), h("div", {}, "Loading…"));
  rowsEl.append(emptyEl);

  const tabDep = h("button", { "aria-selected": "true",  onclick: () => switchMode("departure") }, "Departures");
  const tabArr = h("button", { "aria-selected": "false", onclick: () => switchMode("arrival") },   "Arrivals");

  function switchMode(m) {
    mode = m;
    tabDep.setAttribute("aria-selected", m === "departure" ? "true" : "false");
    tabArr.setAttribute("aria-selected", m === "arrival"   ? "true" : "false");
    fetchAndRender();
  }

  function updateClock() {
    const d = new Date();
    clockEl.textContent = d.toLocaleTimeString("nl-BE", { hour: "2-digit", minute: "2-digit", hour12: false });

    if (!lastSuccess) {
      dotEl.dataset.state = "failed";
    } else {
      const age = (Date.now() - lastSuccess) / 1000;
      dotEl.dataset.state = age <= 60 ? "fresh" : age <= 120 ? "stale" : "failed";
    }
  }
  updateClock();
  setInterval(updateClock, 1000);

  async function fetchAndRender() {
    try {
      const url = `${IRAIL_BASE}/v1/liveboard/?station=${encodeURIComponent(stationName)}`
                + `&arrdep=${mode}&format=json&lang=nl&alerts=false`;
      const r = await fetch(url);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const j = await r.json();
      lastSuccess = Date.now();
      paintBoard(j);
    } catch (e) {
      console.error(e);
      rowsEl.innerHTML = "";
      rowsEl.append(h("div", { class: "empty-board" },
        h("div", { class: "error-msg" }, "Couldn't load this station's board."),
        h("div", { style: "margin-top:8px;font-size:13px;" }, String(e.message || e))));
    }
  }

  function paintBoard(j) {
    const section  = j.departures || j.arrivals;
    const entries  = (section && (section.departure || section.arrival)) || [];

    rowsEl.innerHTML = "";

    if (!entries.length) {
      rowsEl.append(h("div", { class: "empty-board" }, "No upcoming trains in iRail's window."));
      return;
    }

    const now = Date.now() / 1000;
    let shown = 0;
    for (const e of entries) {
      const scheduled = Number(e.time);
      const delay     = Number(e.delay) || 0;
      const actual    = scheduled + delay;
      if (actual < now) continue;                     // past entry
      shown++;

      const dest      = e.station || "";
      const platform  = e.platform || "?";
      const veh       = e.vehicleinfo || {};
      const shortName = veh.shortname || e.vehicle || "";
      const type      = veh.type || (shortName.split(" ")[0] || "");
      const number    = shortName.includes(" ") ? shortName.split(" ").slice(1).join(" ") : "";
      const canceled  = Number(e.canceled) === 1;

      const timeCol = h("div", { class: "time" }, hhmm(scheduled),
        delay >= 60 ? h("span", { class: "badge" }, `+${Math.floor(delay/60)}'`) : null,
        delay >= 60 ? h("span", { class: "newtime" }, hhmm(actual)) : null,
        canceled    ? h("span", { class: "badge" }, "CANC") : null,
      );

      const destCol = h("div", {},
        h("div", { class: "dest" }, dest),
        shortName ? h("span", { class: "vehicle" }, shortName) : null,
      );

      const platCol = h("div", { class: "platform" }, platform);
      const typeCol = h("div", { class: "type" },
        h("b", {}, type),
        number ? h("small", {}, number) : null,
      );

      rowsEl.append(h("li", { class: "row" + (canceled ? " cancel" : "") },
        timeCol, destCol, platCol, typeCol));
    }
    if (!shown) {
      rowsEl.append(h("div", { class: "empty-board" }, "No future trains right now."));
    }
  }

  root.append(
    h("div", { class: "board" },
      h("header", {},
        h("a", { href: "/", class: "back", title: "Back" }, "←"),
        h("h1", {}, stationName,
          h("small", {}, mode === "departure" ? "Departures" : "Arrivals")),
        dotEl, clockEl, starEl
      ),
      h("div", { class: "tabs" }, tabDep, tabArr),
      rowsEl
    )
  );

  fetchAndRender();
  _refreshTimer = setInterval(fetchAndRender, REFRESH_MS);
}

render();
