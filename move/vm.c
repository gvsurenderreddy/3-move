#include "global.h"
#include "object.h"
#include "method.h"
#include "barrier.h"
#include "gc.h"
#include "vm.h"
#include "bytecode.h"
#include "slot.h"
#include "prim.h"
#include "perms.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#if 0
#define DEBUG
#endif

#define O_CAN_SOMETHING(x,w,f)	(PERMS_CAN(x,(f)&O_WORLD_MASK) || \
				 ((w) == (x)->owner && PERMS_CAN(x,(f)&O_OWNER_MASK)) || \
				 (in_group(w, (x)->group) && PERMS_CAN(x,(f)&O_GROUP_MASK)) || \
				 PRIVILEGEDP(w))
#define O_CAN_R(x,w)	O_CAN_SOMETHING(x,w,O_ALL_R)
#define O_CAN_W(x,w)	O_CAN_SOMETHING(x,w,O_ALL_W)
#define O_CAN_X(x,w)	O_CAN_SOMETHING(x,w,O_ALL_X)
#define O_CAN_C(x,w)	O_CAN_SOMETHING(x,w,O_ALL_C)

#define MS_CAN_SOMETHING(x,w,f)	((NUM(AT(x, ME_FLAGS))&(f)&O_WORLD_MASK) || \
				 ((w) == (OBJECT)AT(x, ME_OWNER) && \
				  (NUM(AT(x, ME_FLAGS))&(f)&O_OWNER_MASK)) || \
				 PRIVILEGEDP(w))
#define MS_CAN_R(x,w)	MS_CAN_SOMETHING(x,w,O_ALL_R)
#define MS_CAN_W(x,w)	MS_CAN_SOMETHING(x,w,O_ALL_W)
#define MS_CAN_X(x,w)	MS_CAN_SOMETHING(x,w,O_ALL_X)
#define MS_CAN_C(x,w)	MS_CAN_SOMETHING(x,w,O_ALL_C)

/************************************************************************/
/* Private parts							*/

PRIVATE pthread_mutex_t vm_count_mutex;
PRIVATE pthread_cond_t vm_count_signal;
PRIVATE int vm_count;

PRIVATE int gc_thread_should_exit;
PRIVATE pthread_mutex_t gc_enabler;

PRIVATE struct barrier b_need_gc;
PRIVATE struct barrier b_gc_complete;

#define VMSTACKLENGTH	1024

#define CODEAT(i)	AT(vms->r->vm_code, i)
#define CODE16AT(i)	(((u16) CODEAT(i) << 8) | (u16) CODEAT((i)+1))

#define PUSH(o)		push(vms, o)
#define POP()		pop(vms)
#define PEEK()		peek(vms)

PRIVATE INLINE void push(VMSTATE vms, OBJ o) {
  if (vms->c.vm_top >= VMSTACKLENGTH) {
    vms->c.vm_top = 0;
    vms->c.vm_state = 0;
  }

  ATPUT(vms->r->vm_stack, vms->c.vm_top, o);
  vms->c.vm_top++;
}

PRIVATE INLINE OBJ pop(VMSTATE vms) {
  if (vms->c.vm_top <= 0){
    vms->c.vm_top = 1;
    vms->c.vm_state = 0;
  }

  vms->c.vm_top--;
  return AT(vms->r->vm_stack, vms->c.vm_top);
}

PRIVATE INLINE OBJ peek(VMSTATE vms) {
  return AT(vms->r->vm_stack, vms->c.vm_top - 1);
}

PRIVATE INLINE void fillframe(VMSTATE vms, OVECTOR f, u32 newip) {
  ATPUT(f, FR_CODE, (OBJ) vms->r->vm_code);
  ATPUT(f, FR_IP, MKNUM(newip));
  ATPUT(f, FR_SELF, (OBJ) vms->r->vm_self);
  ATPUT(f, FR_LITS, (OBJ) vms->r->vm_lits);
  ATPUT(f, FR_ENV, (OBJ) vms->r->vm_env);
  ATPUT(f, FR_FRAME, (OBJ) vms->r->vm_frame);
  ATPUT(f, FR_METHOD, (OBJ) vms->r->vm_method);
  ATPUT(f, FR_EFFUID, (OBJ) vms->r->vm_effuid);
}

PRIVATE INLINE void restoreframe(VMSTATE vms, OVECTOR f) {
  vms->r->vm_code = (BVECTOR) AT(f, FR_CODE);
  vms->c.vm_ip = NUM(AT(f, FR_IP));
  vms->r->vm_self = (OBJECT) AT(f, FR_SELF);
  vms->r->vm_lits = (VECTOR) AT(f, FR_LITS);
  vms->r->vm_env = (VECTOR) AT(f, FR_ENV);
  vms->r->vm_frame = (OVECTOR) AT(f, FR_FRAME);
  vms->r->vm_method = (OVECTOR) AT(f, FR_METHOD);
  vms->r->vm_effuid = (OBJECT) AT(f, FR_EFFUID);
}

PRIVATE INLINE void apply_closure(VMSTATE vms, OVECTOR closure, VECTOR argvec) {
  if (closure == NULL || TAGGEDP(closure)) {
    vm_raise(vms, (OBJ) newsym("invalid-callable"), (OBJ) closure);
  } else if (closure->type == T_PRIM) {
    prim_fn fnp = lookup_prim(NUM(AT(closure, PR_NUMBER)));

    if (fnp != NULL)
      vms->r->vm_acc = fnp(vms, argvec);

    if (vms->r->vm_frame != NULL)
      restoreframe(vms, vms->r->vm_frame);
  } else if (closure->type == T_CLOSURE) {
    OVECTOR meth = (OVECTOR) AT(closure, CL_METHOD);

    if (!MS_CAN_X(meth, vms->r->vm_effuid)) {
      vm_raise(vms, (OBJ) newsym("no-permission"), NULL);
      return;
    }

    vms->r->vm_env = argvec;
    ATPUT(vms->r->vm_env, 0, AT(meth, ME_ENV));

    vms->r->vm_lits = (VECTOR) AT(meth, ME_LITS);
    vms->r->vm_code = (BVECTOR) AT(meth, ME_CODE);
    vms->r->vm_self = (OBJECT) AT(closure, CL_SELF);
    vms->c.vm_ip = 0;
    vms->r->vm_method = meth;
    if (NUM(AT(meth, ME_FLAGS)) & O_SETUID)
      vms->r->vm_effuid = (OBJECT) AT(meth, ME_OWNER);
  } else {
    vm_raise(vms, (OBJ) newsym("invalid-callable"), (OBJ) closure);
  }
}

#ifdef DEBUG
#define INSTR0(n)	printf(n); ip++; break
#define INSTR1(n)	printf(n "%d", c[ip+1]); ip+=2; break
#define INSTR2(n)	printf(n "%d %d", c[ip+1], c[ip+2]); ip+=3; break
#define INSTR16(n)	printf(n "%d", (i16) ((int)c[ip+1]*256 + c[ip+2])); ip+=3; break
PRIVATE void debug_dump_instr(byte *c, int ip) {
  printf("%04d ", ip);

  switch (c[ip]) {
    case OP_AT:			INSTR1("AT      ");
    case OP_ATPUT:		INSTR1("ATPUT   ");
    case OP_MOV_A_LOCL:		INSTR2("A<-LOCL ");
    case OP_MOV_A_GLOB:		INSTR1("A<-GLOB ");
    case OP_MOV_A_SLOT:		INSTR1("A<-SLOT ");
    case OP_MOV_A_LITL:		INSTR1("A<-LITL ");
    case OP_MOV_A_SELF:		INSTR0("A<-SELF ");
    case OP_MOV_A_FRAM:		INSTR0("A<-FRAM ");
    case OP_MOV_LOCL_A:		INSTR2("LOCL<-A ");
    case OP_MOV_GLOB_A:		INSTR1("GLOB<-A ");
    case OP_MOV_SLOT_A:		INSTR1("SLOT<-A ");
    case OP_MOV_FRAM_A:		INSTR0("FRAM<-A ");
    case OP_PUSH:		INSTR0("PUSH    ");
    case OP_POP:		INSTR0("POP     ");
    case OP_SWAP:		INSTR0("SWAP    ");
    case OP_VECTOR:		INSTR1("VECTOR  ");
    case OP_ENTER_SCOPE:	INSTR1("ENTER   ");
    case OP_LEAVE_SCOPE:	INSTR0("LEAVE   ");
    case OP_MAKE_VECTOR:	INSTR1("MKVECT  ");
    case OP_FRAME:		INSTR16("FRAME   ");
    case OP_CLOSURE:		INSTR0("CLOSURE ");
    case OP_METHOD_CLOSURE:	INSTR1("METHCLS ");
    case OP_RET:		INSTR0("RETURN  ");
    case OP_CALL:		INSTR1("CALL    ");
    case OP_CALL_AS:		INSTR1("CALLAS  ");
    case OP_APPLY:		INSTR0("APPLY   ");
    case OP_JUMP:		INSTR16("J       ");
    case OP_JUMP_TRUE:		INSTR16("JT      ");
    case OP_JUMP_FALSE:		INSTR16("JF      ");
    case OP_NOT:		INSTR0("NOT     ");
    case OP_EQ:			INSTR0("EQ      ");
    case OP_NE:			INSTR0("NE      ");
    case OP_GT:			INSTR0("GT      ");
    case OP_LT:			INSTR0("LT      ");
    case OP_GE:			INSTR0("GE      ");
    case OP_LE:			INSTR0("LE      ");
    case OP_NEG:		INSTR0("NEG     ");
    case OP_BNOT:		INSTR0("BNOT    ");
    case OP_BOR:		INSTR0("BOR     ");
    case OP_BAND:		INSTR0("BAND    ");
    case OP_PLUS:		INSTR0("PLUS    ");
    case OP_MINUS:		INSTR0("MINUS   ");
    case OP_STAR:		INSTR0("STAR    ");
    case OP_SLASH:		INSTR0("SLASH   ");
    case OP_PERCENT:		INSTR0("PERCENT ");
    default:
      printf("Unknown instr: %d", c[ip]); break;
  }
  printf("\n");
}
#endif

/************************************************************************/
/* Public parts								*/

PUBLIC void init_vm_global(void) {
  pthread_mutex_init(&vm_count_mutex, NULL);
  pthread_cond_init(&vm_count_signal, NULL);
  vm_count = 0;

  gc_thread_should_exit = 0;
  pthread_mutex_init(&gc_enabler, NULL);

  barrier_init(&b_need_gc, 0);
  barrier_init(&b_gc_complete, 0);
}

PUBLIC void *vm_gc_thread_main(void *arg) {
  barrier_inc_threshold(&b_need_gc);
  barrier_inc_threshold(&b_gc_complete);

  while (!gc_thread_should_exit) {
    pthread_mutex_lock(&vm_count_mutex);
    while (vm_count <= 0) {
#ifdef DEBUG
      printf("%d sleeping on vm_count\n", pthread_self());
#endif
      pthread_cond_wait(&vm_count_signal, &vm_count_mutex);
#ifdef DEBUG
      printf("(%d WOKE UP vm_count)\n", pthread_self());
#endif
    }
    pthread_mutex_unlock(&vm_count_mutex);

    barrier_hit(&b_need_gc);
    pthread_mutex_lock(&gc_enabler);
    gc();
    pthread_mutex_unlock(&gc_enabler);
    barrier_hit(&b_gc_complete);
  }

  return NULL;
}

PUBLIC void make_gc_thread_exit(void) {
  gc_thread_should_exit = 1;
}

PUBLIC void gc_inc_safepoints(void) {
  pthread_mutex_lock(&vm_count_mutex);
#ifdef DEBUG
  printf("%d inc ", pthread_self());
#endif
  barrier_inc_threshold(&b_need_gc);
  barrier_inc_threshold(&b_gc_complete);
  vm_count++;
  pthread_cond_broadcast(&vm_count_signal);
#ifdef DEBUG
  printf("done\n");
#endif
  pthread_mutex_unlock(&vm_count_mutex);
}

PUBLIC void gc_dec_safepoints(void) {
  pthread_mutex_lock(&vm_count_mutex);
#ifdef DEBUG
  printf("%d dec ", pthread_self());
#endif
  vm_count--;
  barrier_dec_threshold(&b_need_gc);
  barrier_dec_threshold(&b_gc_complete);
#ifdef DEBUG
  printf("done\n");
#endif
  pthread_mutex_unlock(&vm_count_mutex);
}

PUBLIC void gc_reach_safepoint(void) {
#ifdef DEBUG
  printf("%d reach ", pthread_self());
#endif
  if (need_gc()) {
#ifdef DEBUG
    printf(" yes ");
#endif
    barrier_hit(&b_need_gc);
    barrier_hit(&b_gc_complete);
  }
#ifdef DEBUG
  printf("done\n");
#endif
}

PUBLIC void init_vm(VMSTATE vms) {
  vms->r->vm_acc = NULL;
  vms->r->vm_code = NULL;
  vms->r->vm_env = NULL;
  vms->r->vm_lits = NULL;
  vms->r->vm_self = NULL;
  vms->r->vm_stack = NULL;
  vms->r->vm_frame = NULL;
  vms->r->vm_method = NULL;
  vms->c.vm_ip = vms->c.vm_top = 0;
  vms->r->vm_trap_closure = NULL;
  vms->r->vm_uid = NULL;
  vms->r->vm_effuid = NULL;
  vms->r->vm_input = NULL;
  vms->r->vm_output = NULL;
  vms->c.vm_state = 1;
}

PUBLIC void vm_raise(VMSTATE vms, OBJ exception, OBJ arg) {
  if (vms->r->vm_trap_closure != NULL) {
    VECTOR argvec = newvector_noinit(5);
    OVECTOR vm_state = newovector(FR_MAXSLOTINDEX, T_FRAME);

    fillframe(vms, vm_state, vms->c.vm_ip);

    ATPUT(argvec, 0, NULL);
    ATPUT(argvec, 1, exception);
    ATPUT(argvec, 2, arg);
    ATPUT(argvec, 3, (OBJ) vm_state);
    ATPUT(argvec, 4, vms->r->vm_acc);

    apply_closure(vms, vms->r->vm_trap_closure, argvec);
    vms->r->vm_frame = NULL;	/* If it ever returns, the thread dies. */
  } else {
    fprintf(stderr, "excp sym = '%s\n", ((BVECTOR) AT((OVECTOR) exception, SY_NAME))->vec);
    fprintf(stderr,
	    "Exception raised, no handler installed -> vm death.\n");
    vms->c.vm_state = 0;
  }
}

PRIVATE OBJ make_closure_from(OVECTOR method, OBJECT self, VECTOR env) {
  OVECTOR nmeth = newovector(ME_MAXSLOTINDEX, T_METHOD);
  OVECTOR nclos = newovector(CL_MAXSLOTINDEX, T_CLOSURE);
  int i;

  for (i = 0; i < ME_MAXSLOTINDEX; i++)
    ATPUT(nmeth, i, AT(method, i));

  ATPUT(nmeth, ME_ENV, (OBJ) env);
  ATPUT(nclos, CL_METHOD, (OBJ) nmeth);
  ATPUT(nclos, CL_SELF, (OBJ) self);
  return (OBJ) nclos;
}

PUBLIC BVECTOR bvector_concat(BVECTOR a, BVECTOR b) {
  BVECTOR n = newbvector_noinit(a->_.length + b->_.length);

  memcpy(n->vec, a->vec, a->_.length);
  memcpy(n->vec + a->_.length, b->vec, b->_.length);

  return n;
}

PUBLIC VECTOR vector_concat(VECTOR a, VECTOR b) {
  int alen = a->_.length;
  int blen = b->_.length;
  VECTOR n = newvector(alen + blen);
  int i, j;

  for (i = 0; i < alen; i++)
    ATPUT(n, i, AT(a, i));

  for (i = alen, j = 0; j < blen; i++, j++)
    ATPUT(n, i, AT(b, j));

  return n;
}

#define NUMOP(lab, exp) \
      case lab: \
	if (!NUMP(vms->r->vm_acc) || !NUMP(PEEK())) { \
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc); \
	  break; \
	} \
	exp ; \
	vms->c.vm_ip++; \
	break

#define NOPERMISSION()	vm_raise(vms, (OBJ) newsym("no-permission"), NULL); break

PUBLIC void run_vm(VMSTATE vms, OBJECT self, OVECTOR method) {
  OBJ vm_hold;	/* Holding register. NOT SEEN BY GC */

  gc_inc_safepoints();

  vms->r->vm_acc = NULL;
  vms->r->vm_code = (BVECTOR) AT(method, ME_CODE);
  vms->c.vm_ip = 0;
  vms->r->vm_env = newvector(1);
  ATPUT(vms->r->vm_env, 0, NULL);
  vms->r->vm_lits = (VECTOR) AT(method, ME_LITS);
  vms->r->vm_self = self;
  vms->r->vm_stack = newvector(VMSTACKLENGTH);
  vms->c.vm_top = 0;
  vms->r->vm_frame = NULL;
  vms->r->vm_method = method;
  vms->r->vm_uid = (OBJECT) AT(method, ME_OWNER);
  vms->r->vm_effuid = vms->r->vm_uid;

  while (vms->c.vm_state == 1) {
    if (need_gc()) {
#ifdef DEBUG
      printf("thr %d hit need_gc\n", pthread_self()); fflush(stdout);
#endif
      barrier_hit(&b_need_gc);
#ifdef DEBUG
      printf("thr %d hit gc_complete\n", pthread_self()); fflush(stdout);
#endif
      barrier_hit(&b_gc_complete);
#ifdef DEBUG
      printf("thr %d left gc fire\n", pthread_self()); fflush(stdout);
      printf("vms->r == %p, vms->r->vm_code == %p\n",
	     vms->r,
	     vms->r ? vms->r->vm_code : NULL);
#endif
    }

#ifdef DEBUG
    debug_dump_instr( vms->r->vm_code->vec , vms->c.vm_ip );
#endif

    switch (CODEAT(vms->c.vm_ip)) {
      case OP_AT: {
	int index = CODEAT(vms->c.vm_ip + 1);

	if (index < 0 || index >= vms->r->vm_acc->length) {
	  vm_raise(vms, (OBJ) newsym("range-check-error"), vms->r->vm_acc);
	  break;
	}

	if (!VECTORP(vms->r->vm_acc)) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}

	vms->r->vm_acc = AT((VECTOR) vms->r->vm_acc, index);
	vms->c.vm_ip += 2;
	break;
      }

      case OP_ATPUT: {
	int index = CODEAT(vms->c.vm_ip + 1);

	vm_hold = PEEK();

	if (index < 0 || index >= vm_hold->length) {
	  vm_raise(vms, (OBJ) newsym("range-check-error"), vm_hold);
	  break;
	}

	if (!VECTORP(vm_hold)) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vm_hold);
	  break;
	}

	ATPUT((VECTOR) vm_hold, index, vms->r->vm_acc);
	vms->c.vm_ip += 2;
	break;
      }

      case OP_MOV_A_LOCL: {
	int i = CODEAT(vms->c.vm_ip + 1);
	vm_hold = (OBJ) vms->r->vm_env;
	while (i-- > 0) vm_hold = AT((VECTOR) vm_hold, 0);
	vms->r->vm_acc = AT((VECTOR) vm_hold, CODEAT(vms->c.vm_ip + 2) + 1);
	vms->c.vm_ip += 3;
	break;
      }

      case OP_MOV_A_GLOB:
	vm_hold = AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	vms->r->vm_acc = AT((OVECTOR) vm_hold, SY_VALUE);
	vms->c.vm_ip += 2;
	break;

      case OP_MOV_A_SLOT: {
	OVECTOR slot, slotname;

	if (!OBJECTP(vms->r->vm_acc)) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}

	if (!O_CAN_X((OBJECT) vms->r->vm_acc, vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	slotname = (OVECTOR) AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	slot = findslot((OBJECT) vms->r->vm_acc, slotname, NULL);

	if (slot == NULL) {
	  vm_raise(vms, (OBJ) newsym("slot-not-found"), (OBJ) slotname);
	  break;
	}

	if (!MS_CAN_R(slot, vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	vms->r->vm_acc = AT(slot, SL_VALUE);
	vms->c.vm_ip += 2;
	break;
      }

      case OP_MOV_A_LITL:
	vms->r->vm_acc = AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	vms->c.vm_ip += 2;
	break;

      case OP_MOV_A_SELF: vms->r->vm_acc = (OBJ) vms->r->vm_self; vms->c.vm_ip++; break;
      case OP_MOV_A_FRAM: vms->r->vm_acc = (OBJ) vms->r->vm_frame; vms->c.vm_ip++; break;

      case OP_MOV_LOCL_A: {
	int i = CODEAT(vms->c.vm_ip + 1);
	vm_hold = (OBJ) vms->r->vm_env;
	while (i-- > 0) vm_hold = AT((VECTOR) vm_hold, 0);
	ATPUT((VECTOR) vm_hold, CODEAT(vms->c.vm_ip + 2) + 1, vms->r->vm_acc);
	vms->c.vm_ip += 3;
	break;
      }

      case OP_MOV_GLOB_A:
	if (vms->r->vm_effuid != NULL && !PRIVILEGEDP(vms->r->vm_effuid)) {
	  NOPERMISSION();
	}
	vm_hold = AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	ATPUT((OVECTOR) vm_hold, SY_VALUE, vms->r->vm_acc);
	vms->c.vm_ip += 2;
	break;

      case OP_MOV_SLOT_A: {
	OVECTOR slot, slotname;
	OBJECT target = (OBJECT) POP();
	OBJECT foundin;

	if (!OBJECTP(target)) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), (OBJ) target);
	  break;
	}

	if (!O_CAN_X(target, vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	slotname = (OVECTOR) AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	slot = findslot(target, slotname, &foundin);

	if (slot == NULL) {
	  vm_raise(vms, (OBJ) newsym("slot-not-found"), (OBJ) slotname);
	  break;
	}

	if (!MS_CAN_W(slot, vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	if (foundin == target) {
	  ATPUT(slot, SL_VALUE, vms->r->vm_acc);
	} else {
	  OVECTOR newslot = addslot(target, slotname, (OBJECT) AT(slot, SL_OWNER));
	  ATPUT(newslot, SL_FLAGS, AT(slot, SL_FLAGS));
	  ATPUT(newslot, SL_VALUE, vms->r->vm_acc);
	}

	vms->c.vm_ip += 2;
	break;
      }

      case OP_MOV_FRAM_A:
	if (!PRIVILEGEDP(vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	if (!OVECTORP(vms->r->vm_acc) || ((OVECTOR) vms->r->vm_acc)->type != T_FRAME) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}

	vms->r->vm_frame = (OVECTOR) vms->r->vm_acc;
	vms->c.vm_ip++;
	break;

      case OP_PUSH: PUSH(vms->r->vm_acc); vms->c.vm_ip++; break;
      case OP_POP: vms->r->vm_acc = POP(); vms->c.vm_ip++; break;
      case OP_SWAP:
	vm_hold = POP();
	PUSH(vms->r->vm_acc);
	vms->r->vm_acc = vm_hold;
	vms->c.vm_ip++;
	break;

      case OP_VECTOR:
	vms->r->vm_acc = (OBJ) newvector(CODEAT(vms->c.vm_ip+1));
	vms->c.vm_ip += 2;
	break;
	
      case OP_ENTER_SCOPE:
	vm_hold = (OBJ) newvector(CODEAT(vms->c.vm_ip+1) + 1);
	ATPUT((VECTOR) vm_hold, 0, (OBJ) vms->r->vm_env);
	vms->r->vm_env = (VECTOR) vm_hold;
	vms->c.vm_ip += 2;
	break;

      case OP_LEAVE_SCOPE:
	vms->r->vm_env = (VECTOR) AT(vms->r->vm_env, 0);
	vms->c.vm_ip++;
	break;

      case OP_MAKE_VECTOR: {
	int i = 0;
	int len = CODEAT(vms->c.vm_ip+1);
	VECTOR vec = newvector_noinit(len);

	for (i = len - 1; i >= 0; i--)
	  ATPUT(vec, i, POP());

	vms->r->vm_acc = (OBJ) vec;
	vms->c.vm_ip += 2;
	break;
      }

      case OP_FRAME:
	vm_hold = (OBJ) newovector(FR_MAXSLOTINDEX, T_FRAME);
	fillframe(vms, (OVECTOR) vm_hold, vms->c.vm_ip + 3 + CODE16AT(vms->c.vm_ip+1));
	vms->r->vm_frame = (OVECTOR) vm_hold;
	vms->c.vm_ip += 3;
	break;

      case OP_CLOSURE:
	vms->r->vm_acc = make_closure_from((OVECTOR) vms->r->vm_acc,
					   vms->r->vm_self,
					   vms->r->vm_env);
	vms->c.vm_ip++;
	break;

      case OP_METHOD_CLOSURE: {
	OVECTOR methname = (OVECTOR) AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	OVECTOR method;

	if (!OBJECTP(vms->r->vm_acc)) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}

	method = findmethod((OBJECT) vms->r->vm_acc, methname);

	if (method == NULL) {
	  vm_raise(vms, (OBJ) newsym("method-not-found"), (OBJ) methname);
	  break;
	}

	if (!MS_CAN_R(method, vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	vm_hold = (OBJ) newovector(CL_MAXSLOTINDEX, T_CLOSURE);
	ATPUT((OVECTOR) vm_hold, CL_METHOD, (OBJ) method);
	ATPUT((OVECTOR) vm_hold, CL_SELF, vms->r->vm_acc);
	vms->r->vm_acc = vm_hold;

	vms->c.vm_ip += 2;
	break;
      }

      case OP_RET:
	if (vms->r->vm_frame == NULL) {
	  gc_dec_safepoints();
	  return;
	}
	restoreframe(vms, vms->r->vm_frame);
	break;
	
      case OP_CALL: {
	OVECTOR methname = (OVECTOR) AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	OVECTOR method;

	if (vms->r->vm_acc == NULL || TAGGEDP(vms->r->vm_acc)) {
	  vm_raise(vms,
		   (OBJ) newsym("null-call-error"),
		   AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip+1)));
	  break;
	}

	if (!OBJECTP(vms->r->vm_acc)) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}

	method = findmethod((OBJECT) vms->r->vm_acc, methname);

	if (method == NULL) {
	  vm_raise(vms, (OBJ) newsym("method-not-found"), (OBJ) methname);
	  break;
	}

	if (!MS_CAN_X(method, vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	vms->r->vm_env = (VECTOR) POP();
	ATPUT(vms->r->vm_env, 0, AT(method, ME_ENV));
	vms->r->vm_code = (BVECTOR) AT(method, ME_CODE);
	vms->r->vm_lits = (VECTOR) AT(method, ME_LITS);
	vms->r->vm_self = (OBJECT) vms->r->vm_acc;
	if (NUM(AT(method, ME_FLAGS)) & O_SETUID)
	  vms->r->vm_effuid = (OBJECT) AT(method, ME_OWNER);
	vms->r->vm_method = method;
	vms->c.vm_ip = 0;
	break;
      }

      case OP_CALL_AS: {
	OVECTOR methname = (OVECTOR) AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip + 1));
	OVECTOR method;

	if (vms->r->vm_self == NULL ||
	    vms->r->vm_acc == NULL || TAGGEDP(vms->r->vm_acc)) {
	  vm_raise(vms,
		   (OBJ) newsym("null-call-error"),
		   AT(vms->r->vm_lits, CODEAT(vms->c.vm_ip+1)));
	  break;
	}

	if (!OBJECTP(vms->r->vm_acc)) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}

	method = findmethod((OBJECT) vms->r->vm_acc, methname);

	if (method == NULL) {
	  vm_raise(vms, (OBJ) newsym("method-not-found"), (OBJ) methname);
	  break;
	}

	if (!MS_CAN_X(method, vms->r->vm_effuid)) {
	  NOPERMISSION();
	}

	vms->r->vm_env = (VECTOR) POP();
	ATPUT(vms->r->vm_env, 0, AT(method, ME_ENV));
	vms->r->vm_code = (BVECTOR) AT(method, ME_CODE);
	vms->r->vm_lits = (VECTOR) AT(method, ME_LITS);

	/* don't set vm_self, this is OP_CALL_AS. */
	/* vms->r->vm_self = vms->r->vm_acc; */

	if (NUM(AT(method, ME_FLAGS)) & O_SETUID)
	  vms->r->vm_effuid = (OBJECT) AT(method, ME_OWNER);
	vms->r->vm_method = method;
	vms->c.vm_ip = 0;
	break;
      }

      case OP_APPLY:
	vms->c.vm_ip++;
	apply_closure(vms, (OVECTOR) vms->r->vm_acc, (VECTOR) POP());
	break;

      case OP_JUMP: vms->c.vm_ip += 3 + ((i16) CODE16AT(vms->c.vm_ip+1)); break;

      case OP_JUMP_TRUE:
	vms->c.vm_ip += (vms->r->vm_acc == false) ? 3 :
						    3 + ((i16) CODE16AT(vms->c.vm_ip+1));
	break;

      case OP_JUMP_FALSE:
	vms->c.vm_ip += (vms->r->vm_acc != false) ? 3 :
						    3 + ((i16) CODE16AT(vms->c.vm_ip+1));
	break;

      case OP_NOT:
	vms->r->vm_acc = (vms->r->vm_acc == false) ? true : false;
	vms->c.vm_ip++;
	break;

      case OP_EQ:
	vms->r->vm_acc = (vms->r->vm_acc == POP()) ? true : false;
	vms->c.vm_ip++;
	break;

      case OP_NE:
	vms->r->vm_acc = (vms->r->vm_acc != POP()) ? true : false;
	vms->c.vm_ip++;
	break;

      NUMOP(OP_GT, vms->r->vm_acc = (NUM(vms->r->vm_acc) < NUM(POP())) ? true : false);
      NUMOP(OP_LT, vms->r->vm_acc = (NUM(vms->r->vm_acc) > NUM(POP())) ? true : false);
      NUMOP(OP_GE, vms->r->vm_acc = (NUM(vms->r->vm_acc) <= NUM(POP())) ? true : false);
      NUMOP(OP_LE, vms->r->vm_acc = (NUM(vms->r->vm_acc) >= NUM(POP())) ? true : false);

      NUMOP(OP_NEG, vms->r->vm_acc = MKNUM(-NUM(vms->r->vm_acc)));
      NUMOP(OP_BNOT, vms->r->vm_acc = MKNUM(~NUM(vms->r->vm_acc)));
      NUMOP(OP_BOR, vms->r->vm_acc = MKNUM(NUM(vms->r->vm_acc)|NUM(POP())));
      NUMOP(OP_BAND, vms->r->vm_acc = MKNUM(NUM(vms->r->vm_acc)&NUM(POP())));

      case OP_PLUS:
	if (vms->r->vm_acc == NULL || PEEK() == NULL) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}
	if (NUMP(vms->r->vm_acc) && NUMP(PEEK()))
	  vms->r->vm_acc = MKNUM(NUM(vms->r->vm_acc)+NUM(POP()));
	else if (TAGGEDP(vms->r->vm_acc) || TAGGEDP(PEEK())) {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	} else if (BVECTORP(vms->r->vm_acc) && BVECTORP(PEEK()))
	  vms->r->vm_acc = (OBJ) bvector_concat((BVECTOR) POP(), (BVECTOR) vms->r->vm_acc);
	else if (VECTORP(vms->r->vm_acc) && VECTORP(PEEK()))
	  vms->r->vm_acc = (OBJ) vector_concat((VECTOR) POP(), (VECTOR) vms->r->vm_acc);
	else {
	  vm_raise(vms, (OBJ) newsym("vm-runtime-type-error"), vms->r->vm_acc);
	  break;
	}
	vms->c.vm_ip++;
	break;

      NUMOP(OP_MINUS, vms->r->vm_acc = MKNUM(NUM(POP())-NUM(vms->r->vm_acc)));
      NUMOP(OP_STAR, vms->r->vm_acc = MKNUM(NUM(POP())*NUM(vms->r->vm_acc)));
      NUMOP(OP_SLASH, vms->r->vm_acc = MKNUM(NUM(POP())/NUM(vms->r->vm_acc)));
      NUMOP(OP_PERCENT, vms->r->vm_acc = MKNUM(NUM(POP())%NUM(vms->r->vm_acc)));

      default:
	fprintf(stderr, "Unknown bytecode reached (%d == 0x%x).\n",
		CODEAT(vms->c.vm_ip),
		CODEAT(vms->c.vm_ip));
	exit(1);
    }
  }

  gc_dec_safepoints();
}
