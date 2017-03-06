from __future__ import print_function

import sys
import re
from elftools.elf import elffile

import debuginfo

ELF64_R_INFO = lambda s, t: (s << 32) | t

PREFIX = 'vzpatch'
prefix_re = re.compile(r'^{0}_(\d+_)*'.format(PREFIX))

old_file   = sys.argv[1]
patch_file = sys.argv[2]

p_elf = elffile.ELFFile(open(patch_file, 'r+'))
o_elf = elffile.ELFFile(open(old_file))

def is_mangled(n):
	return n.startswith(PREFIX)

def demangle(n):
	return prefix_re.sub('', n)

def reverse_mapping(d):
	result = dict((v,k) for k, v in d.iteritems())
	assert len(result) == len(d)
	return result

sym_info  = list()
sym_names = set()
symtab = p_elf.get_section_by_name('.dynsym')

print("== Searching symbols to process")
for sym_idx, sym in enumerate(symtab.iter_symbols()):
	if not is_mangled(sym.name):
		continue

	sym_name = sym.name
	addr = sym.entry.st_value
	print("{0:<4d} {1:016x} {2}".format(sym_idx, addr, sym_name))

	sym_name_orig = demangle(sym_name)
	sym_info.append((sym_idx, sym_name_orig, addr))
	sym_names.add(sym_name_orig)

print("== Reading debuginfo for old ELF")
o_di2addr = debuginfo.read(o_elf, sym_names)

print("== Reading debuginfo for new ELF")
p_di2addr = debuginfo.read(p_elf, sym_names,
		lambda n: demangle(n) if is_mangled(n) else n)

p_addr2di = reverse_mapping(p_di2addr)

print("== Resolving addresses in old ELF")
for sym_idx, sym_name, p_addr in sym_info:
	o_addr = o_di2addr[p_addr2di[p_addr]]

	print("{0:<4d} {1:016x} {2}".format(sym_idx, o_addr, sym_name))
