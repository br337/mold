// MIPS is a RISC ISA developed in the '80s. The processor was once fairly
// popular; for examples, Silicon Graphics workstations and Nintendo 64
// game consoles are based on the processor. Even though it's no longer a
// popular choice when creating a new system, there are still many uses of
// the ISA especially in the network router segment.
//
// The MIPS psABIs are in a sad state due to the lack of ownership of the
// ABI. The last major Unix vendor in the MIPS market was Silicon
// Graphics, which effectively ceased its MIPS-based Unix workstation
// business in the '90s. Even at the time the MIPS ABIs looked peculiar.
// After that, various small vendors used MIPS to create appliances and
// notably routers, but no one tried to modernize or improve the ABIs. As
// a result, the MIPS ABIs left as probably the most diverged ABI compared
// to the other psABIs.
//
// Specifically, the MIPS ABIs has the following issues:
//
// 1. Since the ISA does not support PC-relative addressing, each function
//    first materializes the address of GOT + 0x7ff0 in the GP register
//    and access GOT entries relative to the GP's value. This GP-relative
//    access is usually done with a single load instruction with a 16-bit
//    offset. That means only GP ± 32 KiB is addressable. If GOT is larger
//    than that, the linker is expected to create a GOT section for each
//    input file and associate a different GP value for each GOT. This
//    method is called "multi-GOT". Multi-GOT is not necessary for other
//    ABIs because other processors either simply support PC-relative
//    addressing or use two instructions to access GOT entries.
//
// 2. The MIPS ABIs require .dynsym entries to be sorted in a very
//    specific manner to represent some dynamic relocations implicitly
//    rather than explicitly in the .rela.dyn section. This feature is
//    called "Quickstart" in the MIPS documentation.
//
// 3. Unlike other psABIs, a MIPS relocation record can have up to three
//    types -- that is, each record has not only r_type but also r_type2
//    and r_type3. A relocated value is computed by the combination of all
//    the relocation types.
//
// In our MIPS support, we prioritize simplicity of implementation over
// marginal runtime efficiency. Specifically, we made the following
// decisions for simplification:
//
// 1. We do not support multi-GOT. Instead, we'll print out an error
//    message to ask the user to recompile code with the medium code model
//    with the `-mxgot` option if the (single) GOT became too large.
//
// 2. We do not sort .dynsym entries. Quickstart still kicks in at the
//    load-time (there's no way to tell the loader to disable Quickstart),
//    and the loader writes resolved addresses to the beginning of
//    .mips_got. We just ignore these relocated values.
//
// 3. Instead of supporting arbitrary combinations of relocation types, we
//    support only a limited set of them. This works because, in practice,
//    the compiler emits only a limted set of relocation types.

#include "mold.h"

namespace mold::elf {

static constexpr i64 BIAS = 0x8000;

// We don't support lazy symbol resolution for MIPS. All dynamic symbols
// are resolved eagerly on process startup.
template <typename E>
void write_plt_header(Context<E> &ctx, u8 *buf) {}

template <typename E>
void write_plt_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {}

template <typename E>
void write_pltgot_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {}

template <typename E>
void EhFrameSection<E>::apply_eh_reloc(Context<E> &ctx, const ElfRel<E> &rel,
                                       u64 offset, u64 val) {
  u8 *loc = ctx.buf + this->shdr.sh_offset + offset;

  switch (rel.r_type) {
  case R_NONE:
    break;
  case R_MIPS_64:
    // We relocate R_MIPS_64 in .eh_frame as a relative relocation.
    // See the comment for mips_rewrite_cie() below.
    *(U64<E> *)loc = val - this->shdr.sh_addr - offset;
    break;
  default:
    Fatal(ctx) << "unsupported relocation in .eh_frame: " << rel;
  }
}

template <typename E>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  ElfRel<E> *dynrel = nullptr;
  if (ctx.reldyn)
    dynrel = (ElfRel<E> *)(ctx.buf + ctx.reldyn->shdr.sh_offset +
                           file.reldyn_offset + this->reldyn_offset);

  u64 GP = ctx._gp->get_addr(ctx);
  u64 GP0 = file.extra.gp0;

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    auto check = [&](i64 val, i64 lo, i64 hi) {
      if (val < lo || hi <= val)
        Error(ctx) << *this << ": relocation " << rel << " against "
                   << sym << " out of range: " << val << " is not in ["
                   << lo << ", " << hi << "); recompile with -mxgot";
    };

    auto write_hi16 = [&](u64 val) {
      check(val, -(1LL << 31), 1LL << 31);
      *(U32<E> *)loc |= ((val + BIAS) >> 16) & 0xffff;
    };

    auto write_lo16 = [&](u64 val) {
      check(val, -(1 << 15), 1 << 15);
      *(U32<E> *)loc |= val & 0xffff;
    };

    auto write_lo16_nc = [&](u64 val) {
      *(U32<E> *)loc |= val & 0xffff;
    };

    u64 S = sym.get_addr(ctx);
    u64 A = rel.r_addend;
    u64 P = get_addr() + rel.r_offset;
    u64 G = sym.get_got_idx(ctx) * sizeof(Word<E>);
    u64 GOT = ctx.got->shdr.sh_addr;

    switch (rel.r_type) {
    case R_MIPS_64:
      apply_toc_rel(ctx, sym, rel, loc, S, A, P, &dynrel);
      break;
    case R_MIPS_GPREL16 | (R_MIPS_SUB << 8) | (R_MIPS_HI16 << 16): {
      u64 val = sym.is_local(ctx) ? (S + A + GP0 - GP) : (S + A - GP);
      write_hi16(-val);
      break;
    }
    case R_MIPS_GPREL16 | (R_MIPS_SUB << 8) | (R_MIPS_LO16 << 16): {
      u64 val = sym.is_local(ctx) ? (S + A + GP0 - GP) : (S + A - GP);
      write_lo16_nc(-val);
      break;
    }
    case R_MIPS_GPREL32 | (R_MIPS_64 << 8):
      *(U64<E> *)loc = S + A + GP0 - GP;
      break;
    case R_MIPS_GOT_DISP:
      if (A == 0)
        write_lo16(G + GOT - GP);
      else
        write_lo16(ctx.extra.got->get_got_addr(ctx, sym, A) - GP);
      break;
    case R_MIPS_CALL_HI16:
    case R_MIPS_GOT_HI16:
      write_hi16(G + GOT - GP);
      break;
    case R_MIPS_CALL16:
    case R_MIPS_CALL_LO16:
    case R_MIPS_GOT_LO16:
      write_lo16(G + GOT - GP);
      break;
    case R_MIPS_GOT_PAGE:
      write_lo16(ctx.extra.got->get_gotpage_got_addr(ctx, sym, A) - GP);
      break;
    case R_MIPS_GOT_OFST:
      write_lo16(0);
      break;
    case R_MIPS_JALR:
      break;
    case R_MIPS_TLS_TPREL_HI16:
      write_hi16(S + A - ctx.tp_addr);
      break;
    case R_MIPS_TLS_TPREL_LO16:
      write_lo16_nc(S + A - ctx.tp_addr);
      break;
    case R_MIPS_TLS_GOTTPREL:
      write_lo16(sym.get_gottp_addr(ctx) - GP);
      break;
    case R_MIPS_TLS_DTPREL_HI16:
      write_hi16(S + A - ctx.dtp_addr);
      break;
    case R_MIPS_TLS_DTPREL_LO16:
      write_lo16_nc(S + A - ctx.dtp_addr);
      break;
    case R_MIPS_TLS_GD:
      write_lo16(sym.get_tlsgd_addr(ctx) - GP);
      break;
    case R_MIPS_TLS_LDM:
      write_lo16(ctx.got->get_tlsld_addr(ctx) - GP);
      break;
    default:
      unreachable();
    }
  }
}

template <typename E>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    SectionFragment<E> *frag;
    i64 frag_addend;
    std::tie(frag, frag_addend) = get_fragment(ctx, rel);

    u64 S = frag ? frag->get_addr(ctx) : sym.get_addr(ctx);
    u64 A = frag ? frag_addend : get_addend(loc, rel);

    switch (rel.r_type) {
    case R_MIPS_64:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(U64<E> *)loc = *val;
      else
        *(U64<E> *)loc = S + A;
      break;
    case R_MIPS_32:
      *(U32<E> *)loc = S + A;
      break;
    default:
      Fatal(ctx) << *this << ": invalid relocation for non-allocated sections: "
                 << rel;
    }
  }
}

template <typename E>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr().sh_flags & SHF_ALLOC);

  this->reldyn_offset = file.num_dynrel * sizeof(ElfRel<E>);
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];

    switch (rel.r_type) {
    case R_MIPS_64:
      scan_toc_rel(ctx, sym, rel);
      break;
    case R_MIPS_GOT_DISP:
      if (rel.r_addend == 0) {
        sym.flags |= NEEDS_GOT;
      } else {
        std::scoped_lock lock(ctx.extra.got->mu);
        ctx.extra.got->got_syms.push_back({&sym, rel.r_addend});
      }
      break;
    case R_MIPS_CALL16:
    case R_MIPS_CALL_HI16:
    case R_MIPS_CALL_LO16:
    case R_MIPS_GOT_HI16:
    case R_MIPS_GOT_LO16:
      assert(rel.r_addend == 0);
      sym.flags |= NEEDS_GOT;
      break;
    case R_MIPS_GOT_PAGE:
    case R_MIPS_GOT_OFST: {
      std::scoped_lock lock(ctx.extra.got->mu);
      ctx.extra.got->gotpage_syms.push_back({&sym, rel.r_addend});
      break;
    }
    case R_MIPS_TLS_GOTTPREL:
      assert(rel.r_addend == 0);
      sym.flags |= NEEDS_GOTTP;
      break;
    case R_MIPS_TLS_TPREL_HI16:
    case R_MIPS_TLS_TPREL_LO16:
      check_tlsle(ctx, sym, rel);
      break;
    case R_MIPS_TLS_GD:
      assert(rel.r_addend == 0);
      sym.flags |= NEEDS_TLSGD;
      break;
    case R_MIPS_TLS_LDM:
      ctx.needs_tlsld = true;
      break;
    case R_MIPS_GPREL16 | (R_MIPS_SUB << 8) | (R_MIPS_HI16 << 16):
    case R_MIPS_GPREL16 | (R_MIPS_SUB << 8) | (R_MIPS_LO16 << 16):
    case R_MIPS_GPREL32 | (R_MIPS_64 << 8):
    case R_MIPS_JALR:
    case R_MIPS_TLS_DTPREL_HI16:
    case R_MIPS_TLS_DTPREL_LO16:
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

template <typename E>
bool MipsGotSection<E>::SymbolAddend::operator<(const SymbolAddend &other) const {
  return std::tuple(sym->file->priority, sym->sym_idx, addend) <
         std::tuple(other.sym->file->priority, other.sym->sym_idx, other.addend);
};

template <typename E>
static bool compare(const Symbol<E> *a, const Symbol<E> *b) {
  return std::tuple(a->file->priority, a->sym_idx) <
         std::tuple(b->file->priority, b->sym_idx);
};

template <typename E>
u64 MipsGotSection<E>::SymbolAddend::get_addr(Context<E> &ctx, i64 flags) const {
  return sym->get_addr(ctx, flags) + addend;
}

template <typename E>
u64
MipsGotSection<E>::get_got_addr(Context<E> &ctx, Symbol<E> &sym, i64 addend) const {
  auto it = std::lower_bound(got_syms.begin(), got_syms.end(),
                             SymbolAddend{&sym, addend});
  assert(it != got_syms.end());
  i64 idx = NUM_RESERVED + ctx.dynsym->symbols.size() + (it - got_syms.begin());
  return this->shdr.sh_addr + idx * sizeof(Word<E>);
}

template <typename E>
u64 MipsGotSection<E>::get_gotpage_got_addr(Context<E> &ctx, Symbol<E> &sym,
                                            i64 addend) const {
  auto it = std::lower_bound(gotpage_syms.begin(), gotpage_syms.end(),
                             SymbolAddend{&sym, addend});
  assert(it != gotpage_syms.end());
  i64 idx = NUM_RESERVED + ctx.dynsym->symbols.size() + got_syms.size() +
            (it - gotpage_syms.begin());
  return this->shdr.sh_addr + idx * sizeof(Word<E>);
}

template <typename E>
u64 MipsGotSection<E>::get_gotpage_page_addr(Context<E> &ctx, Symbol<E> &sym,
                                            i64 addend) const {
  auto it = std::lower_bound(gotpage_syms.begin(), gotpage_syms.end(),
                             SymbolAddend{&sym, addend});
  assert(it != gotpage_syms.end());
  return it->get_addr(ctx);
}

template <typename E>
std::vector<typename MipsGotSection<E>::GotEntry>
MipsGotSection<E>::get_got_entries(Context<E> &ctx) const {
  std::vector<GotEntry> entries;
  auto add = [&](GotEntry ent) { entries.push_back(ent); };

  // Create GOT entries for ordinary symbols
  for (const SymbolAddend &ent : got_syms) {
    // If a symbol is imported, let the dynamic linker to resolve it.
    if (ent.sym->is_imported) {
      add({0, E::R_DYNAMIC, ent.sym});
      continue;
    }

    // If we know an address at link-time, fill that GOT entry now.
    // It may need a base relocation, though.
    if (ctx.arg.pic && ent.sym->is_relative())
      add({ent.get_addr(ctx, NO_PLT), E::R_RELATIVE});
    else
      add({ent.get_addr(ctx, NO_PLT)});
  }

  // Create GOT entries for GOT_PAGE and GOT_OFST relocs
  for (const SymbolAddend &ent : gotpage_syms) {
    if (ctx.arg.pic && ent.sym->is_relative())
      add({ent.get_addr(ctx), E::R_RELATIVE});
    else
      add({ent.get_addr(ctx)});
  }

  return entries;
}

template <typename E>
void MipsGotSection<E>::update_shdr(Context<E> &ctx) {
  // Finalize got_syms
  sort(got_syms);
  remove_duplicates(got_syms);

  // Finalize gotpage_syms
  sort(gotpage_syms);
  remove_duplicates(gotpage_syms);

  // The first two slots are reserved followed by slots for Quickstart.
  i64 n = NUM_RESERVED + ctx.dynsym->symbols.size() +
          got_syms.size() + gotpage_syms.size();
  this->shdr.sh_size = n * sizeof(Word<E>);
}

template <typename E>
i64 MipsGotSection<E>::get_reldyn_size(Context<E> &ctx) const {
  i64 n = 0;
  for (GotEntry &ent : get_got_entries(ctx))
    if (ent.r_type != R_NONE)
      n++;
  return n;
}

template <typename E>
void MipsGotSection<E>::copy_buf(Context<E> &ctx) {
  U64<E> *buf = (U64<E> *)(ctx.buf + this->shdr.sh_offset);
  memset(buf, 0, this->shdr.sh_size);

  // It is not clear how the runtime uses it, but all MIPS binaries
  // have this value in GOT[1].
  buf[1] = E::is_64 ? 0x8000'0000'0000'0000 : 0x8000'0000;

  for (i64 i = 0; i < ctx.dynsym->symbols.size(); i++)
    if (Symbol<E> *sym = ctx.dynsym->symbols[i])
      if (!sym->file->is_dso && !sym->esym().is_undef())
        buf[i + NUM_RESERVED] = sym->get_addr(ctx, NO_PLT);

  ElfRel<E> *dynrel = (ElfRel<E> *)(ctx.buf + ctx.reldyn->shdr.sh_offset +
                                    this->reldyn_offset);

  for (i64 i = NUM_RESERVED + ctx.dynsym->symbols.size();
       GotEntry &ent : get_got_entries(ctx)) {
    if (ent.r_type != R_NONE)
      *dynrel++ = ElfRel<E>(this->shdr.sh_addr + i * sizeof(Word<E>),
                            ent.r_type,
                            ent.sym ? ent.sym->get_dynsym_idx(ctx) : 0,
                            ent.val);
    buf[i++] = ent.val;
  }
}

// MIPS .eh_frame contains absolute addresses (i.e. R_MIPS_64 relocations)
// even if compiled with -fPIC. Instead of emitting dynamic relocations,
// we rewrite CIEs to convert absolute addresses to relative ones.
template <typename E>
void mips_rewrite_cie(Context<E> &ctx, u8 *buf, CieRecord<E> &cie) {
  u8 *aug = buf + 9; // Skip Length, CIE ID and Version fields
  if (*aug != 'z')
    return;
  aug++;

  // Skip Augmentation String
  u8 *p = aug + strlen((char *)aug) + 1;

  read_uleb(&p); // Skip Code Alignment Factor
  read_uleb(&p); // Skip Data Alignment Factor
  p++;           // Skip Return Address Register
  read_uleb(&p); // Skip Augmentation Data Length

  auto rewrite = [&](u8 *ptr) {
    i64 sz;

    switch (*ptr & 0xf) {
    case DW_EH_PE_absptr:
      sz = sizeof(Word<E>);
      break;
    case DW_EH_PE_udata4:
    case DW_EH_PE_sdata4:
      sz = 4;
      break;
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8:
      sz = 8;
      break;
    default:
      Fatal(ctx) << cie.input_section << ": unknown pointer size";
    }

    if ((*ptr & 0x70) == DW_EH_PE_absptr) {
      if (sz == 4)
        *ptr = (*ptr & 0x80) | DW_EH_PE_pcrel | DW_EH_PE_sdata4;
      else
        *ptr = (*ptr & 0x80) | DW_EH_PE_pcrel | DW_EH_PE_sdata8;
    }
    return sz;
  };

  // Now p points to the beginning of Augmentation Data
  for (; *aug; aug++) {
    switch (*aug) {
    case 'L':
    case 'R':
      rewrite(p);
      p++;
      break;
    case 'P':
      p += rewrite(p) + 1;
      break;
    case 'S':
    case 'B':
      break;
    default:
      Error(ctx) << cie.input_section
                 << ": unknown argumentation string in CIE: '"
                 << (char)*aug << "'";
    }
  }
}

#define INSTANTIATE(E)                                                       \
  template void write_plt_header(Context<E> &, u8 *);                        \
  template void write_plt_entry(Context<E> &, u8 *, Symbol<E> &);            \
  template void write_pltgot_entry(Context<E> &, u8 *, Symbol<E> &);         \
  template void EhFrameSection<E>::                                          \
    apply_eh_reloc(Context<E> &, const ElfRel<E> &, u64, u64);               \
  template void InputSection<E>::apply_reloc_alloc(Context<E> &, u8 *);      \
  template void InputSection<E>::apply_reloc_nonalloc(Context<E> &, u8 *);   \
  template void InputSection<E>::scan_relocations(Context<E> &);             \
  template class MipsGotSection<E>;                                          \
  template void mips_rewrite_cie(Context<E> &, u8 *, CieRecord<E> &);


INSTANTIATE(MIPS64LE);
INSTANTIATE(MIPS64BE);

} // namespace mold::elf
