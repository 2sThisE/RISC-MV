# CVM linker

`cvmlink` combines relocatable CVM objects into a bootable kernel image.

```powershell
.\build\tools\vmasm.exe kernel.s -c -o kernel.o
.\build\tools\cvmlink.exe kernel.o support.o `
    -o KERNEL.CVM --map kernel.map
```

The file naming follows common native-toolchain conventions: `.s` is assembly
source, `.o` is a relocatable object, and `.a` is reserved for a future static
archive format. `.cvm` remains the platform-specific executable/boot-image
extension. Legacy flat assembly continues to support `.asm` to `.bin`.

The linker emits separate page-aligned load segments for `.text` (`r-x`),
`.rodata` (`r--`), `.data` (`rw-`), and `.bss` (`rw-`, zero-filled).
