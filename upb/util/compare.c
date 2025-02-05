/*
 * Copyright (c) 2009-2021, Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Google LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "upb/util/compare.h"

#include <stdbool.h>
#include <setjmp.h>

#include "upb/port_def.inc"

struct upb_UnknownFields;
typedef struct upb_UnknownFields upb_UnknownFields;

typedef struct {
  uint32_t tag;
  union {
    uint64_t varint;
    uint64_t uint64;
    uint32_t uint32;
    upb_strview delimited;
    upb_UnknownFields* group;
  } data;
} upb_UnknownField;

struct upb_UnknownFields {
  size_t size;
  size_t capacity;
  upb_UnknownField* fields;
};

typedef struct {
  const char *end;
  upb_arena *arena;
  upb_UnknownField *tmp;
  size_t tmp_size;
  int depth;
  jmp_buf err;
} upb_UnknownField_Context;

static void upb_UnknownFields_Grow(upb_UnknownField_Context *ctx,
                                   upb_UnknownField **base,
                                   upb_UnknownField **ptr,
                                   upb_UnknownField **end) {
  size_t old = (*ptr - *base);
  size_t new = UPB_MAX(4, old * 2);

  *base = upb_arena_realloc(ctx->arena, *base, old * sizeof(**base),
                            new * sizeof(**base));
  if (!*base) UPB_LONGJMP(ctx->err, kUpb_UnknownCompareResult_OutOfMemory);

  *ptr = *base + old;
  *end = *base + new;
}

static const char *upb_UnknownFields_ParseVarint(const char *ptr,
                                                 const char *limit,
                                                 uint64_t *val) {
  uint8_t byte;
  int bitpos = 0;
  *val = 0;

  do {
    // Unknown field data must be valid.
    UPB_ASSERT(bitpos < 70 && ptr < limit);
    byte = *ptr;
    *val |= (uint64_t)(byte & 0x7F) << bitpos;
    ptr++;
    bitpos += 7;
  } while (byte & 0x80);

  return ptr;
}

// We have to implement our own sort here, since qsort() is not an in-order
// sort. Here we use merge sort, the simplest in-order sort.
static void upb_UnknownFields_Merge(upb_UnknownField *arr, size_t start,
                                    size_t mid, size_t end,
                                    upb_UnknownField *tmp) {
  memcpy(tmp, &arr[start], (end - start) * sizeof(*tmp));

  upb_UnknownField* ptr1 = tmp;
  upb_UnknownField* end1 = &tmp[mid - start];
  upb_UnknownField* ptr2 = &tmp[mid - start];
  upb_UnknownField* end2 = &tmp[end - start];
  upb_UnknownField* out = &arr[start];

  while (ptr1 < end1 && ptr2 < end2) {
    if (ptr1->tag <= ptr2->tag) {
      *out++ = *ptr1++;
    } else {
      *out++ = *ptr2++;
    }
  }

  if (ptr1 < end1) {
    memcpy(out, ptr1, (end1 - ptr1) * sizeof(*out));
  } else if (ptr2 < end2) {
    memcpy(out, ptr1, (end2 - ptr2) * sizeof(*out));
  }
}

static void upb_UnknownFields_SortRecursive(upb_UnknownField *arr,
                                            size_t start, size_t end,
                                            upb_UnknownField *tmp) {
  if (end - start > 1) {
    size_t mid = start + ((end - start) / 2);
    upb_UnknownFields_SortRecursive(arr, start, mid, tmp);
    upb_UnknownFields_SortRecursive(arr, mid, end, tmp);
    upb_UnknownFields_Merge(arr, start, mid, end, tmp);
  }
}

static void upb_UnknownFields_Sort(upb_UnknownField_Context *ctx,
                                   upb_UnknownFields *fields) {
  if (ctx->tmp_size < fields->size) {
    ctx->tmp_size = UPB_MAX(8, ctx->tmp_size);
    while (ctx->tmp_size < fields->size) ctx->tmp_size *= 2;
    ctx->tmp = realloc(ctx->tmp, ctx->tmp_size * sizeof(*ctx->tmp));
  }
  upb_UnknownFields_SortRecursive(fields->fields, 0, fields->size, ctx->tmp);
}

static upb_UnknownFields *upb_UnknownFields_DoBuild(
    upb_UnknownField_Context *ctx, const char **buf) {
  upb_UnknownField *arr_base = NULL;
  upb_UnknownField *arr_ptr = NULL;
  upb_UnknownField *arr_end = NULL;
  const char *ptr = *buf;
  uint32_t last_tag = 0;
  bool sorted = true;
  while (ptr < ctx->end) {
    uint64_t tag;
    ptr = upb_UnknownFields_ParseVarint(ptr, ctx->end, &tag);
    UPB_ASSERT(tag <= UINT32_MAX);
    int wire_type = tag & 7;
    if (wire_type == UPB_WIRE_TYPE_END_GROUP) break;
    if (tag < last_tag) sorted = false;
    last_tag = tag;

    if (arr_ptr == arr_end) {
      upb_UnknownFields_Grow(ctx, &arr_base, &arr_ptr, &arr_end);
    }
    upb_UnknownField *field = arr_ptr;
    field->tag = tag;
    arr_ptr++;

    switch (wire_type) {
      case UPB_WIRE_TYPE_VARINT:
        ptr = upb_UnknownFields_ParseVarint(ptr, ctx->end, &field->data.varint);
        break;
      case UPB_WIRE_TYPE_64BIT:
        UPB_ASSERT(ctx->end - ptr >= 8);
        memcpy(&field->data.uint64, ptr, 8);
        ptr += 8;
        break;
      case UPB_WIRE_TYPE_32BIT:
        UPB_ASSERT(ctx->end - ptr >= 4);
        memcpy(&field->data.uint32, ptr, 4);
        ptr += 4;
        break;
      case UPB_WIRE_TYPE_DELIMITED: {
        uint64_t size;
        ptr = upb_UnknownFields_ParseVarint(ptr, ctx->end, &size);
        UPB_ASSERT(ctx->end - ptr >= size);
        field->data.delimited.data = ptr;
        field->data.delimited.size = size;
        ptr += size;
        break;
      }
      case UPB_WIRE_TYPE_START_GROUP:
        if (--ctx->depth == 0) {
          UPB_LONGJMP(ctx->err, kUpb_UnknownCompareResult_MaxDepthExceeded);
        }
        field->data.group = upb_UnknownFields_DoBuild(ctx, &ptr);
        ctx->depth++;
        break;
      default:
        UPB_UNREACHABLE();
    }
  }

  *buf = ptr;
  upb_UnknownFields *ret = upb_arena_malloc(ctx->arena, sizeof(*ret));
  if (!ret) UPB_LONGJMP(ctx->err, kUpb_UnknownCompareResult_OutOfMemory);
  ret->fields = arr_base;
  ret->size = arr_ptr - arr_base;
  ret->capacity = arr_end - arr_base;
  if (!sorted) {
    upb_UnknownFields_Sort(ctx, ret);
  }
  return ret;
}

// Builds a upb_UnknownFields data structure from the binary data in buf.
static upb_UnknownFields *upb_UnknownFields_Build(upb_UnknownField_Context *ctx,
                                                  const char *buf,
                                                  size_t size) {
  ctx->end = buf + size;
  upb_UnknownFields *fields = upb_UnknownFields_DoBuild(ctx, &buf);
  UPB_ASSERT(buf == ctx->end);
  return fields;
}

// Compares two sorted upb_UnknwonFields structures for equality.
static bool upb_UnknownFields_IsEqual(const upb_UnknownFields *uf1,
                                      const upb_UnknownFields *uf2) {
  if (uf1->size != uf2->size) return false;
  for (size_t i = 0, n = uf1->size; i < n; i++) {
    upb_UnknownField *f1 = &uf1->fields[i];
    upb_UnknownField *f2 = &uf2->fields[i];
    if (f1->tag != f2->tag) return false;
    int wire_type = f1->tag & 7;
    switch (wire_type) {
      case UPB_WIRE_TYPE_VARINT:
        if (f1->data.varint != f2->data.varint) return false;
        break;
      case UPB_WIRE_TYPE_64BIT:
        if (f1->data.uint64 != f2->data.uint64) return false;
        break;
      case UPB_WIRE_TYPE_32BIT:
        if (f1->data.uint32 != f2->data.uint32) return false;
        break;
      case UPB_WIRE_TYPE_DELIMITED:
        if (!upb_strview_eql(f1->data.delimited, f2->data.delimited)) {
          return false;
        }
        break;
      case UPB_WIRE_TYPE_START_GROUP:
        if (!upb_UnknownFields_IsEqual(f1->data.group, f2->data.group)) {
          return false;
        }
        break;
      default:
        UPB_UNREACHABLE();
    }
  }
  return true;
}

upb_UnknownCompareResult upb_Message_UnknownFieldsAreEqual(const char *buf1,
                                                           size_t size1,
                                                           const char *buf2,
                                                           size_t size2,
                                                           int max_depth) {
  if (size1 == 0 && size2 == 0) return kUpb_UnknownCompareResult_Equal;
  if (size1 == 0 || size2 == 0) return kUpb_UnknownCompareResult_NotEqual;
  if (memcmp(buf1, buf2, size1) == 0) return kUpb_UnknownCompareResult_Equal;

  upb_UnknownField_Context ctx = {
    .arena = upb_arena_new(),
    .depth = max_depth,
    .tmp = NULL,
    .tmp_size = 0,
  };

  if (!ctx.arena) return kUpb_UnknownCompareResult_OutOfMemory;

  int ret = UPB_SETJMP(ctx.err);

  if (UPB_LIKELY(ret == 0)) {
    // First build both unknown fields into a sorted data structure (similar
    // to the UnknownFieldSet in C++).
    upb_UnknownFields *uf1 = upb_UnknownFields_Build(&ctx, buf1, size1);
    upb_UnknownFields *uf2 = upb_UnknownFields_Build(&ctx, buf2, size2);

    // Now perform the equality check on the sorted structures.
    if (upb_UnknownFields_IsEqual(uf1, uf2)) {
      ret = kUpb_UnknownCompareResult_Equal;
    } else {
      ret = kUpb_UnknownCompareResult_NotEqual;
    }
  }

  upb_arena_free(ctx.arena);
  free(ctx.tmp);
  return ret;
}
