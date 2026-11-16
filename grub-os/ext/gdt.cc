gdt_set_entry(3, base, limit, 0xFA, 0xCF); // user code
gdt_set_entry(4, base, limit, 0xF2, 0xCF); // user data
