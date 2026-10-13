# Guide sources

These are the guide pages published at
**[andrewdemsds.github.io/hisense-w41h1](https://andrewdemsds.github.io/hisense-w41h1/)**.

They started in the GitHub wiki. The wiki is retired because GitHub serves every `/wiki/` URL
with `X-Robots-Tag: none`, so nothing there could be found through a search engine. Keeping the
pages here means one copy of each document, reviewed through pull requests like the rest of the
repository.

Edit the Markdown here. Pushing to `main` rebuilds the site.

- `images/` is referenced with relative paths, so the images stay beside the pages that use them.
- Links between pages are written wiki-style, without a file extension. The site assembler
  rewrites them to `.html` at build time.
- A file elsewhere in the repository is named in backticks, not linked: a relative link to it
  would not resolve on the site.
- A page added, renamed or removed is also changed in `_Sidebar.md`, in `Home.md`, in
  `GUIDE_ORDER` in `.github/scripts/assemble_docs_site.py`, and in the table on
  `docs-site/index.md`.
- `lint.py` checks heading structure, link targets, the sidebar and a leak denylist. Run
  `python3 docs/guide/lint.py`.
- `Home.md` is not published: the site has its own landing page, `docs-site/index.md`.
