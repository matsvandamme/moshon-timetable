# moshon-timetable — web

Live NMBS / SNCB train departures and arrivals, mirroring the look of the
[firmware](../firmware/) but in a browser. Static site, no backend.

- Pick a station from a search of all 700+ NMBS / SNCB stations.
- Tap the star on a station's board to save it as a favourite.
- Bookmark the station URL directly: `https://timetable.moshon.be/aalter`,
  `https://timetable.moshon.be/brussel-zuid`, etc.
- Refreshes every 45 seconds with a freshness dot in the header.

Data comes straight from the [iRail](https://docs.irail.be) public API.
No tracking, no cookies, no signup. State (favourites + station list cache)
lives in `localStorage`.

## Run locally

```sh
cd web
python3 -m http.server -d public 8000
# open http://localhost:8000/
```

Any static file server works; we don't have a build step. iRail allows
cross-origin requests, so the browser can call the API directly without
a proxy.

## Deploy to Cloudflare (Workers Assets)

Cloudflare merged "Pages" into the unified "Workers & Pages" flow, so
the deploy now runs `wrangler deploy` against a `wrangler.toml` at the
repo root. That file is already committed (`../wrangler.toml`) — it
declares the project as static-assets-only and points at `web/public/`.

1. Sign in to the [Cloudflare dashboard](https://dash.cloudflare.com/).
2. **Workers & Pages** → **Create application** → **Import a repository**
   (or **Connect to Git** depending on the dashboard version).
3. Pick the `moshon-timetable` GitHub repo.
4. Cloudflare reads the committed `wrangler.toml` and shows a summary.
   Leave the fields at their defaults:
   - **Project name**: `moshon-timetable`
   - **Production branch**: `main`
   - **Build command**: *(empty)*
   - **Deploy command**: `npx wrangler deploy` (Cloudflare's default)
5. **Save and Deploy**.

Cloudflare redeploys automatically on every push to `main`. PR branches
get preview URLs.

### Custom domain (timetable.moshon.be)

1. In the project: **Settings** → **Domains & Routes** → **Add custom domain**.
2. Enter `timetable.moshon.be`.
3. If `moshon.be` is on Cloudflare DNS, the CNAME is created
   automatically and SSL provisions in under a minute.
4. If `moshon.be` is hosted elsewhere, Cloudflare shows you a target
   value like `moshon-timetable.<your-subdomain>.workers.dev`. Add it
   as a CNAME for `timetable` at your DNS provider, then come back and
   verify.

## Routing

Pages routes:

| Path | View |
|:--|:--|
| `/` | Landing — favourites + search |
| `/aalter` | Aalter board |
| `/brussel-zuid` | Brussel-Zuid board |
| `/<slug>` | Any station whose name slugifies to `<slug>` |

SPA fallback (so `/aalter` and friends route to `index.html`) is
declared in `wrangler.toml`'s `not_found_handling = "single-page-application"`.
The legacy `_redirects` file is retained for any future migration back to
Pages-style hosting; both Pages and Workers Assets accept it.

## File layout

```
web/
├── public/
│   ├── index.html       page shell
│   ├── style.css        NMBS palette + responsive layout
│   ├── app.js           router, iRail client, render loop
│   ├── icon.svg         favicon (B-in-oval)
│   ├── _redirects       SPA fallback for Cloudflare Pages
│   └── _headers         security + cache headers
└── README.md
```

## Differences from the device firmware

The web version intentionally drops a few firmware-only features:

- **No `/vehicle/` calls.** The device makes one HTTP request per train to
  populate the via list; doing the same from a browser would mean ~24
  requests per refresh. Liveboard data alone is enough on a larger screen.
- **No alerts banner yet.** Adding the iRail alerts feed is on the roadmap.
- **No weather chip yet.** Same.
- **No multilingual UI yet.** The page is English; iRail queries hit the
  Dutch endpoint. Easy add later.

Everything else — colour palette, layout, freshness dot semantics,
synchronised refresh, search-as-you-type station picker — matches.

## License

MIT, same as the firmware. See the [repo-level LICENSE](../LICENSE).
