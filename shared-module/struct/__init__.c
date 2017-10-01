/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Paul Sokolovsky
 * Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <assert.h>
#include <string.h>

#include "py/runtime.h"
#include "py/binary.h"
#include "py/parsenum.h"

char get_fmt_type(const char **fmt) {
    char t = **fmt;
    switch (t) {
        case '!':
            t = '>';
            break;
        case '@':
        case '=':
        case '<':
        case '>':
            break;
        default:
            return '@';
    }
    // Skip type char
    (*fmt)++;
    return t;
}

mp_uint_t get_fmt_num(const char **p) {
    const char *num = *p;
    uint len = 1;
    while (unichar_isdigit(*++num)) {
        len++;
    }
    mp_uint_t val = (mp_uint_t)MP_OBJ_SMALL_INT_VALUE(mp_parse_num_integer(*p, len, 10, NULL));
    *p = num;
    return val;
}


void shared_modules_struct_pack_into_internal(mp_obj_t fmt_in, byte *p, byte* end_p, size_t n_args, const mp_obj_t *args) {
    const char *fmt = mp_obj_str_get_str(fmt_in);
    char fmt_type = get_fmt_type(&fmt);

    size_t i;
    for (i = 0; i < n_args;) {
        mp_uint_t sz = 1;
        if (*fmt == '\0') {
            // more arguments given than used by format string; CPython raises struct.error here
            break;
        }
        if (unichar_isdigit(*fmt)) {
            sz = get_fmt_num(&fmt);
        }
        if (p + sz > end_p) {
            mp_raise_ValueError("buffer too small");
        }

        if (*fmt == 's') {
            mp_buffer_info_t bufinfo;
            mp_get_buffer_raise(args[i++], &bufinfo, MP_BUFFER_READ);
            mp_uint_t to_copy = sz;
            if (bufinfo.len < to_copy) {
                to_copy = bufinfo.len;
            }
            memcpy(p, bufinfo.buf, to_copy);
            memset(p + to_copy, 0, sz - to_copy);
            p += sz;
        } else {
            while (sz--) {
                mp_binary_set_val(fmt_type, *fmt, args[i++], &p);
            }
        }
        fmt++;
    }
}
