// ACME - a crossassembler for producing 6502/65c02/65816 code.
// Copyright (C) 1998-2009 Marco Baye
// Have a look at "acme.c" for further info
//
// Label stuff
//
// 22 Nov 2007	"warn on indented labels" is now a CLI switch
// 25 Sep 2011	Fixed bug in !sl (colons in filename could be interpreted as EOS)
#include <stdio.h>
#include "acme.h"
#include "alu.h"
#include "cpu.h"
#include "dynabuf.h"
#include "global.h"
#include "input.h"
#include "label.h"
#include "platform.h"
#include "section.h"
#include "tree.h"


// constants
#define s_sl	(s_asl + 1)	// Yes, I know I'm sick


// variables
struct node_ra_t	*Label_forest[256];	// ... (because of 8-bit hash)


// Dump label value and flags to dump file
static void dump_one_label(struct node_ra_t *node, FILE *fd)
{
	struct label_t	*label	= node->body;

	// output name
	fprintf(fd, "%s", node->id_string);
	switch (label->result.flags & MVALUE_FORCEBITS) {
	case MVALUE_FORCE16:
		fprintf(fd, "+2=");
		break;
	case MVALUE_FORCE16 | MVALUE_FORCE24:
		/*FALLTHROUGH*/
	case MVALUE_FORCE24:
		fprintf(fd, "+3=");
		break;
	default:
		fprintf(fd, "  =");
	}
	if (label->result.flags & MVALUE_DEFINED) {
		if (label->result.flags & MVALUE_IS_FP)
			fprintf(fd, "%.30f", label->result.val.fpval);	//FIXME %g
		else
			fprintf(fd, "$%x", (unsigned) label->result.val.intval);
	} else {
		fprintf(fd, " ?");
	}
	if (label->result.flags & MVALUE_UNSURE)
		fprintf(fd, "; ?");
	if (label->usage == 0)
		fprintf(fd, "; unused");
	fprintf(fd, "\n");
}


// Search for label. Create if nonexistant. If created, give it flags "Flags".
// The label name must be held in GlobalDynaBuf.
struct label_t *Label_find(zone_t zone, int flags)
{
	struct node_ra_t	*node;
	struct label_t		*label;
	int			node_created,
				force_bits	= flags & MVALUE_FORCEBITS;

	node_created = Tree_hard_scan(&node, Label_forest, zone, TRUE);
	// if node has just been created, create label as well
	if (node_created) {
		// Create new label structure
		label = safe_malloc(sizeof(*label));
		// Finish empty label item
		label->result.flags = flags;
		if (flags & MVALUE_IS_FP)
			label->result.val.fpval = 0;
		else
			label->result.val.intval = 0;
		label->usage = 0;	// usage count
		label->pass = pass_count;
		node->body = label;
	} else {
		label = node->body;
	}
	// make sure the force bits don't clash
	if ((node_created == FALSE) && force_bits)
		if ((label->result.flags & MVALUE_FORCEBITS) != force_bits)
			Throw_error("Too late for postfix.");
	return label;
}


// Assign value to label. The function acts upon the label's flag bits and
// produces an error if needed.
void Label_set_value(struct label_t *label, struct result_t *newvalue, int change_allowed)
{
	int	oldflags	= label->result.flags;

	// value stuff
	if ((oldflags & MVALUE_DEFINED) && (change_allowed == FALSE)) {
		// Label is already defined, so compare new and old values
		// if different type OR same type but different value, complain
		if (((oldflags ^ newvalue->flags) & MVALUE_IS_FP)
		|| ((oldflags & MVALUE_IS_FP)
		? (label->result.val.fpval != newvalue->val.fpval)
		: (label->result.val.intval != newvalue->val.intval)))
			Throw_error("Label already defined.");
	} else {
		// Label is not defined yet OR redefinitions are allowed
		label->result = *newvalue;
	}
	// flags stuff
	// Ensure that "unsure" labels without "isByte" state don't get that
	if ((oldflags & (MVALUE_UNSURE | MVALUE_ISBYTE)) == MVALUE_UNSURE)
		newvalue->flags &= ~MVALUE_ISBYTE;
	if (change_allowed) {
		oldflags = (oldflags & MVALUE_UNSURE) | newvalue->flags;
	} else {
		if ((oldflags & MVALUE_FORCEBITS) == 0)
			if ((oldflags & (MVALUE_UNSURE | MVALUE_DEFINED)) == 0)
				oldflags |= newvalue->flags & MVALUE_FORCEBITS;
		oldflags |= newvalue->flags & ~MVALUE_FORCEBITS;
	}
	label->result.flags = oldflags;
}


// (Re)set label
static enum eos_t PO_set(void)	// Now GotByte = illegal char
{
	struct result_t	result;
	int		force_bit;
	struct label_t	*label;
	zone_t		zone;

	if (Input_read_zone_and_keyword(&zone) == 0)	// skips spaces before
		// Now GotByte = illegal char
		return SKIP_REMAINDER;

	force_bit = Input_get_force_bit();	// skips spaces after
	label = Label_find(zone, force_bit);
	if (GotByte != '=') {
		Throw_error(exception_syntax);
		return SKIP_REMAINDER;
	}

	// label = parsed value
	GetByte();	// proceed with next char
	ALU_any_result(&result);
	// clear label's force bits and set new ones
	label->result.flags &= ~(MVALUE_FORCEBITS | MVALUE_ISBYTE);
	if (force_bit) {
		label->result.flags |= force_bit;
		result.flags &= ~(MVALUE_FORCEBITS | MVALUE_ISBYTE);
	}
	Label_set_value(label, &result, TRUE);
	return ENSURE_EOS;
}


// Select dump file
static enum eos_t PO_sl(void)
{
	// bugfix: first read filename, *then* check for first pass.
	// if skipping right away, quoted colons might be misinterpreted as EOS
	// FIXME - why not just fix the skipping code to handle quotes? :)
	// "!to" has been fixed as well

	// read filename to global dynamic buffer
	// if no file name given, exit (complaining will have been done)
	if (Input_read_filename(FALSE))
		return SKIP_REMAINDER;

	// only process this pseudo opcode in first pass
	if (pass_count)
		return SKIP_REMAINDER;

	// if label dump file already chosen, complain and exit
	if (labeldump_filename) {
		Throw_warning("Label dump file already chosen.");
		return SKIP_REMAINDER;
	}

	// get malloc'd copy of filename
	labeldump_filename = DynaBuf_get_copy(GlobalDynaBuf);
	// ensure there's no garbage at end of line
	return ENSURE_EOS;
}


// predefined stuff
static struct node_t	pseudo_opcodes[]	= {
	PREDEFNODE("set",	PO_set),
	PREDEFLAST(s_sl,	PO_sl),
	//    ^^^^ this marks the last element
};


// Parse implicit label definition (can be either global or local).
// GlobalDynaBuf holds the label name.
void Label_implicit_definition(zone_t zone, int stat_flags, int force_bit, int change)
{
	struct result_t	result;
	struct label_t	*label;

	label = Label_find(zone, force_bit);
	// implicit label definition (label)
	if ((stat_flags & SF_FOUND_BLANK) && warn_on_indented_labels)
		Throw_first_pass_warning("Implicit label definition not in leftmost column.");
	result.flags = CPU_pc.flags & MVALUE_DEFINED;
	result.val.intval = CPU_pc.intval;
	Label_set_value(label, &result, change);
}


// parse label definition (can be either global or local).
// GlobalDynaBuf holds the label name.
void Label_parse_definition(zone_t zone, int stat_flags)
{
	struct result_t	result;
	struct label_t	*label;
	int		force_bit	= Input_get_force_bit();	// skips spaces after
	// FIXME - force bit is allowed for implicit label defs?!

	if (GotByte == '=') {
		// explicit label definition (label = <something>)
		label = Label_find(zone, force_bit);
		// label = parsed value
		GetByte();	// skip '='
		ALU_any_result(&result);
		Label_set_value(label, &result, FALSE);
		Input_ensure_EOS();
	} else {
		Label_implicit_definition(zone, stat_flags, force_bit, FALSE);
	}
}


// set global label to value, no questions asked (for "-D" switch)
// Name must be held in GlobalDynaBuf.
void Label_define(intval_t value)
{
	struct result_t	result;
	struct label_t	*label;

	result.flags = MVALUE_GIVEN;
	result.val.intval = value;
	label = Label_find(ZONE_GLOBAL, 0);
	Label_set_value(label, &result, TRUE);
}


// dump global labels to file
void Label_dump_all(FILE *fd)
{
	Tree_dump_forest(Label_forest, ZONE_GLOBAL, dump_one_label, fd);
	PLATFORM_SETFILETYPE_TEXT(labeldump_filename);
}


// clear label forest (is done early)
void Label_clear_init(void)
{
	struct node_ra_t	**ptr;
	int			i;

	// cut down all the trees (clear pointer table)
	ptr = Label_forest;
	for (i = 255; i >= 0; i--)
		*ptr++ = NULL;
}


// register pseudo opcodes (done later)
void Label_register_init(void)
{
	Tree_add_table(&pseudo_opcode_tree, pseudo_opcodes);
}


// fix name of anonymous forward label (held in DynaBuf, NOT TERMINATED!) so it
// references the *next* anonymous forward label definition. The tricky bit is,
// each name length would need its own counter. But hey, ACME's real quick in
// finding labels, so I'll just abuse the label system to store those counters.
struct label_t *Label_fix_forward_name(void)
{
	struct label_t	*counter_label;
	unsigned long	number;

	// terminate name, find "counter" label and read value
	DynaBuf_append(GlobalDynaBuf, '\0');
	counter_label = Label_find(Section_now->zone, 0);
	// make sure it gets reset to zero in each new pass
	if (counter_label->pass != pass_count) {
		counter_label->pass = pass_count;
		counter_label->result.val.intval = 0;
	}
	number = (unsigned long) counter_label->result.val.intval;
	// now append to the name to make it unique
	GlobalDynaBuf->size--;	// forget terminator, we want to append
	do {
		DYNABUF_APPEND(GlobalDynaBuf, 'a' + (number & 15));
		number >>= 4;
	} while (number);
	DynaBuf_append(GlobalDynaBuf, '\0');
	return counter_label;
}
