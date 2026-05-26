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

## Deploy to Cloudflare Pages

1. Sign in to the [Cloudflare dashboard](https://dash.cloudflare.com/).
2. **Workers & Pages** → **Create** → **Pages** → **Connect to Git**.
3. Pick the `moshon-timetable` GitHub repo.
4. Build settings:
   - **Framework preset**: None
   - **Build command**: *(leave empty)*
   - **Build output directory**: `web/public`
   - **Root directory (advanced)**: `/`
5. Save and deploy.

Cloudflare will redeploy on every push to `main`.

### Custom domain (timetable.moshon.be)

1. In the Pages project, **Custom domains** → **Set up a custom domain**.
2. Enter `timetable.moshon.be`.
3. If the parent domain `moshon.be` is **already on Cloudflare**, the DNS
   record is created automatically — done.
4. If `moshon.be` is hosted **elsewhere**, Cloudflare gives you a CNAME
   value (something like `your-project.pages.dev`). Add it as a CNAME for
   `timetable` at your DNS provider, then come back and click *Activate*.

A free SSL certificate is provisioned automatically.

## Routing

Pages routes:

| Path | View |
|:--|:--|
| `/` | Landing — favourites + search |
| `/aalter` | Aalter board |
| `/brussel-zuid` | Brussel-Zuid board |
| `/<slug>` | Any station whose name slugifies to `<slug>` |

`_redirects` declares an SPA fallback so every path serves
`index.html` and the in-browser router takes over.

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
