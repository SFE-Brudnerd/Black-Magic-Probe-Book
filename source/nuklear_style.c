/*
 * Common styling & layout functions for the Nuklear GUI.
 *
 * Copyright 2021-2022 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "nuklear_style.h"

void nuklear_style(struct nk_context *ctx)
{
  struct nk_color table[NK_COLOR_COUNT];

  assert(ctx != NULL);

  /* adapted from gruvbox palette */
  table[NK_COLOR_TEXT] = nk_rgb_hex("#ebdbb2");                   /* fg */
  table[NK_COLOR_TEXT_GRAY]= nk_rgb_hex("#a89984");               /* gray-b */
  table[NK_COLOR_WINDOW] = nk_rgb_hex("#32302f");                 /* bg0_s */
  table[NK_COLOR_HEADER] = nk_rgb_hex("#076678");                 /* blue-b */
  table[NK_COLOR_BORDER] = nk_rgb_hex("#928374");                 /* gray-f */
  table[NK_COLOR_BUTTON] = nk_rgb_hex("#104b5b");
  table[NK_COLOR_BUTTON_HOVER] = nk_rgb_hex("#076678");           /* blue-f in light mode */
  table[NK_COLOR_BUTTON_ACTIVE] = nk_rgb_hex("#076678");          /* blue-f in light mode */
  table[NK_COLOR_TOGGLE] = nk_rgb_hex("#1d2021");                 /* bg0_h */
  table[NK_COLOR_TOGGLE_HOVER] = nk_rgb_hex("#928374");           /* gray-f */
  table[NK_COLOR_TOGGLE_CURSOR] = nk_rgb_hex("#458588");          /* blue-b */
  table[NK_COLOR_SELECT] = nk_rgb_hex("#1d2021");                 /* bg0_h */
  table[NK_COLOR_SELECT_ACTIVE] = nk_rgb_hex("#fabd2f");          /* yellow-f */
  table[NK_COLOR_SLIDER] = nk_rgb_hex("#1d2021");                 /* bg0_h */
  table[NK_COLOR_SLIDER_CURSOR] = nk_rgb_hex("#d79921");          /* yellow-b */
  table[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb_hex("#fabd2f");    /* yellow-f */
  table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb_hex("#fabd2f");   /* yellow-f */
  table[NK_COLOR_PROPERTY] = nk_rgb_hex("#1d2021");               /* bg0_h */
  table[NK_COLOR_EDIT] = nk_rgb_hex("#1d2021");                   /* bg0_h */
  table[NK_COLOR_EDIT_CURSOR] = nk_rgb_hex("#fbf1c7");            /* fg0 (bg0 in light mode) */
  table[NK_COLOR_COMBO] = nk_rgb_hex("#1d2021");                  /* bg0_h */
  table[NK_COLOR_CHART] = nk_rgb_hex("#1d2021");                  /* bg0_h */
  table[NK_COLOR_CHART_COLOR] = nk_rgb_hex("#cc241d");            /* red-b */
  table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgb_hex("#fb4934");  /* red-f */
  table[NK_COLOR_SCROLLBAR] = nk_rgb_hex("#1d2021");              /* bg0_h */
  table[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb_hex("#928374");       /* gray-f */
  table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb_hex("#a899a4"); /* gray-b */
  table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb_hex("#a899a4");/* gray-b */
  table[NK_COLOR_TAB_HEADER] = nk_rgb_hex("#104b5b");
  table[NK_COLOR_TOOLTIP] = nk_rgb_hex("#fbf1c7");                /* bg0 in light mode, also fg0 */
  table[NK_COLOR_TOOLTIP_TEXT] = nk_rgb_hex("#3c3836");           /* fg in light mode, also bg1 */

  nk_style_from_table(ctx, table);

  /* button */
  ctx->style.button.rounding = 0;
  ctx->style.button.padding.x = 2;
}

float *nk_ratio(int count, ...)
{
# define MAX_ROW_FIELDS 10
  static float r_array[MAX_ROW_FIELDS];
  va_list ap;
  int i;

  assert(count < MAX_ROW_FIELDS);
  va_start(ap, count);
  for (i = 0; i < count; i++)
    r_array[i] = (float) va_arg(ap, double);
  va_end(ap);
  return r_array;
}

bool editctrl_cond_color(struct nk_context *ctx, bool condition, struct nk_color color)
{
  if (condition) {
    nk_style_push_color(ctx, &ctx->style.edit.normal.data.color, color);
    nk_style_push_color(ctx, &ctx->style.edit.hover.data.color, color);
    nk_style_push_color(ctx, &ctx->style.edit.active.data.color, color);
  }
  return condition;
}

void editctrl_reset_color(struct nk_context *ctx, bool condition)
{
  if (condition) {
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
  }
}

