# Guide sources

These are the operator-facing guides published at
**[andrewdemsds.github.io/hisense-w41h1](https://andrewdemsds.github.io/hisense-w41h1/)**.

They started life in the GitHub wiki. The wiki is retired because GitHub serves every `/wiki/` URL
with `X-Robots-Tag: none`, which is noindex and nofollow at the HTTP level, so nothing there could
ever be found through a search engine or the crawlers behind AI search. Keeping the pages here
means one copy per document, reviewed through pull requests like the rest of the repo, and no way
for a wiki copy and a site copy to drift apart.

Edit the markdown here. Pushing to `main` rebuilds the site.

- `images/` is referenced with relative paths, so the images must stay beside the pages that use them.
- Links between pages are written wiki-style, without a file extension; the site assembler
  rewrites them to `.html` at build time.
- `lint.py` checks heading structure, link targets and prose. Run it from this directory.
- `Home.md` is not published: the site has its own landing page at `docs-site/index.md`.
