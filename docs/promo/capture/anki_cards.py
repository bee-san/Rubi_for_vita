"""anki_cards.py PACKAGE OUTDIR: imports the package into a fresh Anki collection (Anki's own import, as the
repo's host/tests/test_apkg.py does) and writes, for every card, what Anki renders for it:
OUTDIR/card-<n>-front.html and -back.html (card.question() / card.answer(): the note type's CSS and the
rendered template), OUTDIR/media/ (the collection's media) and OUTDIR/cards.json (the notes' fields, the
deck, the note type). Run with the pinned anki 26.9.3 (tools/requirements-anki.txt)."""
import json
import os
import pathlib
import shutil
import sys
import tempfile

from anki.buildinfo import version
from anki.collection import Collection, ImportAnkiPackageOptions, ImportAnkiPackageRequest
from anki.import_export_pb2 import ImportAnkiPackageUpdateCondition as Cond

pkg, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
d = tempfile.mkdtemp()
col = Collection(os.path.join(d, "collection.anki2"))
opts = ImportAnkiPackageOptions(merge_notetypes=False, update_notes=Cond.IMPORT_ANKI_PACKAGE_UPDATE_CONDITION_IF_NEWER,
                                update_notetypes=Cond.IMPORT_ANKI_PACKAGE_UPDATE_CONDITION_IF_NEWER,
                                with_scheduling=False, with_deck_configs=False)
log = col.import_anki_package(ImportAnkiPackageRequest(package_path=str(pkg), options=opts)).log
info = {"anki": version, "package": pkg.name, "new_notes": len(log.new), "cards": []}
cids = sorted(col.find_cards(""), key=lambda c: col.get_card(c).nid)
media = out / "media"
media.mkdir(exist_ok=True)
for f in os.listdir(col.media.dir()):
    shutil.copy(os.path.join(col.media.dir(), f), media / f)
for n, cid in enumerate(cids, 1):
    card = col.get_card(cid)
    note = card.note()
    model = note.note_type()
    (out / f"card-{n}-front.html").write_text(card.question(), encoding="utf-8")
    (out / f"card-{n}-back.html").write_text(card.answer(), encoding="utf-8")
    info["cards"].append({"n": n, "deck": col.decks.name(card.did), "notetype": model["name"],
                          "template": card.template()["name"], "guid": note.guid,
                          "fields": {k: note[k] for k in note.keys()}})
info["media"] = sorted(os.listdir(media))
(out / "cards.json").write_text(json.dumps(info, ensure_ascii=False, indent=1), encoding="utf-8")
col.close()
print(json.dumps({k: info[k] for k in ("anki", "package", "new_notes", "media")}, ensure_ascii=False),
      [(c["notetype"], c["deck"], c["fields"].get("Expression")) for c in info["cards"]])
