# screen-snip-c

Herramienta de recorte de pantalla para Windows escrita en C

## Compilación

```bash
gcc -Os -s -nostdlib -mwindows -ffunction-sections -fdata-sections -fno-ident -fno-stack-protector -fno-stack-check -mno-stack-arg-probe -fno-asynchronous-unwind-tables -fno-unwind-tables -Wl,--gc-sections -Wl,--no-insert-timestamp main.c -o screensnip.exe -s -lkernel32 -luser32 -lgdi32 -lgdiplus -lcomdlg32
```
## Uso

1. Compila y Ejecuta `screensnip.exe`.
2. Arrastra con el botón izquierdo para seleccionar.
3. Al soltar, la captura se copia al portapapeles y se abre la ventana de edición.
4. En la ventana de edición:
   - Clic izquierdo/derecho: copia la imagen al portapapeles.
   - `Ctrl+S`: guarda en PNG o JPG.
   - `Esc`: cierra.
5. Clic derecho o `Esc` antes de seleccionar cancela la captura.

## Licencia

Este proyecto está bajo la licencia MIT. Consulta el archivo [LICENSE](LICENSE) para más detalles.
