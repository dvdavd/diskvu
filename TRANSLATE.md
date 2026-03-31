# Translations

Translation files live here. Each language is a Qt `.ts` file (e.g. `diskscape_fr.ts`). Compiled `.qm` files are embedded in the app binary at build time.

**Add a new language:**

1. Add the `.ts` file path to `qt_add_translations()` in `CMakeLists.txt`:
   ```cmake
   qt_add_translations(diskscape
       TS_FILES
           translations/diskscape_fr.ts
           translations/diskscape_de.ts   # new
   )
   ```
2. Create a stub file `translations/diskscape_de.ts`:
   ```xml
   <?xml version="1.0" encoding="utf-8"?>
   <!DOCTYPE TS>
   <TS version="2.1" language="de_DE" sourcelanguage="en">
   </TS>
   ```
3. Extract source strings into the new file:
   ```bash
   cmake --build build --target diskscape_lupdate
   ```
4. Open the `.ts` file in Qt Linguist (or any text editor) and translate the strings.
5. Rebuild — the `.qm` file is compiled and embedded automatically.

The active language is picked up from `LANG`/`LC_ALL` at runtime. To test a translation before installing:

```bash
LANG=de_DE.UTF-8 ./build/diskscape
```
