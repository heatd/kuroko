#include <limits.h>
#include <string.h>
#include "memory.h"
#include "value.h"
#include "object.h"
#include "vm.h"

void krk_initValueArray(KrkValueArray * array) {
	array->values = NULL;
	array->capacity = 0;
	array->count = 0;
}

void krk_writeValueArray(KrkValueArray * array, KrkValue value) {
	if (array->capacity < array->count + 1) {
		int old = array->capacity;
		array->capacity = GROW_CAPACITY(old);
		array->values = GROW_ARRAY(KrkValue, array->values, old, array->capacity);
	}

	array->values[array->count] = value;
	array->count++;
}

void krk_freeValueArray(KrkValueArray * array) {
	FREE_ARRAY(KrkValue, array->values, array->capacity);
	krk_initValueArray(array);
}

void krk_printValue(FILE * f, KrkValue printable) {
	if (!IS_OBJECT(printable)) {
		switch (printable.type) {
			case VAL_INTEGER:  fprintf(f, "%ld", AS_INTEGER(printable)); break;
			case VAL_BOOLEAN:  fprintf(f, "%s", AS_BOOLEAN(printable) ? "True" : "False"); break;
			case VAL_FLOATING: fprintf(f, "%g", AS_FLOATING(printable)); break;
			case VAL_NONE:     fprintf(f, "None"); break;
			case VAL_HANDLER:  fprintf(f, "{try->%ld}", AS_HANDLER(printable)); break;
			case VAL_KWARGS: {
				if (AS_INTEGER(printable) == LONG_MAX) {
					fprintf(f, "{unpack single}");
				} else if (AS_INTEGER(printable) == LONG_MAX-1) {
					fprintf(f, "{unpack list}");
				} else if (AS_INTEGER(printable) == LONG_MAX-2) {
					fprintf(f, "{unpack dict}");
				} else if (AS_INTEGER(printable) == 0) {
					fprintf(f, "{unset default}");
				} else {
					fprintf(f, "{sentinel=%ld}",AS_INTEGER(printable));
				}
				break;
			}
			default: break;
		}
		return;
	}
	krk_push(printable);
	if (krk_bindMethod(AS_CLASS(krk_typeOf(1,(KrkValue[]){printable})), AS_STRING(vm.specialMethodNames[METHOD_REPR]))) {
		switch (krk_callValue(krk_peek(0), 0, 0)) {
			case 2: printable = krk_pop(); break;
			case 1: printable = krk_runNext(); break;
			default: fprintf(f, "[unable to print object at address %p]", (void*)AS_OBJECT(printable)); return;
		}
		fprintf(f, "%s", AS_CSTRING(printable));
	} else {
		krk_pop();
		fprintf(f, "%s", krk_typeName(printable));
	}
}

void krk_printValueSafe(FILE * f, KrkValue printable) {
	if (!IS_OBJECT(printable)) {
		krk_printValue(f,printable);
	} else if (IS_STRING(printable)) {
		fprintf(f, "\"%s\"", AS_CSTRING(printable));
	} else {
		switch (AS_OBJECT(printable)->type) {
			case OBJ_FUNCTION: fprintf(f, "<function %s>", AS_FUNCTION(printable)->name ? AS_FUNCTION(printable)->name->chars : "?"); break;
			case OBJ_CLASS: fprintf(f, "<class %s>", AS_CLASS(printable)->name ? AS_CLASS(printable)->name->chars : "?"); break;
			case OBJ_INSTANCE: fprintf(f, "<instance of %s>", AS_INSTANCE(printable)->_class->name->chars); break;
			case OBJ_NATIVE: fprintf(f, "<nativefn %s>", ((KrkNative*)AS_OBJECT(printable))->name); break;
			case OBJ_CLOSURE: fprintf(f, "<function %s>", AS_CLOSURE(printable)->function->name->chars); break;
			case OBJ_TUPLE: {
				fprintf(f, "<tuple (");
				for (size_t i = 0; i < AS_TUPLE(printable)->values.count; ++i) {
					krk_printValueSafe(f, AS_TUPLE(printable)->values.values[i]);
				}
				fprintf(f, ")>");
			} break;
			case OBJ_BOUND_METHOD: fprintf(f, "<method %s>",
				AS_BOUND_METHOD(printable)->method ? (
				AS_BOUND_METHOD(printable)->method->type == OBJ_CLOSURE ? ((KrkClosure*)AS_BOUND_METHOD(printable)->method)->function->name->chars :
					((KrkNative*)AS_BOUND_METHOD(printable)->method)->name) : "(corrupt bound method)"); break;
			default: fprintf(f, "<%s>", krk_typeName(printable)); break;
		}
	}
}

int krk_valuesEqual(KrkValue a, KrkValue b) {
	if (a.type != b.type) return 0; /* uh, maybe not, this is complicated */

	switch (a.type) {
		case VAL_BOOLEAN:  return AS_BOOLEAN(a) == AS_BOOLEAN(b);
		case VAL_NONE:     return 1; /* None always equals None */
		case VAL_KWARGS:   /* Equal if same number of args; may be useful for comparing sentinels (0) to arg lists. */
		case VAL_INTEGER:  return AS_INTEGER(a) == AS_INTEGER(b);
		case VAL_FLOATING: return AS_FLOATING(a) == AS_FLOATING(b);
		case VAL_HANDLER: {
			fprintf(stderr, "Attempted to compare a value to an exception handler. VM leaked a stack value.\n");
			exit(1);
		}
		case VAL_OBJECT: {
			if (IS_STRING(a) && IS_STRING(b)) return AS_OBJECT(a) == AS_OBJECT(b);
			/* If their pointers are equal, assume they are always equivalent */
			if (IS_TUPLE(a) && IS_TUPLE(b)) {
				if (AS_TUPLE(a)->values.count != AS_TUPLE(b)->values.count) return 0;
				for (size_t i = 0; i < AS_TUPLE(a)->values.count; ++i) {
					if (!krk_valuesEqual(AS_TUPLE(a)->values.values[i], AS_TUPLE(b)->values.values[i])) return 0;
				}
				return 1;
			}
			if (AS_OBJECT(a) == AS_OBJECT(b)) return 1;
			/* TODO: __eq__ */
			return 0;
		}
		default: return 0;
	}
}
