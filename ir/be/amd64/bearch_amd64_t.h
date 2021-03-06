/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   declarations for AMD64 backend -- private header
 */
#ifndef FIRM_BE_AMD64_BEARCH_AMD64_T_H
#define FIRM_BE_AMD64_BEARCH_AMD64_T_H

#include "bearch.h"
#include "../ia32/x86_cconv.h"

extern pmap *amd64_constants; /**< A map of entities that store const tarvals */

extern ir_mode *amd64_mode_E;
extern ir_type *amd64_type_E;
extern ir_mode *amd64_mode_xmm;

extern bool amd64_use_x64_abi;

#define AMD64_REGISTER_SIZE   8
/** power of two stack alignment on calls */
#define AMD64_PO2_STACK_ALIGNMENT 4

/**
 * Determine how function parameters and return values are passed.
 * Decides what goes to register or to stack and what stack offsets/
 * datatypes are used.
 *
 * @param function_type  the type of the caller/callee function
 * @param caller         true for convention for the caller, false for callee
 */
x86_cconv_t *amd64_decide_calling_convention(ir_type *function_type,
                                             ir_graph *irg);

void amd64_cconv_init(void);

#endif
