# Translations

The UI strings for all 41 languages are in the `STRINGS` table of `radio/ui/prototype.html`, one object per language
code, keyed like the English table. `radio/tools/gen_strings.py` turns them into the firmware tables, and
`GET /api/strings` serves the current language to the web page.

All translations need review by native speakers. They were written to fit the 128x32 display, so many use short
forms or abbreviations (for example the text entry title, which shares its row with the character group, the
Mute flag on the home screen, which has room for about five letters, and the RDS programme types, which share the
Station screen's title row with an 8-character station name). These are the least certain: Walloon
(Feller spelling), Irish, Faroese, Maltese and Swahili.

## Changing a translation

1. Edit the language's line in the `STRINGS` table of `radio/ui/prototype.html`. Keep `%d`, `%s` and `%%` exactly as in
   English, and write each string in the case it should appear: the UI never changes case.
2. Run `make test`. The `strings_fit_slots` test fails, naming the string and its width, when a string is too
   wide for a slot on any screen. `strings_have_glyphs` fails when the font lacks a character.
3. Open the prototype in a browser, choose the language in Settings, and check the screens.

## Adding a language

Add the code and the language's own name to `LANGS`, add its table to `STRINGS`, add any missing letters to the
`PIP` font table, and run `make test`. The radio stores the language as an index into `LANGS`, so new languages
go at the end of the list.
