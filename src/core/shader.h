/*
 * Open Surge Engine
 * shader.h - managed shaders
 * Copyright (C) 2008-2023  Alexandre Martins <alemartf@gmail.com>
 * http://opensurge2d.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _SHADER_H
#define _SHADER_H

typedef struct shader_t shader_t;

void shader_init();
void shader_release();
void shader_discard_all();
void shader_recreate_all();

shader_t* shader_get(const char* name);
shader_t* shader_create(const char* name, const char* fs_glsl);
shader_t* shader_create_ex(const char* name, const char* fs_glsl, const char* vs_glsl);
bool shader_use(const shader_t* shader);

#endif