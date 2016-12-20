/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:   woshiguo35@sina.com                                          |
  +----------------------------------------------------------------------+
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_swoole_serialize.h"

static int le_swoole_serialize;
static void swoole_serialize_object(void *buffer, zval *zvalue);
static void swoole_serialize_arr(seriaString *buffer, zend_array *zvalue);
static void* swoole_unserialize_arr(void *buffer, zval *zvalue);
static void* swoole_unserialize_object(void *buffer, zval *return_value, zval *args);

static CPINLINE int swoole_string_new(size_t size, seriaString *str, zend_uchar type)
{
    int total = ZEND_MM_ALIGNED_SIZE(_STR_HEADER_SIZE + size + 1);
    str->total = total;
    //escape the header for later
    str->offset = _STR_HEADER_SIZE;
    //zend string addr
    str->buffer = emalloc(total);
    memset(str->buffer, 0, total);
    if (!str->buffer)
    {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "malloc Error: %s [%d]", strerror(errno), errno);
    }
    //    first 1 bytes is type
    *(zend_uchar*) (str->buffer + str->offset) = type;
    str->offset += sizeof (zend_uchar);
    return 0;
}

static CPINLINE void swoole_check_size(seriaString *str, size_t len)
{

    int new_size = len + str->offset + 3 + sizeof (zend_ulong); //space 1 for the type and 2 for key string len or index len and(zend_ulong) for key h
    if (str->total < new_size)
    {//extend it

        //double size
        new_size = ZEND_MM_ALIGNED_SIZE(new_size + new_size);
        str->buffer = erealloc2(str->buffer, new_size, str->offset);
        if (!str->buffer)
        {
            php_error_docref(NULL TSRMLS_CC, E_ERROR, "realloc Error: %s [%d]", strerror(errno), errno);
        }
        str->total = new_size;
    }

}

static CPINLINE void swoole_string_cpy(seriaString *str, void *mem, size_t len)
{
    swoole_check_size(str, len);
    memcpy(str->buffer + str->offset, mem, len);
    str->offset = len + str->offset;
}

static CPINLINE void swoole_set_zend_value(seriaString *str, void *value)
{
    swoole_check_size(str, sizeof (zend_value));
    *(zend_value*) (str->buffer + str->offset) = *((zend_value*) value);
    str->offset = sizeof (zend_value) + str->offset;
}

/*
 * array
 */

static void* swoole_unserialize_arr(void *buffer, zval *zvalue)
{
    //Initialize zend array
    zend_ulong h, nIndex, max_index = 0;
    ZVAL_NEW_ARR(zvalue);
    seriaArray *seriaArr = (seriaArray*) buffer;
    buffer += sizeof (seriaArray);

    //Initialize buckets
    zend_array *ht = Z_ARR_P(zvalue);
    ht->nTableSize = seriaArr->nTableSize;
    ht->nNumUsed = seriaArr->nNumOfElements;
    ht->nNumOfElements = seriaArr->nNumOfElements;
    ht->nNextFreeElement = 0;
    ht->u.flags = HASH_FLAG_APPLY_PROTECTION;
    ht->nTableMask = -(ht->nTableSize);
    ht->pDestructor = ZVAL_PTR_DTOR;

    GC_REFCOUNT(ht) = 1;
    GC_TYPE_INFO(ht) = IS_ARRAY;
    if (ht->nNumUsed)
    {
        //    void *arData = ecalloc(1, len);
        HT_SET_DATA_ADDR(ht, emalloc(HT_SIZE(ht)));
        ht->u.flags |= HASH_FLAG_INITIALIZED;
        HT_HASH_RESET(ht);
    }


    int idx;
    Bucket *p;
    for (idx = 0; idx < seriaArr->nNumOfElements; idx++)
    {
        SBucketType type = *((SBucketType*) buffer);
        buffer += sizeof (SBucketType);
        p = ht->arData + idx;
        /* Initialize key */
        if (type.key_type == KEY_TYPE_STRING)
        {
            size_t key_len;
            h = *((zend_ulong*) buffer);
            buffer += sizeof (zend_ulong);

            if (type.key_len == 1)
            {
                key_len = *((zend_uchar*) buffer);
                buffer += sizeof (zend_uchar);
            }
            else if (type.key_len == 2)
            {
                key_len = *((unsigned short*) buffer);
                buffer += sizeof (unsigned short);
            }
            else
            {
                key_len = *((size_t*) buffer);
                buffer += sizeof (size_t);
            }
            p->key = zend_string_init((char*) buffer, key_len, 0);
            //            h = zend_inline_hash_func((char*) buffer, key_len);
            p->key->h = p->h = h;
            buffer += key_len;
        }
        else
        {
            if (type.key_len == 1)
            {
                h = *((zend_uchar*) buffer);
                buffer += sizeof (zend_uchar);
            }
            else if (type.key_len == 2)
            {
                h = *((unsigned short*) buffer);
                buffer += sizeof (unsigned short);
            }
            else
            {
                h = *((zend_ulong*) buffer);
                buffer += sizeof (zend_ulong);
            }
            p->h = h;
            p->key = NULL;
            if (h >= max_index)
            {
                max_index = h + 1;
            }
        }

        /* Initialize hash */
        nIndex = h | ht->nTableMask;
        Z_NEXT(p->val) = HT_HASH(ht, nIndex);
        HT_HASH(ht, nIndex) = HT_IDX_TO_HASH(idx);

        /* Initialize data type */
        p->val.u1.v.type = type.data_type;
        Z_TYPE_FLAGS(p->val) = 0;

        /* Initialize data */
        if (type.data_type == IS_STRING)
        {
            size_t data_len;
            if (type.data_len == 1)
            {
                data_len = *((zend_uchar*) buffer);
                buffer += sizeof (zend_uchar);
            }
            else if (type.data_len == 2)
            {
                data_len = *((unsigned short*) buffer);
                buffer += sizeof (unsigned short);
            }
            else
            {
                data_len = *((size_t*) buffer);
                buffer += sizeof (size_t);
            }
            p->val.value.str = zend_string_init((char*) buffer, data_len, 0);
            buffer += data_len;
            Z_TYPE_INFO(p->val) = IS_STRING_EX;
        }
        else if (type.data_type == IS_ARRAY)
        {
            buffer = swoole_unserialize_arr(buffer, &p->val);
        }
        else if (type.data_type == IS_LONG)
        {

            if (type.data_len == 1)
            {
                Z_LVAL(p->val) = *((zend_uchar*) buffer);
                buffer += sizeof (zend_uchar);
            }
            else if (type.data_len == 2)
            {
                Z_LVAL(p->val) = *((unsigned short*) buffer);
                buffer += sizeof (unsigned short);
            }
            else
            {
                p->val.value = *((zend_value*) buffer);
                buffer += sizeof (zend_value);
            }

        }
        else if (type.data_type == IS_DOUBLE)
        {
            p->val.value = *((zend_value*) buffer);
            buffer += sizeof (zend_value);
        }
        else if (type.data_type == IS_UNDEF)
        {
            buffer = swoole_unserialize_object(buffer, &p->val, NULL);
            Z_TYPE_INFO(p->val) = IS_OBJECT_EX;
        }

    }
    ht->nNextFreeElement = max_index;

    return buffer;

}

static void swoole_serialize_arr(seriaString *buffer, zend_array *zvalue)
{
    zval *data;
    zend_string *key;
    zend_ulong index;
    seriaArray seriaArr;
    seriaArr.nTableSize = zvalue->nTableSize; //todo 
    seriaArr.nNumOfElements = zvalue->nNumOfElements;
    swoole_string_cpy(buffer, &seriaArr, sizeof (seriaArray));

    ZEND_HASH_FOREACH_KEY_VAL(zvalue, index, key, data)
    {
        SBucketType type;
        type.data_type = Z_TYPE_P(data);
        //start point
        size_t p = buffer->offset;

        //seria key
        if (key)
        {
            type.key_type = KEY_TYPE_STRING;
            if (key->len <= 0xff)
            {
                type.key_len = 1;
                SERIA_SET_ENTRY_TYPE(buffer, type);
                SERIA_SET_ENTRY_ULONG(buffer, key->h);
                SERIA_SET_ENTRY_TYPE(buffer, key->len);

                swoole_string_cpy(buffer, key->val, key->len);
            }
            else if (key->len <= 0xffff)
            {//if more than this  don't need optimize 
                type.key_len = 2;
                SERIA_SET_ENTRY_TYPE(buffer, type);
                SERIA_SET_ENTRY_ULONG(buffer, key->h);
                SERIA_SET_ENTRY_SHORT(buffer, key->len);

                swoole_string_cpy(buffer, key->val, key->len);
            }
            else
            {
                type.key_len = 0;
                SERIA_SET_ENTRY_TYPE(buffer, type);

                swoole_string_cpy(buffer, key + XtOffsetOf(zend_string, h), sizeof (size_t) + sizeof (zend_ulong) + key->len);
            }

        }
        else
        {
            type.key_type = KEY_TYPE_INDEX;
            if (index <= 0xff)
            {
                type.key_len = 1;
                SERIA_SET_ENTRY_TYPE(buffer, type);
                SERIA_SET_ENTRY_TYPE(buffer, index);
            }
            else if (index <= 0xffff)
            {//if more than this  don't need optimize 
                type.key_len = 2;
                SERIA_SET_ENTRY_TYPE(buffer, type);
                SERIA_SET_ENTRY_SHORT(buffer, index);
            }
            else
            {
                type.key_len = 0;
                SERIA_SET_ENTRY_TYPE(buffer, type);
                SERIA_SET_ENTRY_ULONG(buffer, index);
            }

        }

        //seria data
try_again:
        switch (Z_TYPE_P(data))
        {
            case IS_STRING:
            {
                if (Z_STRLEN_P(data) <= 0xff)
                {
                    ((SBucketType*) (buffer->buffer + p))->data_len = 1;
                    SERIA_SET_ENTRY_TYPE(buffer, Z_STRLEN_P(data));
                    swoole_string_cpy(buffer, Z_STRVAL_P(data), Z_STRLEN_P(data));
                }
                else if (Z_STRLEN_P(data) <= 0xffff)
                {
                    ((SBucketType*) (buffer->buffer + p))->data_len = 2;
                    SERIA_SET_ENTRY_TYPE(buffer, Z_STRLEN_P(data));
                    swoole_string_cpy(buffer, Z_STRVAL_P(data), Z_STRLEN_P(data));
                }
                else
                {
                    ((SBucketType*) (buffer->buffer + p))->data_len = 0;
                    swoole_string_cpy(buffer, (char*) Z_STR_P(data) + XtOffsetOf(zend_string, len), sizeof (size_t) + Z_STRLEN_P(data));
                }
                break;
            }
            case IS_LONG:
            {
                if (Z_LVAL_P(data) <= 0xff)
                {
                    ((SBucketType*) (buffer->buffer + p))->data_len = 1;
                    swoole_check_size(buffer, 1);
                    SERIA_SET_ENTRY_TYPE(buffer, Z_LVAL_P(data));
                }
                else if (Z_LVAL_P(data) <= 0xffff)
                {
                    ((SBucketType*) (buffer->buffer + p))->data_len = 2;
                    swoole_check_size(buffer, 2);
                    SERIA_SET_ENTRY_SHORT(buffer, Z_LVAL_P(data));
                }
                else
                {//if more than this  don't need optimize
                    ((SBucketType*) (buffer->buffer + p))->data_len = 0;
                    swoole_set_zend_value(buffer, &(data->value));
                }
                break;
            }
            case IS_DOUBLE:
                swoole_set_zend_value(buffer, &(data->value));
                break;
            case IS_REFERENCE:
                data = Z_REFVAL_P(data);
                ((SBucketType*) (buffer->buffer + p))->data_type = Z_TYPE_P(data);
                goto try_again;
                break;
            case IS_ARRAY:
            {

                if (ZEND_HASH_GET_APPLY_COUNT(Z_ARRVAL_P(data)) > 1)
                {
                    php_error_docref(NULL TSRMLS_CC, E_NOTICE, "you array have cycle ref");
                }
                else
                {
                    if (ZEND_HASH_APPLY_PROTECTION(Z_ARRVAL_P(data)))
                    {
                        ZEND_HASH_INC_APPLY_COUNT(Z_ARRVAL_P(data));
                        swoole_serialize_arr(buffer, Z_ARRVAL_P(data));
                        ZEND_HASH_DEC_APPLY_COUNT(Z_ARRVAL_P(data));
                    }
                    else
                    {
                        swoole_serialize_arr(buffer, Z_ARRVAL_P(data));
                    }

                }
                break;
            }
                //object propterty table is this type
            case IS_INDIRECT:
                data = Z_INDIRECT_P(data);
                ((SBucketType*) (buffer->buffer + p))->data_type = Z_TYPE_P(data);
                goto try_again;
                break;
            case IS_OBJECT:
            {
                ((SBucketType*) (buffer->buffer + p))->data_type = IS_UNDEF;

                if (ZEND_HASH_APPLY_PROTECTION(Z_OBJPROP_P(data)))
                {
                    ZEND_HASH_INC_APPLY_COUNT(Z_OBJPROP_P(data));
                    swoole_serialize_object(buffer, data);
                    ZEND_HASH_DEC_APPLY_COUNT(Z_OBJPROP_P(data));
                }
                else
                {
                    swoole_serialize_object(buffer, data);
                }

                break;
            }
            default:// check tail space
                swoole_check_size(buffer, 0);
                break;

        }

    }
    ZEND_HASH_FOREACH_END();
}

/*
 * string
 */
static CPINLINE void swoole_serialize_string(seriaString *buffer, zval *zvalue)
{

    swoole_string_cpy(buffer, Z_STRVAL_P(zvalue), Z_STRLEN_P(zvalue));
}

static CPINLINE zend_string* swoole_unserialize_string(void *buffer, size_t len)
{

    return zend_string_init(buffer, len, 0);
}

/*
 * raw
 */

static CPINLINE void swoole_unserialize_raw(void *buffer, zval *zvalue)
{

    memcpy(&zvalue->value, buffer, sizeof (zend_value));
}

/*
 * null
 */

static CPINLINE void swoole_unserialize_null(void *buffer, zval *zvalue)
{

    memcpy(&zvalue->value, buffer, sizeof (zend_value));
}

static CPINLINE void swoole_serialize_raw(seriaString *buffer, zval *zvalue)
{

    swoole_string_cpy(buffer, &zvalue->value, sizeof (zend_value));
}

static void swoole_serialize_object(void *buffer, zval *obj)
{
    zend_string *name = Z_OBJCE_P(obj)->name;
    if (ZEND_HASH_GET_APPLY_COUNT(Z_OBJPROP_P(obj)) > 1)
    {
        zend_throw_exception_ex(NULL, 0, "the object %s have cycle ref!",name->val);
        return;
    }
    zend_class_entry *ce = Z_OBJ_P(obj)->ce;
    swoole_string_cpy(buffer, (char*) name + XtOffsetOf(zend_string, len), sizeof (size_t) + name->len);

    if (ce && zend_hash_exists(&ce->function_table, Z_STR(swSeriaG.sleep_fname)))
    {
        zval retval;
        if (call_user_function_ex(NULL, obj, &swSeriaG.sleep_fname, &retval, 0, 0, 1, NULL) == SUCCESS)
        {
            if (EG(exception))
            {
                zval_dtor(&retval);
                return;
            }
            if (Z_TYPE(retval) == IS_ARRAY)
            {
                zend_string *prop_key;
                zval *prop_value, *sleep_value;
                const char *prop_name, *class_name;
                size_t prop_key_len;
                int got_num = 0;

                //for the zero malloc
                zend_array tmp_arr;
                zend_array *ht = (zend_array *) & tmp_arr;
                _zend_hash_init(ht, zend_hash_num_elements(Z_ARRVAL(retval)), ZVAL_PTR_DTOR, 0 ZEND_FILE_LINE_RELAY_CC);
                ht->nTableMask = -(ht)->nTableSize;
                ALLOCA_FLAG(use_heap);
                void *ht_addr = do_alloca(HT_SIZE(ht), use_heap);
                HT_SET_DATA_ADDR(ht, ht_addr);
                ht->u.flags |= HASH_FLAG_INITIALIZED;
                HT_HASH_RESET(ht);

                //just clean property do not add null when does not exist
                //we double for each, cause we do not malloc  and release it

                ZEND_HASH_FOREACH_STR_KEY_VAL(Z_OBJPROP_P(obj), prop_key, prop_value)
                {
                    //get origin property name
                    zend_unmangle_property_name_ex(prop_key, &class_name, &prop_name, &prop_key_len);

                    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(retval), sleep_value)
                    {
                        if (Z_TYPE_P(sleep_value) == IS_STRING &&
                                Z_STRLEN_P(sleep_value) == prop_key_len &&
                                memcmp(Z_STRVAL_P(sleep_value), prop_name, prop_key_len) == 0)
                        {
                            got_num++;
                            //add mangle key,unmangle in unseria 
                            _zend_hash_add_or_update(ht, prop_key, prop_value, HASH_UPDATE ZEND_FILE_LINE_RELAY_CC);

                            break;
                        }

                    }
                    ZEND_HASH_FOREACH_END();

                }
                ZEND_HASH_FOREACH_END();

                //there some member not in property
                if (zend_hash_num_elements(Z_ARRVAL(retval)) > got_num)
                {
                    php_error_docref(NULL TSRMLS_CC, E_NOTICE, "__sleep() retrun a member but does not exist in property");

                }
                swoole_serialize_arr(buffer, ht);
                ZSTR_ALLOCA_FREE(ht_addr, use_heap);
                zval_dtor(&retval);
                return;

            }
            else
            {
                php_error_docref(NULL TSRMLS_CC, E_NOTICE, " __sleep should return an array only containing the "
                        "names of instance-variables to serialize");
                zval_dtor(&retval);
            }

        }
    }

    swoole_serialize_arr(buffer, Z_OBJPROP_P(obj));
}

/*
 * for the zero malloc
 */
static CPINLINE zend_string *swoole_string_init(const char *str, size_t len)
{
    ALLOCA_FLAG(use_heap);
    zend_string *ret;
    ZSTR_ALLOCA_INIT(ret, str, len, use_heap);

    return ret;
}

/*
 * for the zero malloc
 */
static CPINLINE void swoole_string_release(zend_string *str)
{
    ALLOCA_FLAG(use_heap);
    ZSTR_ALLOCA_FREE(str, use_heap);
}

static CPINLINE zend_class_entry* swoole_try_get_ce(zend_string *class_name)
{
    //user class , do not support incomplete class now
    zend_class_entry *ce = zend_lookup_class(class_name);
    if (ce)
    {
        return ce;
    }
    // try call unserialize callback and retry lookup 
    zval user_func, args[1], retval;
    zend_string *fname = swoole_string_init(PG(unserialize_callback_func), strlen(PG(unserialize_callback_func)));
    Z_STR(user_func) = fname;
    Z_TYPE_INFO(user_func) = IS_STRING_EX;
    ZVAL_STR(&args[0], class_name);

    call_user_function_ex(CG(function_table), NULL, &user_func, &retval, 1, args, 0, NULL);

    swoole_string_release(fname);

    //user class , do not support incomplete class now
    ce = zend_lookup_class(class_name);
    if (!ce)
    {
        zend_throw_exception_ex(NULL, 0, "can not find class %s", class_name->val TSRMLS_CC);
        return NULL;
    }
    else
    {
        return ce;
    }
}

static void* swoole_unserialize_object(void *buffer, zval *return_value, zval *args)
{
    zval property;
    size_t name_len = *((size_t*) buffer);
    buffer += sizeof (size_t);
    zend_string *class_name = swoole_string_init((char*) buffer, name_len);

    zend_class_entry *ce = swoole_try_get_ce(class_name);
    swoole_string_release(class_name);

    if (!ce)
    {
        return;
    }

    buffer += name_len;
    buffer = swoole_unserialize_arr(buffer, &property);

    if (ce->constructor)
    {

        object_init_ex(return_value, ce);

        //        zend_fcall_info fci = {0};
        //        zend_fcall_info_cache fcc = {0};
        //        fci.size = sizeof (zend_fcall_info);
        //        zval retval;
        //        ZVAL_UNDEF(&fci.function_name);
        //        fci.retval = &retval;
        //        fci.param_count = 0;
        //        fci.params = NULL;
        //        fci.no_separation = 1;
        //
        //        zend_fcall_info_args_ex(&fci, ce->constructor, args);
        //
        //        fcc.initialized = 1;
        //        fcc.function_handler = ce->constructor;
        //        fcc.called_scope = ce;
        //        fcc.object = Z_OBJ_P(return_value);
        //        fci.object = Z_OBJ_P(return_value);
        //
        //        if (zend_call_function(&fci, &fcc) == FAILURE)
        //        {
        //            php_error_docref(NULL TSRMLS_CC, E_ERROR, "could not call class constructor");
        //        }
        //        else
        //        {//??? free something?
        //            //cp_zval_ptr_dtor(&args);
        //        }
    }
    else
    {

        object_init_ex(return_value, ce);
    }

    zval *data;
    const zend_string *key;
    zend_ulong index;

    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL(property), index, key, data)
    {

        const char *prop_name, *tmp;
        size_t prop_len;
        zend_unmangle_property_name_ex(key, &tmp, &prop_name, &prop_len);
        zend_update_property(ce, return_value, prop_name, prop_len, data);
    }
    ZEND_HASH_FOREACH_END();

    zval_dtor(&property);

    //call object __wakeup
    if (zend_hash_str_exists(&ce->function_table, "__wakeup", sizeof ("__wakeup") - 1))
    {
        zval ret, wakeup;
        zend_string *fname = swoole_string_init("__wakeup", sizeof ("__wakeup") - 1);
        Z_STR(wakeup) = fname;
        Z_TYPE_INFO(wakeup) = IS_STRING_EX;
        call_user_function_ex(CG(function_table), return_value, &wakeup, &ret, 0, NULL, 1, NULL);
        swoole_string_release(fname);
        zval_ptr_dtor(&ret);
    }

    return buffer;

}

/*
 * dispatch
 */

static CPINLINE void swoole_seria_dispatch(seriaString *buffer, zval *zvalue)
{
again:
    switch (Z_TYPE_P(zvalue))
    {
        case IS_NULL:
        case IS_TRUE:
        case IS_FALSE:
            break;
        case IS_LONG:
        case IS_DOUBLE:
            swoole_serialize_raw(buffer, zvalue);
            break;
        case IS_STRING:
            swoole_serialize_string(buffer, zvalue);
            break;
        case IS_ARRAY:
            swoole_serialize_arr(buffer, Z_ARRVAL_P(zvalue));
            break;
        case IS_REFERENCE:
            zvalue = Z_REFVAL_P(zvalue);
            goto again;
            break;
        case IS_OBJECT:
            swoole_serialize_object(buffer, zvalue);
            break;
        default:
            php_error_docref(NULL TSRMLS_CC, E_ERROR, "swoole serialize not support this type ");

            break;
    }
}

PHP_SWOOLE_SERIALIZE_API zend_string* php_swoole_serialize(zval *zvalue)
{
    seriaString str;
    swoole_string_new(SERIA_SIZE, &str, Z_TYPE_P(zvalue));
    swoole_seria_dispatch(&str, zvalue); //serialize into a string
    zend_string *z_str = (zend_string *) str.buffer;

    z_str->val[str.offset] = '\0';
    z_str->len = str.offset - _STR_HEADER_SIZE;
    z_str->h = 0;
    GC_REFCOUNT(z_str) = 1;
    GC_TYPE_INFO(z_str) = IS_STRING;

    return z_str;
}

/*
 * buffer is seria string buffer
 * len is string len
 * return_value is unseria bucket
 * args is for the object ctor (can be NULL)
 */
PHP_SWOOLE_SERIALIZE_API void php_swoole_unserialize(void * buffer, size_t len, zval *return_value, zval *object_args)
{
    zend_uchar type = *(zend_uchar*) (buffer);
    buffer += sizeof (zend_uchar);
    switch (type)
    {
        case IS_NULL:
        case IS_TRUE:
        case IS_FALSE:
            Z_TYPE_INFO_P(return_value) = type;
            return;
        case IS_LONG:
        case IS_DOUBLE:
            swoole_unserialize_raw(buffer, return_value);
            Z_TYPE_INFO_P(return_value) = type;
            return;
        case IS_STRING:
            len -= sizeof (zend_uchar);
            zend_string *str = swoole_unserialize_string(buffer, len);
            RETURN_STR(str);
        case IS_ARRAY:
            swoole_unserialize_arr(buffer, return_value);
            break;
        case IS_OBJECT:
            swoole_unserialize_object(buffer, return_value, object_args);
            break;
        default:
            php_error_docref(NULL TSRMLS_CC, E_ERROR, "swoole serialize not support this type ");

            break;
    }
}

//static void test()
//{
//        zend_string *fname = swoole_string_init("__wakeup", sizeof ("__wakeup") - 1);
////    zend_string *fname = zend_string_init("__wakeup", sizeof ("__wakeup") - 1, 0);
////    zend_string_release(fname);
//swoole_string_release(fname);
//}

PHP_FUNCTION(swoole_serialize)
{

    zval *zvalue;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &zvalue) == FAILURE)
    {
        return;
    }
    zend_string *z_str = php_swoole_serialize(zvalue);

    RETURN_STR(z_str);
}

PHP_FUNCTION(swoole_unserialize)
{
    char *buffer = NULL;
    size_t arg_len;
    zval *args = NULL; //for object

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|a", &buffer, &arg_len, &args) == FAILURE)
    {

        return;
    }

    php_swoole_unserialize(buffer, arg_len, return_value, args);

}

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(swoole_serialize)
{


    ZVAL_STRING(&swSeriaG.sleep_fname, "__sleep");
    ZVAL_STRING(&swSeriaG.weekup_fname, "__weekup");

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(swoole_serialize)
{

    /* uncomment this line if you have INI entries
    UNREGISTER_INI_ENTRIES();
     */
    return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(swoole_serialize)
{
#if defined(COMPILE_DL_SWOOLE_SERIALIZE) && defined(ZTS)

    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(swoole_serialize)
{

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(swoole_serialize)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "swoole_serialize support", "enabled");
    php_info_print_table_row(2, "Author", "郭新华");
    php_info_print_table_row(2, "email", "woshiguo35@sina.com");
    php_info_print_table_end();

    /* Remove comments if you have entries in php.ini
    DISPLAY_INI_ENTRIES();
     */
}
/* }}} */

/* {{{ swoole_serialize_functions[]
 *
 * Every user visible function must have an entry in swoole_serialize_functions[].
 */
const zend_function_entry swoole_serialize_functions[] = {
    PHP_FE(swoole_serialize, NULL)
    PHP_FE(swoole_unserialize, NULL)
    PHP_FE_END /* Must be the last line in swoole_serialize_functions[] */
};
/* }}} */

/* {{{ swoole_serialize_module_entry
 */
zend_module_entry swoole_serialize_module_entry = {
    STANDARD_MODULE_HEADER,
    "swoole_serialize",
    swoole_serialize_functions,
    PHP_MINIT(swoole_serialize),
    PHP_MSHUTDOWN(swoole_serialize),
    PHP_RINIT(swoole_serialize), /* Replace with NULL if there's nothing to do at request start */
    PHP_RSHUTDOWN(swoole_serialize), /* Replace with NULL if there's nothing to do at request end */
    PHP_MINFO(swoole_serialize),
    PHP_SWOOLE_SERIALIZE_VERSION,
    STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_SWOOLE_SERIALIZE
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(swoole_serialize)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
