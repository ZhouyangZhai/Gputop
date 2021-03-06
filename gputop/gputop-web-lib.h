/*
 * Copyright (C) 2015 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gputop-oa-counters.h"

void _gputop_web_console_log(const char *message);
void gputop_web_console_log(const char *format, ...);

void _gputop_web_console_warn(const char *message);
void gputop_web_console_warn(const char *format, ...);

void _gputop_web_console_error(const char *message);
void gputop_web_console_error(const char *format, ...);

void _gputop_web_console_assert(bool condition, const char *message);
void gputop_web_console_assert(bool condition, const char *format, ...);

void gputop_web_index_metric_set(const char *guid, struct gputop_metric_set *metric_set);
struct gputop_metric_set *gputop_web_lookup_metric_set(const char *guid);
