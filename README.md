# st with graphics

This is a fork of [st](https://st.suckless.org/) that implements a subset of
[kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/).

## Installation

As usual, copy `config.def.h` to `config.h`, modify it according to your needs,
run `make install`.

Additional dependencies required for the graphics module: imlib2, zlib

## Configuration

You may want to change the graphics-related shortcuts and image size limits (see
`config.def.h`).

Default shortcuts:
- `Ctrl+Shift+RightClick` to preview the clicked image in feh.
- `Ctrl+Shift+MiddleClick` to see debug info (image id, placement id, etc).
- `Ctrl+Shift+F1` to enter graphics debug mode. It will show bounding boxes,
  general info, and will also print logs to stderr.
- `Ctrl+Shift+F6` to dump the state of all images to stderr.
- `Ctrl+Shift+F7` to unload all images from ram (but the cache in `/tmp` will be
  preserved).
- `Ctrl+Shift+F8` to toggle image display.

## Features

Originally I implemented it to prototype the Unicode placeholder feature (which
is now included in the kitty graphics protocol). Classic placements were
retrofitted later, and under the hood they are implemented via the same
placeholder mechanism. This means that a cell may be occupied only by one
placement.  It is the main reason why some things don't work or work a bit
differently.

Here is the list of supported (✅ ), unsupported (❌), and non-standard (⚡)
features.

- Uploading:
    - Formats:
        - ✅ PNG (`f=100`)
        - ✅ RGB, RGBA (`f=24`, `f=32`)
        - ✅ Compression with zlib (`o=z`)
        - ⚡ jpeg. Actually any format supported by imlib2 should work. The key
          value is the same as for png (`f=100`).
    - Transmission mediums:
        - ✅ Direct (`m=d`)
        - ✅ File (`m=f`)
        - ❌ Temporary file (`m=t`) - the image gets uploaded, but the original
          file is not deleted.
        - ❌ Shared memory object (`m=s`)
    - ❌ Size and offset specification (`S` and `O` keys)
    - ✅ Image numbers
    - ✅ Responses
    - ✅ Transmit and display (`a=T`)
- Placement:
    - ✅ Classic placements (but see the note above)
    - ✅ Unicode placeholders
    - ✅ Placement IDs
    - ❌ NOTE: Placement IDs must be 24-bit (between 1 and 16777215).
    - ✅ Cursor movement policies `C=1` and `C=0`
    - ✅ Source rectangle (`x, y, w, h`)
    - ✅ The number of rows/columns (`r, c`)
    - ❌ Cell offsets (`X, Y`)
    - ❌ z-index. Classic placements will erase old placements and the text on
      overlap.
    - ❌ Relative placements (`P, Q, H, V`)
- Deletion:
    - ✅ Deletion of image data when the specifier is uppercase
    - ✅ All visible classic placements (`d=a`)
    - ✅ By image id/number and placement id (`d=i`, `d=n`)
    - ❌ By position (specifiers `c, p, q, x, y, z`)
    - ❌ Animation frames (`d=f`)
- ❌ Animation - completely unsupported

## Things I have tested

### Apps that seem to work
- Kitty's icat (including from within tmux).
- [termpdf](https://github.com/dsanson/termpdf.py)
- [ranger](https://github.com/ranger/ranger) - I had to explicitly set
  `TERM=kitty`. This should be fixed in ranger of course.
- [tpix](https://github.com/jesvedberg/tpix)
- [pixcat](https://github.com/mirukana/pixcat) (modulo some unsupported keys
  that are currently ignored).
- [viu](https://github.com/atanunq/viu) - I had to explicitly set
  `TERM=kitty`.
- [timg](https://github.com/hzeller/timg) - I had to explicitly pass `-pk`
  (i.e. `timg -pk <image>`). If your timg is fresh enough, it even works in
  tmux!

### Apps that sort of work
- [hologram.nvim](https://github.com/edluffy/hologram.nvim) - There are some
  glitches, like erasure of parts of the status line.

### Apps that don't work
- [termvisage](https://github.com/AnonymouX47/termvisage) - seems to erase
  cells containing the image after image placement. In kitty this has no effect
  on classic placements because they aren't attached to cells, but in
  st-graphics classic placements are implemented on top of Unicode placements,
  so they get erased.
