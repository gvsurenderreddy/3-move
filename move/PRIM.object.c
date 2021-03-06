/*
 * 3-MOVE, a multi-user networked online text-based programmable virtual environment
 * Copyright 1997, 1998, 1999, 2003, 2005, 2008, 2009 Tony Garnock-Jones <tonyg@kcbbs.gen.nz>
 *
 * 3-MOVE is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * 3-MOVE is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with 3-MOVE.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "global.h"
#include "object.h"
#include "vm.h"
#define DEFINING_MOVE_PRIMITIVES
#include "prim.h"
#include "scanner.h"
#include "parser.h"
#include "conn.h"
#include "perms.h"
#include "slot.h"
#include "method.h"
#include "pair.h"
#include "hashtable.h"
#include "thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define NOPERMISSION()	vm_raise(vms, (OBJ) newsym("no-permission"), (OBJ) argvec); \
			return undefined

PRIVATE void setup_ancestor(void) {
  OVECTOR ansym = newsym("Root");

  if (AT(ansym, SY_VALUE) == undefined) {
    OBJECT ancestor = newobject(NULL, NULL);
    ATPUT(ansym, SY_VALUE, (OBJ) ancestor);
  }
}

PRIVATE INLINE int check_clonable(OBJECT effuid, OBJECT o) {
  if (o->flags & O_C_FLAG)
    return 1;

  if (effuid == o->owner)
    return 1;

  if (PRIVILEGEDP(effuid))
    return 1;

  return 0;
}

DEFPRIM(cloneFun) {
  OBJ p = ARG(0);
  OBJECT result;

  TYPEERRIF(!(OBJECTP(p) || VECTORP(p)));

  if (VECTORP(p)) {
    int i;

    for (i = 0; i < p->length; i++) {
      TYPEERRIF(!OBJECTP(AT((VECTOR) p, i)));
      if (!check_clonable(vms->r->vm_effuid, (OBJECT) AT((VECTOR) p, i)))
	return false;
    }
  } else {
    if (!check_clonable(vms->r->vm_effuid, (OBJECT) p))
      return false;
  }

  result = newobject(p, vms->r->vm_effuid);

  return (OBJ) result;
}

DEFPRIM(privFun) {
  OBJ x = ARG(0);
  TYPEERRIF(!OBJECTP(x));
  return (x != NULL && PRIVILEGEDP((OBJECT) x)) ? true : false;
}

DEFPRIM(setPrivFun) {
  OBJ x = ARG(0);
  OBJ v = ARG(1);

  TYPEERRIF(!OBJECTP(x));

  if (!PRIVILEGEDP(vms->r->vm_effuid))
    return false;

  if (v == false)
    ((OBJECT) x)->flags &= ~O_PRIVILEGED;
  else
    ((OBJECT) x)->flags |= O_PRIVILEGED;

  return true;
}

DEFPRIM(getOFlagsFun) {
  OBJ x = ARG(0);
  TYPEERRIF(!OBJECTP(x));
  return MKNUM(((OBJECT) x)->flags);
}

DEFPRIM(setOFlagsFun) {
  OBJ x = ARG(0);
  OBJ f = ARG(1);
  OBJECT ox = (OBJECT) x;

  TYPEERRIF(!OBJECTP(x) || !NUMP(f));

  if (vms->r->vm_effuid == ox->owner || PRIVILEGEDP(vms->r->vm_effuid)) {
    ox->flags &= ~(O_PERMS_MASK | O_C_FLAG);
    ox->flags |= (NUM(f) & (O_PERMS_MASK | O_C_FLAG));
    return true;
  }

  return false;
}

DEFPRIM(ownerFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  TYPEERRIF(!OBJECTP(x));
  return (OBJ) ox->owner;
}

DEFPRIM(setOwnerFun) {
  OBJ x = ARG(0);
  OBJ o = ARG(1);
  OBJECT ox = (OBJECT) x;

  TYPEERRIF(!OBJECTP(x) || !OBJECTP(o));

  if (PRIVILEGEDP(vms->r->vm_effuid)) {
    ox->owner = (OBJECT) o;
    return true;
  } else
    return false;
}

DEFPRIM(groupFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  TYPEERRIF(!OBJECTP(x));
  if (!PRIVILEGEDP(vms->r->vm_effuid))
    return false;
  return (OBJ) ox->group;
}

DEFPRIM(setGroupFun) {
  OBJ x = ARG(0);
  OBJ g = ARG(1);
  OBJECT ox = (OBJECT) x;

  TYPEERRIF(!OBJECTP(x) || !VECTORP(g));

  if (!PRIVILEGEDP(vms->r->vm_effuid))
    return false;

  ox->group = (VECTOR) g;
  return true;
}

DEFPRIM(parentsFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  TYPEERRIF(!OBJECTP(x));
  if (VECTORP(ox->parents))
    return (OBJ) vector_clone((VECTOR) ox->parents);
  else {
    VECTOR result = newvector_noinit(1);
    ATPUT(result, 0, ox->parents);
    return (OBJ) result;
  }
}

DEFPRIM(setParentsFun) {
  OBJ x = ARG(0);
  OBJ p = ARG(1);
  OBJECT ox = (OBJECT) x;

  TYPEERRIF(!OBJECTP(x) || !(OBJECTP(p) || VECTORP(p)));

  if (!PRIVILEGEDP(vms->r->vm_effuid))
    return false;

  if (VECTORP(p)) {
    VECTOR vp = (VECTOR) p;
    int i;

    for (i = 0; i < p->length; i++) {
      TYPEERRIF(!OBJECTP(AT(vp, i)));
      if (!check_clonable(vms->r->vm_effuid, (OBJECT) AT(vp, i)))
	return false;
    }
  } else {
    if (!check_clonable(vms->r->vm_effuid, (OBJECT) p))
      return false;
  }

  ox->parents = p;
  return true;
}

DEFPRIM(lockObjFun) {
  OBJ x = ARG(0);

  TYPEERRIF(!OBJECTP(x));

  if (!PRIVILEGEDP(vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  if (vms->c.vm_locked_count > 0 && vms->r->vm_locked != (OBJECT) x) {
    vm_raise(vms, (OBJ) newsym("multiple-locks-disallowed"), x);
    return undefined;
  }

  vms->r->vm_locked = (OBJECT) x;
  vms->c.vm_locked_count++;
  if (LOCK((OBJECT) x) < 0) {
    block_thread(BLOCK_CTXT_LOCKING, x);
    return yield_thread;
  } else
    return true;
}

DEFPRIM(unlockObjFun) {
  OBJ x = ARG(0);

  TYPEERRIF(!OBJECTP(x));

  if (!PRIVILEGEDP(vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  if (vms->r->vm_locked != (OBJECT) x)
    return false;

  vms->c.vm_locked_count--;
  if (vms->c.vm_locked_count <= 0) {
    vms->r->vm_locked = NULL;
    vms->c.vm_locked_count = 0;
  }
  UNLOCK((OBJECT) x);

  return true;
}

DEFPRIM(slotRefFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  OBJ n = ARG(1);
  OVECTOR name = (OVECTOR) n;
  OVECTOR slot;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_X(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  slot = findslot(ox, name, NULL);

  if (slot == NULL)
    return undefined;

  if (!MS_CAN_R(slot, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  return AT(slot, SL_VALUE);
}

DEFPRIM(slotSetFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  OBJ n = ARG(1);
  OBJ val = ARG(2);
  OVECTOR name = (OVECTOR) n;
  OVECTOR slot;
  OBJECT foundin;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_X(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  slot = findslot(ox, name, &foundin);

  if (slot == NULL) {
    vm_raise(vms, (OBJ) newsym("slot-not-found"), (OBJ) name);
    return undefined;
  }

  if (!MS_CAN_W(slot, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  if (foundin == ox) {
    ATPUT(slot, SL_VALUE, val);
  } else {
    OVECTOR newslot = addslot(ox, name, (OBJECT) AT(slot, SL_OWNER));
    ATPUT(newslot, SL_FLAGS, AT(slot, SL_FLAGS));
    ATPUT(newslot, SL_VALUE, val);
  }

  return val;
}

DEFPRIM(addSlotFun) {
  OBJ x = ARG(0);
  OBJ n = ARG(1);
  OBJ val = ARG(2);
  OBJECT ox = (OBJECT) x;
  OVECTOR name = (OVECTOR) n;
  OVECTOR slot;
  OBJECT foundin;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_W(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  slot = findslot(ox, name, &foundin);

  if (slot != NULL && !MS_CAN_C(slot, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  if (slot == NULL || foundin != ox) {
    OVECTOR newslot = addslot(ox, name, vms->r->vm_effuid);
    ATPUT(newslot, SL_VALUE, val);
  } else {
    ATPUT(slot, SL_OWNER, (OBJ) vms->r->vm_effuid);
    ATPUT(slot, SL_VALUE, val);
  }

  return val;
}

DEFPRIM(slotOwnerFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  OBJ n = ARG(1);
  OVECTOR name = (OVECTOR) n;
  OVECTOR slot;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_X(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  slot = findslot(ox, name, NULL);

  if (slot == NULL) {
    vm_raise(vms, (OBJ) newsym("slot-not-found"), (OBJ) name);
    return undefined;
  }

  return AT(slot, SL_OWNER);
}

DEFPRIM(setSlotOwnerFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  OBJ n = ARG(1);
  OBJ newown = ARG(2);
  OBJECT newowner = (OBJECT) newown;
  OVECTOR name = (OVECTOR) n;
  OVECTOR slot;
  OBJECT foundin;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL) || !OBJECTP(newown));

  if (!O_CAN_X(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  slot = findslot(ox, name, &foundin);

  if (slot == NULL) {
    vm_raise(vms, (OBJ) newsym("slot-not-found"), (OBJ) name);
    return undefined;
  }

#if 0
  if (vms->r->vm_effuid != (OBJECT) AT(slot, SL_OWNER)) {
    NOPERMISSION();
  }
#endif
  if (!PRIVILEGEDP(vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  if (foundin == ox) {
    ATPUT(slot, SL_OWNER, (OBJ) newowner);
  } else {
    OVECTOR newslot = addslot(ox, name, newowner);
    ATPUT(newslot, SL_FLAGS, AT(slot, SL_FLAGS));
    ATPUT(newslot, SL_VALUE, AT(slot, SL_VALUE));
  }

  return true;
}

DEFPRIM(slotFlagsFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  OBJ n = ARG(1);
  OVECTOR name = (OVECTOR) n;
  OVECTOR slot;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_X(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  slot = findslot(ox, name, NULL);

  if (slot == NULL) {
    vm_raise(vms, (OBJ) newsym("slot-not-found"), (OBJ) name);
    return undefined;
  }

  return AT(slot, SL_FLAGS);
}

DEFPRIM(setSlotFlagsFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  OBJ n = ARG(1);
  OBJ flags = ARG(2);
  OVECTOR name = (OVECTOR) n;
  OVECTOR slot;
  OBJECT foundin;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL) || !NUMP(flags));

  if (!O_CAN_X(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  slot = findslot(ox, name, &foundin);

  if (slot == NULL) {
    vm_raise(vms, (OBJ) newsym("slot-not-found"), (OBJ) name);
    return undefined;
  }

  if (!(vms->r->vm_effuid == (OBJECT) AT(slot, SL_OWNER) || PRIVILEGEDP(vms->r->vm_effuid))) {
    NOPERMISSION();
  }

  if (foundin == ox) {
    ATPUT(slot, SL_FLAGS, MKNUM(NUM(flags) & (O_PERMS_MASK | O_C_FLAG)));
  } else {
    OVECTOR newslot = addslot(ox, name, (OBJECT) AT(slot, SL_OWNER));
    ATPUT(newslot, SL_FLAGS, MKNUM(NUM(flags) & (O_PERMS_MASK | O_C_FLAG)));
    ATPUT(newslot, SL_VALUE, AT(slot, SL_VALUE));
  }

  return true;
}

PRIVATE VECTOR contents_remove(VECTOR c, OBJECT o) {
  VECTOR prev = NULL;
  VECTOR curr = c;

  while (curr != NULL) {
    if (CAR(curr) == (OBJ) o) {
      if (prev == NULL)
	return (VECTOR) CDR(curr);

      SETCDR(prev, CDR(curr));
      return c;
    }

    prev = curr;
    curr = (VECTOR) CDR(curr);
  }

  return c;
}

DEFPRIM(moveToFun) {
  OBJ x = ARG(0);
  OBJ loc = ARG(1);
  OBJECT obj = (OBJECT) x;
  OBJECT dest = (OBJECT) loc;
  OBJECT oldloc;

  TYPEERRIF(!OBJECTP(x) || (loc != NULL && !OBJECTP(loc)));

  if (!PRIVILEGEDP(vms->r->vm_effuid))
    return false;

  oldloc = obj->location;

  if (oldloc != NULL)
    oldloc->contents = contents_remove(oldloc->contents, obj);

  obj->location = dest;

  if (dest != NULL) {
    VECTOR link;
    CONS(link, (OBJ) obj, (OBJ) dest->contents);
    dest->contents = link;
  }

  return true;
}

DEFPRIM(locationFun) {
  OBJ x = ARG(0);
  TYPEERRIF(!OBJECTP(x));
  return (OBJ) ((OBJECT) x)->location;
}

DEFPRIM(contentsFun) {
  OBJ x = ARG(0);
  VECTOR result;

  TYPEERRIF(!OBJECTP(x));

  result = list_to_vector(((OBJECT) x)->contents);
  return (OBJ) result;
}

DEFPRIM(addMethFun) {
  OBJ x = ARG(0);
  OBJ n = ARG(1);
  OBJ val = ARG(2);
  OBJECT ox = (OBJECT) x;
  OVECTOR name = (OVECTOR) n;
  OVECTOR clos = (OVECTOR) val;
  OVECTOR meth;

  TYPEERRIF(!OBJECTP(x) ||
	    !(OVECTORP(n) && name->type == T_SYMBOL) ||
	    !(OVECTORP(val) && clos->type == T_CLOSURE));

  if (!O_CAN_W(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  meth = findmethod(ox, name);

  if (meth != NULL &&
      vms->r->vm_effuid != (OBJECT) AT(meth, ME_OWNER) &&
      !MS_CAN_C(meth, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  addmethod(ox, name, vms->r->vm_effuid, (OVECTOR) AT(clos, CL_METHOD));

  return val;
}

DEFPRIM(methOwnerFun) {
  OBJ c = ARG(0);
  OVECTOR clos = (OVECTOR) c;
  OVECTOR meth;

  TYPEERRIF(!(OVECTORP(c) && clos->type == T_CLOSURE));

  meth = (OVECTOR) AT(clos, CL_METHOD);
  return AT(meth, ME_OWNER);
}

DEFPRIM(setMethOwnerFun) {
  OBJ c = ARG(0);
  OVECTOR clos = (OVECTOR) c;
  OBJ newowner = ARG(1);
  OVECTOR meth;

  TYPEERRIF(!OBJECTP(newowner) || !(OVECTORP(c) && clos->type == T_CLOSURE));

  meth = (OVECTOR) AT(clos, CL_METHOD);

#if 0
  if (vms->r->vm_effuid != (OBJECT) AT(meth, ME_OWNER)) {
    NOPERMISSION();
  }
#endif
  if (!PRIVILEGEDP(vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  ATPUT(meth, ME_OWNER, newowner);
  return true;
}

DEFPRIM(methFlagsFun) {
  OBJ c = ARG(0);
  OVECTOR clos = (OVECTOR) c;
  OVECTOR meth;

  TYPEERRIF(!(OVECTORP(c) && clos->type == T_CLOSURE));

  meth = (OVECTOR) AT(clos, CL_METHOD);
  return AT(meth, ME_FLAGS);
}

DEFPRIM(setMethFlagsFun) {
  OBJ c = ARG(0);
  OVECTOR clos = (OVECTOR) c;
  OBJ newflags = ARG(1);
  unum fl;
  OVECTOR meth;

  TYPEERRIF(!NUMP(newflags) || !(OVECTORP(c) && clos->type == T_CLOSURE));

  meth = (OVECTOR) AT(clos, CL_METHOD);

  if (!(vms->r->vm_effuid == (OBJECT) AT(meth, ME_OWNER) || PRIVILEGEDP(vms->r->vm_effuid))) {
    NOPERMISSION();
  }

  fl = NUM(AT(meth, ME_FLAGS)) & ~(O_PERMS_MASK | O_C_FLAG);
  fl |= NUM(newflags) & (O_PERMS_MASK | O_C_FLAG);
  ATPUT(meth, ME_FLAGS, MKNUM(fl));
  return true;
}

DEFPRIM(methNameFun) {
  OBJ c = ARG(0);
  OVECTOR clos = (OVECTOR) c;
  OVECTOR meth;

  TYPEERRIF(!(OVECTORP(c) && clos->type == T_CLOSURE));

  meth = (OVECTOR) AT(clos, CL_METHOD);
  return AT(meth, ME_NAME);
}

DEFPRIM(getSlotsFun) {
  OBJ o = ARG(0);
  OBJECT obj = (OBJECT) o;
  VECTOR result;

  TYPEERRIF(!OBJECTP(o));

  if (!O_CAN_R(obj, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  result = enumerate_keys(obj->attributes);
  return (OBJ) result;
}

DEFPRIM(getMethodsFun) {
  OBJ o = ARG(0);
  OBJECT obj = (OBJECT) o;
  VECTOR result;

  TYPEERRIF(!OBJECTP(o));

  if (!O_CAN_R(obj, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  result = enumerate_keys(obj->methods);
  return (OBJ) result;
}

PRIVATE int real_isa(OBJECT x, OBJECT y) {
  while (1) {
    if (x == NULL)
      return 0;

    if (x == y)
      return 1;

    if (x->parents == NULL)
      return 0;

    if (OBJECTP(x->parents)) {
      x = (OBJECT) x->parents;
      continue;
    }

    if (VECTORP(x->parents)) {
      int i;
      VECTOR vxp = (VECTOR) x->parents;

      for (i = 0; i < vxp->_.length; i++)
	if (real_isa((OBJECT) AT(vxp, i), y))
	  return 1;

      return 0;
    }

    return 0;
  }
}

DEFPRIM(isaFun) {
  OBJ x = ARG(0);
  OBJ y = ARG(1);
  TYPEERRIF(!OBJECTP(y));
  if (!OBJECTP(x))
    return false;
  return real_isa((OBJECT) x, (OBJECT) y) ? true : false;
}

DEFPRIM(hasSlotFun) {
  OBJ o = ARG(0);
  OBJECT obj = (OBJECT) o;
  OBJ n = ARG(1);
  OVECTOR name = (OVECTOR) n;
  OVECTOR result;

  TYPEERRIF(!OBJECTP(o) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_R(obj, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  result = findslot(obj, name, NULL);
  return result == NULL ? false : true;
}

DEFPRIM(hasMethFun) {
  OBJ o = ARG(0);
  OBJECT obj = (OBJECT) o;
  OBJ n = ARG(1);
  OVECTOR name = (OVECTOR) n;
  OVECTOR result;

  TYPEERRIF(!OBJECTP(o) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_R(obj, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  result = findmethod(obj, name);
  return result == NULL ? false : true;
}

DEFPRIM(slotClearPFun) {
  OBJ o = ARG(0);
  OBJECT obj = (OBJECT) o;
  OBJ n = ARG(1);
  OVECTOR name = (OVECTOR) n;
  OBJECT foundin;
  OVECTOR result;

  TYPEERRIF(!OBJECTP(o) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_R(obj, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  result = findslot(obj, name, &foundin);

  if (result == NULL || foundin != obj)
    return true;

  return false;
}

DEFPRIM(methRefFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;
  OBJ n = ARG(1);
  OVECTOR name = (OVECTOR) n;
  OVECTOR method;
  OVECTOR result;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL));

  method = findmethod(ox, name);

  if (method == NULL)
    return undefined;

  if (!MS_CAN_R(method, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  result = newovector(CL_MAXSLOTINDEX, T_CLOSURE);
  ATPUT(result, CL_METHOD, (OBJ) method);
  ATPUT(result, CL_SELF, x);
  return (OBJ) result;
}

DEFPRIM(stripObjFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;

  TYPEERRIF(!OBJECTP(x));

  if (vms->r->vm_effuid != ox->owner && !PRIVILEGEDP(vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  ox->finalize = 0;
  ox->flags = O_OWNER_MASK;
  ox->methods = newhashtable(19);
  ox->attributes = newhashtable(19);

  return x;
}

DEFPRIM(stripObjMFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;

  TYPEERRIF(!OBJECTP(x));

  if (vms->r->vm_effuid != ox->owner && !PRIVILEGEDP(vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  ox->methods = newhashtable(19);

  return x;
}

DEFPRIM(stripObjSFun) {
  OBJ x = ARG(0);
  OBJECT ox = (OBJECT) x;

  TYPEERRIF(!OBJECTP(x));

  if (vms->r->vm_effuid != ox->owner && !PRIVILEGEDP(vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  ox->attributes = newhashtable(19);

  return x;
}

DEFPRIM(clearSlotFun) {
  OBJ x = ARG(0);
  OBJ n = ARG(1);
  OBJECT ox = (OBJECT) x;
  OVECTOR name = (OVECTOR) n;

  TYPEERRIF(!OBJECTP(x) || !(OVECTORP(n) && name->type == T_SYMBOL));

  if (!O_CAN_W(ox, vms->r->vm_effuid)) {
    NOPERMISSION();
  }

  delslot(ox, name);
  return undefined;
}

PUBLIC void install_PRIM_object(void) {
  setup_ancestor();

  register_prim(1, "clone", 0x04001, cloneFun);
  register_prim(1, "privileged?", 0x04002, privFun);
  register_prim(2, "set-privileged", 0x04003, setPrivFun);
  register_prim(1, "object-flags", 0x04004, getOFlagsFun);
  register_prim(2, "set-object-flags", 0x04005, setOFlagsFun);
  register_prim(1, "owner", 0x04006, ownerFun);
  register_prim(2, "set-owner", 0x04007, setOwnerFun);
  register_prim(1, "group", 0x04008, groupFun);
  register_prim(2, "set-group", 0x04009, setGroupFun);
  register_prim(1, "parents", 0x0400A, parentsFun);
  register_prim(2, "set-parents", 0x0400B, setParentsFun);
  register_prim(1, "lock", 0x0400C, lockObjFun);
  register_prim(1, "unlock", 0x0400D, unlockObjFun);
  register_prim(2, "slot-ref", 0x0400E, slotRefFun);
  register_prim(3, "slot-set", 0x0400F, slotSetFun);
  register_prim(3, "add-slot", 0x04010, addSlotFun);
  register_prim(2, "slot-owner", 0x04011, slotOwnerFun);
  register_prim(3, "set-slot-owner", 0x04012, setSlotOwnerFun);
  register_prim(2, "slot-flags", 0x04013, slotFlagsFun);
  register_prim(3, "set-slot-flags", 0x04014, setSlotFlagsFun);
  register_prim(2, "move-to", 0x04015, moveToFun);
  register_prim(1, "location", 0x04016, locationFun);
  register_prim(1, "contents", 0x04017, contentsFun);
  register_prim(3, "add-method", 0x04018, addMethFun);
  register_prim(1, "method-owner", 0x04019, methOwnerFun);
  register_prim(2, "set-method-owner", 0x0401A, setMethOwnerFun);
  register_prim(1, "method-flags", 0x0401B, methFlagsFun);
  register_prim(2, "set-method-flags", 0x0401C, setMethFlagsFun);
  register_prim(1, "method-name", 0x0401D, methNameFun);
  register_prim(1, "slots", 0x0401D, getSlotsFun);
  register_prim(1, "methods", 0x0401E, getMethodsFun);
  register_prim(2, "isa", 0x0401F, isaFun);
  register_prim(2, "has-slot", 0x04020, hasSlotFun);
  register_prim(2, "has-method", 0x04021, hasMethFun);
  register_prim(2, "slot-clear?", 0x04022, slotClearPFun);
  register_prim(2, "method-ref", 0x04023, methRefFun);
  register_prim(1, "strip-object-clean", 0x04024, stripObjFun);
  register_prim(1, "strip-object-methods", 0x04025, stripObjMFun);
  register_prim(1, "strip-object-slots", 0x04026, stripObjSFun);
  register_prim(2, "clear-slot", 0x04027, clearSlotFun);
}
