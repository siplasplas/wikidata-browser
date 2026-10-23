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
- `wdtriples [--limit MB] [-o triples.bin] dump.xml.bz2` – the triples (see
  below) of a dump part, with sizes, pages, statements, triples by rank and
  the entity types of values (claims, qualifiers, lexeme forms and senses) by
  kind of entity; `--offsets FILE` reads only the bz2 blocks at the listed
  offsets (e.g. those of property or lexeme pages from the dump's index); `--limit` reads only the first MB of compressed data.
- `wdindex [-j 20] [-t triples] [-i index] <dump directory> <date>.lst` – one
  pass over all parts of a dump (`<date>.lst` of `wdlist`), in threads taking
  parts from a queue (the largest first). For part `NNN` it writes
  `triples/NNN` (triples, see below) and `index/NNN`, the bz2 blocks of the part
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
- `wdsort [-j 20] [triples [sort-triples]]` – sorts each triple file of
  `wdindex` by (bit-reversed property, target, item, rank), leaves out repeated
  triples and writes it under the same name to `sort-triples`. Threads take
  files from a queue, the largest first; `NNN.part` is renamed when done.
- `wdmerge [-j 20] [sort-triples [triple-index]]` – merges the sorted files into
  256 index files `triple-index/000` … `255`: file `NNN` holds the triples of
  all parts with property % 256 = `NNN`, sorted by (bit-reversed property,
  target, item, rank), each triple once. Input files are memory-mapped; from each one
  a contiguous run (about 1/256 of it) goes to an output file.

## Building the reverse index of entity links

```
wdlist <dump directory>                                 # 20261001.lst
wdindex -j 20 <dump directory> 20261001.lst             # triples/, index/  (~1 h, Release build)
wdsort -j 20                                            # sort-triples/
wdmerge -j 20                                           # triple-index/
```

Use a Release build: in a Debug build JSON parsing is about 15 times slower
and `wdindex` takes about 5 hours instead of 1.

### Triples
A triple has 12 bytes, 3 little-endian uint32 (`tools/triples.h`): property,
item with rank, target – always P, Q, Q: only claims whose main snak's value is
an item (`datavalue.value` with entity-type `item` and `numeric-id`) give
triples, about 40–60% of all claims; values that are properties or lexemes
(about 0.03%) are left out.

| bytes | field | |
|---|---|---|
| 0–3 | property | 31 for P31 |
| 4–7 | item << 2 \| rank | 30 bits for the item (up to Q1073741823), 2 bits for the rank: 0 deprecated, 1 normal, 2 preferred |
| 8–11 | target | the item it points to |

The rank is in the low bits so that sorting by the field sorts by item.
(31, 56782 << 2 | 1, 34770) is "Q56782 instance of (P31) Q34770", rank normal.
The "truthy" values of a property are its preferred ones if there are any,
else its normal ones; deprecated values are wrong ones kept (with a reason,
P2241). In a sample about 99.8% are normal.

### Splitting by property modulo
The index is split into files by property % 256, not by property ranges, so the
split does not depend on how many properties there are. Triples are sorted by
the property with its 32 bits reversed (bit 0 becomes bit 31):

```cpp
x = ((x >> 1) & 0x55555555) | ((x & 0x55555555) << 1);
x = ((x >> 2) & 0x33333333) | ((x & 0x33333333) << 2);
x = ((x >> 4) & 0x0F0F0F0F) | ((x & 0x0F0F0F0F) << 4);
x = std::byteswap(x);
```

Reversal puts the low bits of the property on top: the top 8 bits of the
reversed property are the reversed property % 256. In every sorted file the
triples of one remainder are therefore one contiguous run, found by binary
search, and `wdmerge` merges these runs. The same works for any power of two:
2^n files take the top n bits of the reversed property.

### Lookup
"All items with occupation (P106) writer (Q36180)": file `triple-index/106`
(106 % 256), binary search for (reverse(106), 36180): the items of the run are
the answer, in increasing order, with the rank of each statement.

### Block index
`index/NNN` of `wdindex` lists the bz2 blocks of dump part `NNN` and their
pages, colon-separated:

```
offset:count:uncompressed        a block: position in the .bz2, number of pages, size uncompressed
:start:symbol:title              a page: start in the uncompressed block, entity id or "other", label
EOF=<size>                       size of the .bz2
```

`count` may differ between blocks. A page ends where the next one starts, the
last at `uncompressed`, so it is cut out without parsing. The title is the last
field and may contain colons: split only up to the third colon. The label is
the English one (lexemes: lemma), else the first; pages without entity JSON
(redirects) are `other` with their page title. Blocks without pages (siteinfo,
end of the file) have count 0, so a block's compressed length is always the
next offset (or `EOF`) minus its own offset.

## Planned formats of entities
- Items (Q, over 100 million): a binary format, fields of whole bytes (enums
  such as rank or entity type are one byte each), compressed in bz2 blocks.
- Properties (P, about 14 thousand) and lexemes (L, about 1.7 million): JSONL,
  one entity per line. Lexemes differ much from items and from each other
  (forms and senses with ids like `L7-F2`, `L7-S1`, their own claims; values
  pointing to lexemes, forms and senses), properties are few: neither is worth
  a binary format.
- Not needed for browsing and left out: statement ids and snak hashes (for
  editing only), lexeme counters `nextFormId` and `nextSenseId` (ids of
  deleted forms and senses are never reused, so they are no count).
