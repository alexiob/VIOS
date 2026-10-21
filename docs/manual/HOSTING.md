# Hosting the VIOS manual

The manual is one Markdown file, [`VIOS-MANUAL.md`](VIOS-MANUAL.md), with its
images in `media/`. It reads fine on GitHub as it is. A static HTML site with
the look of the VIOS GUI is generated from it and served at
**https://vios.iob.dev** by a Cloudflare Worker. This file is for whoever
maintains that site; it is not part of the site.

## Building the site

```sh
make manual          # word reference, example check, screenshots (needs cc, ImageMagick, gifsicle)
make manual-site     # HTML into build/site (creates build/site-venv from tools/manual/requirements.txt)
make manual-preview  # serves build/site on http://127.0.0.1:8000
```

`make manual` is for when the sources or the scenes change: it regenerates
the word reference from the code, runs every Forth example on the host, and
re-renders `media/`. Commit the results. `make manual-site` only turns
Markdown into HTML (Python, `Markdown` and `fonttools`), so it also runs in
CI. The output in `build/site` is plain files: one page per chapter, the
media, the Agon 8×8 font as a web font, the Markdown itself and a
`404.html`. Any static host serves it unchanged.

## How it is served

| Piece | What it is |
|---|---|
| Worker `vios-docs` | An assets-only Cloudflare Worker (no code) on the iob.dev account. Its configuration is [`tools/manual/wrangler.jsonc`](../../tools/manual/wrangler.jsonc): the files come from `build/site`, unknown paths get `404.html`, and its `workers.dev` address is off |
| `vios.iob.dev` | The Worker's custom domain. Cloudflare creates the DNS record and the certificate itself, because the `iob.dev` zone is on the same account. The `iob.dev` site is a separate Worker and only links here |
| [`.github/workflows/manual.yml`](../../.github/workflows/manual.yml) | Builds the site on every push or pull request that touches the manual (the build is kept as an artifact). It **deploys** only for a release tag `vX.Y.Z` and when run by hand (Actions → Manual → Run workflow) |

So the site follows releases: tagging `v0.9.0` publishes the manual as it is
in that commit. Between releases, a correction goes live by running the
workflow by hand on `main`.

## One-time setup

1. **API token.** Cloudflare dashboard → My Profile → API Tokens → Create
   Token → *Edit Cloudflare Workers* template. Under Account Resources choose
   the iob.dev account; under Zone Resources choose the zone `iob.dev`
   (the custom domain needs it). Create it and copy the token.
2. **Account ID.** Cloudflare dashboard → Workers & Pages: the Account ID is
   in the right-hand column.
3. **GitHub secrets.** In `alexiob/VIOS`: Settings → Secrets and variables →
   Actions → New repository secret:
   - `CLOUDFLARE_API_TOKEN`: the token from step 1;
   - `CLOUDFLARE_ACCOUNT_ID`: the ID from step 2.

   Secrets stay private when the repository is public; workflows triggered
   from forks' pull requests never receive them, and this workflow deploys
   only on tags and manual runs anyway. The deploy job runs in the GitHub
   environment `vios-docs`: for a stricter setup, put the two secrets there
   instead (Settings → Environments → vios-docs) and add yourself as a
   required reviewer, so each deploy waits for your approval. The
   `cloudflare/wrangler-action` step is pinned to a commit; update the pin by
   hand when moving to a newer release.
4. **First deploy.** Actions → Manual → Run workflow on `main`. The first run
   creates the `vios-docs` Worker and attaches `vios.iob.dev`; the site is up
   within a minute or two.
5. **Link it from iob.dev.** Add a link to `https://vios.iob.dev` on the
   iob.dev site.

Until the two secrets exist, the deploy job only prints a notice and stops,
so tagging a release without them does not fail.

## Deploying by hand

With [Wrangler](https://developers.cloudflare.com/workers/wrangler/) logged
in to the iob.dev account (`npx wrangler login`):

```sh
make manual-site
npx wrangler deploy --config tools/manual/wrangler.jsonc
```

`npx wrangler deploy --config tools/manual/wrangler.jsonc --dry-run` checks
the configuration and the files without uploading anything.

## Moving it elsewhere

`build/site` is plain static files, so any static host works: GitHub Pages,
Cloudflare Pages, Netlify or a plain web server. Only the deploy job in
`manual.yml` and this file would change.
