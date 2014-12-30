/*-
 * Copyright (c) 2014 Mark Johnston <markj@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/sdt.h>
#include <sys/queue.h>

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gelf.h>
#include <libelf.h>

#define	ELF_ERR()	(elf_errmsg(elf_errno()))
#define	LOG(...) do {					\
	if (verbose)					\
		warnx(__VA_ARGS__);			\
} while (0)

#define	AMD64_CALL	0xe8
#define	AMD64_JMP32	0xe9
#define	AMD64_NOP	0x90
#define	AMD64_RETQ	0xc3

static const char probe_prefix[] = "__dtrace_probe_";
static size_t prefixlen = sizeof(probe_prefix) - 1;

static bool verbose = false;

struct probe_instance {
	const char	*symname;
	uint64_t	offset;
	SLIST_ENTRY(probe_instance) next;
};

SLIST_HEAD(probe_list, probe_instance);

static Elf_Scn *add_section(Elf *, const char *, uint64_t, uint64_t);
static const char *get_scn_name(Elf *, Elf_Scn *);
static int	process_rel(Elf *, GElf_Ehdr *, GElf_Shdr *, Elf_Scn *,
		    uint8_t *, GElf_Addr, GElf_Xword *, struct probe_list *);
static void	process_reloc_scn(Elf *, GElf_Ehdr *, GElf_Shdr *, Elf_Scn *,
		    struct probe_list *);
static void	process_obj(const char *);
static void	record_instance(Elf *, GElf_Ehdr *, Elf_Scn *, Elf_Scn *,
		    struct probe_instance *);
static Elf_Scn *section_by_name(Elf *, const char *);
static GElf_Sym	*symlookup(Elf_Scn *, int);
static void	usage(void);
static void *	xmalloc(size_t);

static Elf_Scn *
add_section(Elf *e, const char *name, uint64_t type, uint64_t flags)
{
	GElf_Shdr newshdr, strshdr;
	Elf_Data *strdata, *newstrdata;
	Elf_Scn *newscn, *strscn;
	size_t len, shdrstrndx;

	/* First add the section name to the section header string table. */
	if (elf_getshdrstrndx(e, &shdrstrndx) != 0)
		errx(1, "elf_getshdrstrndx: %s", ELF_ERR());
	if ((strscn = elf_getscn(e, shdrstrndx)) == NULL)
		errx(1, "elf_getscn on shdrstrtab: %s", ELF_ERR());
	if (gelf_getshdr(strscn, &strshdr) != &strshdr)
		errx(1, "gelf_getshdr on shdrstrtab: %s", ELF_ERR());
	if ((strdata = elf_getdata(strscn, NULL)) == NULL)
		errx(1, "elf_getdata on shdrstrtab: %s", ELF_ERR());
	if ((newstrdata = elf_newdata(strscn)) == NULL)
		errx(1, "elf_newdata on shdrstrtab: %s", ELF_ERR());

	len = strlen(name) + 1;
	newstrdata->d_align = strdata->d_align;
	newstrdata->d_buf = xmalloc(len);
	memcpy(newstrdata->d_buf, name, len);
	newstrdata->d_size = len;
	newstrdata->d_type = strdata->d_type;
	newstrdata->d_version = elf_version(EV_CURRENT);

	strshdr.sh_size += len;

	/* Then create the actual section. */
	if ((newscn = elf_newscn(e)) == NULL)
		errx(1, "elf_newscn: %s", ELF_ERR());
	if (gelf_getshdr(newscn, &newshdr) != &newshdr)
		errx(1, "gelf_getshdr: %s", ELF_ERR());

	newshdr.sh_name = strshdr.sh_size - len;
	newshdr.sh_type = type;
	newshdr.sh_flags = flags;
	newshdr.sh_addralign = 8;

	if (gelf_update_shdr(newscn, &newshdr) == 0)
		errx(1, "gelf_update_shdr: %s", ELF_ERR());
	if (gelf_update_shdr(strscn, &strshdr) == 0)
		errx(1, "gelf_update_shdr: %s", ELF_ERR());

	LOG("added section %s", name);

	return (newscn);
}

static const char *
get_scn_name(Elf *e, Elf_Scn *scn)
{
	GElf_Shdr shdr;
	size_t ndx;

	if (gelf_getshdr(scn, &shdr) != &shdr)
		errx(1, "gelf_getshdr: %s", ELF_ERR());
	if (elf_getshdrstrndx(e, &ndx) != 0)
		errx(1, "elf_getshdrstrndx: %s", ELF_ERR());
	return (elf_strptr(e, ndx, shdr.sh_name));
}

/* XXX need current obj filename for better error messages. */
static int
process_rel(Elf *e, GElf_Ehdr *ehdr, GElf_Shdr *symshdr, Elf_Scn *symtab,
    uint8_t *target, GElf_Addr offset, GElf_Xword *info,
    struct probe_list *plist)
{
	struct probe_instance *inst;
	GElf_Sym *sym;
	char *symname;
	uint8_t opc;

	sym = symlookup(symtab, GELF_R_SYM(*info));
	symname = elf_strptr(e, symshdr->sh_link, sym->st_name);
	if (symname == NULL)
		errx(1, "elf_strptr: %s", ELF_ERR());

	if (strncmp(symname, probe_prefix, prefixlen) != 0)
		/* We're not interested in this relocation. */
		return (1);

	/* Sanity checks. */
	if (GELF_ST_TYPE(sym->st_info) != STT_NOTYPE)
		errx(1, "unexpected symbol type %d for %s",
		    GELF_ST_TYPE(sym->st_info), symname);
	if (GELF_ST_BIND(sym->st_info) != STB_GLOBAL)
		errx(1, "unexpected binding %d for %s",
		    GELF_ST_BIND(sym->st_info), symname);

	switch (ehdr->e_machine) {
	case EM_X86_64:
		/* Sanity checks. */
		opc = target[offset - 1];
		if (opc != AMD64_CALL && opc != AMD64_JMP32)
			errx(1, "unexpected opcode 0x%x for %s at offset 0x%lx",
			    opc, symname, offset);
		if (target[offset + 0] != 0 ||
		    target[offset + 1] != 0 ||
		    target[offset + 2] != 0 ||
		    target[offset + 3] != 0)
			errx(1, "unexpected addr for %s at offset 0x%lx",
			    symname, offset);

		/* Overwrite the call with NOPs. */
		memset(&target[offset - 1], AMD64_NOP, 5);

		/* If this was a tail call, we need to return instead. */
		if (opc == AMD64_JMP32)
			target[offset - 1] = AMD64_RETQ;

		/*
		 * Set the relocation type to R_X86_64_NONE so that the linker
		 * ignores it.
		 */
		*info &= ~GELF_R_TYPE(*info);
		*info |= R_X86_64_NONE;
		break;
	default:
		errx(1, "unhandled machine type 0x%x", ehdr->e_machine);
	}

	LOG("updated relocation for %s at 0x%lx", symname, offset - 1);

	inst = xmalloc(sizeof(*inst));
	inst->symname = symname;
	inst->offset = offset;
	SLIST_INSERT_HEAD(plist, inst, next);

	return (0);
}

static void
process_reloc_scn(Elf *e, GElf_Ehdr *ehdr, GElf_Shdr *shdr, Elf_Scn *scn,
    struct probe_list *plist)
{
	GElf_Shdr symshdr;
	GElf_Rel rel;
	GElf_Rela rela;
	Elf_Data *reldata, *targdata;
	Elf_Scn *symscn, *targscn;
	const char *name;
	u_int i;
	int ret;

	if ((targscn = elf_getscn(e, shdr->sh_info)) == NULL)
		errx(1, "failed to look up relocation section: %s", ELF_ERR());
	if ((targdata = elf_getdata(targscn, NULL)) == NULL)
		errx(1, "failed to look up target section data: %s", ELF_ERR());

	/* We only want to process text relocations. */
	name = get_scn_name(e, targscn);
	if (strcmp(name, ".text") != 0) {
		LOG("skipping relocation section for %s", name);
		return;
	}

	if ((symscn = elf_getscn(e, shdr->sh_link)) == NULL)
		errx(1, "failed to look up symbol table: %s", ELF_ERR());
	if (gelf_getshdr(symscn, &symshdr) == NULL)
		errx(1, "failed to look up symbol table header: %s", ELF_ERR());

	i = 0;
	for (reldata = NULL; (reldata = elf_getdata(scn, reldata)) != NULL; ) {
		for (; i < shdr->sh_size / shdr->sh_entsize; i++) {
			if (shdr->sh_type == SHT_REL) {
				if (gelf_getrel(reldata, i, &rel) == NULL)
					errx(1, "gelf_getrel: %s", ELF_ERR());
				ret = process_rel(e, ehdr, &symshdr, symscn,
				    targdata->d_buf, rel.r_offset, &rel.r_info,
				    plist);
				if (ret == 0 &&
				    gelf_update_rel(reldata, i, &rel) == 0)
					errx(1, "gelf_update_rel: %s",
					    ELF_ERR());
			} else {
				assert(shdr->sh_type == SHT_RELA);
				if (gelf_getrela(reldata, i, &rela) == NULL)
					errx(1, "gelf_getrela: %s", ELF_ERR());
				ret = process_rel(e, ehdr, &symshdr, symscn,
				    targdata->d_buf, rela.r_offset,
				    &rela.r_info, plist);
				if (ret == 0 &&
				    gelf_update_rela(reldata, i, &rela) == 0)
					errx(1, "gelf_update_rela: %s",
					    ELF_ERR());
			}

			/*
			 * We've updated the relocation and the corresponding
			 * text section.
			 */
			if (ret == 0) {
				if (elf_flagdata(targdata, ELF_C_SET,
				    ELF_F_DIRTY) == 0)
					errx(1, "elf_flagdata: %s", ELF_ERR());
				if (elf_flagdata(reldata, ELF_C_SET,
				    ELF_F_DIRTY) == 0)
					errx(1, "elf_flagdata: %s", ELF_ERR());
			}
		}
	}
}

static void
process_obj(const char *obj)
{
	struct probe_list plist;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;
	struct probe_instance *inst, *tmp;
	Elf_Scn *scn, *instscn, *instrelscn;
	Elf *e;
	int fd;

	if ((fd = open(obj, O_RDWR)) < 0)
		err(1, "failed to open %s", obj);

	if ((e = elf_begin(fd, ELF_C_RDWR, NULL)) == NULL)
		errx(1, "elf_begin: %s", ELF_ERR());

	if (gelf_getehdr(e, &ehdr) == NULL)
		errx(1, "gelf_getehdr: %s", ELF_ERR());
	if (ehdr.e_type != ET_REL) {
		warnx("invalid ELF type for '%s'", obj);
		return;
	}

	SLIST_INIT(&plist);

	/* Perform relocations for DTrace probe stub calls. */
	for (scn = NULL; (scn = elf_nextscn(e, scn)) != NULL; ) {
		if (gelf_getshdr(scn, &shdr) == NULL)
			errx(1, "gelf_getshdr: %s", ELF_ERR());

		if (shdr.sh_type == SHT_REL || shdr.sh_type == SHT_RELA)
			process_reloc_scn(e, &ehdr, &shdr, scn, &plist);
	}

	if (SLIST_EMPTY(&plist)) {
		/* No probe instances in this object file, we're done. */
		LOG("no probes found in %s", obj);
		return;
	}

	/* Now record all of the instance sites. */
	instscn = add_section(e, "set_sdt_instance_set", SHT_PROGBITS,
	    SHF_ALLOC);
	instrelscn = add_section(e, ".relaset_sdt_instance_set", SHT_RELA, 0);

	SLIST_FOREACH_SAFE(inst, &plist, next, tmp) {
		record_instance(e, &ehdr, instscn, instrelscn, inst);
		SLIST_REMOVE(&plist, inst, probe_instance, next);
		free(inst);
	}

	if (elf_update(e, ELF_C_WRITE) == -1)
		errx(1, "elf_update: %s", ELF_ERR());

	(void)elf_end(e);
	(void)close(fd);
}

static void
record_instance(Elf *e __unused, GElf_Ehdr *ehdr __unused, Elf_Scn *instscn,
    Elf_Scn *relscn __unused, struct probe_instance *inst)
{
	GElf_Rel rel;
	GElf_Rela rela;
	GElf_Shdr instshdr, proberelshdr, symshdr;
	struct sdt_instance sdtinst;
	Elf_Data *instdata, *probereldata;
	Elf_Scn *probescn, *proberelscn, *symtab;
	GElf_Sym *sym;
	GElf_Addr offset;
	GElf_Xword info;
	const char *name;
	size_t sz;
	u_int i;
	bool found;

	if ((instdata = elf_newdata(instscn)) == NULL)
		errx(1, "elf_newdata: %s", ELF_ERR());

	sz = sizeof(sdtinst);
	sdtinst.probe = NULL; /* Filled in by the linker. */
	sdtinst.offset = inst->offset;

	instdata->d_align = 1;
	instdata->d_buf = xmalloc(sz);
	memcpy(instdata->d_buf, &sdtinst, sz);
	instdata->d_size = sz;
	instdata->d_type = ELF_T_BYTE;
	instdata->d_version = elf_version(EV_CURRENT);

	if (gelf_getshdr(instscn, &instshdr) != &instshdr)
		errx(1, "gelf_getshdr: %s", ELF_ERR());

	instshdr.sh_size += sz;

	/* XXX lift out of this function */
	if ((probescn = section_by_name(e, "set_sdt_probes_set")) == NULL)
		errx(1, "couldn't find SDT probe linker set");

	/* Find the relocation section for the SDT probe linker set. */
	for (proberelscn = NULL;
	    (proberelscn = elf_nextscn(e, proberelscn)) != NULL; ) {
		if (gelf_getshdr(proberelscn, &proberelshdr) == NULL)
			errx(1, "gelf_getshdr: %s", ELF_ERR());

		if ((proberelshdr.sh_type == SHT_REL ||
		    proberelshdr.sh_type == SHT_RELA) &&
		    proberelshdr.sh_info == elf_ndxscn(probescn)) {
			symtab = elf_getscn(e, proberelshdr.sh_link);
			if (symtab == NULL)
				errx(1, "couldn't find symtab: %s", ELF_ERR());
			if (gelf_getshdr(symtab, &symshdr) != &symshdr)
				errx(1, "gelf_getshdr: %s", ELF_ERR());
			break;
		}
	}
	if (proberelscn == NULL)
		errx(1, "couldn't find reloc section for SDT probe linker set");
	found = false;
	i = 0;
	for (probereldata = NULL;
	    (probereldata = elf_getdata(proberelscn, probereldata)) != NULL; ) {
		for (; i < proberelshdr.sh_size / proberelshdr.sh_entsize; i++) {
			if (proberelshdr.sh_type == SHT_REL) {
				if (gelf_getrel(probereldata, i, &rel) == NULL)
					errx(1, "gelf_getrel: %s", ELF_ERR());
				info = rel.r_info;
				offset = rel.r_offset;
			} else {
				assert(proberelshdr.sh_type == SHT_RELA);
				if (gelf_getrela(probereldata, i, &rela) == NULL)
					errx(1, "gelf_getrela: %s", ELF_ERR());
				info = rela.r_info;
				offset = rela.r_offset;
			}

			sym = symlookup(symtab, GELF_R_SYM(info));
			name = elf_strptr(e, symshdr.sh_link, sym->st_name);

			if (strlen(name) < prefixlen)
				continue;
			if (strcmp(name + strlen("sdt_"),
			    inst->symname + prefixlen) == 0) {
				found = true;
				LOG("found relocation at 0x%lx for %s", offset,
				    inst->symname);
				break; /* There's my chippy. */
			}
		}
	}

	if (!found)
		errx(1, "failed to find SDT probe relocation for %s",
		    inst->symname);

	/*
	 * Now scan the SDT probe relocations for the symbol matching our probe
	 * instance. A matching relocation tells us which SDT probe this
	 * instance is associated with. This is a bit cumbersome...
	 */

	if (gelf_update_shdr(instscn, &instshdr) == 0)
		errx(1, "gelf_update_shdr: %s", ELF_ERR());
}

/* Look up an ELF section by name. */
static Elf_Scn *
section_by_name(Elf *e, const char *name)
{
	Elf_Scn *scn;

	for (scn = NULL; (scn = elf_nextscn(e, scn)) != NULL; )
		if (strcmp(get_scn_name(e, scn), name) == 0)
			return (scn);
	return (NULL);
}

/* Retrieve the specified symbol, with bounds checking. */
static GElf_Sym *
symlookup(Elf_Scn *symtab, int ndx)
{
	Elf_Data *symdata;

	if ((symdata = elf_getdata(symtab, NULL)) == NULL)
		errx(1, "couldn't find symbol table data: %s", ELF_ERR());
	if (symdata->d_size < (ndx + 1) * sizeof(GElf_Sym))
		errx(1, "invalid symbol index %d", ndx);
	return (&((GElf_Sym *)symdata->d_buf)[ndx]);
}

static void
usage(void)
{

	fprintf(stderr, "%s: [-v] <obj> [<obj> ...]\n", getprogname());
	exit(1);
}

static void *
xmalloc(size_t n)
{
	void *ret;

	ret = malloc(n);
	if (ret == NULL)
		errx(1, "malloc");
	return (ret);
}

int
main(int argc, char **argv)
{

	if (argc > 1 && strcmp(argv[1], "-v") == 0) {
		verbose = true;
		argv++; argc--;
	}

	if (argc <= 1)
		usage();

	if (elf_version(EV_CURRENT) == EV_NONE)
		errx(1, "ELF library too old");

	for (int i = 1; i < argc; i++)
		process_obj(argv[i]);

	return (0);
}
