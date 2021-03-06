/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   The codegenerator (transform FIRM into arm FIRM)
 * @author  Matthias Braun, Oliver Richter, Tobias Gneist, Michael Beck
 */
#include "bearch_arm_t.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "irgmod.h"
#include "iredges.h"
#include "ircons.h"
#include "dbginfo.h"
#include "iropt_t.h"
#include "debug.h"
#include "panic.h"
#include "util.h"

#include "benode.h"
#include "beirg.h"
#include "beutil.h"
#include "betranshlp.h"

#include "arm_nodes_attr.h"
#include "arm_transform.h"
#include "arm_optimize.h"
#include "arm_new_nodes.h"
#include "arm_cconv.h"

#include "gen_arm_regalloc_if.h"
#include "gen_arm_new_nodes.h"

#define ARM_PO2_STACK_ALIGNMENT 3

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static const arch_register_t *sp_reg = &arm_registers[REG_SP];
static ir_mode               *mode_fp;
static be_stackorder_t       *stackorder;
static calling_convention_t  *cconv = NULL;
static be_start_info_t        start_mem;
static be_start_info_t        start_val[N_ARM_REGISTERS];
static unsigned               start_callee_saves_offset;
static pmap                  *node_to_stack;

static const arch_register_t *const callee_saves[] = {
	&arm_registers[REG_R4],
	&arm_registers[REG_R5],
	&arm_registers[REG_R6],
	&arm_registers[REG_R7],
	&arm_registers[REG_R8],
	&arm_registers[REG_R9],
	&arm_registers[REG_R10],
	&arm_registers[REG_R11],
	&arm_registers[REG_LR],
};

static const arch_register_t *const caller_saves[] = {
	&arm_registers[REG_R0],
	&arm_registers[REG_R1],
	&arm_registers[REG_R2],
	&arm_registers[REG_R3],
	&arm_registers[REG_LR],

	&arm_registers[REG_F0],
	&arm_registers[REG_F1],
	&arm_registers[REG_F2],
	&arm_registers[REG_F3],
	&arm_registers[REG_F4],
	&arm_registers[REG_F5],
	&arm_registers[REG_F6],
	&arm_registers[REG_F7],
};

void arm_gen_vals_from_word(uint32_t value, arm_vals *result)
{
	/* TODO: not optimal yet, as we only "shift" the value and don't take
	 * advantage of rotations */

	/* special case: we prefer shift amount 0 */
	if (value <= 0xFF) {
		result->values[0] = value;
		result->rors[0]   = 0;
		result->ops       = 1;
		return;
	}

	int initial = 0;
	result->ops = 0;
	do {
		while ((value & 0x3) == 0) {
			value  >>= 2;
			initial += 2;
		}

		result->values[result->ops] = value & 0xFF;
		result->rors[result->ops]   = (32-initial) % 32;
		++result->ops;

		value  >>= 8;
		initial += 8;
	} while (value != 0);
}

/**
 * create firm graph for a constant
 */
static ir_node *create_const_graph_value(dbg_info *dbgi, ir_node *block,
                                         uint32_t value)
{
	/* We only have 8 bit immediates. So we possibly have to combine several
	 * operations to construct the desired value.
	 *
	 * we can either create the value by adding bits to 0 or by removing bits
	 * from an register with all bits set. Try which alternative needs fewer
	 * operations */
	arm_vals v;
	arm_gen_vals_from_word(value, &v);
	arm_vals vn;
	arm_gen_vals_from_word(~value, &vn);

	ir_node *result;
	if (vn.ops < v.ops) {
		/* remove bits */
		result = new_bd_arm_Mvn_imm(dbgi, block, vn.values[0], vn.rors[0]);

		for (unsigned cnt = 1; cnt < vn.ops; ++cnt) {
			result = new_bd_arm_Bic_imm(dbgi, block, result,
			                            vn.values[cnt], vn.rors[cnt]);
		}
	} else {
		/* add bits */
		result = new_bd_arm_Mov_imm(dbgi, block, v.values[0], v.rors[0]);

		for (unsigned cnt = 1; cnt < v.ops; ++cnt) {
			result = new_bd_arm_Or_imm(dbgi, block, result,
			                           v.values[cnt], v.rors[cnt]);
		}
	}
	return result;
}

/**
 * Create a DAG constructing a given Const.
 *
 * @param irn  a Firm const
 */
static ir_node *create_const_graph(ir_node *irn, ir_node *block)
{
	ir_tarval *tv   = get_Const_tarval(irn);
	ir_mode   *mode = get_tarval_mode(tv);
	if (mode_is_reference(mode)) {
		/* ARM is 32bit, so we can safely convert a reference tarval into Iu */
		assert(get_mode_size_bits(mode) == get_mode_size_bits(arm_mode_gp));
		tv = tarval_convert_to(tv, arm_mode_gp);
	}
	long value = get_tarval_long(tv);
	return create_const_graph_value(get_irn_dbg_info(irn), block, value);
}

/**
 * Create an And that will zero out upper bits.
 *
 * @param dbgi     debug info
 * @param block    the basic block
 * @param op       the original node
 * param src_bits  number of lower bits that will remain
 */
static ir_node *gen_zero_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                                   unsigned src_bits)
{
	if (src_bits == 8) {
		return new_bd_arm_And_imm(dbgi, block, op, 0xFF, 0);
	} else if (src_bits == 16) {
		ir_node *lshift = new_bd_arm_Mov_reg_shift_imm(dbgi, block, op, ARM_SHF_LSL_IMM, 16);
		ir_node *rshift = new_bd_arm_Mov_reg_shift_imm(dbgi, block, lshift, ARM_SHF_LSR_IMM, 16);
		return rshift;
	} else {
		panic("zero extension only supported for 8 and 16 bits");
	}
}

/**
 * Generate code for a sign extension.
 */
static ir_node *gen_sign_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                                   unsigned src_bits)
{
	unsigned shift_width = 32 - src_bits;
	ir_node *lshift_node = new_bd_arm_Mov_reg_shift_imm(dbgi, block, op, ARM_SHF_LSL_IMM, shift_width);
	ir_node *rshift_node = new_bd_arm_Mov_reg_shift_imm(dbgi, block, lshift_node, ARM_SHF_ASR_IMM, shift_width);
	return rshift_node;
}

static ir_node *gen_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                              ir_mode *orig_mode)
{
	unsigned bits = get_mode_size_bits(orig_mode);
	if (bits == 32)
		return op;

	if (mode_is_signed(orig_mode)) {
		return gen_sign_extension(dbgi, block, op, bits);
	} else {
		return gen_zero_extension(dbgi, block, op, bits);
	}
}

/**
 * returns true if it is assured, that the upper bits of a node are "clean"
 * which means for a 16 or 8 bit value, that the upper bits in the register
 * are 0 for unsigned and a copy of the last significant bit for signed
 * numbers.
 */
static bool upper_bits_clean(ir_node *transformed_node, ir_mode *mode)
{
	(void)transformed_node;
	(void)mode;
	/* TODO */
	return false;
}

/**
 * Transforms a Conv node.
 *
 * @return The created ia32 Conv node
 */
static ir_node *gen_Conv(ir_node *node)
{
	ir_node  *block    = be_transform_nodes_block(node);
	ir_node  *op       = get_Conv_op(node);
	ir_node  *new_op   = be_transform_node(op);
	ir_mode  *src_mode = get_irn_mode(op);
	ir_mode  *dst_mode = get_irn_mode(node);
	dbg_info *dbg      = get_irn_dbg_info(node);

	if (src_mode == dst_mode)
		return new_op;

	if (mode_is_float(src_mode) || mode_is_float(dst_mode)) {
		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			if (mode_is_float(src_mode)) {
				if (mode_is_float(dst_mode)) {
					/* from float to float */
					return new_bd_arm_Mvf(dbg, block, new_op, dst_mode);
				} else {
					/* from float to int */
					panic("TODO");
				}
			} else {
				/* from int to float */
				if (!mode_is_signed(src_mode)) {
					panic("TODO");
				} else {
					return new_bd_arm_FltX(dbg, block, new_op, dst_mode);
				}
			}
		} else {
			panic("softfloat not lowered");
		}
	} else { /* complete in gp registers */
		unsigned src_bits = get_mode_size_bits(src_mode);
		unsigned dst_bits = get_mode_size_bits(dst_mode);
		if (src_bits == dst_bits) {
			/* kill unnecessary conv */
			return new_op;
		}

		unsigned min_bits;
		ir_mode *min_mode;
		if (src_bits < dst_bits) {
			min_bits = src_bits;
			min_mode = src_mode;
		} else {
			min_bits = dst_bits;
			min_mode = dst_mode;
		}

		if (upper_bits_clean(new_op, min_mode)) {
			return new_op;
		}

		if (mode_is_signed(min_mode)) {
			return gen_sign_extension(dbg, block, new_op, min_bits);
		} else {
			return gen_zero_extension(dbg, block, new_op, min_bits);
		}
	}
}

typedef struct {
	uint8_t imm_8;
	uint8_t rot;
} arm_immediate_t;

static bool try_encode_val_as_immediate(uint32_t val, arm_immediate_t *const res)
{
	if (val <= 0xff) {
		res->imm_8 = val;
		res->rot   = 0;
		return true;
	}
	/* arm allows to use to rotate an 8bit immediate value by a multiple of 2
	   (= 0, 2, 4, 6, ...).
	   So we determine the smallest even position with a bit set
	   and the highest even position with no bit set anymore.
	   If the difference between these 2 is <= 8, then we can encode the value
	   as immediate.
	 */
	unsigned low_pos  = ntz(val) & ~1u;
	unsigned high_pos = (32-nlz(val)+1) & ~1u;

	if (high_pos - low_pos <= 8) {
		res->imm_8 = val >> low_pos;
		res->rot   = 32 - low_pos;
		return true;
	}

	if (high_pos > 24) {
		res->rot = 34 - high_pos;
		val      = val >> (32-res->rot) | val << (res->rot);
		if (val <= 0xff) {
			res->imm_8 = val;
			return true;
		}
	}

	return false;
}

static bool try_encode_as_immediate(ir_node const *const node, arm_immediate_t *const res)
{
	if (!is_Const(node))
		return false;
	uint32_t const val = get_Const_long(node);
	return try_encode_val_as_immediate(val, res);
}

static bool try_encode_as_not_immediate(ir_node const *const node, arm_immediate_t *const res)
{
	if (!is_Const(node))
		return false;
	uint32_t const val = get_Const_long(node);
	return try_encode_val_as_immediate(~val, res);
}

typedef enum {
	MATCH_NONE         = 0,
	MATCH_COMMUTATIVE  = 1 << 0,  /**< commutative node */
	MATCH_REVERSE      = 1 << 1,  /**< support reverse opcode */
	MATCH_SIZE_NEUTRAL = 1 << 2,
} match_flags_t;
ENUM_BITSET(match_flags_t)

/**
 * possible binop constructors.
 */
typedef struct arm_binop_factory_t {
	/** normal reg op reg operation. */
	ir_node *(*new_binop_reg)(dbg_info *dbgi, ir_node *block, ir_node *op1, ir_node *op2);
	/** normal reg op imm operation. */
	ir_node *(*new_binop_imm)(dbg_info *dbgi, ir_node *block, ir_node *op1, unsigned char imm8, unsigned char imm_rot);
	/** barrel shifter reg op (reg shift reg operation. */
	ir_node *(*new_binop_reg_shift_reg)(dbg_info *dbgi, ir_node *block, ir_node *left, ir_node *right, ir_node *shift, arm_shift_modifier_t shift_modifier);
	/** barrel shifter reg op (reg shift imm operation. */
	ir_node *(*new_binop_reg_shift_imm)(dbg_info *dbgi, ir_node *block, ir_node *left, ir_node *right, arm_shift_modifier_t shift_modifier, unsigned shift_immediate);
} arm_binop_factory_t;

static ir_node *gen_int_binop_ops(ir_node *node, ir_node *op1, ir_node *op2,
                                  match_flags_t flags,
                                  const arm_binop_factory_t *factory)
{
	ir_node  *block = be_transform_nodes_block(node);
	dbg_info *dbgi  = get_irn_dbg_info(node);

	if (flags & MATCH_SIZE_NEUTRAL) {
		op1 = be_skip_downconv(op1, true);
		op2 = be_skip_downconv(op2, true);
	} else {
		assert(get_mode_size_bits(get_irn_mode(node)) == 32);
		op1 = be_skip_sameconv(op1);
		op2 = be_skip_sameconv(op2);
	}

	arm_immediate_t imm;
	if (try_encode_as_immediate(op2, &imm)) {
		ir_node *new_op1 = be_transform_node(op1);
		return factory->new_binop_imm(dbgi, block, new_op1, imm.imm_8, imm.rot);
	}
	ir_node *new_op2 = be_transform_node(op2);
    if ((flags & (MATCH_COMMUTATIVE|MATCH_REVERSE)) && try_encode_as_immediate(op1, &imm)) {
		if (flags & MATCH_REVERSE)
			return factory[1].new_binop_imm(dbgi, block, new_op2, imm.imm_8, imm.rot);
		else
			return factory[0].new_binop_imm(dbgi, block, new_op2, imm.imm_8, imm.rot);
	}
	ir_node *new_op1 = be_transform_node(op1);

	/* check if we can fold in a Mov */
	if (is_arm_Mov(new_op2)) {
		const arm_shifter_operand_t *attr = get_arm_shifter_operand_attr_const(new_op2);

		switch (attr->shift_modifier) {
		case ARM_SHF_IMM:
		case ARM_SHF_ASR_IMM:
		case ARM_SHF_LSL_IMM:
		case ARM_SHF_LSR_IMM:
		case ARM_SHF_ROR_IMM:
			if (factory->new_binop_reg_shift_imm) {
				ir_node *mov_op = get_irn_n(new_op2, n_arm_Mov_Rm);
				return factory->new_binop_reg_shift_imm(dbgi, block, new_op1, mov_op,
					attr->shift_modifier, attr->shift_immediate);
			}
			break;

		case ARM_SHF_ASR_REG:
		case ARM_SHF_LSL_REG:
		case ARM_SHF_LSR_REG:
		case ARM_SHF_ROR_REG:
			if (factory->new_binop_reg_shift_reg) {
				ir_node *mov_op  = get_irn_n(new_op2, n_arm_Mov_Rm);
				ir_node *mov_sft = get_irn_n(new_op2, n_arm_Mov_Rs);
				return factory->new_binop_reg_shift_reg(dbgi, block, new_op1, mov_op, mov_sft,
					attr->shift_modifier);
			}
			break;
		case ARM_SHF_REG:
		case ARM_SHF_RRX:
			break;
		case ARM_SHF_INVALID:
			panic("invalid shift");
		}
	}
	if ((flags & (MATCH_COMMUTATIVE|MATCH_REVERSE)) && is_arm_Mov(new_op1)) {
		const arm_shifter_operand_t *attr = get_arm_shifter_operand_attr_const(new_op1);
		int idx = flags & MATCH_REVERSE ? 1 : 0;

		switch (attr->shift_modifier) {
		case ARM_SHF_IMM:
		case ARM_SHF_ASR_IMM:
		case ARM_SHF_LSL_IMM:
		case ARM_SHF_LSR_IMM:
		case ARM_SHF_ROR_IMM:
			if (factory[idx].new_binop_reg_shift_imm) {
				ir_node *mov_op = get_irn_n(new_op1, n_arm_Mov_Rm);
				return factory[idx].new_binop_reg_shift_imm(dbgi, block, new_op2, mov_op,
					attr->shift_modifier, attr->shift_immediate);
			}
			break;

		case ARM_SHF_ASR_REG:
		case ARM_SHF_LSL_REG:
		case ARM_SHF_LSR_REG:
		case ARM_SHF_ROR_REG:
			if (factory[idx].new_binop_reg_shift_reg) {
				ir_node *mov_op  = get_irn_n(new_op1, n_arm_Mov_Rm);
				ir_node *mov_sft = get_irn_n(new_op1, n_arm_Mov_Rs);
				return factory[idx].new_binop_reg_shift_reg(dbgi, block, new_op2, mov_op, mov_sft,
					attr->shift_modifier);
			}
			break;

		case ARM_SHF_REG:
		case ARM_SHF_RRX:
			break;
		case ARM_SHF_INVALID:
			panic("invalid shift");
		}
	}
	return factory->new_binop_reg(dbgi, block, new_op1, new_op2);
}

static ir_node *gen_int_binop(ir_node *node, match_flags_t flags,
                              const arm_binop_factory_t *factory)
{
	ir_node *op1 = get_binop_left(node);
	ir_node *op2 = get_binop_right(node);
	return gen_int_binop_ops(node, op1, op2, flags, factory);
}

static ir_node *gen_Ror(ir_node *node, ir_node *op1, ir_node *op2,
                        bool negate_op)
{
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node  *block   = be_transform_nodes_block(node);
	ir_node  *new_op1 = be_transform_node(op1);
	if (is_Const(op2)) {
		ir_tarval *tv   = get_Const_tarval(op2);
		ir_mode   *mode = get_irn_mode(node);
		long       bits = get_mode_size_bits(mode);
		if (tarval_is_long(tv) && bits == 32) {
			long val = get_tarval_long(tv);
			val = (negate_op ? bits - val : val) & 31;
			return new_bd_arm_Mov_reg_shift_imm(dbgi, block, new_op1, ARM_SHF_ROR_IMM, val);
		}
	}

	ir_node *new_op2 = be_transform_node(op2);
	if (negate_op) {
		new_op2 = new_bd_arm_Rsb_imm(dbgi, block, new_op2, 32, 0);
	}
	return new_bd_arm_Mov_reg_shift_reg(dbgi, block, new_op1, new_op2,
											ARM_SHF_ROR_REG);
}

static bool is_low_mask(ir_tarval *tv)
{
	return get_tarval_popcount(tv) == 16 && get_tarval_highest_bit(tv) == 15;
}

static bool is_high_mask(ir_tarval *tv)
{
	return get_tarval_popcount(tv) == 16 && get_tarval_lowest_bit(tv) == 16;
}

static ir_node *match_pkh(ir_node *node)
{
	assert(is_Or(node) || is_Add(node));
	ir_node *left  = get_binop_left(node);
	ir_node *right = get_binop_right(node);
	if (!is_And(left) || !is_And(right))
		return NULL;
	ir_node *left_right  = get_And_right(left);
	ir_node *right_right = get_And_right(right);
	if (!is_Const(left_right) || !is_Const(right_right))
		return NULL;
	/* we want the low-mask on the right side */
	if (is_high_mask(get_Const_tarval(left_right))) {
		ir_node *tmp = left;
		left       = right;
		right      = tmp;
		left_right = right_right;
	} else if (!is_high_mask(get_Const_tarval(right_right))) {
		return NULL;
	}
	if (!is_low_mask(get_Const_tarval(left_right)))
		return NULL;
	ir_node *left_left  = get_And_left(left);
	ir_node *right_left = get_And_left(right);
	static const arm_binop_factory_t pkhbt_pkhtb_factory[2] = {
		{
			new_bd_arm_Pkhbt_reg,
			new_bd_arm_Pkhbt_imm,
			new_bd_arm_Pkhbt_reg_shift_reg,
			new_bd_arm_Pkhbt_reg_shift_imm
		},
		{
			new_bd_arm_Pkhtb_reg,
			new_bd_arm_Pkhtb_imm,
			new_bd_arm_Pkhtb_reg_shift_reg,
			new_bd_arm_Pkhtb_reg_shift_imm
		}
	};
	return gen_int_binop_ops(node, left_left, right_left, MATCH_REVERSE,
	                         pkhbt_pkhtb_factory);
}

/**
 * Creates an ARM Add.
 *
 * @return the created arm Add node
 */
static ir_node *gen_Add(ir_node *node)
{
	ir_node *rotl_left;
	ir_node *rotl_right;
	if (be_pattern_is_rotl(node, &rotl_left, &rotl_right)) {
		if (is_Minus(rotl_right))
			return gen_Ror(node, rotl_left, get_Minus_op(rotl_right), false);
		return gen_Ror(node, rotl_left, rotl_right, true);
	}
	ir_node *pkh = match_pkh(node);
	if (pkh != NULL)
		return pkh;

	ir_mode *mode = get_irn_mode(node);
	if (mode_is_float(mode)) {
		ir_node  *block   = be_transform_nodes_block(node);
		ir_node  *op1     = get_Add_left(node);
		ir_node  *op2     = get_Add_right(node);
		dbg_info *dbgi    = get_irn_dbg_info(node);
		ir_node  *new_op1 = be_transform_node(op1);
		ir_node  *new_op2 = be_transform_node(op2);
		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			return new_bd_arm_Adf(dbgi, block, new_op1, new_op2, mode);
		} else {
			panic("softfloat not lowered");
		}
	} else {
		ir_node *left  = get_Add_left(node);
		ir_node *right = get_Add_right(node);
		ir_node *mul_left;
		ir_node *mul_right;
		ir_node *other;
		if (is_Mul(left)) {
			mul_left  = get_Mul_left(left);
			mul_right = get_Mul_right(left);
			other     = right;
			goto create_mla;
		} else if (is_Mul(right)) {
			mul_left  = get_Mul_left(right);
			mul_right = get_Mul_right(right);
			other     = left;
create_mla:;
			dbg_info *dbgi      = get_irn_dbg_info(node);
			ir_node  *block     = be_transform_nodes_block(node);
			ir_node  *new_left  = be_transform_node(mul_left);
			ir_node  *new_right = be_transform_node(mul_right);
			ir_node  *new_add   = be_transform_node(other);
			if (arm_cg_config.variant < ARM_VARIANT_6)
				return new_bd_arm_Mla_v5(dbgi, block, new_left, new_right, new_add);
			else
				return new_bd_arm_Mla(dbgi, block, new_left, new_right,
				                      new_add);
		}

		static const arm_binop_factory_t add_factory = {
			new_bd_arm_Add_reg,
			new_bd_arm_Add_imm,
			new_bd_arm_Add_reg_shift_reg,
			new_bd_arm_Add_reg_shift_imm
		};
		return gen_int_binop_ops(node, left, right,
		                         MATCH_COMMUTATIVE | MATCH_SIZE_NEUTRAL,
		                         &add_factory);
	}
}

static ir_node *gen_arm_AddS_t(ir_node *node)
{
	static const arm_binop_factory_t adds_factory = {
		new_bd_arm_AddS_reg,
		new_bd_arm_AddS_imm,
		new_bd_arm_AddS_reg_shift_reg,
		new_bd_arm_AddS_reg_shift_imm,
	};
	ir_node *left  = get_irn_n(node, n_arm_AddS_t_left);
	ir_node *right = get_irn_n(node, n_arm_AddS_t_right);
	ir_node *res   = gen_int_binop_ops(node, left, right,
	                                   MATCH_COMMUTATIVE | MATCH_SIZE_NEUTRAL,
	                                   &adds_factory);
	arch_set_irn_register_out(res, pn_arm_AddS_flags, &arm_registers[REG_FL]);
	return res;
}

static ir_node *gen_Proj_arm_AddS_t(ir_node *node)
{
	unsigned pn       = get_Proj_num(node);
	ir_node *pred     = get_Proj_pred(node);
	ir_node *new_pred = be_transform_node(pred);
	switch ((pn_arm_AddS_t)pn) {
	case pn_arm_AddS_t_res:
		return new_r_Proj(new_pred, arm_mode_gp, pn_arm_AddS_res);
	case pn_arm_AddS_t_flags:
		return new_r_Proj(new_pred, arm_mode_flags, pn_arm_AddS_flags);
	}
	panic("%+F: Invalid proj number", node);
}

static ir_node *gen_arm_AdC_t(ir_node *node)
{
	ir_node *left  = get_irn_n(node, n_arm_AdC_t_left);
	ir_node *right = get_irn_n(node, n_arm_AdC_t_right);
	ir_node *flags = get_irn_n(node, n_arm_AdC_t_flags);
	/* TODO: handle complete set of shifter operands */
	ir_node *new_left  = be_transform_node(left);
	ir_node *new_right = be_transform_node(right);
	ir_node *new_flags = be_transform_node(flags);

	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *new_block = be_transform_nodes_block(node);
	ir_node  *res       = new_bd_arm_AdC_reg(dbgi, new_block, new_left,
	                                         new_right, new_flags);
	return res;
}

/**
 * Creates an ARM Mul.
 *
 * @return the created arm Mul node
 */
static ir_node *gen_Mul(ir_node *node)
{
	ir_node  *block   = be_transform_nodes_block(node);
	ir_node  *op1     = get_Mul_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Mul_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbg     = get_irn_dbg_info(node);

	if (mode_is_float(mode)) {
		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			return new_bd_arm_Muf(dbg, block, new_op1, new_op2, mode);
		} else {
			panic("softfloat not lowered");
		}
	}
	assert(mode_is_data(mode));
	if (arm_cg_config.variant < ARM_VARIANT_6) {
		return new_bd_arm_Mul_v5(dbg, block, new_op1, new_op2);
	} else {
		return new_bd_arm_Mul(dbg, block, new_op1, new_op2);
	}
}

static ir_node *gen_arm_UMulL_t(ir_node *node)
{
	ir_node  *block     = be_transform_nodes_block(node);
	ir_node  *left      = get_irn_n(node, n_arm_UMulL_t_left);
	ir_node  *new_left  = be_transform_node(left);
	ir_node  *right     = get_irn_n(node, n_arm_UMulL_t_right);
	ir_node  *new_right = be_transform_node(right);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	return new_bd_arm_UMulL(dbgi, block, new_left, new_right);
}

static ir_node *gen_Proj_arm_UMulL_t(ir_node *node)
{
	unsigned pn       = get_Proj_num(node);
	ir_node *pred     = get_Proj_pred(node);
	ir_node *new_pred = be_transform_node(pred);
	switch ((pn_arm_UMulL_t)pn) {
	case pn_arm_UMulL_t_low:
		return new_r_Proj(new_pred, arm_mode_gp, pn_arm_UMulL_low);
	case pn_arm_UMulL_t_high:
		return new_r_Proj(new_pred, arm_mode_gp, pn_arm_UMulL_high);
	}
	panic("%+F: Invalid proj number", node);
}

static ir_node *gen_Div(ir_node *node)
{
	ir_node  *block   = be_transform_nodes_block(node);
	ir_node  *op1     = get_Div_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Div_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_Div_resmode(node);
	dbg_info *dbg     = get_irn_dbg_info(node);

	/* integer division should be replaced by builtin call */
	assert(mode_is_float(mode));

	if (arm_cg_config.fpu == ARM_FPU_FPA) {
		return new_bd_arm_Dvf(dbg, block, new_op1, new_op2, mode);
	} else {
		panic("softfloat not lowered");
	}
}

static ir_node *gen_And(ir_node *node)
{
	static const arm_binop_factory_t and_factory = {
		new_bd_arm_And_reg,
		new_bd_arm_And_imm,
		new_bd_arm_And_reg_shift_reg,
		new_bd_arm_And_reg_shift_imm
	};
	static const arm_binop_factory_t bic_factory = {
		new_bd_arm_Bic_reg,
		new_bd_arm_Bic_imm,
		new_bd_arm_Bic_reg_shift_reg,
		new_bd_arm_Bic_reg_shift_imm
	};

	/* check for and not */
	arm_immediate_t imm;
	ir_node *left  = get_And_left(node);
	ir_node *right = get_And_right(node);
	if (is_Not(right)) {
		ir_node *right_not = get_Not_op(right);
		return gen_int_binop_ops(node, left, right_not, MATCH_SIZE_NEUTRAL,
		                         &bic_factory);
	} else if (is_Not(left)) {
		ir_node *left_not = get_Not_op(left);
		return gen_int_binop_ops(node, right, left_not, MATCH_SIZE_NEUTRAL,
		                         &bic_factory);
	} else if (try_encode_as_not_immediate(right, &imm)) {
		dbg_info *const dbgi  = get_irn_dbg_info(node);
		ir_node  *const block = be_transform_nodes_block(node);
		ir_node  *const new_l = be_transform_node(left);
		return new_bd_arm_Bic_imm(dbgi, block, new_l, imm.imm_8, imm.rot);
	} else {
		return gen_int_binop(node, MATCH_COMMUTATIVE|MATCH_SIZE_NEUTRAL,
		                     &and_factory);
	}
}

static ir_node *gen_Or(ir_node *node)
{
	ir_node *rotl_left;
	ir_node *rotl_right;
	if (be_pattern_is_rotl(node, &rotl_left, &rotl_right)) {
		if (is_Minus(rotl_right))
			return gen_Ror(node, rotl_left, get_Minus_op(rotl_right), false);
		return gen_Ror(node, rotl_left, rotl_right, true);
	}
	ir_node *pkh = match_pkh(node);
	if (pkh != NULL)
		return pkh;

	static const arm_binop_factory_t or_factory = {
		new_bd_arm_Or_reg,
		new_bd_arm_Or_imm,
		new_bd_arm_Or_reg_shift_reg,
		new_bd_arm_Or_reg_shift_imm
	};
	return gen_int_binop(node, MATCH_COMMUTATIVE | MATCH_SIZE_NEUTRAL, &or_factory);
}

static ir_node *gen_arm_OrPl_t(ir_node *node)
{
	ir_node *left     = get_irn_n(node, n_arm_OrPl_t_left);
	ir_node *right    = get_irn_n(node, n_arm_OrPl_t_right);
	ir_node *falseval = get_irn_n(node, n_arm_OrPl_t_falseval);
	ir_node *flags    = get_irn_n(node, n_arm_OrPl_t_flags);
	/* TODO: handle complete set of shifter operands */
	ir_node *new_left     = be_transform_node(left);
	ir_node *new_right    = be_transform_node(right);
	ir_node *new_falseval = be_transform_node(falseval);
	ir_node *new_flags    = be_transform_node(flags);

	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *new_block = be_transform_nodes_block(node);
	ir_node  *res       = new_bd_arm_OrPl(dbgi, new_block, new_left, new_right,
	                                      new_falseval, new_flags);
	return res;
}

static ir_node *gen_Eor(ir_node *node)
{
	static const arm_binop_factory_t eor_factory = {
		new_bd_arm_Eor_reg,
		new_bd_arm_Eor_imm,
		new_bd_arm_Eor_reg_shift_reg,
		new_bd_arm_Eor_reg_shift_imm
	};
	return gen_int_binop(node, MATCH_COMMUTATIVE | MATCH_SIZE_NEUTRAL,
	                     &eor_factory);
}

static ir_node *gen_Sub(ir_node *node)
{
	ir_mode *mode  = get_irn_mode(node);
	ir_node *left  = get_Sub_left(node);
	ir_node *right = get_Sub_right(node);
	if (mode_is_float(mode)) {
		ir_node  *block     = be_transform_nodes_block(node);
		ir_node  *new_left  = be_transform_node(left);
		ir_node  *new_right = be_transform_node(right);
		dbg_info *dbgi      = get_irn_dbg_info(node);

		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			return new_bd_arm_Suf(dbgi, block, new_left, new_right, mode);
		} else {
			panic("softfloat not lowered");
		}
	} else {
		if (is_Mul(right) && arm_cg_config.variant >= ARM_VARIANT_6T2) {
			dbg_info *dbgi      = get_irn_dbg_info(node);
			ir_node  *block     = be_transform_nodes_block(node);
			ir_node  *mul_left  = get_Mul_left(right);
			ir_node  *mul_right = get_Mul_right(right);
			ir_node  *new_left  = be_transform_node(mul_left);
			ir_node  *new_right = be_transform_node(mul_right);
			ir_node  *new_sub   = be_transform_node(left);
			return new_bd_arm_Mls(dbgi, block, new_left, new_right, new_sub);
		}

		static const arm_binop_factory_t sub_rsb_factory[2] = {
			{
				new_bd_arm_Sub_reg,
				new_bd_arm_Sub_imm,
				new_bd_arm_Sub_reg_shift_reg,
				new_bd_arm_Sub_reg_shift_imm
			},
			{
				new_bd_arm_Rsb_reg,
				new_bd_arm_Rsb_imm,
				new_bd_arm_Rsb_reg_shift_reg,
				new_bd_arm_Rsb_reg_shift_imm
			}
		};
		return gen_int_binop(node, MATCH_SIZE_NEUTRAL | MATCH_REVERSE, sub_rsb_factory);
	}
}

static ir_node *gen_arm_SubS_t(ir_node *node)
{
	static const arm_binop_factory_t subs_factory[2] = {
		{
			new_bd_arm_SubS_reg,
			new_bd_arm_SubS_imm,
			new_bd_arm_SubS_reg_shift_reg,
			new_bd_arm_SubS_reg_shift_imm,
		},
		{
			new_bd_arm_RsbS_reg,
			new_bd_arm_RsbS_imm,
			new_bd_arm_RsbS_reg_shift_reg,
			new_bd_arm_RsbS_reg_shift_imm,
		},
	};
	ir_node *left  = get_irn_n(node, n_arm_SubS_t_left);
	ir_node *right = get_irn_n(node, n_arm_SubS_t_right);
	ir_node *res   = gen_int_binop_ops(node, left, right,
	                                   MATCH_SIZE_NEUTRAL | MATCH_REVERSE,
	                                   subs_factory);
	assert((int)pn_arm_SubS_flags == (int)pn_arm_RsbS_flags);
	arch_set_irn_register_out(res, pn_arm_SubS_flags, &arm_registers[REG_FL]);
	return res;
}

static ir_node *gen_Proj_arm_SubS_t(ir_node *node)
{
	unsigned pn       = get_Proj_num(node);
	ir_node *pred     = get_Proj_pred(node);
	ir_node *new_pred = be_transform_node(pred);
	assert((int)pn_arm_SubS_flags == (int)pn_arm_RsbS_flags);
	assert((int)pn_arm_SubS_res == (int)pn_arm_RsbS_res);
	switch ((pn_arm_SubS_t)pn) {
	case pn_arm_SubS_t_res:
		return new_r_Proj(new_pred, arm_mode_gp, pn_arm_SubS_res);
	case pn_arm_SubS_t_flags:
		return new_r_Proj(new_pred, arm_mode_flags, pn_arm_SubS_flags);
	}
	panic("%+F: Invalid proj number", node);
}

static ir_node *gen_arm_SbC_t(ir_node *node)
{
	ir_node *left  = get_irn_n(node, n_arm_SbC_t_left);
	ir_node *right = get_irn_n(node, n_arm_SbC_t_right);
	ir_node *flags = get_irn_n(node, n_arm_SbC_t_flags);
	/* TODO: handle complete set of shifter operands */
	ir_node *new_left  = be_transform_node(left);
	ir_node *new_right = be_transform_node(right);
	ir_node *new_flags = be_transform_node(flags);

	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *new_block = be_transform_nodes_block(node);
	ir_node  *res       = new_bd_arm_SbC_reg(dbgi, new_block, new_left,
	                                         new_right, new_flags);
	return res;
}

/**
 * Checks if a given value can be used as an immediate for the given
 * ARM shift mode.
 */
static bool can_use_shift_constant(unsigned int val,
                                   arm_shift_modifier_t modifier)
{
	if (val <= 31)
		return true;
	if (val == 32 && modifier != ARM_SHF_LSL_REG && modifier != ARM_SHF_ROR_REG)
		return true;
	return false;
}

/**
 * generate an ARM shift instruction.
 *
 * @param node            the node
 * @param flags           matching flags
 * @param shift_modifier  initial encoding of the desired shift operation
 */
static ir_node *make_shift(ir_node *node, match_flags_t flags,
		arm_shift_modifier_t shift_modifier)
{
	ir_node  *block = be_transform_nodes_block(node);
	ir_node  *op1   = get_binop_left(node);
	ir_node  *op2   = get_binop_right(node);
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	if (get_mode_modulo_shift(mode) != 256)
		panic("modulo shift!=256 not supported");

	if (flags & MATCH_SIZE_NEUTRAL) {
		op1 = be_skip_downconv(op1, true);
		op2 = be_skip_downconv(op2, true);
	}

	ir_node *new_op1 = be_transform_node(op1);
	if (is_Const(op2)) {
		unsigned int const val = get_Const_long(op2);
		if (can_use_shift_constant(val, shift_modifier)) {
			switch (shift_modifier) {
			case ARM_SHF_LSL_REG: shift_modifier = ARM_SHF_LSL_IMM; break;
			case ARM_SHF_LSR_REG: shift_modifier = ARM_SHF_LSR_IMM; break;
			case ARM_SHF_ASR_REG: shift_modifier = ARM_SHF_ASR_IMM; break;
			case ARM_SHF_ROR_REG: shift_modifier = ARM_SHF_ROR_IMM; break;
			default: panic("unexpected shift modifier");
			}
			return new_bd_arm_Mov_reg_shift_imm(dbgi, block, new_op1,
			                                    shift_modifier, val);
		}
	}

	ir_node *new_op2 = be_transform_node(op2);
	return new_bd_arm_Mov_reg_shift_reg(dbgi, block, new_op1, new_op2,
	                                    shift_modifier);
}

static ir_node *gen_Shl(ir_node *node)
{
	return make_shift(node, MATCH_SIZE_NEUTRAL, ARM_SHF_LSL_REG);
}

static ir_node *gen_Shr(ir_node *node)
{
	return make_shift(node, MATCH_NONE, ARM_SHF_LSR_REG);
}

static ir_node *gen_Shrs(ir_node *node)
{
	return make_shift(node, MATCH_NONE, ARM_SHF_ASR_REG);
}

static ir_node *gen_Not(ir_node *node)
{
	ir_node  *block  = be_transform_nodes_block(node);
	ir_node  *op     = get_Not_op(node);
	ir_node  *new_op = be_transform_node(op);
	dbg_info *dbgi   = get_irn_dbg_info(node);

	/* check if we can fold in a Mov */
	if (is_arm_Mov(new_op)) {
		const arm_shifter_operand_t *attr = get_arm_shifter_operand_attr_const(new_op);

		switch (attr->shift_modifier) {
		case ARM_SHF_IMM:
		case ARM_SHF_ASR_IMM:
		case ARM_SHF_LSL_IMM:
		case ARM_SHF_LSR_IMM:
		case ARM_SHF_ROR_IMM: {
			ir_node *mov_op = get_irn_n(new_op, n_arm_Mov_Rm);
			return new_bd_arm_Mvn_reg_shift_imm(dbgi, block, mov_op,
				attr->shift_modifier, attr->shift_immediate);
		}

		case ARM_SHF_ASR_REG:
		case ARM_SHF_LSL_REG:
		case ARM_SHF_LSR_REG:
		case ARM_SHF_ROR_REG: {
			ir_node *mov_op  = get_irn_n(new_op, n_arm_Mov_Rm);
			ir_node *mov_sft = get_irn_n(new_op, n_arm_Mov_Rs);
			return new_bd_arm_Mvn_reg_shift_reg(dbgi, block, mov_op, mov_sft,
				attr->shift_modifier);
		}

		case ARM_SHF_REG:
		case ARM_SHF_RRX:
			break;
		case ARM_SHF_INVALID:
			panic("invalid shift");
		}
	}

	return new_bd_arm_Mvn_reg(dbgi, block, new_op);
}

static ir_node *gen_Minus(ir_node *node)
{
	ir_node  *block  = be_transform_nodes_block(node);
	ir_node  *op     = get_Minus_op(node);
	ir_node  *new_op = be_transform_node(op);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_mode  *mode   = get_irn_mode(node);

	if (mode_is_float(mode)) {
		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			return new_bd_arm_Mvf(dbgi, block, op, mode);
		} else {
			panic("softfloat not lowered");
		}
	}
	assert(mode_is_data(mode));
	return new_bd_arm_Rsb_imm(dbgi, block, new_op, 0, 0);
}

static ir_node *gen_Load(ir_node *node)
{
	ir_node  *block   = be_transform_nodes_block(node);
	ir_node  *ptr     = get_Load_ptr(node);
	ir_node  *new_ptr = be_transform_node(ptr);
	ir_node  *mem     = get_Load_mem(node);
	ir_node  *new_mem = be_transform_node(mem);
	ir_mode  *mode    = get_Load_mode(node);
	dbg_info *dbgi    = get_irn_dbg_info(node);
	if (get_Load_unaligned(node) == align_non_aligned)
		panic("unaligned Loads not supported yet");

	ir_node *new_load;
	if (mode_is_float(mode)) {
		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			new_load = new_bd_arm_Ldf(dbgi, block, new_ptr, new_mem, mode,
			                          NULL, 0, 0, false);
		} else {
			panic("softfloat not lowered");
		}
	} else {
		assert(mode_is_data(mode) && "unsupported mode for Load");

		new_load = new_bd_arm_Ldr(dbgi, block, new_ptr, new_mem, mode, NULL, 0, 0, false);
	}
	set_irn_pinned(new_load, get_irn_pinned(node));

	return new_load;
}

static ir_node *gen_Store(ir_node *node)
{
	ir_node  *block   = be_transform_nodes_block(node);
	ir_node  *ptr     = get_Store_ptr(node);
	ir_node  *new_ptr = be_transform_node(ptr);
	ir_node  *mem     = get_Store_mem(node);
	ir_node  *new_mem = be_transform_node(mem);
	ir_node  *val     = get_Store_value(node);
	ir_node  *new_val = be_transform_node(val);
	ir_mode  *mode    = get_irn_mode(val);
	dbg_info *dbgi    = get_irn_dbg_info(node);
	if (get_Store_unaligned(node) == align_non_aligned)
		panic("unaligned Stores not supported yet");

	ir_node *new_store;
	if (mode_is_float(mode)) {
		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			new_store = new_bd_arm_Stf(dbgi, block, new_ptr, new_val,
			                           new_mem, mode, NULL, 0, 0, false);
		} else {
			panic("softfloat not lowered");
		}
	} else {
		assert(mode_is_data(mode) && "unsupported mode for Store");
		new_store = new_bd_arm_Str(dbgi, block, new_ptr, new_val, new_mem, mode,
		                           NULL, 0, 0, false);
	}
	set_irn_pinned(new_store, get_irn_pinned(node));
	return new_store;
}

static ir_node *gen_Jmp(ir_node *node)
{
	ir_node  *new_block = be_transform_nodes_block(node);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	return new_bd_arm_Jmp(dbgi, new_block);
}

static ir_node *gen_Switch(ir_node *node)
{
	ir_graph              *irg      = get_irn_irg(node);
	ir_node               *block    = be_transform_nodes_block(node);
	ir_node               *selector = get_Switch_selector(node);
	dbg_info              *dbgi     = get_irn_dbg_info(node);
	ir_node               *new_op   = be_transform_node(selector);
	const ir_switch_table *table    = get_Switch_table(node);
	unsigned               n_outs   = get_Switch_n_outs(node);

	table = ir_switch_table_duplicate(irg, table);

	/* switch selector should be lowered to singled word already */
	ir_mode *mode = get_irn_mode(selector);
	if (get_mode_size_bits(mode) != 32)
		panic("arm: unexpected switch selector mode");

	return new_bd_arm_SwitchJmp(dbgi, block, new_op, n_outs, table);
}

static ir_node *gen_Cmp(ir_node *node)
{
	ir_node  *block    = be_transform_nodes_block(node);
	ir_node  *op1      = get_Cmp_left(node);
	ir_node  *op2      = get_Cmp_right(node);
	ir_mode  *cmp_mode = get_irn_mode(op1);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	if (mode_is_float(cmp_mode)) {
		/* TODO: this is broken... */
		ir_node *new_op1 = be_transform_node(op1);
		ir_node *new_op2 = be_transform_node(op2);

		return new_bd_arm_Cmfe(dbgi, block, new_op1, new_op2, false);
	}

	assert(get_irn_mode(op2) == cmp_mode);
	bool is_unsigned = !mode_is_signed(cmp_mode);

	/* integer compare, TODO: use shifter_op in all its combinations */
	ir_node *new_op1 = be_transform_node(op1);
	new_op1 = gen_extension(dbgi, block, new_op1, cmp_mode);
	ir_node *new_op2 = be_transform_node(op2);
	new_op2 = gen_extension(dbgi, block, new_op2, cmp_mode);
	return new_bd_arm_Cmp_reg(dbgi, block, new_op1, new_op2, false,
	                          is_unsigned);
}

static ir_node *gen_Cond(ir_node *node)
{
	ir_node    *const block     = be_transform_nodes_block(node);
	dbg_info   *const dbgi      = get_irn_dbg_info(node);
	ir_node    *const selector  = get_Cond_selector(node);
	ir_node    *const flag_node = be_transform_node(selector);
	ir_relation const relation  = get_Cmp_relation(selector);
	return new_bd_arm_B(dbgi, block, flag_node, relation);
}

enum fpa_imm_mode {
	FPA_IMM_FLOAT  = 0,
	FPA_IMM_DOUBLE = 1,
	FPA_IMM_MAX    = FPA_IMM_DOUBLE
};

static ir_tarval *fpa_imm[FPA_IMM_MAX + 1][fpa_max];

static ir_node *gen_Const(ir_node *node)
{
	ir_node  *block = be_transform_nodes_block(node);
	ir_mode  *mode  = get_irn_mode(node);
	dbg_info *dbg   = get_irn_dbg_info(node);

	if (mode_is_float(mode)) {
		if (arm_cg_config.fpu == ARM_FPU_FPA) {
			ir_tarval *tv = get_Const_tarval(node);
			return new_bd_arm_fConst(dbg, block, tv);
		} else {
			panic("softfloat not lowered");
		}
	}
	return create_const_graph(node, block);
}

static ir_node *gen_Address(ir_node *node)
{
	ir_node   *block  = be_transform_nodes_block(node);
	ir_entity *entity = get_Address_entity(node);
	dbg_info  *dbgi   = get_irn_dbg_info(node);
	if (is_tls_entity(entity))
		panic("TLS not supported yet");
	return new_bd_arm_Address(dbgi, block, entity, 0);
}

static ir_node *ints_to_double(dbg_info *dbgi, ir_node *block, ir_node *node0,
                               ir_node *node1)
{
	/* the good way to do this would be to use the stm (store multiple)
	 * instructions, since our input is nearly always 2 consecutive 32bit
	 * registers... */
	ir_graph *irg   = get_irn_irg(block);
	ir_node  *stack = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *str0  = new_bd_arm_Str(dbgi, block, stack, node0, nomem,
	                                 arm_mode_gp, NULL, 0, 0, true);
	ir_node  *str1  = new_bd_arm_Str(dbgi, block, stack, node1, nomem,
	                                 arm_mode_gp, NULL, 0, 4, true);
	ir_node  *in[]  = { str0, str1 };
	ir_node  *sync  = new_r_Sync(block, ARRAY_SIZE(in), in);
	set_irn_pinned(str0, op_pin_state_floats);
	set_irn_pinned(str1, op_pin_state_floats);

	ir_node *ldf = new_bd_arm_Ldf(dbgi, block, stack, sync, mode_D, NULL, 0, 0,
	                              true);
	set_irn_pinned(ldf, op_pin_state_floats);

	return new_r_Proj(ldf, mode_fp, pn_arm_Ldf_res);
}

static ir_node *int_to_float(dbg_info *dbgi, ir_node *block, ir_node *node)
{
	ir_graph *irg   = get_irn_irg(block);
	ir_node  *stack = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *str   = new_bd_arm_Str(dbgi, block, stack, node, nomem,
	                                 arm_mode_gp, NULL, 0, 0, true);
	set_irn_pinned(str, op_pin_state_floats);

	ir_node *ldf = new_bd_arm_Ldf(dbgi, block, stack, str, mode_F, NULL, 0, 0,
	                              true);
	set_irn_pinned(ldf, op_pin_state_floats);

	return new_r_Proj(ldf, mode_fp, pn_arm_Ldf_res);
}

static ir_node *float_to_int(dbg_info *dbgi, ir_node *block, ir_node *node)
{
	ir_graph *irg   = get_irn_irg(block);
	ir_node  *stack = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *stf   = new_bd_arm_Stf(dbgi, block, stack, node, nomem, mode_F,
	                                 NULL, 0, 0, true);
	set_irn_pinned(stf, op_pin_state_floats);

	ir_node *ldr = new_bd_arm_Ldr(dbgi, block, stack, stf, arm_mode_gp,
	                              NULL, 0, 0, true);
	set_irn_pinned(ldr, op_pin_state_floats);

	return new_r_Proj(ldr, arm_mode_gp, pn_arm_Ldr_res);
}

static void double_to_ints(dbg_info *dbgi, ir_node *block, ir_node *node,
                           ir_node **out_value0, ir_node **out_value1)
{
	ir_graph *irg   = get_irn_irg(block);
	ir_node  *stack = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *stf   = new_bd_arm_Stf(dbgi, block, stack, node, nomem, mode_D,
	                                 NULL, 0, 0, true);
	set_irn_pinned(stf, op_pin_state_floats);

	ir_node *ldr0 = new_bd_arm_Ldr(dbgi, block, stack, stf, arm_mode_gp, NULL,
	                               0, 0, true);
	set_irn_pinned(ldr0, op_pin_state_floats);
	ir_node *ldr1 = new_bd_arm_Ldr(dbgi, block, stack, stf, arm_mode_gp, NULL,
	                               0, 4, true);
	set_irn_pinned(ldr1, op_pin_state_floats);

	*out_value0 = new_r_Proj(ldr0, arm_mode_gp, pn_arm_Ldr_res);
	*out_value1 = new_r_Proj(ldr1, arm_mode_gp, pn_arm_Ldr_res);
}

static ir_node *gen_CopyB(ir_node *node)
{
	ir_node  *block    = be_transform_nodes_block(node);
	ir_node  *src      = get_CopyB_src(node);
	ir_node  *new_src  = be_transform_node(src);
	ir_node  *dst      = get_CopyB_dst(node);
	ir_node  *new_dst  = be_transform_node(dst);
	ir_node  *mem      = get_CopyB_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	dbg_info *dbg      = get_irn_dbg_info(node);
	int       size     = get_type_size_bytes(get_CopyB_type(node));
	ir_node  *src_copy = be_new_Copy(block, new_src);
	ir_node  *dst_copy = be_new_Copy(block, new_dst);

	return new_bd_arm_CopyB(dbg, block, dst_copy, src_copy,
	                        be_new_AnyVal(block, &arm_reg_classes[CLASS_arm_gp]),
	                        be_new_AnyVal(block, &arm_reg_classes[CLASS_arm_gp]),
	                        be_new_AnyVal(block, &arm_reg_classes[CLASS_arm_gp]),
	                        new_mem, size);
}

/**
 * Transform builtin clz.
 */
static ir_node *gen_clz(ir_node *node)
{
	ir_node  *block  = be_transform_nodes_block(node);
	dbg_info *dbg    = get_irn_dbg_info(node);
	ir_node  *op     = get_irn_n(node, 1);
	ir_node  *new_op = be_transform_node(op);

	/* TODO armv5 instruction, otherwise create a call */
	return new_bd_arm_Clz(dbg, block, new_op);
}

/**
 * Transform Builtin node.
 */
static ir_node *gen_Builtin(ir_node *node)
{
	ir_builtin_kind kind = get_Builtin_kind(node);
	switch (kind) {
	case ir_bk_trap:
	case ir_bk_debugbreak:
	case ir_bk_return_address:
	case ir_bk_frame_address:
	case ir_bk_prefetch:
	case ir_bk_ffs:
		break;
	case ir_bk_clz:
		return gen_clz(node);
	case ir_bk_ctz:
	case ir_bk_parity:
	case ir_bk_popcount:
	case ir_bk_bswap:
	case ir_bk_outport:
	case ir_bk_inport:
	case ir_bk_saturating_increment:
	case ir_bk_compare_swap:
	case ir_bk_may_alias:
		break;
	}
	panic("Builtin %s not implemented", get_builtin_kind_name(kind));
}

/**
 * Transform Proj(Builtin) node.
 */
static ir_node *gen_Proj_Builtin(ir_node *proj)
{
	ir_node         *node     = get_Proj_pred(proj);
	ir_node         *new_node = be_transform_node(node);
	ir_builtin_kind kind      = get_Builtin_kind(node);

	switch (kind) {
	case ir_bk_return_address:
	case ir_bk_frame_address:
	case ir_bk_ffs:
	case ir_bk_clz:
	case ir_bk_ctz:
	case ir_bk_parity:
	case ir_bk_popcount:
	case ir_bk_bswap:
		assert(get_Proj_num(proj) == pn_Builtin_max+1);
		return new_node;
	case ir_bk_trap:
	case ir_bk_debugbreak:
	case ir_bk_prefetch:
	case ir_bk_outport:
		assert(get_Proj_num(proj) == pn_Builtin_M);
		return new_node;
	case ir_bk_inport:
	case ir_bk_saturating_increment:
	case ir_bk_compare_swap:
	case ir_bk_may_alias:
		break;
	}
	panic("Builtin %s not implemented", get_builtin_kind_name(kind));
}

static ir_node *gen_Proj_Load(ir_node *node)
{
	ir_node  *load     = get_Proj_pred(node);
	ir_node  *new_load = be_transform_node(load);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	unsigned  pn       = get_Proj_num(node);

	/* renumber the proj */
	switch (get_arm_irn_opcode(new_load)) {
	case iro_arm_Ldr:
		/* handle all gp loads equal: they have the same proj numbers. */
		if (pn == pn_Load_res) {
			return new_rd_Proj(dbgi, new_load, arm_mode_gp, pn_arm_Ldr_res);
		} else if (pn == pn_Load_M) {
			return new_rd_Proj(dbgi, new_load, mode_M, pn_arm_Ldr_M);
		}
		break;
	case iro_arm_Ldf:
		if (pn == pn_Load_res) {
			ir_mode *mode = get_Load_mode(load);
			return new_rd_Proj(dbgi, new_load, mode, pn_arm_Ldf_res);
		} else if (pn == pn_Load_M) {
			return new_rd_Proj(dbgi, new_load, mode_M, pn_arm_Ldf_M);
		}
		break;
	default:
		break;
	}
	panic("unsupported Proj from Load");
}

static ir_node *gen_Proj_Div(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	unsigned  pn       = get_Proj_num(node);

	switch ((pn_Div)pn) {
	case pn_Div_M:
		return new_rd_Proj(dbgi, new_pred, mode_M, pn_arm_Dvf_M);
	case pn_Div_res:
		return new_rd_Proj(dbgi, new_pred, mode, pn_arm_Dvf_res);
	case pn_Div_X_regular:
	case pn_Div_X_except:
		break;
	}
	panic("unsupported Proj from Div");
}

static ir_node *gen_Proj_Start(ir_node *node)
{
	unsigned pn = get_Proj_num(node);
	switch ((pn_Start)pn) {
	case pn_Start_M:
		return be_get_start_proj(get_irn_irg(node), &start_mem);

	case pn_Start_T_args:
		return new_r_Bad(get_irn_irg(node), mode_T);

	case pn_Start_P_frame_base:
		return be_get_start_proj(get_irn_irg(node), &start_val[REG_SP]);
	}
	panic("unexpected start proj: %u", pn);
}

static ir_node *gen_Proj_Proj_Start(ir_node *node)
{
	/* Proj->Proj->Start must be a method argument */
	assert(get_Proj_num(get_Proj_pred(node)) == pn_Start_T_args);

	ir_node                  *const new_block = be_transform_nodes_block(node);
	ir_graph                 *const irg       = get_irn_irg(new_block);
	unsigned                  const pn        = get_Proj_num(node);
	reg_or_stackslot_t const *const param     = &cconv->parameters[pn];
	arch_register_t    const *const reg0      = param->reg0;
	if (reg0 != NULL) {
		/* argument transmitted in register */
		ir_node *value = be_get_start_proj(irg, &start_val[reg0->global_index]);

		if (mode_is_float(reg0->cls->mode)) {
			ir_node *value1 = NULL;

			const arch_register_t *reg1 = param->reg1;
			if (reg1 != NULL) {
				value1 = be_get_start_proj(irg, &start_val[reg1->global_index]);
			} else if (param->entity != NULL) {
				ir_node *const fp  = get_irg_frame(irg);
				ir_node *const mem = be_get_start_proj(irg, &start_mem);
				ir_node *const ldr = new_bd_arm_Ldr(NULL, new_block, fp, mem,
				                                    arm_mode_gp, param->entity,
				                                    0, 0, true);
				value1 = new_r_Proj(ldr, arm_mode_gp, pn_arm_Ldr_res);
			}

			/* convert integer value to float */
			if (value1 == NULL) {
				value = int_to_float(NULL, new_block, value);
			} else {
				value = ints_to_double(NULL, new_block, value, value1);
			}
		}
		return value;
	} else {
		/* argument transmitted on stack */
		ir_node *const fp   = get_irg_frame(irg);
		ir_node *const mem  = be_get_start_proj(irg, &start_mem);
		ir_mode *const mode = get_type_mode(param->type);

		ir_node *load;
		ir_node *value;
		if (mode_is_float(mode)) {
			load  = new_bd_arm_Ldf(NULL, new_block, fp, mem, mode,
			                       param->entity, 0, 0, true);
			value = new_r_Proj(load, mode_fp, pn_arm_Ldf_res);
		} else {
			load  = new_bd_arm_Ldr(NULL, new_block, fp, mem, mode,
			                       param->entity, 0, 0, true);
			value = new_r_Proj(load, arm_mode_gp, pn_arm_Ldr_res);
		}
		set_irn_pinned(load, op_pin_state_floats);

		return value;
	}
}

/**
 * Finds number of output value of a mode_T node which is constrained to
 * a single specific register.
 */
static int find_out_for_reg(ir_node *node, const arch_register_t *reg)
{
	be_foreach_out(node, o) {
		const arch_register_req_t *req = arch_get_irn_register_req_out(node, o);
		if (req == reg->single_req)
			return o;
	}
	return -1;
}

static ir_node *gen_Proj_Proj_Call(ir_node *node)
{
	unsigned              pn            = get_Proj_num(node);
	ir_node              *call          = get_Proj_pred(get_Proj_pred(node));
	ir_node              *new_call      = be_transform_node(call);
	ir_type              *function_type = get_Call_type(call);
	calling_convention_t *cconv
		= arm_decide_calling_convention(NULL, function_type);
	const reg_or_stackslot_t *res = &cconv->results[pn];

	assert(res->reg0 != NULL && res->reg1 == NULL);
	int regn = find_out_for_reg(new_call, res->reg0);
	if (regn < 0) {
		panic("Internal error in calling convention for return %+F", node);
	}
	ir_mode *const mode = res->reg0->cls->mode;

	arm_free_calling_convention(cconv);

	return new_r_Proj(new_call, mode, regn);
}

static ir_node *gen_Proj_Call(ir_node *node)
{
	unsigned pn        = get_Proj_num(node);
	ir_node *call      = get_Proj_pred(node);
	ir_node *new_call  = be_transform_node(call);
	switch ((pn_Call)pn) {
	case pn_Call_M:
		return new_r_Proj(new_call, mode_M, pn_arm_Bl_M);
	case pn_Call_X_regular:
	case pn_Call_X_except:
	case pn_Call_T_result:
		break;
	}
	panic("unexpected Call proj %u", pn);
}

static ir_node *gen_Proj_Store(ir_node *node)
{
	ir_node *pred = get_Proj_pred(node);
	unsigned pn   = get_Proj_num(node);
	switch ((pn_Store)pn) {
	case pn_Store_M:
		return be_transform_node(pred);
	case pn_Store_X_regular:
	case pn_Store_X_except:
		break;
	}
	panic("unsupported Proj from Store");
}

static ir_node *gen_Proj_Proj(ir_node *node)
{
	ir_node *pred      = get_Proj_pred(node);
	ir_node *pred_pred = get_Proj_pred(pred);
	if (is_Call(pred_pred)) {
		return gen_Proj_Proj_Call(node);
	} else if (is_Start(pred_pred)) {
		return gen_Proj_Proj_Start(node);
	}
	panic("code selection didn't expect Proj(Proj) after %+F", pred_pred);
}

static ir_node *gen_Unknown(ir_node *node)
{
	ir_node  *new_block = be_transform_nodes_block(node);
	dbg_info *dbgi      = get_irn_dbg_info(node);

	/* just produce a 0 */
	ir_mode *mode = get_irn_mode(node);
	if (mode_is_float(mode)) {
		ir_tarval *tv     = get_mode_null(mode);
		ir_node   *fconst = new_bd_arm_fConst(dbgi, new_block, tv);
		return fconst;
	} else if (get_mode_arithmetic(mode) == irma_twos_complement) {
		return create_const_graph_value(dbgi, new_block, 0);
	}

	panic("unexpected Unknown mode");
}

/**
 * Produces the type which sits between the stack args and the locals on the
 * stack. It will contain the return address and space to store the old base
 * pointer.
 * @return The Firm type modeling the ABI between type.
 */
static ir_type *arm_get_between_type(void)
{
	static ir_type *between_type = NULL;
	if (between_type == NULL) {
		between_type = new_type_class(new_id_from_str("arm_between_type"));
		set_type_size_bytes(between_type, 0);
	}

	return between_type;
}

static void create_stacklayout(ir_graph *irg)
{
	ir_entity         *entity        = get_irg_entity(irg);
	ir_type           *function_type = get_entity_type(entity);
	be_stack_layout_t *layout        = be_get_irg_stack_layout(irg);

	/* calling conventions must be decided by now */
	assert(cconv != NULL);

	/* construct argument type */
	ident   *const arg_type_id = new_id_fmt("%s_arg_type", get_entity_ident(entity));
	ir_type *const arg_type    = new_type_struct(arg_type_id);
	for (unsigned p = 0, n_params = get_method_n_params(function_type);
	     p < n_params; ++p) {
		reg_or_stackslot_t *param = &cconv->parameters[p];
		if (param->type == NULL)
			continue;

		ident *const id = new_id_fmt("param_%u", p);
		param->entity = new_entity(arg_type, id, param->type);
		set_entity_offset(param->entity, param->offset);
	}

	/* TODO: what about external functions? we don't know most of the stack
	 * layout for them. And probably don't need all of this... */
	memset(layout, 0, sizeof(*layout));
	layout->frame_type     = get_irg_frame_type(irg);
	layout->between_type   = arm_get_between_type();
	layout->arg_type       = arg_type;
	layout->initial_offset = 0;
	layout->initial_bias   = 0;
	layout->sp_relative    = true;

	assert(N_FRAME_TYPES == 3);
	layout->order[0] = layout->frame_type;
	layout->order[1] = layout->between_type;
	layout->order[2] = layout->arg_type;
}

/**
 * transform the start node to the prolog code
 */
static ir_node *gen_Start(ir_node *node)
{
	ir_graph       *irg           = get_irn_irg(node);
	ir_entity      *entity        = get_irg_entity(irg);
	ir_type        *function_type = get_entity_type(entity);
	ir_node        *new_block     = be_transform_nodes_block(node);
	dbg_info       *dbgi          = get_irn_dbg_info(node);

	unsigned n_outs = 2; /* memory, sp */
	n_outs += cconv->n_param_regs;
	n_outs += ARRAY_SIZE(callee_saves);
	ir_node *start = new_bd_arm_Start(dbgi, new_block, n_outs);
	unsigned o     = 0;

	be_make_start_mem(&start_mem, start, o++);

	be_make_start_out(&start_val[REG_SP], start, o++, &arm_registers[REG_SP], true);

	/* function parameters in registers */
	for (size_t i = 0; i < get_method_n_params(function_type); ++i) {
		const reg_or_stackslot_t *param = &cconv->parameters[i];
		const arch_register_t    *reg0  = param->reg0;
		if (reg0)
			be_make_start_out(&start_val[reg0->global_index], start, o++, reg0, false);
		const arch_register_t *reg1 = param->reg1;
		if (reg1)
			be_make_start_out(&start_val[reg1->global_index], start, o++, reg1, false);
	}
	/* callee save regs */
	start_callee_saves_offset = o;
	for (size_t i = 0; i < ARRAY_SIZE(callee_saves); ++i) {
		const arch_register_t *reg = callee_saves[i];
		arch_set_irn_register_req_out(start, o, reg->single_req);
		arch_set_irn_register_out(start, o, reg);
		++o;
	}
	assert(n_outs == o);

	return start;
}

static ir_node *get_stack_pointer_for(ir_node *node)
{
	/* get predecessor in stack_order list */
	ir_node *stack_pred = be_get_stack_pred(stackorder, node);
	if (stack_pred == NULL) {
		/* first stack user in the current block. We can simply use the
		 * initial sp_proj for it */
		ir_graph *irg = get_irn_irg(node);
		return be_get_start_proj(irg, &start_val[REG_SP]);
	}

	be_transform_node(stack_pred);
	ir_node *stack = pmap_get(ir_node, node_to_stack, stack_pred);
	if (stack == NULL) {
		return get_stack_pointer_for(stack_pred);
	}

	return stack;
}

/**
 * transform a Return node into epilogue code + return statement
 */
static ir_node *gen_Return(ir_node *node)
{
	ir_node        *new_block      = be_transform_nodes_block(node);
	dbg_info       *dbgi           = get_irn_dbg_info(node);
	ir_node        *mem            = get_Return_mem(node);
	ir_node        *new_mem        = be_transform_node(mem);
	unsigned        n_callee_saves = ARRAY_SIZE(callee_saves);
	ir_node        *sp             = get_stack_pointer_for(node);
	unsigned        n_res          = get_Return_n_ress(node);
	ir_graph       *irg            = get_irn_irg(node);

	unsigned       p     = n_arm_Return_first_result;
	unsigned const n_ins = p + n_res + n_callee_saves;

	arch_register_req_t const **const reqs = be_allocate_in_reqs(irg, n_ins);
	ir_node **in = ALLOCAN(ir_node*, n_ins);

	in[n_arm_Return_mem]   = new_mem;
	reqs[n_arm_Return_mem] = arch_no_register_req;

	in[n_arm_Return_sp]   = sp;
	reqs[n_arm_Return_sp] = sp_reg->single_req;

	/* result values */
	for (size_t i = 0; i < n_res; ++i) {
		ir_node                  *res_value     = get_Return_res(node, i);
		ir_node                  *new_res_value = be_transform_node(res_value);
		const reg_or_stackslot_t *slot          = &cconv->results[i];
		const arch_register_t    *reg           = slot->reg0;
		assert(slot->reg1 == NULL);
		in[p]   = new_res_value;
		reqs[p] = reg->single_req;
		++p;
	}
	/* connect callee saves with their values at the function begin */
	ir_node *start = get_irg_start(irg);
	for (unsigned i = 0; i < n_callee_saves; ++i) {
		const arch_register_t *reg   = callee_saves[i];
		ir_mode               *mode  = reg->cls->mode;
		unsigned               idx   = start_callee_saves_offset + i;
		ir_node               *value = new_r_Proj(start, mode, idx);
		in[p]   = value;
		reqs[p] = reg->single_req;
		++p;
	}
	assert(p == n_ins);

	ir_node *ret = new_bd_arm_Return(dbgi, new_block, n_ins, in);
	arch_set_irn_register_reqs_in(ret, reqs);
	return ret;
}

static ir_node *gen_Call(ir_node *node)
{
	ir_graph             *irg          = get_irn_irg(node);
	ir_node              *callee       = get_Call_ptr(node);
	ir_node              *new_block    = be_transform_nodes_block(node);
	ir_node              *mem          = get_Call_mem(node);
	ir_node              *new_mem      = be_transform_node(mem);
	dbg_info             *dbgi         = get_irn_dbg_info(node);
	ir_type              *type         = get_Call_type(node);
	calling_convention_t *cconv        = arm_decide_calling_convention(NULL, type);
	size_t                n_params     = get_Call_n_params(node);
	size_t const          n_param_regs = cconv->n_param_regs;
	/* max inputs: memory, stack, callee, register arguments */
	size_t const          max_inputs   = 3 + n_param_regs;
	ir_node             **in           = ALLOCAN(ir_node*, max_inputs);
	ir_node             **sync_ins     = ALLOCAN(ir_node*, n_params);
	arch_register_req_t const **const in_req = be_allocate_in_reqs(irg, max_inputs);
	size_t                in_arity       = 0;
	size_t                sync_arity     = 0;
	size_t const          n_caller_saves = ARRAY_SIZE(caller_saves);
	ir_entity            *entity         = NULL;

	assert(n_params == get_method_n_params(type));

	/* memory input */
	int mem_pos     = in_arity++;
	in_req[mem_pos] = arch_no_register_req;
	/* stack pointer (create parameter stackframe + align stack)
	 * Note that we always need an IncSP to ensure stack alignment */
	ir_node *new_frame = get_stack_pointer_for(node);
	ir_node *incsp     = be_new_IncSP(sp_reg, new_block, new_frame,
	                                  cconv->param_stack_size,
	                                  ARM_PO2_STACK_ALIGNMENT);
	int sp_pos = in_arity++;
	in_req[sp_pos] = sp_reg->single_req;
	in[sp_pos]     = incsp;

	/* parameters */
	for (size_t p = 0; p < n_params; ++p) {
		ir_node                  *value      = get_Call_param(node, p);
		ir_node                  *new_value  = be_transform_node(value);
		ir_node                  *new_value1 = NULL;
		const reg_or_stackslot_t *param      = &cconv->parameters[p];
		ir_type                  *param_type = get_method_param_type(type, p);
		ir_mode                  *mode       = get_type_mode(param_type);

		if (mode_is_float(mode) && param->reg0 != NULL) {
			unsigned size_bits = get_mode_size_bits(mode);
			if (size_bits == 64) {
				double_to_ints(dbgi, new_block, new_value, &new_value,
				               &new_value1);
			} else {
				assert(size_bits == 32);
				new_value = float_to_int(dbgi, new_block, new_value);
			}
		}

		/* put value into registers */
		if (param->reg0 != NULL) {
			in[in_arity]     = new_value;
			in_req[in_arity] = param->reg0->single_req;
			++in_arity;
			if (new_value1 == NULL)
				continue;
		}
		if (param->reg1 != NULL) {
			assert(new_value1 != NULL);
			in[in_arity]     = new_value1;
			in_req[in_arity] = param->reg1->single_req;
			++in_arity;
			continue;
		}

		/* we need a store if we're here */
		if (new_value1 != NULL) {
			new_value = new_value1;
			mode      = arm_mode_gp;
		}

		/* create a parameter frame if necessary */
		ir_node *str;
		if (mode_is_float(mode)) {
			str = new_bd_arm_Stf(dbgi, new_block, incsp, new_value, new_mem,
			                     mode, NULL, 0, param->offset, true);
		} else {
			str = new_bd_arm_Str(dbgi, new_block, incsp, new_value, new_mem,
								 mode, NULL, 0, param->offset, true);
		}
		sync_ins[sync_arity++] = str;
	}

	/* construct memory input */
	if (sync_arity == 0) {
		in[mem_pos] = new_mem;
	} else if (sync_arity == 1) {
		in[mem_pos] = sync_ins[0];
	} else {
		in[mem_pos] = new_r_Sync(new_block, sync_arity, sync_ins);
	}

	/* TODO: use a generic address matcher here */
	unsigned shiftop_input = 0;
	if (is_Address(callee)) {
		entity = get_Address_entity(callee);
	} else {
		/* TODO: finish load matcher here */
		shiftop_input    = in_arity;
		in[in_arity]     = be_transform_node(callee);
		in_req[in_arity] = arm_reg_classes[CLASS_arm_gp].class_req;
		++in_arity;
	}
	assert(sync_arity <= n_params);
	assert(in_arity <= max_inputs);

	/* Count outputs. */
	unsigned const out_arity = pn_arm_Bl_first_result + n_caller_saves;

	ir_node *res;
	if (entity != NULL) {
		/* TODO: use a generic address matcher here
		 * so we can also handle entity+offset, etc. */
		res = new_bd_arm_Bl(dbgi, new_block, in_arity, in, out_arity,entity, 0);
	} else {
		/* TODO:
		 * - use a proper shifter_operand matcher
		 * - we could also use LinkLdrPC
		 */
		res = new_bd_arm_LinkMovPC(dbgi, new_block, in_arity, in, out_arity,
		                           shiftop_input, ARM_SHF_REG, 0, 0);
	}

	arch_set_irn_register_reqs_in(res, in_req);

	/* create output register reqs */
	arch_set_irn_register_req_out(res, pn_arm_Bl_M, arch_no_register_req);
	arch_copy_irn_out_info(res, pn_arm_Bl_stack, incsp);

	for (size_t o = 0; o < n_caller_saves; ++o) {
		const arch_register_t *reg = caller_saves[o];
		arch_set_irn_register_req_out(res, pn_arm_Bl_first_result + o, reg->single_req);
	}

	/* copy pinned attribute */
	set_irn_pinned(res, get_irn_pinned(node));

	/* IncSP to destroy the call stackframe */
	ir_node *const call_stack = new_r_Proj(res, arm_mode_gp, pn_arm_Bl_stack);
	incsp = be_new_IncSP(sp_reg, new_block, call_stack, -cconv->param_stack_size, 0);
	/* if we are the last IncSP producer in a block then we have to keep
	 * the stack value.
	 * Note: This here keeps all producers which is more than necessary */
	keep_alive(incsp);

	pmap_insert(node_to_stack, node, incsp);

	arm_free_calling_convention(cconv);
	return res;
}

static ir_node *gen_Member(ir_node *node)
{
	dbg_info  *dbgi      = get_irn_dbg_info(node);
	ir_node   *new_block = be_transform_nodes_block(node);
	ir_node   *ptr       = get_Member_ptr(node);
	ir_node   *new_ptr   = be_transform_node(ptr);
	ir_entity *entity    = get_Member_entity(node);

	/* must be the frame pointer all other sels must have been lowered
	 * already */
	assert(is_Proj(ptr) && is_Start(get_Proj_pred(ptr)));

	return new_bd_arm_FrameAddr(dbgi, new_block, new_ptr, entity, 0);
}

static ir_node *gen_Phi(ir_node *node)
{
	ir_mode                   *mode = get_irn_mode(node);
	const arch_register_req_t *req;
	if (get_mode_arithmetic(mode) == irma_twos_complement) {
		/* we shouldn't have any 64bit stuff around anymore */
		assert(get_mode_size_bits(mode) <= 32);
		/* all integer operations are on 32bit registers now */
		req  = arm_reg_classes[CLASS_arm_gp].class_req;
	} else {
		req = arch_no_register_req;
	}

	return be_transform_phi(node, req);
}

/**
 * Enters all transform functions into the generic pointer
 */
static void arm_register_transformers(void)
{
	be_start_transform_setup();

	be_set_transform_function(op_Add,         gen_Add);
	be_set_transform_function(op_Address,     gen_Address);
	be_set_transform_function(op_And,         gen_And);
	be_set_transform_function(op_arm_AdC_t,   gen_arm_AdC_t);
	be_set_transform_function(op_arm_AddS_t,  gen_arm_AddS_t);
	be_set_transform_function(op_arm_OrPl_t,  gen_arm_OrPl_t);
	be_set_transform_function(op_arm_SbC_t,   gen_arm_SbC_t);
	be_set_transform_function(op_arm_SubS_t,  gen_arm_SubS_t);
	be_set_transform_function(op_arm_UMulL_t, gen_arm_UMulL_t);
	be_set_transform_function(op_Builtin,     gen_Builtin);
	be_set_transform_function(op_Call,        gen_Call);
	be_set_transform_function(op_Cmp,         gen_Cmp);
	be_set_transform_function(op_Cond,        gen_Cond);
	be_set_transform_function(op_Const,       gen_Const);
	be_set_transform_function(op_Conv,        gen_Conv);
	be_set_transform_function(op_CopyB,       gen_CopyB);
	be_set_transform_function(op_Div,         gen_Div);
	be_set_transform_function(op_Eor,         gen_Eor);
	be_set_transform_function(op_Jmp,         gen_Jmp);
	be_set_transform_function(op_Load,        gen_Load);
	be_set_transform_function(op_Member,      gen_Member);
	be_set_transform_function(op_Minus,       gen_Minus);
	be_set_transform_function(op_Mul,         gen_Mul);
	be_set_transform_function(op_Not,         gen_Not);
	be_set_transform_function(op_Or,          gen_Or);
	be_set_transform_function(op_Phi,         gen_Phi);
	be_set_transform_function(op_Return,      gen_Return);
	be_set_transform_function(op_Shl,         gen_Shl);
	be_set_transform_function(op_Shr,         gen_Shr);
	be_set_transform_function(op_Shrs,        gen_Shrs);
	be_set_transform_function(op_Start,       gen_Start);
	be_set_transform_function(op_Store,       gen_Store);
	be_set_transform_function(op_Sub,         gen_Sub);
	be_set_transform_function(op_Switch,      gen_Switch);
	be_set_transform_function(op_Unknown,     gen_Unknown);

	be_set_transform_proj_function(op_arm_AddS_t,  gen_Proj_arm_AddS_t);
	be_set_transform_proj_function(op_arm_SubS_t,  gen_Proj_arm_SubS_t);
	be_set_transform_proj_function(op_arm_UMulL_t, gen_Proj_arm_UMulL_t);
	be_set_transform_proj_function(op_Builtin,     gen_Proj_Builtin);
	be_set_transform_proj_function(op_Call,        gen_Proj_Call);
	be_set_transform_proj_function(op_Cond,        be_duplicate_node);
	be_set_transform_proj_function(op_Div,         gen_Proj_Div);
	be_set_transform_proj_function(op_Load,        gen_Proj_Load);
	be_set_transform_proj_function(op_Proj,        gen_Proj_Proj);
	be_set_transform_proj_function(op_Start,       gen_Proj_Start);
	be_set_transform_proj_function(op_Store,       gen_Proj_Store);
	be_set_transform_proj_function(op_Switch,      be_duplicate_node);
}

/**
 * Initialize fpa Immediate support.
 */
static void arm_init_fpa_immediate(void)
{
	/* 0, 1, 2, 3, 4, 5, 10, or 0.5. */
	fpa_imm[FPA_IMM_FLOAT][fpa_null]  = get_mode_null(mode_F);
	fpa_imm[FPA_IMM_FLOAT][fpa_one]   = get_mode_one(mode_F);
	fpa_imm[FPA_IMM_FLOAT][fpa_two]   = new_tarval_from_str("2", 1, mode_F);
	fpa_imm[FPA_IMM_FLOAT][fpa_three] = new_tarval_from_str("3", 1, mode_F);
	fpa_imm[FPA_IMM_FLOAT][fpa_four]  = new_tarval_from_str("4", 1, mode_F);
	fpa_imm[FPA_IMM_FLOAT][fpa_five]  = new_tarval_from_str("5", 1, mode_F);
	fpa_imm[FPA_IMM_FLOAT][fpa_ten]   = new_tarval_from_str("10", 2, mode_F);
	fpa_imm[FPA_IMM_FLOAT][fpa_half]  = new_tarval_from_str("0.5", 3, mode_F);

	fpa_imm[FPA_IMM_DOUBLE][fpa_null]  = get_mode_null(mode_D);
	fpa_imm[FPA_IMM_DOUBLE][fpa_one]   = get_mode_one(mode_D);
	fpa_imm[FPA_IMM_DOUBLE][fpa_two]   = new_tarval_from_str("2", 1, mode_D);
	fpa_imm[FPA_IMM_DOUBLE][fpa_three] = new_tarval_from_str("3", 1, mode_D);
	fpa_imm[FPA_IMM_DOUBLE][fpa_four]  = new_tarval_from_str("4", 1, mode_D);
	fpa_imm[FPA_IMM_DOUBLE][fpa_five]  = new_tarval_from_str("5", 1, mode_D);
	fpa_imm[FPA_IMM_DOUBLE][fpa_ten]   = new_tarval_from_str("10", 2, mode_D);
	fpa_imm[FPA_IMM_DOUBLE][fpa_half]  = new_tarval_from_str("0.5", 3, mode_D);
}

/**
 * Transform a Firm graph into an ARM graph.
 */
void arm_transform_graph(ir_graph *irg)
{
	assure_irg_properties(irg, IR_GRAPH_PROPERTY_NO_TUPLES
	                         | IR_GRAPH_PROPERTY_NO_BADS);

	mode_fp = arm_reg_classes[CLASS_arm_fpa].mode;

	static bool imm_initialized = false;
	if (!imm_initialized) {
		arm_init_fpa_immediate();
		imm_initialized = true;
	}
	arm_register_transformers();

	node_to_stack = pmap_create();

	assert(cconv == NULL);
	stackorder = be_collect_stacknodes(irg);
	ir_entity *entity = get_irg_entity(irg);
	cconv = arm_decide_calling_convention(irg, get_entity_type(entity));
	create_stacklayout(irg);
	be_add_parameter_entity_stores(irg);

	be_transform_graph(irg, NULL);

	be_free_stackorder(stackorder);
	stackorder = NULL;

	arm_free_calling_convention(cconv);
	cconv = NULL;

	ir_type *frame_type = get_irg_frame_type(irg);
	if (get_type_state(frame_type) == layout_undefined) {
		default_layout_compound_type(frame_type);
	}

	pmap_destroy(node_to_stack);
	node_to_stack = NULL;
}

void arm_init_transform(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.arm.transform");
}
