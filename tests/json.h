/* A minimal JSON reader for the conformance data. */
#ifndef FHP_TEST_JSON_H
#define FHP_TEST_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } jtype;

typedef struct jvalue jvalue;
struct jvalue {
    jtype type;
    int b;
    double num;
    char *str; /* J_STRING (UTF-8, zero-terminated; may contain \0: see len) */
    size_t len;
    jvalue **items; /* J_ARRAY: values; J_OBJECT: values, keys in keys */
    char **keys;
    size_t *key_lens;
    size_t count;
};

/* NULL on a syntax error. Free with json_free. */
jvalue *json_parse(const char *s, size_t len);
void json_free(jvalue *v);
/* The member of an object, or NULL. */
const jvalue *json_get(const jvalue *obj, const char *key);
/* Writes the value as compact JSON (for messages). */
void json_print(const jvalue *v, char *buf, size_t size);

#endif
