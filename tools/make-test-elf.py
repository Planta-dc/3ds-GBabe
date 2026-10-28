"""Build a minimal but valid CTR-style ARM ELF, so makerom can be exercised
offline (makerom needs an executable ARM32 ELF with text/rodata/data PT_LOAD
segments). This is a test fixture, not part of the product."""
import struct
import sys

TEXT_VADDR = 0x00100000
RO_VADDR = 0x00110000
RW_VADDR = 0x00120000
PAGE = 0x1000


def build(text: bytes, rodata: bytes, data: bytes) -> bytes:
    # pad each segment up to a page boundary
    def pad(b):
        return b + b"\x00" * (-len(b) % PAGE)

    text, rodata, data = pad(text), pad(rodata), pad(data)

    ehsize, phentsize, shentsize = 52, 32, 40
    phnum = 3

    shstrtab = b"\x00.text\x00.rodata\x00.data\x00.shstrtab\x00"
    # one section header per segment + null + shstrtab
    shnum = 1 + 3 + 1

    body_off = ehsize + phnum * phentsize
    text_off = body_off
    rodata_off = text_off + len(text)
    data_off = rodata_off + len(rodata)
    shstr_off = data_off + len(data)
    shoff = shstr_off + len(shstrtab)

    segments = [
        # type, offset, vaddr, filesz, memsz, flags, align
        (1, text_off, TEXT_VADDR, len(text), len(text), 5, PAGE),      # PF_R|PF_X
        (1, rodata_off, RO_VADDR, len(rodata), len(rodata), 4, PAGE),  # PF_R
        (1, data_off, RW_VADDR, len(data), len(data), 6, PAGE),        # PF_R|PF_W
    ]

    phdrs = b"".join(
        struct.pack("<IIIIIIII", t, o, v, v, fsz, msz, fl, al)
        for (t, o, v, fsz, msz, fl, al) in segments
    )

    def shdr(name, typ, flags, addr, off, size, align):
        return struct.pack("<IIIIIIIIII", name, typ, flags, addr, off, size,
                           0, 0, align, 0)

    shstr_names = {".text": 1, ".rodata": 7, ".data": 15, ".shstrtab": 21}
    shdrs = shdr(0, 0, 0, 0, 0, 0, 0)
    shdrs += shdr(shstr_names[".text"], 1, 6, TEXT_VADDR, text_off, len(text), PAGE)
    shdrs += shdr(shstr_names[".rodata"], 1, 2, RO_VADDR, rodata_off, len(rodata), PAGE)
    shdrs += shdr(shstr_names[".data"], 1, 3, RW_VADDR, data_off, len(data), PAGE)
    shdrs += shdr(shstr_names[".shstrtab"], 3, 0, 0, shstr_off, len(shstrtab), 1)

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2,        # ET_EXEC
        40,       # EM_ARM
        1,        # e_version
        TEXT_VADDR,
        ehsize,   # e_phoff
        shoff,    # e_shoff
        0x05000000,  # e_flags (EABI v5)
        ehsize,
        phentsize,
        phnum,
        shentsize,
        shnum,
        4,        # e_shstrndx (index of .shstrtab)
    )

    out = ehdr + phdrs + text + rodata + data + shstrtab + shdrs
    assert len(out) == shoff + shnum * shentsize
    return out


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "test.elf"
    # A couple of ARM NOPs (mov r0, r0) as "code"
    elf = build(text=b"\x00\x00\xa0\xe1" * 16, rodata=b"GBABE", data=b"\x00" * 64)
    with open(path, "wb") as f:
        f.write(elf)
    print(f"wrote {path} ({len(elf)} bytes)")
