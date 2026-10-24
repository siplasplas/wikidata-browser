# wikidata-browser

Browser of Wikidata dumps (C++, Qt6), like wiktbrowse of wiktparse for Wiktionaries.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## Browser
`wdbrowse [file.jsonl | file.jsonl.bz2 | Q42]` (Qt6, with qcodeedit and
qt-extra from the sibling checkouts `../qcodeedit`, `../qt-extra` or fetched;
`-DWDB_BROWSER=OFF` builds only the tools):
- File > Open: a JSONL file of entities (e.g. `wiktNeeded_1.jsonl` of
  wiktparse), plain or `.jsonl.bz2` with its `.jsonl.idx`; the lines with id
  and English label, a filter, "go to line".
- Search names (field above the list, Ctrl+K, Enter): the items whose name
  starts with the text, case and diacritics ignored (`lodz` finds Łódź),
  shown in the list on the left like the lines of a JSONL file – all of them,
  millions for one letter, names read only as shown (`names`, `names-by-q`,
  `q-by-name` of `wdnames` in the index directory). Exact names first, then
  the most popular (`popularity` of `wddense`: number of Wikipedias, shown in
  brackets). Typing `Q42` goes to Q42.
- Go > Entity (Ctrl+G): `Q42`, `P31` or `L7` read from the whole dump through
  `q.idx`, `p.idx`, `l.idx` (`wddense`): one bz2 stream is decompressed.
- JSON tab: the original JSON, indented, Kate highlighting and folding; Tree
  tab: keys and values, with View > Property names (`P31  instance of`, from
  `p.idx`).
- Entity ids in the tree (`Q5`, `P31`, Wikidata URLs of units, calendars,
  globes) are links: click or F2 goes to the entity; Go > Back / Forward
  (Alt+Left, Alt+Right) through the places shown, lines and entities.
- What points here: Go > What links here (Ctrl+L) shows in the list on the
  left the properties pointing to the entity (or to the entity of the current
  row of the tree) with their numbers of items, "P31 instance of — 9,793";
  Enter or a double click lists those items, the most popular first,
  Backspace goes back. The context menu of a value of a claim (or Ctrl+I)
  lists the items with the same property and value: on "occupation: writer"
  of Q42, all writers. From `target-counts/` (wdcount) and
  `triple-index-slim/` (wdfilter) in the index directory.
- Search > Find (Ctrl+F, F3, Shift+F3) in the JSON and the tree; F4 opens the
  JSON in Kate.
- Settings: the index directory (`q.idx`, `p.idx`, `l.idx`, `<date>.lst`,
  names, popularity), the dump directory and the slim directory of `wdconvert`
  (`slim/`: `items/`, `q.idx`…, `items.dict`…), kept in `wdbrowse.ini` next to
  the binary. Entities are read from the slim files first (one zstd frame of
  about 512 KiB: under a millisecond), from the dump when they are not there
  (items left out, about 30 ms); the status bar tells which. A merged entity
  (its page a redirect; `slim/redirects/`, about 5 million, read when first
  needed) opens the one it was merged into: "Q114496635 → merged into
  Q7024157", also through chains of redirects.

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
  `triples/NNN` (triples, see below), `sitelinks/NNN` (for each item with
  articles in language Wikipedias: uint32 Q, uint16 their number) and `index/NNN`, the bz2 blocks of the part
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
- `wdconvert [-j 20] [--level 9] [--frame 524288] [--removed removed] [-o slim] <dump directory> <date>.lst <Wikimedia index directory>`
  – the dump slimmed (no hashes, no statement ids, no lexeme counters;
  references kept; items of the mask `removed` left out) into
  `slim/items/NNN`, `slim/properties/NNN`, `slim/lexemes/NNN`: one entity per
  line in independent zstd frames of about 512 KiB, level 9, with a dictionary
  per kind (`items.dict`, `properties.dict`, `lexemes.dict`, trained first on
  a sample of streams and kept), each with its frame index `NNN.idx`
  (`offset:count:uncompressed`, `:start:id`, `EOF=`); `slim/redirects/NNN`:
  merged entities, `Q1:Q2`. Threads take parts from a queue; `.part` files,
  a stopped run continues. `zstd -D slim/items.dict -dc slim/items/000` reads a
  file.
- `wdfilter [-j 20] [--class 13442814] [-i triple-index] [-o triple-index-slim] [--popularity popularity] [--removed removed]`
  – the triple index without the items of a class in no Wikipedia (scholarly
  articles by default): items with P31 = the class (rank not deprecated) and 0
  in `popularity`, found by binary search in `triple-index/031`. Writes
  `removed`, the mask of the items left out (1 bit per Q: bit q % 8 of byte
  q / 8, also for the conversion of the dump), and the 256 files without their
  triples. Triples pointing to them from other items stay.
- `wdsample [-j 20] [--percent 3] [--seed 1] <dump directory> <date>.lst <Wikimedia index directory>`
  – what a slimmer Wikidata would take, on a random sample of whole bz2
  streams of all parts (offsets from the dump's own indexes): the items' JSON
  as it is, without hashes, without statement ids, without references, and the
  bytes of items in no Wikipedia by class (first P31); the same for the items
  kept (all but scholarly articles in no Wikipedia), compressed with zstd in
  frames of about 64 KiB, 256 KiB, 512 KiB and 1 MiB uncompressed, with
  references (and without, at 512 KiB), level 3, level 3 and level 19 with a
  dictionary trained on the sample; scaled to the whole dump. `--levels
  3,9,15,19` also compares zstd levels (512 KiB frames, dictionary): size and
  compression time.
- `wdsort [-j 20] [--by property|target] [triples [sort-triples]]` – sorts each triple file of
  `wdindex` by (bit-reversed property, target word, item word), leaves out repeated
  triples and writes it under the same name to `sort-triples`. Threads take
  files from a queue, the largest first; `NNN.part` is renamed when done.
- `wdmerge [-j 20] [--by property|target] [sort-triples [triple-index]]` – merges the sorted files into
  256 index files `triple-index/000` … `255`: file `NNN` holds the triples of
  all parts with property % 256 = `NNN`, sorted by (bit-reversed property,
  target word, item word), each triple once. Input files are memory-mapped; from each one
  a contiguous run (about 1/256 of it) goes to an output file.

- `wddense [-i index] [-s sitelinks] [-o .] <date>.lst` – dense indexes of
  entities from the block indexes of `wdindex` (no second pass over the dump):
  `q.idx`, `p.idx`, `l.idx`, and `popularity` from `sitelinks/`, see below.
  `wddense --slim slim <date>.lst`: the same for the slim files of
  `wdconvert`, from their frame indexes, into `slim/` (part = the file `NNN`
  of the entity's kind; no names, no popularity: those of the dump's indexes
  serve).

- `wdnames [-j 20] [-i index] [--slim slim] [-o .] <date>.lst` – name indexes
  of items from `index/` of `wdindex`: `names`, `names-by-q`, `q-by-name`, see
  below. `--slim`: the names of the items of the slim files from their labels
  (only the `labels` object of each line is parsed; minutes), the items left
  out keep those of `index/`.

## Building the reverse index of entity links

```
wdlist <dump directory>                                 # 20261001.lst
wdindex -j 20 <dump directory> 20261001.lst             # triples/, index/  (~1 h, Release build)
wdsort -j 20                                            # sort-triples/
wdmerge -j 20                                           # triple-index/
wddense 20261001.lst                                    # q.idx, p.idx, l.idx, popularity
wdfilter -j 20                                          # removed, triple-index-slim/
wdcount -j 20                                           # target-counts/
wdconvert -j 20 <dump directory> 20261001.lst <Wikimedia index directory>   # slim/ (~1 h)
wddense --slim slim 20261001.lst                        # slim/q.idx, p.idx, l.idx
```

Use a Release build: in a Debug build JSON parsing is about 15 times slower
and `wdindex` takes about 5 hours instead of 1.

### Triples
A triple has 12 bytes, 3 little-endian uint32 (`tools/triples.h`): property,
item with rank, target with type. The item is always a Q (claims of properties
and lexemes go to their own JSONL). Only claims whose main snak's value is an
entity with a `numeric-id` (item, property, lexeme) give triples, about 40–60%
of all claims; forms, senses and entity schemas (ids like `L7-F2`) are left
out.

| bytes | word | |
|---|---|---|
| 0–3 | property | 31 for P31 |
| 4–7 | item << 2 \| rank | 30 bits for the item, 2 bits rank: 0 deprecated, 1 normal, 2 preferred |
| 8–11 | target << 2 \| type | 30 bits for the target, 2 bits type: 0 item (Q), 1 property (P), 2 lexeme (L) |

The 2 bits are the low ones, so that sorting by a word sorts by its number; 30
bits reach 1073741823 (today Q is about 140 million). (31, 56782 << 2 | 1,
34770 << 2 | 0) is "Q56782 instance of (P31) Q34770", rank normal. Targets of
items are almost all Q (in a sample: 256785 Q, 8 P, 6 L). The "truthy" values
of a property are its preferred ones if there are any, else its normal ones;
deprecated values are wrong ones kept (with a reason, P2241). In a sample about
99.8% are normal.

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

### What points to an entity
`wdcount [-j 20] [-i triple-index-slim] [-o target-counts]` counts, for each
(target, property), the items pointing to the target (statements not
deprecated) – each such pair is one contiguous run of the triple index. A
record has 12 bytes, 3 little-endian uint32: target word (number << 2 | type),
property, count; 256 files by target number % 256, sorted by (bit-reversed
target number, type, property). All that points to Q34770 (language) is one
contiguous run of file `34770 % 256`: "P31 instance of 7812, P279 subclass of
312…"; the items of one property are then the run (P, Q34770) of the triple
index. No item numbers are repeated, so it is much smaller than the triples.

(`wdsort`/`wdmerge --by target` can also make the full index by target, with
the items: (bit-reversed target number, type, property, item word) – not
needed with the counts.)

### Lookup
"All items with occupation (P106) writer (Q36180)": file `triple-index/106`
(106 % 256), binary search for (reverse(106), 36180 << 2 | item): the items of
the run are the answer, in increasing order, with the rank of each statement.

### Dense indexes of entities
Numbers of entities are dense, so the entry of Q*N* is at byte N × 12 of
`q.idx`: one seek, no search. An entry has 12 bytes, little-endian:

| bytes | field |
|---|---|
| 0–1 | dump part: line `NNN` of the `.lst` |
| 2–7 | offset of the page's bz2 stream in that part (48 bits) |
| 8–11 | start of the page in the uncompressed stream |

An absent entity (never created, deleted, or merged into another: its page is a
redirect) has an entry of zeros – offset 0 is the siteinfo stream of every part,
never a page. `l.idx` is the same for lexemes. `p.idx` is small and also holds
the English labels, to show `instance of` instead of `P31`: uint32 count, then
16-byte entries (12 bytes as above, uint32 offset of the label in the names),
then the names, UTF-8, each ended by `\0`. Sizes: about 1.7 GB for Q (up to
Q~140 million), 20 MB for L, under 1 MB for P.

### Names of items
The name of an item is its English label, else its label for many languages
(`mul`: Wikidata keeps one label for names the same in many languages, such as
"Donald Trump", and then often no `en` – in some parts nearly all items), else
the first one; `en` comes first because where both differ `mul` is often a
catalogue name (Earth: "Sol 3"). One name per
item, not all labels, so the index does not repeat Wikidata. The names of all
items take 6.4 GB (121.8 million items, 52 bytes on average – titles of
scholarly articles and chemical names are long), so 4-byte offsets to bytes
would not do:
- `names`: the names in the order of Q numbers, UTF-8, each ended by `\0` and
  padded with a second `\0` to an even length; an empty name at offset 0.
- `names-by-q`: dense, uint32 per Q: offset of its name in 2-byte units (4
  bytes reach 8 GiB); 0 = no name. The name of any Q in one seek.
- `q-by-name`: the Q numbers of named items sorted by the search key of the
  name, then by Q. Binary search finds a name and, as one contiguous range,
  the names starting with a text – its size is known at once, the first ones
  are shown.

The search key (`tools/fold.h`, ICU) ignores case and diacritics: case
folding, NFD without combining marks, and letters that do not decompose by
hand (Łódź → lodz, Straße → strasse, Ærøskøbing → aeroskobing). Sorting and
searching must use the same function.

### Popularity
`popularity`: dense, uint16 per Q, the number of language Wikipedias with an
article on the item – its sitelinks `<code>wiki` (`plwiki`, `simplewiki`),
not the other projects ending in `wiki` (`commonswiki`, `specieswiki`,
`metawiki`, `wikidatawiki`…) nor Wiktionaries, Wikisources and the like. Earth
has over 300, a typical scholarly article 0. The browser ranks names found by
it: items with exactly the name searched first, then the others, in both
groups the most popular first (counting sort: linear, fast for millions).

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
- Items (Q): the JSON of the dump, slimmed, references kept, in zstd frames
  (independent, like the bz2 streams of the dump) of about 512 KiB
  uncompressed (about 80 items), level 9 with a dictionary trained on the
  data: about 19 GB for the items kept (sample of 3%: 1 MiB frames 17.5,
  64 KiB 23.2 GB at level 19; references cost about 3 GB; level 9 19.3 GB,
  level 19 18.0 GB, but level 9 adds about 2 minutes to the conversion and
  level 19 about an hour). Each converted file `NNN`
  has an index of its frames, with only the ids of the entities (names and
  other data are for the indexes of the tools, such as `index/` of wdindex):

  ```
  # wdconvert index: offset:count:uncompressed, then :start:id per entry; EOF=<size>
  0:100:412345            frame: offset in the file, entities, size uncompressed
  :0:Q315                 entity: start in the uncompressed frame, id
  :3785:P31
  38211:67:328120
  :0:Q54254
  EOF=5123456             size of the file
  ```

  As `index/NNN`: an entity ends where the next one starts, the last at the
  frame's size; a frame's compressed length is the next offset (or `EOF`)
  minus its own. A binary format was considered: simple for values that are
  entities (about 90% of claims), but dates, quantities, coordinates and
  monolingual texts each need an encoding of their own.
- Left out entirely: hashes of snaks and references, statement ids
  (`Q42$…`), lexeme counters `nextFormId`, `nextSenseId` – for editing only.
- References: to a separate zstd file, one line per item in the shape of its
  claims: `{"id":"Q42","references":{"P569":[[ref, ref], []]}}` – the
  references of statement *i* of a property are its element *i*.
- Scholarly articles (P31 = Q13442814) in no Wikipedia are left out: about two
  thirds of the JSON of all items (first sample). Their names stay in `names`,
  so links to them still show a name; the full entity is in the original dump.
- Properties (P, about 14 thousand) and lexemes (L, about 1.7 million): JSONL,
  slimmed the same way.
- Sizes (sample of 0.05%, scaled; wdsample --percent 3 to confirm): uncompressed
  XML 3.5 TB, items' JSON 1.93 TB, without hashes 1.56 TB, and without
  statement ids 1.46 TB, and without references 0.71 TB; items in no Wikipedia
  76% of items and 1.65 TB, of it scholarly articles 1.32 TB.
