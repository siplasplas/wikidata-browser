# wikidata-browser

Browser of Wikidata dumps (C++, Qt6), like wiktbrowse of wiktparse for Wiktionaries.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## Tools
- `wdlist [directory]` (default `.`) – finds the dump parts
  `wikidatawiki-<date>-pages-articles-multistream*.xml-*.bz2` in the directory
  and writes `<date>.lst` in the current directory for each date found: the
  files in alphabetical order. First line `# <files>:<total bytes>`, then a line
  per file: number (`000`, `001`, …), `:`, size in bytes right-aligned to the
  longest, `:`, the name without the directory.

  ```
  # 102:182364632104
  000:2403144939:wikidatawiki-20261001-pages-articles-multistream1.xml-p1p441397.bz2
  001: 764071939:wikidatawiki-20261001-pages-articles-multistream10.xml-p13998368p15498367.bz2
  ```
