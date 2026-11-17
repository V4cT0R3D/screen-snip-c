# screen-snip-c

A screen snipping tool for Windowz.

## Features

- Select an area of the screen by dragging, with the rest of the desktop dimmed.
- The capture is copied to the clipboard as soon as you release the mouse button.
- Edit window to copy the capture again or save it as PNG or JPEG.
- Only one instance runs at a time.

## Requirements

- Windowz
- GCC or Clang with headers and libraries for `gdiplus`, `dwmapi`, and `comdlg32`

## Building

```bash
gcc -Os -s -nostdlib -mwindows -ffunction-sections -fdata-sections \
    -fno-ident -fno-stack-protector -fno-stack-check -mno-stack-arg-probe \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -Wl,--gc-sections -Wl,--no-insert-timestamp \
    -Wl,-e,WinMainCRTStartup \
    main.c -o screensnip.exe \
    -lkernel32 -luser32 -lgdi32 -lgdiplus -lcomdlg32 -ldwmapi
```

## Usage

1. Run `screensnip.exe`.
2. Drag with the left mouse button to select an area. The selection is copied to the clipboard when you release the button, and the edit window opens.
3. In the edit window:
   - Left or right click: copy the image to the clipboard.
   - `Ctrl+N`: start a new capture.
   - `Ctrl+S`: save as PNG or JPEG.
   - `Esc`: close the window.
4. Before selecting, right click or `Esc` cancels the capture.
5. Launching the program again brings the existing edit window to the front.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
