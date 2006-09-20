/**
 * Internal backend global data structures.
 * @author Sebastian Hack
 * @date 8.12.2004
 */

#ifndef _BE_T_H
#define _BE_T_H

#include "firm_types.h"
#include "obst.h"
#include "debug.h"
#include "bitset.h"

#include "be.h"
#include "bearch.h"
#include "beirgmod.h"
#include "be_dbgout.h"

#define DUMP_NONE       0
#define DUMP_INITIAL    (1 << 0)
#define DUMP_ABI        (1 << 1)
#define DUMP_SCHED      (1 << 2)
#define DUMP_PREPARED   (1 << 3)
#define DUMP_RA         (1 << 4)
#define DUMP_FINAL      (1 << 5)
#define DUMP_BE         (1 << 6)

enum {
	BE_TIME_OFF,
	BE_TIME_ON
};

enum {
	BE_VRFY_OFF,
	BE_VRFY_WARN,
	BE_VRFY_ASSERT
};

enum {
	BE_SCHED_SELECT_TRIVIAL  = 0,
	BE_SCHED_SELECT_REGPRESS = 1,
	BE_SCHED_SELECT_MUCHNIK  = 2,
	BE_SCHED_SELECT_HEUR     = 3,
	BE_SCHED_SELECT_HMUCHNIK = 4,
	BE_SCHED_SELECT_RANDOM   = 5
};

enum {
	BE_SCHED_PREP_NONE = 0,
	BE_SCHED_PREP_MRIS = 2,
	BE_SCHED_PREP_RSS  = 3
};

/** Backend options */
struct _be_options_t {
	int  dump_flags;          /**< backend dumping flags */
	int  timing;              /**< time the backend phases */
	int  sched_select;        /**< the current scheduler */
	int  sched_prep;
	int  opt_profile;         /**< instrument code for profiling */
	int  mris;                /**< enable mris preparation */
	int  omit_fp;             /**< try to omit the frame pointer */
	int  stabs_debug_support; /**< enable stabs debugging support */
	int  vrfy_option;         /**< backend verify option */
	char ilp_server[128];     /**< the ilp server name */
	char ilp_solver[128];     /**< the ilp solver name */
};

struct _be_main_env_t {
	struct obstack obst;
	struct _be_node_factory_t *node_factory;
	struct _arch_env_t *arch_env;
	struct _be_options_t *options;
	struct _arch_code_generator_t *cg;
	struct _arch_irn_handler_t *phi_handler;
	dbg_handle *db_handle;
	DEBUG_ONLY(firm_dbg_module_t *dbg;)
};

struct _be_irg_t {
	ir_graph                      *irg;
	struct _be_main_env_t         *main_env;
	struct _be_abi_irg_t          *abi;
	struct _arch_code_generator_t *cg;
	exec_freq_t *execfreqs;
};

/**
* Put the registers to be ignored in this IRG into a bitset.
* @param birg The backend IRG data structure.
* @param cls  The register class.
* @param bs   The bitset (may be NULL).
* @return The number of registers to be ignored.
*/
int be_put_ignore_regs(const struct _be_irg_t *birg, const struct _arch_register_class_t *cls, bitset_t *bs);



#endif /* _BE_T_H */
