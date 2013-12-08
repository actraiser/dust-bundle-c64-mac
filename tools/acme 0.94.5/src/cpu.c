// ACME - a crossassembler for producing 6502/65c02/65816 code.
// Copyright (C) 1998-2009 Marco Baye
// Have a look at "acme.c" for further info
//
// CPU stuff
#include "config.h"
#include "alu.h"
#include "cpu.h"
#include "dynabuf.h"
#include "global.h"
#include "input.h"
#include "mnemo.h"
#include "output.h"
#include "tree.h"


// constants
static struct cpu_t	CPU_6502 = {
	keyword_is_6502mnemo,
	CPUFLAG_INDIRECTJMPBUGGY,	// JMP ($xxFF) is buggy
	234,			// !align fills with "NOP"
	FALSE,			// short accu
	FALSE			// short xy
};
static struct cpu_t	CPU_6510 = {
	keyword_is_6510mnemo,
	CPUFLAG_INDIRECTJMPBUGGY,	// JMP ($xxFF) is buggy
	234,			// !align fills with "NOP"
	FALSE,			// short accu
	FALSE			// short xy
};
static struct cpu_t	CPU_65c02= {
	keyword_is_65c02mnemo,
	0,			// no flags
	234,			// !align fills with "NOP"
	FALSE,			// short accu
	FALSE			// short xy
};
/*
static struct cpu_t	CPU_Rockwell65c02 = {
	keyword_is_Rockwell65c02mnemo,
	0,			// no flags
	234,			// !align fills with "NOP"
	FALSE,			// short accu
	FALSE			// short xy
};
static struct cpu_t	CPU_WDC65c02	= {
	keyword_is_WDC65c02mnemo,
	0,			// no flags
	234,			// !align fills with "NOP"
	FALSE,			// short accu
	FALSE			// short xy
};
*/
static struct cpu_t	CPU_65816 = {
	keyword_is_65816mnemo,
	CPUFLAG_SUPPORTSLONGREGS,	// allows A and XY to be 16bits wide
	234,			// !align fills with "NOP"
	FALSE,			// short accu
	FALSE			// short xy
};
#define s_rl	(s_brl + 1)	// Yes, I know I'm sick


// variables
struct cpu_t		*CPU_now;	// struct of current CPU type (default 6502)
struct result_int_t	CPU_pc;		// (pseudo) program counter at start of statement
int			CPU_2add;	// increase PC by this after statement
static intval_t	current_offset;	// PseudoPC - MemIndex (FIXME - why is this needed?)
static int	uses_pseudo_pc;	// offset assembly active?	FIXME - what is this for?
// predefined stuff
static struct node_t	*CPU_tree	= NULL;	// tree to hold CPU types
static struct node_t	CPUs[]	= {
//	PREDEFNODE("z80",		&CPU_Z80),
	PREDEFNODE("6502",		&CPU_6502),
	PREDEFNODE("6510",		&CPU_6510),
	PREDEFNODE("65c02",		&CPU_65c02),
//	PREDEFNODE("Rockwell65c02",	&CPU_Rockwell65c02),
//	PREDEFNODE("WDC65c02",		&CPU_WDC65c02),
	PREDEFLAST(s_65816,		&CPU_65816),
	//    ^^^^ this marks the last element
};


// insert byte until PC fits condition
static enum eos_t PO_align(void) {
	intval_t	and,
			equal,
			fill,
			test	= CPU_pc.intval;

	// make sure PC is defined.
	if ((CPU_pc.flags & MVALUE_DEFINED) == 0) {
		Throw_error(exception_pc_undefined);
		CPU_pc.flags |= MVALUE_DEFINED;	// do not complain again
		return SKIP_REMAINDER;
	}

	and = ALU_defined_int();
	if (!Input_accept_comma())
		Throw_error(exception_syntax);
	equal = ALU_defined_int();
	if (Input_accept_comma())
		fill = ALU_any_int();
	else
		fill = CPU_now->default_align_value;
	while ((test++ & and) != equal)
		Output_8b(fill);
	return ENSURE_EOS;
}


// try to find CPU type held in DynaBuf. Returns whether succeeded.
int CPU_find_cpu_struct(struct cpu_t **target)
{
	void	*node_body;

	if (!Tree_easy_scan(CPU_tree, &node_body, GlobalDynaBuf))
		return 0;
	*target = node_body;
	return 1;
}


// select CPU ("!cpu" pseudo opcode)
static enum eos_t PO_cpu(void)
{
	struct cpu_t	*cpu_buffer	= CPU_now;	// remember current cpu

	if (Input_read_and_lower_keyword())
		if (!CPU_find_cpu_struct(&CPU_now))
			Throw_error("Unknown processor.");
	// if there's a block, parse that and then restore old value!
	if (Parse_optional_block())
		CPU_now = cpu_buffer;
	return ENSURE_EOS;
}


static const char	Warning_old_offset_assembly[]	=
	"\"!pseudopc/!realpc\" is deprecated; use \"!pseudopc {}\" instead.";


// start offset assembly
static enum eos_t PO_pseudopc(void)
{
// future algo: remember outer memaddress and outer pseudopc
	int		outer_state	= uses_pseudo_pc;
	intval_t	new_pc,
			outer_offset	= current_offset;
	int		outer_flags	= CPU_pc.flags;

	// set new
	new_pc = ALU_defined_int();	// FIXME - allow for undefined pseudopc!
	current_offset = (current_offset + new_pc - CPU_pc.intval) & 0xffff;
	CPU_pc.intval = new_pc;
	CPU_pc.flags |= MVALUE_DEFINED;	// FIXME - remove!
	uses_pseudo_pc = TRUE;
	// if there's a block, parse that and then restore old value!
	if (Parse_optional_block()) {
		// restore old
		uses_pseudo_pc = outer_state;
		CPU_pc.flags = outer_flags;
		CPU_pc.intval = (outer_offset + CPU_pc.intval - current_offset) & 0xffff;
		current_offset = outer_offset;
// future algo: new outer pseudopc = (old outer pseudopc + (current memaddress - outer memaddress)) & 0xffff
	} else {
		Throw_first_pass_warning(Warning_old_offset_assembly);
	}
	return ENSURE_EOS;
}


// end offset assembly
static enum eos_t PO_realpc(void)
{
	Throw_first_pass_warning(Warning_old_offset_assembly);
	// deactivate offset assembly
	CPU_pc.intval = (CPU_pc.intval - current_offset) & 0xffff;
	current_offset = 0;
	uses_pseudo_pc = FALSE;
	return ENSURE_EOS;
}


// return whether offset assembly is active (FIXME - remove this function)
int CPU_uses_pseudo_pc(void)
{
	return uses_pseudo_pc;
}


// if cpu type and value match, set register length variable to value.
// if cpu type and value don't match, complain instead.
static void check_and_set_reg_length(int *var, int make_long)
{
	if (((CPU_now->flags & CPUFLAG_SUPPORTSLONGREGS) == 0) && make_long)
		Throw_error("Chosen CPU does not support long registers.");
	else
		*var = make_long;
}


// set register length, block-wise if needed.
static enum eos_t set_register_length(int *var, int make_long)
{
	int	old_size	= *var;

	// set new register length (or complain - whichever is more fitting)
	check_and_set_reg_length(var, make_long);
	// if there's a block, parse that and then restore old value!
	if (Parse_optional_block())
		check_and_set_reg_length(var, old_size);	// restore old length
	return ENSURE_EOS;
}


// switch to long accu ("!al" pseudo opcode)
static enum eos_t PO_al(void)
{
	return set_register_length(&CPU_now->a_is_long, TRUE);
}


// switch to short accu ("!as" pseudo opcode)
static enum eos_t PO_as(void)
{
	return set_register_length(&CPU_now->a_is_long, FALSE);
}


// switch to long index registers ("!rl" pseudo opcode)
static enum eos_t PO_rl(void)
{
	return set_register_length(&CPU_now->xy_are_long, TRUE);
}


// switch to short index registers ("!rs" pseudo opcode)
static enum eos_t PO_rs(void)
{
	return set_register_length(&CPU_now->xy_are_long, FALSE);
}


// pseudo opcode table
static struct node_t	pseudo_opcodes[]	= {
	PREDEFNODE("align",	PO_align),
	PREDEFNODE("cpu",	PO_cpu),
	PREDEFNODE("pseudopc",	PO_pseudopc),
	PREDEFNODE("realpc",	PO_realpc),
	PREDEFNODE("al",	PO_al),
	PREDEFNODE("as",	PO_as),
	PREDEFNODE(s_rl,	PO_rl),
	PREDEFLAST("rs",	PO_rs),
	//    ^^^^ this marks the last element
};


// set default values for pass
void CPU_passinit(struct cpu_t *cpu_type)
{
	// handle cpu type (default is 6502)
	CPU_now		= cpu_type ? cpu_type : &CPU_6502;
	CPU_pc.flags	= 0;	// not defined yet
	CPU_pc.intval	= 512;	// actually, there should be no need to init
	CPU_2add	= 0;	// increase PC by this at end of statement
	CPU_65816.a_is_long = FALSE;	// short accu
	CPU_65816.xy_are_long = FALSE;	// short index regs
	uses_pseudo_pc	= FALSE;	// offset assembly is not active,
	current_offset	= 0;		// so offset is 0
}


// create cpu type tree (is done early)
void CPUtype_init(void)
{
	Tree_add_table(&CPU_tree, CPUs);
}


// register pseudo opcodes (done later)
void CPU_init(void)
{
	Tree_add_table(&pseudo_opcode_tree, pseudo_opcodes);
}


// set program counter to defined value
void CPU_set_pc(intval_t new_pc)
{
	CPU_pc.flags |= MVALUE_DEFINED;
	CPU_pc.intval = new_pc;
}
