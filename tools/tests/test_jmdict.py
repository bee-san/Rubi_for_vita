from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_jmdict import entry_rows, rules_for


class JMdictConversion(unittest.TestCase):
    def test_reading_and_sense_restrictions_and_inherited_rules(self):
        entry = ET.fromstring("""<entry><ent_seq>1</ent_seq>
        <k_ele><keb>甲</keb></k_ele><k_ele><keb>乙</keb></k_ele>
        <r_ele><reb>こう</reb><re_restr>甲</re_restr></r_ele>
        <r_ele><reb>おつ</reb><re_restr>乙</re_restr></r_ele>
        <sense><pos>Ichidan verb</pos><stagk>甲</stagk><gloss>first spelling only</gloss></sense>
        <sense><stagr>こう</stagr><gloss>inherited verb rule</gloss></sense>
        <sense><pos>noun (common)</pos><gloss>both spellings</gloss>
        <gloss xml:lang="ger">excluded foreign sense</gloss></sense></entry>""")
        rows = list(entry_rows(entry))
        self.assertEqual([(r[0], r[1], r[3]) for r in rows], [("甲", "こう", "v1"), ("乙", "おつ", "")])
        self.assertEqual(len(rows[0][5]), 3)
        self.assertEqual(len(rows[1][5]), 1)
        self.assertNotIn("excluded", " ".join(rows[0][5]))

    def test_kana_only_and_irregular_deinflection(self):
        entry = ET.fromstring("""<entry><ent_seq>2</ent_seq><k_ele><keb>仮</keb></k_ele>
        <r_ele><reb>かな</reb><re_nokanji/></r_ele>
        <sense><pos>noun</pos><gloss>test kana sense</gloss></sense></entry>""")
        row = next(entry_rows(entry))
        self.assertEqual(row[:2], ["かな", ""])
        for label, rule in [("Godan verb with 'u' ending", "v5"),
                            ("Kuru verb - special class", "vk"),
                            ("suru verb - special class", "vs"),
                            ("Ichidan verb - zuru verb", "vz"),
                            ("adjective (keiyoushi)", "adj-i")]:
            self.assertEqual(rules_for([label]), rule)
