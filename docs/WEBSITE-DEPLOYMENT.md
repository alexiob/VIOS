# Website deployment

The VIOS website, **https://vios.iob.dev**, is the manual
([`docs/manual/VIOS-MANUAL.md`](manual/VIOS-MANUAL.md)) rendered as a static
site in the style of the VIOS GUI. A Cloudflare Worker on the iob.dev account
serves it, and GitHub Actions deploys it. This document covers how it is set
up, how it was deployed the first time (2026-10-21), and how to deploy,
check and maintain it. It is not part of the site.

## Contents

- [How it works](#how-it-works)
- [Building and previewing the site](#building-and-previewing-the-site)
- [First-time setup](#first-time-setup)
  - [1. A Cloudflare API token](#1-a-cloudflare-api-token)
  - [2. The account ID](#2-the-account-id)
  - [3. GitHub secrets](#3-github-secrets)
  - [4. The first deploy](#4-the-first-deploy)
  - [5. Check the site](#5-check-the-site)
  - [6. Link it from iob.dev](#6-link-it-from-iobdev)
- [Everyday deploys](#everyday-deploys)
- [Deploying by hand](#deploying-by-hand)
- [Maintenance](#maintenance)
- [Troubleshooting](#troubleshooting)
- [Moving the site elsewhere](#moving-the-site-elsewhere)

## How it works

| Piece | What it is |
|---|---|
| **Worker `vios-docs`** | An assets-only Cloudflare Worker (static files, no Worker code) on the iob.dev account. Configured in [`tools/manual/wrangler.jsonc`](../tools/manual/wrangler.jsonc): files from `build/site`, unknown paths answered with the site's `404.html`, no `workers.dev` address and no preview URLs. |
| **Domain `vios.iob.dev`** | The Worker's custom domain. The `iob.dev` zone is on the same account, so Cloudflare created the DNS record and the certificate itself on the first deploy. |
| **iob.dev** | A separate Worker, untouched by this one. It only links to `vios.iob.dev`. |
| **Workflow** | [`.github/workflows/manual.yml`](../.github/workflows/manual.yml): builds the site on every push or pull request that changes the manual (kept as the `manual-site` artifact) and deploys it on a release tag `vX.Y.Z` or a manual run. |
| **Secrets** | `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` in the repository's Actions secrets. Without them the deploy job prints a notice and stops instead of failing. |

The deploy step uses `cloudflare/wrangler-action`, pinned to the commit of
v3.15.0 because it receives the API token.

So the website follows releases: tagging `v0.9.0` publishes the manual as it
is in that commit, the same version as the release's downloads.

## Building and previewing the site

```bash
make manual
```

```bash
make manual-site
```

```bash
make manual-preview
```

- `make manual` regenerates the parts that come from the code: the word
  reference, a host run of every Forth example, and the screenshots and
  animations in `docs/manual/media/`. It needs cc, ImageMagick and gifsicle.
  Run it after changing the sources or the scenes, and commit the results.
- `make manual-site` turns the Markdown into HTML in `build/site`: a page per
  chapter, the media, the Agon 8×8 font as a web font, the Markdown itself and
  `404.html`. It needs only Python with `Markdown` and `fonttools`, so it runs
  in CI too.
- `make manual-preview` serves `build/site` on http://127.0.0.1:8000.

## First-time setup

Done once, on 2026-10-21. Repeat it to set up the site on another account or
after deleting the Worker.

### 1. A Cloudflare API token

Use a token just for VIOS, not the token that deploys iob.dev:

- that token has only account permissions, while attaching a custom domain
  needs permissions on the `iob.dev` **zone**;
- a token per repository can be revoked or rotated without breaking the
  other site, which matters once the VIOS repository is public.

In the Cloudflare dashboard:

1. Profile icon (top right) → **My Profile** → **API Tokens** → **Create Token**.
2. At the bottom: **Create Custom Token** → **Get started**.
3. **Token name:** `vios-docs GitHub deployment`.
4. **Permissions** (three rows, **+ Add more** for each extra one):

   | Scope | Item | Access |
   |---|---|---|
   | Account | Workers Scripts | Edit |
   | Zone | Workers Routes | Edit |
   | Zone | DNS | Edit |

5. **Account Resources:** Include → the iob.dev account.
6. **Zone Resources:** Include → Specific zone → `iob.dev`.
7. **Continue to summary** → **Create Token**, and copy the token. Cloudflare
   shows it only once.

An Account API Token (not tied to a user) works the same way, with the same
permissions.

### 2. The account ID

Cloudflare dashboard → **Workers & Pages**: the **Account ID** is in the
right-hand column. It is an identifier, not a secret, but it is stored as a
secret beside the token.

### 3. GitHub secrets

In `alexiob/VIOS`: **Settings** → **Secrets and variables** → **Actions** →
**New repository secret**, twice:

| Name | Value |
|---|---|
| `CLOUDFLARE_API_TOKEN` | the token from step 1 |
| `CLOUDFLARE_ACCOUNT_ID` | the account ID from step 2 |

Or from a terminal; each command prompts for the value, so it never shows on
screen or in the shell history:

```bash
gh secret set CLOUDFLARE_API_TOKEN -R alexiob/VIOS
```

```bash
gh secret set CLOUDFLARE_ACCOUNT_ID -R alexiob/VIOS
```

Check that both exist (values are never shown):

```bash
gh secret list -R alexiob/VIOS
```

Secrets stay private when the repository is public, and workflows run from
forks' pull requests never receive them. For a stricter setup, keep the
secrets in the GitHub environment `vios-docs` (Settings → Environments →
vios-docs, created by the first deploy) instead of at repository level, and
add yourself as a required reviewer: every deploy then waits for your
approval.

### 4. The first deploy

On GitHub: **Actions** → **Manual** → **Run workflow** on `main`. Or:

```bash
gh workflow run manual.yml -R alexiob/VIOS --ref main
```

```bash
gh run watch -R alexiob/VIOS
```

The run has two jobs, `build` and `deploy`; on 2026-10-21 both took under a
minute. The `deploy` job's log shows Wrangler uploading the site
(`Uploaded 69 of 69 assets`). The first deploy also creates the Worker and
attaches `vios.iob.dev`.

### 5. Check the site

```bash
curl -s -o /dev/null -w "%{http_code}\n" https://vios.iob.dev/
```

```bash
curl -s -o /dev/null -w "%{http_code}\n" https://vios.iob.dev/no-such-page
```

Expect `200` and `404`. Right after the first deploy, the first requests may
fail (curl prints `000`) for a minute or so while Cloudflare sets up the new
domain's DNS record and certificate. Then open https://vios.iob.dev: the
manual, in the VIOS style, says "Edition: VIOS 0.8.0" (the current version).

In Cloudflare, **Workers & Pages** → `vios-docs` shows the Worker, its
deployments and, under **Settings → Domains & Routes**, `vios.iob.dev`.

### 6. Link it from iob.dev

Add a link to `https://vios.iob.dev` on the iob.dev site. The VIOS repository
already points there: the README's header and the repository's **About**
homepage.

## Everyday deploys

| When | What happens |
|---|---|
| A release tag `vX.Y.Z` is pushed ([RELEASING.md](RELEASING.md)) | The Manual workflow builds and deploys the site from the tagged commit, next to the release build. |
| A fix to the manual between releases | Merge it to `main`, then **Actions** → **Manual** → **Run workflow** on `main` (or `gh workflow run manual.yml -R alexiob/VIOS --ref main`). |
| A push or pull request that changes the manual | The site is built and checked, not deployed. The built site is attached to the run as the `manual-site` artifact. |

## Deploying by hand

From a computer, with [Wrangler](https://developers.cloudflare.com/workers/wrangler/)
logged in to the iob.dev account:

```bash
npx wrangler login
```

```bash
make manual-site
```

```bash
npx wrangler deploy --config tools/manual/wrangler.jsonc
```

To check the configuration and the file list without uploading anything:

```bash
npx wrangler deploy --config tools/manual/wrangler.jsonc --dry-run
```

## Maintenance

- **Rotating the token.** Create a new token as in step 1, update the
  `CLOUDFLARE_API_TOKEN` secret, run the workflow once to check it, then
  delete the old token in Cloudflare (My Profile → API Tokens → ⋯ → Delete).
- **Updating the Wrangler action.** It is pinned to a commit. To move to a
  newer release, find the release's commit (`gh api
  repos/cloudflare/wrangler-action/git/ref/tags/vX.Y.Z`), replace the hash and
  the version comment in `manual.yml`, and run the workflow by hand.
- **Node.js deprecation.** GitHub warns that `actions/checkout@v4`,
  `actions/download-artifact@v4` and the Wrangler action target Node.js 20;
  they still run (on Node.js 24). Move them to their newer major versions when
  convenient.
- **Taking the site down.** Cloudflare → Workers & Pages → `vios-docs` →
  Settings → Delete. To stop deploys without deleting anything, remove the
  `CLOUDFLARE_API_TOKEN` secret.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| The `deploy` job shows a notice "CLOUDFLARE_API_TOKEN or CLOUDFLARE_ACCOUNT_ID is not set" | A secret is missing or misnamed. Check `gh secret list -R alexiob/VIOS`. |
| Wrangler fails with an authentication error (code 10000) | The token is wrong, expired or revoked, or lacks Workers Scripts: Edit on the account. |
| Wrangler fails while attaching `vios.iob.dev` | The token lacks the zone permissions (Workers Routes: Edit, DNS: Edit) on `iob.dev`, or the zone is not on the same account. |
| `curl` prints `000` or the browser cannot resolve `vios.iob.dev` | Right after the first deploy: wait a minute for DNS and the certificate. Later: check Settings → Domains & Routes on the Worker. |
| The site shows an old version | It deploys only on tags and manual runs; run the workflow on `main`. Browsers may also cache pages briefly: reload. |
| The deploy job did not run at all | It runs only for tags and manual runs. A push to `main` builds the site but does not deploy it. |

## Moving the site elsewhere

`build/site` is plain static files, so any static host serves it unchanged
(GitHub Pages, Cloudflare Pages, Netlify, a plain web server). Only the
`deploy` job in `manual.yml`, `tools/manual/wrangler.jsonc` and this document
would change.
