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
- `wdtriples [--limit MB] [-o triples.bin] dump.xml.bz2` – links between
  entities of a dump part: for each claim `P..` of an item, the main snak's
  entity value (`datavalue.value.numeric-id`), as a triple of little-endian
  uint32 (property, item, target): (31, 56782, 34770) is "Q56782 instance of
  Q34770". Prints sizes, pages and triples; `--limit` reads only the first MB of
  compressed data.
- `wdindex [-j 20] [-t triples] [-i index] <dump directory> <date>.lst` – one
  pass over all parts of a dump (`<date>.lst` of `wdlist`), in threads taking
  parts from a queue (the largest first). For part `NNN` it writes
  `triples/NNN` (as `wdtriples`) and `index/NNN`, the bz2 blocks of the part
  with their pages, colon-separated:

  ```
  613:100:1206398                      offset in the .bz2:pages:size uncompressed
  :0:Q107906754:45th US Colored Infantry: Jones, Jacob P (Age 18)
  :12414:Q107906755:…                  :start in the block:entity id:label
  EOF=20012742                         size of the .bz2
  ```

  A page ends where the next one starts (the last at the block size); its label
  is the English one (lexemes: lemma), else the first; pages without entity
  JSON (redirects) are `other` with their page title. Blocks without pages
  (siteinfo, end) have count 0. Results are written as `NNN.part` and renamed
  when done: a stopped run continues with the parts not done. Progress every 10 s.
